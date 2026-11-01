"""Doctor must name toolchain drift instead of hiding it behind a version floor.

The gates fail closed when a tool is too old, but three failures are invisible to
a floor check: the tool is newer than CI's pinned build (so it reports a
different check set), it is missing entirely, or its version output cannot be
parsed. Doctor is the one place that says "this machine cannot reproduce the
gate" before a push, so its verdicts are part of the gate surface.
"""

import os
import unittest

from harness import REPO_ROOT, load

doctor = load("doctor")


class VersionParsing(unittest.TestCase):
    def test_parses_the_llvm_banner(self):
        self.assertEqual(
            doctor.parse_version("Ubuntu LLVM version 18.1.3"), (18, 1, 3)
        )

    def test_parses_the_cppcheck_banner(self):
        self.assertEqual(doctor.parse_version("Cppcheck 2.22.0"), (2, 22, 0))

    def test_parses_a_bare_version(self):
        self.assertEqual(doctor.parse_version("gcovr 8.6"), (8, 6))

    def test_missing_output_is_not_a_version(self):
        self.assertIsNone(doctor.parse_version(""))

    def test_unparsable_output_is_not_a_version(self):
        self.assertIsNone(doctor.parse_version("not a version"))


class Verdicts(unittest.TestCase):
    def test_a_missing_tool_is_a_blocker(self):
        verdict, _ = doctor.classify(None, (18, 1, 3), "major")
        self.assertEqual(verdict, "fail")

    def test_a_tool_older_than_the_floor_is_a_blocker(self):
        verdict, _ = doctor.classify((2, 19, 0), (2, 22, 0), "min")
        self.assertEqual(verdict, "fail")

    def test_a_newer_major_cannot_reproduce_ci(self):
        verdict, _ = doctor.classify((21, 1, 8), (18, 1, 3), "major")
        self.assertEqual(verdict, "fail")

    def test_a_newer_patch_in_the_same_major_is_a_warning(self):
        verdict, _ = doctor.classify((18, 1, 8), (18, 1, 3), "major")
        self.assertEqual(verdict, "warn")

    def test_an_exact_patch_mismatch_is_a_warning(self):
        verdict, _ = doctor.classify((18, 1, 8), (18, 1, 3), "exact")
        self.assertEqual(verdict, "warn")

    def test_an_exact_match_is_clean(self):
        verdict, _ = doctor.classify((18, 1, 3), (18, 1, 3), "exact")
        self.assertEqual(verdict, "ok")

    def test_a_newer_minimum_is_clean(self):
        verdict, _ = doctor.classify((2, 23, 0), (2, 22, 0), "min")
        self.assertEqual(verdict, "ok")


class ExitCode(unittest.TestCase):
    def test_warnings_alone_do_not_block(self):
        self.assertEqual(doctor.exit_code([{"status": "ok"}, {"status": "warn"}]), 0)

    def test_a_blocker_fails(self):
        self.assertEqual(doctor.exit_code([{"status": "ok"}, {"status": "fail"}]), 1)

    def test_an_unknown_status_fails_closed(self):
        self.assertEqual(doctor.exit_code([{"status": "unknown"}]), 1)


class PinsMatchThePipeline(unittest.TestCase):
    def test_pins_match_the_makefile_floors(self):
        path = os.path.join(REPO_ROOT, "Makefile.in")
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
        for name, pin in doctor.MAKEFILE_PINS.items():
            self.assertIn(f"{name} ?= {pin}", text, f"{name} drifted from doctor")

    def test_pins_match_the_ci_workflow(self):
        path = os.path.join(REPO_ROOT, ".github", "workflows", "ci.yml")
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
        for needle in doctor.CI_PIN_NEEDLES:
            self.assertIn(needle, text, f"doctor pins {needle!r} but CI does not")


if __name__ == "__main__":
    unittest.main()
