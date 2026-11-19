// Minimal, dependency-free JSON value type used by the core (roaster/bean
// definitions) and by the service layer (WebSocket / HTTP / C API).
//
// Locale independent: numbers are formatted and parsed with std::to_chars /
// std::from_chars, so results are identical on Windows, Linux and Haiku.
#pragma once

#include <cstddef>
#include <string>
#include <type_traits>
#include <vector>

namespace roastsim::json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : type_(Type::Bool), b_(b) {}
    template <typename T,
              typename = std::enable_if_t<std::is_arithmetic_v<T> && !std::is_same_v<T, bool>>>
    Value(T v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    Value(const char* s) : type_(Type::String), str_(s ? s : "") {}
    Value(std::string s) : type_(Type::String), str_(std::move(s)) {}

    static Value array() { Value v; v.type_ = Type::Array; return v; }
    static Value object() { Value v; v.type_ = Type::Object; return v; }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    // Scalar accessors with fallbacks (never throw).
    double num(double def = 0.0) const;
    bool boolean(bool def = false) const;
    const std::string& str() const;  // empty string if not a string
    std::string strOr(const std::string& def) const { return isString() ? str_ : def; }

    // Object access.
    bool has(const std::string& key) const;
    const Value& operator[](const std::string& key) const;  // Null value if missing
    Value& operator[](const std::string& key);              // creates (converts to object)
    const Value& operator[](const char* key) const { return (*this)[std::string(key)]; }
    Value& operator[](const char* key) { return (*this)[std::string(key)]; }
    double get(const std::string& key, double def) const;
    std::string get(const std::string& key, const char* def) const;
    bool getBool(const std::string& key, bool def) const;
    const std::vector<std::string>& keys() const { return keys_; }
    void erase(const std::string& key);

    // Array access.
    std::size_t size() const;
    const Value& operator[](std::size_t i) const;
    const Value& at(std::size_t i) const { return (*this)[i]; }
    void push(Value v);
    const std::vector<Value>& items() const { return vals_; }

    std::string dump(int indent = -1) const;
    static Value parse(const std::string& text, std::string* error = nullptr);

private:
    void dumpTo(std::string& out, int indent, int depth) const;

    Type type_ = Type::Null;
    bool b_ = false;
    double num_ = 0.0;
    std::string str_;
    std::vector<std::string> keys_;  // object keys (insertion order)
    std::vector<Value> vals_;        // array items, or object values parallel to keys_
};

std::string escapeString(const std::string& s);
std::string formatNumber(double v, int maxDecimals = -1);

}  // namespace roastsim::json
