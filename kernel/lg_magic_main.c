/*  This is part of lg_magic_dkms

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/
#include <linux/module.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/firmware.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kstrtox.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#include "lg_magic_airmouse.h"

static int debug = 1;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Debug message level (0=quiet, 1=normal, 2=verbose)");

#define lgmagic_dev_dbg(dev, fmt, ...) \
do { if (debug >= 2) dev_dbg(dev, fmt, ##__VA_ARGS__); } while (0)

#define lgmagic_dev_info(dev, fmt, ...) \
do { if (debug >= 1) dev_info(dev, fmt, ##__VA_ARGS__); } while (0)

#define lgmagic_dev_warn(dev, fmt, ...) \
do { if (debug >= 1) dev_warn(dev, fmt, ##__VA_ARGS__); } while (0)

#define lgmagic_dev_err(dev, fmt, ...) \
do { dev_err(dev, fmt, ##__VA_ARGS__); } while (0)

static int airmouse = 1;
module_param(airmouse, int, 0644);
MODULE_PARM_DESC(airmouse, "Enable air mouse at probe (per-device sysfs override)");

static int airmouse_threshold = 300;
module_param(airmouse_threshold, int, 0644);
MODULE_PARM_DESC(airmouse_threshold, "Shake threshold at probe, deg/s (per-device sysfs override)");

static int sensitivity_x = 1200;
module_param(sensitivity_x, int, 0644);
MODULE_PARM_DESC(sensitivity_x, "Cursor sensitivity X at probe, px/rad (per-device sysfs override)");

static int sensitivity_y = 1200;
module_param(sensitivity_y, int, 0644);
MODULE_PARM_DESC(sensitivity_y, "Cursor sensitivity Y at probe, px/rad (per-device sysfs override)");

static int imu_evdev = 0;
module_param(imu_evdev, int, 0644);
MODULE_PARM_DESC(imu_evdev, "Expose raw IMU");

#define LGMAGIC_THRESHOLD_MAX	100000	/* deg/s */
#define LGMAGIC_SENSITIVITY_MAX	100000	/* px/rad */

static int lgmagic_clamp_param(int value, int max)
{
	if (value < 0)
		return 0;
	if (value > max)
		return max;
	return value;
}

#define LGMAGIC_SCD_MAX 512
#define LGMAGIC_FW_VER_MAX 32
#define LGMAGIC_SCD_RETRIES 3
#define LGMAGIC_SCD_RETRY_MS 1000
#define LGMAGIC_SCD_CACHE_MAX 4

struct lgmagic_drvdata {
	struct input_dev *input_hid;
	struct input_dev *input_imu;
	struct hid_device *hdev;
	struct work_struct startup;
	struct delayed_work scd_retry;
	unsigned int scd_retries;
	bool started;
	bool shutdown;
	bool blob_valid;
	bool pointer_ready;
	bool airmouse_enabled;
	bool motion_on;

	spinlock_t lock;
	u8 fw_ver[LGMAGIC_FW_VER_MAX];
	size_t fw_len;
	u8 scd[LGMAGIC_SCD_MAX];
	size_t scd_len;
	bool scd_done;
	bool scd_expected;
	u8 fw_major;
	u8 fw_zone;
	u8 battery;
	u8 last_motion_state;

	u16 last_keycode;
	u16 last_btncode;
	int mode;
	struct lg_magic_airmouse_state airmouse;
};

#define LGMAGIC_REPORT_CMD 0xF9
#define LGMAGIC_REPORT_VOICE 0xFE
#define LGMAGIC_REPORT_MOTION 0xFD
#define LGMAGIC_MOTION_STATE_MASK 0x03
#define LGMAGIC_MOTION_STATE_VALID 0x01

#define LGMAGIC_CMD_MOTION_ON 0x01
#define LGMAGIC_CMD_MOTION_SLEEP 0x02
#define LGMAGIC_CMD_FW_VER 0x11
#define LGMAGIC_CMD_SCD 0x17

#define LGMAGIC_RSP_FW_VER 0x12
#define LGMAGIC_RSP_FW_VER_CK 0x14
#define LGMAGIC_RSP_SCD 0x17

#define LGMAGIC_CODE_WHEEL 0x8044
#define LGMAGIC_CODE_VOICE_START 0x800A
#define LGMAGIC_CODE_VOICE_STOP 0x800D
#define LGMAGIC_CODE_MOTION_START 0x803E
#define LGMAGIC_CODE_MOTION_STOP 0x803F

/*
 * Streaming shortcuts are mapped by physical button position, not by
 * service: different remotes put IVI or Netflix on the same button, so
 * both codes report the same KEY_PROG* code.
 */
