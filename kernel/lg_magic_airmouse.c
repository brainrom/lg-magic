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
#include <linux/printk.h>
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

/* Q16.16 multiply; division keeps negative values truncated toward zero. */
static inline s64 lgmagic_fpmul(s64 a, s64 b)
{
	return (a * b) / LGMAGIC_FP_ONE;
}

#define LGMAGIC_DEG_TO_RAD_Q16	1144	/* (pi / 180) in Q16.16 */

int lgmagic_parse_calib_phys(s64 gyro_bias[3], s64 gyro_scale[3],
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
		s64 bias = (s32)le32_to_cpu(blob.gyro_bias[i]);
		s64 scale = (s32)le32_to_cpu(blob.gyro_scale[i]);

		if (lgmagic_fabs(bias) > 100 * LGMAGIC_FP_ONE ||
		    scale <= 0 || scale > 10 * LGMAGIC_FP_ONE)
			return -EINVAL;

		gyro_bias[i] = bias;
		gyro_scale[i] = (scale * LGMAGIC_DEG_TO_RAD_Q16 +
				 LGMAGIC_FP_ONE / 2) / LGMAGIC_FP_ONE;
	}

	if (le32_to_cpu(blob.alpha) || le32_to_cpu(blob.mouse_k))
		pr_warn_once("lg_magic: calibration alpha/mouse_k are deprecated and ignored\n");

	return 0;
}

s64 lgmagic_degps_to_radps(s64 degps)
{
	return degps * LGMAGIC_DEG_TO_RAD_Q16;
}

s64 lgmagic_radps_to_degps(s64 radps)
{
	if (radps >= 0)
		return (radps + LGMAGIC_DEG_TO_RAD_Q16 / 2) /
		       LGMAGIC_DEG_TO_RAD_Q16;

	return -((-radps + LGMAGIC_DEG_TO_RAD_Q16 / 2) /
		 LGMAGIC_DEG_TO_RAD_Q16);
}

static u32 lgmagic_get_le32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
	       ((u32)p[3] << 24);
}

/* Convert an IEEE-754 binary32 bit pattern to Q16.16 without FPU. */
static int lgmagic_float_to_q16(u32 bits, s64 *out)
{
	u32 sign = bits >> 31;
	u32 exp = (bits >> 23) & 0xff;
	u32 mant = bits & 0x7fffff;
	u64 m;
	int shift;
	s64 v;

	if (exp == 0xff)
		return -EINVAL;
	if (exp == 0)
	{
		if (mant == 0)
		{
			*out = 0;
			return 0;
		}
		return -EINVAL; /* subnormals are not expected here */
	}

	m = (1ULL << 23) | mant;
	shift = (int)exp - 134; /* value * 2^16 = m * 2^(exp-134) */

	if (shift >= 0)
	{
		if (shift > 40 || m > (~(u64)0 >> shift))
			return -EINVAL;
		v = (s64)(m << shift);
	}
	else
	{
		int rshift = -shift;

		if (rshift >= 64)
			v = 0;
		else
			v = (s64)((m + (1ULL << (rshift - 1))) >> rshift);
	}

	*out = sign ? -v : v;
	return 0;
}

/* floor(sqrt(x)); avoids a kernel-version dependency in the host tests. */
static u64 lgmagic_isqrt64(u64 x)
{
	u64 res = 0;
	u64 bit = 1ULL << 62;

	while (bit > x)
		bit >>= 2;

	while (bit)
	{
		if (x >= res + bit)
		{
			x -= res + bit;
			res = (res >> 1) + bit;
		}
		else
		{
			res >>= 1;
		}
		bit >>= 2;
	}

	return res;
}

static s64 lgmagic_fpdot(const s64 a[3], const s64 b[3])
{
	return (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) /
	       LGMAGIC_FP_ONE;
}

static void lgmagic_fpcross(const s64 a[3], const s64 b[3], s64 out[3])
{
	out[0] = (a[1] * b[2] - a[2] * b[1]) / LGMAGIC_FP_ONE;
	out[1] = (a[2] * b[0] - a[0] * b[2]) / LGMAGIC_FP_ONE;
	out[2] = (a[0] * b[1] - a[1] * b[0]) / LGMAGIC_FP_ONE;
}

