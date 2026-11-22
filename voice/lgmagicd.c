// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * lgmagicd - privileged LG Magic Remote microphone daemon.
 *
 * This is the only process allowed to open the remote's hidraw node.  It
 * arms the microphone on a physical button press, keeps the stream alive and
 * decodes mSBC to S16LE mono 16 kHz PCM.  A read-only PCM pipe is handed to
 * the session through D-Bus fd passing; the unprivileged PipeWire module
 * publishes it as the "lg-magic-voice" source.  An optional FIFO remains
 * available for simple installations.
 *
 * The session never gets access to hidraw and cannot arm the microphone.
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <linux/hidraw.h>

#include <libudev.h>
#include <sbc/sbc.h>
#include <systemd/sd-bus.h>

#define LGMAGIC_DRIVER			"lgmagic"

#define LGMAGIC_REPORT_CMD		0xF9
#define LGMAGIC_REPORT_MOTION		0xFD
#define LGMAGIC_REPORT_VOICE		0xFE

#define LGMAGIC_CMD_VOICE_START		0x03
#define LGMAGIC_CMD_VOICE_STOP		0x04
#define LGMAGIC_CMD_VOICE_RESTART	0x05

#define LGMAGIC_CODE_VOICE_KEY		0x808B

#define LGMAGIC_MOTION_SIZE		20
#define LGMAGIC_VOICE_SIZE		124

/* One 60-byte block is an H2 header (2 bytes), a 57-byte mSBC frame and
 * one padding byte.  Only 0x01 of the H2 header is constant; the second
 * byte carries sequence/padding bits, so its low nibble is 0x8.
 */
#define LGMAGIC_BLOCK_SIZE		60
#define LGMAGIC_FRAME_OFFSET		2
#define LGMAGIC_FRAME_SIZE		57
#define LGMAGIC_H2_BYTE0		0x01
#define LGMAGIC_H2_BYTE1_MASK		0x0f
#define LGMAGIC_H2_BYTE1		0x08
#define LGMAGIC_MSBC_SYNC		0xAD

#define LGMAGIC_RATE			16000
#define LGMAGIC_CHANNELS		1
#define LGMAGIC_STRIDE			2

#define LGMAGIC_DEFAULT_PIPE		"/run/lgmagic/voice.pcm"
#define LGMAGIC_DEFAULT_LOCK		"/run/lgmagic/lgmagicd.lock"
#define LGMAGIC_DEFAULT_SILENCE_MS	1500
#define LGMAGIC_DEFAULT_MAX_MS		30000
#define LGMAGIC_DEFAULT_VAD		500

#define LGMAGIC_TICK_MS			200
#define LGMAGIC_KEEPALIVE_MS		4000
#define LGMAGIC_STALL_MS		1500
#define LGMAGIC_RETRY_MAX		3

/* Active-user lookups are cached for a short time: the PipeWire module
 * retries Acquire once a second, and any local process can call the method.
 * A cache keeps a failed logind or a caller loop from stalling the single
 * audio/D-Bus loop.
 */
#define LGMAGIC_ACTIVE_CACHE_MS		2000
#define LGMAGIC_ACTIVE_ERROR_MS		1000
#define LGMAGIC_ACTIVE_CACHE_SIZE	4
#define LGMAGIC_CHECK_LOG_MS		10000

/* Only report a stalled PCM consumer; an idle source has no reader at all
 * until an application starts recording.
 */
#define LGMAGIC_DROP_LOG_MS		10000

#define LGMAGIC_DBUS_NAME		"org.lgmagic"
#define LGMAGIC_DBUS_PATH		"/org/lgmagic/Voice1"
#define LGMAGIC_DBUS_IFACE		"org.lgmagic.Voice1"

/* Stable Disarmed() reasons for services that use the arm-event side
 * channel without consuming PCM.
 */
enum lgmagicd_reason {
	LGMAGIC_REASON_SILENCE,
	LGMAGIC_REASON_TIMEOUT,
	LGMAGIC_REASON_NO_AUDIO,
	LGMAGIC_REASON_DISCONNECT,
	LGMAGIC_REASON_SHUTDOWN,
};

struct lgmagicd_client {
	int fd;
	char *sender;
	bool data_logged;
	bool read_since_arm;
	bool stalled;
	bool write_error_logged;
	uint64_t dropped_bytes;
	int64_t last_drop_log;
};

struct lgmagicd_active {
	uid_t uid;
	int state;
	int64_t checked;
	bool valid;
};

struct lgmagicd {
	struct udev *udev;
	struct udev_monitor *monitor;
	sd_bus *bus;
	sd_bus_slot *dbus_slot;
	sd_bus_slot *name_slot;
	sbc_t sbc;

	const char *pipe_path;
	const char *force_device;
	const char *lock_path;
	char *devnode;

	unsigned int silence_ms;
	unsigned int max_ms;
	unsigned int vad_threshold;

	int fd;
	int pipe_fd;
	int lock_fd;
	struct lgmagicd_client client;

	struct lgmagicd_active active_cache[LGMAGIC_ACTIVE_CACHE_SIZE];
	unsigned int active_cache_next;
	int last_check_error;
	int64_t last_check_log;

	/* Product state: capture duration, keepalive, stall detection and
	 * device rescanning.
	 */
	int64_t armed_since;
	int64_t last_audio;
	int64_t last_command;
	int64_t last_scan;
	int64_t last_voice;
	unsigned int retries;

	/* Rate-limited diagnostics.  These do not drive the state machine. */
	int64_t last_block_log;
	int64_t last_decode_log;
	int64_t last_voice_log;
	int last_cmd_error;
	unsigned char last_cmd_opcode;
	int last_open_error;
	int last_read_error;
	uint16_t last_motion_code;

	bool armed;
	bool streaming;
	bool sbc_ready;
	bool verbose;
};

static volatile sig_atomic_t lgmagicd_stop;