static const struct {
	u16 code;
	u16 keycode;
} lg_btn_map[] = {
	{ 0x8099, KEY_SLEEP },
	{ 0x8010, KEY_0 }, { 0x8011, KEY_1 }, { 0x8012, KEY_2 },
	{ 0x8013, KEY_3 }, { 0x8014, KEY_4 }, { 0x8015, KEY_5 },
	{ 0x8016, KEY_6 }, { 0x8017, KEY_7 }, { 0x8018, KEY_8 }, { 0x8019, KEY_9 },
	{ LGMAGIC_CODE_WHEEL, KEY_ENTER },
	{ LGMAGIC_CODE_WHEEL, BTN_LEFT },
	{ 0x8000, KEY_CHANNELUP }, // POWER is IR-only and cannot be told apart over Bluetooth
	{ 0x8001, KEY_CHANNELDOWN },
	{ 0x8002, KEY_VOLUMEUP },
	{ 0x8003, KEY_VOLUMEDOWN },
	{ 0x8006, KEY_RIGHT },
	{ 0x8007, KEY_LEFT },
	{ 0x8009, KEY_MUTE },
	{ 0x800B, KEY_SWITCHVIDEOMODE }, // Input/source
	{ 0x80A1, KEY_SWITCHVIDEOMODE }, // Input/source
	{ 0x8024, KEY_PROG4 }, // YouTube
	{ 0x8028, KEY_BACK },
	{ 0x8029, KEY_FAVORITES }, // Heart button
	{ 0x8031, KEY_PROG3 }, // Disney
	{ 0x8040, KEY_UP },
	{ 0x8041, KEY_DOWN },
	{ 0x8043, KEY_SETUP },
	{ 0x8045, KEY_MENU }, // ... button
	{ 0x8048, KEY_CHANNEL }, // LG Channels
	{ 0x804C, KEY_LIST }, // List button on MR19, 0x8053 on MR20
	{ 0x8053, KEY_LIST }, // List button on MR20
	{ 0x8056, KEY_PROG1 }, // Netflix; IVI shares this button
	{ 0x805C, KEY_PROG2 }, // Amazon Prime
	{ 0x805D, KEY_PROG1 }, // IVI; Netflix shares this button
	{ 0x8061, KEY_BLUE },
	{ 0x8063, KEY_YELLOW },
	{ 0x8071, KEY_GREEN },
	{ 0x8072, KEY_RED },
	{ 0x807B, KEY_NEWS }, // News
	{ 0x807C, KEY_HOME },
	{ 0x8081, KEY_VIDEO }, // MOVIES
	{ 0x8098, KEY_CONTEXT_MENU }, // STB MENU
	{ 0x80AB, KEY_PROGRAM },
	{ 0x808B, KEY_VOICECOMMAND },
	{ 0x80B0, KEY_PLAY },
	{ 0x80BA, KEY_PAUSE },
};

static bool lgmagic_code_is_control(u16 code)
{
	return code == LGMAGIC_CODE_VOICE_START ||
	       code == LGMAGIC_CODE_VOICE_STOP ||
	       code == LGMAGIC_CODE_MOTION_START ||
	       code == LGMAGIC_CODE_MOTION_STOP;
}

static int lgmagic_cmd_send(struct hid_device *hdev, u8 opcode)
{
	u8 buf[2] = { LGMAGIC_REPORT_CMD, opcode };
	int ret;

	ret = hid_hw_raw_request(hdev, buf[0], buf, sizeof(buf),
				 HID_FEATURE_REPORT, HID_REQ_SET_REPORT);
	if (ret == sizeof(buf))
		return 0;

	lgmagic_dev_warn(&hdev->dev, "Command %02x rejected: %d", opcode, ret);

	return ret < 0 ? ret : -EIO;
}

/* A new request discards any partial SCD transfer. */
static void lgmagic_request_scd(struct lgmagic_drvdata *drvdata)
{
	unsigned long flags;

	spin_lock_irqsave(&drvdata->lock, flags);
	drvdata->scd_len = 0;
	drvdata->scd_done = false;
	drvdata->scd_expected = true;
	spin_unlock_irqrestore(&drvdata->lock, flags);

	lgmagic_cmd_send(drvdata->hdev, LGMAGIC_CMD_SCD);
}

static void lgmagic_set_motion(struct lgmagic_drvdata *drvdata, bool on)
{
	int ret = lgmagic_cmd_send(drvdata->hdev,
				   on ? LGMAGIC_CMD_MOTION_ON :
					LGMAGIC_CMD_MOTION_SLEEP);

	if (ret == 0)
		drvdata->motion_on = on;
}

static void lgmagic_set_airmouse(struct lgmagic_drvdata *drvdata, bool on)
{
	drvdata->airmouse_enabled = on;

	if (!on)
	{
		drvdata->mode = 0;
		drvdata->airmouse.gravity_valid = 0;
	}

	lgmagic_set_motion(drvdata, on);
}

struct lgmagic_scd_cache_entry {
	char uniq[18];
	u8 raw[LGMAGIC_SCD_SIZE];
	bool valid;
};

