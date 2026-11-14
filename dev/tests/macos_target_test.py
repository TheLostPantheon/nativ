#!/usr/bin/env python3
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[2]
BUILD_PATH = ROOT / "runtime" / "build.py"
LAUNCHER_PATH = ROOT / "launcher" / "build.sh"
INSTALL_PATH = ROOT / "dev" / "macos-install.sh"
TARGET_PATH = ROOT / "runtime" / "platform" / "macos" / "target.sh"
SPEC = importlib.util.spec_from_file_location("nativ_runtime_build", BUILD_PATH)
BUILD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILD)


def executable(path, body):
    path.write_text(body)
    path.chmod(0o755)
    return path


def fake_compiler(path):
    return executable(path, f"""#!{sys.executable}
import fcntl
import json
import os
from pathlib import Path
import sys

args = sys.argv[1:]
with open(os.environ["NATIV_TEST_LOG"], "a") as log:
    fcntl.flock(log, fcntl.LOCK_EX)
    log.write(json.dumps(args) + "\\n")
if "-o" in args:
    out = Path(args[args.index("-o") + 1])
    out.parent.mkdir(parents=True, exist_ok=True)
    out.touch()
""")


class MacOSTargetTest(unittest.TestCase):
    def target(self):
        return BUILD.macos_target()

    def assert_target_flags(self, argv):
        architecture, minimum = self.target()
        self.assertIn("-arch", argv)
        self.assertEqual(argv[argv.index("-arch") + 1], architecture)
        self.assertIn(f"-mmacosx-version-min={minimum}", argv)
        self.assertIn("-Werror=unguarded-availability-new", argv)
        self.assertNotIn("-w", argv)
        self.assertIn("-Wno-everything", argv)
        self.assertGreater(argv.index("-Werror=unguarded-availability-new"), argv.index("-Wno-everything"))

    def test_python_parser_rejects_shell_syntax(self):
        with tempfile.TemporaryDirectory() as td:
            target = Path(td) / "target.sh"
            marker = Path(td) / "executed"
            target.write_text(f"NATIV_MACOS_MIN=$(touch {marker})\nNATIV_MACOS_ARCH=arm64\n")
            with self.assertRaises(ValueError):
                BUILD.macos_target(target)
            self.assertFalse(marker.exists())

    @unittest.skipUnless(sys.platform == "darwin", "macOS toolchain test")
    def test_runtime_build_propagates_target(self):
        with tempfile.TemporaryDirectory() as td:
            td = Path(td)
            log = td / "compiler.jsonl"
            fake_bin = td / "bin"
            fake_bin.mkdir()
            compiler = fake_compiler(fake_bin / "compiler")
            executable(fake_bin / "pkg-config", "#!/bin/sh\nexit 1\n")
            toolchain = td / "toolchain"
            (toolchain / "SDL3" / "include").mkdir(parents=True)
            (toolchain / "SDL3" / "lib").mkdir()
            (toolchain / "SDL3" / "lib" / "libSDL3.0.dylib").touch()
            base = td / "game"
            base.with_suffix(".c").write_text("int generated_guest_code;\n")
            out = td / "game-bin"
            env = dict(os.environ,
                       ATIV_MAC_TOOLCHAIN=str(toolchain),
                       CC=str(compiler),
                       CXX=str(compiler),
                       JOBS="1",
                       NATIV_TEST_LOG=str(log),
                       RECOMP_CPU="generic",
                       PATH=str(fake_bin) + os.pathsep + os.environ["PATH"])
            subprocess.run([sys.executable, BUILD_PATH, base, out], cwd=ROOT, env=env, check=True,
                           capture_output=True, text=True)
            commands = [json.loads(line) for line in log.read_text().splitlines()]
            c_compile = next(argv for argv in commands if "-c" in argv and any(arg.endswith(".c") for arg in argv))
            cxx_compile = next(argv for argv in commands if "-c" in argv and any(arg.endswith(".cpp") for arg in argv))
            link = next(argv for argv in commands if "-c" not in argv)
            for argv in (c_compile, cxx_compile, link):
                self.assert_target_flags(argv)
            architecture, minimum = self.target()
            cache = f"obj_mt_{architecture}_macos{minimum.replace('.', '_')}"
            outputs = [argv[argv.index("-o") + 1] for argv in commands if "-c" in argv]
            self.assertTrue(any(cache in output for output in outputs))

    @unittest.skipUnless(sys.platform == "darwin", "macOS toolchain test")
    def test_launcher_propagates_target_and_separates_cache(self):
        with tempfile.TemporaryDirectory() as td:
            td = Path(td)
            root = td / "root"
            (root / "launcher").mkdir(parents=True)
            (root / "runtime" / "platform" / "macos").mkdir(parents=True)
            shutil.copy2(LAUNCHER_PATH, root / "launcher" / "build.sh")
            shutil.copy2(TARGET_PATH, root / "runtime" / "platform" / "macos" / "target.sh")
            toolchain = td / "toolchain"
            (toolchain / "SDL3" / "include").mkdir(parents=True)
            (toolchain / "SDL3" / "lib").mkdir()
            (toolchain / "SDL3" / "lib" / "libSDL3.0.dylib").touch()
            fake_bin = td / "bin"
            fake_bin.mkdir()
            log = td / "compiler.jsonl"
            fake_compiler(fake_bin / "c++")
            executable(fake_bin / "pkg-config", "#!/bin/sh\nexit 0\n")
            out = td / "nativ"
            env = dict(os.environ,
                       ATIV_MAC_TOOLCHAIN=str(toolchain),
                       NATIV_TEST_LOG=str(log),
                       PATH=str(fake_bin) + os.pathsep + os.environ["PATH"])
            result = subprocess.run(["zsh", root / "launcher" / "build.sh", out], env=env,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            commands = [json.loads(line) for line in log.read_text().splitlines()]
            self.assertGreaterEqual(len(commands), 2)
            for argv in commands:
                self.assert_target_flags(argv)
            architecture, minimum = self.target()
            cache = f"obj_mt_{architecture}_macos{minimum.replace('.', '_')}"
            outputs = [argv[argv.index("-o") + 1] for argv in commands if "-c" in argv]
            self.assertTrue(any(cache in output for output in outputs))

    @unittest.skipUnless(sys.platform == "darwin", "macOS package test")
    def test_installer_writes_configured_minimum(self):
        with tempfile.TemporaryDirectory() as td:
            td = Path(td)
            root = td / "root"
            for directory in ("dev", "launcher", "pipeline", "assets/brand", "runtime/platform/macos"):
                (root / directory).mkdir(parents=True, exist_ok=True)
            shutil.copy2(INSTALL_PATH, root / "dev" / "macos-install.sh")
            shutil.copy2(TARGET_PATH, root / "runtime" / "platform" / "macos" / "target.sh")
            (root / "assets" / "brand" / "nativ.icns").touch()
            toolchain = td / "toolchain"
            for directory in ("SDL3/lib", "spirv-cross/lib", "ffmpeg"):
                (toolchain / directory).mkdir(parents=True)
            (toolchain / "SDL3" / "lib" / "libSDL3.0.dylib").touch()
            (toolchain / "spirv-cross" / "lib" / "libspirv-cross-c.a").touch()
            destination = td / "install"
            resources = destination / "Project Nativ.app" / "Contents" / "Resources"
            python = resources / "python" / "bin" / "python3"
            python.parent.mkdir(parents=True)
            executable(python, "#!/bin/sh\nif [ \"$1\" = -c ]; then echo 3.13.17; fi\n")
            (resources / "version.txt").write_text("v1.2.3\n")
            fake_bin = td / "bin"
            fake_bin.mkdir()
            fake_python = executable(fake_bin / "python3", "#!/bin/sh\nexit 0\n")
            executable(fake_bin / "zsh", """#!/bin/sh
for out in "$@"; do :; done
mkdir -p "$(dirname "$out")"
: > "$out"
""")
            env = dict(os.environ,
                       ATIV_MAC_TOOLCHAIN=str(toolchain),
                       PYTHON=str(fake_python),
                       PATH=str(fake_bin) + os.pathsep + os.environ["PATH"])
            subprocess.run(["bash", root / "dev" / "macos-install.sh", destination], env=env, check=True,
                           capture_output=True, text=True)
            with open(destination / "Project Nativ.app" / "Contents" / "Info.plist", "rb") as f:
                info = plistlib.load(f)
            self.assertEqual(info["LSMinimumSystemVersion"], self.target()[1])

    @unittest.skipUnless(sys.platform == "darwin", "Mach-O metadata test")
    def test_compiler_emits_target_metadata_and_rejects_newer_api(self):
        for command in ("clang", "lipo", "vtool"):
            if not shutil.which(command): self.skipTest(f"{command} is not installed")
        architecture, minimum = self.target()
        flags = ["-Wno-everything", "-arch", architecture, f"-mmacosx-version-min={minimum}",
                 "-Werror=unguarded-availability-new"]
        with tempfile.TemporaryDirectory() as td:
            td = Path(td)
            source = td / "fixture.c"
            binary = td / "fixture"
            source.write_text("int main(void) { return 0; }\n")
            subprocess.run(["clang", *flags, source, "-o", binary], check=True)
            archs = subprocess.run(["lipo", "-archs", binary], check=True, capture_output=True, text=True).stdout.split()
            self.assertEqual(archs, [architecture])
            metadata = subprocess.run(["vtool", "-show-build", binary], check=True,
                                      capture_output=True, text=True).stdout
            self.assertRegex(metadata, rf"\bminos {re.escape(minimum)}\b")
            source.write_text(textwrap.dedent("""
                extern void newer_api(void) __attribute__((availability(macos, introduced=12.0)));
                void call_newer_api(void) { newer_api(); }
            """))
            result = subprocess.run(["clang", *flags, "-c", source, "-o", td / "newer.o"],
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unguarded-availability-new", result.stderr)


if __name__ == "__main__":
    unittest.main()
