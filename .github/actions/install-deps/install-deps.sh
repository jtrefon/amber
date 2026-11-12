#!/usr/bin/env bash
# Install apt build dependencies with bounded retries, a fallback mirror and an
# offline path through a restored package cache.
#
# This is the logic behind .github/actions/install-deps. It is a standalone
# shell script so the pinned ubuntu:24.04 containers in the packaging workflow
# can run it too: they have no Python and no actions/cache.
#
# The failure this addresses: on 2026-11-11 the runners could not resolve
# archive.ubuntu.com, security.ubuntu.com or azure.archive.ubuntu.com. Every
# apt job failed with a bare exit 124 after its timeout, which reads like a
# code failure and told nobody what to do. A mirror outage is now retried,
# then routed through a documented fallback, then served from the cache, and
# only then reported as an infrastructure failure -- with the cause in the
# message.

set -uo pipefail

# The script is also called directly from a container, where the workflow's
# top-level env does not reach it. A debconf prompt would hang the install.
export DEBIAN_FRONTEND="${DEBIAN_FRONTEND:-noninteractive}"

ATTEMPTS="${AMBER_APT_ATTEMPTS:-3}"
BACKOFF_SECONDS="${AMBER_APT_BACKOFF_SECONDS:-10}"
UPDATE_TIMEOUT="${AMBER_APT_UPDATE_TIMEOUT:-300}"
UPDATE_BUDGET="${AMBER_APT_UPDATE_BUDGET:-300}"
PROBE_TIMEOUT="${AMBER_APT_PROBE_TIMEOUT:-60}"
OFFLINE_TIMEOUT="${AMBER_APT_OFFLINE_TIMEOUT:-120}"
CACHE_DIR="${AMBER_APT_CACHE_DIR:-}"
# Deliberately not an ubuntu.com host: the outage that motivated this script
# was DNS failure for archive/security/azure.archive.ubuntu.com. apt verifies
# package signatures, so http is safe here and works in a bare container that
# has no ca-certificates yet.
FALLBACKS="${AMBER_APT_FALLBACKS-http://mirrors.edge.kernel.org/ubuntu}"
PRIMARY_SOURCES="${AMBER_APT_PRIMARY_SOURCES:-}"

EXTRA=""
CPP_TOOLCHAIN="false"
PROBE=0

usage() {
    cat <<'EOF'
usage: install-deps.sh [--cpp-toolchain true|false] [--extra "pkg ..."] [--probe]

  --cpp-toolchain  add clang, ncurses and pkg-config to the base set
  --extra          extra apt packages (space separated)
  --probe          only check mirror connectivity, install nothing

Environment:
  AMBER_APT_ATTEMPTS         per-mirror update/install attempts (default 3)
  AMBER_APT_BACKOFF_SECONDS  first retry delay, multiplied by attempt number
  AMBER_APT_UPDATE_BUDGET    total update seconds per mirror (default 300)
  AMBER_APT_INSTALL_BUDGET   total install seconds (default 1800 full, 600 base)
  AMBER_APT_OFFLINE_TIMEOUT  cached-install attempt seconds (default 120)
  AMBER_APT_FALLBACKS        space-separated fallback mirrors (empty disables)
  AMBER_APT_CACHE_DIR        apt archives/lists cache directory (optional)
  AMBER_APT_PRIMARY_SOURCES  primary sources file (auto-detected)
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --extra)
            [ $# -ge 2 ] || { usage >&2; exit 2; }
            EXTRA="$2"
            shift 2
            ;;
        --cpp-toolchain)
            [ $# -ge 2 ] || { usage >&2; exit 2; }
            CPP_TOOLCHAIN="$2"
            shift 2
            ;;
        --probe)
            PROBE=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "install-deps: unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

case "$CPP_TOOLCHAIN" in
    true|false) ;;
    *)
        echo "install-deps: --cpp-toolchain must be true or false" >&2
        exit 2
        ;;
esac

if [ "$(id -u)" -eq 0 ]; then
    SUDO=()
else
    SUDO=(sudo)
fi

if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
fi
CODENAME="${VERSION_CODENAME:-noble}"

if [ -z "$PRIMARY_SOURCES" ]; then
    if [ -f /etc/apt/sources.list.d/ubuntu.sources ]; then
        PRIMARY_SOURCES=/etc/apt/sources.list.d/ubuntu.sources
    elif [ -f /etc/apt/sources.list ]; then
        PRIMARY_SOURCES=/etc/apt/sources.list
    fi
fi

if [ -n "$PRIMARY_SOURCES" ] && [ -r "$PRIMARY_SOURCES" ]; then
    PRIMARY_SOURCE_OPTS=(
        -o "Dir::Etc::sourcelist=$PRIMARY_SOURCES"
        -o "Dir::Etc::sourceparts=-"
    )
else
    PRIMARY_SOURCES=""
    PRIMARY_SOURCE_OPTS=()