static int64_t lgmagicd_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const char *lgmagicd_reason_name(enum lgmagicd_reason reason)
{
	switch (reason) {
	case LGMAGIC_REASON_SILENCE:
		return "silence";
	case LGMAGIC_REASON_TIMEOUT:
		return "timeout";
	case LGMAGIC_REASON_NO_AUDIO:
		return "no audio";
	case LGMAGIC_REASON_DISCONNECT:
		return "disconnect";
	case LGMAGIC_REASON_SHUTDOWN:
		return "shutdown";
	default:
		return "unknown";
	}
}

static void lgmagicd_log(struct lgmagicd *d, const char *fmt, ...)
{
	va_list ap;

	if (!d->verbose)
		return;

	fprintf(stderr, "[%lld] ", (long long)lgmagicd_now_ms());
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static void lgmagicd_err(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static uint16_t lgmagicd_be16(const unsigned char *p)
{
	return ((uint16_t)p[0] << 8) | p[1];
}

static int lgmagicd_sbc_reset(struct lgmagicd *d)
{
	int ret;

	if (d->sbc_ready) {
		sbc_finish(&d->sbc);
		d->sbc_ready = false;
	}

	memset(&d->sbc, 0, sizeof(d->sbc));
	ret = sbc_init_msbc(&d->sbc, 0);
	if (ret < 0)
		return ret;

	d->sbc.endian = SBC_LE;
	d->sbc_ready = true;

	if (sbc_get_frame_length(&d->sbc) != LGMAGIC_FRAME_SIZE)
		lgmagicd_err("lgmagicd: unexpected mSBC frame length %zu",
			     sbc_get_frame_length(&d->sbc));

	return 0;
}

static int lgmagicd_command(struct lgmagicd *d, unsigned char opcode)
{
	unsigned char buf[2] = { LGMAGIC_REPORT_CMD, opcode };
	int ret;

	if (d->fd < 0)
		return -ENODEV;

	if (ioctl(d->fd, HIDIOCSFEATURE(sizeof(buf)), buf) >= 0) {
		d->last_cmd_error = 0;
		d->last_cmd_opcode = 0;
		lgmagicd_log(d, "command %02x", opcode);
		return 0;
	}

	ret = -errno;
	if (ret != d->last_cmd_error || opcode != d->last_cmd_opcode) {
		lgmagicd_err("lgmagicd: command %02x failed: %s", opcode,
			     strerror(-ret));
		d->last_cmd_error = ret;
		d->last_cmd_opcode = opcode;
	}

	return ret;
}

static void lgmagicd_signal(int signo)
{
	(void)signo;
	lgmagicd_stop = 1;
}

static int lgmagicd_fifo_create(struct lgmagicd *d)
{
	struct stat st;

	if (mkfifo(d->pipe_path, 0660) < 0 && errno != EEXIST)
		return -errno;

	if (lstat(d->pipe_path, &st) < 0)
		return -errno;

	if (!S_ISFIFO(st.st_mode)) {
		errno = EINVAL;
		return -EINVAL;
	}

	/* The service owns the FIFO; best effort, the group must be able to
	 * read and write it.
	 */
	if (chmod(d->pipe_path, 0660) < 0 && errno != EPERM)
		return -errno;

	return 0;
}

static int lgmagicd_fifo_open(struct lgmagicd *d)
{
	int fd;

	fd = open(d->pipe_path, O_WRONLY | O_NONBLOCK | O_CLOEXEC |
		  O_NOFOLLOW);
	if (fd < 0)
		return -errno;

	d->pipe_fd = fd;

	return 0;
}

static void lgmagicd_fifo_close(struct lgmagicd *d)
{
	if (d->pipe_fd >= 0)
		close(d->pipe_fd);

	d->pipe_fd = -1;
}

static void lgmagicd_fifo_write(struct lgmagicd *d, const void *data,
				size_t size)
{
	ssize_t ret;

	if (!d->pipe_path)
		return;

	if (d->pipe_fd < 0 && lgmagicd_fifo_open(d) < 0)
		return;

	ret = write(d->pipe_fd, data, size);
	if (ret < 0) {
		if (errno == EPIPE || errno == ENXIO || errno == EBADF)
			lgmagicd_fifo_close(d);
		return;
	}

	/* Real-time audio: on a short write the rest is stale, drop it. */
}

static void lgmagicd_client_close(struct lgmagicd *d)
{
	struct lgmagicd_client *client = &d->client;

	if (client->fd >= 0)
		close(client->fd);

	client->fd = -1;
	client->data_logged = false;
	client->read_since_arm = false;
	client->stalled = false;
	client->write_error_logged = false;
	client->dropped_bytes = 0;
	client->last_drop_log = 0;
	free(client->sender);
	client->sender = NULL;
}

static void lgmagicd_client_drop(struct lgmagicd *d, size_t size)
{
	struct lgmagicd_client *client = &d->client;
	int64_t now = lgmagicd_now_ms();

	client->dropped_bytes += size;

	/* Before the first successful write the consumer is most likely an
	 * idle source with a paused PipeWire stream, not a stalled reader.
	 * Only report a consumer that was reading and then stopped.
	 */
	if (client->read_since_arm && !client->stalled) {
		lgmagicd_err("lgmagicd: PCM consumer stopped reading mid-phrase; "
			     "dropped %llu bytes",
			     (unsigned long long)client->dropped_bytes);
		client->stalled = true;
		client->last_drop_log = now;
		return;
	}

	if (!client->read_since_arm &&
	    now - client->last_drop_log >= LGMAGIC_DROP_LOG_MS) {
		lgmagicd_log(d, "PCM consumer is not reading; dropped %llu bytes",
			     (unsigned long long)client->dropped_bytes);
		client->last_drop_log = now;
	}
}

static void lgmagicd_output(struct lgmagicd *d, const void *data, size_t size)
{
	struct lgmagicd_client *client = &d->client;
	ssize_t ret;

	lgmagicd_fifo_write(d, data, size);

	if (client->fd < 0)
		return;

	do {
		ret = write(client->fd, data, size);
	} while (ret < 0 && errno == EINTR);

	if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			lgmagicd_client_drop(d, size);
			return;
		}
		if (!client->write_error_logged) {
			lgmagicd_err("lgmagicd: PCM client write failed: %s",
				     strerror(errno));
			client->write_error_logged = true;
		}
		if (errno == EPIPE || errno == EBADF)
			lgmagicd_client_close(d);
		return;
	}

	client->read_since_arm = true;
	client->stalled = false;
	client->write_error_logged = false;

	if ((size_t)ret < size)
		client->dropped_bytes += size - (size_t)ret;
	else
		client->dropped_bytes = 0;

	if (!client->data_logged) {
		lgmagicd_log(d, "PCM client got %zd bytes", ret);
		client->data_logged = true;
	}
}

