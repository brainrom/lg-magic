#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lg_magic_airmouse.h"

static int failures;

#define CHECK(cond)                                                          \
	do {                                                                 \
		if (!(cond)) {                                               \
			fprintf(stderr, "FAIL %s:%d: %s\n",                   \
				__FILE__, __LINE__, #cond);                  \
			failures++;                                          \
		}                                                            \
	} while (0)

static s64 q16(double value)
{
	return (s64)llround(value * LGMAGIC_FP_ONE);
}

static void mat_identity(s64 m[3][3])
{
	memset(m, 0, sizeof(s64) * 9);
	m[0][0] = LGMAGIC_FP_ONE;
	m[1][1] = LGMAGIC_FP_ONE;
	m[2][2] = LGMAGIC_FP_ONE;
}

static void mat_rz90(s64 m[3][3])
{
	memset(m, 0, sizeof(s64) * 9);
	m[0][1] = -LGMAGIC_FP_ONE;
	m[1][0] = LGMAGIC_FP_ONE;
	m[2][2] = LGMAGIC_FP_ONE;
}

static struct lg_magic_airmouse_state make_state(void)
{
	struct lg_magic_airmouse_state state;

	lgmagic_airmouse_init(&state);
	mat_identity(state.gyro_matrix);
	state.gyro_matrix_valid = 1;
	state.sensitivity_x = LGMAGIC_FP_ONE;
	state.sensitivity_y = LGMAGIC_FP_ONE;
	state.dt = LGMAGIC_FP_ONE;
	state.deadzone = 0;
	state.gravity_valid = 1;
	state.gravity[2] = -LGMAGIC_FP_ONE;

	return state;
}

static void put_le32(u8 *p, u32 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
	p[2] = (u8)(v >> 16);
	p[3] = (u8)(v >> 24);
}

static void put_float(u8 *p, float value)
{
	u32 bits;

	memcpy(&bits, &value, sizeof(bits));
	put_le32(p, bits);
}

static void write_scd_matrix(u8 *scd, size_t first_float,
			     const double m[3][3])
{
	size_t base = 9;
	int i, j;

	for (i = 0; i < 3; i++)
	{
		for (j = 0; j < 3; j++)
		{
			size_t at = base + (first_float + 3 * i + j) * 4;

			put_float(scd + at, (float)m[i][j]);
		}
	}
}

static void test_accel_gravity_direction(void)
{
	struct lg_magic_airmouse_state state = make_state();
	s16 gyro[3] = { 0, 0, 0 };
	s16 accel[3] = { 0, 0, -41 };
	s16 mouse[2];

	/* Raw path used by the blob-only calibration. */
	state.gravity_valid = 0;
	lgmagic_calc_mouse_gravity(&state, gyro, accel, mouse);
	CHECK(llabs(state.gravity[0]) < 8);
	CHECK(llabs(state.gravity[1]) < 8);
	CHECK(llabs(state.gravity[2] + LGMAGIC_FP_ONE) < 8);

	/* SCD remap path. */
	state = make_state();
	mat_rz90(state.accel_matrix);
	state.accel_matrix_valid = 1;
	state.gravity_valid = 0;
	accel[0] = 41;
	accel[2] = 0;
	lgmagic_calc_mouse_gravity(&state, gyro, accel, mouse);
	CHECK(llabs(state.gravity[0]) < 8);
	CHECK(llabs(state.gravity[1] - LGMAGIC_FP_ONE) < 8);
	CHECK(llabs(state.gravity[2]) < 8);
}

static void test_gravity_projection(void)
{
	static const double angles[] = { 0.0, 45.0, 90.0, 135.0, -90.0 };
	const double pi = 3.14159265358979323846;
	s64 pointing[3] = { 0, LGMAGIC_FP_ONE, 0 };
	s64 gravity0[3] = { 0, 0, -LGMAGIC_FP_ONE };
	s64 omega0[3] = { 0, LGMAGIC_FP_ONE, 0 };
	s64 yaw, pitch;

	/* A roll about the pointing axis must not move the cursor. */
	CHECK(lgmagic_project(omega0, gravity0, pointing, &yaw, &pitch) == 0);
	CHECK(llabs(yaw) < 8);
	CHECK(llabs(pitch) < 8);

	/* A physical yaw about the world vertical is roll invariant. */
	for (unsigned int i = 0; i < sizeof(angles) / sizeof(angles[0]); i++)
	{
		double a = angles[i] * pi / 180.0;
		s64 gravity[3] = { q16(-sin(a)), 0, q16(-cos(a)) };
		s64 omega[3] = { gravity[0], gravity[1], gravity[2] };

		CHECK(lgmagic_project(omega, gravity, pointing,
				      &yaw, &pitch) == 0);
		CHECK(llabs(yaw - LGMAGIC_FP_ONE) < 16);
		CHECK(llabs(pitch) < 16);
	}
}

static void test_cursor_sign_matches_kernel_convention(void)
{
	struct lg_magic_airmouse_state state = make_state();
	s16 gyro[3] = { 2, 0, 3 };
	s16 accel[3] = { 0, 0, -41 };
	s16 mouse[2];

	state.gravity_alpha = 0;

	lgmagic_calc_mouse_gravity(&state, gyro, accel, mouse);
	CHECK(mouse[0] == 3);	/* +omega_z */
	CHECK(mouse[1] == 2);	/* +omega_x */
}

static void test_remap_rotates_pointing_and_keeps_projection(void)
{
	struct lg_magic_airmouse_state state = make_state();
	s64 remap[3][3];
	s16 gyro[3] = { 2, 0, 3 };
	s16 accel[3] = { 0, 0, -41 };
	s16 mouse[2];

	mat_rz90(state.gyro_matrix);
	mat_rz90(state.accel_matrix);
	state.accel_matrix_valid = 1;
	mat_rz90(remap);
	lgmagic_airmouse_set_remap(&state, remap);

	CHECK(llabs(state.pointing[0] + LGMAGIC_FP_ONE) < 8);
	CHECK(llabs(state.pointing[1]) < 8);
	CHECK(llabs(state.pointing[2]) < 8);

	state.gravity_alpha = 0;
	lgmagic_calc_mouse_gravity(&state, gyro, accel, mouse);
	CHECK(mouse[0] == 3);
	CHECK(mouse[1] == 2);
}

static void test_auto_bias_converges(void)
{
	struct lg_magic_airmouse_state state = make_state();
	s16 gyro[3] = { 100, -100, 50 };
	s16 accel[3] = { 0, 0, -41 };
	s16 mouse[2];
	int i;

	state.auto_bias = 1;
	state.bias_beta = q16(0.5);
	state.zero_rate = q16(1000.0);
	state.gravity_alpha = 0;

	for (i = 0; i < 40; i++)
		lgmagic_calc_mouse_gravity(&state, gyro, accel, mouse);

	CHECK(llabs(state.gyro_bias[0] - q16(100.0)) < 2);
	CHECK(llabs(state.gyro_bias[1] - q16(-100.0)) < 2);
	CHECK(llabs(state.gyro_bias[2] - q16(50.0)) < 2);
}

static void test_scd_matrix_validation(void)
{
	u8 scd[LGMAGIC_SCD_SIZE];
	s64 gyro[3][3], accel[3][3], remap[3][3], gyro_scale[3];
	s64 accel_scale, sample_dt;
	double gm[3][3] = {
		{ 0.001, 0.0, 0.0 },
		{ 0.0, 0.001, 0.0 },
		{ 0.0, 0.0, 0.001 },
	};
	double am[3][3] = {
		{ 78.4, 0.0, 0.0 },
		{ 0.0, 78.4, 0.0 },
		{ 0.0, 0.0, 78.4 },
	};

	memset(scd, 0, sizeof(scd));
	memcpy(scd, "\x00\x04\x01\x05\x00", 5);
	put_float(scd + 9, 0.01f);
	write_scd_matrix(scd, 24, gm);
	write_scd_matrix(scd, 2, am);

	CHECK(lgmagic_parse_scd_matrices(scd, sizeof(scd), gyro, accel,
					 remap, gyro_scale, &accel_scale,
					 &sample_dt) == 0);
	CHECK(sample_dt == q16(0.01));
	CHECK(llabs(gyro_scale[0] - q16(0.001)) <= 1);
	CHECK(llabs(gyro_scale[1] - q16(0.001)) <= 1);
	CHECK(llabs(gyro_scale[2] - q16(0.001)) <= 1);

	/* A missing/garbage sample period falls back to the 10 ms default. */
	put_float(scd + 9, 0.0f);
	CHECK(lgmagic_parse_scd_matrices(scd, sizeof(scd), gyro, accel,
					 remap, gyro_scale, &accel_scale,
					 &sample_dt) == 0);
	CHECK(sample_dt == LGMAGIC_DEFAULT_DT);
	put_float(scd + 9, 0.01f);

	/* Per-axis scales are preserved in the raw-axis gyro scales. */
	gm[0][0] = 0.0005;
	gm[1][1] = 0.001;
	gm[2][2] = 0.0015;
	write_scd_matrix(scd, 24, gm);
	CHECK(lgmagic_parse_scd_matrices(scd, sizeof(scd), gyro, accel,
					 remap, gyro_scale, &accel_scale,
					 &sample_dt) == 0);
	CHECK(llabs(gyro[0][0] - q16(0.0005)) <= 1);
	CHECK(llabs(gyro[1][1] - q16(0.001)) <= 1);
	CHECK(llabs(gyro[2][2] - q16(0.0015)) <= 1);
	CHECK(llabs(gyro_scale[0] - q16(0.0005)) <= 1);
	CHECK(llabs(gyro_scale[1] - q16(0.001)) <= 1);
	CHECK(llabs(gyro_scale[2] - q16(0.0015)) <= 1);

	/* Short payloads and wrong headers are rejected. */
	CHECK(lgmagic_parse_scd_matrices(scd, LGMAGIC_SCD_SIZE - 1, gyro,
					 accel, remap, gyro_scale,
					 &accel_scale, &sample_dt) == -EINVAL);
	CHECK(lgmagic_parse_scd_matrices(scd, 140, gyro, accel, remap,
					 gyro_scale, &accel_scale,
					 &sample_dt) == -EINVAL);
	scd[0] = 0xff;
	CHECK(lgmagic_parse_scd_matrices(scd, sizeof(scd), gyro, accel,
					 remap, gyro_scale, &accel_scale,
					 &sample_dt) == -EINVAL);
	scd[0] = 0x00;

	/* A reflection is an inversion, not a remap. */
	gm[0][0] = -0.001;
	gm[1][1] = 0.001;
	gm[2][2] = 0.001;
	write_scd_matrix(scd, 24, gm);
	CHECK(lgmagic_parse_scd_matrices(scd, sizeof(scd), gyro, accel,
					 remap, gyro_scale, &accel_scale,
					 &sample_dt) == -EINVAL);

	/* A singular matrix is rejected. */
	gm[0][0] = 0.0;
	gm[1][1] = 0.0;
	gm[2][2] = 0.0;
	write_scd_matrix(scd, 24, gm);
	CHECK(lgmagic_parse_scd_matrices(scd, sizeof(scd), gyro, accel,
					 remap, gyro_scale, &accel_scale,
					 &sample_dt) == -EINVAL);
}

static void test_scd_independent_remaps(void)
{
	u8 scd[LGMAGIC_SCD_SIZE];
	s64 gyro[3][3], accel[3][3], remap[3][3], gyro_scale[3];
	s64 accel_scale, sample_dt;
	double gm[3][3] = {
		{ 0.0, -0.001, 0.0 },
		{ 0.001, 0.0, 0.0 },
		{ 0.0, 0.0, 0.001 },
	};
	double am[3][3] = {
		{ 78.4, 0.0, 0.0 },
		{ 0.0, 78.4, 0.0 },
		{ 0.0, 0.0, 78.4 },
	};

	memset(scd, 0, sizeof(scd));
	memcpy(scd, "\x00\x04\x01\x05\x00", 5);
	put_float(scd + 9, 0.01f);
	write_scd_matrix(scd, 24, gm);
	write_scd_matrix(scd, 2, am);

	CHECK(lgmagic_parse_scd_matrices(scd, sizeof(scd), gyro, accel,
					 remap, gyro_scale, &accel_scale,
					 &sample_dt) == 0);

	/* The remap rotates the axes, but the raw-axis scales are unchanged. */
	CHECK(llabs(remap[0][1] + LGMAGIC_FP_ONE) < 8);
	CHECK(llabs(remap[1][0] - LGMAGIC_FP_ONE) < 8);
	CHECK(llabs(remap[2][2] - LGMAGIC_FP_ONE) < 8);
	CHECK(llabs(gyro_scale[0] - q16(0.001)) <= 1);
	CHECK(llabs(gyro_scale[1] - q16(0.001)) <= 1);
	CHECK(llabs(gyro_scale[2] - q16(0.001)) <= 1);
}

static void test_scd_real_payload(void)
{
	static const u8 scd[260] = {
		0x00, 0x04, 0x01, 0x05, 0x00, 0xfb, 0xfa, 0xe1, 0xe7, 0x0a, 0xd7, 0x23,
		0x3c, 0x0a, 0xd7, 0xa3, 0x3c, 0xcd, 0xcc, 0x9c, 0x28, 0xcd, 0xcc, 0x9c,
		0xc2, 0x00, 0x00, 0x00, 0x00, 0xcd, 0xcc, 0x9c, 0x42, 0xcd, 0xcc, 0x9c,
		0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0xcd, 0xcc, 0x9c, 0x42, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
		0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
		0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
		0x80, 0x00, 0x00, 0x00, 0x80, 0xe0, 0x2d, 0x10, 0x41, 0xe0, 0x2d, 0x10,
		0x41, 0xe0, 0x2d, 0x10, 0x41, 0x00, 0x00, 0x80, 0xbf, 0x6f, 0xa1, 0x8b,
		0x20, 0x6f, 0xa1, 0x8b, 0xba, 0x00, 0x00, 0x00, 0x00, 0x6f, 0xa1, 0x8b,
		0x3a, 0x6f, 0xa1, 0x8b, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x6f, 0xa1, 0x8b, 0x3a, 0x00, 0x00, 0x00,
		0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6f, 0xa1, 0x8b,
		0x3a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6f, 0xa1, 0x8b,
		0x3a, 0x00, 0x00, 0x80, 0x3f, 0x6f, 0xa1, 0x8b, 0x3a, 0x0a, 0x1c, 0x01,
		0x40, 0xc2, 0xb8, 0xb2, 0x3e, 0x5d, 0xc4, 0x27, 0x40, 0x5d, 0xc4, 0x27,
		0x40, 0x5d, 0xc4, 0x27, 0x40, 0x00, 0x48, 0x46, 0xc1, 0x64, 0x43, 0xc4,
		0x3d, 0xa9, 0x58, 0xd1, 0x39, 0xe4, 0xba, 0xac,
	};
	s64 gyro[3][3], accel[3][3], remap[3][3], gyro_scale[3];
	s64 accel_scale, sample_dt;
	const double gs = 0.0010652969358488917;
	const double as = 78.4000015258789;
	int i;

	CHECK(lgmagic_parse_scd_matrices(scd, sizeof(scd), gyro, accel,
					 remap, gyro_scale, &accel_scale,
					 &sample_dt) == 0);
	CHECK(sample_dt == q16(0.01));
	CHECK(llabs(gyro[0][1] + q16(gs)) <= 1);
	CHECK(llabs(gyro[1][0] - q16(gs)) <= 1);
	CHECK(llabs(gyro[2][2] - q16(gs)) <= 1);
	CHECK(llabs(accel[0][1] + q16(as)) <= 2);
	CHECK(llabs(accel[1][0] - q16(as)) <= 2);
	CHECK(llabs(accel[2][2] - q16(as)) <= 2);

	/* The column norms are the per-raw-axis gyro scales. */
	for (i = 0; i < 3; i++)
		CHECK(llabs(gyro_scale[i] - q16(gs)) <= 1);
}

static struct lgmagic_calib_blob make_blob(void)
{
	struct lgmagic_calib_blob blob;

	memset(&blob, 0, sizeof(blob));
	blob.magic = LGMAGIC_CALIB_MAGIC;
	blob.version = LGMAGIC_CALIB_VERSION;
	blob.size = sizeof(blob);
	blob.gyro_bias[0] = (u32)(s32)q16(1.5);
	blob.gyro_bias[1] = (u32)(s32)q16(-2.25);
	blob.gyro_bias[2] = (u32)(s32)q16(3.0);
	blob.gyro_scale[0] = (u32)(s32)q16(0.07);
	blob.gyro_scale[1] = (u32)(s32)q16(0.08);
	blob.gyro_scale[2] = (u32)(s32)q16(0.09);
	blob.alpha = (u32)(s32)q16(0.2);
	blob.mouse_k = (u32)(s32)q16(0.5);

	return blob;
}

static void test_phys_calib_parse(void)
{
	const double pi = 3.14159265358979323846;
	struct lgmagic_calib_blob blob = make_blob();
	s64 bias[3], scale[3];

	CHECK(lgmagic_parse_calib_phys(bias, scale, (const u8 *)&blob,
				       sizeof(blob)) == 0);
	CHECK(bias[0] == q16(1.5));
	CHECK(bias[1] == q16(-2.25));
	CHECK(bias[2] == q16(3.0));
	CHECK(llabs(scale[0] - q16(0.07 * pi / 180.0)) <= 1);
	CHECK(llabs(scale[1] - q16(0.08 * pi / 180.0)) <= 1);
	CHECK(llabs(scale[2] - q16(0.09 * pi / 180.0)) <= 1);

	/* zero/negative scale marks the blob as broken */
	blob = make_blob();
	blob.gyro_scale[0] = 0;
	CHECK(lgmagic_parse_calib_phys(bias, scale, (const u8 *)&blob,
				       sizeof(blob)) == -EINVAL);

	blob = make_blob();
	blob.gyro_scale[0] = (u32)(s32)q16(-0.07);
	CHECK(lgmagic_parse_calib_phys(bias, scale, (const u8 *)&blob,
				       sizeof(blob)) == -EINVAL);

	/* bad header and truncated payload */
	blob = make_blob();
	blob.magic = 0;
	CHECK(lgmagic_parse_calib_phys(bias, scale, (const u8 *)&blob,
				       sizeof(blob)) == -EINVAL);
	blob = make_blob();
	CHECK(lgmagic_parse_calib_phys(bias, scale, (const u8 *)&blob,
				       sizeof(blob) - 1) == -EINVAL);
}

int main(void)
{
	test_accel_gravity_direction();
	test_gravity_projection();
	test_cursor_sign_matches_kernel_convention();
	test_remap_rotates_pointing_and_keeps_projection();
	test_auto_bias_converges();
	test_scd_matrix_validation();
	test_scd_independent_remaps();
	test_scd_real_payload();
	test_phys_calib_parse();

	if (failures) {
		fprintf(stderr, "%d test(s) failed\n", failures);
		return 1;
	}

	printf("all airmouse math tests passed\n");
	return 0;
}
