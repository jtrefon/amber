#!/bin/sh
# Build-hygiene invariants (AGENTS.md "Build & verify" + the architecture-audit
# table). Static checks only: dry-run make, no writes to the tree.
# Run from the repository root, or via `make check`.
#   P1  `make test` must build the plugin fixtures tests/plugin_test.cpp reads.
#   P2  every compiled TU emits a .d dependency file (AGENTS.md gotcha).
#   P3  `make clean` removes every build artifact.
#   P4  compile_commands.json is never committed (stale machine-specific flags).
#   P5  AGENTS.md audit-table line counts match the tree.

cd "$(dirname "$0")/.." || exit 1
failures=0
warn() { echo "FAIL: $1"; failures=$((failures + 1)); }
ok() { echo "  ok: $1"; }

# P1
if make -n -B test 2>/dev/null | grep -q 'sysinfo-plugin'; then
    ok "make test builds sysinfo-plugin"
else
    warn "P1: 'make test' does not build sysinfo-plugin (tests/plugin_test.cpp requires it)"
fi
if make -n -B test 2>/dev/null | grep -q 'cdp-plugin'; then
    ok "make test builds cdp-plugin"
else
    warn "P1: 'make test' does not build cdp-plugin (tests/plugin_test.cpp requires it)"
fi

# P2
test_out=$(make -n -B test 2>/dev/null)
if echo "$test_out" | grep -E 'ws_test' | grep -q -- '-MMD'; then
    ok "ws_test compiles with -MMD"
else
    warn "P2: ws_test compiles without -MMD dependency generation"
fi
for p in sysinfo-plugin cdp-plugin; do
    if echo "$test_out" | grep -E "$p" | grep -q -- '-MMD'; then
        ok "$p compiles with -MMD"
    else
        warn "P2: $p compiles without -MMD dependency generation"
    fi
done
if awk '/^-include/{print}' Makefile.in | grep -q 'TUI_TEST_OBJ'; then
    ok "tui_tests.d is in the -include list"
else
    warn "P2: TUI_TEST_OBJ missing from the -include list (stale tui_tests.o risk)"
fi
if awk '/^-include/{print}' Makefile.in | grep -q 'PLUGIN_TEST_OBJ'; then
    ok "plugin_test.d is in the -include list"
else
    warn "P2: PLUGIN_TEST_OBJ missing from the -include list (stale plugin_test.o risk)"
fi
if awk '/^-include/{print}' Makefile.in | grep -q 'COMP_TEST_OBJ'; then
    ok "completions_test.d is in the -include list"
else
    warn "P2: COMP_TEST_OBJ missing from the -include list (stale completions_test.o risk)"
fi
if echo "$test_out" | grep -E 'md4c/md4c' | grep -q -- '-MMD'; then
    ok "md4c compiles with -MMD"
else
    warn "P2: md4c compiles without -MMD dependency generation"
fi
if awk '/^-include/{print}' Makefile.in | grep -q 'md4c'; then
    ok "md4c.d is in the -include list"
else
    warn "P2: md4c.d missing from the -include list (stale md4c.o risk)"
fi
for v in CDP_MAIN_OBJ CDP_WS_OBJ WS_TEST_OBJ; do
    if awk '/^-include/{print}' Makefile.in | grep -q "$v"; then
        ok "$v .d is in the -include list"
    else
        warn "P2: $v missing from the -include list (stale plugin/ws_test object risk)"
    fi
done

# P3
artifacts="run_tests command_line_test e2e_test completions_test plugin_test ws_test bench_test smoketest run_command_line_test run_completions_test run_e2e_test amber-cli amber amber-bench sysinfo-plugin cdp-plugin libagent_core.a libagent_tools.a"
clean_out=$(make -n clean 2>/dev/null)
for f in $artifacts; do
    if echo "$clean_out" | grep -qF "$f"; then
        ok "make clean removes $f"
    else
        warn "P3: 'make clean' does not remove $f"
    fi
done
for p in 'lib/*.o' 'lib/*.d' 'src/*.o' 'tui/*.o' 'tests/*.o' 'tests/*.d' 'bench/*.o' 'bench/*.d' 'tools/*.o' 'tools/search/*.o' 'third_party/md4c/*.o' 'tools/plugins/cdp/*.o' 'tools/plugins/cdp/*.d'; do
    if echo "$clean_out" | grep -qF "$p"; then
        ok "make clean removes $p"
    else
        warn "P3: 'make clean' does not remove $p"
    fi
done
if echo "$clean_out" | grep -q 'libagent\.a'; then
    warn "P3: 'make clean' still references the retired libagent.a"
else
    ok "make clean has no legacy libagent.a reference"
fi

# P4
if [ -f compile_commands.json ]; then
    warn "P4: compile_commands.json is present (stale machine-specific flags)"
else
    ok "compile_commands.json absent"
fi
if grep -q 'compile_commands' .gitignore; then
    ok "compile_commands.json is gitignored"
else
    warn "P4: compile_commands.json is not gitignored"
fi

# P5
for f in tests/run_tests.cpp lib/session.cpp tui/tui_render.cpp tui/tui_input.cpp; do
    actual=$(wc -l < "$f")
    doc=$(grep -E "^\| \`$f\` \| " AGENTS.md | head -1 |
          sed -E 's/^\| [^|]* \| ([0-9]+) \|.*/\1/')
    if [ -n "$doc" ] && [ "$doc" = "$actual" ]; then
        ok "AGENTS.md lists $f at $actual lines"
    else
        warn "P5: AGENTS.md audit table lists $f at '$doc' lines; tree has $actual"
    fi
done

if [ "$failures" -gt 0 ]; then
    echo "build-hygiene: $failures invariant(s) FAILED"
    exit 1
fi
echo "build-hygiene: all invariants hold"