static struct lgmagic_scd_cache_entry lgmagic_scd_cache[LGMAGIC_SCD_CACHE_MAX];
static DEFINE_SPINLOCK(lgmagic_scd_cache_lock);
static unsigned int lgmagic_scd_cache_next;

static bool lgmagic_scd_cache_lookup(const char *uniq,
				     u8 raw[LGMAGIC_SCD_SIZE])
{
	unsigned long flags;
	bool found = false;
	int i;

	if (!uniq || !uniq[0])
		return false;

	spin_lock_irqsave(&lgmagic_scd_cache_lock, flags);
	for (i = 0; i < LGMAGIC_SCD_CACHE_MAX; i++)
	{
		if (lgmagic_scd_cache[i].valid &&
		    !strcmp(lgmagic_scd_cache[i].uniq, uniq))
		{
			memcpy(raw, lgmagic_scd_cache[i].raw,
			       sizeof(lgmagic_scd_cache[i].raw));
			found = true;
			break;
		}
	}
	spin_unlock_irqrestore(&lgmagic_scd_cache_lock, flags);

	return found;
}

static void lgmagic_scd_cache_store(const char *uniq, const u8 *raw)
{
	unsigned long flags;
	int i, slot = -1;

	if (!uniq || !uniq[0])
		return;

	spin_lock_irqsave(&lgmagic_scd_cache_lock, flags);
	for (i = 0; i < LGMAGIC_SCD_CACHE_MAX; i++)
	{
		if (lgmagic_scd_cache[i].valid &&
		    !strcmp(lgmagic_scd_cache[i].uniq, uniq))
		{
			slot = i;
			break;
		}
		if (!lgmagic_scd_cache[i].valid && slot < 0)
			slot = i;
	}
	if (slot < 0)
		slot = lgmagic_scd_cache_next++ % LGMAGIC_SCD_CACHE_MAX;

	strscpy(lgmagic_scd_cache[slot].uniq, uniq,
		sizeof(lgmagic_scd_cache[slot].uniq));
	memcpy(lgmagic_scd_cache[slot].raw, raw,
	       sizeof(lgmagic_scd_cache[slot].raw));
	lgmagic_scd_cache[slot].valid = true;
	spin_unlock_irqrestore(&lgmagic_scd_cache_lock, flags);
}

/* A blob wins over the SCD gyro matrix; the accel matrix is still useful. */
static void lgmagic_apply_scd(struct lgmagic_drvdata *drvdata,
			      const s64 gyro_matrix[3][3],
			      const s64 accel_matrix[3][3],
			      const s64 gyro_remap[3][3],
			      const s64 gyro_scale[3],
			      s64 accel_scale, s64 sample_dt)
{
	int i;

	/* The sample period describes the stream, not the sensor frame. */
	if (sample_dt > 0)
		drvdata->airmouse.dt = sample_dt;

	drvdata->airmouse.accel_scale = accel_scale;

	/* A blob is raw-frame; do not mix it with the SCD remap. */
	if (drvdata->blob_valid)
		return;

	memcpy(drvdata->airmouse.gyro_matrix, gyro_matrix,
	       sizeof(drvdata->airmouse.gyro_matrix));
	memcpy(drvdata->airmouse.accel_matrix, accel_matrix,
	       sizeof(drvdata->airmouse.accel_matrix));
	for (i = 0; i < 3; i++)
		drvdata->airmouse.gyro_scale[i] = gyro_scale[i];
	lgmagic_airmouse_set_remap(&drvdata->airmouse, gyro_remap);
	drvdata->airmouse.gyro_matrix_valid = 1;
	drvdata->airmouse.accel_matrix_valid = 1;
	drvdata->airmouse.auto_bias = 1;
	drvdata->pointer_ready = true;
}

static void lgmagic_fw_version(struct hid_device *hdev, u8 *data, int size)
{
	struct lgmagic_drvdata *drvdata = hid_get_drvdata(hdev);
	unsigned long flags;
	size_t len;

	if (size < 4)
		return;

	len = min_t(size_t, size - 4, LGMAGIC_FW_VER_MAX);

	spin_lock_irqsave(&drvdata->lock, flags);
	drvdata->fw_major = data[2];
	drvdata->fw_zone = data[3];
	memcpy(drvdata->fw_ver, data + 4, len);
	drvdata->fw_len = len;
	spin_unlock_irqrestore(&drvdata->lock, flags);

	lgmagic_dev_info(&hdev->dev, "Remote firmware %u zone %u: %*ph", data[2],
			 data[3], (int)len, data + 4);
}

