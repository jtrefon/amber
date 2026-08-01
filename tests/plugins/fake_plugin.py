#!/usr/bin/env python3
import json
import sys

for line in sys.stdin:
    msg = json.loads(line)
    method = msg.get("method")
    if method == "initialize":
        sys.stdout.write(json.dumps({"id": msg["id"], "result": {
            "protocol_version": 1, "ok": True}}) + "\n")
    elif method == "tool.call":
        name = msg["params"]["name"]
        args = msg["params"]["args"]
        if name == "echo":
            result = {"ok": True, "output": "echo:" + args.get("text", ""), "meta": {}}
        elif name == "fail":
            result = {"ok": False, "output": "ERROR: nope"}
        else:
            result = {"ok": False, "output": "ERROR: unknown tool " + name}
        sys.stdout.write(json.dumps({"id": msg["id"], "result": result}) + "\n")
    elif method == "shutdown":
        sys.exit(0)
    sys.stdout.flush()
