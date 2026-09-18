# syntax=docker/dockerfile:1
# Builds knxd for an ARM target (BAServer: Debian 11 bullseye aarch64; armhf boards
# such as Ubuntu 20.04/Debian 11-based, armv7l/glibc 2.31). Runs under QEMU emulation
# via buildx -- no cross-toolchain needed. The target architecture is NOT hardcoded
# here: it comes from buildx's own --platform flag (e.g. `--platform linux/arm64` or
# `--platform linux/arm/v7`), which also selects the matching debian:bullseye variant
# below since this FROM has no --platform override of its own.

FROM debian:bullseye AS build

# Bullseye's plain `bullseye`/`bullseye-updates` suites are frozen on
# archive.debian.org (last synced ~2024-08/2025-06) as bullseye ages off the live
# deb.debian.org mirror. BUT bullseye-security is NOT archived yet -- it's still
# actively signed and served from deb.debian.org (still gets point releases). The
# official debian:bullseye image tracks that live security channel, so its baked-in
# package versions (e.g. libc6, perl-base, libsystemd0) only resolve against
# archive.debian.org main/updates + deb.debian.org's still-live security repo
# together -- dropping security entirely (as a prior version of this file did)
# left no source at all for those exact pinned versions. When bullseye-security
# does eventually get archived, expect the host to become archive.debian.org with
# the security suite renamed to the bare codename (that's the pattern archive.debian.org
# used for buster: see archive.debian.org/debian-security/dists/buster/).
#
# Check-Valid-Until is disabled because the archived Release files are intentionally
# past their nominal expiry; apt would otherwise refuse to use them.
RUN set -eux; \
    printf '%s\n' \
      'deb http://archive.debian.org/debian bullseye main contrib non-free' \
      'deb http://archive.debian.org/debian bullseye-updates main contrib non-free' \
      'deb http://deb.debian.org/debian-security bullseye-security main contrib non-free' \
      > /etc/apt/sources.list; \
    rm -f /etc/apt/sources.list.d/*.list /etc/apt/sources.list.d/*.sources 2>/dev/null || true; \
    printf '%s\n' \
      'Acquire::Check-Valid-Until "false";' \
      'Acquire::Retries "5";' \
      > /etc/apt/apt.conf.d/99build

# libfmt-dev matters: without it, knxd's configure falls back to tools/get_libfmt,
# which git-clones fmtlib and builds it with cmake at build time. That makes the build
# depend on GitHub being reachable and on an unpinned 4.x branch. BAServer's native
# build had libfmt-dev installed, so this also keeps the container build faithful.
#
# update + install run in the SAME layer deliberately: a cached `apt-get update`
# from an earlier, now-stale index (e.g. before a mirror rotation) combined with a
# fresh `install` in a later layer is exactly how the original 404s crept in.
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

# Export stage: `--output type=tar` (or type=local) copies this filesystem to the host.
FROM scratch AS artifact
COPY --from=build /out /
