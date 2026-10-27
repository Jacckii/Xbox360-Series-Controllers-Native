#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the Xbox 360 side with OpenXeChain. Run in this folder on the toolchain host:
#   OXC=/path/to/openxechain/sysroot bash build.sh
#
#   out/xa.xex         the driver (system DLL at 0x91500000)
#   out/xa_loader.xex  development loader for it
#   out/xa_m3.xex      a register probe for bring-up (system DLL at 0x91D00000)
set -euo pipefail
cd "$(dirname "$0")"
TC=${OXC:-/opt/openxechain/sysroot}
unset C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH
mkdir -p out
# System DLLs: 4 KB pages (/align:0x1000) and SynthXEX's system-DLL patch (README, Building).
# Never -g (the toolchain's clang crashes emitting DWARF into COFF). lld's
# "/align specified without /driver" warning is expected.
DLL_LINK="-Wl,/entry:DllMain,/align:0x1000"
# SynthXEX maps only the bytes the PE file holds, so zero-initialised data (.bss) would
# lie outside the loaded image. Keep it in .data; check_xex.py fails the build otherwise.
# No floating point: DLL code runs on the kernel's DPCs, which have no FP context, and
# the ppc32 ABI's variadic prologue spills f1-f8 unconditionally (stfd) in every printf-
# style function. check_xex.py fails the build on any FP instruction.
CFLAGS_DLL="-fno-zero-initialized-in-bss -msoft-float"
# XA_HOOKS picks the installed XInput hooks (default: read,caps,set; "read" = input only)
case "${XA_HOOKS:-read,caps,set}" in
    *caps*) CFLAGS_DLL="$CFLAGS_DLL -DXA_HOOK_CAPS=1" ;;
esac
case "${XA_HOOKS:-read,caps,set}" in
    *set*) CFLAGS_DLL="$CFLAGS_DLL -DXA_HOOK_SET=1" ;;
esac

# XA_SLOT=n: build the plugin as out/xa<n>.xex at its own base, and the loader for it, so
# a new build can replace a running one without a reboot (xa.py reload). A loaded
# module's name and address can't be reused until the console reboots. Slot 0 (default)
# is xa.xex at 0x91500000; slot n is xa<n>.xex at 0x92000000 + (n-1) MB.
#
# A system DLL only loads at its own base, so it must not overlap another plugin's.
# 0x91B00000-0x91E70000 is crowded (RPC360 and others sit at 0x91E00000, connectx at
# 0x91E60000); the gap between hud.xex (ends 0x91429000) and ximecore.xex (0x91680000)
# is unused by system modules and known plugins.
SLOT=${XA_SLOT:-0}
if [ "$SLOT" = 0 ]; then
    NAME=xa
    BASE=0x91500000
else
    NAME=xa$SLOT
    BASE=$(printf "0x%08X" $((0x92000000 + (SLOT - 1) * 0x100000)))
fi

# micro-ecc: only P-256, portable C (no assembly)
ECC_FLAGS="-DuECC_SUPPORTS_secp160r1=0 -DuECC_SUPPORTS_secp192r1=0 -DuECC_SUPPORTS_secp224r1=0
    -DuECC_SUPPORTS_secp256k1=0 -DuECC_PLATFORM=uECC_arch_other"
CORE="../core/mt76.c ../core/dongle.c ../core/gip.c ../core/sha256.c ../third_party/micro-ecc/uECC.c"

$TC/bin/clang -O2 -Wall -Wno-unused-parameter $CFLAGS_DLL $ECC_FLAGS $DLL_LINK,/base:$BASE \
    -DXA_NAME="\"$NAME.xex\"" \
    thunks.s xa_plugin.c platform_360.c xinput_360.c detour.c $CORE -o out/$NAME.exe
$TC/bin/synthxex-patched -i out/$NAME.exe -o out/$NAME.xex -t sysdll >/dev/null

$TC/bin/clang -O2 loader.c -Wl,/entry:stage_entry -DPLUGIN_NAME="\"$NAME.xex\"" \
    -o out/xa_loader.exe
$TC/bin/synthxex -i out/xa_loader.exe -o out/xa_loader.xex -t title >/dev/null

$TC/bin/clang++ -O2 -fno-exceptions -fno-rtti $CFLAGS_DLL $DLL_LINK,/base:0x91D00000 \
    m3_probe.cpp -o out/xa_m3.exe
$TC/bin/synthxex-patched -i out/xa_m3.exe -o out/xa_m3.xex -t sysdll >/dev/null

python3 check_xex.py out/xa_loader.exe out/xa_loader.xex
echo "$NAME" > out/built_name # read by xa.py (upload, load, reload)
for x in $NAME xa_m3; do
    python3 check_xex.py --no-fp "$TC/bin/llvm-objdump" out/$x.exe out/$x.xex
done
ls -l out/*.xex
