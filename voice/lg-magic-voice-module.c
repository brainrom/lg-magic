// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * lg-magic-voice-module - PipeWire source for lgmagicd.
 *
 * WirePlumber loads this as a "pw-module-client" component, so it runs in
 * the client context, on its own thread, together with the other media
 * objects.  It acquires a read-only PCM pipe from the privileged lgmagicd
 * over the system bus and publishes it as the "lg-magic-voice" source.
 *
 * It never opens hidraw and cannot arm the microphone; it only consumes
 * audio that the daemon has already captured.  The source is silent until
 * a remote button arms the microphone.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <pipewire/impl.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/support/loop.h>
#include <systemd/sd-bus.h>

#define LGMAGIC_RATE		16000
#define LGMAGIC_CHANNELS	1
#define LGMAGIC_STRIDE		2

#define LGMAGIC_DEFAULT_NAME	"lg-magic-voice"
#define LGMAGIC_DEFAULT_DESC	"LG Magic Remote microphone"

#define LGMAGIC_DBUS_NAME	"org.lgmagic"
#define LGMAGIC_DBUS_PATH	"/org/lgmagic/Voice1"
#define LGMAGIC_DBUS_IFACE	"org.lgmagic.Voice1"

#define LGMAGIC_TIMER_MS	1000
#define LGMAGIC_RETRY_MS	1000
#define LGMAGIC_RETRY_MAX_MS	5000
#define LGMAGIC_BUS_RETRY_MS	2000
/* The daemon keeps each of its two logind calls below one second, so its
 * worst case stays under this deadline.
 */
#define LGMAGIC_ACQUIRE_TIMEOUT_MS 3000
#define LGMAGIC_LOG_INTERVAL_MS	10000
#define LGMAGIC_PIPELINE_LOG_MS	10000

#define LGMAGIC_MAX_FRAMES	(LGMAGIC_RATE / 20)

PW_LOG_TOPIC_STATIC(mod_topic, "mod.lgmagic");
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct impl {
	struct pw_context *context;
	struct pw_loop *loop;
	struct pw_impl_module *module;
	struct spa_hook module_listener;

	struct pw_properties *props;

	struct pw_core *core;
	struct spa_hook core_listener;
	struct spa_hook core_proxy_listener;

	struct pw_stream *stream;
	struct spa_hook stream_listener;
	bool connected;

	int64_t core_retry_at;
	int64_t stream_retry_at;
	unsigned int core_retry_count;
	unsigned int stream_retry_count;
	bool recovering;
	int64_t last_pipeline_log;

	sd_bus *bus;
	struct spa_source *bus_io;
	struct spa_source *timer;

	bool acquire_pending;
	bool bus_error;
	int64_t acquire_started;
	int64_t retry_at;
	int64_t last_error_log;
	int64_t last_eof_log;

	int fd;
	bool primed;
	bool data_logged;
	bool quitting;
};

static int64_t lgmagic_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void lgmagic_warn(struct impl *impl, const char *fmt, ...)
{
	char buffer[256];
	int64_t now = lgmagic_now_ms();
	va_list ap;

	if (now - impl->last_error_log < LGMAGIC_LOG_INTERVAL_MS)
		return;
	impl->last_error_log = now;

	va_start(ap, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, ap);
	va_end(ap);
	pw_log_warn("%s", buffer);
}

static int64_t lgmagic_retry_delay(unsigned int count)
{
	int64_t delay = (int64_t)LGMAGIC_RETRY_MS << (count < 3 ? count : 3);

	return delay > LGMAGIC_RETRY_MAX_MS ? LGMAGIC_RETRY_MAX_MS : delay;
}

static void lgmagic_pipeline_warn(struct impl *impl, const char *fmt, ...)
{
	char buffer[256];
	int64_t now = lgmagic_now_ms();
	va_list ap;

	if (now - impl->last_pipeline_log < LGMAGIC_PIPELINE_LOG_MS)
		return;
	impl->last_pipeline_log = now;

	va_start(ap, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, ap);
	va_end(ap);
	pw_log_warn("%s", buffer);
}

