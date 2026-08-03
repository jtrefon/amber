amber: an AI agent harness for Linux servers

amber is a free-software agent runtime that exposes a small set of
pre-defined tools (read with pagination, patch-style write, a search
tool that starts as a grep wrapper and can grow into indexed / semantic
search, and an approval-gated bash tool for running shell commands) to an
OpenAI-compatible LLM API. System and tool descriptions are
written as Markdown prompts. The request is routed to the LLM, which may
invoke tools; results are fed back until the agent terminates.

Build requirements:
  - A C++17 compiler (g++ or clang++)
  - GNU make
  - libcurl (development headers)
  - ncurses (development headers, for the TUI client)
  - OpenSSL (for HTTPS, pulled in by libcurl)
  - nlohmann/json is vendored under include/ (no separate install needed)

Layout:
  lib/        libagent_core.a + libagent_tools.a — harness core (LLM client,
              tool registry, agent loop, prompt/markdown loader, built-in
              tools). No UI dependency.
  src/        amber-cli — headless CLI client linking libagent.
  tui/        amber — ncurses TUI client linking libagent.
  bench/      amber-bench — benchmark & KPI harness (scenarios, oracle scoring,
              static templates). Observer over AgentHooks; no engine changes.
  tools/      the pre-defined tools: read (paginated), write (patch-style),
              search (pluggable backend: grep or local semantic index),
              bash (approval-gated shell execution in the workspace).

Building:
  ./configure
  make
  make check      # builds and runs the tool smoke test
  make install    # optional, honors --prefix

Run:
  ./amber
  ./amber-cli --help
  ./amber-bench run        # hermetic benchmark corpus (no model needed)
  ./amber-bench run --live # benchmark against your configured model
  ./amber-bench report bench/results/*.json --format markdown
                           # render the published KPI report (see BENCHMARK.md)

  Pass --yes to auto-approve the bash tool for the session (headless CLI);
  otherwise amber-cli prompts before each shell command on an interactive
  terminal and denies shell execution when stdin is not a TTY.

Streaming:
  The LLM client supports OpenAI-compatible SSE streaming; token deltas are
  surfaced via AgentHooks::on_token for live TUI rendering. Disable with
  --no-stream (CLI) or AMBER_STREAM=0.

Search backends:
  The search tool delegates to a pluggable SearchBackend. mode="grep" (default)
  wraps `grep -rnI`; mode="semantic" builds a local, dependency-free lexical
  index (hashing-trick vectors + IDF-weighted cosine ranking) over the tree.
  The `embed()` step is the single point to swap for a real embedding model.

Security model:
  The agent executes tools on behalf of an LLM, so treat model output as
  untrusted input.
  - Filesystem confinement: the read and write tools resolve every path and
    refuse anything outside the workspace root. The root defaults to the
    process working directory and can be overridden with AMBER_WORKSPACE.
    Absolute paths and "../" traversal that escape the root are rejected.
  - Search: the grep backend passes the query and root as single-quoted
    arguments (shell metacharacters are neutralized), so a crafted query
    cannot inject shell commands.
  - Shell execution: the bash tool runs arbitrary commands and is therefore
    approval-gated. Each call must be approved by the host before it runs; if
    no approval handler is installed the call is denied (fail-safe). The CLI
    prompts on a TTY (allow once / allow session / deny) and denies when stdin
    is not interactive unless --yes is passed; the TUI shows a confirmation
    dialog. Commands run in their own process group with the working directory
    set to the workspace root, are killed on timeout (default 60s), and their
    combined output is capped at 64 KiB.
  - Run the agent as an unprivileged user, ideally in a container or
    dedicated working directory, and review write operations for anything
    you would not run yourself.

License:
  Apache License 2.0. See LICENSE and NOTICE. Bundled third-party
  components are listed in THIRD_PARTY_LICENSES (nlohmann/json is MIT).
