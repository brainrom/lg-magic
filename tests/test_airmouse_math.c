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

#define GYRO_SCALE	0.0010652969358488917	/* rad/s per count */

static s64 q16(double value)
{
	return (s64)llround(value * LGMAGIC_FP_ONE);
}


static void make_rz90(s64 m[3][3])
{
	m[0][0] = 0;
	m[0][1] = -LGMAGIC_FP_ONE;
	m[0][2] = 0;
	m[1][0] = LGMAGIC_FP_ONE;
	m[1][1] = 0;
	m[1][2] = 0;
	m[2][0] = 0;
	m[2][1] = 0;
	m[2][2] = LGMAGIC_FP_ONE;
}

static void make_rz90_scaled(s64 m[3][3], double scale)
{
	s64 s = q16(scale);

	m[0][0] = 0;
	m[0][1] = -s;
	m[0][2] = 0;
	m[1][0] = s;
	m[1][1] = 0;
	m[1][2] = 0;
	m[2][0] = 0;
	m[2][1] = 0;
	m[2][2] = s;
}

static double gravity_angle_deg(const s64 g[3], const s16 accel[3])
{
	double a[3], b[3], na = 0.0, nb = 0.0, dot = 0.0;
	int i;

	for (i = 0; i < 3; i++)
	{
		a[i] = (double)g[i];
		b[i] = (double)accel[i];
		na += a[i] * a[i];
		nb += b[i] * b[i];
		dot += a[i] * b[i];
	}
	if (na <= 0.0 || nb <= 0.0)
		return 180.0;
	dot /= sqrt(na * nb);
	if (dot > 1.0)
		dot = 1.0;
	if (dot < -1.0)
		dot = -1.0;
	return acos(dot) * 180.0 / M_PI;
}

static struct lg_magic_airmouse_state make_state(void)
{
	struct lg_magic_airmouse_state state;

	lgmagic_airmouse_init(&state);

	state.gyro_scale[0] = q16(GYRO_SCALE);
	state.gyro_scale[1] = q16(GYRO_SCALE);
	state.gyro_scale[2] = q16(GYRO_SCALE);
	state.gyro_valid = 1;
	state.sensitivity_x = 1000LL << LGMAGIC_FP_SHIFT;
	state.sensitivity_y = 1000LL << LGMAGIC_FP_SHIFT;
	state.dt = LGMAGIC_FP_ONE;
	state.threshold = 100 * LGMAGIC_FP_ONE;

	return state;
}

/*
 * The pre-SCD working driver used exactly dx = +gyro_z * scale_z and
 * dy = +gyro_x * scale_x.  These tests pin that mapping down: changing
 * the axis or the sign must fail here, not on the user's desk.
 */
static void test_baseline_axes_and_signs(void)
{
	struct lg_magic_airmouse_state state;
	s16 gyro[3];
	s16 mouse[2];

	/* Only gyro_z: horizontal motion, no vertical coupling. */
	state = make_state();
	gyro[0] = 0;
	gyro[1] = 0;
	gyro[2] = 1000;
	lgmagic_calc_mouse(&state, gyro, mouse);
	CHECK(mouse[0] > 0);
	CHECK(mouse[1] == 0);

	/* Only gyro_x: vertical motion, no horizontal coupling. */
	state = make_state();
	gyro[0] = 1000;
	gyro[1] = 0;
	gyro[2] = 0;
	lgmagic_calc_mouse(&state, gyro, mouse);
	CHECK(mouse[0] == 0);
	CHECK(mouse[1] > 0);

	/* Negative rates move in the opposite direction. */
	state = make_state();
	gyro[0] = -1000;
	gyro[1] = 0;
	gyro[2] = -1000;
	lgmagic_calc_mouse(&state, gyro, mouse);
	CHECK(mouse[0] < 0);
	CHECK(mouse[1] < 0);

	/* gyro_y must not leak into either cursor axis. */
	state = make_state();
	gyro[0] = 0;
	gyro[1] = 1000;
	gyro[2] = 0;
	lgmagic_calc_mouse(&state, gyro, mouse);
	CHECK(mouse[0] == 0);
	CHECK(mouse[1] == 0);
}


static struct lg_magic_airmouse_state make_gravity_state(void)
{
	struct lg_magic_airmouse_state state;