static int lgmagicd_dbus_get_is_armed(sd_bus *bus, const char *path,
				      const char *interface, const char *property,
				      sd_bus_message *reply, void *userdata,
				      sd_bus_error *ret_error)
{
	struct lgmagicd *d = userdata;

	(void)bus;
	(void)path;
	(void)interface;
	(void)property;
	(void)ret_error;

	return sd_bus_message_append(reply, "b", d->armed);
}

static int lgmagicd_dbus_get_is_streaming(sd_bus *bus, const char *path,
					  const char *interface,
					  const char *property,
					  sd_bus_message *reply, void *userdata,
					  sd_bus_error *ret_error)
{
	struct lgmagicd *d = userdata;

	(void)bus;
	(void)path;
	(void)interface;
	(void)property;
	(void)ret_error;

	return sd_bus_message_append(reply, "b", d->streaming);
}

static int lgmagicd_query_active_user(uid_t uid)
{
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;
	sd_bus *bus = NULL;
	char *user_path = NULL;
	char *state = NULL;
	int ret;

	ret = sd_bus_open_system(&bus);
	if (ret < 0)
		goto out;

	/* GetUser() and State are two calls.  Keep both short so the
	 * worst case stays below the module's Acquire deadline.
	 */
	sd_bus_set_method_call_timeout(bus, 1 * 1000 * 1000);

	/* Do not use GetSessionByPID(): PipeWire and WirePlumber run in user
	 * services and are not attached to a logind session by PID.  Their
	 * processes still belong to an active user, which is what matters.
	 */
	ret = sd_bus_call_method(bus, "org.freedesktop.login1",
				 "/org/freedesktop/login1",
				 "org.freedesktop.login1.Manager",
				 "GetUser", &error, &reply, "u",
				 (uint32_t)uid);
	if (ret < 0)
		goto out;

	{
		const char *path;

		ret = sd_bus_message_read(reply, "o", &path);
		if (ret < 0)
			goto out;
		user_path = strdup(path);
		if (!user_path) {
			ret = -ENOMEM;
			goto out;
		}
	}

	reply = sd_bus_message_unref(reply);
	sd_bus_error_free(&error);
	error = SD_BUS_ERROR_NULL;

	ret = sd_bus_get_property_string(bus, "org.freedesktop.login1",
					 user_path, "org.freedesktop.login1.User",
					 "State", &error, &state);
	if (ret < 0)
		goto out;

	ret = state && !strcmp(state, "active") ? 1 : 0;

out:
	if (reply)
		sd_bus_message_unref(reply);
	sd_bus_error_free(&error);
	if (bus)
		sd_bus_unref(bus);
	free(user_path);
	free(state);

	return ret;
}

static void lgmagicd_active_cache_store(struct lgmagicd *d, uid_t uid, int state)
{
	struct lgmagicd_active *entry = NULL;
	int64_t now = lgmagicd_now_ms();
	unsigned int i;

	for (i = 0; i < LGMAGIC_ACTIVE_CACHE_SIZE; i++) {
		struct lgmagicd_active *candidate = &d->active_cache[i];
		int64_t ttl = candidate->state < 0 ?
			      LGMAGIC_ACTIVE_ERROR_MS : LGMAGIC_ACTIVE_CACHE_MS;

		if (!candidate->valid || now - candidate->checked >= ttl) {
			entry = candidate;
			break;
		}
	}
	if (!entry)
		entry = &d->active_cache[d->active_cache_next++ %
					 LGMAGIC_ACTIVE_CACHE_SIZE];

	entry->uid = uid;
	entry->state = state;
	entry->checked = now;
	entry->valid = true;
}

static int lgmagicd_dbus_sender_active(struct lgmagicd *d, sd_bus_message *m)
{
	sd_bus_creds *creds = NULL;
	int64_t now = lgmagicd_now_ms();
	uid_t uid;
	unsigned int i;
	int ret;

	/* Ask the bus for the caller's effective UID.  Do not use
	 * SD_BUS_CREDS_AUGMENT here: augmentation asks for the peer PID and
	 * may read /proc/<pid>, which is unavailable when ProtectProc hides
	 * other users' processes.  The effective UID is exactly what the
	 * logind user check needs.
	 */
	ret = sd_bus_query_sender_creds(m, SD_BUS_CREDS_EUID, &creds);
	if (ret < 0)
		return ret;

	ret = sd_bus_creds_get_euid(creds, &uid);
	if (ret < 0)
		ret = sd_bus_creds_get_uid(creds, &uid);
	if (ret < 0)
		goto out;

	/* The daemon user (and root) are allowed for bring-up/tests.  The
	 * session normally runs as a different user and is checked below.
	 */
	if (uid == 0 || uid == getuid()) {
		ret = 1;
		goto out;
	}

	for (i = 0; i < LGMAGIC_ACTIVE_CACHE_SIZE; i++) {
		struct lgmagicd_active *entry = &d->active_cache[i];
		int64_t ttl = entry->state < 0 ?
			      LGMAGIC_ACTIVE_ERROR_MS : LGMAGIC_ACTIVE_CACHE_MS;

		if (entry->valid && entry->uid == uid &&
		    now - entry->checked < ttl) {
			ret = entry->state;
			goto out;
		}
	}

	ret = lgmagicd_query_active_user(uid);
	lgmagicd_active_cache_store(d, uid, ret);

out:
	if (creds)
		sd_bus_creds_unref(creds);

	return ret;
}