/* Bytes past data[3] are the previous frame; only expected chunks count. */
static void lgmagic_scd_chunk(struct hid_device *hdev, u8 *data, int size)
{
	struct lgmagic_drvdata *drvdata = hid_get_drvdata(hdev);
	unsigned long flags;
	s64 gyro_matrix[3][3], accel_matrix[3][3], gyro_remap[3][3];
	s64 gyro_scale[3];
	s64 accel_scale = 0;
	s64 sample_dt = LGMAGIC_DEFAULT_DT;
	u8 raw[LGMAGIC_SCD_SIZE];
	size_t len, total;
	bool complete = false;
	bool parsed = false;

	if (size < 4)
		return;

	len = min_t(size_t, data[3], size - 4);

	spin_lock_irqsave(&drvdata->lock, flags);
	if (!drvdata->scd_expected)
	{
		spin_unlock_irqrestore(&drvdata->lock, flags);
		lgmagic_dev_dbg(&hdev->dev, "Unexpected SCD chunk, %zu bytes",
				len);
		return;
	}

	len = min_t(size_t, len, LGMAGIC_SCD_MAX - drvdata->scd_len);
	memcpy(drvdata->scd + drvdata->scd_len, data + 4, len);
	drvdata->scd_len += len;
	total = drvdata->scd_len;

	if (!data[2])
	{
		drvdata->scd_expected = false;
		drvdata->scd_done = true;
		complete = true;
		parsed = lgmagic_parse_scd_matrices(drvdata->scd, total,
						    gyro_matrix,
						    accel_matrix,
						    gyro_remap,
						    gyro_scale,
						    &accel_scale,
						    &sample_dt) == 0;
		if (parsed)
			memcpy(raw, drvdata->scd, sizeof(raw));
	}
	spin_unlock_irqrestore(&drvdata->lock, flags);

	lgmagic_dev_dbg(&hdev->dev, "SCD %zu bytes, more %u", len, data[2]);

	if (!complete)
		return;

	lgmagic_dev_info(&hdev->dev, "SCD complete: %zu bytes", total);
	cancel_delayed_work(&drvdata->scd_retry);

	if (parsed)
	{
		lgmagic_scd_cache_store(hdev->uniq, raw);
		lgmagic_apply_scd(drvdata, gyro_matrix, accel_matrix,
				  gyro_remap, gyro_scale, accel_scale,
				  sample_dt);
		lgmagic_dev_info(&hdev->dev,
				 "SCD accepted, gyro scale %lld/%lld/%lld, dt %lld (Q16.16)",
				 gyro_scale[0], gyro_scale[1], gyro_scale[2],
				 sample_dt);
		return;
	}

	if (drvdata->blob_valid || drvdata->pointer_ready)
	{
		lgmagic_dev_warn(&hdev->dev,
				 "SCD is not usable; keeping the current calibration");
		return;
	}

	lgmagic_dev_warn(&hdev->dev,
			 "SCD is not usable; air mouse stays disabled");
	drvdata->scd_done = false;
	schedule_delayed_work(&drvdata->scd_retry,
			      msecs_to_jiffies(LGMAGIC_SCD_RETRY_MS));
}

static void lgmagic_cmd_response(struct hid_device *hdev, u8 *data, int size)
{
	if (size < 2)
		return;

	switch (data[1]) {
	case LGMAGIC_RSP_FW_VER:
	case LGMAGIC_RSP_FW_VER_CK:
		lgmagic_fw_version(hdev, data, size);
		break;
	case LGMAGIC_RSP_SCD:
		lgmagic_scd_chunk(hdev, data, size);
		break;
	default:
		lgmagic_dev_dbg(&hdev->dev, "Command response %02x, %d bytes",
				data[1], size);
		break;
	}
}

