#ifndef PHASE2_JSON_UTILS_H
#define PHASE2_JSON_UTILS_H

#include <cstddef>
#include <string>

namespace phase2 {
namespace detail {

// POSIX filenames may contain arbitrary bytes. Preserve valid UTF-8 and replace
// malformed sequences, so diagnostic messages always produce valid JSON text.
inline std::string jsonString(const std::string& value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (std::size_t offset = 0; offset < value.size();) {
        const auto first = static_cast<unsigned char>(value[offset]);
        if (first < 0x80) {
            if (first == '"' || first == '\\') {
                result += '\\';
                result += static_cast<char>(first);
            } else if (first < 0x20) {
                result += "\\u00";
                result += hex[first >> 4];
                result += hex[first & 0x0f];
            } else {
                result += static_cast<char>(first);
            }
            ++offset;
            continue;
        }
        std::size_t length = 0;
        if (first >= 0xc2 && first <= 0xdf) length = 2;
        else if (first >= 0xe0 && first <= 0xef) length = 3;
        else if (first >= 0xf0 && first <= 0xf4) length = 4;
        bool valid = length != 0 && length <= value.size() - offset;
        for (std::size_t index = 1; valid && index < length; ++index) {
            const auto next = static_cast<unsigned char>(value[offset + index]);
            valid = next >= 0x80 && next <= 0xbf;
            if (index == 1) {
                if (first == 0xe0 && next < 0xa0) valid = false;
                if (first == 0xed && next >= 0xa0) valid = false;
                if (first == 0xf0 && next < 0x90) valid = false;
                if (first == 0xf4 && next >= 0x90) valid = false;
            }
        }
        if (valid) {
            result.append(value, offset, length);
            offset += length;
        } else {
            result += "\\ufffd";
            ++offset;
        }
    }
    return result + '"';
}

}  // namespace detail
}  // namespace phase2

#endif
