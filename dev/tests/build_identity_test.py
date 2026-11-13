#!/usr/bin/env python3
"""Play-build identity and `vitarecomp ready` compatibility checks, without a game or a compile.

    python3 dev/tests/build_identity_test.py
"""
import importlib.util
import json
import os
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def load_runtime_build():
    path = os.path.join(ROOT, "runtime", "build.py")
    spec = importlib.util.spec_from_file_location("nativ_runtime_build", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


RUNTIME_BUILD = load_runtime_build()
sys.path.insert(0, os.path.join(ROOT, "pipeline"))
import vitarecomp as VR  # noqa: E402


def mac_identity(mode, target_cpu):
    target = {
        "cc": "clang",
        "cpu": mode,
        "cpu_flags": [] if mode == "generic" else ["-mcpu=native"],
        "extra": [],
        "target_os": "macos",
    }
    with mock.patch.object(RUNTIME_BUILD, "compiler_probe",
                           return_value=("arm64-apple-macosx11.0.0", target_cpu)):
        return RUNTIME_BUILD.build_identity(target, {})


class BuildIdentityTest(unittest.TestCase):
    def ready(self, built, current):
        tmp = tempfile.TemporaryDirectory()
        out = os.path.join(tmp.name, "recomp")
        elf = os.path.join(tmp.name, "elf")
        os.makedirs(out)
        os.makedirs(elf)
        open(os.path.join(elf, "eboot.bin"), "wb").close()
        open(os.path.join(out, "game"), "wb").close()
        with open(os.path.join(out, ".recompile_inputs"), "w", encoding="utf-8") as f: f.write("inputs\n")
        with open(os.path.join(out, ".built_version"), "w", encoding="utf-8") as f: f.write("tools\n")
        if built is not None:
            with open(os.path.join(out, VR.BUILD_IDENTITY_STAMP), "w", encoding="utf-8") as f: json.dump(built, f)
        g = SimpleNamespace(OUT=out, W=tmp.name, ELF="eboot.bin", EXE="game", T="PCSZ00042")
        run = mock.Mock()
        say = mock.Mock()
        patches = (
            mock.patch.object(VR, "recompile_inputs", return_value="inputs"),
            mock.patch.object(VR, "tools_version", return_value="tools"),
            mock.patch.object(VR, "play_build_identity", return_value=current),
            mock.patch.object(VR, "run", run),
            mock.patch.object(VR, "say", say),
        )
        for patch in patches: patch.start()
        try: VR.cmd_ready(g)
        finally:
            for patch in reversed(patches): patch.stop()
            tmp.cleanup()
        return run, [str(call.args[0]) for call in say.call_args_list]

    def test_native_m5_build_rebuilds_on_m1(self):
        built_on_m5 = mac_identity("native", "apple-m4")
        opened_on_m1 = mac_identity("native", "apple-m1")
        self.assertEqual({
            "os", "architecture", "cpu_mode", "requested_cpu", "target_cpu",
            "compiler_target", "deployment_target",
        }, set(built_on_m5) - {"schema"})
        self.assertEqual("11.0.0", built_on_m5["deployment_target"])
        run, messages = self.ready(built_on_m5, opened_on_m1)
        run.assert_called_once_with(VR.SELF + ["build", "PCSZ00042"])
        self.assertTrue(any("build target changed" in message for message in messages))
        self.assertFalse(any("up to date" in message for message in messages))

    def test_generic_build_is_current_on_m5_and_m1(self):
        built_on_m5 = mac_identity("generic", "apple-m1")
        for opened_on in ("M5", "M1"):
            with self.subTest(opened_on=opened_on):
                current = mac_identity("generic", "apple-m1")
                run, messages = self.ready(built_on_m5, current)
                run.assert_not_called()
                self.assertTrue(any("up to date" in message for message in messages))

    def test_existing_build_without_identity_rebuilds(self):
        current = mac_identity("native", "apple-m4")
        run, messages = self.ready(None, current)
        run.assert_called_once_with(VR.SELF + ["build", "PCSZ00042"])
        self.assertTrue(any("build target changed" in message for message in messages))

    def test_build_stamps_are_written_only_after_success(self):
        identity = mac_identity("native", "apple-m4")
        g = SimpleNamespace(RUN_ENV="", GAME_ADDRS="", PACK="PCSZ00042", OUT="", BASE="game",
                            EXE="game", windows=False, MODS=[])
        for succeeds in (False, True):
            with self.subTest(succeeds=succeeds), tempfile.TemporaryDirectory() as tmp:
                g.OUT = tmp
                run = mock.Mock(side_effect=None if succeeds else SystemExit(2))
                with mock.patch.object(VR, "PACKS", tmp), \
                     mock.patch.object(VR, "play_build_identity", return_value=identity), \
                     mock.patch.object(VR, "tools_version", return_value="tools"), \
                     mock.patch.object(VR, "run", run):
                    if succeeds: VR.cmd_build(g, "")
                    else:
                        with self.assertRaises(SystemExit): VR.cmd_build(g, "")
                version = os.path.join(tmp, ".built_version")
                target = os.path.join(tmp, VR.BUILD_IDENTITY_STAMP)
                self.assertEqual(succeeds, os.path.isfile(version))
                self.assertEqual(succeeds, os.path.isfile(target))
                if succeeds:
                    with open(target, encoding="utf-8") as f: self.assertEqual(identity, json.load(f))


if __name__ == "__main__":
    unittest.main()