static int lgmagic_vec_normalize(s64 v[3])
{
	s64 n2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
	u64 n;
	int i;

	if (n2 <= 0)
		return -EINVAL;

	n = lgmagic_isqrt64((u64)n2);
	if (!n)
		return -EINVAL;

	for (i = 0; i < 3; i++)
		v[i] = (v[i] * LGMAGIC_FP_ONE) / (s64)n;

	return 0;
}

static int lgmagic_accel_unit(const s16 a[3], s64 out[3])
{
	s64 x = a[0], y = a[1], z = a[2];
	u64 n2 = (u64)(x * x) + (u64)(y * y) + (u64)(z * z);
	u64 n;
	int i;

	n = lgmagic_isqrt64(n2);
	if (!n)
		return -EINVAL;

	for (i = 0; i < 3; i++)
		out[i] = ((s64)a[i] * LGMAGIC_FP_ONE) / (s64)n;

	return 0;
}

static s16 lgmagic_clamp_s16(s64 v)
{
	if (v > 32767)
		return 32767;
	if (v < -32768)
		return -32768;
	return (s16)v;
}

/* Largest accepted SCD matrix element; keeps the norm computations in u64. */
#define LGMAGIC_MATRIX_ELEMENT_MAX	(1LL << 30)
#define LGMAGIC_MATRIX_TOL		(LGMAGIC_FP_ONE / 16)
#define LGMAGIC_ACCEL_COUNTS_PER_G	41

#define LGMAGIC_SCD_FLOAT_BASE		9
#define LGMAGIC_SCD_ACCEL_FLOAT		2
#define LGMAGIC_SCD_GYRO_FLOAT		24

static s64 lgmagic_mat3_norm(const s64 m[3][3], bool column, int idx)
{
	u64 n2 = 0;
	int i;

	for (i = 0; i < 3; i++)
	{
		s64 v = column ? m[i][idx] : m[idx][i];

		n2 += (u64)(v * v);
	}

	return (s64)lgmagic_isqrt64(n2);
}

/* Normalize rows or columns; one of the two is an orthonormal rotation. */
static int lgmagic_mat3_normalize(const s64 m[3][3], bool columns,
				  s64 r[3][3], s64 *average)
{
	s64 n[3], sum = 0;
	int i, j;

	for (i = 0; i < 3; i++)
	{
		n[i] = lgmagic_mat3_norm(m, columns, i);
		if (n[i] <= 0)
			return -EINVAL;
		sum += n[i];
	}

	for (i = 0; i < 3; i++)
	{
		for (j = 0; j < 3; j++)
		{
			s64 den = columns ? n[j] : n[i];

			r[i][j] = (m[i][j] * LGMAGIC_FP_ONE) / den;
		}
	}

	*average = (sum + 1) / 3;
	return 0;
}

static s64 lgmagic_det3(const s64 m[3][3])
{
	s64 a = m[0][0], b = m[0][1], c = m[0][2];
	s64 d = m[1][0], e = m[1][1], f = m[1][2];
	s64 g = m[2][0], h = m[2][1], i = m[2][2];

	return lgmagic_fpmul(a, lgmagic_fpmul(e, i) - lgmagic_fpmul(f, h)) -
	       lgmagic_fpmul(b, lgmagic_fpmul(d, i) - lgmagic_fpmul(f, g)) +
	       lgmagic_fpmul(c, lgmagic_fpmul(d, h) - lgmagic_fpmul(e, g));
}

static bool lgmagic_mat3_is_rotation(const s64 r[3][3])
{
	s64 det;
	int i, j, k;

	for (i = 0; i < 3; i++)
	{
		for (j = 0; j < 3; j++)
		{
			s64 dot = 0;
			s64 expected = (i == j) ? LGMAGIC_FP_ONE : 0;

			for (k = 0; k < 3; k++)
				dot += lgmagic_fpmul(r[k][i], r[k][j]);

			if (lgmagic_fabs(dot - expected) > LGMAGIC_MATRIX_TOL)
				return false;
		}
	}

	det = lgmagic_det3(r);

	return lgmagic_fabs(det - LGMAGIC_FP_ONE) <= LGMAGIC_MATRIX_TOL;
}

