#!/usr/bin/env python3
"""Exercise a real 0.8.9 -> 0.9.0 installed-tree upgrade.

This is a release check, not a unit test: it drives the real TUI binaries on a
pty, a loopback OpenAI endpoint and a scratch XDG tree. The old binary creates
the 0.8.9 state (config, a saved session, a disabled plugin); the new binary
reads it, migrates the connection to a profile, applies a project profile pin,
creates a checkpoint and proves the 0.9 state survives a restart.

Usage:
  python3 tools/upgrade_check.py --old /path/to/0.8.9/usr/local/bin/amber \
      --new ./amber
  make upgrade-check UPGRADE_OLD=/path/to/0.8.9/usr/local/bin/amber

The old binary is not downloaded automatically: CI has no 0.8.9 artifact and a
release check must name the exact build it exercised.
"""

import argparse
import json
import os
import pty
import re
import select
import shutil
import subprocess
import signal
import stat
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ANSI = re.compile(r"\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*(?:\x07|\x1b\\)|\([A-Z0-9]|[=>])")
EXIT_READY = "[amber] exit-ready"


def text_of(raw):
    return ANSI.sub("", raw.decode("utf-8", "replace")).replace("\r", "")


class Endpoint:
    """A loopback OpenAI endpoint with a scripted conversation."""

    def __init__(self):
        self.requests = []
        endpoint = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def reply(self, payload, code=200):
                body = json.dumps(payload).encode()
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                if self.path.endswith("/models"):
                    self.reply({"object": "list", "data": [
                        {"id": "fake", "object": "model", "created": 1, "owned_by": "upgrade"},
                        {"id": "profile-model", "object": "model", "created": 1,
                         "owned_by": "upgrade"},
                    ]})
                else:
                    self.reply({"error": "not found"}, 404)

            def do_POST(self):
                length = int(self.headers.get("Content-Length", 0))
                body = self.rfile.read(length).decode("utf-8", "replace")
                endpoint.requests.append(body)
                self.reply(endpoint.answer(body))

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    @property
    def base(self):
        return f"http://127.0.0.1:{self.port}/v1"

    def close(self):
        self.server.server_close()

    @staticmethod
    def answer(request):
        try:
            messages = json.loads(request).get("messages", [])
            tool_result = bool(messages) and messages[-1].get("role") == "tool"
        except (ValueError, AttributeError):
            tool_result = '"role": "tool"' in request
        if tool_result:
            done = ("UPGRADE-CHECKPOINT-DONE" if "UPGRADE-CHECKPOINT" in request
                    else "UPGRADE-OLD-DONE")
            return completion({"role": "assistant", "content": done}, "stop")
        if "UPGRADE-CHECKPOINT" in request:
            return completion({"role": "assistant", "content": None, "tool_calls": [{
                "id": "call-checkpoint", "type": "function",
                "function": {"name": "write", "arguments": json.dumps({
                    "path": "note.txt",
                    "edits": [{"old": "two", "new": "three"}],
                })},
            }]}, "tool_calls")
        if "UPGRADE-NEW" in request:
            return completion({"role": "assistant", "content": "UPGRADE-CONTEXT-CARRIED"}, "stop")
        if "UPGRADE-OLD" in request:
            return completion({"role": "assistant", "content": None, "tool_calls": [{
                "id": "call-old", "type": "function",
                "function": {"name": "write", "arguments": json.dumps({
                    "path": "note.txt",
                    "edits": [{"old": "one", "new": "two"}],
                })},
            }]}, "tool_calls")
        return completion({"role": "assistant", "content": "UPGRADE-UNEXPECTED"}, "stop")


def completion(message, finish_reason):
    return {
        "id": "chatcmpl-upgrade",
        "object": "chat.completion",
        "created": 1,
        "model": "fake",
        "choices": [{"index": 0, "message": message, "finish_reason": finish_reason}],
        "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15},
    }


