# SPDX-License-Identifier: GPL-2.0-or-later
# bbport: a glslang -V --vn header made usable on its own (uint32_t needs <cstdint>).
file(READ "${HEADER_FILE}" HEADER_CONTENT)
string(REPLACE "#pragma once" "#pragma once\n#include <cstdint>" HEADER_CONTENT "${HEADER_CONTENT}")
file(WRITE "${HEADER_FILE}" "${HEADER_CONTENT}")
