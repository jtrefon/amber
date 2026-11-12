"""Tests for the apt install script behind .github/actions/install-deps.

The script is plain bash so the pinned container jobs can run it. These tests
put fake `sudo`, `timeout` and `apt-get` binaries first on PATH, which lets them
simulate a DNS outage without touching the network or the host's package
database.
"""

import os
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

from harness import REPO_ROOT

SCRIPT = Path(REPO_ROOT) / ".github" / "actions" / "install-deps" / "install-deps.sh"

FAKE_SUDO = """#!/usr/bin/env bash
exec "$@"
"""

FAKE_TIMEOUT = """#!/usr/bin/env bash
# usage: timeout [options] duration command...
while [ $# -gt 0 ] && [[ "$1" == -* ]]; do
    shift
done
shift
exec "$@"
"""

FAKE_APT = r"""#!/usr/bin/env bash
set -u
printf 'apt-get %s\n' "$*" >> "$FAKE_APT_LOG"

command=""
for arg in "$@"; do
    case "$arg" in
        update|install) command="$arg"; break ;;
    esac
done

if [ "$command" = "update" ]; then
    count=0
    if [ -f "$FAKE_APT_STATE/updates" ]; then
        count="$(cat "$FAKE_APT_STATE/updates")"
    fi
    count=$((count + 1))
    printf '%s\n' "$count" > "$FAKE_APT_STATE/updates"

    case "$*" in
        *fallback-*.list*)
            if [ "${FAKE_APT_FALLBACK_OK:-0}" = "1" ]; then
                exit 0
            fi
            echo "Err:1 http://mirrors.edge.kernel.org/ubuntu noble InRelease" >&2
            echo "  Temporary failure resolving 'mirrors.edge.kernel.org'" >&2
            exit 100
            ;;
    esac

    if [ "${FAKE_APT_FAIL_UPDATES:-0}" != "0" ] &&
        [ "$count" -le "${FAKE_APT_FAIL_UPDATES}" ]; then
        echo "Err:1 http://archive.ubuntu.com/ubuntu noble InRelease" >&2
        echo "  Temporary failure resolving 'archive.ubuntu.com'" >&2
        exit 100
    fi
    exit 0
fi

if [ "$command" = "install" ]; then
    case "$*" in
        *--no-download*)
            if [ "${FAKE_APT_OFFLINE_OK:-0}" = "1" ]; then
                exit 0
            fi
            echo "E: Unable to fetch some archives, maybe run apt-get update" >&2
            echo "  Temporary failure resolving 'archive.ubuntu.com'" >&2
            exit 100
            ;;
    esac
    if [ "${FAKE_APT_MISSING_PACKAGE:-0}" = "1" ]; then
        echo "E: Unable to locate package made-up" >&2
        exit 100
    fi
    if [ "${FAKE_APT_INSTALL_FAILS:-0}" = "1" ]; then
        exit 100
    fi
    exit 0
fi

exit 0
"""


