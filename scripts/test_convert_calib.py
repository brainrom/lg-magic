#!/usr/bin/env python3

import json
import math
import struct
import tempfile
import unittest
from pathlib import Path

import convert_calib as cc


class BlobFormatTests(unittest.TestCase):
    def test_blob_size_and_layout(self):
        self.assertEqual(cc.BLOB_SIZE, 40)
        self.assertEqual(cc.BLOB_STRUCT.format, "<4sHH8i")

    def test_magic_and_header(self):
        blob = cc.build_blob([0, 0, 0], [0.07, 0.07, 0.07], 0.2, 0.5)
        self.assertEqual(blob[:4], b"LGC2")
        self.assertEqual(struct.unpack_from("<I", blob)[0], 0x3243474C)
        self.assertEqual(struct.unpack_from("<H", blob, 4)[0], cc.VERSION)
        self.assertEqual(struct.unpack_from("<H", blob, 6)[0], cc.BLOB_SIZE)

    def test_values_roundtrip(self):
        blob = cc.build_blob([1.5, -2.25, 3.0],
                             [0.07, 0.08, 0.09], 0.2, 0.5)
        parsed = cc.parse_blob(blob)

        self.assertEqual(parsed["gyro_bias"],
                         [cc.to_q16(1.5), cc.to_q16(-2.25), cc.to_q16(3.0)])
        self.assertEqual(parsed["gyro_scale"],
                         [cc.to_q16(0.07), cc.to_q16(0.08), cc.to_q16(0.09)])
        self.assertEqual(parsed["alpha"], cc.to_q16(0.2))
        self.assertEqual(parsed["mouse_k"], cc.to_q16(0.5))

    def test_rounding_half_away_from_zero(self):
        self.assertEqual(cc.to_q16(0.5), 32768)
        self.assertEqual(cc.to_q16(-0.5), -32768)
        self.assertEqual(cc.to_q16(0.5 / cc.Q16_ONE), 1)
        self.assertEqual(cc.to_q16(-0.5 / cc.Q16_ONE), -1)

    def test_parse_accepts_trailing_data(self):
        blob = cc.build_blob([0, 0, 0], [0.07, 0.07, 0.07], 0.2, 0.5)
        self.assertEqual(cc.parse_blob(blob + b"extra")["size"], cc.BLOB_SIZE)

    def test_parse_rejects_bad_magic(self):
        blob = bytearray(cc.build_blob([0, 0, 0], [0.07, 0.07, 0.07], 0.2, 0.5))
        blob[0] = ord("X")
        with self.assertRaisesRegex(ValueError, "magic"):
            cc.parse_blob(bytes(blob))

    def test_parse_rejects_bad_version(self):
        blob = bytearray(cc.build_blob([0, 0, 0], [0.07, 0.07, 0.07], 0.2, 0.5))
        struct.pack_into("<H", blob, 4, cc.VERSION + 1)
        with self.assertRaisesRegex(ValueError, "version"):
            cc.parse_blob(bytes(blob))

    def test_parse_rejects_bad_size(self):
        blob = bytearray(cc.build_blob([0, 0, 0], [0.07, 0.07, 0.07], 0.2, 0.5))
        struct.pack_into("<H", blob, 6, cc.BLOB_SIZE - 1)
        with self.assertRaisesRegex(ValueError, "size"):
            cc.parse_blob(bytes(blob))

    def test_parse_rejects_truncated_blob(self):
        blob = cc.build_blob([0, 0, 0], [0.07, 0.07, 0.07], 0.2, 0.5)
        with self.assertRaisesRegex(ValueError, "too short"):
            cc.parse_blob(blob[:-1])


class ValidationTests(unittest.TestCase):
    def test_accepts_boundary_values(self):
        cc.build_blob([100, -100, 0], [10, -10, 0], 0.0, 0.0)
        cc.build_blob([0, 0, 0], [0, 0, 0], 1.0, 1.0)

    def test_rejects_non_finite_values(self):
        for value in (math.nan, math.inf, -math.inf):
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    cc.build_blob([value, 0, 0], [0, 0, 0], 0.5, 0.5)
                with self.assertRaises(ValueError):
                    cc.build_blob([0, 0, 0], [value, 0, 0], 0.5, 0.5)
                with self.assertRaises(ValueError):
                    cc.build_blob([0, 0, 0], [0, 0, 0], value, 0.5)
                with self.assertRaises(ValueError):
                    cc.build_blob([0, 0, 0], [0, 0, 0], 0.5, value)

    def test_rejects_out_of_range_values(self):
        for bias in (100.001, -100.001):
            with self.assertRaises(ValueError):
                cc.build_blob([bias, 0, 0], [0, 0, 0], 0.5, 0.5)
        for scale in (10.001, -10.001):
            with self.assertRaises(ValueError):
                cc.build_blob([0, 0, 0], [scale, 0, 0], 0.5, 0.5)
        for value in (-0.001, 1.001):
            with self.assertRaises(ValueError):
                cc.build_blob([0, 0, 0], [0, 0, 0], value, 0.5)
            with self.assertRaises(ValueError):
                cc.build_blob([0, 0, 0], [0, 0, 0], 0.5, value)

    def test_rejects_wrong_number_of_axes(self):
        with self.assertRaises(ValueError):
            cc.build_blob([0, 0], [0, 0, 0], 0.5, 0.5)
        with self.assertRaises(ValueError):
            cc.build_blob([0, 0, 0], [0, 0], 0.5, 0.5)


