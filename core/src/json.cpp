#include "roastsim/json.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace roastsim::json {

namespace {
const Value kNull;
const std::string kEmpty;
}  // namespace

double Value::num(double def) const {
    if (type_ == Type::Number) return num_;
    if (type_ == Type::Bool) return b_ ? 1.0 : 0.0;
    return def;
}

bool Value::boolean(bool def) const {
    if (type_ == Type::Bool) return b_;
    if (type_ == Type::Number) return num_ != 0.0;
    return def;
}

const std::string& Value::str() const { return type_ == Type::String ? str_ : kEmpty; }

bool Value::has(const std::string& key) const {
    if (type_ != Type::Object) return false;
    for (const auto& k : keys_)
        if (k == key) return true;
    return false;
}

const Value& Value::operator[](const std::string& key) const {
    if (type_ != Type::Object) return kNull;
    for (std::size_t i = 0; i < keys_.size(); ++i)
        if (keys_[i] == key) return vals_[i];
    return kNull;
}

Value& Value::operator[](const std::string& key) {
    if (type_ != Type::Object) {
        *this = Value::object();
    }
    for (std::size_t i = 0; i < keys_.size(); ++i)
        if (keys_[i] == key) return vals_[i];
    keys_.push_back(key);
    vals_.emplace_back();
    return vals_.back();
}

double Value::get(const std::string& key, double def) const {
    const Value& v = (*this)[key];
    return v.isNumber() ? v.num_ : (v.isBool() ? (v.b_ ? 1.0 : 0.0) : def);
}

std::string Value::get(const std::string& key, const char* def) const {
    const Value& v = (*this)[key];
    return v.isString() ? v.str_ : std::string(def ? def : "");
}

bool Value::getBool(const std::string& key, bool def) const { return (*this)[key].boolean(def); }

void Value::erase(const std::string& key) {
    if (type_ != Type::Object) return;
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) {
            keys_.erase(keys_.begin() + static_cast<long>(i));
            vals_.erase(vals_.begin() + static_cast<long>(i));
            return;
        }
    }
}

std::size_t Value::size() const {
    return (type_ == Type::Array || type_ == Type::Object) ? vals_.size() : 0;
}

const Value& Value::operator[](std::size_t i) const {
    if (type_ != Type::Array || i >= vals_.size()) return kNull;
    return vals_[i];
}

void Value::push(Value v) {
    if (type_ != Type::Array) *this = Value::array();
    vals_.push_back(std::move(v));
}

std::string escapeString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
    return out;
}

std::string formatNumber(double v, int maxDecimals) {
    if (!std::isfinite(v)) return "null";
    if (maxDecimals >= 0) {
        double p = std::pow(10.0, maxDecimals);
        v = std::round(v * p) / p;
    }
    if (v == std::floor(v) && std::fabs(v) < 1e15) {
        char buf[32];
        auto r = std::to_chars(buf, buf + sizeof buf, static_cast<long long>(v));
        return std::string(buf, r.ptr);
    }
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, v);
    return std::string(buf, r.ptr);
}

void Value::dumpTo(std::string& out, int indent, int depth) const {
    auto nl = [&](int d) {
        if (indent < 0) return;
        out.push_back('\n');
        out.append(static_cast<std::size_t>(indent * d), ' ');
    };
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += b_ ? "true" : "false"; break;
        case Type::Number: out += formatNumber(num_); break;
        case Type::String: out += escapeString(str_); break;
        case Type::Array:
            out.push_back('[');
            for (std::size_t i = 0; i < vals_.size(); ++i) {
                if (i) out.push_back(',');
                nl(depth + 1);
                vals_[i].dumpTo(out, indent, depth + 1);
            }
            if (!vals_.empty()) nl(depth);
            out.push_back(']');
            break;
        case Type::Object:
            out.push_back('{');
            for (std::size_t i = 0; i < keys_.size(); ++i) {
                if (i) out.push_back(',');
                nl(depth + 1);
                out += escapeString(keys_[i]);
                out += indent >= 0 ? ": " : ":";
                vals_[i].dumpTo(out, indent, depth + 1);
            }
            if (!keys_.empty()) nl(depth);
            out.push_back('}');
            break;
    }
}

std::string Value::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

// ---------------------------------------------------------------- parser
namespace {

struct Parser {
    const char* p;
    const char* end;
    std::string err;

