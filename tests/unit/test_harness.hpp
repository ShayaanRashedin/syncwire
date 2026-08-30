#pragma once

#include <iostream>
#include <string_view>

class TestRunner {
public:
    void expect(const bool condition, const std::string_view description) {
        ++checks_;
        if (!condition) {
            ++failures_;
            std::cerr << "FAIL: " << description << '\n';
        }
    }

    [[nodiscard]] int finish() const {
        if (failures_ == 0) {
            std::cout << "PASS: " << checks_ << " checks\n";
            return 0;
        }

        std::cerr << "FAILED: " << failures_ << " of " << checks_ << " checks\n";
        return 1;
    }

private:
    int checks_{0};
    int failures_{0};
};

void run_protocol_tests(TestRunner& runner);
void run_frame_parser_tests(TestRunner& runner);

