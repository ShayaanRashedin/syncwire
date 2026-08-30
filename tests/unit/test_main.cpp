#include "test_harness.hpp"

int main() {
    TestRunner runner;
    run_protocol_tests(runner);
    run_frame_parser_tests(runner);
    return runner.finish();
}