class TuiRun:
    def __init__(self, binary, root, env, steps, name, timeout=90):
        self.binary = binary
        self.root = root
        self.env = env
        self.steps = steps
        self.name = name
        self.timeout = timeout
        self.raw = bytearray()
        self.fd = -1
        self.pid = -1

    def _pump(self, seconds):
        deadline = time.time() + seconds
        while time.time() < deadline:
            ready, _, _ = select.select([self.fd], [], [], min(0.1, deadline - time.time()))
            if not ready:
                continue
            try:
                chunk = os.read(self.fd, 65536)
            except OSError:
                return False
            if not chunk:
                return False
            self.raw.extend(chunk)
        return True

    def _text(self):
        return text_of(bytes(self.raw))

    def wait_for(self, marker, cap):
        deadline = time.time() + cap
        while time.time() < deadline:
            if marker in self._text():
                return True
            self._pump(0.1)
        return False

    def run(self):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            try:
                os.chdir(self.root)
                os.execvpe(self.binary, [self.binary], self.env)
            except Exception as error:  # pragma: no cover - child exec failure
                os.write(2, f"upgrade-check: exec failed: {error}\n".encode())
                os._exit(127)
        if not self.wait_for("❯", 30) and not self.wait_for("loaded completions", 5):
            self._abort("the UI never reached its prompt")
        print(f"    [{self.name}] ready", flush=True)
        self._pump(1.0)
        for send, marker, cap in self.steps:
            print(f"    [{self.name}] step {send!r}", flush=True)
            payload = send.rstrip("\r")
            payload += "\x07" if payload.startswith("/") else "\r"
            os.write(self.fd, payload.encode())
            if marker and not self.wait_for(marker, cap):
                self._abort(f"timed out waiting for {marker!r}")
            self._pump(0.25)
        print(f"    [{self.name}] quit", flush=True)
        os.write(self.fd, b"/quit\x07")
        if not self.wait_for(EXIT_READY, 30):
            os.write(self.fd, b"\x03")
            if not self.wait_for(EXIT_READY, 20):
                self._abort("the UI did not exit through its quit path")
        self._reap()
        os.close(self.fd)
        return self._text()

    def _reap(self):
        deadline = time.time() + 5
        while time.time() < deadline:
            try:
                done, _ = os.waitpid(self.pid, os.WNOHANG)
            except ChildProcessError:
                return
            if done == self.pid:
                return
            time.sleep(0.05)
        try:
            os.kill(self.pid, signal.SIGKILL)
        except ProcessLookupError:
            return
        # Never block on a child that is wedged in an uninterruptible write:
        # the pty is gone, the process is not coming back, and a blocking
        # waitpid would hang the release check itself.
        for _ in range(50):
            try:
                done, _ = os.waitpid(self.pid, os.WNOHANG)
            except ChildProcessError:
                return
            if done == self.pid:
                return
            time.sleep(0.05)

    def _abort(self, why):
        tail = self._text()[-2500:]
        self._pump(0.3)
        try:
            os.write(self.fd, b"\x03")
        except OSError:
            pass
        self._pump(0.3)
        self._reap()
        raise RuntimeError(f"{self.name}: {why}\n--- last output ---\n{tail}")


class Check:
    def __init__(self):
        self.failures = []

    def ok(self, name, condition, detail=""):
        verdict = "ok" if condition else "FAIL"
        suffix = f": {detail}" if detail and not condition else ""
        print(f"  {verdict}  {name}{suffix}")
        if not condition:
            self.failures.append(name)


