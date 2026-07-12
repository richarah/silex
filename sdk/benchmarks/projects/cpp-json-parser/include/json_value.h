#pragma once

#include <string>
#include <vector>
#include <unordered_map>

namespace json {

class Value;

// JSON array
class Array {
public:
    Array() = default;

    void push_back(Value val);
    size_t size() const;
    const Value& operator[](size_t index) const;

    using iterator = std::vector<Value>::iterator;
    using const_iterator = std::vector<Value>::const_iterator;

    iterator begin();
    iterator end();
    const_iterator begin() const;
    const_iterator end() const;

private:
    std::vector<Value> values_;
};

// JSON object
class Object {
public:
    Object() = default;

    void insert(std::string key, Value val);
    bool contains(const std::string& key) const;
    const Value& at(const std::string& key) const;
    size_t size() const;

    using iterator = std::unordered_map<std::string, Value>::iterator;
    using const_iterator = std::unordered_map<std::string, Value>::const_iterator;

    iterator begin();
    iterator end();
    const_iterator begin() const;
    const_iterator end() const;

private:
    std::unordered_map<std::string, Value> values_;
};

} // namespace json