static void lgmagic_close_fd(struct impl *impl)
{
	if (impl->fd >= 0) {
		close(impl->fd);
		impl->fd = -1;
	}
	impl->primed = false;
	impl->data_logged = false;
}

/* Bytes queued in the daemon-to-plugin pipe. */
static int lgmagic_queued(int fd)
{
	int avail = 0;

	if (ioctl(fd, FIONREAD, &avail) < 0)
		return -1;

	return avail;
}

static void lgmagic_discard(int fd, size_t size)
{
	uint8_t scratch[4096];

	while (size > 0) {
		size_t chunk = size < sizeof(scratch) ? size : sizeof(scratch);
		ssize_t ret = read(fd, scratch, chunk);

		if (ret <= 0)
			break;
		size -= ret;
	}
}

static void lgmagic_drain(int fd)
{
	uint8_t scratch[4096];

	while (read(fd, scratch, sizeof(scratch)) > 0)
		;
}

static void lgmagic_on_process(void *userdata)
{
	struct impl *impl = userdata;
	struct pw_buffer *b;
	struct spa_data *bd;
	uint32_t frames;
	size_t bytes, low, high;
	size_t have = 0;
	int avail;
	void *dst;

	b = pw_stream_dequeue_buffer(impl->stream);
	if (!b)
		return;

	bd = &b->buffer->datas[0];
	dst = bd->data;
	if (!dst) {
		pw_stream_queue_buffer(impl->stream, b);
		return;
	}

	frames = bd->maxsize / LGMAGIC_STRIDE;
	if (b->requested) {
		frames = b->requested;
		if (frames > bd->maxsize / LGMAGIC_STRIDE)
			frames = bd->maxsize / LGMAGIC_STRIDE;
	} else if (frames > LGMAGIC_MAX_FRAMES) {
		frames = LGMAGIC_MAX_FRAMES;
	}

	bytes = frames * LGMAGIC_STRIDE;

	/* Keep a small cushion so the daemon's mSBC bursts (7.5 ms) do not
	 * underrun every graph quantum, but never let a stale backlog build
	 * up.  The oldest data is dropped only when the cushion grows too
	 * large, not on every process callback.
	 */
	low = (size_t)LGMAGIC_RATE * LGMAGIC_STRIDE * 30 / 1000;
	high = (size_t)LGMAGIC_RATE * LGMAGIC_STRIDE * 120 / 1000;
	if (low < bytes * 2)
		low = bytes * 2;
	if (high < low * 2)
		high = low * 2;

	if (impl->fd >= 0) {
		avail = lgmagic_queued(impl->fd);
		if (avail > 0 && (size_t)avail > high) {
			size_t dropped = (size_t)avail - low;

			lgmagic_discard(impl->fd, dropped);
			avail = low;
		}

		/* After a resume the pipe is drained on PAUSED -> STREAMING,
		 * so wait for a fresh cushion before handing audio to PipeWire.
		 * This avoids both stale PCM and a zero-filled start.
		 */
		if (avail < 0)
			impl->primed = true;
		else if (!impl->primed && (size_t)avail >= low)
			impl->primed = true;

		if (impl->primed) {
			while (have < bytes) {
				ssize_t size = read(impl->fd,
						    (uint8_t *)dst + have,
						    bytes - have);

				if (size > 0) {
					have += size;
					continue;
				}
				if (size == 0) {
					int64_t now = lgmagic_now_ms();

					if (now - impl->last_eof_log >=
					    LGMAGIC_LOG_INTERVAL_MS) {
						pw_log_warn("PCM pipe closed (EOF)");
						impl->last_eof_log = now;
					}
					lgmagic_close_fd(impl);
					impl->retry_at = now + LGMAGIC_RETRY_MS;
					break;
				}
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					break;
				lgmagic_warn(impl, "read: %s", strerror(errno));
				lgmagic_close_fd(impl);
				impl->retry_at = lgmagic_now_ms() +
						 LGMAGIC_RETRY_MS;
				break;
			}
		}
	}

	if (have > 0 && !impl->data_logged) {
		pw_log_info("PCM data flowing");
		impl->data_logged = true;
	}

	if (have < bytes)
		memset((uint8_t *)dst + have, 0, bytes - have);

	bd->chunk->offset = 0;
	bd->chunk->stride = LGMAGIC_STRIDE;
	bd->chunk->size = bytes;
	b->size = frames;

	pw_stream_queue_buffer(impl->stream, b);
}