/* Accept a proper rotation times per-axis scales. */
static int lgmagic_mat3_validate(const s64 m[3][3], s64 min_scale,
				 s64 max_scale, s64 *average,
				 s64 rotation[3][3], s64 axis_scale[3])
{
	s64 r[3][3], scale;
	int i, j;

	for (i = 0; i < 3; i++)
	{
		for (j = 0; j < 3; j++)
		{
			if (m[i][j] > LGMAGIC_MATRIX_ELEMENT_MAX ||
			    m[i][j] < -LGMAGIC_MATRIX_ELEMENT_MAX)
				return -EINVAL;
		}
	}

	if (lgmagic_mat3_normalize(m, true, r, &scale) == 0 &&
	    scale >= min_scale && scale <= max_scale &&
	    lgmagic_mat3_is_rotation(r))
	{
		*average = scale;
		memcpy(rotation, r, sizeof(r));
		if (axis_scale)
			for (i = 0; i < 3; i++)
				axis_scale[i] = lgmagic_mat3_norm(m, true, i);
		return 0;
	}

	if (lgmagic_mat3_normalize(m, false, r, &scale) == 0 &&
	    scale >= min_scale && scale <= max_scale &&
	    lgmagic_mat3_is_rotation(r))
	{
		*average = scale;
		memcpy(rotation, r, sizeof(r));
		if (axis_scale)
			for (i = 0; i < 3; i++)
				axis_scale[i] = lgmagic_mat3_norm(m, false, i);
		return 0;
	}

	return -EINVAL;
}

static int lgmagic_scd_read_matrix(const u8 *scd, size_t base,
				   size_t first_float, s64 m[3][3])
{
	int i, j;

	for (i = 0; i < 3; i++)
	{
		for (j = 0; j < 3; j++)
		{
			size_t at = base + (first_float + 3 * i + j) * 4;

			if (lgmagic_float_to_q16(lgmagic_get_le32(scd + at),
						 &m[i][j]))
				return -EINVAL;
		}
	}

	return 0;
}

int lgmagic_parse_scd_matrices(const u8 *scd, size_t size,
			       s64 gyro_matrix[3][3],
			       s64 accel_matrix[3][3],
			       s64 gyro_remap[3][3],
			       s64 gyro_scale[3],
			       s64 *accel_scale,
			       s64 *sample_dt)
{
	static const u8 magic[5] = { 0x00, 0x04, 0x01, 0x05, 0x00 };
	const size_t base = LGMAGIC_SCD_FLOAT_BASE;
	s64 gm[3][3], am[3][3];
	s64 gm_remap[3][3], am_remap[3][3];
	s64 gavg, aavg, per_count, dt;
	s64 sample;

	if (size != LGMAGIC_SCD_SIZE)
		return -EINVAL;
	if (memcmp(scd, magic, sizeof(magic)))
		return -EINVAL;

	if (lgmagic_scd_read_matrix(scd, base, LGMAGIC_SCD_GYRO_FLOAT, gm) ||
	    lgmagic_scd_read_matrix(scd, base, LGMAGIC_SCD_ACCEL_FLOAT, am))
		return -EINVAL;

	/* ~0.0001 .. 0.1 rad/s per count. */
	if (lgmagic_mat3_validate(gm, 6, 6554, &gavg, gm_remap, gyro_scale))
		return -EINVAL;

	/* Full scale in m/s^2; the reference table encodes 8 g. */
	if (lgmagic_mat3_validate(am, LGMAGIC_FP_ONE, 100 * LGMAGIC_FP_ONE,
				  &aavg, am_remap, NULL))
		return -EINVAL;

	per_count = (aavg + (8 * LGMAGIC_ACCEL_COUNTS_PER_G) / 2) /
		    (8 * LGMAGIC_ACCEL_COUNTS_PER_G);
	if (per_count < 100 || per_count > 65536)
		return -EINVAL;

	/* Float 0 is the report period in seconds (0.01 on the MR19/MR20). */
	dt = LGMAGIC_DEFAULT_DT;
	if (!lgmagic_float_to_q16(lgmagic_get_le32(scd + base), &sample) &&
	    sample >= 65 && sample <= 6554)
		dt = sample;

	memcpy(gyro_matrix, gm, sizeof(gm));
	memcpy(accel_matrix, am, sizeof(am));
	memcpy(gyro_remap, gm_remap, sizeof(gm_remap));
	*accel_scale = per_count;
	*sample_dt = dt;
	return 0;
}

static void lgmagic_mat3_vec(const s64 m[3][3], const s64 v[3], s64 out[3])
{
	int i, j;

	for (i = 0; i < 3; i++)
	{
		s64 sum = 0;

		for (j = 0; j < 3; j++)
			sum += lgmagic_fpmul(m[i][j], v[j]);

		out[i] = sum;
	}
}

