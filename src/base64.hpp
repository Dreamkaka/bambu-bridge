#ifndef BAMBU_BRIDGE_BASE64_HPP
#define BAMBU_BRIDGE_BASE64_HPP

#include <string>
#include <string_view>

namespace bambu {

inline constexpr std::string_view base64_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline std::string base64_encode(std::string_view bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        unsigned a = static_cast<unsigned char>(bytes[i]);
        unsigned b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
        unsigned c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
        out += base64_alphabet[a >> 2];
        out += base64_alphabet[((a & 3) << 4) | (b >> 4)];
        out += i + 1 < bytes.size() ? base64_alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        out += i + 2 < bytes.size() ? base64_alphabet[c & 63] : '=';
    }
    return out;
}

} // namespace bambu

#endif
