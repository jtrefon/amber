#!/bin/sh
# Assemble the signed apt and rpm repository tree that GitHub Pages serves.
#
#   build-repo.sh <deb-dir> <rpm-dir> <out-dir>
#
# The caller sets GNUPGHOME and GPG_KEY_ID, and GPG_PASSPHRASE when the key is
# protected (it always is in CI). Everything written here is what a client
# verifies: the InRelease signature, the repomd.xml.asc signature and the
# signature on each rpm.
#
# SPDX-License-Identifier: Apache-2.0

set -eu

deb_in=${1:?usage: build-repo.sh <deb-dir> <rpm-dir> <out-dir>}
rpm_in=${2:?usage: build-repo.sh <deb-dir> <rpm-dir> <out-dir>}
out=${3:?usage: build-repo.sh <deb-dir> <rpm-dir> <out-dir>}

: "${GPG_KEY_ID:?GPG_KEY_ID must name the signing key}"
SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-$(date +%s)}
export SOURCE_DATE_EPOCH

# This script begins with rm -rf, so an output path that is a filesystem root or
# the current directory is refused rather than trusted.
case "$out" in
    / | . | .. | "") printf 'build-repo: refusing to clean %s\n' "$out" >&2; exit 2 ;;
esac

log() { printf 'build-repo: %s\n' "$*"; }
die() { printf 'build-repo: error: %s\n' "$*" >&2; exit 1; }

gpg_sign() {
    # --pinentry-mode loopback is what makes an unattended signature possible
    # for a passphrase-protected key; without it gpg waits for a pinentry that
    # does not exist on a runner and the job hangs.
    if [ -n "${GPG_PASSPHRASE:-}" ]; then
        printf '%s' "$GPG_PASSPHRASE" | gpg --batch --yes --pinentry-mode loopback \
            --passphrase-fd 0 --local-user "$GPG_KEY_ID" "$@"
    else
        gpg --batch --yes --local-user "$GPG_KEY_ID" "$@"
    fi
}

apt_pool="$out/apt/pool/main/a/amber-agent"
# dnf substitutes $basearch in the .repo file, so the directory has to be named
# the way rpm names this machine.
rpm_arch="$(rpm --eval '%{_arch}' 2>/dev/null || echo x86_64)"
rpm_pool="$out/rpm/fedora/$rpm_arch"
dists="$out/apt/dists/stable"

rm -rf "$out"
mkdir -p "$apt_pool" "$rpm_pool" "$dists" "$out/keys"

# --- packages --------------------------------------------------------------

for deb in "$deb_in"/*.deb; do
    [ -e "$deb" ] || die "no .deb files in $deb_in"
    cp "$deb" "$apt_pool/"
done

for rpmfile in "$rpm_in"/*.rpm; do
    [ -e "$rpmfile" ] || die "no .rpm files in $rpm_in"
    cp "$rpmfile" "$rpm_pool/"
done

# Retention: the current release plus the two before it. Packages are ~1.5 MB,
# and a repository that grows without bound eventually runs into the 1 GB Pages
# ceiling. Nothing is lost: any version is rebuildable from its tag.
retain=3
for dir in "$apt_pool" "$rpm_pool"; do
    files="$(find "$dir" -maxdepth 1 -type f \( -name '*.deb' -o -name '*.rpm' \) |
        sort -V)"
    count="$(printf '%s\n' "$files" | grep -c . || true)"
    if [ "$count" -gt "$retain" ]; then
        printf '%s\n' "$files" | head -n "-$retain" |
            while read -r old; do rm -f "$old"; done
    fi
done

# --- apt -------------------------------------------------------------------

# Sign first: the index records the hashes of the signed files, and a package
# for which no signature exists is not installable with gpgcheck on.
log "signing debs in $apt_pool"
for deb in "$apt_pool"/*.deb; do
    rm -f "$deb.asc"
    gpg_sign --armor --detach-sign --output "$deb.asc" "$deb"
done

log "generating apt index"
# The index is per architecture, and the Release must declare exactly the
# architectures it carries or apt refuses the repository on every other one.
arch=""
for deb in "$apt_pool"/*.deb; do
    candidate="$(dpkg-deb -f "$deb" Architecture)"
    if [ "$candidate" != "all" ]; then
        arch="$candidate"
        break
    fi
done
[ -n "$arch" ] || arch="all"
mkdir -p "$dists/main/binary-$arch"
(
    cd "$out/apt"
    apt-ftparchive packages pool > "$dists/main/binary-$arch/Packages"
    gzip -9nc "$dists/main/binary-$arch/Packages" > "$dists/main/binary-$arch/Packages.gz"
    apt-ftparchive \
        -o APT::FTPArchive::Release::Origin="amber" \
        -o APT::FTPArchive::Release::Label="amber-agent" \
        -o APT::FTPArchive::Release::Suite="stable" \
        -o APT::FTPArchive::Release::Codename="stable" \
        -o APT::FTPArchive::Release::Architectures="$arch" \
        -o APT::FTPArchive::Release::Components="main" \
        -o APT::FTPArchive::Release::Description="amber-agent package repository" \
        release dists/stable > "$dists/Release"
)
gpg_sign --clearsign --output "$dists/InRelease" "$dists/Release"
gpg_sign --armor --detach-sign --output "$dists/Release.gpg" "$dists/Release"

# --- rpm -------------------------------------------------------------------

log "signing rpms in $rpm_pool"
if command -v rpm >/dev/null 2>&1; then
    passfile="$(mktemp)"
    trap 'rm -f "$passfile"' EXIT INT TERM
    if [ -n "${GPG_PASSPHRASE:-}" ]; then
        printf '%s' "$GPG_PASSPHRASE" > "$passfile"
    else
        : > "$passfile"
    fi
    chmod 600 "$passfile"
    # rpm drives gpg itself, and its default command line reads the passphrase
    # from a pipe rpm does not open. Overriding the command is the only form
    # that works without a tty.
    for rpmfile in "$rpm_pool"/*.rpm; do
        rpm --addsign \
            --define "_gpg_name $GPG_KEY_ID" \
            --define "_gpg_path $GNUPGHOME" \
            --define "_gpg_sign_cmd %{__gpg} gpg --batch --no-armor --pinentry-mode loopback --passphrase-file $passfile -u \"%{_gpg_name}\" -sbo %{__signature_filename} %{__plaintext_filename}" \
            "$rpmfile"
    done
else
    die "rpm is not installed: cannot sign the rpms"
fi

log "generating rpm metadata"
createrepo_c --quiet --database "$rpm_pool"
gpg_sign --armor --detach-sign \
    --output "$rpm_pool/repodata/repomd.xml.asc" "$rpm_pool/repodata/repomd.xml"

# --- site ------------------------------------------------------------------

# Resolved from the script so the tree is assembled correctly no matter which
# directory the caller started in.
here="$(cd "$(dirname "$0")" && pwd)"
cp "$here/keys/amber-agent-archive-keyring.asc" "$out/keys/"
cp "$here/keys/fingerprint.txt" "$out/keys/"
cp "$here/install.sh" "$out/install.sh"
cp "$here/repo-index.html" "$out/index.html"
: > "$out/.nojekyll"

# Pages serves 404s for directories without an index; the CNAME file is absent
# on purpose, so the site stays on the github.io hostname it was published
# under (that URL is in every user's sources.list).
log "wrote $out"