/* v -= dt * (omega x v): rotate a world-fixed vector by the body rotation. */
static void lgmagic_rotate_back(s64 v[3], const s64 omega[3], s64 dt)
{
	s64 c[3];
	int i;

	lgmagic_fpcross(omega, v, c);
	for (i = 0; i < 3; i++)
		v[i] -= lgmagic_fpmul(dt, c[i]);
}

/*
 * Gravity estimation.
 *
 * The first sample whose magnitude is close to 1 g seeds the estimate
 * immediately, so shaking the remote enables the pointer without first
 * having to hold it still.  After that the gyro carries the up vector and
 * the accelerometer corrects it:
 *
 *   - while still, the accelerometer *is* the direction of gravity, so the
 *     estimate is replaced by it; this also repairs a bad startup seed
 *     without reconnecting the remote;
 *   - while moving, the gyro propagates the estimate and the accelerometer
 *     corrects it.
 *
 * A fresh estimate has not been checked against a still accelerometer yet, so
 * it learns fast: a startup sample taken during hand acceleration is corrected
 * within a fraction of a second of normal motion, without waiting for a still
 * period.  The first still sample confirms the estimate and switches to the
 * slower, steadier gain, where hand acceleration no longer tilts the basis.
 *
 * The accelerometer is ignored while its magnitude is far from 1 g.
 */
static int lgmagic_update_gravity(struct lg_magic_airmouse_state *state,
				  const s16 *accel, const s64 *omega,
				  const s64 *unit)
{
	s64 g[3], gain, dev, speed2;
	u64 n2, n;
	bool a_ok, still;
	int i;

	n2 = (u64)((s64)accel[0] * accel[0]) +
	     (u64)((s64)accel[1] * accel[1]) +
	     (u64)((s64)accel[2] * accel[2]);
	n = lgmagic_isqrt64(n2);
	dev = (s64)n - LGMAGIC_ACCEL_COUNTS_PER_G;
	if (dev < 0)
		dev = -dev;
	a_ok = dev <= state->accel_gate;

	speed2 = lgmagic_fpdot(omega, omega);
	still = speed2 < lgmagic_fpmul(state->zero_rate, state->zero_rate);

	if (!state->gravity_valid)
	{
		/* Do not seed from a sample taken under linear acceleration. */
		if (!a_ok)
			return -EAGAIN;

		for (i = 0; i < 3; i++)
			state->gravity[i] = unit[i];
		state->gravity_valid = 1;
		state->gravity_settled = still;
		return 0;
	}

	for (i = 0; i < 3; i++)
		g[i] = state->gravity[i];

	lgmagic_rotate_back(g, omega, state->dt);
	if (lgmagic_vec_normalize(g))
		return -EINVAL;

	if (a_ok)
	{
		if (still)
		{
			for (i = 0; i < 3; i++)
				g[i] = unit[i];
			state->gravity_settled = 1;
		}
		else
		{
			s64 rate = state->gravity_settled ?
				   state->gravity_gain :
				   state->gravity_gain_fast;

			gain = lgmagic_fpmul(rate, state->dt);
			for (i = 0; i < 3; i++)
				g[i] += lgmagic_fpmul(gain, unit[i] - g[i]);
		}

		if (lgmagic_vec_normalize(g))
			return -EINVAL;
	}

	for (i = 0; i < 3; i++)
		state->gravity[i] = g[i];

	return 0;
}

void lgmagic_airmouse_init(struct lg_magic_airmouse_state *state)
{
	memset(state, 0, sizeof(*state));

	/* Algorithm constants only; calibration and UX fields start at zero. */
	state->dt = LGMAGIC_DEFAULT_DT;
	state->gravity_gain = LGMAGIC_FP_ONE;
	state->gravity_gain_fast = 10 * LGMAGIC_FP_ONE;
	state->accel_gate = 4;		/* ~0.1 g, in accelerometer counts */
	state->pointing[1] = LGMAGIC_FP_ONE;
	/* Identity remap until the SCD supplies one (blob/raw devices). */
	state->fallback_right[2] = LGMAGIC_FP_ONE;
	state->fallback_vertical[0] = LGMAGIC_FP_ONE;
	state->deadzone = 1311;		/* 0.02 rad/s */
	state->bias_beta = 1311;	/* 0.02 per report */
	state->zero_rate = 3277;	/* 0.05 rad/s */
	state->auto_bias = 0;
	state->gravity_valid = 0;
}

