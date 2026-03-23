#include "json_value.h"
#include "json_parser.h"
#include <stdexcept>

namespace json {

// Array implementation
void Array::push_back(Value val) {
    values_.push_back(std::move(val));
}

size_t Array::size() const {
    return values_.size();
}

const Value& Array::operator[](size_t index) const {
    return values_[index];
}

Array::iterator Array::begin() {
    return values_.begin();
}

Array::iterator Array::end() {
    return values_.end();
}

Array::const_iterator Array::begin() const {
    return values_.begin();
}

Array::const_iterator Array::end() const {
    return values_.end();
}

// Object implementation
void Object::insert(std::string key, Value val) {
    values_[std::move(key)] = std::move(val);
}

bool Object::contains(const std::string& key) const {
    return values_.find(key) != values_.end();
}

const Value& Object::at(const std::string& key) const {
    return values_.at(key);
}

size_t Object::size() const {
    return values_.size();
}

Object::iterator Object::begin() {
    return values_.begin();
}

Object::iterator Object::end() {
    return values_.end();
}

Object::const_iterator Object::begin() const {
    return values_.begin();
}

Object::const_iterator Object::end() const {
    return values_.end();
}

} // namespace json
