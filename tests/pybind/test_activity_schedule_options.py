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

    def test_final_shared_input_peer_options_are_forwarded(self) -> None:
        self.assertEqual(
            _compile_activity_schedule_kwargs(
                {
                    "final_shared_input_peer_policy": "probe",
                    "final_shared_input_peer_profile_path": "/tmp/shared-fire.tsv",
                    "final_shared_input_peer_max_node_ops": 8,
                    "final_shared_input_peer_max_inputs": 16,
                    "final_shared_input_peer_max_outputs": 16,
                    "final_shared_input_peer_max_value_width": 64,
                    "final_shared_input_peer_max_peers": 8,
                    "final_shared_input_peer_max_candidates": 4096,
                    "final_shared_input_peer_max_moves": 128,
                    "final_shared_input_peer_max_moved_op_ppm": 200,
                    "final_shared_input_peer_profile_min_source_fire": 1234,
                }
            ),
            [
                "-final-shared-input-peer-policy",
                "probe",
                "-final-shared-input-peer-profile-path",
                "/tmp/shared-fire.tsv",
                "-final-shared-input-peer-max-node-ops",
                "8",
                "-final-shared-input-peer-max-inputs",
                "16",
                "-final-shared-input-peer-max-outputs",
                "16",
                "-final-shared-input-peer-max-value-width",
                "64",
                "-final-shared-input-peer-max-peers",
                "8",
                "-final-shared-input-peer-max-candidates",
                "4096",
                "-final-shared-input-peer-max-moves",
                "128",
                "-final-shared-input-peer-max-moved-op-ppm",
                "200",
                "-final-shared-input-peer-profile-min-source-fire",
                "1234",
            ],
        )

    def test_final_shared_input_peer_integer_options_must_be_non_negative(self) -> None:
        for option in (
            "final_shared_input_peer_max_node_ops",
            "final_shared_input_peer_max_inputs",
            "final_shared_input_peer_max_outputs",
            "final_shared_input_peer_max_value_width",
            "final_shared_input_peer_max_peers",
            "final_shared_input_peer_max_candidates",
            "final_shared_input_peer_max_moves",
            "final_shared_input_peer_max_moved_op_ppm",
            "final_shared_input_peer_profile_min_source_fire",
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
