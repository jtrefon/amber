#!/bin/sh
# Install amber-agent from a repository tree and prove the result works.
#
#   verify-install.sh <repo-dir> [<expected-version>]
#
# Runs inside the target distribution's container, against the repository
# produced by build-repo.sh, addressed over file:// so the check does not depend
# on the Pages deployment having propagated.
#
# Two kinds of proof are required:
#   positive  the repository installs, and the installed binaries run from a
#             directory that contains no project files
#   negative  a tampered index, and a source without Signed-By, are both
#             refused. A signature nobody checks is decoration.
#
# SPDX-License-Identifier: Apache-2.0

set -eu

repo=$(cd "${1:?usage: verify-install.sh <repo-dir> [version]}" && pwd)
expected=${2:-}

fail() { printf 'verify: FAIL: %s\n' "$*" >&2; exit 1; }
ok() { printf 'verify: ok: %s\n' "$*"; }

[ -d "$repo/apt" ] && [ -d "$repo/rpm" ] || fail "not a repository tree: $repo"

manager=""
if command -v apt-get >/dev/null 2>&1; then
    manager="apt"
elif command -v dnf >/dev/null 2>&1; then
    manager="dnf"
else
    fail "no apt-get or dnf in this image"
fi
ok "package manager: $manager"

echo "verify: --- install through the published script ---"
AMBER_REPO_BASE_URL="file://$repo" sh "$repo/install.sh"

echo "verify: --- the installed package ---"
for binary in amber amber-cli amber-bench; do
    command -v "$binary" >/dev/null 2>&1 || fail "$binary is not on PATH after install"
done
installed="$(amber-cli --version | awk '{ print $2 }')"
[ -n "$installed" ] || fail "amber-cli --version printed no version"
if [ -n "$expected" ]; then
    [ "$installed" = "$expected" ] || fail "installed $installed, expected $expected"
fi
ok "amber-cli $installed"

# The point of the data-directory work: everything is resolved from the install
# prefix, not from whatever directory the user happens to be in.
mkdir -p /tmp/empty-cwd
cd /tmp/empty-cwd
amber --version >/dev/null || fail "amber --version failed from an empty directory"
amber-cli update --print >/dev/null || fail "amber-cli update --print failed"
amber-bench list >/tmp/bench-list.txt || fail "amber-bench list failed from an empty directory"
[ -s /tmp/bench-list.txt ] || fail "amber-bench found no scenarios"
ok "all three binaries run from an empty directory"

case "$manager" in
apt)
    [ -f /usr/share/amber/completions.json ] || fail "completions.json is not installed"
    [ -f /usr/share/amber/keybindings.json ] || fail "keybindings.json is not installed"
    [ -d /usr/lib/amber/plugins ] || fail "plugins are not installed under /usr/lib/amber"
    # From the package's file list, not the filesystem: container images
    # routinely exclude /usr/share/man via dpkg's path-exclude, and the
    # question here is whether the package ships the page.
    dpkg -L amber-agent | grep -q '/man/man1/amber\.1' || fail "the package ships no man page"
    ok "runtime data, plugins and man page are installed"
    ;;
dnf)
    [ -f /usr/share/amber/completions.json ] || fail "completions.json is not installed"
    [ -d /usr/libexec/amber/plugins ] || fail "plugins are not installed under /usr/libexec/amber"
    rpm -ql amber-agent | grep -q '/man/man1/amber\.1' || fail "the package ships no man page"
    ok "runtime data, plugins and man page are installed"
    ;;
esac

echo "verify: --- negative: a tampered index must be refused ---"
case "$manager" in
apt)
    rm -rf /tmp/tampered && cp -a "$repo" /tmp/tampered
    # Both the compressed and the plain index: apt fetches Packages.gz when the
    # Release lists it, and tampering only with the file it does not fetch
    # proves nothing. Expanded into variables because dash does not glob a
    # redirection target.
    for index in /tmp/tampered/apt/dists/stable/main/binary-*/Packages \
                 /tmp/tampered/apt/dists/stable/main/binary-*/Packages.gz; do
        printf '\n' >> "$index"
    done
    cat > /etc/apt/sources.list.d/amber-tampered.sources <<EOF
Types: deb
URIs: file:///tmp/tampered/apt
Suites: stable
Components: main
Signed-By: /usr/share/keyrings/amber-agent-archive-keyring.gpg
EOF
    if apt-get update \
        -o Dir::Etc::sourcelist=/etc/apt/sources.list.d/amber-tampered.sources \
        -o Dir::Etc::sourceparts=- \
        -o APT::Get::List-Cleanup=0 >/tmp/apt-tampered.log 2>&1; then
        cat /tmp/apt-tampered.log
        fail "apt accepted an index that does not match the signed Release"
    fi
    ok "apt refused a tampered index"

    # The same repository, untouched, with the signature requirement removed:
    # the failure has to come from the missing signature alone. It is copied
    # rather than referenced so apt cannot answer from the list it already
    # fetched from the original URI.
    rm -rf /tmp/unsigned && cp -a "$repo" /tmp/unsigned
    cat > /etc/apt/sources.list.d/amber-unsigned.sources <<EOF
Types: deb
URIs: file:///tmp/unsigned/apt
Suites: stable
Components: main
EOF
    rm -f /etc/apt/sources.list.d/amber-tampered.sources
    if apt-get update \
        -o Dir::Etc::sourcelist=/etc/apt/sources.list.d/amber-unsigned.sources \
        -o Dir::Etc::sourceparts=- \
        -o APT::Get::List-Cleanup=0 >/tmp/apt-unsigned.log 2>&1; then
        cat /tmp/apt-unsigned.log
        fail "apt accepted the repository with no Signed-By"
    fi
    ok "apt refused the repository once Signed-By was removed"
    rm -f /etc/apt/sources.list.d/amber-unsigned.sources
    ;;
dnf)
    rpm -K "$repo"/rpm/fedora/*/amber-agent-*.rpm >/tmp/rpm-k.log 2>&1 ||
        fail "rpm --checksig rejected a published package: $(cat /tmp/rpm-k.log)"
    grep -q 'signatures OK' /tmp/rpm-k.log || fail "the rpm carries no signature"
    ok "the rpm signature verifies"

    rm -rf /tmp/tampered && cp -a "$repo" /tmp/tampered
    # Expanded first: dash does not glob a redirection target.
    repomd="$(ls /tmp/tampered/rpm/fedora/*/repodata/repomd.xml)"
    printf ' ' >> "$repomd"
    cat > /etc/yum.repos.d/amber-tampered.repo <<EOF
[amber-tampered]
name=amber-tampered
baseurl=file:///tmp/tampered/rpm/fedora/\$basearch/
enabled=1
gpgcheck=1
repo_gpgcheck=1
gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-amber-agent
skip_if_unavailable=False
EOF
    # The assertion is the signature error in the output, not the exit status:
    # dnf reports a bad signature and then carries on without the repository,
    # which is a refusal but not a failure.
    dnf --disablerepo='*' --enablerepo=amber-tampered -y makecache \
        >/tmp/dnf-tampered.log 2>&1 || true
    if grep -qi 'signature' /tmp/dnf-tampered.log; then
        ok "dnf refused a tampered repomd.xml"
    else
        cat /tmp/dnf-tampered.log
        fail "dnf did not reject metadata that does not match its signature"
    fi
    rm -f /etc/yum.repos.d/amber-tampered.repo
    ;;
esac

printf '\nverify: PASSED on %s\n' "$(cat /etc/os-release | sed -n 's/^PRETTY_NAME=//p' | tr -d '"')"
