#!/bin/sh
# SPDX-License-Identifier: MIT
# build.sh [eboot.elf]: builds build/bbcoop.so for bbport (desktop and Steam Deck) and,
# given the decrypted 1.09 ELF, checks every patch and function byte in addrs.hpp
# against it.
#
# The plugin uses no C++ library at all (no std::, no exceptions, no RTTI),
# so it needs only libc, up to GLIBC_2.38 (the Steam Deck build's); the check below fails a
# build that asks more.
set -eu
cd "$(dirname "$0")"
mkdir -p build
CXX=${CXX:-g++}
FLAGS="-std=c++17 -O2 -fPIC -fvisibility=hidden -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-asynchronous-unwind-tables"
WARN="-Wall -Wextra -Wformat=2 -Wshadow -Wconversion -Wno-sign-conversion -Wno-missing-field-initializers -Werror"
$CXX $FLAGS $WARN -I../../gpu/shim/plugins -shared -nostdlib++ -static-libgcc -Wl,--as-needed -Wl,-z,defs -Wl,-z,now \
    -o build/bbcoop.so bbcoop.cpp
$CXX -std=c++17 -O2 $WARN -o build/verify tools/verify.cpp

# What it asks of the system: libc only, and its newest symbol version.
needed=$(readelf -d build/bbcoop.so | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | tr '\n' ' ')
newest=$(readelf -V build/bbcoop.so | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
echo "build/bbcoop.so: needs ${needed}; newest symbol version ${newest}"
case "$needed" in *stdc++*) echo "error: links libstdc++" >&2; exit 1 ;; esac
max=GLIBC_2.38
if [ "$(printf '%s\n%s\n' "$newest" "$max" | sort -V | tail -1)" != "$max" ]; then
    echo "error: needs ${newest}, newer than the Steam Deck build's ${max}" >&2
    exit 1
fi
for sym in bb_plugin_info bb_plugin_options bb_plugin_init bb_plugin_image; do
    nm -D --defined-only build/bbcoop.so | grep -q " $sym\$" || { echo "error: $sym not exported" >&2; exit 1; }
done
echo "exports: $(nm -D --defined-only build/bbcoop.so | awk '{print $3}' | grep '^bb_' | tr '\n' ' ')"
if [ $# -ge 1 ]; then
    ./build/verify "$1"
fi