static void lgmagic_schedule_stream_restart(struct impl *impl, const char *why)
{
	if (impl->quitting || impl->recovering || impl->stream_retry_at)
		return;

	impl->stream_retry_at = lgmagic_now_ms() +
				lgmagic_retry_delay(impl->stream_retry_count++);
	pw_log_warn("stream lost (%s), recreating source", why);
}

static void lgmagic_schedule_core_restart(struct impl *impl, const char *why)
{
	if (impl->quitting || impl->recovering || impl->core_retry_at)
		return;

	impl->core_retry_at = lgmagic_now_ms() +
			      lgmagic_retry_delay(impl->core_retry_count++);
	pw_log_warn("PipeWire core lost (%s), reconnecting", why);
}

static void lgmagic_stream_destroy(void *userdata)
{
	struct impl *impl = userdata;

	spa_hook_remove(&impl->stream_listener);
	impl->stream = NULL;
	impl->connected = false;
	impl->primed = false;
	impl->data_logged = false;
}

static void lgmagic_on_state(void *userdata, enum pw_stream_state old,
			     enum pw_stream_state state, const char *error)
{
	struct impl *impl = userdata;

	(void)old;

	if (state == PW_STREAM_STATE_CONNECTING)
		impl->connected = true;

	if (state == PW_STREAM_STATE_STREAMING) {
		if (impl->fd >= 0)
			lgmagic_drain(impl->fd);
		impl->primed = false;
		impl->data_logged = false;
	}

	if (state == PW_STREAM_STATE_ERROR) {
		pw_log_error("stream error: %s", error ? error : "unknown");
		impl->connected = false;
		lgmagic_schedule_stream_restart(impl, "stream error");
	} else if (state == PW_STREAM_STATE_UNCONNECTED &&
		   impl->connected && !impl->quitting) {
		pw_log_error("stream lost");
		impl->connected = false;
		lgmagic_schedule_stream_restart(impl, "stream disconnected");
	}
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = lgmagic_stream_destroy,
	.state_changed = lgmagic_on_state,
	.process = lgmagic_on_process,
};

static void lgmagic_core_error(void *userdata, uint32_t id, int seq, int res,
			       const char *message)
{
	struct impl *impl = userdata;

	pw_log_error("core error id:%u seq:%d res:%d: %s",
		     id, seq, res, message ? message : "unknown");

	if (id == PW_ID_CORE &&
	    (res == -EPIPE || res == -ECONNRESET || res == -ENOTCONN))
		lgmagic_schedule_core_restart(impl, "core error");
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = lgmagic_core_error,
};

static void lgmagic_core_destroy(void *userdata)
{
	struct impl *impl = userdata;

	spa_hook_remove(&impl->core_listener);
	spa_hook_remove(&impl->core_proxy_listener);
	impl->core = NULL;

	if (!impl->quitting && !impl->recovering)
		lgmagic_schedule_core_restart(impl, "core removed");
}

static const struct pw_proxy_events core_proxy_events = {
	.destroy = lgmagic_core_destroy,
};

static int lgmagic_core_connect(struct impl *impl)
{
	struct pw_core *core;
	int ret;

	if (impl->core)
		return 0;

	core = pw_context_connect(impl->context, NULL, 0);
	if (!core) {
		ret = -errno;
		return ret ? ret : -EIO;
	}

	impl->core = core;
	pw_proxy_add_listener((struct pw_proxy *)core,
			      &impl->core_proxy_listener,
			      &core_proxy_events, impl);
	pw_core_add_listener(core, &impl->core_listener, &core_events, impl);

	return 0;
}

