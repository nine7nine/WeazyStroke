#pragma once
// A tiny, self-contained JSON value + parser + serializer.
//
// Deliberately hand-written and dependency-free: no Boost, no vendored blob.
// Scope is exactly what the config layer needs (objects, arrays, strings,
// numbers, bools, null) with enough robustness to tolerate hand edits.

#include <map>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace es::json {

class Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

class ParseError : public std::runtime_error {
public:
    explicit ParseError(const std::string &msg) : std::runtime_error(msg) {}
};

class Value {
public:
    Value() : v_(nullptr) {}
    Value(std::nullptr_t) : v_(nullptr) {}
    Value(bool b) : v_(b) {}
    Value(int i) : v_(static_cast<double>(i)) {}
    Value(double d) : v_(d) {}
    Value(const char *s) : v_(std::string(s)) {}
    Value(std::string s) : v_(std::move(s)) {}
    Value(Array a) : v_(std::move(a)) {}
    Value(Object o) : v_(std::move(o)) {}

    bool is_null() const { return std::holds_alternative<std::nullptr_t>(v_); }
    bool is_bool() const { return std::holds_alternative<bool>(v_); }
    bool is_number() const { return std::holds_alternative<double>(v_); }
    bool is_string() const { return std::holds_alternative<std::string>(v_); }
    bool is_array() const { return std::holds_alternative<Array>(v_); }
    bool is_object() const { return std::holds_alternative<Object>(v_); }

    // Typed accessors. Throw std::runtime_error on a type mismatch.
    bool as_bool() const;
    double as_number() const;
    const std::string &as_string() const;
    const Array &as_array() const;
    const Object &as_object() const;

    // Object member lookup; returns a shared null Value when missing or when
    // this is not an object (so chained access never throws).
    const Value &operator[](const std::string &key) const;

    std::string dump(int indent = 2) const;
    static Value parse(const std::string &text);

private:
    void dump_to(std::string &out, int indent, int depth) const;

    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> v_;
};

} // namespace es::json
