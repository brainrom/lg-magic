#ifndef LG_MAGIC_AIRMOUSE_H
#define LG_MAGIC_AIRMOUSE_H

#include <linux/types.h>

#define LGMAGIC_FP_SHIFT	16
#define LGMAGIC_FP_ONE		(1LL << LGMAGIC_FP_SHIFT)

/* Fixed size of the F9 17 sensor calibration table. */
#define LGMAGIC_SCD_SIZE	260

/* Sample period used when the SCD does not provide one: 10 ms. */
#define LGMAGIC_DEFAULT_DT	656

/* On-disk calibration blob, little-endian Q16.16.  Keep in sync with
 * scripts/convert_calib.py.
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

/*
 * Read gyro_bias and gyro_scale from a v1 blob; the on-disk scale is
 * deg/s per count.  alpha/mouse_k are ignored.
 */
int lgmagic_parse_calib_phys(s64 gyro_bias[3], s64 gyro_scale[3],
			     const u8 *blob, size_t size);

s64 lgmagic_degps_to_radps(s64 degps);

s64 lgmagic_radps_to_degps(s64 radps);

/*
 * Parse the SCD matrices (rad/s and m/s^2 per count), the per-raw-axis
 * gyro scales (used by the fixed-axis reference), the gyro remap, the
 * per-axis accelerometer scale in counts per g and the sample period.
 * The accelerometer scale is zero when the SCD tail is not recognized.
 */
int lgmagic_parse_scd_matrices(const u8 *scd, size_t size,
			       s64 gyro_matrix[3][3],
			       s64 accel_matrix[3][3],
			       s64 gyro_remap[3][3],
			       s64 gyro_scale[3],
			       s64 accel_counts_per_g[3],
			       s64 *sample_dt);

/*
 * Air mouse state, all Q16.16.
 *
 * The gravity path builds an instantaneous basis from the accelerometer up
 * vector and the fixed pointing axis.  It has no stored screen-forward or
 * initial yaw.  The fixed-axis fields are kept as the reference mapping used
 * by the tests: at a level grip the gravity path must reduce to
 * dx = +gyro_z, dy = +gyro_x in raw sensor axes.
 */
struct lg_magic_airmouse_state
{
	s64 gyro_bias[3];	/* counts */
	s64 gyro_matrix[3][3];	/* rad/s per count, includes remap */
	s64 accel_matrix[3][3];	/* m/s^2 per count, includes remap */
	s64 gyro_scale[3];	/* raw-axis scales for the reference path */
	s64 accel_scale[3];	/* raw-axis counts per g, 0 = unknown */
	s64 sensitivity_x;	/* pixels per radian */
	s64 sensitivity_y;	/* pixels per radian */
	s64 dt;			/* seconds per report */
	s64 gravity[3];		/* unit vector, accelerometer up */
	s64 gravity_gain;	/* accelerometer correction, per second */
	s64 gravity_gain_fast;	/* correction until the estimate is settled */
	s64 accel_gate;		/* accepted deviation from 1 g, Q16.16 g */
	s64 pointing[3];	/* unit vector, pointing axis */
	s64 fallback_right[3];	/* device direction of raw +z, pre-gravity X */
	s64 fallback_vertical[3];/* device direction of raw +x, pre-gravity Y */
	s64 deadzone;		/* rad/s */
	s64 threshold;		/* rad/s, shake-to-enable */
	s64 bias_beta;		/* Q16.16 auto-bias update weight */
	s64 zero_rate;		/* rad/s, still threshold */
	u8 auto_bias;
	u8 gravity_valid;
	u8 gravity_settled;
	u8 gyro_matrix_valid;
	u8 accel_matrix_valid;
	u8 gyro_valid;
};

void lgmagic_airmouse_init(struct lg_magic_airmouse_state *state);
void lgmagic_airmouse_set_remap(struct lg_magic_airmouse_state *state,
				const s64 remap[3][3]);
int lgmagic_calc_mouse_gravity(struct lg_magic_airmouse_state *state,
			       const s16 *gyro, const s16 *accel,
			       s16 *mouse);
/* Fixed-axis reference path, used by the tests and diagnostics. */
int lgmagic_calc_mouse(struct lg_magic_airmouse_state *state,
		       const s16 *gyro, s16 *mouse);

#endif
