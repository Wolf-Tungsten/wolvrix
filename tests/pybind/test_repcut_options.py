#!/usr/bin/env python3

import unittest

from wolvrix import _compile_repcut_kwargs


class RepcutOptionTest(unittest.TestCase):
    def test_cost_model_is_sparse_and_forwarded(self) -> None:
        self.assertEqual(_compile_repcut_kwargs({}), [])
        self.assertEqual(_compile_repcut_kwargs({"cost_model": None}), [])
        for value in ("legacy", "verilator-v1", "verilator-lowered-v1", "grhsim-v1"):
            with self.subTest(value=value):
                self.assertEqual(
                    _compile_repcut_kwargs({"cost_model": value}),
                    ["-cost-model", value],
                )

    def test_cost_model_is_strictly_validated(self) -> None:
        for value in (
            "", "gsim", "grhsim", "verilator", "verilator_v1", "LEGACY",
            " legacy", "legacy ", False, 1, [], {},
        ):
            with self.subTest(value=value):
                with self.assertRaisesRegex(
                    ValueError, "cost_model must be one of: legacy, verilator-v1, verilator-lowered-v1, grhsim-v1"
                ):
                    _compile_repcut_kwargs({"cost_model": value})

    def test_cost_model_and_weight_mode_are_independent(self) -> None:
        for cost_model in ("legacy", "verilator-v1", "verilator-lowered-v1", "grhsim-v1"):
            for weight_mode in ("baseline", "closure-aware"):
                with self.subTest(cost_model=cost_model, weight_mode=weight_mode):
                    self.assertEqual(
                        _compile_repcut_kwargs(
                            {"cost_model": cost_model, "weight_mode": weight_mode}
                        ),
                        ["-cost-model", cost_model, "-weight-mode", weight_mode],
                    )

    def test_weight_mode_is_sparse_and_forwarded(self) -> None:
        self.assertEqual(_compile_repcut_kwargs({}), [])
        self.assertEqual(_compile_repcut_kwargs({"weight_mode": None}), [])
        for value in ("baseline", "closure-aware"):
            with self.subTest(value=value):
                self.assertEqual(
                    _compile_repcut_kwargs({"weight_mode": value}),
                    ["-weight-mode", value],
                )

    def test_weight_mode_is_strictly_validated(self) -> None:
        for value in ("", "candidate", "closure_aware", "BASELINE", " baseline", False, 1):
            with self.subTest(value=value):
                with self.assertRaisesRegex(
                    ValueError, "weight_mode must be one of: baseline, closure-aware"
                ):
                    _compile_repcut_kwargs({"weight_mode": value})

    def test_communication_weight_scale_is_sparse_and_validated(self) -> None:
        self.assertEqual(_compile_repcut_kwargs({"communication_weight_scale": 1}),
                         ["-communication-weight-scale", "1"])
        self.assertEqual(_compile_repcut_kwargs({"communication_weight_scale": 2}),
                         ["-communication-weight-scale", "2"])
        for value in (0, -1, False, 1.5, "2"):
            with self.subTest(value=value):
                with self.assertRaisesRegex(ValueError, "communication_weight_scale must be a positive integer"):
                    _compile_repcut_kwargs({"communication_weight_scale": value})

    def test_communication_receiver_weight_scale_is_sparse_and_validated(self) -> None:
        self.assertEqual(_compile_repcut_kwargs({"communication_receiver_weight_scale": 0}),
                         ["-communication-receiver-weight-scale", "0"])
        self.assertEqual(_compile_repcut_kwargs({"communication_receiver_weight_scale": 16}),
                         ["-communication-receiver-weight-scale", "16"])
        for value in (-1, False, 1.5, "16"):
            with self.subTest(value=value):
                with self.assertRaisesRegex(ValueError, "communication_receiver_weight_scale must be a non-negative integer"):
                    _compile_repcut_kwargs({"communication_receiver_weight_scale": value})

    def test_communication_value_edge_weight_scale_is_sparse_and_validated(self) -> None:
        self.assertEqual(_compile_repcut_kwargs({"communication_value_edge_weight_scale": 0}),
                         ["-communication-value-edge-weight-scale", "0"])
        self.assertEqual(_compile_repcut_kwargs({"communication_value_edge_weight_scale": 64}),
                         ["-communication-value-edge-weight-scale", "64"])
        for value in (-1, False, 1.5, "64"):
            with self.subTest(value=value):
                with self.assertRaisesRegex(ValueError, "communication_value_edge_weight_scale must be a non-negative integer"):
                    _compile_repcut_kwargs({"communication_value_edge_weight_scale": value})

    def test_unknown_options_remain_strictly_rejected(self) -> None:
        with self.assertRaisesRegex(TypeError, "unknown_weight_option"):
            _compile_repcut_kwargs({"unknown_weight_option": "baseline"})

    def test_initial_assignment_is_explicit_and_validated(self) -> None:
        self.assertEqual(_compile_repcut_kwargs({"initial_assignment": None}), [])
        self.assertEqual(_compile_repcut_kwargs({"initial_assignment": "grhsim-terminal-v1",
                                               "initial_core_imbalance_factor": 0.05}),
                         ["-initial-assignment", "grhsim-terminal-v1", "-initial-core-imbalance-factor", "0.05"])
        self.assertEqual(_compile_repcut_kwargs({"initial_assignment": "grhsim-terminal-state-v1"}),
                         ["-initial-assignment", "grhsim-terminal-state-v1"])
        for value in ("grhsim", "unknown", False, [], {}):
            with self.subTest(value=value):
                with self.assertRaisesRegex(ValueError, "initial_assignment"):
                    _compile_repcut_kwargs({"initial_assignment": value})
        for value in (-1, True, float("nan"), float("inf"), "0.05oops", [], {}):
            with self.subTest(value=value):
                with self.assertRaisesRegex(ValueError, "initial_core_imbalance_factor"):
                    _compile_repcut_kwargs({"initial_core_imbalance_factor": value})


if __name__ == "__main__":
    unittest.main()
