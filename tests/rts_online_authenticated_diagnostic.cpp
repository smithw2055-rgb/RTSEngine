#define main rts_online_hardening_combined_main
#include "rts_online_hardening_tests.cpp"
#undef main

int main() {
    testAuthenticatedEncryptedRuntimeAndMigration();
    return EXIT_SUCCESS;
}
