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
#include <linux/spinlock.h>
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
MODULE_PARM_DESC(airmouse, "Report mouse events");

static int airmouse_threshold = 300;
module_param(airmouse_threshold, int, 0644);
MODULE_PARM_DESC(airmouse_threshold, "Airmouse enable threshold");

static int imu_evdev = 0;
module_param(imu_evdev, int, 0644);
MODULE_PARM_DESC(imu_evdev, "Expose raw IMU");

#define LGMAGIC_SCD_MAX 512
#define LGMAGIC_FW_VER_MAX 32

struct lgmagic_drvdata {
	struct input_dev *input_hid;
	struct input_dev *input_imu;
	struct hid_device *hdev;
	struct work_struct startup;
	bool started;

	spinlock_t lock;
	u8 fw_ver[LGMAGIC_FW_VER_MAX];
	size_t fw_len;
	u8 scd[LGMAGIC_SCD_MAX];
	size_t scd_len;
	bool scd_done;
	u8 fw_major;
	u8 fw_zone;
	u8 battery;

	u16 last_keycode;
	u16 last_btncode;
	s64 gyro_acc[3];
	int mode;
	struct lg_magic_airmouse_calib calib;
};

#define LGMAGIC_REPORT_CMD 0xF9
#define LGMAGIC_REPORT_VOICE 0xFE

#define LGMAGIC_CMD_MOTION_ON 0x01
#define LGMAGIC_CMD_FW_VER 0x11
#define LGMAGIC_CMD_SCD 0x17

#define LGMAGIC_RSP_FW_VER 0x12
#define LGMAGIC_RSP_FW_VER_CK 0x14
#define LGMAGIC_RSP_SCD 0x17

#define LGMAGIC_CODE_WHEEL 0x8044

static const struct {
	u16 code;
	u16 keycode;
} lg_btn_map[] = {
	{ 0x8000, KEY_POWER },
	{ 0x8099, KEY_SLEEP },
	{ 0x8010, KEY_0 }, { 0x8011, KEY_1 }, { 0x8012, KEY_2 },
	{ 0x8013, KEY_3 }, { 0x8014, KEY_4 }, { 0x8015, KEY_5 },
	{ 0x8016, KEY_6 }, { 0x8017, KEY_7 }, { 0x8018, KEY_8 }, { 0x8019, KEY_9 },
	{ LGMAGIC_CODE_WHEEL, KEY_ENTER },
	{ LGMAGIC_CODE_WHEEL, BTN_LEFT },
	{ 0x8053, KEY_LIST },
	{ 0x8045, KEY_MENU }, // ... button
	{ 0x8002, KEY_VOLUMEUP },
	{ 0x8003, KEY_VOLUMEDOWN },
	{ 0x8009, KEY_MUTE },
	{ 0x808B, KEY_VOICECOMMAND },
	{ 0x807C, KEY_HOME },
	{ 0x8043, KEY_SETUP },
	{ 0x8028, KEY_BACK },
	{ 0x80AB, KEY_PROGRAM },
	{ 0x805D, KEY_MEDIA }, // IVI
	{ 0x800B, KEY_TV },
	{ 0x8098, KEY_CONTEXT_MENU }, // STB MENU
	//{ 0x8000, KEY_CHANNELUP }, // Somehow it collides with power button
	{ 0x8001, KEY_CHANNELDOWN },
	{ 0x8072, KEY_RED },
	{ 0x8071, KEY_GREEN },
	{ 0x8063, KEY_YELLOW },
	{ 0x8061, KEY_BLUE },
	{ 0x8081, KEY_VIDEO }, // MOVIES
	{ 0x80B0, KEY_PLAY },
	{ 0x80BA, KEY_PAUSE },
	{ 0x8040, KEY_UP },
	{ 0x8041, KEY_DOWN },
	{ 0x8006, KEY_RIGHT },
	{ 0x8007, KEY_LEFT },
};

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

/* The remote reuses one transmit buffer, so everything past data[3] bytes
 * is the previous frame and must not be stored.
 */