static int lgmagic_raw_event(struct hid_device *hdev, struct hid_report *report,
				u8 *data, int size)
{
	struct lgmagic_drvdata *drvdata = hid_get_drvdata(hdev);
	unsigned long flags;
	u8 reporting = 0;
	u16 counter;
	s16 imu[6];
	s16 mouse[2] = {0};

	int i;

	if (!drvdata || !drvdata->input_hid || !drvdata->input_imu)
	{
		lgmagic_dev_warn(&hdev->dev, "No drvdata or no input dev");
		return 0;
	}

	if (size < 1)
		return 0;

	if (!drvdata->started) {
		drvdata->started = true;
		schedule_work(&drvdata->startup);
	}

	if (data[0] == LGMAGIC_REPORT_CMD)
	{
		lgmagic_cmd_response(hdev, data, size);
		return 0;
	}

	if (data[0] == LGMAGIC_REPORT_VOICE)
		return 0;

	if (size != 20 || data[0] != LGMAGIC_REPORT_MOTION)
	{
		lgmagic_dev_dbg(&hdev->dev, "Ignored report with size %d and type %x", size, data[0]);
		return 0; // Not ours
	}

	// Button is last two bytes before wheel
	u16 btn_code = (data[17] << 8) | data[18];
	s8 wheel = (s8)data[19];
	u8 motion_state = data[3] & LGMAGIC_MOTION_STATE_MASK;

	/* Parse counter (little-endian) */
	counter = data[1] | (data[2] << 8);

	spin_lock_irqsave(&drvdata->lock, flags);
	drvdata->battery = data[3] >> 2;
	spin_unlock_irqrestore(&drvdata->lock, flags);

	if (motion_state != drvdata->last_motion_state)
	{
		lgmagic_dev_dbg(&hdev->dev, "Motion state %u (byte 3 %02x)",
				motion_state, data[3]);
		drvdata->last_motion_state = motion_state;
	}

	/* Parse 6 signed 16-bit values, big-endian */
	for (i = 0; i < 6; i++) {
		imu[i] = (data[5 + 2*i] << 8) | data[6 + 2*i];
	}

	if (btn_code != drvdata->last_btncode)
	{
		if (drvdata->last_keycode)
			input_report_key(drvdata->input_hid, drvdata->last_keycode, 0);
		reporting = 1;
		drvdata->last_keycode = 0;
		drvdata->last_btncode = btn_code;
		u16 report_keycode = 0;
		if (btn_code != 0)
		{
			if (lgmagic_code_is_control(btn_code))
			{
				lgmagic_dev_dbg(&hdev->dev, "Stream control code %04x",
						btn_code);
			}
			else
			{
				for (int i = 0; i < ARRAY_SIZE(lg_btn_map); i++) {
					if (lg_btn_map[i].code == btn_code) {
						report_keycode = lg_btn_map[i].keycode;

						if (lg_btn_map[i].code==LGMAGIC_CODE_WHEEL && drvdata->mode)
							report_keycode = BTN_LEFT;
						else
							drvdata->mode = 0;

						input_report_key(drvdata->input_hid, report_keycode, 1);
						drvdata->last_keycode = report_keycode;
						break;
					}
				}

				if (!report_keycode)
					lgmagic_dev_dbg(&hdev->dev, "Unmapped button code %04x",
							btn_code);
			}
		}
	}

	if (wheel != 0)
	{
		if (drvdata->mode)
			input_report_rel(drvdata->input_hid, REL_WHEEL, wheel);
		else
		{
			input_report_key(drvdata->input_hid, wheel>0 ? KEY_UP : KEY_DOWN, 1);
			input_report_key(drvdata->input_hid, wheel>0 ? KEY_UP : KEY_DOWN, 0);
		}
		reporting = 1;
	}

	if (drvdata->airmouse_enabled && drvdata->pointer_ready &&
	    motion_state == LGMAGIC_MOTION_STATE_VALID)
	{
		int bigmove = lgmagic_calc_mouse_gravity(&drvdata->airmouse,
							 imu, &imu[3],
							 mouse);
		if (bigmove)
			drvdata->mode = 1;

		if (drvdata->mode == 1)
		{
			input_report_rel(drvdata->input_hid, REL_X, mouse[0]);
			input_report_rel(drvdata->input_hid, REL_Y, mouse[1]);
		}
	}

	if (mouse[0] || mouse[1])
		reporting = 1;

	if (reporting)
	{
		input_sync(drvdata->input_hid);
	}

	if (!imu_evdev)
		return 0;

	/* Report counter */
	input_event(drvdata->input_imu, EV_MSC, MSC_SERIAL, counter);

	/* Report IMU axes */
	input_report_abs(drvdata->input_imu, ABS_X,  imu[3]);
	input_report_abs(drvdata->input_imu, ABS_Y,  imu[4]);
	input_report_abs(drvdata->input_imu, ABS_Z,  imu[5]);
	input_report_abs(drvdata->input_imu, ABS_RX, imu[0]);
	input_report_abs(drvdata->input_imu, ABS_RY, imu[1]);
	input_report_abs(drvdata->input_imu, ABS_RZ, imu[2]);

	input_sync(drvdata->input_imu);

	return 0;
}

static ssize_t fw_version_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct lgmagic_drvdata *drvdata = hid_get_drvdata(to_hid_device(dev));
	u8 fw_ver[LGMAGIC_FW_VER_MAX];
	unsigned long flags;
	size_t len, i;
	int at = 0;

	spin_lock_irqsave(&drvdata->lock, flags);
	len = drvdata->fw_len;
	memcpy(fw_ver, drvdata->fw_ver, len);
	spin_unlock_irqrestore(&drvdata->lock, flags);

	for (i = 0; i < len; i++)
		at += sysfs_emit_at(buf, at, "%02x", fw_ver[i]);

	return at + sysfs_emit_at(buf, at, "\n");
}
static DEVICE_ATTR_RO(fw_version);

static ssize_t lgmagic_u8_show(struct lgmagic_drvdata *drvdata,
			       const u8 *field, char *buf)
{
	unsigned long flags;
	u8 val;

	spin_lock_irqsave(&drvdata->lock, flags);
	val = *field;
	spin_unlock_irqrestore(&drvdata->lock, flags);

	return sysfs_emit(buf, "%u\n", val);
}