	lgmagic_airmouse_init(&state);
	state.gyro_matrix[0][0] = q16(GYRO_SCALE);
	state.gyro_matrix[1][1] = q16(GYRO_SCALE);
	state.gyro_matrix[2][2] = q16(GYRO_SCALE);
	state.gyro_scale[0] = q16(GYRO_SCALE);
	state.gyro_scale[1] = q16(GYRO_SCALE);
	state.gyro_scale[2] = q16(GYRO_SCALE);
	state.gyro_matrix_valid = 1;
	state.gyro_valid = 1;
	/* Canonical pointing axis for raw +Y under Rz90. */
	state.pointing[0] = -LGMAGIC_FP_ONE;
	state.pointing[1] = 0;
	state.pointing[2] = 0;
	state.sensitivity_x = 1000LL << LGMAGIC_FP_SHIFT;
	state.sensitivity_y = 1000LL << LGMAGIC_FP_SHIFT;
	state.dt = LGMAGIC_DEFAULT_DT;
	/* Isolate the projection geometry from the accelerometer correction. */
	state.gravity_valid = 1;
	state.gravity[2] = -LGMAGIC_FP_ONE;
	state.gravity_gain = 0;
	state.gravity_gain_fast = 0;
	state.deadzone = 0;
	state.threshold = 100 * LGMAGIC_FP_ONE;
	/* Per-axis test scale: 41 counts/g is convenient for the raw vectors. */
	state.accel_scale[0] = q16(41.0);
	state.accel_scale[1] = q16(41.0);
	state.accel_scale[2] = q16(41.0);
	state.accel_gate = q16(0.15);

	return state;
}

/*
 * At a level grip the gravity projection must reduce to the fixed-axis
 * baseline: canonical z (raw z) drives X, canonical y (raw x) drives Y.
 */
static void test_gravity_projection_matches_baseline(void)
{
	struct lg_magic_airmouse_state state = make_gravity_state();
	s16 zero[3] = { 0, 0, 0 };
	s16 up[3] = { 0, 0, -41 };
	s16 gyro[3];
	s16 mouse[2];

	/* First report initialises gravity and the screen basis. */
	lgmagic_calc_mouse_gravity(&state, zero, up, mouse);

	gyro[0] = 0;
	gyro[1] = 0;
	gyro[2] = 1000;
	lgmagic_calc_mouse_gravity(&state, gyro, up, mouse);
	CHECK(mouse[0] > 0);
	CHECK(mouse[1] == 0);

	state = make_gravity_state();
	lgmagic_calc_mouse_gravity(&state, zero, up, mouse);
	gyro[0] = 0;
	gyro[1] = 1000;
	gyro[2] = 0;
	lgmagic_calc_mouse_gravity(&state, gyro, up, mouse);
	CHECK(mouse[0] == 0);
	CHECK(mouse[1] > 0);

	/* A roll around the pointing axis must not move the cursor. */
	state = make_gravity_state();
	lgmagic_calc_mouse_gravity(&state, zero, up, mouse);
	gyro[0] = 1000;
	gyro[1] = 0;
	gyro[2] = 0;
	lgmagic_calc_mouse_gravity(&state, gyro, up, mouse);
	CHECK(llabs(mouse[0]) <= 2);
	CHECK(llabs(mouse[1]) <= 2);
}


