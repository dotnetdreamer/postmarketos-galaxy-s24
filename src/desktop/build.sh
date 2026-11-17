#!/usr/bin/env bash
# build.sh: the S24 mouse mode's two C programs, built for the phone (aarch64 Alpine musl) in
# the openandroiddex-pmos build image (clang, lld and an aarch64 Alpine sysroot with
# linux-headers and wayland-dev). Claude, 2026-11-15.
set -euo pipefail
cd "$(dirname "$0")"
HOST_DIR=$(pwd -W 2>/dev/null || pwd)
MSYS_NO_PATHCONV=1 docker run --rm -v "$HOST_DIR:/src" -w /src openandroiddex-pmos-build sh -euc '
  export PKG_CONFIG_SYSROOT_DIR=/sysroot PKG_CONFIG_LIBDIR=/sysroot/usr/lib/pkgconfig:/sysroot/usr/share/pkgconfig
  cc="clang --target=aarch64-alpine-linux-musl --sysroot=/sysroot -fuse-ld=lld -O2 -Wall -Wextra -Wno-unused-parameter"
  mkdir -p out /tmp/gen
  xml=/usr/share/plasma-wayland-protocols/dpms.xml
  wayland-scanner client-header $xml /tmp/gen/dpms-client-protocol.h
  wayland-scanner private-code $xml /tmp/gen/dpms-protocol.c
  $cc -o out/s24-mouse-moded s24-mouse-moded.c -s
  $cc -I/tmp/gen $(pkg-config --cflags wayland-client) -o out/s24-output-watch s24-output-watch.c /tmp/gen/dpms-protocol.c $(pkg-config --libs wayland-client) -s
'
ls -l out/