/* Rotate the fixed pointing axis from the gyro sensor frame. */
void lgmagic_airmouse_set_remap(struct lg_magic_airmouse_state *state,
				const s64 remap[3][3])
{
	s64 p[3] = { 0, LGMAGIC_FP_ONE, 0 };
	s64 raw_x[3] = { LGMAGIC_FP_ONE, 0, 0 };
	s64 raw_z[3] = { 0, 0, LGMAGIC_FP_ONE };

	lgmagic_mat3_vec(remap, p, state->pointing);
	if (lgmagic_vec_normalize(state->pointing))
	{
		state->pointing[0] = 0;
		state->pointing[1] = LGMAGIC_FP_ONE;
		state->pointing[2] = 0;
	}

	lgmagic_mat3_vec(remap, raw_z, state->fallback_right);
	if (lgmagic_vec_normalize(state->fallback_right))
	{
		state->fallback_right[0] = 0;
		state->fallback_right[1] = 0;
		state->fallback_right[2] = LGMAGIC_FP_ONE;
	}

	lgmagic_mat3_vec(remap, raw_x, state->fallback_vertical);
	if (lgmagic_vec_normalize(state->fallback_vertical))
	{
		state->fallback_vertical[0] = LGMAGIC_FP_ONE;
		state->fallback_vertical[1] = 0;
		state->fallback_vertical[2] = 0;
	}
}


/*
 * Approximate cursor motion before gravity has been acquired.  This is the
 * pre-SCD fixed-axis mapping (raw z -> X, raw x -> Y) expressed in the device
 * frame: the two directions are the SCD remap images of raw +z and raw +x, so
 * the fallback shares the frame of the gravity path.  It is not used once
 * gravity exists.
 */
static void lgmagic_fallback_mouse(const struct lg_magic_airmouse_state *state,
				   const s64 *omega, s16 *mouse)
{
	s64 dxr, dyr, dx, dy;

	dxr = lgmagic_fpdot(omega, state->fallback_right);
	dyr = lgmagic_fpdot(omega, state->fallback_vertical);

	if (lgmagic_fabs(dxr) < state->deadzone)
		dxr = 0;
	if (lgmagic_fabs(dyr) < state->deadzone)
		dyr = 0;

	dx = lgmagic_fpmul(lgmagic_fpmul(state->sensitivity_x, dxr),
			   state->dt);
	dy = lgmagic_fpmul(lgmagic_fpmul(state->sensitivity_y, dyr),
			   state->dt);

	mouse[0] = lgmagic_clamp_s16(dx / LGMAGIC_FP_ONE);
	mouse[1] = lgmagic_clamp_s16(dy / LGMAGIC_FP_ONE);
}

