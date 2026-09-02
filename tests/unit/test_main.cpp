#include "test_harness.hpp"

int main() {
    TestRunner runner;
    run_protocol_tests(runner);
    run_frame_parser_tests(runner);
    run_unique_fd_tests(runner);
    run_socket_io_tests(runner);
    run_ping_pong_tests(runner);
    run_transfer_codec_tests(runner);
    run_file_transfer_tests(runner);
    return runner.finish();
}