static int lgmagicd_dbus_name_owner_changed(sd_bus_message *m, void *userdata,
					    sd_bus_error *ret_error)
{
	struct lgmagicd *d = userdata;
	const char *name, *old_owner, *new_owner;
	int ret;

	(void)ret_error;

	ret = sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner);
	if (ret < 0)
		return ret;

	/* A unique name disappears when old_owner was set and new_owner is
	 * empty.  Ignore name replacement and the initial appearance.
	 */
	if (!name || !old_owner || !*old_owner ||
	    (new_owner && *new_owner))
		return 1;

	if (d->client.fd >= 0 && d->client.sender &&
	    !strcmp(d->client.sender, name)) {
		lgmagicd_log(d, "PCM client disappeared (%s)", name);
		lgmagicd_client_close(d);
	}

	return 1;
}

static int lgmagicd_dbus_acquire(sd_bus_message *m, void *userdata,
				 sd_bus_error *ret_error)
{
	struct lgmagicd *d = userdata;
	sd_bus_message *reply = NULL;
	const char *sender;
	char *sender_copy = NULL;
	int pipefd[2] = { -1, -1 };
	int flags;
	int64_t now;
	int ret;

	/* Check for an existing owner before doing any logind work.  Any
	 * local process can call Acquire, and a caller that keeps retrying
	 * while the transport is busy must not force a synchronous logind
	 * round trip or stall the audio loop.
	 */
	if (d->client.fd >= 0)
		return sd_bus_error_set(ret_error, SD_BUS_ERROR_ACCESS_DENIED,
					"PCM transport already acquired");

	sender = sd_bus_message_get_sender(m);
	if (!sender)
		return sd_bus_error_set(ret_error, SD_BUS_ERROR_ACCESS_DENIED,
					"no sender");

	ret = lgmagicd_dbus_sender_active(d, m);
	if (ret < 0) {
		now = lgmagicd_now_ms();
		if (ret != d->last_check_error ||
		    now - d->last_check_log >= LGMAGIC_CHECK_LOG_MS) {
			lgmagicd_err("lgmagicd: cannot check the active session: %s",
				     strerror(-ret));
			d->last_check_error = ret;
			d->last_check_log = now;
		}
		return sd_bus_error_setf(ret_error, SD_BUS_ERROR_ACCESS_DENIED,
					 "active session check failed: %s",
					 strerror(-ret));
	}
	d->last_check_error = 0;
	if (ret == 0)
		return sd_bus_error_set(ret_error, SD_BUS_ERROR_ACCESS_DENIED,
					"only the active session may acquire "
					"the microphone");

	sender_copy = strdup(sender);
	if (!sender_copy)
		return sd_bus_error_set_errno(ret_error, ENOMEM);

	/* The daemon's write end is non-blocking; the consumer's read end is
	 * blocking by default and may switch itself to non-blocking.
	 */
	if (pipe2(pipefd, O_CLOEXEC) < 0) {
		int err = errno;

		free(sender_copy);
		return sd_bus_error_set_errno(ret_error, err);
	}

	flags = fcntl(pipefd[1], F_GETFL, 0);
	if (flags < 0 || fcntl(pipefd[1], F_SETFL, flags | O_NONBLOCK) < 0) {
		ret = -errno;
		free(sender_copy);
		close(pipefd[0]);
		close(pipefd[1]);
		return sd_bus_error_set_errno(ret_error, -ret);
	}

	ret = sd_bus_message_new_method_return(m, &reply);
	if (ret < 0)
		goto fail;

	ret = sd_bus_message_append(reply, "h", pipefd[0]);
	if (ret < 0)
		goto fail_reply;

	ret = sd_bus_send(d->bus, reply, NULL);
	if (ret < 0)
		goto fail_reply;

	d->client.fd = pipefd[1];
	d->client.sender = sender_copy;
	d->client.data_logged = false;
	d->client.read_since_arm = false;
	d->client.stalled = false;
	d->client.write_error_logged = false;
	d->client.dropped_bytes = 0;
	d->client.last_drop_log = 0;
	sender_copy = NULL;
	pipefd[1] = -1;
	close(pipefd[0]);
	pipefd[0] = -1;

	sd_bus_message_unref(reply);

	lgmagicd_log(d, "PCM client attached (D-Bus)");

	return 1;

fail_reply:
	sd_bus_message_unref(reply);
fail:
	free(sender_copy);
	if (pipefd[0] >= 0)
		close(pipefd[0]);
	if (pipefd[1] >= 0)
		close(pipefd[1]);

	return ret;
}

static int lgmagicd_dbus_release(sd_bus_message *m, void *userdata,
				 sd_bus_error *ret_error)
{
	struct lgmagicd *d = userdata;
	const char *sender = sd_bus_message_get_sender(m);

	if (d->client.fd < 0 || !d->client.sender || !sender ||
	    strcmp(sender, d->client.sender))
		return sd_bus_error_set(ret_error, SD_BUS_ERROR_ACCESS_DENIED,
					"not the PCM transport owner");

	lgmagicd_client_close(d);

	return sd_bus_reply_method_return(m, NULL);
}

static void lgmagicd_dbus_armed(struct lgmagicd *d)
{
	if (!d->bus)
		return;

	sd_bus_emit_signal(d->bus, LGMAGIC_DBUS_PATH, LGMAGIC_DBUS_IFACE,
			   "Armed", NULL);
	sd_bus_flush(d->bus);
}

static void lgmagicd_dbus_disarmed(struct lgmagicd *d,
				   enum lgmagicd_reason reason)
{
	if (!d->bus)
		return;

	sd_bus_emit_signal(d->bus, LGMAGIC_DBUS_PATH, LGMAGIC_DBUS_IFACE,
			   "Disarmed", "s", lgmagicd_reason_name(reason));
	sd_bus_flush(d->bus);
}

