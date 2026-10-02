// Minimal JSON document model used for .pforge project files.
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pf::json {

enum class Type { Null, Bool, Number, String, Array, Object };

class Value {
public:
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    Value() = default;
    static Value makeObject();
    static Value makeArray();
    static Value makeNumber(double v);
    static Value makeBool(bool v);
    static Value makeString(const std::string &v);

    bool isNull() const { return type == Type::Null; }
    bool isObject() const { return type == Type::Object; }
    bool isArray() const { return type == Type::Array; }
    bool isNumber() const { return type == Type::Number; }
    bool isString() const { return type == Type::String; }
    bool isBool() const { return type == Type::Bool; }

    bool has(const std::string &key) const;
    // Missing keys and type mismatches produce a null value rather than an
    // exception, which keeps project loading tolerant of older files.
    const Value &operator[](const std::string &key) const;
    const Value &at(size_t index) const;
    size_t size() const;

    void set(const std::string &key, Value v);
    void set(const std::string &key, double v) { set(key, makeNumber(v)); }
    void set(const std::string &key, int v) { set(key, makeNumber(static_cast<double>(v))); }
    void set(const std::string &key, bool v) { set(key, makeBool(v)); }
    void set(const std::string &key, const std::string &v) { set(key, makeString(v)); }
    void set(const std::string &key, const char *v) { set(key, makeString(v ? v : "")); }
    void push(Value v);

    double asDouble(double fallback = 0.0) const;
    float asFloat(float fallback = 0.0f) const;
    int asInt(int fallback = 0) const;
    bool asBool(bool fallback = false) const;
    std::string asString(const std::string &fallback = std::string()) const;
};

std::string write(const Value &value, int indent = 2);
bool parse(const std::string &text, Value &out, std::string *error = nullptr);

}  // namespace pf::json
