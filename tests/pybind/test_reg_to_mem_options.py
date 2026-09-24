#!/usr/bin/env python3

import unittest

from wolvrix import _compile_reg_to_mem_kwargs, _compile_run_pass


class RegToMemOptionTest(unittest.TestCase):
    def test_default_does_not_change_pass_arguments(self) -> None:
        self.assertEqual(_compile_reg_to_mem_kwargs({}), [])
        self.assertEqual(_compile_reg_to_mem_kwargs({"profitability_filter": None}), [])
        self.assertEqual(_compile_run_pass("reg_to_mem", [], {}), ("reg-to-mem", []))

    def test_profitability_filter_is_forwarded(self) -> None:
        for value in ("none", "verilator-v1"):
            with self.subTest(value=value):
                expected = ["-profitability-filter", value]
                self.assertEqual(
                    _compile_reg_to_mem_kwargs({"profitability_filter": value}),
                    expected,
                )
                self.assertEqual(
                    _compile_run_pass("reg-to-mem", [], {"profitability_filter": value}),
                    ("reg-to-mem", expected),
                )

    def test_profitability_filter_coexists_with_existing_options(self) -> None:
        self.assertEqual(
            _compile_reg_to_mem_kwargs(
                {
                    "ordered_writes": True,
                    "min_element_count": 32,
                    "profitability_filter": "verilator-v1",
                }
            ),
            [
                "-ordered-writes",
                "-min-element-count",
                "32",
                "-profitability-filter",
                "verilator-v1",
            ],
        )

    def test_profitability_filter_is_strictly_validated(self) -> None:
        for value in (
            "", "verilator", "verilator_v1", "Verilator-v1", " none", "none ",
            False, 1, [], {},
        ):
            with self.subTest(value=value):
                with self.assertRaisesRegex(ValueError, "profitability_filter"):
                    _compile_reg_to_mem_kwargs({"profitability_filter": value})

    def test_unknown_named_options_remain_rejected(self) -> None:
        with self.assertRaisesRegex(TypeError, "unknown_filter"):
            _compile_reg_to_mem_kwargs({"unknown_filter": "verilator-v1"})


if __name__ == "__main__":
    unittest.main()
