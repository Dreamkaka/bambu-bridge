#include "json.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace bambu {

// ---------------------------------------------------------------------------
// dump
// ---------------------------------------------------------------------------
static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

static void dump_to(const Json& j, std::string& out) {
    switch (j.kind) {
        case Json::Null:   out += "null"; break;
        case Json::Bool:   out += j.b ? "true" : "false"; break;
        case Json::Number: {
            std::ostringstream ss;
            if (j.num == std::floor(j.num) && std::fabs(j.num) < 1e15) {
                ss << (long long)j.num;
            } else {
                ss << std::setprecision(15) << j.num;
            }
            out += ss.str();
            break;
        }
        case Json::String: out += '"' + json_escape(j.str) + '"'; break;
        case Json::Array: {
            out += '[';
            for (size_t i = 0; i < j.arr.size(); ++i) {
                if (i) out += ',';
                dump_to(j.arr[i], out);
            }
            out += ']';
            break;
        }
        case Json::Object: {
            out += '{';
            for (size_t i = 0; i < j.obj.size(); ++i) {
                if (i) out += ',';
                out += '"' + json_escape(j.obj[i].first) + "\":";
                dump_to(j.obj[i].second, out);
            }
            out += '}';
            break;
        }
    }
}

std::string json_dump(const Json& j) {
    std::string out;
    dump_to(j, out);
    return out;
}

// ---------------------------------------------------------------------------
// parse
// ---------------------------------------------------------------------------
namespace {
struct Parser {
    const char* p;
    const char* end;

    bool ws() { while (p < end && std::isspace((unsigned char)*p)) ++p; return true; }
    bool eof() const { return p >= end; }
    char peek() { return p < end ? *p : '\0'; }
    bool consume(char c) { if (p < end && *p == c) { ++p; return true; } return false; }

    bool parse(Json* out) {
        ws();
        if (eof()) return false;
        return parse_value(out);
    }

    bool parse_value(Json* out) {
        ws();
        if (eof()) return false;
        char c = *p;
        if (c == '{') return parse_object(out);
        if (c == '[') return parse_array(out);
        if (c == '"') { out->kind = Json::String; return parse_string(&out->str); }
        if (c == 't') return parse_lit("true", Json::boolean(true), out);
        if (c == 'f') return parse_lit("false", Json::boolean(false), out);
        if (c == 'n') return parse_lit("null", Json::null(), out);
        return parse_number(out);
    }

    bool parse_number(Json* out) {
        const char* start = p;
        if (peek() == '-') { ++p; }
        while (p < end && std::isdigit((unsigned char)*p)) ++p;
        if (p < end && *p == '.') {
            ++p;
            while (p < end && std::isdigit((unsigned char)*p)) ++p;
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
            ++p;
            if (p < end && (*p == '+' || *p == '-')) ++p;
            while (p < end && std::isdigit((unsigned char)*p)) ++p;
        }
        if (start == p) return false;
        out->kind = Json::Number;
        out->num = std::strtod(start, nullptr);
        return true;
    }

    bool parse_lit(const char* lit, Json v, Json* out) {
        size_t n = std::strlen(lit);
        if (p + n > end) return false;
        if (std::strncmp(p, lit, n) != 0) return false;
        p += n;
        *out = v;
        return true;
    }

    bool parse_string(std::string* out) {
        if (!consume('"')) return false;
        while (p < end) {
            char c = *p++;
            if (c == '"') return true;
            if (c == '\\') {
                if (p >= end) return false;
                char e = *p++;
                switch (e) {
                    case '"':  out->push_back('"');  break;
                    case '\\': out->push_back('\\'); break;
                    case '/':  out->push_back('/');  break;
                    case 'b':  out->push_back('\b'); break;
                    case 'f':  out->push_back('\f'); break;
                    case 'n':  out->push_back('\n'); break;
                    case 'r':  out->push_back('\r'); break;
                    case 't':  out->push_back('\t'); break;
                    case 'u': {
                        if (p + 4 > end) return false;
                        unsigned code = 0;
                        for (int k = 0; k < 4; ++k) {
                            char h = *p++;
                            code <<= 4;
                            if (h >= '0' && h <= '9') code |= (h - '0');
                            else if (h >= 'a' && h <= 'f') code |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') code |= (h - 'A' + 10);
                            else return false;
                        }
                        // Encode as UTF-8 (only BMP useful here)
                        if (code < 0x80) out->push_back((char)code);
                        else if (code < 0x800) {
                            out->push_back((char)(0xC0 | (code >> 6)));
                            out->push_back((char)(0x80 | (code & 0x3F)));
                        } else {
                            out->push_back((char)(0xE0 | (code >> 12)));
                            out->push_back((char)(0x80 | ((code >> 6) & 0x3F)));
                            out->push_back((char)(0x80 | (code & 0x3F)));
                        }
                        break;
                    }
                    default: return false;
                }
            } else {
                out->push_back(c);
            }
        }
        return false;
    }

    bool parse_array(Json* out) {
        if (!consume('[')) return false;
        out->kind = Json::Array;
        out->arr.clear();
        ws();
        if (consume(']')) return true;
        while (true) {
            Json item;
            if (!parse_value(&item)) return false;
            out->arr.push_back(std::move(item));
            ws();
            if (consume(']')) return true;
            if (!consume(',')) return false;
            ws();
        }
    }

    bool parse_object(Json* out) {
        if (!consume('{')) return false;
        out->kind = Json::Object;
        out->obj.clear();
        ws();
        if (consume('}')) return true;
        while (true) {
            ws();
            std::string key;
            if (!parse_string(&key)) return false;
            ws();
            if (!consume(':')) return false;
            Json val;
            if (!parse_value(&val)) return false;
            out->obj.emplace_back(std::move(key), std::move(val));
            ws();
            if (consume('}')) return true;
            if (!consume(',')) return false;
        }
    }
};
} // namespace

bool json_parse(const std::string& text, Json* out) {
    Parser parser{text.data(), text.data() + text.size()};
    return parser.parse(out);
}

} // namespace bambu
