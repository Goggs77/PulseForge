#include "core/Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace pf::json {

static const Value kNull;

Value Value::makeObject() {
    Value v;
    v.type = Type::Object;
    return v;
}
Value Value::makeArray() {
    Value v;
    v.type = Type::Array;
    return v;
}
Value Value::makeNumber(double n) {
    Value v;
    v.type = Type::Number;
    v.number = n;
    return v;
}
Value Value::makeBool(bool b) {
    Value v;
    v.type = Type::Bool;
    v.boolean = b;
    return v;
}
Value Value::makeString(const std::string &s) {
    Value v;
    v.type = Type::String;
    v.string = s;
    return v;
}

bool Value::has(const std::string &key) const {
    for (const auto &kv : object) {
        if (kv.first == key) return true;
    }
    return false;
}

const Value &Value::operator[](const std::string &key) const {
    for (const auto &kv : object) {
        if (kv.first == key) return kv.second;
    }
    return kNull;
}

const Value &Value::at(size_t index) const {
    if (index < array.size()) return array[index];
    return kNull;
}

size_t Value::size() const {
    if (type == Type::Array) return array.size();
    if (type == Type::Object) return object.size();
    return 0;
}

void Value::set(const std::string &key, Value v) {
    if (type != Type::Object) {
        type = Type::Object;
    }
    for (auto &kv : object) {
        if (kv.first == key) {
            kv.second = std::move(v);
            return;
        }
    }
    object.emplace_back(key, std::move(v));
}

void Value::push(Value v) {
    if (type != Type::Array) type = Type::Array;
    array.push_back(std::move(v));
}

double Value::asDouble(double fallback) const {
    if (type == Type::Number) return number;
    if (type == Type::Bool) return boolean ? 1.0 : 0.0;
    // ffprobe and friends emit numbers as strings ("4.000000").
    if (type == Type::String && !string.empty()) {
        char *end = nullptr;
        const double parsed = std::strtod(string.c_str(), &end);
        if (end && end != string.c_str()) return parsed;
    }
    return fallback;
}
float Value::asFloat(float fallback) const { return static_cast<float>(asDouble(fallback)); }
int Value::asInt(int fallback) const {
    if (type == Type::Number) return static_cast<int>(number);
    if (type == Type::Bool) return boolean ? 1 : 0;
    if (type == Type::String && !string.empty()) {
        char *end = nullptr;
        const long parsed = std::strtol(string.c_str(), &end, 10);
        if (end && end != string.c_str()) return static_cast<int>(parsed);
    }
    return fallback;
}
bool Value::asBool(bool fallback) const {
    if (type == Type::Bool) return boolean;
    if (type == Type::Number) return number != 0.0;
    return fallback;
}
std::string Value::asString(const std::string &fallback) const {
    if (type == Type::String) return string;
    return fallback;
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

static void writeEscaped(const std::string &s, std::string &out) {
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
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

static void writeNumber(double n, std::string &out) {
    if (std::isfinite(n)) {
        if (n == static_cast<double>(static_cast<long long>(n)) && std::fabs(n) < 1e15) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(n));
            out += buf;
        } else {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.10g", n);
            out += buf;
        }
    } else {
        out += "0";
    }
}

static void writeValue(const Value &v, std::string &out, int indent, int depth) {
    const bool pretty = indent > 0;
    const std::string pad = pretty ? std::string(static_cast<size_t>(indent * (depth + 1)), ' ') : std::string();
    const std::string padEnd = pretty ? std::string(static_cast<size_t>(indent * depth), ' ') : std::string();

    switch (v.type) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += v.boolean ? "true" : "false"; break;
        case Type::Number: writeNumber(v.number, out); break;
        case Type::String: writeEscaped(v.string, out); break;
        case Type::Array:
            if (v.array.empty()) {
                out += "[]";
                break;
            }
            out += "[";
            for (size_t i = 0; i < v.array.size(); ++i) {
                if (i) out += ",";
                if (pretty) {
                    out += "\n";
                    out += pad;
                }
                writeValue(v.array[i], out, indent, depth + 1);
            }
            if (pretty) {
                out += "\n";
                out += padEnd;
            }
            out += "]";
            break;
        case Type::Object:
            if (v.object.empty()) {
                out += "{}";
                break;
            }
            out += "{";
            for (size_t i = 0; i < v.object.size(); ++i) {
                if (i) out += ",";
                if (pretty) {
                    out += "\n";
                    out += pad;
                }
                writeEscaped(v.object[i].first, out);
                out += pretty ? ": " : ":";
                writeValue(v.object[i].second, out, indent, depth + 1);
            }
            if (pretty) {
                out += "\n";
                out += padEnd;
            }
            out += "}";
            break;
    }
}