static void lgmagic_drop_core(struct impl *impl)
{
	/* Destroying the stream and the core emits state changes; they must
	 * not schedule another restart while the caller rebuilds the
	 * pipeline.
	 */
	impl->recovering = true;

	if (impl->stream) {
		pw_stream_destroy(impl->stream);
		impl->stream = NULL;
	}

	if (impl->core) {
		/* Unlink the listeners while the core is still alive.  A core
		 * that the server already removed is a zombie, and
		 * pw_core_disconnect() then frees it without emitting
		 * destroy, leaving the hooks pointing into freed memory.
		 */
		spa_hook_remove(&impl->core_listener);
		spa_hook_remove(&impl->core_proxy_listener);
		pw_core_disconnect(impl->core);
		impl->core = NULL;
	}

	impl->recovering = false;
}

static int lgmagic_create_stream(struct impl *impl)
{
	const char *str;
	struct pw_properties *props;
	uint8_t buffer[1024];
	struct spa_pod_builder b;
	struct spa_audio_info_raw info;
	const struct spa_pod *params[1];
	int res;

	if (!impl->core)
		return -ENOTCONN;

	impl->connected = false;
	impl->primed = false;
	impl->data_logged = false;

	props = pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Audio",
		PW_KEY_MEDIA_CLASS, "Audio/Source",
		PW_KEY_NODE_NAME, LGMAGIC_DEFAULT_NAME,
		PW_KEY_NODE_DESCRIPTION, LGMAGIC_DEFAULT_DESC,
		PW_KEY_NODE_VIRTUAL, "true",
		PW_KEY_NODE_WANT_DRIVER, "false",
		PW_KEY_NODE_PAUSE_ON_IDLE, "true",
		PW_KEY_NODE_ALWAYS_PROCESS, "false",
		"priority.session", "0",
		NULL);
	if (!props)
		return errno ? -errno : -ENOMEM;

	if ((str = pw_properties_get(impl->props, PW_KEY_NODE_NAME)))
		pw_properties_set(props, PW_KEY_NODE_NAME, str);
	if ((str = pw_properties_get(impl->props, PW_KEY_NODE_DESCRIPTION)))
		pw_properties_set(props, PW_KEY_NODE_DESCRIPTION, str);
	if ((str = pw_properties_get(impl->props, "priority.session")))
		pw_properties_set(props, "priority.session", str);

	impl->stream = pw_stream_new(impl->core, "LG Magic Remote", props);
	if (!impl->stream)
		return errno ? -errno : -ENOMEM;

	pw_stream_add_listener(impl->stream, &impl->stream_listener,
			       &stream_events, impl);

	info = SPA_AUDIO_INFO_RAW_INIT(
		.format = SPA_AUDIO_FORMAT_S16,
		.rate = LGMAGIC_RATE,
		.channels = LGMAGIC_CHANNELS,
		.position = { SPA_AUDIO_CHANNEL_MONO });

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

	res = pw_stream_connect(impl->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
				PW_STREAM_FLAG_MAP_BUFFERS, params, 1);
	if (res < 0) {
		pw_stream_destroy(impl->stream);
		impl->stream = NULL;
		return res;
	}

	return 0;
}

static void lgmagic_retry_pipeline(struct impl *impl)
{
	int64_t now = lgmagic_now_ms();
	int ret;

	if (impl->core_retry_at && now >= impl->core_retry_at) {
		impl->core_retry_at = 0;

		if (impl->core)
			lgmagic_drop_core(impl);

		ret = lgmagic_core_connect(impl);
		if (ret < 0) {
			impl->core_retry_at = now +
				lgmagic_retry_delay(impl->core_retry_count++);
			lgmagic_pipeline_warn(impl,
					      "cannot reconnect to PipeWire: %s",
					      strerror(-ret));
			return;
		}

		impl->core_retry_count = 0;
		pw_log_info("reconnected to PipeWire");

		if (!impl->stream)
			impl->stream_retry_at = now;
	}

	if (impl->stream_retry_at && now >= impl->stream_retry_at) {
		if (!impl->core)
			return;

		impl->stream_retry_at = 0;

		/* The destroy and connect calls can emit state changes
		 * synchronously; do not let them schedule a second restart.
		 */
		impl->recovering = true;
		if (impl->stream) {
			pw_stream_destroy(impl->stream);
			impl->stream = NULL;
		}
		ret = lgmagic_create_stream(impl);
		impl->recovering = false;

		if (ret < 0) {
			impl->stream_retry_at = now +
				lgmagic_retry_delay(impl->stream_retry_count++);
			lgmagic_pipeline_warn(impl, "cannot create source: %s",
					      strerror(-ret));
			return;
		}

		impl->stream_retry_count = 0;
		pw_log_info("source recreated");
	}
}

