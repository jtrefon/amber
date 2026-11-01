# Pinned analyzer image for local/CI parity.
#
# clang-tidy 18 and cppcheck 2.22 cannot be installed at CI's versions on every
# developer machine (macOS has no clang-tidy-18 package; distro cppcheck is
# older than the checks the gate relies on). This image is the fallback: it
# carries exactly the pins in .github/workflows/ci.yml, so `make
# verify-container` measures the same check sets as the coverage/lint jobs.
#
# Keep the versions in step with ci.yml and tools/gates_manifest.py;
# tests/build_hygiene.sh P17 checks they match.

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential \
      ca-certificates \
      clang \
      clang-format-18 \
      clang-tidy-18 \
      cmake \
      curl \
      git \
      libcurl4-openssl-dev \
      libncursesw5-dev \
      libpcre3-dev \
      make \
      ninja-build \
      pkg-config \
      python3 \
      python3-pip \
      xz-utils \
    && rm -rf /var/lib/apt/lists/*

# The Python gates are pinned in the workflow; pip is the only source for all
# three. --break-system-packages is required by Ubuntu 24.04's PEP 668 setup.
RUN pip3 install --break-system-packages \
      "lizard==1.24.0" \
      "gcovr==8.6"

# cppcheck 2.22.0: the distro package lacks checks such as
# uninitMemberVarNoCtor, so it would quietly analyze less than CI.
ARG CPPCHECK_VERSION=2.22.0
RUN curl -fsSL \
      "https://github.com/danmar/cppcheck/archive/refs/tags/${CPPCHECK_VERSION}.tar.gz" \
      -o /tmp/cppcheck.tar.gz \
    && mkdir -p /tmp/cppcheck \
    && tar -xzf /tmp/cppcheck.tar.gz -C /tmp/cppcheck --strip-components=1 \
    && make -C /tmp/cppcheck -j"$(nproc)" MATCHCOMPILER=yes \
         FILESDIR=/usr/share/cppcheck \
    && make -C /tmp/cppcheck install FILESDIR=/usr/share/cppcheck \
    && rm -rf /tmp/cppcheck /tmp/cppcheck.tar.gz

# Ubuntu ships the versioned names; the Makefile expects the unversioned ones.
RUN ln -sf /usr/bin/clang-tidy-18 /usr/local/bin/clang-tidy \
    && ln -sf /usr/bin/clang-format-18 /usr/local/bin/clang-format

WORKDIR /src
