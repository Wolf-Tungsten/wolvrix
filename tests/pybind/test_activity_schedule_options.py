#!/usr/bin/env python3

import unittest

from wolvrix import _compile_activity_schedule_kwargs


class ActivityScheduleOptionTest(unittest.TestCase):
    def test_final_sibling_fusion_options_are_forwarded(self) -> None:
        self.assertEqual(
            _compile_activity_schedule_kwargs(
                {
                    "final_sibling_fusion_policy": "probe",
                    "final_sibling_fusion_min_gain": 4,
                    "final_sibling_fusion_max_pairs": 256,
                    "final_sibling_fusion_max_fused_op_ppm": 5000,
                }
            ),
            [
                "-final-sibling-fusion-policy",
                "probe",
                "-final-sibling-fusion-min-gain",
                "4",
                "-final-sibling-fusion-max-pairs",
                "256",
                "-final-sibling-fusion-max-fused-op-ppm",
                "5000",
            ],
        )

    def test_final_sibling_fusion_integer_options_must_be_non_negative(self) -> None:
        for option in (
            "final_sibling_fusion_min_gain",
            "final_sibling_fusion_max_pairs",
            "final_sibling_fusion_max_fused_op_ppm",
        ):
            with self.subTest(option=option):
                with self.assertRaisesRegex(ValueError, option):
                    _compile_activity_schedule_kwargs({option: -1})
                with self.assertRaisesRegex(ValueError, option):
                    _compile_activity_schedule_kwargs({option: "1"})

    def test_unknown_options_remain_strictly_rejected(self) -> None:
        with self.assertRaisesRegex(TypeError, "unknown_final_sibling_fusion_option"):
            _compile_activity_schedule_kwargs({"unknown_final_sibling_fusion_option": 1})


if __name__ == "__main__":
    unittest.main()
