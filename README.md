cpp-agent: an AI agent harness for Linux servers

cpp-agent is a free-software agent runtime that exposes a small set of
pre-defined tools (read with pagination, patch-style write, and a search
tool that starts as a grep wrapper and can grow into indexed / semantic
search) to an OpenAI-compatible LLM API. System and tool descriptions are
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
  lib/        libagent.a — harness core (LLM client, tool registry, agent loop,
              prompt/markdown loader, built-in tools). No UI dependency.
  src/        cpp-agent  — headless CLI client linking libagent.
  tui/        cpp-agent-tui — ncurses TUI client linking libagent.
  tools/      the pre-defined tools: read (paginated), write (patch-style),
              search (pluggable backend: grep or local semantic index).

Building:
  ./configure
  make
  make check      # builds and runs the tool smoke test
  make install    # optional, honors --prefix

Run:
  ./cpp-agent --help
  ./cpp-agent-tui

Streaming:
  The LLM client supports OpenAI-compatible SSE streaming; token deltas are
  surfaced via AgentHooks::on_token for live TUI rendering. Disable with
  --no-stream (CLI) or CPP_AGENT_STREAM=0.

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
    process working directory and can be overridden with CPP_AGENT_WORKSPACE.
    Absolute paths and "../" traversal that escape the root are rejected.
  - Search: the grep backend passes the query and root as single-quoted
    arguments (shell metacharacters are neutralized), so a crafted query
    cannot inject shell commands.
  - Run the agent as an unprivileged user, ideally in a container or
    dedicated working directory, and review write operations for anything
    you would not run yourself.

License:
  Apache License 2.0. See LICENSE and NOTICE. Bundled third-party
  components are listed in THIRD_PARTY_LICENSES (nlohmann/json is MIT).
