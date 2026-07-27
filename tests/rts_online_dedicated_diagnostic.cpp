#include <cstdio>
#include <cstdlib>

namespace std {
[[noreturn]] inline void rtsDiagnosticAbort(int line) {
    std::fprintf(stderr, "dedicated online assertion failed at line %d\n", line);
    std::_Exit(134);
}
} // namespace std

#define abort() rtsDiagnosticAbort(__LINE__)
#define main rts_online_hardening_combined_main
#include "rts_online_hardening_tests.cpp"
#undef main
#undef abort

int main() {
    testDedicatedServerHasNoLocalPlayer();
    return EXIT_SUCCESS;
}