static void test_gravity_seed_and_recovery(void)
{
	struct lg_magic_airmouse_state state = make_gravity_state();
	s16 zero[3] = { 0, 0, 0 };
	s16 bad[3] = { 41, 0, -41 };	/* 1.4 g, hand acceleration */
	s16 up[3] = { 0, 0, -41 };
	s16 shake[3] = { 5000, 5000, 5000 };
	s16 mouse[2];

	/* A sample under linear acceleration must not become the reference. */
	state.gravity_valid = 0;
	state.gravity_gain = LGMAGIC_FP_ONE;
	CHECK(lgmagic_calc_mouse_gravity(&state, zero, bad, mouse) == 0);
	CHECK(state.gravity_valid == 0);

	/*
	 * A shake with a valid 1 g sample seeds gravity on the same report,
	 * so the pointer activates immediately instead of waiting for a
	 * still period.  The next report already moves the cursor.
	 */
	state = make_gravity_state();
	state.gravity_valid = 0;
	state.threshold = q16(1.0);
	CHECK(lgmagic_calc_mouse_gravity(&state, shake, up, mouse) == 1);
	CHECK(state.gravity_valid == 1);
	CHECK(state.gravity_settled == 0);
	lgmagic_calc_mouse_gravity(&state, shake, up, mouse);
	CHECK(mouse[0] != 0 || mouse[1] != 0);

	/*
	 * A shake without a usable accelerometer still enters pointer mode and
	 * moves the cursor on the raw axes.
	 */
	state = make_gravity_state();
	state.gravity_valid = 0;
	state.threshold = q16(1.0);
	CHECK(lgmagic_calc_mouse_gravity(&state, shake, bad, mouse) == 1);
	CHECK(state.gravity_valid == 0);
	CHECK(mouse[0] != 0 && mouse[1] != 0);

	/* A still 1 g sample seeds gravity on its own. */
	state = make_gravity_state();
	state.gravity_valid = 0;
	lgmagic_calc_mouse_gravity(&state, zero, up, mouse);
	CHECK(state.gravity_valid == 1);
	CHECK(state.gravity_settled == 1);
	CHECK(llabs(state.gravity[0]) < 8);
	CHECK(llabs(state.gravity[1]) < 8);
	CHECK(llabs(state.gravity[2] + LGMAGIC_FP_ONE) < 8);

	/*
	 * A wrong seed is repaired by the first still 1 g sample, so a bad
	 * startup orientation does not survive until the next reconnect.
	 */
	state = make_gravity_state();
	state.gravity[0] = LGMAGIC_FP_ONE;
	state.gravity[1] = 0;
	state.gravity[2] = 0;
	lgmagic_calc_mouse_gravity(&state, zero, up, mouse);
	CHECK(llabs(state.gravity[0]) < 8);
	CHECK(llabs(state.gravity[1]) < 8);
	CHECK(llabs(state.gravity[2] + LGMAGIC_FP_ONE) < 8);
}


/*
 * A seed taken during hand motion may be tens of degrees off.  Until the
 * estimate is confirmed while still it must learn fast, so normal motion is
 * enough to repair it; after confirmation it must track slowly so that hand
 * acceleration does not tilt the basis.
 */

/* Without an SCD scale the magnitude gate is skipped, not hard-coded. */

/*
 * The SCD-derived scales must accept real 1 g orientations.  The vectors
 * are the measured means from the six static positions; the worst one is the
 * low y side, which is why the gate is 0.15 g rather than 0.1 g.
 */
static void test_scd_accel_scale_accepts_1g_orientations(void)
{
	static const s16 vectors[][3] = {
		{ 1, 0, -42 },		/* z+ */
		{ 0, 5, 44 },		/* z- */
		{ -31, 4, -3 },		/* x+ */
		{ 35, 3, 3 },		/* x- */
		{ 1, 35, -5 },		/* y+ */
		{ -1, -29, -3 },	/* y- */
	};
	s16 zero[3] = { 0, 0, 0 };
	s16 mouse[2];
	unsigned int i;

	for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++)
	{
		struct lg_magic_airmouse_state state = make_gravity_state();

		state.accel_scale[0] = q16(33.711);
		state.accel_scale[1] = q16(33.711);
		state.accel_scale[2] = q16(43.805);
		state.gravity_valid = 0;

		lgmagic_calc_mouse_gravity(&state, zero, vectors[i], mouse);
		CHECK(state.gravity_valid == 1);
	}
}

static void test_gravity_without_scale_skips_gate(void)
{
	struct lg_magic_airmouse_state state = make_gravity_state();
	s16 zero[3] = { 0, 0, 0 };
	s16 high[3] = { 41, 0, -41 };	/* 1.4 g, but no scale is known */
	s16 mouse[2];

	state.gravity_valid = 0;
	state.accel_scale[0] = 0;
	state.accel_scale[1] = 0;
	state.accel_scale[2] = 0;

	lgmagic_calc_mouse_gravity(&state, zero, high, mouse);
	CHECK(state.gravity_valid == 1);
}

