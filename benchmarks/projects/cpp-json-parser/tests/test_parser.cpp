#include "json_parser.h"
#include <iostream>
#include <cassert>

void test_null() {
    auto result = json::parse("null");
    assert(result.has_value());
    assert(result->is_null());
    std::cout << "✓ test_null passed" << std::endl;
}

void test_boolean() {
    auto result_true = json::parse("true");
    assert(result_true.has_value());
    assert(result_true->is_bool());
    assert(result_true->as_bool() == true);

    auto result_false = json::parse("false");
    assert(result_false.has_value());
    assert(result_false->is_bool());
    assert(result_false->as_bool() == false);

    std::cout << "✓ test_boolean passed" << std::endl;
}

void test_number() {
    auto result1 = json::parse("42");
    assert(result1.has_value());
    assert(result1->is_number());
    assert(result1->as_number() == 42.0);

    auto result2 = json::parse("-3.14");
    assert(result2.has_value());
    assert(result2->is_number());
    assert(result2->as_number() == -3.14);

    std::cout << "✓ test_number passed" << std::endl;
}

void test_string() {
    auto result = json::parse("\"hello world\"");
    assert(result.has_value());
    assert(result->is_string());
    assert(result->as_string() == "hello world");

    std::cout << "✓ test_string passed" << std::endl;
}

void test_array() {
    auto result = json::parse("[1, 2, 3, 4, 5]");
    assert(result.has_value());
    assert(result->is_array());
    assert(result->as_array().size() == 5);

    std::cout << "✓ test_array passed" << std::endl;
}

void test_object() {
    auto result = json::parse(R"({"name": "test", "value": 42})");
    assert(result.has_value());
    assert(result->is_object());

    auto& obj = result->as_object();
    assert(obj.size() == 2);
    assert(obj.contains("name"));
    assert(obj.contains("value"));
    assert(obj.at("name").as_string() == "test");
    assert(obj.at("value").as_number() == 42.0);

    std::cout << "✓ test_object passed" << std::endl;
}

void test_nested() {
    auto result = json::parse(R"({
        "users": [
            {"name": "Alice", "age": 30},
            {"name": "Bob", "age": 25}
        ],
        "count": 2
    })");

    assert(result.has_value());
    assert(result->is_object());

    auto& obj = result->as_object();
    assert(obj.contains("users"));
    assert(obj.at("users").is_array());
    assert(obj.at("users").as_array().size() == 2);

    std::cout << "✓ test_nested passed" << std::endl;
}

int main() {
    std::cout << "Running JSON parser tests..." << std::endl;

    test_null();
    test_boolean();
    test_number();
    test_string();
    test_array();
    test_object();
    test_nested();

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
