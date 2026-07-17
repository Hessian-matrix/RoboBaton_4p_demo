#!/usr/bin/env python3
"""package_runtime.sh one-click build and transactional publish regression tests."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[1]
PACKAGE_SCRIPT = ROOT / "scripts/package_runtime.sh"


def write_executable(path: Path, content: str) -> None:
    path.write_text(textwrap.dedent(content).lstrip(), encoding="utf-8")
    path.chmod(0o755)


class PackageRuntimeScriptTest(unittest.TestCase):
    def make_fixture(self) -> tuple[Path, Path, Path]:
        """构造隔离工程，不触碰真实仓库输出。"""
        temporary = Path(tempfile.mkdtemp(prefix="package-runtime-script-test-", dir="/tmp"))
        self.addCleanup(shutil.rmtree, temporary, True)
        project = temporary / "project"
        scripts = project / "scripts"
        scripts.mkdir(parents=True)
        shutil.copy2(PACKAGE_SCRIPT, scripts / "package_runtime.sh")
        workspace = project / "workspace"
        workspace_scripts = workspace / "scripts"
        workspace_scripts.mkdir(parents=True)
        (workspace / "CMakeLists.txt").write_text("# fixture\n", encoding="utf-8")
        toolchain_bin = project / "fixture-toolchain" / "bin"
        toolchain_bin.mkdir(parents=True)
        write_executable(
            toolchain_bin / "aarch64-fixture-linux-gnu-gcc",
            "#!/usr/bin/env bash\n"
            "[[ \"${1:-}\" == '-dumpmachine' ]] || exit 2\n"
            "printf '%s\\n' aarch64-fixture-linux-gnu\n",
        )
        write_executable(toolchain_bin / "aarch64-fixture-linux-gnu-strip", "#!/usr/bin/env bash\nexit 0\n")
        for script_name, library in (
            ("build_sc132.sh", "sc132"),
            ("build_rtsp_so_mp4.sh", "prrtsp"),
        ):
            write_executable(
                workspace_scripts / script_name,
                f"""
                #!/usr/bin/env bash
                set -Eeuo pipefail
                # 2026-07-17 修改原因：记录 wrapper 实参，防止 producer 悄悄回退到各自硬编码工具链。
                printf 'producer-{library} %s\n' "$*" >> "{project}/build-calls.log"
                for suffix in .2.0.0 .2 ''; do
                  printf '%s\n' fresh-{library} > "${{PACKAGE_LIB_DIR}}/lib{library}.so${{suffix}}"
                done
                """,
            )

        write_executable(
            scripts / "verify_runtime_package.py",
            "#!/usr/bin/env python3\n"
            "from pathlib import Path\n"
            "import os\n"
            "import sys\n\n"
            "if os.environ.get('PACKAGE_TEST_FAIL_VERIFY') == '1':\n"
            "    raise SystemExit(23)\n"
            "root = Path(sys.argv[-1])\n"
            "required = [\n"
            "    'bin/cam_demo', 'bin/imu_reader_demo', 'bin/serial_port_demo',\n"
            "    'cam_demo', 'imu_reader_demo', 'serial_port_demo', 'env.sh',\n"
            "]\n"
            "required.extend(\n"
            "    f'lib/lib{name}.so{suffix}'\n"
            "    for name in ('icm42688', 'sc132', 'prrtsp')\n"
            "    for suffix in ('.2.0.0', '.2', '')\n"
            ")\n"
            "missing = [name for name in required if not (root / name).is_file()]\n"
            "if missing:\n"
            "    print(f'missing fixture package files: {missing}', file=sys.stderr)\n"
            "    raise SystemExit(24)\n"
            "if '--write-manifest' in sys.argv:\n"
            "    (root / 'manifest.sha256').write_text('fixture-manifest\\n', encoding='utf-8')\n",
        )

        return project, project / "build_x5", project / "demo"

    def run_package(self, project: Path, build: Path, output: Path, *, fail_verify: bool = False,
                    strip_tool: Path | None = None) -> subprocess.CompletedProcess[str]:
        """执行真实 shell 入口，仅替换外部构建和 verifier。"""
        env = dict(os.environ)
        env.update({
            "BUILD_DIR": str(build),
            "PACKAGE_LIB_DIR": str(project / "lib"),
            "OUTPUT_DIR": str(output),
            "TOOLCHAIN_FILE": "/tmp/package-runtime-fixture-toolchain.cmake",
            "PATH": f"{project / 'fake-bin'}:{env.get('PATH', '')}",
            "WORKSPACE_DIR": str(project / "workspace"),
        })
        if strip_tool is not None:
            env["STRIP_TOOL"] = str(strip_tool)
        if fail_verify:
            env["PACKAGE_TEST_FAIL_VERIFY"] = "1"
        return subprocess.run(
            [str(project / "scripts/package_runtime.sh")],
            cwd=project,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )

    def install_fake_cmake(self, project: Path) -> None:
        """替换 CMake，模拟 ICM producer 与 consumer 的干净全目标构建。"""
        fake_bin = project / "fake-bin"
        fake_bin.mkdir()
        write_executable(
            fake_bin / "cmake",
            f"""
            #!/usr/bin/env bash
            set -Eeuo pipefail
            if [[ "$1" == "-S" ]]; then
              source_dir="$2"
              build_dir="$4"
              mkdir -p "${{build_dir}}"
              if [[ "${{source_dir}}" == "{project}/workspace" ]]; then
                printf '%s\n' configure-icm >> "{project}/build-calls.log"
                # 2026-07-17 修改原因：模拟 CMake 持久化的真实编译器路径，供打包入口统一 producer 工具链。
                mkdir -p "${{build_dir}}/CMakeFiles/fixture"
                printf '%s\n' 'set(CMAKE_C_COMPILER "{project}/fixture-toolchain/bin/aarch64-fixture-linux-gnu-gcc")' \
                  > "${{build_dir}}/CMakeFiles/fixture/CMakeCCompiler.cmake"
              else
                printf '%s\n' configure-consumers >> "{project}/build-calls.log"
              fi
              exit 0
            fi
            if [[ "$1" == "--build" ]]; then
              build_dir="$2"
              if [[ "${{build_dir}}" == *.package-build-icm42688 ]]; then
                for suffix in .2.0.0 .2 ''; do
                  printf '%s\n' fresh-icm42688 > "{project}/lib/libicm42688.so${{suffix}}"
                done
                printf '%s\n' producer-icm42688 >> "{project}/build-calls.log"
                exit 0
              fi
              for target in cam_demo imu_reader_demo serial_port_demo; do
                printf '%s\n' "${{target}}" > "${{build_dir}}/${{target}}"
                chmod 755 "${{build_dir}}/${{target}}"
              done
              printf '%s\n' consumers-all >> "{project}/build-calls.log"
              exit 0
            fi
            exit 2
            """,
        )

    def test_default_command_builds_all_targets_and_publishes_complete_package(self) -> None:
        """默认命令构建三个目标并一次发布完整运行包。"""
        project, build, output = self.make_fixture()
        self.install_fake_cmake(project)
        build.mkdir()
        stale_object = build / "stale-object.o"
        stale_object.write_text("must-be-removed\n", encoding="utf-8")

        result = self.run_package(project, build, output)

        self.assertEqual(result.returncode, 0, result.stdout)
        toolchain_prefix = project / "fixture-toolchain/bin/aarch64-fixture-linux-gnu-"
        self.assertEqual(
            (project / "build-calls.log").read_text(encoding="utf-8").splitlines(),
            ["configure-icm", "producer-icm42688",
             f"producer-sc132 --gcc {toolchain_prefix}gcc --strip {toolchain_prefix}strip --clean-first",
             f"producer-prrtsp --cross-compile {toolchain_prefix} --clean-first",
             "configure-consumers", "consumers-all"],
        )
        self.assertFalse(stale_object.exists(), "package build reused stale build output")
        for library in ("icm42688", "sc132", "prrtsp"):
            packaged = output / f"lib/lib{library}.so.2.0.0"
            self.assertTrue(packaged.read_text(encoding="utf-8").startswith("fresh-"))
        expected = {
            "bin", "lib", "bin/cam_demo", "bin/imu_reader_demo", "bin/serial_port_demo",
            "cam_demo", "imu_reader_demo", "serial_port_demo", "env.sh", "manifest.sha256",
        }
        expected.update(
            f"lib/lib{name}.so{suffix}"
            for name in ("icm42688", "sc132", "prrtsp")
            for suffix in (".2.0.0", ".2", "")
        )
        actual = {path.relative_to(output).as_posix() for path in output.rglob("*")}
        self.assertEqual(actual, expected)
        self.assertEqual(stat.S_IMODE((output / "cam_demo").stat().st_mode), 0o755)
        self.assertEqual(stat.S_IMODE((output / "env.sh").stat().st_mode), 0o644)

    def test_custom_stage_strip_does_not_change_producer_toolchain(self) -> None:
        """显式 stage strip 不得替换 SC132/RTSP 的同 triplet producer 工具。"""
        project, build, output = self.make_fixture()
        self.install_fake_cmake(project)
        strip_log = project / "stage-strip.log"
        custom_strip = project / "custom-stage-strip"
        write_executable(
            custom_strip,
            f"#!/usr/bin/env bash\nprintf '%s\\n' \"$*\" >> '{strip_log}'\n",
        )

        result = self.run_package(project, build, output, strip_tool=custom_strip)

        self.assertEqual(result.returncode, 0, result.stdout)
        toolchain_prefix = project / "fixture-toolchain/bin/aarch64-fixture-linux-gnu-"
        self.assertIn(
            f"producer-sc132 --gcc {toolchain_prefix}gcc --strip {toolchain_prefix}strip --clean-first",
            (project / "build-calls.log").read_text(encoding="utf-8").splitlines(),
        )
        stripped_names = {
            Path(line.split()[-1]).name
            for line in strip_log.read_text(encoding="utf-8").splitlines()
        }
        self.assertEqual(stripped_names, {"cam_demo", "imu_reader_demo", "serial_port_demo"})

    def test_failed_verification_preserves_previous_output(self) -> None:
        """stage 验证失败不得覆盖上一版运行包。"""
        project, build, output = self.make_fixture()
        self.install_fake_cmake(project)
        # 预置 build 产物以覆盖发布失败的事务语义。
        build.mkdir()
        for target in ("cam_demo", "imu_reader_demo", "serial_port_demo"):
            binary = build / target
            binary.write_text(f"prebuilt-{target}\n", encoding="utf-8")
            binary.chmod(0o755)
        output.mkdir()
        sentinel = output / "previous-package.txt"
        sentinel.write_text("keep-me\n", encoding="utf-8")

        result = self.run_package(project, build, output, fail_verify=True)

        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertTrue(sentinel.is_file(), result.stdout)
        self.assertEqual(sentinel.read_text(encoding="utf-8"), "keep-me\n")
        self.assertEqual({path.name for path in output.iterdir()}, {"previous-package.txt"})

    def test_rejects_toolchain_without_companion_strip(self) -> None:
        """CMake 编译器旁缺少目标 triplet strip 时必须在 producer 构建前失败。"""
        project, build, output = self.make_fixture()
        self.install_fake_cmake(project)
        (project / "fixture-toolchain/bin/aarch64-fixture-linux-gnu-strip").unlink()

        result = self.run_package(project, build, output)

        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("Missing companion strip for configured C compiler", result.stdout)
        self.assertEqual(
            (project / "build-calls.log").read_text(encoding="utf-8").splitlines(),
            ["configure-icm"],
        )

    def test_rejects_build_directory_outside_project(self) -> None:
        """外部 build 路径必须在任何删除动作前被拒绝。"""
        project, _, output = self.make_fixture()
        self.install_fake_cmake(project)
        outside = project.parent / "outside-build"
        outside.mkdir()
        sentinel = outside / "keep-me"
        sentinel.write_text("keep\n", encoding="utf-8")

        result = self.run_package(project, outside, output)

        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("Refusing build directory outside project", result.stdout)
        self.assertTrue(sentinel.is_file(), "unsafe build path was deleted")

if __name__ == "__main__":
    unittest.main(verbosity=2)