static int lgmagic_on_name_owner_changed(sd_bus_message *m, void *userdata,
					 sd_bus_error *ret_error)
{
	struct impl *impl = userdata;
	const char *name, *old_owner, *new_owner;
	int ret;

	(void)ret_error;

	ret = sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner);
	if (ret < 0)
		return ret;

	if (!name || strcmp(name, LGMAGIC_DBUS_NAME))
		return 0;

	if (old_owner && *old_owner && (!new_owner || !*new_owner)) {
		pw_log_info("%s disappeared", LGMAGIC_DBUS_NAME);
		lgmagic_close_fd(impl);
		impl->retry_at = lgmagic_now_ms() + LGMAGIC_RETRY_MS;
	} else if (new_owner && *new_owner) {
		impl->retry_at = lgmagic_now_ms();
	}

	return 0;
}

static int lgmagic_on_acquire(sd_bus_message *m, void *userdata,
			      sd_bus_error *ret_error)
{
	struct impl *impl = userdata;
	const sd_bus_error *error;
	int owned = -1;
	int fd = -1;
	int flags;
	int ret;

	(void)ret_error;

	impl->acquire_pending = false;

	if (sd_bus_message_is_method_error(m, NULL)) {
		error = sd_bus_message_get_error(m);
		lgmagic_warn(impl, "Acquire failed: %s",
			     error ? error->message : "unknown error");
		impl->retry_at = lgmagic_now_ms() + LGMAGIC_RETRY_MS;
		return 0;
	}

	ret = sd_bus_message_read(m, "h", &owned);
	if (ret < 0) {
		lgmagic_warn(impl, "Acquire reply: %s", strerror(-ret));
		impl->retry_at = lgmagic_now_ms() + LGMAGIC_RETRY_MS;
		return 0;
	}

	/* "h" is borrowed from the message; keep our own copy. */
	fd = fcntl(owned, F_DUPFD_CLOEXEC, 3);
	if (fd < 0) {
		lgmagic_warn(impl, "dup: %s", strerror(errno));
		impl->retry_at = lgmagic_now_ms() + LGMAGIC_RETRY_MS;
		return 0;
	}

	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		int err = errno;

		lgmagic_warn(impl, "non-blocking: %s", strerror(err));
		close(fd);
		impl->retry_at = lgmagic_now_ms() + LGMAGIC_RETRY_MS;
		return 0;
	}

	lgmagic_close_fd(impl);
	impl->fd = fd;
	impl->primed = false;
	impl->data_logged = false;
	impl->retry_at = 0;

	pw_log_info("acquired PCM transport (fd %d)", fd);

	return 0;
}

static void lgmagic_on_bus_io(void *userdata, int fd, uint32_t mask)
{
	struct impl *impl = userdata;
	unsigned int budget = 64;
	int ret;

	(void)fd;
	(void)mask;

	/* Bound the work per loop iteration; a burst of D-Bus traffic must
	 * not starve the audio source.
	 */
	while (budget-- > 0 &&
	       (ret = sd_bus_process(impl->bus, NULL)) > 0)
		;

	if (ret < 0) {
		impl->bus_error = true;
		return;
	}

	if (impl->bus_io)
		pw_loop_update_io(impl->loop, impl->bus_io,
				  SPA_IO_IN |
				  sd_bus_get_events(impl->bus) |
				  SPA_IO_HUP | SPA_IO_ERR);
}

static void lgmagic_bus_teardown(struct impl *impl)
{
	if (impl->bus_io) {
		pw_loop_destroy_source(impl->loop, impl->bus_io);
		impl->bus_io = NULL;
	}
	if (impl->bus) {
		sd_bus_flush_close_unref(impl->bus);
		impl->bus = NULL;
	}
	impl->acquire_pending = false;
	impl->bus_error = false;
	lgmagic_close_fd(impl);
}