def read(path):
    return Path(path).read_text(encoding="utf-8")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--old", required=True, help="the 0.8.9 amber binary")
    parser.add_argument("--new", required=True, help="the 0.9.0 candidate amber binary")
    parser.add_argument("--keep", action="store_true", help="keep the scratch tree")
    args = parser.parse_args(argv)

    old = os.path.abspath(args.old)
    new = os.path.abspath(args.new)
    for path in (old, new):
        if not os.path.isfile(path):
            print(f"upgrade-check: not a file: {path}", file=sys.stderr)
            return 2

    scratch = Path(tempfile.mkdtemp(prefix="amber-upgrade-"))
    root = scratch / "project"
    home = scratch / "home"
    config = scratch / "config"
    state = scratch / "state"
    cache = scratch / "cache"
    for directory in (root, home, config, state, cache):
        directory.mkdir(parents=True)
    (root / "note.txt").write_text("one\n")
    (root / ".amber").mkdir()
    env = os.environ.copy()
    env.update(
        HOME=str(home),
        XDG_CONFIG_HOME=str(config),
        XDG_STATE_HOME=str(state),
        XDG_CACHE_HOME=str(cache),
        AMBER_WORKSPACE=str(root),
        AMBER_EXIT_READY="1",
        TERM="xterm-256color",
        LINES="30",
        COLUMNS="100",
    )
    for name in ("AMBER_API_BASE", "AMBER_API_KEY", "AMBER_MODEL", "AMBER_STREAM"):
        env.pop(name, None)
    subprocess_ok = os.system(f"git -C '{root}' init -q") == 0
    subprocess_ok = subprocess_ok and os.system(
        f"git -C '{root}' -c user.name=upgrade -c user.email=upgrade@example.invalid "
        "add note.txt && "
        f"git -C '{root}' -c user.name=upgrade -c user.email=upgrade@example.invalid "
        "commit -q -m initial"
    ) == 0
    if not subprocess_ok:
        print("upgrade-check: git fixture failed", file=sys.stderr)
        return 2
    head_before = read(root / ".git/HEAD").strip()
    commit_before = os.popen(f"git -C '{root}' rev-parse HEAD").read().strip()

    endpoint = Endpoint()
    (config / "amber").mkdir(parents=True)
    (config / "amber/config").write_text("provider=custom\napi_key=sk-old-abcd\n")
    # The connection a 0.8.9 TUI actually uses is the project settings file;
    # the global config selects the provider. The migration reads the resolved
    # connection, so this is what becomes profiles/default.json.
    (root / "amber.conf").write_text(
        f"api_base={endpoint.base}\n"
        "api_key=sk-old-abcd\n"
        "model=fake\n"
        "stream=0\n"
    )
    check = Check()
    old_version = subprocess.run([old, "--version"], capture_output=True, text=True).stdout
    check.ok("the old binary reports 0.8.9", "0.8.9" in old_version, old_version.strip())
    try:
        print("0.8.9")
        old_text = TuiRun(old, str(root), env, [
            ("/set plugin off clock\r", "plugin clock off", 15),
            ("Change note.txt from one to two. UPGRADE-OLD\r", "UPGRADE-OLD-DONE", 45),
        ], "0.8.9").run()
        check.ok("the 0.8.9 turn completed", "UPGRADE-OLD-DONE" in old_text)
        check.ok("the 0.8.9 file change landed", read(root / "note.txt") == "two\n")
        plugin_state = config / "amber/plugins/clock/plugin.conf"
        check.ok("the 0.8.9 plugin state was written", plugin_state.exists())
        sessions = list((state / "amber/projects").glob("*/sessions/*.json"))
        check.ok("the 0.8.9 session was saved", bool(sessions))

        print("0.9.0 first start (migration + session resume)")
        new_text = TuiRun(new, str(root), env, [
            ("/profile list\r", "default", 15),
            ("What did I change? UPGRADE-NEW\r", "UPGRADE-CONTEXT-CARRIED", 45),
        ], "0.9.0 migration").run()
        check.ok("the old session carried its context", "UPGRADE-CONTEXT-CARRIED" in new_text)
        check.ok("the old session was visible",
                 "UPGRADE-OLD" in new_text or "UPGRADE-OLD-DONE" in new_text)
        default_profile = config / "amber/profiles/default.json"
        check.ok("the connection migrated to profiles/default.json", default_profile.exists())
        if default_profile.exists():
            profile = json.loads(default_profile.read_text())
            check.ok("the migrated profile kept the endpoint",
                     profile.get("api_base") == endpoint.base)
            check.ok("the migrated profile kept the model", profile.get("model") == "fake")
            check.ok("the migrated key is masked in the transcript", "sk-old-abcd" not in new_text)
            check.ok("the migrated key is stored", profile.get("api_key") == "sk-old-abcd")
            check.ok("the profile file is private",
                     stat.S_IMODE(default_profile.stat().st_mode) == 0o600)
        profiles_dir = config / "amber/profiles"
        check.ok("the profiles directory is private",
                 stat.S_IMODE(profiles_dir.stat().st_mode) == 0o700)

        # A second profile and a project pin. The pin changes the model, so the
        # next request proves it applied before the turn.
        (profiles_dir / "upgrade.json").write_text(json.dumps({
            "version": 1,
            "name": "upgrade",
            "provider": "openai",
            "flavor": "openai",
            "api_base": endpoint.base,
            "api_key": "sk-prof-1234",
            "model": "profile-model",
        }, indent=2) + "\n")
        (root / ".amber/profile").write_text("upgrade\n")

        print("0.9.0 profile pin + checkpoint")
        checkpoint_text = TuiRun(new, str(root), env, [
            ("/get model\r", "profile-model", 15),
            ("/profile list\r", "upgrade", 15),
            ("Change note.txt from two to three. UPGRADE-CHECKPOINT\r",
             "UPGRADE-CHECKPOINT-DONE", 45),
            ("/session history\r", "checkpoints (newest first", 15),
        ], "0.9.0 checkpoint").run()
        check.ok("the project profile pin applied", "profile-model" in checkpoint_text)
        check.ok("the second profile is listed", "upgrade" in checkpoint_text)
        check.ok("the profile key is masked", "sk-prof-1234" not in checkpoint_text)
        check.ok("the masked profile key is shown", "sk-...1234" in checkpoint_text)
        check.ok("the checkpoint turn completed", "UPGRADE-CHECKPOINT-DONE" in checkpoint_text)
        check.ok("the checkpoint history lists a turn", "turn 1" in checkpoint_text)
        refs = os.popen(
            f"git -C '{root}' for-each-ref --format='%(refname)' refs/amber/checkpoints"
        ).read()
        check.ok("the checkpoint ref exists", "refs/amber/checkpoints/" in refs)
        check.ok("the user HEAD did not move", read(root / ".git/HEAD").strip() == head_before)
        check.ok("the user commit did not move",
                 os.popen(f"git -C '{root}' rev-parse HEAD").read().strip() == commit_before)

        print("0.9.0 restart (session + checkpoint + plugin state)")
        restart_text = TuiRun(new, str(root), env, [
            ("/session history\r", "checkpoints (newest first", 15),
            ("/get plugin info clock\r", "clock", 15),
        ], "0.9.0 restart").run()
        check.ok("the checkpoint survives a restart", "turn 1" in restart_text)
        check.ok("the old plugin state survives", "off" in restart_text)
        check.ok("the checkpoint ref still exists",
                 "refs/amber/checkpoints/" in os.popen(
                     f"git -C '{root}' for-each-ref --format='%(refname)' "
                     "refs/amber/checkpoints").read())

        print("0.9.0 missing-config fallback")
        (config / "amber/config").rename(config / "amber/config.old")
        fallback_text = TuiRun(new, str(root), env, [], "0.9.0 missing config").run()
        check.ok("a missing config falls back with a warning",
                 "wrote default config" in fallback_text)
        check.ok("the fallback config was written", (config / "amber/config").exists())

        print("0.9.0 legacy-value fallback")
        with (root / "amber.conf").open("a", encoding="utf-8") as handle:
            handle.write("compression_target_pct=bogus\n")
        legacy_text = TuiRun(new, str(root), env, [], "0.9.0 legacy config").run()
        check.ok("a legacy value warns and falls back",
                 "not a number; using default" in legacy_text)
    finally:
        endpoint.close()
        if args.keep or check.failures:
            print(f"upgrade-check: scratch kept at {scratch}")
        else:
            shutil.rmtree(scratch, ignore_errors=True)

    if check.failures:
        print(f"\nupgrade-check: FAILED ({len(check.failures)} check(s))")
        return 1
    print("\nupgrade-check: passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
