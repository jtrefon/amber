"""The plugin API source-compatibility gate.

`docs/spec/plugins/api-compatibility.md` promises the documented plugin surface
is source-stable in 1.x: removals and signature changes need a deprecation
release, additions are allowed. The gate records the public declarations and
fails when one disappears; it cannot be tested by deleting a real header method
from the tree, so the extractor and the comparison are exercised on fixtures.
"""

import io
import os
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

from harness import load

api = load("api_snapshot")

FIXTURE = """#ifndef API_FIXTURE_H
#define API_FIXTURE_H

namespace agent {

// A class starts private; the gate must record only the public part.
class IPlugin {
public:
    virtual ~IPlugin() = default;
    virtual std::string id() const = 0;
    virtual void tick() { ++ticks_; }
    int defaulted(int value = 0) const;

protected:
    int protected_value = 0;

private:
    int ticks_ = 0;
};

// A struct starts public.
struct Item {
    int value = 0;
    void reset();
};

enum class Kind : std::uint8_t {
    First,
    Second,
};

using Callback = std::function<void()>;

class Forward;

} // namespace agent

#endif
"""


def run_check(entries, snapshot, minimum=1):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        rc = api.check(entries, snapshot, minimum=minimum)
    return rc, out.getvalue() + err.getvalue()


class Extraction(unittest.TestCase):
    def entries(self):
        return api.extract_header(FIXTURE)

    def test_namespace_class_and_public_methods_are_recorded(self):
        entries = self.entries()
        self.assertIn("agent | class IPlugin", entries)
        self.assertIn("agent::IPlugin | virtual ~IPlugin() = default;", entries)
        self.assertIn("agent::IPlugin | virtual std::string id() const = 0;", entries)
        self.assertIn("agent::IPlugin | int defaulted(int value = 0) const;", entries)

    def test_private_and_protected_members_are_not_recorded(self):
        entries = self.entries()
        self.assertNotIn("agent::IPlugin | int ticks_ = 0;", entries)
        self.assertNotIn("agent::IPlugin | int protected_value = 0;", entries)

    def test_struct_members_are_public_by_default(self):
        entries = self.entries()
        self.assertIn("agent::Item | int value = 0;", entries)
        self.assertIn("agent::Item | void reset();", entries)

    def test_enum_and_enumerators_are_recorded(self):
        entries = self.entries()
        self.assertIn("agent | enum class Kind : std::uint8_t", entries)
        self.assertIn("agent::Kind::First", entries)
        self.assertIn("agent::Kind::Second", entries)

    def test_aliases_and_macros_are_recorded(self):
        entries = self.entries()
        self.assertIn("agent | using Callback = std::function<void()>;", entries)
        self.assertIn("#define API_FIXTURE_H", entries)

    def test_a_forward_declaration_does_not_duplicate_the_definition(self):
        self.assertIn("agent | class Forward", self.entries())

    def test_comments_and_literals_do_not_change_the_snapshot(self):
        noisy = FIXTURE.replace("virtual std::string id() const = 0;",
                                "virtual std::string id() const = 0; // a comment")
        self.assertEqual(api.extract_header(noisy), api.extract_header(FIXTURE))

    def test_a_function_body_is_not_surface(self):
        entries = self.entries()
        self.assertIn("agent::IPlugin | virtual void tick()", entries)
        self.assertFalse(any("ticks_" in entry for entry in entries))


class Comparison(unittest.TestCase):
    def setUp(self):
        self.entries = api.extract_header(FIXTURE)

    def test_a_removal_fails_and_names_the_declaration(self):
        snapshot = self.entries | {"agent::IPlugin | virtual std::string id() const = 0;"}
        entries = {e for e in snapshot if "virtual std::string id" not in e}
        rc, out = run_check(entries, snapshot)
        self.assertEqual(rc, 1)
        self.assertIn("virtual std::string id", out)

    def test_a_rename_fails(self):
        snapshot = self.entries | {"agent::IPlugin | virtual std::string identifier() const = 0;"}
        rc, out = run_check(self.entries, snapshot)
        self.assertEqual(rc, 1)
        self.assertIn("identifier", out)

    def test_a_signature_change_fails(self):
        snapshot = self.entries | {"agent::IPlugin | int defaulted() const;"}
        rc, _ = run_check(self.entries, snapshot)
        self.assertEqual(rc, 1)

    def test_additive_change_passes_and_is_reported(self):
        snapshot = {e for e in self.entries if "defaulted" not in e}
        rc, out = run_check(self.entries, snapshot)
        self.assertEqual(rc, 0)
        self.assertIn("additive", out)

    def test_equal_snapshot_passes(self):
        rc, out = run_check(self.entries, self.entries)
        self.assertEqual(rc, 0)
        self.assertIn("intact", out)

    def test_an_empty_measurement_fails_closed(self):
        """A parser that returns almost nothing would make the gate green on an
        empty snapshot, which is the failure mode every gate here exists to
        prevent."""
        rc, out = run_check({"agent | class IPlugin"}, {"agent | class IPlugin"},
                            minimum=api.MIN_ENTRIES)
        self.assertEqual(rc, 2)
        self.assertIn("floor", out)

    def test_an_empty_snapshot_fails_closed(self):
        rc, _ = run_check(self.entries, set())
        self.assertEqual(rc, 2)

    def test_missing_header_is_an_error_not_a_skip(self):
        with self.assertRaises(SystemExit):
            api.extract(["include/agent/does_not_exist.h"], root="/tmp")


class SnapshotFile(unittest.TestCase):
    def test_round_trip(self):
        handle = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False)
        handle.close()
        try:
            api.write_snapshot(handle.name, {"agent | class IPlugin", "agent::IPlugin::id"})
            loaded = api.read_snapshot(handle.name)
        finally:
            os.unlink(handle.name)
        self.assertEqual(loaded, {"agent | class IPlugin", "agent::IPlugin::id"})

    def test_comments_and_blank_lines_are_ignored_but_macros_are_data(self):
        handle = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False)
        handle.write("# comment\n#define AGENT_X 1\n\nagent | class IPlugin\n")
        handle.close()
        try:
            self.assertEqual(api.read_snapshot(handle.name),
                             {"#define AGENT_X 1", "agent | class IPlugin"})
        finally:
            os.unlink(handle.name)

    def test_the_committed_snapshot_matches_the_tree(self):
        """The real snapshot is what CI reads. A stale one fails the gate, so
        this test asserts the checked-in artefact and the extractor agree."""
        rc, out = run_check(api.extract(api.HEADERS), api.read_snapshot(api.SNAPSHOT),
                            minimum=api.MIN_ENTRIES)
        self.assertEqual(rc, 0, out)


if __name__ == "__main__":
    unittest.main()
