#include "json_tokenizer.h"
#include <cctype>
#include <stdexcept>

namespace json {

Tokenizer::Tokenizer(std::string_view input)
    : input_(input), position_(0) {
    skip_whitespace();
}

void Tokenizer::skip_whitespace() {
    while (position_ < input_.size() && std::isspace(input_[position_])) {
        ++position_;
    }
}

std::optional<Token> Tokenizer::next() {
    skip_whitespace();

    if (position_ >= input_.size()) {
        return Token{TokenType::EndOfInput, "", position_};
    }

    char ch = input_[position_];

    switch (ch) {
        case '{':
            ++position_;
            return Token{TokenType::LeftBrace, "{", position_ - 1};
        case '}':
            ++position_;
            return Token{TokenType::RightBrace, "}", position_ - 1};
        case '[':
            ++position_;
            return Token{TokenType::LeftBracket, "[", position_ - 1};
        case ']':
            ++position_;
            return Token{TokenType::RightBracket, "]", position_ - 1};
        case ':':
            ++position_;
            return Token{TokenType::Colon, ":", position_ - 1};
        case ',':
            ++position_;
            return Token{TokenType::Comma, ",", position_ - 1};
        case '"':
            return read_string();
        case 't':
            return read_keyword("true", TokenType::True);
        case 'f':
            return read_keyword("false", TokenType::False);
        case 'n':
            return read_keyword("null", TokenType::Null);
        default:
            if (ch == '-' || std::isdigit(ch)) {
                return read_number();
            }
            return std::nullopt;
    }
}

std::optional<Token> Tokenizer::peek() const {
    Tokenizer temp = *this;
    return temp.next();
}

bool Tokenizer::has_next() const {
    Tokenizer temp = *this;
    temp.skip_whitespace();
    return temp.position_ < temp.input_.size();
}

std::optional<Token> Tokenizer::read_string() {
    size_t start = position_;
    ++position_; // skip opening quote

    std::string result;

    while (position_ < input_.size() && input_[position_] != '"') {
        if (input_[position_] == '\\') {
            ++position_;
            if (position_ >= input_.size()) return std::nullopt;

            char escaped = input_[position_];
            switch (escaped) {
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                case '/': result += '/'; break;
                case 'b': result += '\b'; break;
                case 'f': result += '\f'; break;
                case 'n': result += '\n'; break;
                case 'r': result += '\r'; break;
                case 't': result += '\t'; break;
                default: return std::nullopt;
            }
        } else {
            result += input_[position_];
        }
        ++position_;
    }

    if (position_ >= input_.size()) return std::nullopt;

    ++position_; // skip closing quote

    return Token{TokenType::String, result, start};
}

std::optional<Token> Tokenizer::read_number() {
    size_t start = position_;

    if (input_[position_] == '-') {
        ++position_;
    }

    if (position_ >= input_.size() || !std::isdigit(input_[position_])) {
        return std::nullopt;
    }

    while (position_ < input_.size() && std::isdigit(input_[position_])) {
        ++position_;
    }

    if (position_ < input_.size() && input_[position_] == '.') {
        ++position_;
        while (position_ < input_.size() && std::isdigit(input_[position_])) {
            ++position_;
        }
    }

    if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
        ++position_;
        if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) {
            ++position_;
        }
        while (position_ < input_.size() && std::isdigit(input_[position_])) {
            ++position_;
        }
    }

    std::string value(input_.substr(start, position_ - start));
    return Token{TokenType::Number, value, start};
}

std::optional<Token> Tokenizer::read_keyword(std::string_view keyword, TokenType type) {
    size_t start = position_;

    if (position_ + keyword.size() > input_.size()) {
        return std::nullopt;
    }

    if (input_.substr(position_, keyword.size()) != keyword) {
        return std::nullopt;
    }

    position_ += keyword.size();

    return Token{type, std::string(keyword), start};
}

} // namespace json
