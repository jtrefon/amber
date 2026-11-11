#!/usr/bin/env python3
"""A well-behaved plugin whose tool call never returns.

Used to prove that disabling is bounded: teardown must cancel the call and
kill the process instead of waiting for the 60 s call deadline.
"""
import json
import os
import sys
import time

for line in sys.stdin:
    msg = json.loads(line)
    method = msg.get("method")
    if method == "initialize":
        sys.stdout.write(
            json.dumps({"id": msg["id"], "result": {"protocol_version": 1, "ok": True}}) + "\n"
        )
        sys.stdout.flush()
    elif method == "tool.call":
        marker = os.environ.get("AMBER_PLUGIN_TEST_MARKER")
        if marker:
            with open(marker, "w", encoding="utf-8") as handle:
                handle.write("called")
        time.sleep(3600)