static int lgmagic_bus_open(struct impl *impl)
{
	static const char match[] =
		"type='signal',sender='org.freedesktop.DBus',"
		"interface='org.freedesktop.DBus',"
		"member='NameOwnerChanged'";
	sd_bus *bus = NULL;
	int fd;
	int ret;

	ret = sd_bus_open_system(&bus);
	if (ret < 0) {
		lgmagic_warn(impl, "system bus: %s", strerror(-ret));
		return ret;
	}

	sd_bus_set_method_call_timeout(bus, 2 * 1000 * 1000);
	sd_bus_set_exit_on_disconnect(bus, 0);

	ret = sd_bus_add_match(bus, NULL, match,
			       lgmagic_on_name_owner_changed, impl);
	if (ret < 0) {
		lgmagic_warn(impl, "NameOwnerChanged match: %s",
			     strerror(-ret));
		sd_bus_unref(bus);
		return ret;
	}

	fd = sd_bus_get_fd(bus);
	if (fd < 0) {
		lgmagic_warn(impl, "system bus fd: %s", strerror(-fd));
		sd_bus_unref(bus);
		return fd;
	}

	impl->bus = bus;
	impl->bus_io = pw_loop_add_io(impl->loop, fd,
				      SPA_IO_IN |
				      sd_bus_get_events(bus) |
				      SPA_IO_HUP | SPA_IO_ERR,
				      false, lgmagic_on_bus_io, impl);
	if (!impl->bus_io) {
		ret = -errno;
		lgmagic_warn(impl, "system bus io: %s", strerror(errno));
		impl->bus = NULL;
		sd_bus_unref(bus);
		return ret;
	}

	pw_log_info("system bus connected");
	return 0;
}

static void lgmagic_do_acquire(struct impl *impl)
{
	int ret;

	if (!impl->bus || impl->fd >= 0 || impl->acquire_pending)
		return;

	impl->acquire_pending = true;
	impl->acquire_started = lgmagic_now_ms();

	ret = sd_bus_call_method_async(impl->bus, NULL, LGMAGIC_DBUS_NAME,
				       LGMAGIC_DBUS_PATH, LGMAGIC_DBUS_IFACE,
				       "Acquire", lgmagic_on_acquire, impl,
				       NULL);
	if (ret < 0) {
		impl->acquire_pending = false;
		impl->retry_at = lgmagic_now_ms() + LGMAGIC_RETRY_MS;
		lgmagic_warn(impl, "Acquire: %s", strerror(-ret));
		return;
	}

	sd_bus_flush(impl->bus);
	if (impl->bus_io)
		pw_loop_update_io(impl->loop, impl->bus_io,
				  SPA_IO_IN |
				  sd_bus_get_events(impl->bus) |
				  SPA_IO_HUP | SPA_IO_ERR);
}

static void lgmagic_on_timer(void *userdata, uint64_t expirations)
{
	struct impl *impl = userdata;
	int64_t now;

	(void)expirations;

	if (impl->quitting)
		return;

	now = lgmagic_now_ms();

	lgmagic_retry_pipeline(impl);

	if (impl->bus_error) {
		lgmagic_warn(impl, "system bus lost");
		lgmagic_bus_teardown(impl);
		impl->retry_at = now + LGMAGIC_BUS_RETRY_MS;
	}

	if (!impl->bus) {
		if (now < impl->retry_at)
			return;
		if (lgmagic_bus_open(impl) < 0) {
			impl->retry_at = now + LGMAGIC_BUS_RETRY_MS;
			return;
		}
		impl->retry_at = now;
	}

	if (impl->acquire_pending &&
	    now - impl->acquire_started > LGMAGIC_ACQUIRE_TIMEOUT_MS) {
		lgmagic_warn(impl, "Acquire timed out");
		lgmagic_bus_teardown(impl);
		impl->retry_at = now + LGMAGIC_BUS_RETRY_MS;
		return;
	}

	if (impl->fd < 0 && !impl->acquire_pending && now >= impl->retry_at)
		lgmagic_do_acquire(impl);
}