fi
SOURCE_OPTS=("${PRIMARY_SOURCE_OPTS[@]}")

PACKAGES="build-essential libcurl4-openssl-dev"
if [ "$CPP_TOOLCHAIN" = "true" ]; then
    PACKAGES="$PACKAGES clang libncursesw5-dev pkg-config"
fi
if [ -n "$EXTRA" ]; then
    PACKAGES="$PACKAGES $EXTRA"
fi

if [ -n "${AMBER_APT_INSTALL_BUDGET:-}" ]; then
    INSTALL_BUDGET="$AMBER_APT_INSTALL_BUDGET"
elif [ "$CPP_TOOLCHAIN" = "true" ]; then
    # The full toolchain pulls ~72 MB; 30 minutes is the measured worst case
    # on a slow mirror. The base set is small, so a 30-minute job still has
    # room for the mirror budget and the diagnostic.
    INSTALL_BUDGET=1800
else
    INSTALL_BUDGET=600
fi

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/amber-apt.XXXXXX")"
LOG="$WORK_DIR/apt.log"
cleanup() { rm -rf "$WORK_DIR"; }
trap cleanup EXIT

APT_OPTS=(
    -o DPkg::Lock::Timeout=120
    -o Acquire::Retries=5
    -o Acquire::ForceIPv4=true
    -o Acquire::http::Timeout=30
    -o Acquire::https::Timeout=30
)
if [ -n "$CACHE_DIR" ]; then
    mkdir -p "$CACHE_DIR/archives/partial" "$CACHE_DIR/lists/partial"
    APT_OPTS+=(
        -o "Dir::Cache=$CACHE_DIR/cache"
        -o "Dir::Cache::archives=$CACHE_DIR/archives"
        -o "Dir::State::lists=$CACHE_DIR/lists"
        # The cache directory belongs to the runner user; apt's default _apt
        # sandbox user cannot write there. The runner is disposable, so root
        # is the simpler answer than reparenting the directory for every user.
        -o "APT::Sandbox::User=root"
    )
fi

write_sources() {
    local mirror="$1" file="$2"
    {
        printf 'deb %s %s main restricted universe multiverse\n' "$mirror" "$CODENAME"
        printf 'deb %s %s-updates main restricted universe multiverse\n' "$mirror" "$CODENAME"
        printf 'deb %s %s-security main restricted universe multiverse\n' "$mirror" "$CODENAME"
    } > "$file"
}

apt_get() {
    local limit="$1"
    shift
    "${SUDO[@]}" timeout "$limit" apt-get "${APT_OPTS[@]}" "$@"
}

# The budget is the important bound: a single slow download may legitimately
# use the whole install window, while a fast failure leaves room for another
# attempt. Without this, three 30-minute attempts could outlive the job.
remaining_limit() {
    local deadline="$1" cap="$2"
    local remaining=$((deadline - $(date +%s)))
    if [ "$remaining" -le 0 ]; then
        echo 0
    elif [ "$remaining" -gt "$cap" ]; then
        echo "$cap"
    else
        echo "$remaining"
    fi
}

apt_update() {
    local limit="$1"
    apt_get "$limit" "${SOURCE_OPTS[@]}" update
}

apt_install() {
    local offline="$1" limit="$2"
    local offline_opts=()
    if [ "$offline" = "true" ]; then
        offline_opts=(--no-download)
    fi
    # PACKAGES is a deliberate word list, not a single argument.
    # shellcheck disable=SC2086
    apt_get "$limit" "${SOURCE_OPTS[@]}" install -y \
        --no-install-recommends "${offline_opts[@]}" $PACKAGES
}

attempt_update() {
    local label="$1"
    local i delay limit
    local deadline=$(( $(date +%s) + UPDATE_BUDGET ))
    for ((i = 1; i <= ATTEMPTS; i++)); do
        limit="$(remaining_limit "$deadline" "$UPDATE_TIMEOUT")"
        if [ "$limit" -le 0 ]; then
            echo "install-deps: update budget exhausted for $label" >&2
            break
        fi
        echo "install-deps: apt-get update via $label (attempt $i/$ATTEMPTS, limit ${limit}s)"
        if apt_update "$limit" >>"$LOG" 2>&1; then
            echo "install-deps: $label is reachable"
            return 0
        fi
        if [ "$i" -lt "$ATTEMPTS" ]; then
            delay=$((BACKOFF_SECONDS * i))
            echo "install-deps: attempt $i failed; retrying in ${delay}s" >&2
            sleep "$delay"
        fi
    done
    return 1
}