class InstallDeps(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="amber-apt-test-")
        self.root = Path(self._tmp.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self._write_executable("sudo", FAKE_SUDO)
        self._write_executable("timeout", FAKE_TIMEOUT)
        self._write_executable("apt-get", FAKE_APT)
        self.log = self.root / "apt.log"
        self.state = self.root / "state"
        self.state.mkdir()
        sources = self.root / "primary.sources"
        sources.write_text(
            "Types: deb\nURIs: http://archive.ubuntu.com/ubuntu\n"
            "Suites: noble\nComponents: main universe\n",
            encoding="utf-8",
        )
        self.env = {
            "PATH": f"{self.bin}:{os.environ['PATH']}",
            "HOME": str(self.root),
            "FAKE_APT_LOG": str(self.log),
            "FAKE_APT_STATE": str(self.state),
            "AMBER_APT_PRIMARY_SOURCES": str(sources),
            "AMBER_APT_FALLBACKS": "http://mirrors.edge.kernel.org/ubuntu",
            "AMBER_APT_CACHE_DIR": str(self.root / "cache"),
            "AMBER_APT_ATTEMPTS": "3",
            "AMBER_APT_BACKOFF_SECONDS": "0",
            "AMBER_APT_UPDATE_TIMEOUT": "5",
            "AMBER_APT_UPDATE_BUDGET": "5",
            "AMBER_APT_INSTALL_BUDGET": "5",
            "AMBER_APT_OFFLINE_TIMEOUT": "5",
            "AMBER_APT_PROBE_TIMEOUT": "5",
        }

    def tearDown(self):
        self._tmp.cleanup()

    def _write_executable(self, name, content):
        path = self.bin / name
        path.write_text(content, encoding="utf-8")
        path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    def run_script(self, *args, **env):
        merged = dict(self.env)
        merged.update(env)
        return subprocess.run(
            ["bash", str(SCRIPT), *args],
            env=merged,
            text=True,
            capture_output=True,
            timeout=60,
            check=False,
        )

    def update_count(self):
        path = self.state / "updates"
        if not path.exists():
            return 0
        return int(path.read_text(encoding="utf-8").strip())

    def test_script_parses(self):
        proc = subprocess.run(
            ["bash", "-n", str(SCRIPT)], text=True, capture_output=True, check=False
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_a_transient_failure_is_retried_until_it_succeeds(self):
        proc = self.run_script("--cpp-toolchain", "false", FAKE_APT_FAIL_UPDATES="2")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("retrying in 0s", proc.stderr)
        self.assertEqual(self.update_count(), 3)
        self.assertIn("install-deps: installed:", proc.stdout)

    def test_a_dead_primary_uses_the_fallback_mirror(self):
        proc = self.run_script(
            "--cpp-toolchain",
            "false",
            FAKE_APT_FAIL_UPDATES="99",
            FAKE_APT_FALLBACK_OK="1",
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("fallback http://mirrors.edge.kernel.org/ubuntu", proc.stdout)
        text = self.log.read_text(encoding="utf-8")
        self.assertIn("fallback-1.list", text)

    def test_a_total_outage_reports_an_infrastructure_failure(self):
        proc = self.run_script(
            "--cpp-toolchain",
            "false",
            FAKE_APT_FAIL_UPDATES="99",
            FAKE_APT_FALLBACK_OK="0",
        )
        self.assertEqual(proc.returncode, 75, proc.stdout)
        self.assertIn("Ubuntu mirror unreachable", proc.stderr)
        self.assertIn("infrastructure failure, not a code failure", proc.stderr)
        self.assertIn("Temporary failure resolving", proc.stderr)
        self.assertIn("mirrors.edge.kernel.org", proc.stderr)

    def test_an_outage_falls_back_to_the_restored_cache(self):
        proc = self.run_script(
            "--cpp-toolchain",
            "false",
            FAKE_APT_FAIL_UPDATES="99",
            FAKE_APT_FALLBACK_OK="0",
            FAKE_APT_OFFLINE_OK="1",
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("installed from the restored apt cache", proc.stderr)
        self.assertIn("trying cached packages only", proc.stdout)

    def test_a_missing_package_is_not_reported_as_a_mirror_outage(self):
        proc = self.run_script(
            "--cpp-toolchain",
            "false",
            "--extra",
            "made-up",
            FAKE_APT_MISSING_PACKAGE="1",
        )
        self.assertEqual(proc.returncode, 1, proc.stdout)
        self.assertIn("Package not available", proc.stderr)
        self.assertNotIn("Ubuntu mirror unreachable", proc.stderr)

    def test_the_cpp_toolchain_adds_clang_and_ncurses(self):
        proc = self.run_script("--cpp-toolchain", "true", "--extra", "clang-tidy-18")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        install = [line for line in self.log.read_text().splitlines() if " install " in line]
        self.assertTrue(install)
        self.assertIn("clang", install[0])
        self.assertIn("libncursesw5-dev", install[0])
        self.assertIn("clang-tidy-18", install[0])

    def test_no_toolchain_keeps_the_base_packages_only(self):
        proc = self.run_script("--cpp-toolchain", "false")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        install = [line for line in self.log.read_text().splitlines() if " install " in line]
        self.assertTrue(install)
        self.assertIn("build-essential", install[0])
        self.assertNotIn("clang", install[0])

    def test_probe_reports_a_healthy_primary(self):
        proc = self.run_script("--probe")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("primary", proc.stdout)
        self.assertIn("mirror probe passed", proc.stdout)

    def test_probe_accepts_a_working_fallback(self):
        proc = self.run_script(
            "--probe", FAKE_APT_FAIL_UPDATES="99", FAKE_APT_FALLBACK_OK="1"
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("the fallback is usable", proc.stdout)

    def test_probe_fails_when_every_mirror_is_down(self):
        proc = self.run_script(
            "--probe", FAKE_APT_FAIL_UPDATES="99", FAKE_APT_FALLBACK_OK="0"
        )
        self.assertEqual(proc.returncode, 1, proc.stdout)
        self.assertIn("every configured fallback failed", proc.stderr)

    def test_the_action_delegates_to_this_script(self):
        action = (SCRIPT.parent / "action.yml").read_text(encoding="utf-8")
        self.assertIn("install-deps.sh", action)
        self.assertIn("apt-ubuntu-24.04", action)
        self.assertNotIn("apt-get", action)
        self.assertIn("actions/cache/restore", action)

    def test_an_unknown_argument_is_a_usage_error(self):
        proc = self.run_script("--wat")
        self.assertEqual(proc.returncode, 2)


if __name__ == "__main__":
    unittest.main()
