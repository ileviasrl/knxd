# syntax=docker/dockerfile:1
# Builds knxd for an ARM target (BAServer: Debian 11 bullseye aarch64; armhf boards
# such as Ubuntu 20.04/Debian 11-based, armv7l/glibc 2.31). Runs under QEMU emulation
# via buildx -- no cross-toolchain needed. The target architecture is NOT hardcoded
# here: it comes from buildx's own --platform flag (e.g. `--platform linux/arm64` or
# `--platform linux/arm/v7`), which also selects the matching debian:bullseye variant
# below since this FROM has no --platform override of its own.

FROM debian:bullseye AS build

# Bullseye is aging off deb.debian.org's live mirror: exact point-release .deb files
# (especially for less-mirrored architectures like armhf) start 404ing there before
# the base image itself catches up. archive.debian.org keeps every package for every
# architecture indefinitely, so pin sources there instead of the rolling mirror.
# Archived Release files are also intentionally past their Valid-Until date, so that
# check has to be disabled too, or every apt-get call fails on "Release file expired".
RUN printf 'deb http://archive.debian.org/debian bullseye main\ndeb http://archive.debian.org/debian-security bullseye-security main\ndeb http://archive.debian.org/debian bullseye-updates main\n' > /etc/apt/sources.list \
    && printf 'Acquire::Check-Valid-Until "false";\n' > /etc/apt/apt.conf.d/99no-check-valid-until

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
