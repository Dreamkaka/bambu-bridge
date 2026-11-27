#include "http_websocket.hpp"
#include "base64.hpp"

#include <array>
#include <algorithm>

namespace bambu {
namespace {

uint32_t rol(uint32_t x, unsigned bits) { return (x << bits) | (x >> (32 - bits)); }

// Only used for the RFC 6455 handshake. Its input has a fixed length of 60 bytes.
std::array<char, 20> accept_digest(std::string_view key) {
    constexpr std::string_view guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::array<uint8_t, 128> data{};
    std::copy(key.begin(), key.end(), data.begin());
    std::copy(guid.begin(), guid.end(), data.begin() + 24);
    data[60] = 0x80;
    data[126] = 1; data[127] = 224; // 480-bit message length
    std::array<uint32_t, 5> hash{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    for (size_t block = 0; block < data.size(); block += 64) {
        uint32_t words[80];
        for (unsigned i = 0; i < 16; ++i) {
            const auto* p = data.data() + block + i * 4;
            words[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        }
        for (unsigned i = 16; i < 80; ++i)
            words[i] = rol(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
        auto [a, b, c, d, e] = hash;
        for (unsigned i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
            else { f = b ^ c ^ d; k = 0xca62c1d6; }
            const uint32_t sum = rol(a, 5) + f + e + k + words[i];
            e = d; d = c; c = rol(b, 30); b = a; a = sum;
        }
        hash[0] += a; hash[1] += b; hash[2] += c; hash[3] += d; hash[4] += e;
    }
    std::array<char, 20> digest{};
    for (size_t i = 0; i < hash.size(); ++i)
        for (unsigned j = 0; j < 4; ++j) digest[i * 4 + j] = static_cast<char>(hash[i] >> (24 - j * 8));
    return digest;
}

} // namespace

std::optional<std::string> websocket_accept(std::string_view key) {
    if (key.size() != 24 || key.substr(22) != "==" ||
        key.substr(0, 22).find_first_not_of(base64_alphabet) != std::string_view::npos ||
        (base64_alphabet.find(key[21]) & 15u) != 0) return std::nullopt;
    const auto digest = accept_digest(key);
    return base64_encode(std::string_view(digest.data(), digest.size()));
}

std::string websocket_frame(uint8_t opcode, std::string_view payload) {
    const uint64_t size = payload.size();
    const unsigned extra = size <= 125 ? 0 : size <= 65535 ? 2 : 8;
    std::string wire;
    wire.reserve(2 + extra + payload.size());
    wire += static_cast<char>(0x80 | opcode);
    wire += static_cast<char>(extra == 0 ? size : extra == 2 ? 126 : 127);
    for (unsigned i = extra; i > 0; --i) wire += static_cast<char>(size >> ((i - 1) * 8));
    wire.append(payload);
    return wire;
}

bool websocket_utf8_valid(std::string_view text) {
    for (size_t i = 0; i < text.size();) {
        const auto lead = static_cast<uint8_t>(text[i++]);
        if (lead < 0x80) continue;
        unsigned follow;
        uint8_t lo = 0x80, hi = 0xbf;
        if (lead >= 0xc2 && lead <= 0xdf) follow = 1;
        else if (lead >= 0xe0 && lead <= 0xef) {
            follow = 2;
            if (lead == 0xe0) lo = 0xa0;
            if (lead == 0xed) hi = 0x9f;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            follow = 3;
            if (lead == 0xf0) lo = 0x90;
            if (lead == 0xf4) hi = 0x8f;
        } else return false;
        if (text.size() - i < follow) return false;
        auto next = static_cast<uint8_t>(text[i++]);
        if (next < lo || next > hi) return false;
        while (--follow) if ((static_cast<uint8_t>(text[i++]) & 0xc0) != 0x80) return false;
    }
    return true;
}

WsRead websocket_consume(std::string& pending, WsFrame& frame) {
    if (pending.size() < 2) return WsRead::incomplete;
    const auto b = [&](size_t i) { return static_cast<uint8_t>(pending[i]); };
    const auto opcode = static_cast<uint8_t>(b(0) & 0x0f);
    const bool control = opcode == 8 || opcode == 9 || opcode == 10;
    unsigned length = b(1) & 0x7f;
    if ((b(0) & 0x70) || !(b(1) & 0x80) || (!control && opcode != 1 && opcode != 2) ||
        (control && (!(b(0) & 0x80) || length > 125)) || (opcode == 8 && length == 1))
        return WsRead::protocol_error;
    if (!control) {
        const size_t extra = length == 126 ? 2 : length == 127 ? 8 : 0;
        if (pending.size() < 2 + extra + 4) return WsRead::incomplete;
        if ((extra == 2 && ((unsigned(b(2)) << 8) | b(3)) < 126) ||
            (extra == 8 && ((b(2) & 0x80) || !(b(2) || b(3) || b(4) || b(5) || b(6) || b(7)))))
            return WsRead::protocol_error;
        return WsRead::unsupported_data;
    }
    if (pending.size() < 6 + length) return WsRead::incomplete;
    frame.opcode = opcode;
    frame.payload.resize(length);
    for (unsigned i = 0; i < length; ++i) frame.payload[i] = static_cast<char>(b(6 + i) ^ b(2 + i % 4));
    if (opcode == 8 && length >= 2) {
        const auto code = (unsigned(static_cast<uint8_t>(frame.payload[0])) << 8) | static_cast<uint8_t>(frame.payload[1]);
        const bool standard = code >= 1000 && code <= 1014 && code != 1004 && code != 1005 && code != 1006;
        if (!standard && (code < 3000 || code > 4999)) return WsRead::protocol_error;
        if (!websocket_utf8_valid(std::string_view(frame.payload).substr(2))) return WsRead::invalid_utf8;
    }
    pending.erase(0, 6 + length);
    return WsRead::frame;
}

} // namespace bambu
