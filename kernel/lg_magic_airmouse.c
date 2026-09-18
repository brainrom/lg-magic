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

#include <linux/bits.h>
#include <linux/limits.h>
#include <linux/types.h>
#include "lg_magic_airmouse.h"

#define LGMAGIC_F32_MANT_BITS	23
#define LGMAGIC_F32_EXP_BIAS	127

static inline s64 lgmagic_fabs(s64 val)
{
	if (val<0)
		return -val;
	return val;
}

static inline s64 lgmagic_fpmul(s64 a, s64 b)
{
	return (a * b) >> LGMAGIC_FP_SHIFT;
}

static s64 lgmagic_f32_to_fp(u32 bits)
{
	int shift;
	s64 mant;
	int exp;

	exp = (bits >> LGMAGIC_F32_MANT_BITS) & 0xFF;
	mant = bits & GENMASK(LGMAGIC_F32_MANT_BITS - 1, 0);

	if (!exp)
		return 0;

	if (exp == 0xFF)
		return S64_MAX;

	mant |= BIT(LGMAGIC_F32_MANT_BITS);
	shift = exp - LGMAGIC_F32_EXP_BIAS - LGMAGIC_F32_MANT_BITS +
		LGMAGIC_FP_SHIFT;

	if (shift > 24)
		return S64_MAX;

	if (shift >= 0)
		mant <<= shift;
	else if (-shift < 62)
		mant >>= -shift;
	else
		mant = 0;

	return (bits & BIT(31)) ? -mant : mant;
}

void lgmagic_convert_calib(struct lg_magic_airmouse_calib *calib, const u8 *blob)
{
	size_t i;
	u32 bits;

	for (i = 0; i < LGMAGIC_CALIB_SIZE / sizeof(bits); i++)
	{
		bits = blob[4 * i] | (blob[4 * i + 1] << 8) |
		       (blob[4 * i + 2] << 16) | ((u32)blob[4 * i + 3] << 24);

		if (i < 3)
			calib->gyro_bias[i] = lgmagic_f32_to_fp(bits);
		else if (i < 6)
			calib->gyro_scale[i - 3] = lgmagic_f32_to_fp(bits);
		else if (i == 6)
			calib->alpha = lgmagic_f32_to_fp(bits);
		else
			calib->mouse_k = lgmagic_f32_to_fp(bits);
	}
}

int lgmagic_validate_calib(struct lg_magic_airmouse_calib *calib)
{
	for (size_t i = 0; i < 3; i++)
	{
		if (lgmagic_fabs(calib->gyro_bias[i])>100*LGMAGIC_FP_ONE || lgmagic_fabs(calib->gyro_scale[i])>10*LGMAGIC_FP_ONE)
			return 1;
	}
	if (calib->alpha<0 || calib->alpha>LGMAGIC_FP_ONE)
			return 1;

	if (calib->mouse_k<0 || calib->mouse_k>LGMAGIC_FP_ONE)
		return 1;

	return 0;
}

int lgmagic_calc_mouse(struct lg_magic_airmouse_calib *calib, s64 *gyro_acc, u16 threshold, s16 *gyro, s16 *mouse)
{
	s64 limit = (s64)threshold << LGMAGIC_FP_SHIFT;

	for (size_t i = 0; i < 3; i++)
	{
		s64 gyro_corr = ((s64)gyro[i] << LGMAGIC_FP_SHIFT) - calib->gyro_bias[i];

		gyro_corr = lgmagic_fpmul(gyro_corr, calib->gyro_scale[i]);
		gyro_acc[i] = lgmagic_fpmul(calib->alpha, gyro_corr) +
			      lgmagic_fpmul(LGMAGIC_FP_ONE - calib->alpha, gyro_acc[i]);
	}

	mouse[0] = lgmagic_fpmul(gyro_acc[2], calib->mouse_k) >> LGMAGIC_FP_SHIFT;
	mouse[1] = lgmagic_fpmul(gyro_acc[0], calib->mouse_k) >> LGMAGIC_FP_SHIFT;

	if (lgmagic_fabs(gyro_acc[0])>limit || lgmagic_fabs(gyro_acc[2])>limit)
		return 1;
	return 0;
}
