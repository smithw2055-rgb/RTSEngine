#define main rts_online_hardening_combined_main
#include "rts_online_hardening_tests.cpp"
#undef main

int main() {
    testDedicatedServerHasNoLocalPlayer();
    return EXIT_SUCCESS;
}
