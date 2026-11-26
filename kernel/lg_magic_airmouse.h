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
 * gyro scales, the gyro remap and the sample period from float 0.
 */
int lgmagic_parse_scd_matrices(const u8 *scd, size_t size,
			       s64 gyro_matrix[3][3],
			       s64 accel_matrix[3][3],
			       s64 gyro_remap[3][3],
			       s64 gyro_scale[3],
			       s64 *accel_scale,
			       s64 *sample_dt);

/*
 * Gravity-projected pointer state, all Q16.16.  The cursor uses yaw =
 * omega . gravity and pitch = omega . normalize(pointing x gravity), so a
 * roll about the pointing axis does not move it.
 */
struct lg_magic_airmouse_state
{
	s64 gyro_bias[3];	/* counts */
	s64 gyro_matrix[3][3];	/* rad/s per count, includes remap */
	s64 accel_matrix[3][3];	/* m/s^2 per count, includes remap */
	s64 gyro_scale[3];	/* per-raw-axis scale, rad/s per count */
	s64 accel_scale;	/* unused summary, m/s^2 per count */
	s64 sensitivity_x;	/* pixels per radian */
	s64 sensitivity_y;	/* pixels per radian */
	s64 dt;			/* seconds per report */
	s64 gravity[3];		/* unit vector, accelerometer up */
	s64 gravity_alpha;	/* per-report low-pass weight */
	s64 pointing[3];	/* unit vector, pointing axis */
	s64 deadzone;		/* rad/s */
	s64 threshold;		/* rad/s, shake-to-enable */
	s64 bias_beta;		/* Q16.16 auto-bias update weight */
	s64 zero_rate;		/* rad/s, still threshold */
	u8 auto_bias;
	u8 gravity_valid;
	u8 gyro_matrix_valid;
	u8 accel_matrix_valid;
};

void lgmagic_airmouse_init(struct lg_magic_airmouse_state *state);
void lgmagic_airmouse_set_remap(struct lg_magic_airmouse_state *state,
				const s64 remap[3][3]);
int lgmagic_project(const s64 *omega, const s64 *gravity,
		    const s64 *pointing, s64 *yaw, s64 *pitch);
int lgmagic_calc_mouse_gravity(struct lg_magic_airmouse_state *state,
			       const s16 *gyro, const s16 *accel,
			       s16 *mouse);

#endif