static void lgmagicd_dbus_props(struct lgmagicd *d)
{
	if (!d->bus)
		return;

	sd_bus_emit_properties_changed(d->bus, LGMAGIC_DBUS_PATH,
				       LGMAGIC_DBUS_IFACE, "IsArmed",
				       "IsStreaming", NULL);
	sd_bus_flush(d->bus);
}

static int lgmagicd_dbus_setup(struct lgmagicd *d)
{
	static const sd_bus_vtable vtable[] = {
		SD_BUS_VTABLE_START(0),
		SD_BUS_METHOD("Acquire", "", "h", lgmagicd_dbus_acquire,
			      SD_BUS_VTABLE_UNPRIVILEGED),
		SD_BUS_METHOD("Release", "", "", lgmagicd_dbus_release,
			      SD_BUS_VTABLE_UNPRIVILEGED),
		SD_BUS_PROPERTY("IsArmed", "b", lgmagicd_dbus_get_is_armed, 0,
				SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
		SD_BUS_PROPERTY("IsStreaming", "b",
				lgmagicd_dbus_get_is_streaming, 0,
				SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
		/* Advisory side channel for services that do not consume PCM;
		 * no Acquire()/Release() ownership is involved.
		 */
		SD_BUS_SIGNAL("Armed", "", 0),
		SD_BUS_SIGNAL("Disarmed", "s", 0),
		SD_BUS_VTABLE_END
	};
	int ret;

	ret = sd_bus_open_system(&d->bus);
	if (ret < 0) {
		lgmagicd_err("lgmagicd: cannot connect to the system bus: %s",
			     strerror(-ret));
		d->bus = NULL;
		return ret;
	}

	sd_bus_set_exit_on_disconnect(d->bus, 0);

	ret = sd_bus_add_object_vtable(d->bus, &d->dbus_slot,
				       LGMAGIC_DBUS_PATH, LGMAGIC_DBUS_IFACE,
				       vtable, d);
	if (ret < 0)
		goto fail;

	ret = sd_bus_request_name(d->bus, LGMAGIC_DBUS_NAME, 0);
	if (ret < 0)
		goto fail;

	ret = sd_bus_add_match(d->bus, &d->name_slot,
			       "type='signal',sender='org.freedesktop.DBus',"
			       "interface='org.freedesktop.DBus',"
			       "member='NameOwnerChanged'",
			       lgmagicd_dbus_name_owner_changed, d);
	if (ret < 0)
		goto fail;

	lgmagicd_log(d, "D-Bus %s ready", LGMAGIC_DBUS_NAME);

	return 0;

fail:
	lgmagicd_err("lgmagicd: cannot export %s: %s", LGMAGIC_DBUS_NAME,
		     strerror(-ret));
	if (d->name_slot) {
		sd_bus_slot_unref(d->name_slot);
		d->name_slot = NULL;
	}
	if (d->dbus_slot) {
		sd_bus_slot_unref(d->dbus_slot);
		d->dbus_slot = NULL;
	}
	sd_bus_flush_close_unref(d->bus);
	d->bus = NULL;

	return ret;
}

static bool lgmagicd_is_voice(const int16_t *pcm, size_t frames,
			      unsigned int threshold)
{
	uint64_t sum = 0;
	uint64_t limit;
	size_t i;

	if (!frames)
		return false;

	for (i = 0; i < frames; i++) {
		int32_t sample = pcm[i];

		sum += (int64_t)sample * sample;
	}

	limit = (uint64_t)threshold * threshold;

	return sum / frames >= limit;
}

static void lgmagicd_decode_block(struct lgmagicd *d,
				  const unsigned char *block)
{
	int16_t pcm[240];
	size_t written = 0;
	size_t frames;
	ssize_t ret;

	/* Full format signature: H2 byte 0, low nibble of H2 byte 1, mSBC
	 * sync.  The high nibble of byte 1 is the sequence/padding field.
	 */
	if (block[0] != LGMAGIC_H2_BYTE0 ||
	    (block[1] & LGMAGIC_H2_BYTE1_MASK) != LGMAGIC_H2_BYTE1 ||
	    block[LGMAGIC_FRAME_OFFSET] != LGMAGIC_MSBC_SYNC) {
		if (d->verbose) {
			int64_t now = lgmagicd_now_ms();

			if (now - d->last_block_log >= 1000) {
				lgmagicd_log(d, "dropped block, sync %02x %02x %02x",
					     block[0], block[1],
					     block[LGMAGIC_FRAME_OFFSET]);
				d->last_block_log = now;
			}
		}
		return;
	}

	ret = sbc_decode(&d->sbc, block + LGMAGIC_FRAME_OFFSET,
			 LGMAGIC_FRAME_SIZE, pcm, sizeof(pcm), &written);
	if (ret < 0 || !written) {
		if (d->verbose) {
			int64_t now = lgmagicd_now_ms();

			if (now - d->last_decode_log >= 1000) {
				lgmagicd_log(d, "mSBC decode failed: %zd, written %zu, "
					     "frame %02x %02x %02x %02x",
					     ret, written,
					     block[LGMAGIC_FRAME_OFFSET],
					     block[LGMAGIC_FRAME_OFFSET + 1],
					     block[LGMAGIC_FRAME_OFFSET + 2],
					     block[LGMAGIC_FRAME_OFFSET + 3]);
				d->last_decode_log = now;
			}
		}
		return;
	}

	frames = written / sizeof(int16_t);
	if (lgmagicd_is_voice(pcm, frames, d->vad_threshold))
		d->last_voice = lgmagicd_now_ms();

	lgmagicd_output(d, pcm, written);

	if (!d->streaming) {
		d->streaming = true;
		lgmagicd_dbus_props(d);
	}
}

static void lgmagicd_disarm(struct lgmagicd *d, enum lgmagicd_reason reason)
{
	if (!d->armed)
		return;

	if (d->fd >= 0)
		lgmagicd_command(d, LGMAGIC_CMD_VOICE_STOP);

	d->armed = false;
	d->streaming = false;
	d->retries = 0;

	lgmagicd_dbus_props(d);
	lgmagicd_dbus_disarmed(d, reason);

	lgmagicd_log(d, "disarmed: %s", lgmagicd_reason_name(reason));
}

static void lgmagicd_arm(struct lgmagicd *d)
{
	int64_t now;
	int ret;

	if (d->armed || d->fd < 0)
		return;

	ret = lgmagicd_sbc_reset(d);
	if (ret < 0) {
		lgmagicd_err("lgmagicd: cannot reset mSBC: %s",
			     strerror(-ret));
		return;
	}

	if (lgmagicd_command(d, LGMAGIC_CMD_VOICE_START) < 0)
		return;

	now = lgmagicd_now_ms();
	d->armed = true;
	d->armed_since = now;
	d->last_audio = now;
	d->last_voice = now;
	d->last_command = now;
	d->retries = 0;
	d->streaming = false;

	/* Track whether the consumer actually read this phrase, so an idle
	 * PipeWire source is not reported as a stalled reader.
	 */
	d->client.read_since_arm = false;
	d->client.stalled = false;
	d->client.dropped_bytes = 0;
	d->client.last_drop_log = 0;

	lgmagicd_dbus_props(d);
	lgmagicd_dbus_armed(d);

	lgmagicd_log(d, "armed");
}

static void lgmagicd_voice_report(struct lgmagicd *d,
				  const unsigned char *data, int size)
{
	int offset;

	if (size != LGMAGIC_VOICE_SIZE || !d->armed)
		return;

	{
		int64_t now = lgmagicd_now_ms();

		d->last_audio = now;
		if (d->verbose && now - d->last_voice_log >= 1000) {
			lgmagicd_log(d, "voice report size=%d code=%04x", size,
				     lgmagicd_be16(data + 2));
			d->last_voice_log = now;
		}
	}
	d->retries = 0;

	for (offset = 4; offset + LGMAGIC_BLOCK_SIZE <= size;
	     offset += LGMAGIC_BLOCK_SIZE)
		lgmagicd_decode_block(d, data + offset);
}

static void lgmagicd_motion_report(struct lgmagicd *d,
				   const unsigned char *data, int size)
{
	uint16_t code;

	if (size != LGMAGIC_MOTION_SIZE)
		return;

	code = lgmagicd_be16(data + 17);
	if (d->verbose && code != d->last_motion_code) {
		lgmagicd_log(d, "motion button %04x", code);
		d->last_motion_code = code;
	}
	if (code == LGMAGIC_CODE_VOICE_KEY && !d->armed) {
		lgmagicd_log(d, "microphone button press");
		lgmagicd_arm(d);
	}
}

static void lgmagicd_close_device(struct lgmagicd *d)
{
	bool was_active = d->armed || d->streaming;

	if (d->fd >= 0)
		close(d->fd);

	d->fd = -1;
	free(d->devnode);
	d->devnode = NULL;
	d->armed = false;
	d->streaming = false;
	d->last_read_error = 0;
	d->last_motion_code = 0;
	/* Allow the next tick to reopen immediately after an I/O error or
	 * a hotplug event instead of waiting for the periodic rescan.
	 */
	d->last_scan = 0;

	if (was_active) {
		lgmagicd_dbus_props(d);
		lgmagicd_dbus_disarmed(d, LGMAGIC_REASON_DISCONNECT);
	}
}

static int lgmagicd_open_device(struct lgmagicd *d)
{
	struct udev_list_entry *entry;
	struct udev_enumerate *e;
	int ret = -ENODEV;

	if (d->fd >= 0)
		return 0;

	e = udev_enumerate_new(d->udev);
	if (!e)
		return -ENOMEM;

	udev_enumerate_add_match_subsystem(e, "hidraw");
	udev_enumerate_scan_devices(e);

	udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(e)) {
		struct udev_device *dev;
		struct udev_device *hid;
		const char *devnode;
		const char *driver;
		int fd;

		dev = udev_device_new_from_syspath(d->udev,
						   udev_list_entry_get_name(entry));
		if (!dev)
			continue;

		devnode = udev_device_get_devnode(dev);
		hid = udev_device_get_parent_with_subsystem_devtype(dev, "hid",
								    NULL);
		driver = hid ? udev_device_get_driver(hid) : NULL;

		if (!devnode || !driver || strcmp(driver, LGMAGIC_DRIVER) ||
		    (d->force_device && strcmp(d->force_device, devnode))) {
			udev_device_unref(dev);
			continue;
		}

		fd = open(devnode, O_RDWR | O_NONBLOCK | O_CLOEXEC |
			  O_NOFOLLOW);
		if (fd < 0) {
			if (errno == EACCES || errno == EPERM) {
				if (d->last_open_error != errno) {
					lgmagicd_err("lgmagicd: cannot open %s: %s",
						     devnode, strerror(errno));
					d->last_open_error = errno;
				}
			} else {
				lgmagicd_log(d, "cannot open %s: %s", devnode,
					     strerror(errno));
			}
			udev_device_unref(dev);
			continue;
		}

		ret = lgmagicd_sbc_reset(d);
		if (ret < 0) {
			if (d->last_open_error != ret) {
				lgmagicd_err("lgmagicd: cannot reset mSBC: %s",
					     strerror(-ret));
				d->last_open_error = ret;
			}
			close(fd);
			udev_device_unref(dev);
			continue;
		}

		d->devnode = strdup(devnode);
		if (!d->devnode) {
			close(fd);
			udev_device_unref(dev);
			ret = -ENOMEM;
			break;
		}

		d->fd = fd;
		d->last_open_error = 0;
		udev_device_unref(dev);

		lgmagicd_log(d, "using %s", devnode);
		ret = 0;
		break;
	}

	udev_enumerate_unref(e);

	return ret;
}

static void lgmagicd_read_device(struct lgmagicd *d)
{
	unsigned char buf[1024];
	unsigned int budget = 64;

	for (;;) {
		ssize_t size;

		/* Do not let a noisy or stuck hidraw node starve the D-Bus
		 * loop and the arming timers.
		 */
		if (budget-- == 0)
			return;

		size = read(d->fd, buf, sizeof(buf));

		if (size < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;

			if (d->last_read_error != errno) {
				lgmagicd_err("lgmagicd: read: %s",
					     strerror(errno));
				d->last_read_error = errno;
			}
			lgmagicd_close_device(d);
			return;
		}

		d->last_read_error = 0;

		if (!size) {
			lgmagicd_close_device(d);
			return;
		}

		if (buf[0] == LGMAGIC_REPORT_VOICE)
			lgmagicd_voice_report(d, buf, size);
		else if (buf[0] == LGMAGIC_REPORT_MOTION)
			lgmagicd_motion_report(d, buf, size);
	}
}

static void lgmagicd_udev_event(struct lgmagicd *d)
{
	struct udev_device *dev;
	const char *action;
	const char *devnode;

	dev = udev_monitor_receive_device(d->monitor);
	if (!dev)
		return;

	action = udev_device_get_action(dev);
	devnode = udev_device_get_devnode(dev);

	if (action && !strcmp(action, "remove") && d->devnode && devnode &&
	    !strcmp(d->devnode, devnode))
		lgmagicd_close_device(d);
	else if (d->fd < 0)
		lgmagicd_open_device(d);

	udev_device_unref(dev);
}

static void lgmagicd_tick(struct lgmagicd *d)
{
	int64_t now = lgmagicd_now_ms();

	if (d->client.fd >= 0) {
		struct pollfd pfd = { .fd = d->client.fd, .events = 0 };

		if (poll(&pfd, 1, 0) > 0 &&
		    (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
			lgmagicd_log(d, "PCM client closed");
			lgmagicd_client_close(d);
		}
	}

	if (d->fd < 0) {
		/* The remote drops the link when idle, so a missed udev event
		 * must not leave us without a device until the next key press.
		 */
		if (now - d->last_scan >= 1000) {
			d->last_scan = now;
			lgmagicd_open_device(d);
		}
		return;
	}

	if (!d->armed)
		return;

	/* The remote stops the stream by itself after a few seconds. */
	if (now - d->last_command >= LGMAGIC_KEEPALIVE_MS) {
		lgmagicd_command(d, LGMAGIC_CMD_VOICE_RESTART);
		d->last_command = now;
	}

	/* No audio at all: try to restart the stream a few times. */
	if (now - d->last_audio > LGMAGIC_STALL_MS) {
		if (d->retries++ >= LGMAGIC_RETRY_MAX) {
			lgmagicd_disarm(d, LGMAGIC_REASON_NO_AUDIO);
			return;
		}
		lgmagicd_command(d, LGMAGIC_CMD_VOICE_START);
		d->last_audio = now;
		d->last_command = now;
		return;
	}

	if (now - d->last_voice >= d->silence_ms) {
		lgmagicd_disarm(d, LGMAGIC_REASON_SILENCE);
		return;
	}

	if (now - d->armed_since >= d->max_ms)
		lgmagicd_disarm(d, LGMAGIC_REASON_TIMEOUT);
}

static int lgmagicd_lock(struct lgmagicd *d)
{
	char *path;
	char *slash;
	int fd;
	int ret;

	path = strdup(d->lock_path);
	if (!path)
		return -ENOMEM;

	slash = strrchr(path, '/');
	if (slash && slash != path) {
		*slash = '\0';
		if (mkdir(path, 0750) < 0 && errno != EEXIST) {
			ret = -errno;
			free(path);
			return ret;
		}
	}
	free(path);

	fd = open(d->lock_path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW,
		  0600);
	if (fd < 0)
		return -errno;

	if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
		ret = -errno;
		close(fd);
		return ret;
	}

	d->lock_fd = fd;

	return 0;
}

static int lgmagicd_parse_uint(const char *arg, unsigned int *value)
{
	unsigned long val;
	char *end;

	if (!isdigit((unsigned char)*arg))
		return -EINVAL;

	errno = 0;
	val = strtoul(arg, &end, 0);
	if (errno || *end || val > UINT_MAX)
		return -EINVAL;

	*value = val;

	return 0;
}

static void lgmagicd_usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [-d /dev/hidrawN] [-p pipe | --no-fifo]\n"
		"          [--lock file] [-s ms] [-M ms] [-V threshold]\n"
		"          [-v]\n"
		"\n"
		"  -d, --device     only use this hidraw node\n"
		"  -p, --pipe       PCM FIFO (default %s)\n"
		"      --no-fifo    do not create or write the PCM FIFO\n"
		"      --lock       lock file (default %s)\n"
		"  -s, --silence    stop after this much silence, ms\n"
		"  -M, --max        maximum capture duration, ms\n"
		"  -V, --vad        voice activity threshold (RMS)\n"
		"  -v, --verbose    log state transitions\n",
		argv0, LGMAGIC_DEFAULT_PIPE, LGMAGIC_DEFAULT_LOCK);
}

