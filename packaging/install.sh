#!/bin/sh
# amber-agent repository installer.
#
# Adds the signed apt or dnf repository, then installs the package from it. The
# script is served from the same host as the repository, so reading it before
# running it is the only check it cannot do for itself; the signing key
# fingerprint below is pinned so a substitute key cannot be swapped in.
#
#   curl -fsSL https://jtrefon.github.io/amber-packages/install.sh | sudo sh
#
# SPDX-License-Identifier: Apache-2.0

set -eu

BASE_URL="${AMBER_REPO_BASE_URL:-https://jtrefon.github.io/amber-packages}"
KEY_FPR="C373A7C2F4BB30ABEB1B35DAAAFCEAE6C29E0D1B"
APT_KEYRING="/usr/share/keyrings/amber-agent-archive-keyring.gpg"
APT_SOURCES="/etc/apt/sources.list.d/amber-agent.sources"
RPM_KEY="/etc/pki/rpm-gpg/RPM-GPG-KEY-amber-agent"
RPM_REPO="/etc/yum.repos.d/amber-agent.repo"

dry_run=0
action="install"
tmp=""

usage() {
    cat <<EOF
Usage: install.sh [--dry-run] [--uninstall]

  --dry-run     print what would be written and run, change nothing
  --uninstall   remove the repository definition and signing key; the package
                itself is left installed (remove it with your package manager)

Adds https://jtrefon.github.io/amber-packages and installs amber-agent.
EOF
}

log() { printf 'amber-agent: %s\n' "$*"; }
die() { printf 'amber-agent: error: %s\n' "$*" >&2; exit 1; }

cleanup() { if [ -n "$tmp" ]; then rm -f "$tmp"; fi; }
trap cleanup EXIT INT TERM

for arg in "$@"; do
    case "$arg" in
        --dry-run)   dry_run=1 ;;
        --uninstall) action="uninstall" ;;
        -h|--help)   usage; exit 0 ;;
        *)           usage >&2; exit 2 ;;
    esac
done

# Root is needed to write /etc and /usr/share/keyrings. Re-exec once when the
# script is a file on disk; when it arrived through a pipe there is nothing to
# re-exec, so say what to run instead of failing on a missing path.
if [ "$(id -u)" -ne 0 ] && [ "$dry_run" -eq 0 ]; then
    if [ -f "$0" ]; then
        command -v sudo >/dev/null 2>&1 || die "run this as root, or install sudo"
        exec sudo sh "$0" "$@"
    fi
    die "run this as root: curl -fsSL $BASE_URL/install.sh | sudo sh"
fi

have() { command -v "$1" >/dev/null 2>&1; }

fetch() {
    if have curl; then
        curl -fsSL "$1" -o "$2"
    elif have wget; then
        wget -qO "$2" "$1"
    else
        die "need curl or wget to download the key"
    fi
}

manager=""
if have apt-get; then
    manager="apt"
elif have dnf; then
    manager="dnf"
elif have dnf5; then
    manager="dnf5"
else
    die "no apt-get or dnf found: this script supports Debian/Ubuntu (including WSL) and Fedora"
fi

need_gpg() {
    if ! have gpg; then
        log "installing gnupg (needed to verify the archive key)"
        if [ "$manager" = "apt" ]; then
            apt-get update -qq && apt-get install -y --no-install-recommends gnupg
        else
            "$manager" install -y gnupg2
        fi
    fi
}

# Download the armored key and refuse to continue unless its primary
# fingerprint is the one pinned above.
fetch_verified_key() {
    need_gpg
    tmp="$(mktemp)"
    fetch "$BASE_URL/keys/amber-agent-archive-keyring.asc" "$tmp" ||
        die "could not download the archive key"
    actual="$(gpg --show-keys --with-colons "$tmp" 2>/dev/null |
        awk -F: '/^fpr:/ { print $10; exit }')"
    [ -n "$actual" ] || die "the downloaded file is not an OpenPGP key"
    if [ "$actual" != "$KEY_FPR" ]; then
        die "key fingerprint mismatch: expected $KEY_FPR, got $actual"
    fi
    log "key fingerprint verified: $KEY_FPR"
}

if [ "$action" = "uninstall" ]; then
    if [ "$dry_run" -eq 1 ]; then
        log "would remove $APT_SOURCES $APT_KEYRING $RPM_REPO $RPM_KEY"
        exit 0
    fi
    rm -f "$APT_SOURCES" "$APT_KEYRING" "$RPM_REPO" "$RPM_KEY"
    log "repository removed; amber-agent itself is still installed"
    exit 0
fi

fetch_verified_key

if [ "$manager" = "apt" ]; then
    if [ "$dry_run" -eq 1 ]; then
        log "would write $APT_KEYRING and $APT_SOURCES, then apt-get install amber-agent"
        exit 0
    fi
    install -d -m 755 "$(dirname "$APT_KEYRING")" "$(dirname "$APT_SOURCES")"
    gpg --dearmor --yes --output "$APT_KEYRING" "$tmp"
    chmod 644 "$APT_KEYRING"
    cat > "$APT_SOURCES" <<EOF
Types: deb
URIs: $BASE_URL/apt
Suites: stable
Components: main
Signed-By: $APT_KEYRING
EOF
    # Refresh only this list, so an unrelated broken repository cannot stop the
    # install. The next ordinary "apt-get update" picks the file up as usual.
    apt-get update \
        -o Dir::Etc::sourcelist="$APT_SOURCES" \
        -o Dir::Etc::sourceparts="-" \
        -o APT::Get::List-Cleanup="0"
    DEBIAN_FRONTEND=noninteractive apt-get install -y amber-agent
else
    if [ "$dry_run" -eq 1 ]; then
        log "would write $RPM_KEY and $RPM_REPO, then install amber-agent"
        exit 0
    fi
    install -d -m 755 "$(dirname "$RPM_KEY")" "$(dirname "$RPM_REPO")"
    install -m 644 "$tmp" "$RPM_KEY"
    cat > "$RPM_REPO" <<EOF
[amber-agent]
name=amber-agent
baseurl=$BASE_URL/rpm/fedora/\$basearch/
enabled=1
gpgcheck=1
repo_gpgcheck=1
gpgkey=file://$RPM_KEY
skip_if_unavailable=False
EOF
    "$manager" --disablerepo='*' --enablerepo=amber-agent -y install amber-agent
fi

log "installed: $(amber-cli --version 2>/dev/null || echo 'amber-agent')"
cat <<EOF
amber-agent: run 'amber' for the terminal client or 'amber-cli --help' for the
headless one. Future upgrades arrive with your normal system updates; after one,
restart amber to run the new build.
EOF
