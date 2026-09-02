# Homebrew formula for amber.
#
# The release tarball (amber-<ver>-darwin-<arch>.tar.gz) is a staged install
# tree: bin/, lib/, include/, share/amber/. This formula installs that tree
# into the Cellar prefix.
#
# NOTE (Apple Silicon): the app resolves its data files (prompts, completions,
# plugins) via lib/data_path.cpp, which currently checks XDG_DATA_HOME/amber,
# ~/.local/share/amber, ~/.config/amber, /usr/local/share/amber, and
# /usr/share/amber. On Intel Homebrew the prefix is /usr/local, so the data
# lands at /usr/local/share/amber and is found. On Apple Silicon the prefix is
# /opt/homebrew, so the data lands at /opt/homebrew/share/amber and is NOT in
# the search list. Two fixes (either works):
#   1. (recommended) add the Homebrew prefix to data_path.cpp's search list,
#      or
#   2. run with XDG_DATA_HOME="$(brew --prefix)/share" amber-cli ...
# Until then, `brew install amber` works on Intel; on Apple Silicon set the
# env var above.

class Amber < Formula
  desc "C++ AI agent harness with a headless CLI and ncurses TUI"
  homepage "https://github.com/jtrefon/amber"
  license "Apache-2.0"

  # Bump per release; the release workflow names the tarball amber-<ver>-darwin-<arch>.
  version "0.1.0"

  url "https://github.com/jtrefon/amber/releases/download/v#{version}/amber-#{version}-darwin-#{Hardware::CPU.arm? ? 'arm64' : 'x86_64'}.tar.gz"

  depends_on "curl"
  depends_on "ncurses"

  def install
    # Binaries.
    bin.install "bin/amber"
    bin.install "bin/amber-cli"
    bin.install "bin/amber-bench"

    # Static libs + headers (for downstream linking / plugin development).
    lib.install "lib/libagent_core.a"
    lib.install "lib/libagent_tools.a"
    (include/"agent").install Dir["include/agent/*.h"]
    (include/"nlohmann").install "include/nlohmann/json.hpp"

    # Data files (prompts, completions, plugins).
    # NOTE (Apple Silicon): the app resolves data via lib/data_path.cpp, which
    # checks /usr/local/share/amber and /usr/share/amber. On Intel Homebrew the
    # prefix is /usr/local so this is found; on Apple Silicon the prefix is
    # /opt/homebrew, so set XDG_DATA_HOME="$(brew --prefix)/share" until
    # data_path.cpp gains the Homebrew prefix in its search list.
    share.install "share/amber"
  end

  test do
    system bin/"amber-cli", "--version"
  end
end