int main(int argc, char *argv[])
{
	static const struct option options[] = {
		{ "device", required_argument, NULL, 'd' },
		{ "pipe", required_argument, NULL, 'p' },
		{ "no-fifo", no_argument, NULL, 'F' },
		{ "lock", required_argument, NULL, 'L' },
		{ "silence", required_argument, NULL, 's' },
		{ "max", required_argument, NULL, 'M' },
		{ "vad", required_argument, NULL, 'V' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	struct lgmagicd d;
	unsigned int value;
	int opt;
	int ret = 0;

	memset(&d, 0, sizeof(d));
	d.pipe_path = LGMAGIC_DEFAULT_PIPE;
	d.lock_path = LGMAGIC_DEFAULT_LOCK;
	d.silence_ms = LGMAGIC_DEFAULT_SILENCE_MS;
	d.max_ms = LGMAGIC_DEFAULT_MAX_MS;
	d.vad_threshold = LGMAGIC_DEFAULT_VAD;
	d.fd = -1;
	d.pipe_fd = -1;
	d.lock_fd = -1;
	d.client.fd = -1;
	d.client.sender = NULL;
	d.client.data_logged = false;

	while ((opt = getopt_long(argc, argv, "d:p:FL:s:M:V:vh", options,
				  NULL)) != -1) {
		switch (opt) {
		case 'd':
			d.force_device = optarg;
			break;
		case 'p':
			d.pipe_path = optarg;
			break;
		case 'F':
			d.pipe_path = NULL;
			break;
		case 'L':
			d.lock_path = optarg;
			break;
		case 's':
			if (lgmagicd_parse_uint(optarg, &value))
				goto bad;
			d.silence_ms = value;
			break;
		case 'M':
			if (lgmagicd_parse_uint(optarg, &value))
				goto bad;
			d.max_ms = value;
			break;
		case 'V':
			if (lgmagicd_parse_uint(optarg, &value))
				goto bad;
			d.vad_threshold = value;
			break;
		case 'v':
			d.verbose = true;
			break;
		default:
			lgmagicd_usage(argv[0]);
			return opt == 'h' ? 0 : 1;
		}
	}

	if (d.max_ms < d.silence_ms) {
		lgmagicd_err("lgmagicd: maximum duration is below the silence "
			     "timeout");
		return 1;
	}

	ret = lgmagicd_lock(&d);
	if (ret < 0) {
		if (ret == -EWOULDBLOCK || ret == -EAGAIN)
			lgmagicd_err("lgmagicd: another instance is already "
				     "running");
		else
			lgmagicd_err("lgmagicd: cannot lock %s: %s",
				     d.lock_path, strerror(-ret));
		return 1;
	}

	ret = lgmagicd_sbc_reset(&d);
	if (ret < 0) {
		lgmagicd_err("lgmagicd: cannot set up mSBC: %s",
			     strerror(-ret));
		goto out_lock;
	}

	if (d.pipe_path) {
		ret = lgmagicd_fifo_create(&d);
		if (ret < 0) {
			lgmagicd_err("lgmagicd: cannot create %s: %s",
				     d.pipe_path, strerror(-ret));
			goto out_sbc;
		}
	}

	d.udev = udev_new();
	if (!d.udev) {
		lgmagicd_err("lgmagicd: cannot set up udev");
		ret = -ENOMEM;
		goto out_sbc;
	}

	d.monitor = udev_monitor_new_from_netlink(d.udev, "udev");
	if (!d.monitor) {
		lgmagicd_err("lgmagicd: cannot create a udev monitor");
		ret = -ENOMEM;
		goto out_udev;
	}

	if (udev_monitor_filter_add_match_subsystem_devtype(d.monitor, "hidraw",
							    NULL) < 0 ||
	    udev_monitor_enable_receiving(d.monitor) < 0) {
		lgmagicd_err("lgmagicd: cannot monitor hidraw devices");
		ret = -EIO;
		goto out_monitor;
	}

	ret = lgmagicd_dbus_setup(&d);
	if (ret < 0)
		goto out_monitor;

	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, lgmagicd_signal);
	signal(SIGTERM, lgmagicd_signal);

	if (lgmagicd_open_device(&d) < 0)
		lgmagicd_log(&d, "waiting for an LG Magic Remote");

	while (!lgmagicd_stop) {
		struct pollfd pfd[3] = {
			{ .fd = d.fd, .events = POLLIN },
			{ .fd = udev_monitor_get_fd(d.monitor),
			  .events = POLLIN },
			{ .fd = d.bus ? sd_bus_get_fd(d.bus) : -1,
			  .events = POLLIN |
				    (d.bus ? (sd_bus_get_events(d.bus) & POLLOUT) : 0) },
		};
		int n;

		n = poll(pfd, 3, LGMAGIC_TICK_MS);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			lgmagicd_err("lgmagicd: poll: %s", strerror(errno));
			ret = -errno;
			break;
		}

		if (pfd[0].revents & (POLLIN | POLLHUP | POLLERR))
			lgmagicd_read_device(&d);
		if (pfd[1].revents & (POLLERR | POLLHUP)) {
			lgmagicd_err("lgmagicd: udev monitor failed");
			ret = -EIO;
			break;
		}
		if (pfd[1].revents & POLLIN)
			lgmagicd_udev_event(&d);
		if (d.bus &&
		    (pfd[2].revents & (POLLIN | POLLOUT | POLLHUP | POLLERR))) {
			unsigned int bus_budget = 64;
			int bus_ret;

			lgmagicd_log(&d, "D-Bus revents %x", pfd[2].revents);

			/* Bound the work per poll iteration; a burst of D-Bus
			 * traffic must not starve hidraw and the arming timers.
			 */
			while (bus_budget-- > 0 &&
			       (bus_ret = sd_bus_process(d.bus, NULL)) > 0)
				;
			if (bus_ret < 0) {
				lgmagicd_err("lgmagicd: D-Bus: %s",
					     strerror(-bus_ret));
				ret = bus_ret;
				break;
			}
			sd_bus_flush(d.bus);
		}

		lgmagicd_tick(&d);
	}

	if (d.armed)
		lgmagicd_disarm(&d, LGMAGIC_REASON_SHUTDOWN);

out_monitor:
	if (d.name_slot)
		sd_bus_slot_unref(d.name_slot);
	if (d.dbus_slot)
		sd_bus_slot_unref(d.dbus_slot);
	if (d.bus)
		sd_bus_flush_close_unref(d.bus);
	udev_monitor_unref(d.monitor);
out_udev:
	udev_unref(d.udev);
out_sbc:
	if (d.sbc_ready)
		sbc_finish(&d.sbc);
	lgmagicd_fifo_close(&d);
	lgmagicd_close_device(&d);
	lgmagicd_client_close(&d);
out_lock:
	if (d.lock_fd >= 0)
		close(d.lock_fd);

	return ret < 0 ? 1 : 0;

bad:
	lgmagicd_usage(argv[0]);
	return 1;
}