static ssize_t fw_major_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct lgmagic_drvdata *drvdata = hid_get_drvdata(to_hid_device(dev));

	return lgmagic_u8_show(drvdata, &drvdata->fw_major, buf);
}
static DEVICE_ATTR_RO(fw_major);

static ssize_t fw_zone_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct lgmagic_drvdata *drvdata = hid_get_drvdata(to_hid_device(dev));

	return lgmagic_u8_show(drvdata, &drvdata->fw_zone, buf);
}
static DEVICE_ATTR_RO(fw_zone);

static ssize_t battery_level_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct lgmagic_drvdata *drvdata = hid_get_drvdata(to_hid_device(dev));

	return lgmagic_u8_show(drvdata, &drvdata->battery, buf);
}
static DEVICE_ATTR_RO(battery_level);

static ssize_t scd_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct lgmagic_drvdata *drvdata = hid_get_drvdata(to_hid_device(dev));
	u8 scd[LGMAGIC_SCD_MAX];
	unsigned long flags;
	size_t len, i;
	int at = 0;

	spin_lock_irqsave(&drvdata->lock, flags);
	len = drvdata->scd_len;
	memcpy(scd, drvdata->scd, len);
	spin_unlock_irqrestore(&drvdata->lock, flags);

	for (i = 0; i < len; i++)
		at += sysfs_emit_at(buf, at, "%02x", scd[i]);

	return at + sysfs_emit_at(buf, at, "\n");
}
static DEVICE_ATTR_RO(scd);

static ssize_t airmouse_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct lgmagic_drvdata *drvdata = hid_get_drvdata(to_hid_device(dev));

	return sysfs_emit(buf, "%u\n", drvdata->airmouse_enabled);
}

static ssize_t airmouse_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct lgmagic_drvdata *drvdata = hid_get_drvdata(to_hid_device(dev));
	int val;

	if (kstrtoint(buf, 0, &val) || (val != 0 && val != 1))
		return -EINVAL;

	lgmagic_set_airmouse(drvdata, val);
	return count;
}
static DEVICE_ATTR_RW(airmouse);

static ssize_t airmouse_threshold_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct lgmagic_drvdata *drvdata = hid_get_drvdata(to_hid_device(dev));

	return sysfs_emit(buf, "%lld\n",
			  lgmagic_radps_to_degps(drvdata->airmouse.threshold));
}

static ssize_t airmouse_threshold_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct lgmagic_drvdata *drvdata = hid_get_drvdata(to_hid_device(dev));
	int val;

	if (kstrtoint(buf, 0, &val) || val < 0 || val > LGMAGIC_THRESHOLD_MAX)
		return -EINVAL;

	drvdata->airmouse.threshold = lgmagic_degps_to_radps(val);
	return count;
}
static DEVICE_ATTR_RW(airmouse_threshold);

static ssize_t sensitivity_x_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct lgmagic_drvdata *drvdata = hid_get_drvdata(to_hid_device(dev));

	return sysfs_emit(buf, "%lld\n",
			  drvdata->airmouse.sensitivity_x / LGMAGIC_FP_ONE);
}

static ssize_t sensitivity_x_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct lgmagic_drvdata *drvdata = hid_get_drvdata(to_hid_device(dev));
	int val;

	if (kstrtoint(buf, 0, &val) || val < 0 || val > LGMAGIC_SENSITIVITY_MAX)
		return -EINVAL;

	drvdata->airmouse.sensitivity_x = (s64)val * LGMAGIC_FP_ONE;
	return count;
}
static DEVICE_ATTR_RW(sensitivity_x);

static ssize_t sensitivity_y_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct lgmagic_drvdata *drvdata = hid_get_drvdata(to_hid_device(dev));

	return sysfs_emit(buf, "%lld\n",
			  drvdata->airmouse.sensitivity_y / LGMAGIC_FP_ONE);
}

static ssize_t sensitivity_y_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct lgmagic_drvdata *drvdata = hid_get_drvdata(to_hid_device(dev));
	int val;

	if (kstrtoint(buf, 0, &val) || val < 0 || val > LGMAGIC_SENSITIVITY_MAX)
		return -EINVAL;

	drvdata->airmouse.sensitivity_y = (s64)val * LGMAGIC_FP_ONE;
	return count;
}
static DEVICE_ATTR_RW(sensitivity_y);

static ssize_t motion_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct lgmagic_drvdata *drvdata = hid_get_drvdata(to_hid_device(dev));

	return sysfs_emit(buf, "%u\n", drvdata->motion_on);
}
static DEVICE_ATTR_RO(motion);

