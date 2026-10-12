#!/usr/bin/env python3

import inspect
import types
import unittest
from unittest.mock import patch

import wolvrix


class DummySession:
    _capsule = object()

    def _ensure_open(self):
        pass

    def _complete_action(self, name, diagnostics, **kwargs):
        return name, diagnostics, kwargs


class EmitGrhsimRepCutPackageTest(unittest.TestCase):
    def test_public_interface_and_forwarding(self):
        calls = []

        def fake_emit(*args, **kwargs):
            calls.append((args, kwargs))
            return True, []

        with patch.object(
            wolvrix, "_native",
            types.SimpleNamespace(session_emit_grhsim_repcut_package=fake_emit),
        ):
            result = wolvrix.Session.emit_grhsim_repcut_package(
                DummySession(), design="design.main", output="package", top=["SimTop"]
            )
        self.assertEqual(calls, [((DummySession._capsule,), {
            "design": "design.main", "output": "package", "top": ["SimTop"],
        })])
        self.assertEqual(result[0], "emit_grhsim_repcut_package")
        self.assertTrue(result[2]["success"])
        for name in ("design", "output", "top"):
            parameter = inspect.signature(
                wolvrix.Session.emit_grhsim_repcut_package
            ).parameters[name]
            self.assertIs(parameter.kind, inspect.Parameter.KEYWORD_ONLY)

    def test_python_rejects_unsupported_options(self):
        for kwargs in ({"waveform": "off"}, {"perf": "eval"}, {"backend": "verilator"}):
            with self.subTest(kwargs=kwargs):
                with self.assertRaises(TypeError):
                    wolvrix.Session.emit_grhsim_repcut_package(
                        DummySession(), design="design.main", output="package", **kwargs
                    )

    def test_native_rejects_unsupported_options(self):
        for kwargs in ({"waveform": "off"}, {"perf": "eval"}, {"backend": "verilator"}):
            with self.subTest(kwargs=kwargs):
                with self.assertRaises(TypeError):
                    wolvrix._native.session_emit_grhsim_repcut_package(
                        None, design="design.main", output="package", **kwargs
                    )

    def test_closed_session_rejected(self):
        session = wolvrix.Session()
        session.close()
        with self.assertRaises(RuntimeError):
            session.emit_grhsim_repcut_package(design="design.main", output="package")


if __name__ == "__main__":
    unittest.main()
