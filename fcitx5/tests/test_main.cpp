#include <cstdlib>

int run_config_tests();
int run_bopomofo_tests();
int run_buffer_tests();
int run_protocol_tests();
int run_service_transport_tests();
int run_keypad_tests();

int main() {
    if (run_config_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_bopomofo_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_buffer_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_protocol_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_service_transport_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    if (run_keypad_tests() != EXIT_SUCCESS) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