    void ws() {
        while (p < end) {
            if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
                ++p;
            } else if (*p == '/' && p + 1 < end && p[1] == '/') {  // allow // comments in data files
                while (p < end && *p != '\n') ++p;
            } else {
                break;
            }
        }
    }

    bool fail(const char* msg) {
        if (err.empty()) err = msg;
        return false;
    }

    static void appendUtf8(std::string& s, unsigned cp) {
        if (cp < 0x80) {
            s.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool hex4(unsigned& out) {
        if (end - p < 4) return fail("bad \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *p++;
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<unsigned>(c - 'A' + 10);
            else return fail("bad hex digit");
        }
        return true;
    }

    bool parseString(std::string& s) {
        ++p;  // opening quote
        while (p < end) {
            char c = *p++;
            if (c == '"') return true;
            if (c == '\\') {
                if (p >= end) break;
                char e = *p++;
                switch (e) {
                    case '"': s.push_back('"'); break;
                    case '\\': s.push_back('\\'); break;
                    case '/': s.push_back('/'); break;
                    case 'b': s.push_back('\b'); break;
                    case 'f': s.push_back('\f'); break;
                    case 'n': s.push_back('\n'); break;
                    case 'r': s.push_back('\r'); break;
                    case 't': s.push_back('\t'); break;
                    case 'u': {
                        unsigned cp;
                        if (!hex4(cp)) return false;
                        if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                            p += 2;
                            unsigned lo;
                            if (!hex4(lo)) return false;
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        appendUtf8(s, cp);
                        break;
                    }
                    default: return fail("bad escape");
                }
            } else {
                s.push_back(c);
            }
        }
        return fail("unterminated string");
    }

    bool parseValue(Value& v, int depth) {
        if (depth > 64) return fail("nesting too deep");
        ws();
        if (p >= end) return fail("unexpected end");
        char c = *p;
        if (c == '{') {
            ++p;
            v = Value::object();
            ws();
            if (p < end && *p == '}') { ++p; return true; }
            for (;;) {
                ws();
                if (p >= end || *p != '"') return fail("expected key");
                std::string key;
                if (!parseString(key)) return false;
                ws();
                if (p >= end || *p != ':') return fail("expected ':'");
                ++p;
                Value child;
                if (!parseValue(child, depth + 1)) return false;
                v[key] = std::move(child);
                ws();
                if (p < end && *p == ',') { ++p; ws(); if (p < end && *p == '}') { ++p; return true; } continue; }
                if (p < end && *p == '}') { ++p; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++p;
            v = Value::array();
            ws();
            if (p < end && *p == ']') { ++p; return true; }
            for (;;) {
                Value child;
                if (!parseValue(child, depth + 1)) return false;
                v.push(std::move(child));
                ws();
                if (p < end && *p == ',') { ++p; ws(); if (p < end && *p == ']') { ++p; return true; } continue; }
                if (p < end && *p == ']') { ++p; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            std::string s;
            if (!parseString(s)) return false;
            v = Value(std::move(s));
            return true;
        }
        if (end - p >= 4 && std::strncmp(p, "true", 4) == 0) { p += 4; v = Value(true); return true; }
        if (end - p >= 5 && std::strncmp(p, "false", 5) == 0) { p += 5; v = Value(false); return true; }
        if (end - p >= 4 && std::strncmp(p, "null", 4) == 0) { p += 4; v = Value(); return true; }
        if (c == '-' || c == '+' || (c >= '0' && c <= '9')) {
            const char* start = (c == '+') ? p + 1 : p;
            double d = 0;
            auto r = std::from_chars(start, end, d);
            if (r.ec != std::errc()) return fail("bad number");
            p = r.ptr;
            v = Value(d);
            return true;
        }
        return fail("unexpected character");
    }
};

}  // namespace

Value Value::parse(const std::string& text, std::string* error) {
    Parser ps{text.data(), text.data() + text.size(), {}};
    Value v;
    bool ok = ps.parseValue(v, 0);
    if (ok) {
        ps.ws();
        if (ps.p != ps.end) ok = ps.fail("trailing characters");
    }
    if (!ok) {
        if (error) {
            *error = ps.err + " at offset " + std::to_string(ps.p - text.data());
        }
        return Value();
    }
    if (error) error->clear();
    return v;
}

}  // namespace roastsim::json
