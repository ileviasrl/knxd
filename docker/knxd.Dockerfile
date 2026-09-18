# syntax=docker/dockerfile:1
# Builds knxd for an ARM target (BAServer: Debian 11 bullseye aarch64; armhf boards
# such as Ubuntu 20.04/Debian 11-based, armv7l/glibc 2.31). Runs under QEMU emulation
# via buildx -- no cross-toolchain needed. The target architecture is NOT hardcoded
# here: it comes from buildx's own --platform flag (e.g. `--platform linux/arm64` or
# `--platform linux/arm/v7`), which also selects the matching debian:bullseye variant
# below since this FROM has no --platform override of its own.

FROM debian:bullseye AS build

# libfmt-dev matters: without it, knxd's configure falls back to tools/get_libfmt,
# which git-clones fmtlib and builds it with cmake at build time. That makes the build
# depend on GitHub being reachable and on an unpinned 4.x branch. BAServer's native
# build had libfmt-dev installed, so this also keeps the container build faithful.
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