static void test_gravity_fast_learning_until_settled(void)
{
	struct lg_magic_airmouse_state state = make_gravity_state();
	s16 zero[3] = { 0, 0, 0 };
	s16 moving[3] = { 60, 0, 0 };	/* ~0.064 rad/s, above zero_rate */
	s16 up[3] = { 0, 0, -41 };
	s16 mouse[2];
	const s64 bad_x = q16(sin(27.0 * M_PI / 180.0));
	const s64 bad_z = -q16(cos(27.0 * M_PI / 180.0));
	int i;

	state.gravity_valid = 1;
	state.gravity_settled = 0;
	state.gravity[0] = bad_x;
	state.gravity[1] = 0;
	state.gravity[2] = bad_z;
	state.gravity_gain = LGMAGIC_FP_ONE;
	state.gravity_gain_fast = 10 * LGMAGIC_FP_ONE;

	for (i = 0; i < 20; i++)
		lgmagic_calc_mouse_gravity(&state, moving, up, mouse);
	CHECK(state.gravity_settled == 0);
	CHECK(gravity_angle_deg(state.gravity, up) < 5.0);

	/* One still report confirms the estimate. */
	lgmagic_calc_mouse_gravity(&state, zero, up, mouse);
	CHECK(state.gravity_settled == 1);
	CHECK(gravity_angle_deg(state.gravity, up) < 1.0);

	/* After that, a disturbance is rejected rather than chased. */
	state.gravity[0] = bad_x;
	state.gravity[1] = 0;
	state.gravity[2] = bad_z;
	for (i = 0; i < 20; i++)
		lgmagic_calc_mouse_gravity(&state, moving, up, mouse);
	CHECK(gravity_angle_deg(state.gravity, up) > 10.0);
}


/*
 * Before gravity exists the pointer must still move, using the pre-SCD raw
 * axes, so the user can tell that the shake was accepted.  Once gravity is
 * seeded the fallback must not be used.
 */
static void test_gravity_fallback_before_seed(void)
{
	struct lg_magic_airmouse_state state = make_gravity_state();
	s16 up[3] = { 0, 0, -41 };
	s16 bad[3] = { 41, 0, -41 };	/* 1.4 g, cannot seed */
	s16 z_only[3] = { 0, 0, 4000 };
	s16 x_only[3] = { 4000, 0, 0 };
	s16 mouse[2];

	/* Raw +z drives X, raw +x drives Y, with the known-good signs. */
	state.gravity_valid = 0;
	state.threshold = q16(1.0);
	CHECK(lgmagic_calc_mouse_gravity(&state, z_only, bad, mouse) == 1);
	CHECK(state.gravity_valid == 0);
	CHECK(mouse[0] > 0);
	CHECK(mouse[1] == 0);

	state = make_gravity_state();
	state.gravity_valid = 0;
	state.threshold = q16(1.0);
	CHECK(lgmagic_calc_mouse_gravity(&state, x_only, bad, mouse) == 1);
	CHECK(mouse[0] == 0);
	CHECK(mouse[1] > 0);

	/* A still remote before the seed has no fallback drift. */
	state = make_gravity_state();
	state.gravity_valid = 0;
	{
		s16 zero[3] = { 0, 0, 0 };

		lgmagic_calc_mouse_gravity(&state, zero, bad, mouse);
		CHECK(mouse[0] == 0 && mouse[1] == 0);
	}

	/* Once a usable sample seeds gravity, the basis path takes over. */
	state = make_gravity_state();
	state.gravity_valid = 0;
	state.threshold = q16(1.0);
	lgmagic_calc_mouse_gravity(&state, z_only, up, mouse);
	CHECK(state.gravity_valid == 1);
	CHECK(llabs(state.gravity[0]) < 8);
	CHECK(llabs(state.gravity[2] + LGMAGIC_FP_ONE) < 8);
}


/* The pre-gravity fallback must use the same remap as the gravity path. */
static void test_fallback_basis_uses_remap(void)
{
	struct lg_magic_airmouse_state state = make_gravity_state();
	s64 rz90[3][3];

	make_rz90(rz90);
	lgmagic_airmouse_set_remap(&state, rz90);

	/* raw +Y (pointing), raw +z (X) and raw +x (Y) in the device frame. */
	CHECK(llabs(state.pointing[0] + LGMAGIC_FP_ONE) < 8);
	CHECK(llabs(state.pointing[1]) < 8);
	CHECK(llabs(state.pointing[2]) < 8);

	CHECK(llabs(state.fallback_right[0]) < 8);
	CHECK(llabs(state.fallback_right[1]) < 8);
	CHECK(llabs(state.fallback_right[2] - LGMAGIC_FP_ONE) < 8);

	CHECK(llabs(state.fallback_vertical[0]) < 8);
	CHECK(llabs(state.fallback_vertical[1] - LGMAGIC_FP_ONE) < 8);
	CHECK(llabs(state.fallback_vertical[2]) < 8);
}


/*
 * The accelerometer matrix, not the raw axes, defines up.  Under Rz90 a raw
 * +X sample points along device +Y.
 */