static void lgmagic_release(struct impl *impl)
{
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;
	int ret;

	if (!impl->bus || impl->fd < 0)
		return;

	sd_bus_set_method_call_timeout(impl->bus, 1 * 1000 * 1000);
	ret = sd_bus_call_method(impl->bus, LGMAGIC_DBUS_NAME,
				 LGMAGIC_DBUS_PATH, LGMAGIC_DBUS_IFACE,
				 "Release", &error, &reply, NULL);
	if (ret < 0)
		lgmagic_warn(impl, "Release: %s",
			     error.message ? error.message : strerror(-ret));

	sd_bus_message_unref(reply);
	sd_bus_error_free(&error);
}

static void lgmagic_impl_destroy(struct impl *impl)
{
	impl->quitting = true;

	if (impl->timer) {
		pw_loop_destroy_source(impl->loop, impl->timer);
		impl->timer = NULL;
	}

	if (impl->stream) {
		pw_stream_destroy(impl->stream);
		impl->stream = NULL;
	}

	lgmagic_release(impl);
	lgmagic_bus_teardown(impl);

	/* Remove the listeners unconditionally: the core may be destroyed
	 * later by the context, and it must not call back into freed impl.
	 */
	spa_hook_remove(&impl->core_listener);
	spa_hook_remove(&impl->core_proxy_listener);

	if (impl->core) {
		pw_core_disconnect(impl->core);
		impl->core = NULL;
	}

	if (impl->props)
		pw_properties_free(impl->props);
	free(impl);
}

static void lgmagic_module_destroy(void *userdata)
{
	struct impl *impl = userdata;

	impl->quitting = true;
	spa_hook_remove(&impl->module_listener);
	lgmagic_impl_destroy(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = lgmagic_module_destroy,
};

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "LG Magic DKMS contributors" },
	{ PW_KEY_MODULE_DESCRIPTION,
	  "LG Magic Remote microphone source for lgmagicd" },
	{ PW_KEY_MODULE_USAGE,
	  "( node.name=<name> ) ( node.description=<description> ) "
	  "( priority.session=<priority> )" },
	{ PW_KEY_MODULE_VERSION, "1" },
};

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct timespec value = {
		.tv_sec = LGMAGIC_TIMER_MS / 1000,
		.tv_nsec = (LGMAGIC_TIMER_MS % 1000) * 1000000,
	};
	struct timespec interval = {
		.tv_sec = LGMAGIC_TIMER_MS / 1000,
		.tv_nsec = (LGMAGIC_TIMER_MS % 1000) * 1000000,
	};
	struct impl *impl;
	int ret;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(*impl));
	if (!impl)
		return -errno;

	impl->context = context;
	impl->loop = pw_context_get_main_loop(context);
	impl->module = module;
	impl->fd = -1;

	impl->props = pw_properties_new_string(args ? args : "");
	if (!impl->props) {
		ret = -errno;
		goto error;
	}

	ret = lgmagic_core_connect(impl);
	if (ret < 0) {
		pw_log_error("cannot connect to PipeWire: %s", strerror(-ret));
		impl->core_retry_at = lgmagic_now_ms() +
			lgmagic_retry_delay(impl->core_retry_count++);
	} else {
		ret = lgmagic_create_stream(impl);
		if (ret < 0) {
			pw_log_error("cannot create source: %s",
				     strerror(-ret));
			impl->stream_retry_at = lgmagic_now_ms() +
				lgmagic_retry_delay(impl->stream_retry_count++);
		}
	}

	impl->timer = pw_loop_add_timer(impl->loop, lgmagic_on_timer, impl);
	if (!impl->timer) {
		ret = -errno;
		pw_log_error("cannot create retry timer: %s", strerror(errno));
		goto error;
	}
	pw_loop_update_timer(impl->loop, impl->timer, &value, &interval, false);

	if (lgmagic_bus_open(impl) == 0)
		lgmagic_do_acquire(impl);
	else
		impl->retry_at = lgmagic_now_ms() + LGMAGIC_BUS_RETRY_MS;

	pw_impl_module_add_listener(module, &impl->module_listener,
				    &module_events, impl);
	pw_impl_module_update_properties(module,
					 &SPA_DICT_INIT_ARRAY(module_props));

	pw_log_info("loaded");
	return 0;

error:
	lgmagic_impl_destroy(impl);
	return ret;
}
