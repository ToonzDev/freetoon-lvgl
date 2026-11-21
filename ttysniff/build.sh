#!/bin/sh
# Build libttysniff.so for the Toon 2 (run in WSL/Linux with the Linaro
# armhf toolchain, same TC_ROOT as lvgl_ui_recovered/src/Makefile).
set -e
cd "$(dirname "$0")"
TC_ROOT=${TC_ROOT:-$HOME/toolchains}
T=$TC_ROOT/linaro
CROSS=$T/bin/arm-linux-gnueabihf-
${CROSS}gcc --sysroot=$T/arm-linux-gnueabihf/libc -shared -fPIC -O2 -Wall -Wextra \
    -o libttysniff.so ttysniff.c -ldl
${CROSS}strip libttysniff.so
# Same rule as `make abi-check`: nothing newer than the Toon's glibc 2.21.
bad=$(${CROSS}readelf -V libttysniff.so | grep -oE 'GLIBC_2\.[0-9]+' | sort -uV \
      | awk -F. '$2+0 > 21 {print}')
if [ -n "$bad" ]; then echo "FAIL: needs $bad"; exit 1; fi
echo "OK: libttysniff.so $(wc -c < libttysniff.so) bytes, glibc <= 2.21"
${CROSS}readelf --dyn-syms libttysniff.so | awk '$7=="UND"{print $8}' | sort -u | tr '\n' ' '; echo
