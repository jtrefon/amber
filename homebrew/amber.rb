# Homebrew formula for amber (Apple Silicon only — Intel macOS builds were
# retired; the release workflow ships darwin-arm64 tarballs only).
#
# The release tarball (amber-<ver>-darwin-arm64.tar.gz) is a staged install
# tree: bin/, lib/, include/, share/amber/. This formula installs that tree
# into the Cellar prefix. The installed app resolves its data files relative
# to argv0 (<prefix>/bin/amber -> <prefix>/share/amber), so the layout works
# from any Homebrew prefix.

class Amber < Formula
  desc "C++ AI agent harness with a headless CLI and ncurses TUI"
  homepage "https://github.com/jtrefon/amber"
  license "Apache-2.0"

  # Bump per release; the release workflow names the tarball amber-<ver>-darwin-arm64.
  version "0.4.10"

  url "https://github.com/jtrefon/amber/releases/download/v#{version}/amber-#{version}-darwin-arm64.tar.gz"

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
    share.install "share/amber"
  end

  test do
    system bin/"amber-cli", "--version"
  end
end
