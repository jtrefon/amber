#!/usr/bin/env python3
"""
Cross-file duplicate code block detector.

Fingerprints brace-delimited blocks across all project source files and
reports pairs with identical normalized content (after stripping comments,
string literals, and numeric literals).

Usage: python3 duplicate_detector.py [--min-lines 6] [dirs ...]
"""

import hashlib
import os
import re
import sys

def normalize(line):
    line = re.sub(r'//.*', '', line)          # strip line comments
    line = re.sub(r'\"[^\"]*\"', '""', line)   # normalize string literals
    line = re.sub(r'\b\d+\b', '0', line)       # normalize numbers
    return line.strip()

def extract_blocks(text, min_lines):
    blocks = []
    lines = text.split('\n')
    i = 0
    while i < len(lines):
        if '{' in lines[i] or '}' in lines[i]:
            brace = lines[i].count('{') - lines[i].count('}')
            start = i
            while i < len(lines) and brace != 0:
                i += 1
                if i < len(lines):
                    brace += lines[i].count('{') - lines[i].count('}')
            if i - start + 1 >= min_lines:
                block = '\n'.join(normalize(l) for l in lines[start:i+1])
                if block.strip():
                    sig = hashlib.md5(block.encode()).hexdigest()
                    blocks.append((sig, start + 1, i + 1))
        i += 1
    return blocks

def main():
    min_lines = 6
    src_dirs = ['lib', 'tools', 'tui', 'tests', 'src']
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if args:
        src_dirs = args
    for a in sys.argv[1:]:
        if a.startswith('--min-lines='):
            min_lines = int(a.split('=')[1])

    seen = {}
    exit_code = 0
    for d in src_dirs:
        if not os.path.isdir(d):
            continue
        for root, _, files in os.walk(d):
            for f in files:
                if not f.endswith(('.cpp', '.h')):
                    continue
                path = os.path.join(root, f)
                try:
                    with open(path) as fh:
                        text = fh.read()
                except Exception:
                    continue
                for sig, start, end in extract_blocks(text, min_lines):
                    if sig in seen:
                        prev_path, prev_start, prev_end = seen[sig]
                        if prev_path != path:
                            print(f'  {prev_path}:{prev_start}-{prev_end}  <->  {path}:{start}-{end}')
                            exit_code = 1
                    else:
                        seen[sig] = (path, start, end)
    sys.exit(exit_code)

if __name__ == '__main__':
    main()
