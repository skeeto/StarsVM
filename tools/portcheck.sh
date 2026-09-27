#!/bin/sh
# portcheck.sh - the integer x87, and the library on it, wherever a Linux
# box can take them: `make portcheck` does what a Windows machine alone can,
# and this does the rest.
#
#   x80test under gcc, clang, gcc -m32 and UBSan
#   the library with FPU=soft, on this machine: libtest
#   x80test and libtest built for AArch64 and run under qemu-user
#
# Run from the top of a checkout with stars.exe and bench/ copied in (they
# are not in the repository), on a Debian-ish system with gcc, clang,
# gcc-multilib, gcc-aarch64-linux-gnu and qemu-user.  Each build gets its own
# directory under build/, so nothing here disturbs another build.  The
# AArch64 runs take a while: libtest generates turns under emulation.

set -e
say() { printf '\n=== %s\n' "$*"; }

say "x80test: gcc"
make -s x80test CROSS=
say "x80test: clang"
make -s x80test CROSS= CC=clang
say "x80test: gcc -m32"
make -s x80test CROSS= CC="gcc -m32"
say "x80test: gcc, UBSan"
make -s x80test CROSS= XCFLAGS="-fsanitize=undefined -fno-sanitize-recover=all"

say "libtest: FPU=soft, this machine"
make -s libtest CROSS= FPU=soft

# Statically linked, so qemu needs no AArch64 sysroot for x80test; libtest
# links libpthread, so it is given the cross toolchain's as its root.
say "x80test: AArch64 under qemu"
make -s x80test CROSS=aarch64-linux-gnu- XCFLAGS=-static
say "libtest: AArch64 under qemu"
QEMU_LD_PREFIX=/usr/aarch64-linux-gnu make -s libtest CROSS=aarch64-linux-gnu-

say "portcheck.sh: passed"
