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

static struct lg_magic_airmouse_calib make_calib(void)
{
	struct lg_magic_airmouse_calib calib = {
		.gyro_scale = { q16(0.07), q16(0.07), q16(0.07) },
		.alpha = q16(0.2),
		.mouse_k = q16(0.5),
	};

	return calib;
}

static uint32_t rng_state = 0x12345678;

static s16 rng_gyro(void)
{
	rng_state = rng_state * 1664525u + 1013904223u;
	return (s16)((int)(rng_state >> 16) % 241 - 120);
}

static int calc_mouse_float(const float *bias, const float *scale,
			    float alpha, float mouse_k, u16 threshold,
			    float *acc, const s16 *gyro, s16 *mouse)
{
	for (int i = 0; i < 3; i++) {
		float corr = ((float)gyro[i] - bias[i]) * scale[i];

		acc[i] = alpha * corr + (1.0f - alpha) * acc[i];
	}

	mouse[0] = (s16)(acc[2] * mouse_k);
	mouse[1] = (s16)(acc[0] * mouse_k);

	return fabsf(acc[0]) > threshold || fabsf(acc[2]) > threshold;
}

static void test_zero_mean_noise_does_not_drift(void)
{
	struct lg_magic_airmouse_calib calib = make_calib();
	const float bias[3] = { 0.0f, 0.0f, 0.0f };
	const float scale[3] = { 0.07f, 0.07f, 0.07f };
	const float alpha = 0.2f;
	const float mouse_k = 0.5f;
	const u16 threshold = 300;
	float float_acc[3] = { 0.0f, 0.0f, 0.0f };
	s64 fixed_acc[3] = { 0, 0, 0 };
	long long float_sum = 0;
	long long fixed_sum = 0;

	for (int i = 0; i < 200000; i++) {
		s16 gyro[3] = { rng_gyro(), 0, rng_gyro() };
		s16 float_mouse[2];
		s16 fixed_mouse[2];

		calc_mouse_float(bias, scale, alpha, mouse_k, threshold,
				 float_acc, gyro, float_mouse);
		lgmagic_calc_mouse(&calib, fixed_acc, threshold, gyro, fixed_mouse);

		float_sum += float_mouse[0] + float_mouse[1];
		fixed_sum += fixed_mouse[0] + fixed_mouse[1];
	}

	/*
	 * A right shift in the conversion back to integer rounds negative
	 * values towards -infinity and accumulates a negative drift.  Keep
	 * the accumulated error close to the float implementation.
	 */
	CHECK(llabs(fixed_sum - float_sum) < 2000);
	CHECK(llabs(fixed_sum) < 2000);
}

static void test_constant_negative_input_does_not_drift(void)
{
	struct lg_magic_airmouse_calib calib = make_calib();
	const float bias[3] = { 0.0f, 0.0f, 0.0f };
	const float scale[3] = { 0.07f, 0.07f, 0.07f };
	const u16 threshold = 300;
	float float_acc[3] = { 0.0f, 0.0f, 0.0f };
	s64 fixed_acc[3] = { 0, 0, 0 };
	long long float_sum = 0;
	long long fixed_sum = 0;

	for (int i = 0; i < 100000; i++) {
		s16 gyro[3] = { -1, 0, -1 };
		s16 float_mouse[2];
		s16 fixed_mouse[2];

		calc_mouse_float(bias, scale, 0.2f, 0.5f, threshold,
				 float_acc, gyro, float_mouse);
		lgmagic_calc_mouse(&calib, fixed_acc, threshold, gyro, fixed_mouse);

		float_sum += float_mouse[0] + float_mouse[1];
		fixed_sum += fixed_mouse[0] + fixed_mouse[1];
	}

	CHECK(float_sum == 0);
	CHECK(fixed_sum == 0);
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

static void test_blob_roundtrip(void)
{
	struct lgmagic_calib_blob blob = make_blob();
	struct lg_magic_airmouse_calib calib;

	CHECK(lgmagic_convert_calib(&calib, (const u8 *)&blob, sizeof(blob)) == 0);
	CHECK(calib.gyro_bias[0] == q16(1.5));
	CHECK(calib.gyro_bias[1] == q16(-2.25));
	CHECK(calib.gyro_bias[2] == q16(3.0));
	CHECK(calib.gyro_scale[0] == q16(0.07));
	CHECK(calib.gyro_scale[1] == q16(0.08));
	CHECK(calib.gyro_scale[2] == q16(0.09));
	CHECK(calib.alpha == q16(0.2));
	CHECK(calib.mouse_k == q16(0.5));
	CHECK(lgmagic_validate_calib(&calib) == 0);
}

static void test_blob_rejects_bad_header(void)
{
	struct lgmagic_calib_blob blob;
	struct lg_magic_airmouse_calib calib;

	blob = make_blob();
	blob.magic = 0;
	CHECK(lgmagic_convert_calib(&calib, (const u8 *)&blob, sizeof(blob)) == -EINVAL);

	blob = make_blob();
	blob.version = LGMAGIC_CALIB_VERSION + 1;
	CHECK(lgmagic_convert_calib(&calib, (const u8 *)&blob, sizeof(blob)) == -EINVAL);

	blob = make_blob();
	blob.size = sizeof(blob) - 1;
	CHECK(lgmagic_convert_calib(&calib, (const u8 *)&blob, sizeof(blob)) == -EINVAL);

	blob = make_blob();
	CHECK(lgmagic_convert_calib(&calib, (const u8 *)&blob, sizeof(blob) - 1) == -EINVAL);
}

static void test_validation_ranges(void)
{
	struct lg_magic_airmouse_calib calib = make_calib();

	CHECK(lgmagic_validate_calib(&calib) == 0);

	calib.gyro_bias[0] = 101 * LGMAGIC_FP_ONE;
	CHECK(lgmagic_validate_calib(&calib) == 1);

	calib = make_calib();
	calib.gyro_scale[0] = 11 * LGMAGIC_FP_ONE;
	CHECK(lgmagic_validate_calib(&calib) == 1);

	calib = make_calib();
	calib.alpha = -1;
	CHECK(lgmagic_validate_calib(&calib) == 1);

	calib = make_calib();
	calib.alpha = LGMAGIC_FP_ONE + 1;
	CHECK(lgmagic_validate_calib(&calib) == 1);

	calib = make_calib();
	calib.mouse_k = -1;
	CHECK(lgmagic_validate_calib(&calib) == 1);

	calib = make_calib();
	calib.mouse_k = LGMAGIC_FP_ONE + 1;
	CHECK(lgmagic_validate_calib(&calib) == 1);
}

int main(void)
{
	test_zero_mean_noise_does_not_drift();
	test_constant_negative_input_does_not_drift();
	test_blob_roundtrip();
	test_blob_rejects_bad_header();
	test_validation_ranges();

	if (failures) {
		fprintf(stderr, "%d test(s) failed\n", failures);
		return 1;
	}

	printf("all airmouse math tests passed\n");
	return 0;
}
