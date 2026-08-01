#!/usr/bin/env python3
"""MCP stdio test server that ignores SIGTERM and writes its PID to a file.

Usage: mcp_ignore_sigterm.py <pidfile>
"""
import os
import signal
import sys
import time


def main():
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    pidfile = sys.argv[1]
    with open(pidfile, "w") as f:
        f.write(str(os.getpid()))
    while True:
        line = sys.stdin.readline()
        if not line:
            time.sleep(3600)
            continue


if __name__ == "__main__":
    main()
