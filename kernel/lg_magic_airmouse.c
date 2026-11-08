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

#include <linux/build_bug.h>
#include <asm/byteorder.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include "lg_magic_airmouse.h"

static_assert(sizeof(struct lgmagic_calib_blob) == 40,
	      "unexpected lgmagic calibration blob layout");

static inline s64 lgmagic_fabs(s64 val)
{
	if (val < 0)
		return -val;
	return val;
}

/*
 * Q16.16 multiply.  Use signed division instead of a right shift so that
 * negative values are truncated towards zero, matching the previous float
 * implementation.  The divisor is a power of two, so the compiler emits a
 * shift with a sign correction rather than a call to a division routine.
 */
static inline s64 lgmagic_fpmul(s64 a, s64 b)
{
	return (a * b) / LGMAGIC_FP_ONE;
}

int lgmagic_convert_calib(struct lg_magic_airmouse_calib *calib,
			  const u8 *data, size_t size)
{
	struct lgmagic_calib_blob blob;
	int i;

	if (size < sizeof(blob))
		return -EINVAL;

	memcpy(&blob, data, sizeof(blob));

	if (le32_to_cpu(blob.magic) != LGMAGIC_CALIB_MAGIC ||
	    le16_to_cpu(blob.version) != LGMAGIC_CALIB_VERSION ||
	    le16_to_cpu(blob.size) != (u16)sizeof(blob))
		return -EINVAL;

	for (i = 0; i < 3; i++)
	{
		calib->gyro_bias[i] = (s32)le32_to_cpu(blob.gyro_bias[i]);
		calib->gyro_scale[i] = (s32)le32_to_cpu(blob.gyro_scale[i]);
	}
	calib->alpha = (s32)le32_to_cpu(blob.alpha);
	calib->mouse_k = (s32)le32_to_cpu(blob.mouse_k);

	return 0;
}

int lgmagic_validate_calib(struct lg_magic_airmouse_calib *calib)
{
	int i;

	for (i = 0; i < 3; i++)
	{
		if (lgmagic_fabs(calib->gyro_bias[i]) > 100 * LGMAGIC_FP_ONE ||
		    lgmagic_fabs(calib->gyro_scale[i]) > 10 * LGMAGIC_FP_ONE)
			return 1;
	}

	if (calib->alpha < 0 || calib->alpha > LGMAGIC_FP_ONE)
		return 1;

	if (calib->mouse_k < 0 || calib->mouse_k > LGMAGIC_FP_ONE)
		return 1;

	return 0;
}

int lgmagic_calc_mouse(struct lg_magic_airmouse_calib *calib, s64 *gyro_acc, u16 threshold, s16 *gyro, s16 *mouse)
{
	s64 limit = (s64)threshold << LGMAGIC_FP_SHIFT;
	int i;

	for (i = 0; i < 3; i++)
	{
		s64 gyro_corr = ((s64)gyro[i] << LGMAGIC_FP_SHIFT) -
				calib->gyro_bias[i];

		gyro_corr = lgmagic_fpmul(gyro_corr, calib->gyro_scale[i]);
		gyro_acc[i] = lgmagic_fpmul(calib->alpha, gyro_corr) +
			      lgmagic_fpmul(LGMAGIC_FP_ONE - calib->alpha,
					    gyro_acc[i]);
	}

	mouse[0] = lgmagic_fpmul(gyro_acc[2], calib->mouse_k) / LGMAGIC_FP_ONE;
	mouse[1] = lgmagic_fpmul(gyro_acc[0], calib->mouse_k) / LGMAGIC_FP_ONE;

	if (lgmagic_fabs(gyro_acc[0]) > limit ||
	    lgmagic_fabs(gyro_acc[2]) > limit)
		return 1;

	return 0;
}
