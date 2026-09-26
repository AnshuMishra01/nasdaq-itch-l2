#include "decode.h"

#include <cstdint>
#include <iostream>

int main() {
    const unsigned char bytes[6] = {0x0A, 0x11, 0xEA, 0x0E, 0x8C, 0x43};
    const std::uint64_t value = read_be<std::uint64_t, 6>(bytes);
    std::cout << std::hex << value << '\n';
    return 0;
}