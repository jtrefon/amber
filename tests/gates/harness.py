"""Import the gate tools by path.

The tools in tools/ are standalone scripts, not an installed package, so the
tests load them by file location. Centralised here so each test module does not
repeat the loader, and so a tool that moves fails in one place.
"""

import importlib.util
import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOLS_DIR = os.path.join(REPO_ROOT, "tools")

# tools/ is a script directory, not a package: the gates import each other by
# plain module name (`import cpp_source`), which works when tools/ is on the path.
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

_CACHE = {}


def load(name):
    """Import tools/<name>.py as a module, once per process."""
    if name in _CACHE:
        return _CACHE[name]
    path = os.path.join(TOOLS_DIR, f"{name}.py")
    if not os.path.exists(path):
        raise AssertionError(f"gate tool not found: {path}")
    spec = importlib.util.spec_from_file_location(f"amber_gate_{name}", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"could not load gate tool: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    _CACHE[name] = module
    return module
