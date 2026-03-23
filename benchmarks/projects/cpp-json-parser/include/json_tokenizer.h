#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <optional>

namespace json {

enum class TokenType {
    LeftBrace,      // {
    RightBrace,     // }
    LeftBracket,    // [
    RightBracket,   // ]
    Colon,          // :
    Comma,          // ,
    String,
    Number,
    True,
    False,
    Null,
    EndOfInput
};

struct Token {
    TokenType type;
    std::string value;
    size_t position;
};

class Tokenizer {
public:
    explicit Tokenizer(std::string_view input);

    std::optional<Token> next();
    std::optional<Token> peek() const;
    bool has_next() const;

private:
    void skip_whitespace();
    std::optional<Token> read_string();
    std::optional<Token> read_number();
    std::optional<Token> read_keyword(std::string_view keyword, TokenType type);

    std::string_view input_;
    size_t position_;
};

} // namespace json
