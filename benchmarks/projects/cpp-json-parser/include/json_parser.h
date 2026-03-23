#pragma once

#include <string>
#include <string_view>
#include <memory>
#include <optional>

namespace json {

// Forward declarations
class Value;
class Object;
class Array;

// Parse JSON from string
std::optional<Value> parse(std::string_view input);

// JSON value types
enum class Type {
    Null,
    Boolean,
    Number,
    String,
    Array,
    Object
};

// Main JSON value class
class Value {
public:
    Value();
    explicit Value(std::nullptr_t);
    explicit Value(bool b);
    explicit Value(double n);
    explicit Value(std::string s);
    explicit Value(Object obj);
    explicit Value(Array arr);

    Type type() const;
    bool is_null() const;
    bool is_bool() const;
    bool is_number() const;
    bool is_string() const;
    bool is_array() const;
    bool is_object() const;

    bool as_bool() const;
    double as_number() const;
    const std::string& as_string() const;
    const Array& as_array() const;
    const Object& as_object() const;

    std::string to_string() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace json