std::string write(const Value &value, int indent) {
    std::string out;
    out.reserve(4096);
    writeValue(value, out, indent, 0);
    out.push_back('\n');
    return out;
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

namespace {
class Parser {
public:
    Parser(const std::string &text, std::string *error) : text_(text), error_(error) {}

    bool run(Value &out) {
        skipWhitespace();
        if (!parseValue(out)) return false;
        skipWhitespace();
        if (pos_ != text_.size()) return fail("trailing characters after top-level value");
        return true;
    }

private:
    bool fail(const std::string &message) {
        if (error_) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "at byte %zu: ", pos_);
            *error_ = std::string(buf) + message;
        }
        return false;
    }

    void skipWhitespace() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool literal(const char *word) {
        const size_t len = std::char_traits<char>::length(word);
        if (text_.compare(pos_, len, word) != 0) return fail("invalid literal");
        pos_ += len;
        return true;
    }

    bool parseValue(Value &out) {
        if (pos_ >= text_.size()) return fail("unexpected end of input");
        switch (text_[pos_]) {
            case '{': return parseObject(out);
            case '[': return parseArray(out);
            case '"': {
                std::string s;
                if (!parseString(s)) return false;
                out = Value::makeString(s);
                return true;
            }
            case 't':
                if (!literal("true")) return false;
                out = Value::makeBool(true);
                return true;
            case 'f':
                if (!literal("false")) return false;
                out = Value::makeBool(false);
                return true;
            case 'n':
                if (!literal("null")) return false;
                out = Value();
                return true;
            default: return parseNumber(out);
        }
    }

    bool parseObject(Value &out) {
        out = Value::makeObject();
        ++pos_;  // '{'
        skipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            skipWhitespace();
            std::string key;
            if (!parseString(key)) return false;
            skipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != ':') return fail("expected ':'");
            ++pos_;
            skipWhitespace();
            Value child;
            if (!parseValue(child)) return false;
            out.set(key, std::move(child));
            skipWhitespace();
            if (pos_ >= text_.size()) return fail("unterminated object");
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    bool parseArray(Value &out) {
        out = Value::makeArray();
        ++pos_;  // '['
        skipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            skipWhitespace();
            Value child;
            if (!parseValue(child)) return false;
            out.push(std::move(child));
            skipWhitespace();
            if (pos_ >= text_.size()) return fail("unterminated array");
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    bool parseString(std::string &out) {
        if (pos_ >= text_.size() || text_[pos_] != '"') return fail("expected string");
        ++pos_;
        out.clear();
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) return fail("unterminated escape");
            const char esc = text_[pos_++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'u': {
                    if (pos_ + 4 > text_.size()) return fail("truncated \\u escape");
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char h = text_[pos_++];
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                        else return fail("invalid hex digit in \\u escape");
                    }
                    // UTF-8 encode (BMP only, which covers what we write).
                    if (code < 0x80) {
                        out.push_back(static_cast<char>(code));
                    } else if (code < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    break;
                }
                default: return fail("unknown escape sequence");
            }
        }
        return fail("unterminated string");
    }

    bool parseNumber(Value &out) {
        const size_t start = pos_;
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
        bool any = false;
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
            ++pos_;
            any = true;
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
                ++pos_;
                any = true;
            }
        }
        if (!any) return fail("invalid number");
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        out = Value::makeNumber(std::strtod(text_.substr(start, pos_ - start).c_str(), nullptr));
        return true;
    }

    const std::string &text_;
    std::string *error_;
    size_t pos_ = 0;
};
}  // namespace

bool parse(const std::string &text, Value &out, std::string *error) {
    Parser parser(text, error);
    return parser.run(out);
}

}  // namespace pf::json
