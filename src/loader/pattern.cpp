// IDA-style signature scanning: "8B 0D ?? ?? ?? ?? 85 C9 74 ?"
#include "internal.h"

#include <cctype>

namespace hml {

void* pattern_find(const char* pattern, const uint8_t* begin, const uint8_t* end) {
    if (!pattern || !begin || begin >= end) return nullptr;

    std::vector<int> bytes;  // -1 = wildcard
    for (const char* p = pattern; *p;) {
        if (std::isspace(static_cast<unsigned char>(*p))) {
            ++p;
        } else if (*p == '?') {
            bytes.push_back(-1);
            while (*p == '?') ++p;
        } else if (std::isxdigit(static_cast<unsigned char>(p[0])) &&
                   std::isxdigit(static_cast<unsigned char>(p[1]))) {
            char hex[3] = {p[0], p[1], 0};
            bytes.push_back(static_cast<int>(std::strtoul(hex, nullptr, 16)));
            p += 2;
        } else {
            LOG_W("find_pattern: bad pattern near \"%s\"", p);
            return nullptr;
        }
    }
    if (bytes.empty() || static_cast<size_t>(end - begin) < bytes.size()) return nullptr;

    const size_t n = bytes.size();
    for (const uint8_t* at = begin; at + n <= end; ++at) {
        size_t i = 0;
        while (i < n && (bytes[i] < 0 || at[i] == bytes[i])) ++i;
        if (i == n) return const_cast<uint8_t*>(at);
    }
    return nullptr;
}

}  // namespace hml
