#ifndef LG_MAGIC_AIRMOUSE_H
#define LG_MAGIC_AIRMOUSE_H

#include <linux/types.h>

#define LGMAGIC_FP_SHIFT	16
#define LGMAGIC_FP_ONE		(1LL << LGMAGIC_FP_SHIFT)

/*
 * On-disk calibration blob.  All values are signed 16.16 fixed-point
 * numbers stored in 32 bits, in little-endian byte order.  Keep this
 * layout in sync with scripts/convert_calib.py.
 */
#define LGMAGIC_CALIB_MAGIC	0x3243474c	/* "LGC2" */
#define LGMAGIC_CALIB_VERSION	1

struct lgmagic_calib_blob
{
	__le32 magic;
	__le16 version;
	__le16 size;
	__le32 gyro_bias[3];
	__le32 gyro_scale[3];
	__le32 alpha;
	__le32 mouse_k;
};

struct lg_magic_airmouse_calib
{
	s64 gyro_bias[3];
	s64 gyro_scale[3];
	s64 alpha;
	s64 mouse_k;
};

int lgmagic_convert_calib(struct lg_magic_airmouse_calib *calib,
			  const u8 *blob, size_t size);
int lgmagic_calc_mouse(struct lg_magic_airmouse_calib *calib, s64 *gyro_acc, u16 threshold, s16 *gyro, s16 *mouse);
int lgmagic_validate_calib(struct lg_magic_airmouse_calib *calib);

#endif
