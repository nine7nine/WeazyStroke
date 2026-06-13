#include "json.h"

#include <cmath>
#include <cstdio>

namespace es::json {

namespace {

const Value &null_value() {
    static const Value n;
    return n;
}

void append_escaped(std::string &out, const std::string &s) {
    out += '"';
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

void append_number(std::string &out, double d) {
    char buf[32];
    // Print integral values without a fractional part; otherwise round-trip.
    if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 1e15)
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(d));
    else
        std::snprintf(buf, sizeof(buf), "%.17g", d);
    out += buf;
}

void indent_to(std::string &out, int indent, int depth) {
    if (indent <= 0)
        return;
    out += '\n';
    out.append(static_cast<size_t>(indent) * depth, ' ');
}

// --- Parser -------------------------------------------------------------

class Parser {
public:
    explicit Parser(const std::string &s) : s_(s) {}

    Value parse() {
        skip_ws();
        Value v = parse_value();
        skip_ws();
        if (i_ != s_.size())
            fail("trailing characters after JSON value");
        return v;
    }

private:
    [[noreturn]] void fail(const std::string &msg) {
        throw ParseError("json: " + msg + " at offset " + std::to_string(i_));
    }

    char peek() {
        if (i_ >= s_.size())
            fail("unexpected end of input");
        return s_[i_];
    }

    void skip_ws() {
        while (i_ < s_.size()) {
            char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++i_;
            else
                break;
        }
    }

    Value parse_value() {
        char c = peek();
        switch (c) {
        case '{': return parse_object();
        case '[': return parse_array();
        case '"': return Value(parse_string());
        case 't': case 'f': return parse_bool();
        case 'n': return parse_null();
        default: return parse_number();
        }
    }

    void expect(char c) {
        if (peek() != c)
            fail(std::string("expected '") + c + "'");
        ++i_;
    }

    Value parse_object() {
        expect('{');
        Object o;
        skip_ws();
        if (peek() == '}') {
            ++i_;
            return Value(std::move(o));
        }
        for (;;) {
            skip_ws();
            if (peek() != '"')
                fail("expected object key");
            std::string key = parse_string();
            skip_ws();
            expect(':');
            skip_ws();
            o[key] = parse_value();
            skip_ws();
            char c = peek();
            if (c == ',') {
                ++i_;
                continue;
            }
            if (c == '}') {
                ++i_;
                break;
            }
            fail("expected ',' or '}'");
        }
        return Value(std::move(o));
    }

    Value parse_array() {
        expect('[');
        Array a;
        skip_ws();
        if (peek() == ']') {
            ++i_;
            return Value(std::move(a));
        }
        for (;;) {
            skip_ws();
            a.push_back(parse_value());
            skip_ws();
            char c = peek();
            if (c == ',') {
                ++i_;
                continue;
            }
            if (c == ']') {
                ++i_;
                break;
            }
            fail("expected ',' or ']'");
        }
        return Value(std::move(a));
    }

    void append_utf8(std::string &out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    unsigned parse_hex4() {
        unsigned v = 0;
        for (int n = 0; n < 4; ++n) {
            if (i_ >= s_.size())
                fail("unterminated \\u escape");
            char c = s_[i_++];
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v |= static_cast<unsigned>(c - 'A' + 10);
            else
                fail("bad hex digit in \\u escape");
        }
        return v;
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        for (;;) {
            if (i_ >= s_.size())
                fail("unterminated string");
            char c = s_[i_++];
            if (c == '"')
                break;
            if (c == '\\') {
                if (i_ >= s_.size())
                    fail("unterminated escape");
                char e = s_[i_++];
                switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'n':  out += '\n'; break;
                case 't':  out += '\t'; break;
                case 'r':  out += '\r'; break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'u':  append_utf8(out, parse_hex4()); break;
                default: fail("invalid escape");
                }
            } else {
                out += c;
            }
        }
        return out;
    }

    Value parse_number() {
        size_t start = i_;
        if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+'))
            ++i_;
        bool any = false;
        while (i_ < s_.size()) {
            char c = s_[i_];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' ||
                c == '-') {
                ++i_;
                any = true;
            } else {
                break;
            }
        }
        if (!any)
            fail("invalid number");
        return Value(std::strtod(s_.c_str() + start, nullptr));
    }

    Value parse_bool() {
        if (s_.compare(i_, 4, "true") == 0) {
            i_ += 4;
            return Value(true);
        }
        if (s_.compare(i_, 5, "false") == 0) {
            i_ += 5;
            return Value(false);
        }
        fail("invalid literal");
    }

    Value parse_null() {
        if (s_.compare(i_, 4, "null") == 0) {
            i_ += 4;
            return Value(nullptr);
        }
        fail("invalid literal");
    }

    const std::string &s_;
    size_t i_ = 0;
};

} // namespace

bool Value::as_bool() const {
    if (auto *p = std::get_if<bool>(&v_))
        return *p;
    throw std::runtime_error("json: value is not a bool");
}

double Value::as_number() const {
    if (auto *p = std::get_if<double>(&v_))
        return *p;
    throw std::runtime_error("json: value is not a number");
}

const std::string &Value::as_string() const {
    if (auto *p = std::get_if<std::string>(&v_))
        return *p;
    throw std::runtime_error("json: value is not a string");
}

const Array &Value::as_array() const {
    if (auto *p = std::get_if<Array>(&v_))
        return *p;
    throw std::runtime_error("json: value is not an array");
}

const Object &Value::as_object() const {
    if (auto *p = std::get_if<Object>(&v_))
        return *p;
    throw std::runtime_error("json: value is not an object");
}

const Value &Value::operator[](const std::string &key) const {
    if (auto *p = std::get_if<Object>(&v_)) {
        auto it = p->find(key);
        if (it != p->end())
            return it->second;
    }
    return null_value();
}

void Value::dump_to(std::string &out, int indent, int depth) const {
    if (auto *b = std::get_if<bool>(&v_)) {
        out += *b ? "true" : "false";
    } else if (auto *d = std::get_if<double>(&v_)) {
        append_number(out, *d);
    } else if (auto *s = std::get_if<std::string>(&v_)) {
        append_escaped(out, *s);
    } else if (auto *a = std::get_if<Array>(&v_)) {
        if (a->empty()) {
            out += "[]";
            return;
        }
        out += '[';
        bool first = true;
        for (const Value &e : *a) {
            if (!first)
                out += ',';
            first = false;
            indent_to(out, indent, depth + 1);
            e.dump_to(out, indent, depth + 1);
        }
        indent_to(out, indent, depth);
        out += ']';
    } else if (auto *o = std::get_if<Object>(&v_)) {
        if (o->empty()) {
            out += "{}";
            return;
        }
        out += '{';
        bool first = true;
        for (const auto &kv : *o) {
            if (!first)
                out += ',';
            first = false;
            indent_to(out, indent, depth + 1);
            append_escaped(out, kv.first);
            out += indent > 0 ? ": " : ":";
            kv.second.dump_to(out, indent, depth + 1);
        }
        indent_to(out, indent, depth);
        out += '}';
    } else {
        out += "null";
    }
}

std::string Value::dump(int indent) const {
    std::string out;
    dump_to(out, indent, 0);
    return out;
}

Value Value::parse(const std::string &text) {
    return Parser(text).parse();
}

} // namespace es::json