int lgmagic_calc_mouse_gravity(struct lg_magic_airmouse_state *state,
			       const s16 *gyro, const s16 *accel,
			       s16 *mouse)
{
	s64 corr[3], omega[3], unit[3];
	s64 right[3], vertical[3], motion[3];
	s64 dx = 0, dy = 0, dxr, dyr, speed2, limit2;
	int i;

	mouse[0] = 0;
	mouse[1] = 0;

	if (!state->gyro_matrix_valid)
		return 0;

	for (i = 0; i < 3; i++)
		corr[i] = (s64)gyro[i] * LGMAGIC_FP_ONE - state->gyro_bias[i];

	lgmagic_mat3_vec(state->gyro_matrix, corr, omega);

	speed2 = lgmagic_fpdot(omega, omega);

	/* Auto-bias is only for SCD-only; a blob keeps its measured bias. */
	if (state->auto_bias &&
	    speed2 < lgmagic_fpmul(state->zero_rate, state->zero_rate))
	{
		for (i = 0; i < 3; i++)
		{
			s64 raw = (s64)gyro[i] * LGMAGIC_FP_ONE;

			state->gyro_bias[i] +=
				lgmagic_fpmul(state->bias_beta,
					      raw - state->gyro_bias[i]);
		}
	}

	if (state->accel_matrix_valid)
	{
		s64 raw[3];

		for (i = 0; i < 3; i++)
			raw[i] = (s64)accel[i] * LGMAGIC_FP_ONE;

		lgmagic_mat3_vec(state->accel_matrix, raw, unit);
		if (lgmagic_vec_normalize(unit))
			return 0;
	}
	else if (state->accel_scale)
	{
		/* accel_scale is parsed but intentionally unused. */
		for (i = 0; i < 3; i++)
			unit[i] = (s64)accel[i] * state->accel_scale;

		if (lgmagic_vec_normalize(unit))
			return 0;
	}
	else if (lgmagic_accel_unit(accel, unit))
	{
		return 0;
	}

	if (lgmagic_update_gravity(state, accel, omega, unit))
	{
		/*
		 * Gravity is not usable yet.  Move approximately on the raw
		 * axes so the shake visibly enters pointer mode; the exact
		 * gravity basis takes over as soon as a usable sample
		 * arrives.
		 */
		lgmagic_fallback_mouse(state, omega, mouse);
		limit2 = lgmagic_fpmul(state->threshold, state->threshold);
		return speed2 > limit2;
	}

	/*
	 * Instantaneous screen basis.  Gravity supplies the vertical; the
	 * fixed pointing axis supplies the screen normal.  There is no stored
	 * screen-forward or initial yaw: every report is independent of how
	 * the remote was held at probe time.
	 */
	lgmagic_fpcross(state->gravity, state->pointing, right);
	if (lgmagic_vec_normalize(right))
		return 0;
	lgmagic_fpcross(state->pointing, right, vertical);
	if (lgmagic_vec_normalize(vertical))
		return 0;

	/* Motion of the pointing vector; a roll around it cancels. */
	lgmagic_fpcross(omega, state->pointing, motion);

	dxr = lgmagic_fpdot(motion, right);
	dyr = lgmagic_fpdot(motion, vertical);

	if (lgmagic_fabs(dxr) < state->deadzone)
		dxr = 0;
	if (lgmagic_fabs(dyr) < state->deadzone)
		dyr = 0;

	dx = -lgmagic_fpmul(lgmagic_fpmul(state->sensitivity_x, dxr),
			    state->dt);
	dy = -lgmagic_fpmul(lgmagic_fpmul(state->sensitivity_y, dyr),
			    state->dt);

	mouse[0] = lgmagic_clamp_s16(dx / LGMAGIC_FP_ONE);
	mouse[1] = lgmagic_clamp_s16(dy / LGMAGIC_FP_ONE);

	limit2 = lgmagic_fpmul(state->threshold, state->threshold);

	return speed2 > limit2;
}

/*
 * Fixed-axis reference path.  This is the original working mapping
 * dx = +gyro_z, dy = +gyro_x; the gravity path is checked against it at a
 * level grip.
 */
int lgmagic_calc_mouse(struct lg_magic_airmouse_state *state,
		       const s16 *gyro, s16 *mouse)
{
	s64 rate[3], speed2, dx, dy;
	int i;

	mouse[0] = 0;
	mouse[1] = 0;

	if (!state->gyro_valid)
		return 0;

	for (i = 0; i < 3; i++)
	{
		s64 corr = (s64)gyro[i] * LGMAGIC_FP_ONE -
			   state->gyro_bias[i];

		rate[i] = lgmagic_fpmul(corr, state->gyro_scale[i]);
	}

	speed2 = (rate[0] * rate[0] + rate[1] * rate[1] +
		  rate[2] * rate[2]) / LGMAGIC_FP_ONE;

	if (state->auto_bias &&
	    speed2 < lgmagic_fpmul(state->zero_rate, state->zero_rate))
	{
		for (i = 0; i < 3; i++)
		{
			s64 raw = (s64)gyro[i] * LGMAGIC_FP_ONE;

			state->gyro_bias[i] +=
				lgmagic_fpmul(state->bias_beta,
					      raw - state->gyro_bias[i]);
		}
	}

	dx = lgmagic_fpmul(lgmagic_fpmul(state->sensitivity_x, rate[2]),
			   state->dt);
	dy = lgmagic_fpmul(lgmagic_fpmul(state->sensitivity_y, rate[0]),
			   state->dt);

	mouse[0] = lgmagic_clamp_s16(dx / LGMAGIC_FP_ONE);
	mouse[1] = lgmagic_clamp_s16(dy / LGMAGIC_FP_ONE);

	return lgmagic_fabs(rate[0]) > state->threshold ||
	       lgmagic_fabs(rate[2]) > state->threshold;
}
