#ifndef LG_MAGIC_AIRMOUSE_H
#define LG_MAGIC_AIRMOUSE_H

#define LGMAGIC_FP_SHIFT	16
#define LGMAGIC_FP_ONE		(1LL << LGMAGIC_FP_SHIFT)

#define LGMAGIC_CALIB_SIZE	32

struct lg_magic_airmouse_calib
{
	s64 gyro_bias[3];
	s64 gyro_scale[3];
	s64 alpha;
	s64 mouse_k;
};

void lgmagic_convert_calib(struct lg_magic_airmouse_calib *calib, const u8 *blob);
int lgmagic_calc_mouse(struct lg_magic_airmouse_calib *calib, s64 *gyro_acc, u16 threshold, s16 *gyro, s16 *mouse);
int lgmagic_validate_calib(struct lg_magic_airmouse_calib *calib);

#endif
