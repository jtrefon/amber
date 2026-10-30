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

deb_in=${1:?usage: build-repo.sh <deb-dir> <rpm-dir> <out-dir> [previous-repo]}
rpm_in=${2:?usage: build-repo.sh <deb-dir> <rpm-dir> <out-dir> [previous-repo]}
out=${3:?usage: build-repo.sh <deb-dir> <rpm-dir> <out-dir> [previous-repo]}
previous=${4:-}

: "${GPG_KEY_ID:?GPG_KEY_ID must name the signing key}"
SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-$(date +%s)}
export SOURCE_DATE_EPOCH

# Resolved from the script so paths work no matter which directory the caller
# started in.
here="$(cd "$(dirname "$0")" && pwd)"

# Everything below is written relative to the tree root, and the apt index is
# generated from inside it: an absolute output path means those two views cannot
# disagree. The rehearsal passed an absolute path and CI passed a relative one,
# which is exactly how the double-prefixed path got through.
case "$out" in
    /*) ;;
    *) out="$PWD/$out" ;;
esac

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

# A package's version, read from the package itself. A file name is not the
# place to look: a release is several packages, and they have to be kept or
# dropped together.
package_version() {
    case "$1" in
        *.deb) dpkg-deb -f "$1" Version 2>/dev/null ;;
        *.rpm) rpm -qp --qf '%{VERSION}-%{RELEASE}\n' "$1" 2>/dev/null ;;
        *) printf '\n' ;;
    esac
}

# Retention: the current release plus the two before it, as a set. Anything
# else in the pool goes, which also clears signatures from the version it
# belonged to.
prune_pool() {
    dir=$1
    keep=$(
        for f in "$dir"/*; do
            [ -f "$f" ] || continue
            package_version "$f"
        done | grep . | sort -uV | tail -n 3
    )
    for f in "$dir"/*; do
        [ -f "$f" ] || continue
        printf '%s\n' "$keep" | grep -qxF "$(package_version "$f")" || rm -f "$f"
    done
}

# --- packages --------------------------------------------------------------

for deb in "$deb_in"/*.deb; do
    [ -e "$deb" ] || die "no .deb files in $deb_in"
    cp "$deb" "$apt_pool/"
done

for rpmfile in "$rpm_in"/*.rpm; do
    [ -e "$rpmfile" ] || die "no .rpm files in $rpm_in"
    # Debug and source packages are not published: they are tens of megabytes
    # each, they are optional by design, and they are regenerable from the
    # .src.rpm. A repository that carries them would pass Pages' ceiling in a
    # few releases.
    case "$(basename "$rpmfile")" in
        *-debuginfo-* | *-debugsource-* | *.src.rpm) continue ;;
    esac
    cp "$rpmfile" "$rpm_pool/"
done

# Carry the previous releases forward. Packages are ~1.5 MB, and a repository
# that grows without bound eventually runs into the 1 GB Pages ceiling; this is
# what gives retention something to keep, instead of every publish starting
# from a pool that holds only today's build.
if [ -n "$previous" ] && [ -d "$previous" ]; then
    log "carrying forward packages from $previous"
    for old in "$previous"/apt/pool/main/a/amber-agent/*.deb; do
        [ -e "$old" ] && cp -n "$old" "$apt_pool/"
    done
    for old in "$previous"/rpm/fedora/*/*.rpm; do
        [ -e "$old" ] && cp -n "$old" "$rpm_pool/"
    done
fi

prune_pool "$apt_pool"
prune_pool "$rpm_pool"

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
        release dists/stable > "$dists/Release.new"
    )
    # Written via a temporary file: apt-ftparchive hashes whatever is in the
    # directory it scans, and a shell redirect creates Release before it runs,
    # which leaves the file listing itself.
    mv "$dists/Release.new" "$dists/Release"
gpg_sign --clearsign --output "$dists/InRelease" "$dists/Release"
gpg_sign --armor --detach-sign --output "$dists/Release.gpg" "$dists/Release"

# --- rpm -------------------------------------------------------------------

log "checking rpm signatures in $rpm_pool"
if command -v rpm >/dev/null 2>&1; then
    # Signing happens in the Fedora job, where rpm's own gpg plumbing works;
    # this is the check that it did. A package nobody can verify must fail here
    # rather than at the user's first upgrade.
    #
    # The check runs against a trust database of its own: importing into, and
    # then reading from, whatever database rpm resolves by default is how a
    # signed package got reported as unsigned on the first publish run, and the
    # output is printed so the next failure says why instead of leaving it to
    # be guessed.
    rpm_db="$(mktemp -d)"
    rpm --dbpath "$rpm_db" --import "$here/keys/amber-agent-archive-keyring.asc" ||
        die "cannot import the archive key into $rpm_db"
    for rpmfile in "$rpm_pool"/*.rpm; do
        signature="$(rpm --dbpath "$rpm_db" -K "$rpmfile" 2>&1)" ||
            die "$(basename "$rpmfile") is not signed: $signature"
        printf '%s\n' "$signature" | grep -q 'signatures OK' ||
            die "$(basename "$rpmfile") is not signed: $signature"
    done
    log "  $(ls "$rpm_pool"/*.rpm | wc -l) package(s) verified"
    rm -rf "$rpm_db"
else
    die "rpm is not installed: cannot check the rpm signatures"
fi

log "generating rpm metadata"
createrepo_c --quiet --database "$rpm_pool"
gpg_sign --armor --detach-sign \
    --output "$rpm_pool/repodata/repomd.xml.asc" "$rpm_pool/repodata/repomd.xml"

# --- site ------------------------------------------------------------------

cp "$here/keys/amber-agent-archive-keyring.asc" "$out/keys/"
cp "$here/keys/fingerprint.txt" "$out/keys/"
cp "$here/install.sh" "$out/install.sh"
cp "$here/repo-index.html" "$out/index.html"
: > "$out/.nojekyll"

# Pages serves 404s for directories without an index; the CNAME file is absent
# on purpose, so the site stays on the github.io hostname it was published
# under (that URL is in every user's sources.list).
log "wrote $out"