static void test_accel_up_vector_uses_matrix(void)
{
	struct lg_magic_airmouse_state state = make_gravity_state();
	s16 zero[3] = { 0, 0, 0 };
	s16 accel[3] = { 41, 0, 0 };
	s16 mouse[2];

	make_rz90(state.accel_matrix);
	state.accel_matrix_valid = 1;
	state.gravity_valid = 0;
	lgmagic_calc_mouse_gravity(&state, zero, accel, mouse);

	CHECK(state.gravity_valid == 1);
	CHECK(llabs(state.gravity[0]) < 8);
	CHECK(llabs(state.gravity[1] - LGMAGIC_FP_ONE) < 8);
	CHECK(llabs(state.gravity[2]) < 8);
}

/*
 * A rotation about the world vertical must be pure cursor-X at any tilt:
 * the vertical component of motion = omega x p cancels.  This is the
 * property that used to leak into Y before the basis was rebuilt.
 */
static void test_gravity_projection_is_tilt_invariant(void)
{
	static const double angles[] = { 0.0, 30.0, 45.0, -30.0 };
	const double pi = 3.14159265358979323846;
	s16 up[3] = { 0, 0, -41 };
	unsigned int i;

	for (i = 0; i < sizeof(angles) / sizeof(angles[0]); i++)
	{
		struct lg_magic_airmouse_state state = make_gravity_state();
		double a = angles[i] * pi / 180.0;
		s16 gyro[3];
		s16 mouse[2];

		state.gravity[0] = q16(-sin(a));
		state.gravity[1] = 0;
		state.gravity[2] = q16(-cos(a));

		/* omega = gravity: a rotation about the world vertical. */
		gyro[0] = (s16)llround(-sin(a) / GYRO_SCALE);
		gyro[1] = 0;
		gyro[2] = (s16)llround(-cos(a) / GYRO_SCALE);

		lgmagic_calc_mouse_gravity(&state, gyro, up, mouse);
		CHECK(mouse[0] < 0);
		CHECK(llabs(mouse[1]) <= 1);

		/* A roll around p at the same tilt must not move the cursor. */
		state.gravity[0] = q16(-sin(a));
		state.gravity[1] = 0;
		state.gravity[2] = q16(-cos(a));
		gyro[0] = (s16)llround(-1.0 / GYRO_SCALE);
		gyro[1] = 0;
		gyro[2] = 0;
		lgmagic_calc_mouse_gravity(&state, gyro, up, mouse);
		CHECK(llabs(mouse[0]) <= 1);
		CHECK(llabs(mouse[1]) <= 1);
	}
}

/*
 * With both the gyro matrix and the remap at Rz90 the device-frame
 * projection must still reduce to the raw-axis reference mapping.
 */
static void test_remap_keeps_projection(void)
{
	struct lg_magic_airmouse_state state = make_gravity_state();
	s64 remap[3][3];
	s16 gyro[3] = { 2000, 0, 3000 };
	s16 up[3] = { 0, 0, -41 };
	s16 mouse[2];

	make_rz90_scaled(state.gyro_matrix, GYRO_SCALE);
	make_rz90(state.accel_matrix);
	state.accel_matrix_valid = 1;
	make_rz90(remap);
	lgmagic_airmouse_set_remap(&state, remap);

	lgmagic_calc_mouse_gravity(&state, gyro, up, mouse);

	CHECK(mouse[0] > 0);	/* raw z */
	CHECK(mouse[1] > 0);	/* raw x */
}

static void test_auto_bias_converges(void)
{
	struct lg_magic_airmouse_state state = make_state();
	s16 gyro[3] = { 100, -100, 50 };
	s16 mouse[2];
	int i;

	state.auto_bias = 1;
	state.bias_beta = q16(0.5);
	state.zero_rate = q16(1000.0);

	for (i = 0; i < 40; i++)
		lgmagic_calc_mouse(&state, gyro, mouse);

	CHECK(llabs(state.gyro_bias[0] - q16(100.0)) < 2);
	CHECK(llabs(state.gyro_bias[1] - q16(-100.0)) < 2);
	CHECK(llabs(state.gyro_bias[2] - q16(50.0)) < 2);
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
		for (j = 0; j < 3; j++)
		{
			size_t at = base + (first_float + 3 * i + j) * 4;

			put_float(scd + at, (float)m[i][j]);
		}
}