class LegacyTests(unittest.TestCase):
    def test_parse_legacy_little_and_big_endian(self):
        values = (1.5, -2.25, 3.0, 0.07, 0.08, 0.09, 0.2, 0.5)
        little = struct.pack("<3f3fff", *values)
        big = struct.pack(">3f3fff", *values)

        for parsed, expected in ((cc.parse_legacy_blob(little, "little"), values),
                                 (cc.parse_legacy_blob(big, "big"), values)):
            for got, want in zip(parsed, expected):
                self.assertAlmostEqual(got, want)
        self.assertNotEqual(cc.parse_legacy_blob(big, "little")[0], values[0])

    def test_legacy_blob_upgrade(self):
        values = (1.5, -2.25, 3.0, 0.07, 0.08, 0.09, 0.2, 0.5)
        legacy = struct.pack("<3f3fff", *values)
        parsed_values = cc.parse_legacy_blob(legacy)
        parsed = cc.parse_blob(cc.build_blob(
            parsed_values[0:3], parsed_values[3:6],
            parsed_values[6], parsed_values[7]))

        self.assertEqual(parsed["gyro_bias"], [cc.to_q16(v) for v in values[:3]])
        self.assertEqual(parsed["gyro_scale"], [cc.to_q16(v) for v in values[3:6]])
        self.assertEqual(parsed["alpha"], cc.to_q16(values[6]))
        self.assertEqual(parsed["mouse_k"], cc.to_q16(values[7]))

    def test_legacy_blob_too_short(self):
        with self.assertRaisesRegex(ValueError, "too short"):
            cc.parse_legacy_blob(b"\0" * (cc.LEGACY_SIZE - 1))


class CommandLineTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)

    def test_json_input(self):
        json_file = self.dir / "calib.json"
        out_file = self.dir / "calib.bin"
        json_file.write_text(json.dumps({
            "gyro": {
                "bias": [1.5, -2.25, 3.0],
                "scale": [0.07, 0.08, 0.09],
            }
        }), encoding="utf-8")

        ret = cc.main([str(json_file), str(out_file),
                       "--alpha", "0.2", "--mouse_k", "0.5"])

        self.assertEqual(ret, 0)
        parsed = cc.parse_blob(out_file.read_bytes())
        self.assertEqual(parsed["gyro_bias"][0], cc.to_q16(1.5))
        self.assertEqual(parsed["alpha"], cc.to_q16(0.2))

    def test_legacy_input(self):
        legacy_file = self.dir / "legacy.bin"
        out_file = self.dir / "calib.bin"
        legacy_file.write_bytes(struct.pack(
            "<3f3fff", 1.5, -2.25, 3.0, 0.07, 0.08, 0.09, 0.2, 0.5))

        ret = cc.main([str(legacy_file), str(out_file), "--from-legacy"])

        self.assertEqual(ret, 0)
        parsed = cc.parse_blob(out_file.read_bytes())
        self.assertEqual(parsed["gyro_bias"][1], cc.to_q16(-2.25))
        self.assertEqual(parsed["mouse_k"], cc.to_q16(0.5))

    def test_legacy_big_endian_input(self):
        legacy_file = self.dir / "legacy-be.bin"
        out_file = self.dir / "calib.bin"
        legacy_file.write_bytes(struct.pack(
            ">3f3fff", 1.5, -2.25, 3.0, 0.07, 0.08, 0.09, 0.2, 0.5))

        ret = cc.main([str(legacy_file), str(out_file),
                       "--from-legacy", "--legacy-endian", "big"])

        self.assertEqual(ret, 0)
        parsed = cc.parse_blob(out_file.read_bytes())
        self.assertEqual(parsed["gyro_bias"][0], cc.to_q16(1.5))

    def test_json_requires_alpha_and_mouse_k(self):
        json_file = self.dir / "calib.json"
        out_file = self.dir / "calib.bin"
        json_file.write_text(json.dumps({
            "gyro": {"bias": [0, 0, 0], "scale": [1, 1, 1]}
        }), encoding="utf-8")

        self.assertEqual(cc.main([str(json_file), str(out_file)]), 1)
        self.assertFalse(out_file.exists())

    def test_legacy_rejects_alpha(self):
        legacy_file = self.dir / "legacy.bin"
        out_file = self.dir / "calib.bin"
        legacy_file.write_bytes(struct.pack("<3f3fff", 0, 0, 0, 1, 1, 1, 0.2, 0.5))

        self.assertEqual(cc.main([str(legacy_file), str(out_file),
                                  "--from-legacy", "--alpha", "0.2"]), 1)
        self.assertFalse(out_file.exists())

    def test_json_missing_gyro_section(self):
        json_file = self.dir / "calib.json"
        out_file = self.dir / "calib.bin"
        json_file.write_text(json.dumps({"accel": {}}), encoding="utf-8")

        self.assertEqual(cc.main([str(json_file), str(out_file),
                                  "--alpha", "0.2", "--mouse_k", "0.5"]), 1)
        self.assertFalse(out_file.exists())

    def test_json_gyro_section_is_not_an_object(self):
        json_file = self.dir / "calib.json"
        out_file = self.dir / "calib.bin"
        json_file.write_text(json.dumps({"gyro": []}), encoding="utf-8")

        self.assertEqual(cc.main([str(json_file), str(out_file),
                                  "--alpha", "0.2", "--mouse_k", "0.5"]), 1)
        self.assertFalse(out_file.exists())


if __name__ == "__main__":
    unittest.main()
