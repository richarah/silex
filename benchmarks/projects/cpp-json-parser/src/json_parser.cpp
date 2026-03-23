#include "json_parser.h"
#include "json_value.h"
#include "json_tokenizer.h"
#include <stdexcept>
#include <variant>

namespace json {

// Internal implementation using std::variant
struct Value::Impl {
    std::variant<
        std::nullptr_t,
        bool,
        double,
        std::string,
        Array,
        Object
    > data;
};

Value::Value() : impl_(std::make_unique<Impl>()) {
    impl_->data = nullptr;
}

Value::Value(std::nullptr_t) : impl_(std::make_unique<Impl>()) {
    impl_->data = nullptr;
}

Value::Value(bool b) : impl_(std::make_unique<Impl>()) {
    impl_->data = b;
}

Value::Value(double n) : impl_(std::make_unique<Impl>()) {
    impl_->data = n;
}

Value::Value(std::string s) : impl_(std::make_unique<Impl>()) {
    impl_->data = std::move(s);
}

Value::Value(Object obj) : impl_(std::make_unique<Impl>()) {
    impl_->data = std::move(obj);
}

Value::Value(Array arr) : impl_(std::make_unique<Impl>()) {
    impl_->data = std::move(arr);
}

Type Value::type() const {
    return static_cast<Type>(impl_->data.index());
}

bool Value::is_null() const { return type() == Type::Null; }
bool Value::is_bool() const { return type() == Type::Boolean; }
bool Value::is_number() const { return type() == Type::Number; }
bool Value::is_string() const { return type() == Type::String; }
bool Value::is_array() const { return type() == Type::Array; }
bool Value::is_object() const { return type() == Type::Object; }

bool Value::as_bool() const {
    return std::get<bool>(impl_->data);
}

double Value::as_number() const {
    return std::get<double>(impl_->data);
}

const std::string& Value::as_string() const {
    return std::get<std::string>(impl_->data);
}

const Array& Value::as_array() const {
    return std::get<Array>(impl_->data);
}

const Object& Value::as_object() const {
    return std::get<Object>(impl_->data);
}

std::string Value::to_string() const {
    switch (type()) {
        case Type::Null:
            return "null";
        case Type::Boolean:
            return as_bool() ? "true" : "false";
        case Type::Number:
            return std::to_string(as_number());
        case Type::String:
            return "\"" + as_string() + "\"";
        case Type::Array:
            return "[array]";
        case Type::Object:
            return "{object}";
    }
    return "";
}

// Parser implementation
class Parser {
public:
    explicit Parser(Tokenizer& tokenizer) : tokenizer_(tokenizer) {}

    std::optional<Value> parse() {
        return parse_value();
    }

private:
    std::optional<Value> parse_value() {
        auto token = tokenizer_.peek();
        if (!token) return std::nullopt;

        switch (token->type) {
            case TokenType::Null:
                tokenizer_.next();
                return Value(nullptr);
            case TokenType::True:
                tokenizer_.next();
                return Value(true);
            case TokenType::False:
                tokenizer_.next();
                return Value(false);
            case TokenType::Number:
                tokenizer_.next();
                return Value(std::stod(token->value));
            case TokenType::String:
                tokenizer_.next();
                return Value(token->value);
            case TokenType::LeftBrace:
                return parse_object();
            case TokenType::LeftBracket:
                return parse_array();
            default:
                return std::nullopt;
        }
    }

    std::optional<Value> parse_object() {
        tokenizer_.next(); // consume '{'

        Object obj;

        while (true) {
            auto token = tokenizer_.peek();
            if (!token) return std::nullopt;

            if (token->type == TokenType::RightBrace) {
                tokenizer_.next();
                break;
            }

            // Expect string key
            if (token->type != TokenType::String) return std::nullopt;
            std::string key = token->value;
            tokenizer_.next();

            // Expect colon
            token = tokenizer_.next();
            if (!token || token->type != TokenType::Colon) return std::nullopt;

            // Parse value
            auto value = parse_value();
            if (!value) return std::nullopt;

            obj.insert(std::move(key), std::move(*value));

            // Check for comma or closing brace
            token = tokenizer_.peek();
            if (!token) return std::nullopt;

            if (token->type == TokenType::Comma) {
                tokenizer_.next();
            } else if (token->type == TokenType::RightBrace) {
                tokenizer_.next();
                break;
            } else {
                return std::nullopt;
            }
        }

        return Value(std::move(obj));
    }

    std::optional<Value> parse_array() {
        tokenizer_.next(); // consume '['

        Array arr;

        while (true) {
            auto token = tokenizer_.peek();
            if (!token) return std::nullopt;

            if (token->type == TokenType::RightBracket) {
                tokenizer_.next();
                break;
            }

            auto value = parse_value();
            if (!value) return std::nullopt;

            arr.push_back(std::move(*value));

            token = tokenizer_.peek();
            if (!token) return std::nullopt;

            if (token->type == TokenType::Comma) {
                tokenizer_.next();
            } else if (token->type == TokenType::RightBracket) {
                tokenizer_.next();
                break;
            } else {
                return std::nullopt;
            }
        }

        return Value(std::move(arr));
    }

    Tokenizer& tokenizer_;
};

std::optional<Value> parse(std::string_view input) {
    Tokenizer tokenizer(input);
    Parser parser(tokenizer);
    return parser.parse();
}

} // namespace json
