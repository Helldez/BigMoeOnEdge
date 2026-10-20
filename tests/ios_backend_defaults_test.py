#!/usr/bin/env python3
"""Configure the real project and dependency to check iOS backend policy; no device is used."""

from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == "darwin", "Apple SDK configure checks require macOS")
class BackendDefaultsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not (SOURCE / "third_party/llama.cpp/CMakeLists.txt").is_file():
            raise RuntimeError("initialize the llama.cpp submodule before running the configure checks")
        cls.temporary = tempfile.TemporaryDirectory(prefix="bmoe-backend-defaults-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.build_root = Path(cls.temporary.name)

    def configure(self, name, sdk=None, blas=None):
        command = ["cmake", "-S", str(SOURCE), "-B", str(self.build_root / name), "-G", "Xcode",
                   "-DBMOE_BUILD_TESTS=OFF", "-DBMOE_BUILD_CLI=OFF", "-DBMOE_BUILD_SERVER=OFF",
                   "-DGGML_NATIVE=OFF", "-DGGML_OPENMP=OFF", "-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO"]
        if sdk:
            arch = "arm64" if sdk == "iphoneos" else platform.machine()
            command += ["-DCMAKE_SYSTEM_NAME=iOS", "-DCMAKE_OSX_SYSROOT=" + sdk,
                        "-DCMAKE_OSX_ARCHITECTURES=" + arch, "-DCMAKE_OSX_DEPLOYMENT_TARGET=16.4"]
        if blas is not None:
            command.append("-DGGML_BLAS=" + blas)
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(result.returncode, 0, result.stdout)
        cache = {}
        for line in (self.build_root / name / "CMakeCache.txt").read_text().splitlines():
            if line and not line.startswith(("#", "//")) and "=" in line and ":" in line:
                key, value = line.split("=", 1)
                cache[key.split(":", 1)[0]] = value
        return cache

    def check_backends(self, cache, blas):
        self.assertEqual(cache["GGML_BLAS"], blas)
        self.assertEqual(cache["GGML_ACCELERATE"], "ON")
        self.assertEqual(cache["GGML_METAL"], "ON")
        backends = cache["GGML_AVAILABLE_BACKENDS"].split(";")
        self.assertEqual("ggml-blas" in backends, blas == "ON")
        self.assertIn("ggml-metal", backends)
        self.assertIn("ggml-cpu", backends)

    def test_device_default(self):
        self.check_backends(self.configure("device-default", "iphoneos"), "OFF")

    def test_simulator_default(self):
        self.check_backends(self.configure("sim-default", "iphonesimulator"), "OFF")

    def test_explicit_and_cached_on(self):
        self.check_backends(self.configure("sim-opt-in", "iphonesimulator", "ON"), "ON")
        self.check_backends(self.configure("sim-opt-in", "iphonesimulator"), "ON")

    def test_macos_default_unchanged(self):
        self.check_backends(self.configure("mac-default"), "ON")


if __name__ == "__main__":
    unittest.main()
