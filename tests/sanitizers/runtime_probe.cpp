// Deliberate faults in a developer-only subprocess: the harness requires ASan
// to diagnose them, proving the runtime really executes on the CI runner.
#include <cstring>

int main(int argc, char** argv)
{
    if (argc != 2) return 64;
    auto* allocation = new unsigned char[16];
    allocation[0] = 42;
    volatile unsigned char* bytes = allocation;
    unsigned char result = 0;
    if (std::strcmp(argv[1], "heap-overflow") == 0) {
        result = bytes[16];
    } else if (std::strcmp(argv[1], "use-after-free") == 0) {
        delete[] allocation;
        return bytes[0];
    } else {
        delete[] allocation;
        return 64;
    }
    delete[] allocation;
    return result;
}
