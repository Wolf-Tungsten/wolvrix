#!/usr/bin/env python3

import unittest

from wolvrix import _compile_repcut_kwargs


class RepcutOptionTest(unittest.TestCase):
    def test_cost_model_is_sparse_and_forwarded(self) -> None:
        self.assertEqual(_compile_repcut_kwargs({}), [])
        self.assertEqual(_compile_repcut_kwargs({"cost_model": None}), [])
        for value in ("legacy", "verilator-v1"):
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
                    ValueError, "cost_model must be one of: legacy, verilator-v1"
                ):
                    _compile_repcut_kwargs({"cost_model": value})

    def test_cost_model_and_weight_mode_are_independent(self) -> None:
        for cost_model in ("legacy", "verilator-v1"):
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

    def test_unknown_options_remain_strictly_rejected(self) -> None:
        with self.assertRaisesRegex(TypeError, "unknown_weight_option"):
            _compile_repcut_kwargs({"unknown_weight_option": "baseline"})


if __name__ == "__main__":
    unittest.main()
