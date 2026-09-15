#include "test_harness.hpp"

int main() {
    TestRunner runner;
    std::cout << "SUITE: authentication" << std::endl;
    run_authentication_tests(runner);
    std::cout << "SUITE: resume" << std::endl;
    run_resume_tests(runner);
    std::cout << "SUITE: protocol and I/O" << std::endl;
    run_protocol_tests(runner);
    run_frame_parser_tests(runner);
    run_unique_fd_tests(runner);
    run_socket_io_tests(runner);
    run_ping_pong_tests(runner);
    run_transfer_codec_tests(runner);
    run_file_transfer_tests(runner);
    std::cout << "SUITE: directory sync" << std::endl;
    run_directory_sync_tests(runner);
    std::cout << "SUITE: concurrent server / repeated shutdown" << std::endl;
    run_concurrent_server_tests(runner);
    return runner.finish();
}
