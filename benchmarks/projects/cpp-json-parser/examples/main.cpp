#include "json_parser.h"
#include <iostream>
#include <fstream>
#include <sstream>

int main(int argc, char* argv[]) {
    std::string input;

    if (argc > 1) {
        // Read from file
        std::ifstream file(argv[1]);
        if (!file) {
            std::cerr << "Error: Could not open file " << argv[1] << std::endl;
            return 1;
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        input = buffer.str();
    } else {
        // Example JSON
        input = R"({
            "name": "Silex",
            "version": 1.0,
            "fast": true,
            "features": ["clang", "mold", "ninja", "sccache"],
            "metadata": {
                "author": "CS Student",
                "license": "MIT"
            }
        })";
    }

    std::cout << "Parsing JSON..." << std::endl;

    auto result = json::parse(input);

    if (result) {
        std::cout << "Parse successful!" << std::endl;
        std::cout << "Type: " << static_cast<int>(result->type()) << std::endl;

        if (result->is_object()) {
            auto& obj = result->as_object();
            std::cout << "Object size: " << obj.size() << std::endl;
        }
    } else {
        std::cerr << "Parse failed!" << std::endl;
        return 1;
    }

    return 0;
}
