// SPDX-License-Identifier: GPL-2.0-or-later
#include <cerrno>
#include <cstring>

#include "common/error.h"

namespace Common {

std::string NativeErrorToString(int e) {
    char buffer[256];
    // GNU strerror_r: returns the message, which may or may not be in `buffer`.
    return std::string{strerror_r(e, buffer, sizeof(buffer))};
}

std::string GetLastErrorMsg() {
    return NativeErrorToString(errno);
}

} // namespace Common