attempt_install() {
    local i delay limit
    local deadline=$(( $(date +%s) + INSTALL_BUDGET ))
    for ((i = 1; i <= ATTEMPTS; i++)); do
        limit="$(remaining_limit "$deadline" "$INSTALL_BUDGET")"
        if [ "$limit" -le 0 ]; then
            echo "install-deps: install budget exhausted" >&2
            break
        fi
        echo "install-deps: installing packages (attempt $i/$ATTEMPTS, limit ${limit}s)"
        if apt_install false "$limit" >>"$LOG" 2>&1; then
            return 0
        fi
        if [ "$i" -lt "$ATTEMPTS" ]; then
            delay=$((BACKOFF_SECONDS * i))
            echo "install-deps: install attempt $i failed; retrying in ${delay}s" >&2
            sleep "$delay"
        fi
    done
    return 1
}

try_cached_install() {
    echo "install-deps: trying cached packages only"
    apt_install true "$OFFLINE_TIMEOUT" >>"$LOG" 2>&1
}

cached_success() {
    echo "::warning title=Ubuntu mirror unreachable::installed from the restored apt cache; this run did not contact a mirror." >&2
    echo "install-deps: installed from cache: $PACKAGES"
    exit 0
}

fail_install() {
    if grep -Eq "Unable to locate package|has no installation candidate" "$LOG"; then
        {
            echo "::error title=Package not available::apt could not find one of the requested packages."
            echo "install-deps: requested: $PACKAGES"
            tail -n 15 "$LOG" | sed 's/^/  /'
        } >&2
        exit 1
    fi
    fail_outage
}

fail_outage() {
    {
        echo "::error title=Ubuntu mirror unreachable::apt could not reach the configured mirrors and the cache did not satisfy the request."
        echo "install-deps: Ubuntu mirror unreachable after $ATTEMPTS attempt(s) per mirror."
        echo "install-deps: this is an infrastructure failure, not a code failure; the job compiled nothing."
        echo "install-deps: primary: ${PRIMARY_SOURCES:-default apt sources}"
        for mirror in $FALLBACKS; do
            echo "install-deps: fallback: $mirror"
        done
        echo "install-deps: last apt output:"
        tail -n 25 "$LOG" | sed 's/^/  /'
        echo "install-deps: check https://www.githubstatus.com and re-run the job."
    } >&2
    exit 75
}

probe_mirror() {
    local label="$1"
    shift
    local opts=("$@")
    echo "install-deps: probing $label"
    if apt_get "$PROBE_TIMEOUT" "${opts[@]}" update >>"$LOG" 2>&1; then
        echo "install-deps: $label OK"
        return 0
    fi
    echo "install-deps: $label FAILED" >&2
    tail -n 5 "$LOG" | sed 's/^/  /' >&2
    return 1
}

if [ "$PROBE" -eq 1 ]; then
    primary_ok=0
    if probe_mirror "primary (${PRIMARY_SOURCES:-default apt sources})" "${PRIMARY_SOURCE_OPTS[@]}"; then
        primary_ok=1
    fi
    fallback_ok=0
    fallback_index=0
    for mirror in $FALLBACKS; do
        fallback_index=$((fallback_index + 1))
        file="$WORK_DIR/fallback-${fallback_index}.list"
        write_sources "$mirror" "$file"
        if probe_mirror "fallback $mirror" \
            -o "Dir::Etc::sourcelist=$file" -o "Dir::Etc::sourceparts=-"; then
            fallback_ok=1
        fi
    done
    if [ "$primary_ok" -eq 1 ] || [ "$fallback_ok" -eq 1 ]; then
        if [ "$primary_ok" -eq 0 ]; then
            echo "::warning::the primary Ubuntu mirror is unreachable; the fallback is usable"
        fi
        echo "install-deps: mirror probe passed"
        exit 0
    fi
    echo "::error title=Ubuntu mirrors unreachable::the primary mirror and every configured fallback failed" >&2
    exit 1
fi

updated=0
if attempt_update "primary (${PRIMARY_SOURCES:-default apt sources})"; then
    updated=1
fi

fallback_files=()
if [ "$updated" -eq 0 ]; then
    fallback_index=0
    for mirror in $FALLBACKS; do
        fallback_index=$((fallback_index + 1))
        file="$WORK_DIR/fallback-${fallback_index}.list"
        write_sources "$mirror" "$file"
        fallback_files+=("$file")
        SOURCE_OPTS=(-o "Dir::Etc::sourcelist=$file" -o "Dir::Etc::sourceparts=-")
        if attempt_update "fallback $mirror"; then
            updated=1
            break
        fi
    done
fi

if [ "$updated" -eq 0 ]; then
    SOURCE_OPTS=("${PRIMARY_SOURCE_OPTS[@]}")
    if try_cached_install; then
        cached_success
    fi
    for file in "${fallback_files[@]}"; do
        SOURCE_OPTS=(-o "Dir::Etc::sourcelist=$file" -o "Dir::Etc::sourceparts=-")
        if try_cached_install; then
            cached_success
        fi
    done
    fail_outage
fi

if ! attempt_install; then
    if try_cached_install; then
        cached_success
    fi
    fail_install
fi

echo "install-deps: installed: $PACKAGES"