static void lgmagic_scd_chunk(struct hid_device *hdev, u8 *data, int size)
{
	struct lgmagic_drvdata *drvdata = hid_get_drvdata(hdev);
	unsigned long flags;
	size_t len;

	if (size < 4)
		return;

	len = min_t(size_t, data[3], size - 4);

	spin_lock_irqsave(&drvdata->lock, flags);
	if (drvdata->scd_done)
		drvdata->scd_len = 0;
	drvdata->scd_done = !data[2];
	len = min_t(size_t, len, LGMAGIC_SCD_MAX - drvdata->scd_len);
	memcpy(drvdata->scd + drvdata->scd_len, data + 4, len);
	drvdata->scd_len += len;
	spin_unlock_irqrestore(&drvdata->lock, flags);

	lgmagic_dev_dbg(&hdev->dev, "SCD %zu bytes, more %u", len, data[2]);
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

	if (!drvdata->started) {
		drvdata->started = true;
		schedule_work(&drvdata->startup);
	}

	if (size >= 1 && data[0] == LGMAGIC_REPORT_CMD)
	{
		lgmagic_cmd_response(hdev, data, size);
		return 0;
	}

	if (size >= 1 && data[0] == LGMAGIC_REPORT_VOICE)
		return 0;

	if (size != 20 || data[0] != 0xFD)
	{
		lgmagic_dev_warn(&hdev->dev, "Unknown descriptor with size %d and type %x", size, data[0]);
		return 0; // Not ours
	}

	// Button is last two bytes before wheel
	u16 btn_code = (data[17] << 8) | data[18];
	s8 wheel = (s8)data[19];

	/* Parse counter (little-endian) */
	counter = data[1] | (data[2] << 8);

	spin_lock_irqsave(&drvdata->lock, flags);
	drvdata->battery = data[3] >> 2;
	spin_unlock_irqrestore(&drvdata->lock, flags);

	/* Parse 6 signed 16-bit values, big-endian */
	for (i = 0; i < 6; i++) {
		imu[i] = (data[5 + 2*i] << 8) | data[6 + 2*i];
	}

	if (btn_code != drvdata->last_btncode)
	{
		input_report_key(drvdata->input_hid, drvdata->last_keycode, 0);
		reporting = 1;
		drvdata->last_keycode = 0;
		drvdata->last_btncode = btn_code;
		if (btn_code != 0)
		{
			for (int i = 0; i < ARRAY_SIZE(lg_btn_map); i++) {
				if (lg_btn_map[i].code == btn_code) {
					u16 report_keycode = lg_btn_map[i].keycode;

					if (lg_btn_map[i].code==LGMAGIC_CODE_WHEEL && drvdata->mode)
						report_keycode = BTN_LEFT;
					else
						drvdata->mode = 0;

					input_report_key(drvdata->input_hid, report_keycode, 1);
					drvdata->last_keycode = report_keycode;
					break;
				}
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

	if (airmouse)
	{
		int bigmove = lgmagic_calc_mouse(&drvdata->calib, drvdata->gyro_acc, airmouse_threshold, imu, mouse);
		if (bigmove)
			drvdata->mode = 1;

		if (drvdata->mode==1)
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

static struct attribute *lgmagic_attrs[] = {
	&dev_attr_fw_version.attr,
	&dev_attr_fw_major.attr,
	&dev_attr_fw_zone.attr,
	&dev_attr_battery_level.attr,
	&dev_attr_scd.attr,
	NULL
};

static const struct attribute_group lgmagic_group = {
	.attrs = lgmagic_attrs,
};

static void lgmagic_startup_commands(struct hid_device *hdev)
{
	lgmagic_cmd_send(hdev, LGMAGIC_CMD_FW_VER);
	lgmagic_cmd_send(hdev, LGMAGIC_CMD_SCD);
	lgmagic_cmd_send(hdev, LGMAGIC_CMD_MOTION_ON);
}

/* A command issued as probe ends is answered about half the time. The
 * remote's first report proves the link carries traffic, so repeat there.
 */
static void lgmagic_startup_work(struct work_struct *work)
{
	struct lgmagic_drvdata *drvdata;

	drvdata = container_of(work, struct lgmagic_drvdata, startup);

	lgmagic_startup_commands(drvdata->hdev);
}

static void lgmagic_sanitize_mac(const char *uniq, char *out)
{
	size_t i = 0;
	for (i = 0; uniq[i] && i<17; i++) {
		if (uniq[i] == ':')
			out[i] = '_';  /* or just skip it */
			else
				out[i] = uniq[i];
	}
}

static int lgmagic_load_fw(const char *fwname, struct device *dev, struct lgmagic_drvdata *drvdata)
{
	int ret;
	const struct firmware *fw;

	ret = request_firmware(&fw, fwname, dev);
	if (ret)
		return ret;

	if (lgmagic_convert_calib(&drvdata->calib, fw->data, fw->size))
	{
		lgmagic_dev_warn(dev, "Calibration %s is not a supported fixed-point blob (run scripts/convert_calib.py for legacy float files)", fwname);
		memset(&drvdata->calib, 0, sizeof(drvdata->calib));
		release_firmware(fw);
		return -EINVAL;
	}

	release_firmware(fw);

	if (lgmagic_validate_calib(&drvdata->calib))
	{
		lgmagic_dev_warn(dev, "Calibration %s isn't valid", fwname);
		memset(&drvdata->calib, 0, sizeof(drvdata->calib));
		return -EINVAL;
	}

	lgmagic_dev_info(dev, "Loaded LG Magic calibration from %s", fwname);

	return 0;
}

static int lgmagic_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	int ret, i;
	struct lgmagic_drvdata *drvdata;

	drvdata = devm_kzalloc(&hdev->dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	hid_set_drvdata(hdev, drvdata);
	drvdata->hdev = hdev;
	INIT_WORK(&drvdata->startup, lgmagic_startup_work);
	spin_lock_init(&drvdata->lock);

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret)
		return ret;

	if (strlen(hdev->uniq)==17)
	{
		char addr_fw_name[] = "lg_magic_calib_XX_XX_XX_XX_XX_XX.bin";
		lgmagic_sanitize_mac(hdev->uniq, addr_fw_name+sizeof("lg_magic_calib_")-1);
		if (lgmagic_load_fw(addr_fw_name, &hdev->dev, drvdata)==0)
			goto loaded;
	}
		lgmagic_load_fw("lg_magic_calib.bin", &hdev->dev, drvdata);
loaded:

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

	lgmagic_startup_commands(hdev);

	return 0;
}

static void lgmagic_remove(struct hid_device *hdev)
{
	struct lgmagic_drvdata *drvdata = hid_get_drvdata(hdev);

	cancel_work_sync(&drvdata->startup);
	hid_hw_stop(hdev);
}

static const struct hid_device_id lgmagic_devices[] = {
	{ HID_BLUETOOTH_DEVICE(0x000f, 0x3412) }, // LG Magic Remote
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
