#!/usr/bin/env python3

import types
import unittest

import wolvrix
from wolvrix import _compile_emit_grhsim_cpp_kwargs
from wolvrix import _wolvrix as native


class DummySession:
    _capsule = object()

    def _ensure_open(self) -> None:
        pass

    def _complete_action(self, _name, _diagnostics, **_kwargs):
        return None


class EmitGrhsimCppOptionTest(unittest.TestCase):
    def test_python_none_is_omitted_and_false_is_forwarded(self) -> None:
        calls = []

        def fake_emit(*args, **kwargs):
            calls.append((args, kwargs))
            return True, []

        original_native = wolvrix._native
        wolvrix._native = types.SimpleNamespace(session_emit_grhsim_cpp=fake_emit)
        try:
            wolvrix.Session.emit_grhsim_cpp(
                DummySession(),
                design="design.main",
                output="unused",
                direct_single_writer_state_reads=None,
                pure_event_compute_word_bypass=None,
                active_mask_gap_pack_policy=None,
            )
            self.assertNotIn("direct_single_writer_state_reads", calls[-1][1])
            self.assertNotIn("pure_event_compute_word_bypass", calls[-1][1])
            self.assertNotIn("active_mask_gap_pack_policy", calls[-1][1])

            wolvrix.Session.emit_grhsim_cpp(
                DummySession(),
                design="design.main",
                output="unused",
                direct_single_writer_state_reads=False,
                pure_event_compute_word_bypass=False,
                active_mask_gap_pack_policy="probe",
            )
            self.assertIs(calls[-1][1]["direct_single_writer_state_reads"], False)
            self.assertIs(calls[-1][1]["pure_event_compute_word_bypass"], False)
            self.assertEqual(calls[-1][1]["active_mask_gap_pack_policy"], "probe")
        finally:
            wolvrix._native = original_native

    def test_python_validation_rejects_non_bool(self) -> None:
        _compile_emit_grhsim_cpp_kwargs({"direct_single_writer_state_reads": True})
        _compile_emit_grhsim_cpp_kwargs({"direct_single_writer_state_reads": False})
        with self.assertRaises(ValueError):
            _compile_emit_grhsim_cpp_kwargs({"direct_single_writer_state_reads": 1})

    def test_python_active_mask_gap_pack_policy_validation(self) -> None:
        _compile_emit_grhsim_cpp_kwargs({"active_mask_gap_pack_policy": "off"})
        _compile_emit_grhsim_cpp_kwargs({"active_mask_gap_pack_policy": "probe"})
        for value in ("", "targeted", " probe ", False, 1):
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    _compile_emit_grhsim_cpp_kwargs({"active_mask_gap_pack_policy": value})

    def test_native_keyword_is_accepted(self) -> None:
        with wolvrix.Session() as session:
            with self.assertRaisesRegex(KeyError, "design key not found"):
                native.session_emit_grhsim_cpp(
                    session._capsule,
                    design="missing.design",
                    output="unused",
                    top=[],
                    direct_single_writer_state_reads=False,
                    active_mask_gap_pack_policy="probe",
                )

    def test_native_active_mask_gap_pack_policy_rejects_invalid_value(self) -> None:
        with wolvrix.Session() as session:
            with self.assertRaisesRegex(ValueError, "active_mask_gap_pack_policy"):
                native.session_emit_grhsim_cpp(
                    session._capsule,
                    design="missing.design",
                    output="unused",
                    active_mask_gap_pack_policy="targeted",
                )

    def test_native_active_mask_gap_pack_policy_accepts_explicit_none(self) -> None:
        with wolvrix.Session() as session:
            with self.assertRaisesRegex(KeyError, "design key not found"):
                native.session_emit_grhsim_cpp(
                    session._capsule,
                    design="missing.design",
                    output="unused",
                    active_mask_gap_pack_policy=None,
                )

    def test_native_new_positional_argument_is_appended(self) -> None:
        with wolvrix.Session() as session:
            legacy_args = (
                session._capsule,
                "missing.design",
                "unused",
                [],
                None,
                None,
                None,
                None,
                None,
                None,
                "off",
                "off",
                None,
                None,
                None,
                None,
                None,
                "off",
                None,
                None,
                False,
            )
            with self.assertRaisesRegex(KeyError, "design key not found"):
                native.session_emit_grhsim_cpp(*legacy_args)
            with self.assertRaisesRegex(KeyError, "design key not found"):
                native.session_emit_grhsim_cpp(*legacy_args, "probe")

        self.assertIn(
            "direct_single_writer_state_reads=None, active_mask_gap_pack_policy=None",
            native.session_emit_grhsim_cpp.__doc__,
        )


if __name__ == "__main__":
    unittest.main()
