#include <cstdlib>
#include <iostream>

int run_config_tests();
int run_bopomofo_tests();
int run_buffer_tests();
int run_protocol_tests();
int run_service_transport_tests();
int run_keypad_tests();

int main() {
    if (run_config_tests() != EXIT_SUCCESS) { std::cerr << "config tests failed\n"; return EXIT_FAILURE; }
    if (run_bopomofo_tests() != EXIT_SUCCESS) { std::cerr << "bopomofo tests failed\n"; return EXIT_FAILURE; }
    if (run_buffer_tests() != EXIT_SUCCESS) { std::cerr << "buffer tests failed\n"; return EXIT_FAILURE; }
    if (run_protocol_tests() != EXIT_SUCCESS) { std::cerr << "protocol tests failed\n"; return EXIT_FAILURE; }
    if (run_service_transport_tests() != EXIT_SUCCESS) { std::cerr << "service transport tests failed\n"; return EXIT_FAILURE; }
    if (run_keypad_tests() != EXIT_SUCCESS) { std::cerr << "keypad tests failed\n"; return EXIT_FAILURE; }
    return EXIT_SUCCESS;
}