static struct attribute *lgmagic_attrs[] = {
	&dev_attr_fw_version.attr,
	&dev_attr_fw_major.attr,
	&dev_attr_fw_zone.attr,
	&dev_attr_battery_level.attr,
	&dev_attr_scd.attr,
	&dev_attr_airmouse.attr,
	&dev_attr_airmouse_threshold.attr,
	&dev_attr_sensitivity_x.attr,
	&dev_attr_sensitivity_y.attr,
	&dev_attr_motion.attr,
	NULL
};

static const struct attribute_group lgmagic_group = {
	.attrs = lgmagic_attrs,
};

static void lgmagic_startup_commands(struct lgmagic_drvdata *drvdata)
{
	lgmagic_cmd_send(drvdata->hdev, LGMAGIC_CMD_FW_VER);
	if (!drvdata->scd_done)
		lgmagic_request_scd(drvdata);
	lgmagic_set_motion(drvdata, drvdata->airmouse_enabled);
}

static void lgmagic_scd_retry_work(struct work_struct *work){
	struct lgmagic_drvdata *drvdata =
		container_of(work, struct lgmagic_drvdata, scd_retry.work);

	if (drvdata->shutdown || drvdata->scd_done ||
	    drvdata->blob_valid)
		return;

	if (drvdata->scd_retries++ >= LGMAGIC_SCD_RETRIES)
	{
		lgmagic_dev_warn(&drvdata->hdev->dev,
				 "No SCD response after %u retries",
				 LGMAGIC_SCD_RETRIES);
		return;
	}

	lgmagic_request_scd(drvdata);
	schedule_delayed_work(&drvdata->scd_retry,
			      msecs_to_jiffies(LGMAGIC_SCD_RETRY_MS));
}

/* Probe commands are often missed; repeat once reports flow. */
static void lgmagic_startup_work(struct work_struct *work)
{
	struct lgmagic_drvdata *drvdata;

	drvdata = container_of(work, struct lgmagic_drvdata, startup);

	if (drvdata->shutdown)
		return;

	lgmagic_startup_commands(drvdata);

	if (!drvdata->blob_valid && !drvdata->scd_done)
		schedule_delayed_work(&drvdata->scd_retry,
				      msecs_to_jiffies(LGMAGIC_SCD_RETRY_MS));
}

static void lgmagic_calib_name(const char *uniq, char *out, size_t out_size)
{
	char mac[18];
	size_t i;

	for (i = 0; i < 17 && uniq[i]; i++)
		mac[i] = uniq[i] == ':' ? '_' : uniq[i];
	mac[i] = '\0';

	snprintf(out, out_size, "lg_magic_calib_%s.bin", mac);
}

static int lgmagic_load_fw(const char *fwname, struct device *dev, struct lgmagic_drvdata *drvdata)
{
	const struct firmware *fw;
	s64 bias[3], scale[3];
	int ret, i;

	ret = request_firmware(&fw, fwname, dev);
	if (ret)
		return ret;

	ret = lgmagic_parse_calib_phys(bias, scale, fw->data, fw->size);
	release_firmware(fw);

	if (ret)
	{
		lgmagic_dev_warn(dev, "Calibration %s is invalid or has zero scale; falling back to SCD", fwname);
		return ret;
	}

	for (i = 0; i < 3; i++)
	{
		drvdata->airmouse.gyro_bias[i] = bias[i];
		drvdata->airmouse.gyro_matrix[i][i] = scale[i];
	}

	drvdata->airmouse.gyro_matrix_valid = 1;
	drvdata->blob_valid = true;
	drvdata->pointer_ready = true;

	lgmagic_dev_info(dev, "Loaded LG Magic physical calibration from %s", fwname);

	return 0;
}

static int lgmagic_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	int ret, i;
	unsigned long flags;
	struct lgmagic_drvdata *drvdata;

	drvdata = devm_kzalloc(&hdev->dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	hid_set_drvdata(hdev, drvdata);
	drvdata->hdev = hdev;
	lgmagic_airmouse_init(&drvdata->airmouse);
	drvdata->airmouse_enabled = !!airmouse;
	drvdata->airmouse.threshold =
		lgmagic_degps_to_radps(lgmagic_clamp_param(airmouse_threshold,
							   LGMAGIC_THRESHOLD_MAX));
	drvdata->airmouse.sensitivity_x =
		(s64)lgmagic_clamp_param(sensitivity_x,
					 LGMAGIC_SENSITIVITY_MAX) *
		LGMAGIC_FP_ONE;
	drvdata->airmouse.sensitivity_y =
		(s64)lgmagic_clamp_param(sensitivity_y,
					 LGMAGIC_SENSITIVITY_MAX) *
		LGMAGIC_FP_ONE;
	INIT_WORK(&drvdata->startup, lgmagic_startup_work);
	INIT_DELAYED_WORK(&drvdata->scd_retry, lgmagic_scd_retry_work);
	spin_lock_init(&drvdata->lock);

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret)
		return ret;

	if (strlen(hdev->uniq)==17)
	{
		char addr_fw_name[sizeof("lg_magic_calib_XX_XX_XX_XX_XX_XX.bin")];

		lgmagic_calib_name(hdev->uniq, addr_fw_name,
				   sizeof(addr_fw_name));
		if (lgmagic_load_fw(addr_fw_name, &hdev->dev, drvdata)==0)
			goto loaded;
	}
		lgmagic_load_fw("lg_magic_calib.bin", &hdev->dev, drvdata);