static void test_scd_matrix_validation(void)
{
	u8 scd[LGMAGIC_SCD_SIZE];
	s64 gyro[3][3], accel[3][3], remap[3][3], gyro_scale[3];
	s64 accel_scale[3], sample_dt;
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
					 remap, gyro_scale, accel_scale,
					 &sample_dt) == 0);
	CHECK(sample_dt == q16(0.01));
	CHECK(llabs(gyro_scale[0] - q16(0.001)) <= 1);
	CHECK(llabs(gyro_scale[1] - q16(0.001)) <= 1);
	CHECK(llabs(gyro_scale[2] - q16(0.001)) <= 1);

	/* A zero tail leaves the accelerometer scale unknown. */
	CHECK(accel_scale[0] == 0);
	CHECK(accel_scale[1] == 0);
	CHECK(accel_scale[2] == 0);

	/* A missing/garbage sample period falls back to the 10 ms default. */
	put_float(scd + 9, 0.0f);
	CHECK(lgmagic_parse_scd_matrices(scd, sizeof(scd), gyro, accel,
					 remap, gyro_scale, accel_scale,
					 &sample_dt) == 0);
	CHECK(sample_dt == LGMAGIC_DEFAULT_DT);
	put_float(scd + 9, 0.01f);

	/* Per-axis scales are preserved in the raw-axis gyro scales. */
	gm[0][0] = 0.0005;
	gm[1][1] = 0.001;
	gm[2][2] = 0.0015;
	write_scd_matrix(scd, 24, gm);
	CHECK(lgmagic_parse_scd_matrices(scd, sizeof(scd), gyro, accel,
					 remap, gyro_scale, accel_scale,
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
					 accel_scale, &sample_dt) == -EINVAL);
	CHECK(lgmagic_parse_scd_matrices(scd, 140, gyro, accel, remap,
					 gyro_scale, accel_scale,
					 &sample_dt) == -EINVAL);
	scd[0] = 0xff;
	CHECK(lgmagic_parse_scd_matrices(scd, sizeof(scd), gyro, accel,
					 remap, gyro_scale, accel_scale,
					 &sample_dt) == -EINVAL);
	scd[0] = 0x00;

	/* A reflection is an inversion, not a remap. */
	gm[0][0] = -0.001;
	gm[1][1] = 0.001;
	gm[2][2] = 0.001;
	write_scd_matrix(scd, 24, gm);
	CHECK(lgmagic_parse_scd_matrices(scd, sizeof(scd), gyro, accel,
					 remap, gyro_scale, accel_scale,
					 &sample_dt) == -EINVAL);

	/* A singular matrix is rejected. */
	gm[0][0] = 0.0;
	gm[1][1] = 0.0;
	gm[2][2] = 0.0;
	write_scd_matrix(scd, 24, gm);
	CHECK(lgmagic_parse_scd_matrices(scd, sizeof(scd), gyro, accel,
					 remap, gyro_scale, accel_scale,
					 &sample_dt) == -EINVAL);
}

static void test_scd_independent_remaps(void)
{
	u8 scd[LGMAGIC_SCD_SIZE];
	s64 gyro[3][3], accel[3][3], remap[3][3], gyro_scale[3];
	s64 accel_scale[3], sample_dt;
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
					 remap, gyro_scale, accel_scale,
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
	s64 accel_scale[3], sample_dt;
	const double gs = 0.0010652969358488917;
	const double as = 78.4000015258789;
	int i;

	CHECK(lgmagic_parse_scd_matrices(scd, sizeof(scd), gyro, accel,
					 remap, gyro_scale, accel_scale,
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

	/*
	 * The SCD tail gives counts/g = g * reference / coefficient:
	 * 9.80665 * 9.0112 / 2.62136 for x/y and / 2.01734 for z.
	 */
	CHECK(llabs(accel_scale[0] - q16(33.711)) <= q16(0.2));
	CHECK(llabs(accel_scale[1] - q16(33.711)) <= q16(0.2));
	CHECK(llabs(accel_scale[2] - q16(43.805)) <= q16(0.2));
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
	test_baseline_axes_and_signs();
	test_gravity_projection_matches_baseline();
	test_gravity_seed_and_recovery();
	test_scd_accel_scale_accepts_1g_orientations();
	test_gravity_without_scale_skips_gate();
	test_gravity_fast_learning_until_settled();
	test_gravity_fallback_before_seed();
	test_fallback_basis_uses_remap();
	test_accel_up_vector_uses_matrix();
	test_gravity_projection_is_tilt_invariant();
	test_remap_keeps_projection();
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
