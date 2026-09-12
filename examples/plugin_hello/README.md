# Reference plugin: hello

The compiled example from `docs/spec/plugins/developer-guide.md`. It contributes
a single `greet` tool through `ToolCapability`, so it demonstrates the whole
core-plugin path: declare a capability, let the runtime install it, and let
disable unwind it.

It is **not** bundled, it is not registered in `make_bundled_plugins()`, so it
does not ship in the product. It is compiled and exercised by
`tests/example_plugin_test.cpp`, which is the guard that the guide's anatomy
still builds and behaves.

To make your own plugin from this:

1. Copy this directory to `plugins/<your-id>/`.
2. Rename the class and change `id()`/`name()`/`description()`/`category()`.
3. Replace `GreetTool` with your tool.
4. Register it in `make_bundled_plugins()` (`lib/plugins_bundled.cpp`) and add
   the object to `Makefile.in`.
5. Write a test like `tests/example_plugin_test.cpp`.