loaded:

	{
		s64 cached_gyro[3][3], cached_accel[3][3], cached_remap[3][3];
		s64 cached_scale[3], cached_average, cached_dt;
		u8 cached_raw[LGMAGIC_SCD_SIZE];

		/* A cache hit means no new F9 17 request. */
		if (lgmagic_scd_cache_lookup(hdev->uniq, cached_raw) &&
		    !lgmagic_parse_scd_matrices(cached_raw, sizeof(cached_raw),
						cached_gyro, cached_accel,
						cached_remap, cached_scale,
						&cached_average, &cached_dt))
		{
			lgmagic_apply_scd(drvdata, cached_gyro, cached_accel,
					  cached_remap, cached_scale,
					  cached_average, cached_dt);
			spin_lock_irqsave(&drvdata->lock, flags);
			memcpy(drvdata->scd, cached_raw, sizeof(cached_raw));
			drvdata->scd_len = sizeof(cached_raw);
			drvdata->scd_done = true;
			spin_unlock_irqrestore(&drvdata->lock, flags);
		}
	}

	drvdata->input_hid = devm_input_allocate_device(&hdev->dev);
	if (!drvdata->input_hid)
		return -ENOMEM;

	drvdata->input_hid->name = "LG Magic Remote";
	drvdata->input_hid->id.bustype = hdev->bus;
	drvdata->input_hid->id.vendor = hdev->vendor;
	drvdata->input_hid->id.product = hdev->product;

	set_bit(EV_KEY, drvdata->input_hid->evbit);
	set_bit(EV_REL, drvdata->input_hid->evbit);
	set_bit(REL_WHEEL, drvdata->input_hid->relbit);
	set_bit(REL_X, drvdata->input_hid->relbit);
	set_bit(REL_Y, drvdata->input_hid->relbit);

	for (i = 0; i < ARRAY_SIZE(lg_btn_map); i++)
		set_bit(lg_btn_map[i].keycode, drvdata->input_hid->keybit);

	ret = input_register_device(drvdata->input_hid);
	if (ret)
		return ret;

	drvdata->input_imu = devm_input_allocate_device(&hdev->dev);
	if (!drvdata->input_imu)
		return -ENOMEM;

	drvdata->input_imu->name = "LG Magic Remote IMU";
	drvdata->input_imu->id.bustype = hdev->bus;
	drvdata->input_imu->id.vendor = hdev->vendor;
	drvdata->input_imu->id.product = hdev->product;

	set_bit(EV_ABS, drvdata->input_imu->evbit);
	set_bit(EV_MSC, drvdata->input_imu->evbit);

	set_bit(MSC_SERIAL, drvdata->input_imu->mscbit);

	for (i = ABS_X; i <= ABS_RZ; i++)
	{
		set_bit(i, drvdata->input_imu->absbit);
		input_set_abs_params(drvdata->input_imu, i, -32768, 32767, 4, 4);
	}

	if (imu_evdev)
	{
		ret = input_register_device(drvdata->input_imu);
		if (ret)
			return ret;
	}

	ret = devm_device_add_group(&hdev->dev, &lgmagic_group);
	if (ret)
		return ret;

	lgmagic_startup_commands(drvdata);

	return 0;
}

static void lgmagic_remove(struct hid_device *hdev)
{
	struct lgmagic_drvdata *drvdata = hid_get_drvdata(hdev);

	drvdata->shutdown = true;
	cancel_delayed_work_sync(&drvdata->scd_retry);
	cancel_work_sync(&drvdata->startup);
	hid_hw_stop(hdev);
}

static const struct hid_device_id lgmagic_devices[] = {
	{ HID_BLUETOOTH_DEVICE(0x000f, 0x3412) }, // LG Magic Remote MR19/MR20
	{ }
};
MODULE_DEVICE_TABLE(hid, lgmagic_devices);

static struct hid_driver lgmagic_driver = {
	.name = "lgmagic",
	.id_table = lgmagic_devices,
	.raw_event = lgmagic_raw_event,
	.probe = lgmagic_probe,
	.remove = lgmagic_remove,
};

module_hid_driver(lgmagic_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ilya \"Kokokoshka\" Chelyadin <ilya77105@gmail.com>");
MODULE_DESCRIPTION("LG Magic Remote HID Driver");
