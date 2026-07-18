#!/usr/bin/env python3

import unittest

from wolvrix import _compile_activity_schedule_kwargs


class ActivityScheduleOptionTest(unittest.TestCase):
    def test_final_terminal_pushforward_options_are_forwarded(self) -> None:
        self.assertEqual(
            _compile_activity_schedule_kwargs(
                {
                    "final_terminal_pushforward_policy": "probe",
                    "final_terminal_pushforward_profile_path": "/tmp/fire.tsv",
                    "final_terminal_pushforward_max_node_ops": 9,
                    "final_terminal_pushforward_max_inputs": 17,
                    "final_terminal_pushforward_max_outputs": 18,
                    "final_terminal_pushforward_max_value_width": 65,
                    "final_terminal_pushforward_min_bae_gain": 2,
                    "final_terminal_pushforward_min_boundary_value_gain": 3,
                    "final_terminal_pushforward_max_moves": 129,
                    "final_terminal_pushforward_max_moved_op_ppm": 201,
                    "final_terminal_pushforward_profile_min_source_fire": 1234,
                }
            ),
            [
                "-final-terminal-pushforward-policy",
                "probe",
                "-final-terminal-pushforward-profile-path",
                "/tmp/fire.tsv",
                "-final-terminal-pushforward-max-node-ops",
                "9",
                "-final-terminal-pushforward-max-inputs",
                "17",
                "-final-terminal-pushforward-max-outputs",
                "18",
                "-final-terminal-pushforward-max-value-width",
                "65",
                "-final-terminal-pushforward-min-bae-gain",
                "2",
                "-final-terminal-pushforward-min-boundary-value-gain",
                "3",
                "-final-terminal-pushforward-max-moves",
                "129",
                "-final-terminal-pushforward-max-moved-op-ppm",
                "201",
                "-final-terminal-pushforward-profile-min-source-fire",
                "1234",
            ],
        )

    def test_final_terminal_pushforward_integer_options_must_be_non_negative(self) -> None:
        for option in (
            "final_terminal_pushforward_max_node_ops",
            "final_terminal_pushforward_max_inputs",
            "final_terminal_pushforward_max_outputs",
            "final_terminal_pushforward_max_value_width",
            "final_terminal_pushforward_min_bae_gain",
            "final_terminal_pushforward_min_boundary_value_gain",
            "final_terminal_pushforward_max_moves",
            "final_terminal_pushforward_max_moved_op_ppm",
            "final_terminal_pushforward_profile_min_source_fire",
        ):
            with self.subTest(option=option):
                with self.assertRaisesRegex(ValueError, option):
                    _compile_activity_schedule_kwargs({option: -1})
                with self.assertRaisesRegex(ValueError, option):
                    _compile_activity_schedule_kwargs({option: "1"})

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
