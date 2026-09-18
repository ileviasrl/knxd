# syntax=docker/dockerfile:1
# Builds knxd for an ARM target (BAServer: Debian 11 bullseye aarch64; armhf boards
# such as Ubuntu 20.04/Debian 11-based, armv7l/glibc 2.31). Runs under QEMU emulation
# via buildx -- no cross-toolchain needed. The target architecture is NOT hardcoded
# here: it comes from buildx's own --platform flag (e.g. `--platform linux/arm64` or
# `--platform linux/arm/v7`), which also selects the matching debian:bullseye variant
# below since this FROM has no --platform override of its own.

FROM debian:bullseye AS build

# Getting a working apt source for bullseye took three failed attempts, so this is
# spelled out in full:
#
# 1. Plain deb.debian.org for everything -> 404s on debian-security .deb files.
# 2. archive.debian.org for main+updates+security -> archive.debian.org has NO
#    bullseye-security Release file at all (never mirrored that suite).
# 3. archive.debian.org main+updates, deb.debian.org for the still-signed
#    bullseye-security suite -> `apt-get update` succeeds (the Release/Packages
#    metadata for bullseye-security is still being published, dated ~2026-09-12),
#    but `apt-get install` 404s on the ACTUAL .deb files under
#    security.debian.org/.../pool/updates/main/... for exactly the packages this
#    base image has baked in (libc6-dev, perl, openssl, libsystemd-dev, etc, on
#    every architecture, not just one). Verified live: the pool has been drained
#    now that bullseye is past its support window, while the index listing those
#    filenames hasn't been pruned to match yet.
#
# Fix: snapshot.debian.org, pinned to a verified-good timestamp, for all three
# suites. Unlike the live mirrors, snapshot keeps the actual .deb bytes forever,
# and this specific timestamp was confirmed (by downloading real package files,
# not just probing headers) to carry the exact versions already baked into this
# base image -- so no version pinning or base-image downgrade is needed.
#
# Check-Valid-Until is disabled because snapshot's Release files are, by design,
# long past their nominal expiry; apt would otherwise refuse to use them.
ARG DEBIAN_SNAPSHOT=20260901T000000Z
RUN set -eux; \
    printf '%s\n' \
      "deb http://snapshot.debian.org/archive/debian/${DEBIAN_SNAPSHOT}/ bullseye main contrib non-free" \
      "deb http://snapshot.debian.org/archive/debian/${DEBIAN_SNAPSHOT}/ bullseye-updates main contrib non-free" \
      "deb http://snapshot.debian.org/archive/debian-security/${DEBIAN_SNAPSHOT}/ bullseye-security main contrib non-free" \
      > /etc/apt/sources.list; \
    rm -f /etc/apt/sources.list.d/*.list /etc/apt/sources.list.d/*.sources 2>/dev/null || true; \
    printf '%s\n' \
      'Acquire::Check-Valid-Until "false";' \
      'Acquire::Retries "8";' \
      'Acquire::http::Timeout "180";' \
      'Acquire::http::No-Cache "true";' \
      > /etc/apt/apt.conf.d/99snapshot

# libfmt-dev matters: without it, knxd's configure falls back to tools/get_libfmt,
# which git-clones fmtlib and builds it with cmake at build time. That makes the build
# depend on GitHub being reachable and on an unpinned 4.x branch. BAServer's native
# build had libfmt-dev installed, so this also keeps the container build faithful.
# `file` is needed by the CRLF-stripping step further down, not just as a CLI tool.
#
# update + install run in the SAME layer deliberately: a cached `apt-get update`
# from an earlier, now-stale index (e.g. before a mirror rotation) combined with a
# fresh `install` in a later layer is exactly how the original 404s crept in.
# snapshot.debian.org is also rate-limited and slow under QEMU emulation -- expect
# this layer to take several minutes, hence the generous retry/timeout settings above.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential autoconf automake libtool pkg-config cmake \
        libsystemd-dev libusb-1.0-0-dev libev-dev libfmt-dev \
        git ca-certificates file \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src

# The tree carries CRLF endings from Windows checkouts; autotools chokes on them.
RUN find . -path ./.git -prune -o -type f -print \
      | xargs file \
      | grep "CRLF" | cut -d: -f1 \
      | xargs -r sed -i 's/\r$//'

RUN sh bootstrap.sh \
    && ./configure --prefix=/usr --with-systemd \
    && make -j"$(nproc)" \
    && make install-strip DESTDIR=/out

# `make install-strip DESTDIR=/out` only captures knxd's OWN build output. It does
# NOT capture runtime shared libraries that came from apt (libfmt-dev's runtime
# counterpart, libfmt7) -- those only exist in this build container's system
# paths, so a plain `tar -C / -xf` of /out on the target never sees them and knxd
# fails at startup with:
#   error while loading shared libraries: libfmt.so.7: cannot open shared object file
# Fix: copy the actual runtime .so files apt installed into /out, at the same
# multiarch path the target distro itself uses (e.g. /usr/lib/arm-linux-gnueabihf
# for armhf, /usr/lib/aarch64-linux-gnu for arm64), so extracting the tar onto the
# target drops them straight into its normal library search path -- no rpath or
# extra ldconfig config needed, just `ldconfig` after extraction to refresh the
# cache (the deploy script does this).
RUN set -eux; \
    multiarch="$(dpkg-architecture -qDEB_HOST_MULTIARCH)"; \
    mkdir -p "/out/usr/lib/${multiarch}"; \
    cp -a /usr/lib/"${multiarch}"/libfmt.so.7* "/out/usr/lib/${multiarch}/"

# Export stage: `--output type=tar` (or type=local) copies this filesystem to the host.
FROM scratch AS artifact
COPY --from=build /out /
