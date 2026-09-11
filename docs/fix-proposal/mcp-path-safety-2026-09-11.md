# MCP Path Safety — Proposal

## Problem

`lib/mcp_config.cpp` has three path-safety gaps that allow MCP server
configurations to escape the workspace or write/delete files outside
the MCP config directory.

### 1. `cwd` not confined

`make_transport()` passes `cfg.cwd` directly to `StdioTransport`
without calling `Workspace::confine()`. A malicious or misconfigured
MCP server config can set `cwd=/etc` or `cwd=../../` to execute
commands from an arbitrary directory.

```cpp
std::string cwd = cfg.cwd.empty() ? Workspace::root() : cfg.cwd;
return std::make_unique<StdioTransport>(cfg.command, args, cwd, ...);
```

### 2. Server name in path without sanitization

`save_mcp_server()` and `delete_mcp_server()` use `cfg.name + ".conf"`
directly in filesystem paths:

```cpp
std::ofstream f(dir / (cfg.name + ".conf"), std::ios::trunc);
fs::remove(fs::path(mcp_dir(true)) / (name + ".conf"), ec);
```

A name like `../../etc/passwd` would write or delete outside the MCP
config directory (path traversal).

### 3. No symlink check on config files

`load_dir()` uses `fs::directory_iterator` without checking for
symlinks. A symlinked `.conf` file pointing outside the config
directory would cause reads from arbitrary locations.

## Target state

### 1. Confine `cwd`

In `validate()`, if `cwd` is non-empty, call `Workspace::confine()`.
If it fails, set `cfg.error` and clear `cwd` so `make_transport()`
falls back to `Workspace::root()`.

### 2. Sanitize server names

Add a `valid_server_name()` helper that rejects names containing `/`,
`..`, leading dots, or any character outside `[a-zA-Z0-9_-]`. Call it
in `validate()`, `save_mcp_server()`, and `delete_mcp_server()`.

### 3. Reject symlinked config files

In `load_dir()`, skip entries where `is_symlink()` is true. This
prevents a symlinked `.conf` from redirecting reads outside the
config directory.

## Scope

- `lib/mcp_config.cpp`: add `valid_server_name()`, confine `cwd` in
  `validate()`, reject symlinks in `load_dir()`, sanitize names in
  `save_mcp_server()` / `delete_mcp_server()`
- `tests/mcp_config_test.cpp`: Red tests for each gap, then Green
  after implementation

## Risk

Low. The changes add validation to existing code paths without
changing the happy path. Existing tests use valid names and workspace-
relative `cwd` values, so they remain unaffected.
