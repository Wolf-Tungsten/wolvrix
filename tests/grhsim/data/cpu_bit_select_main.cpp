#include "grhsim_cpu_bit_select.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>

int main() {
    GrhSIM_cpu_bit_select model;
    unsigned checks = 0;
    std::uint64_t random = UINT64_C(0x9e3779b97f4a7c15);
    const auto signed5 = [](std::uint64_t value) { return static_cast<int>((value & 31) ^ 16) - 16; };
    for (unsigned reset = 0; reset < 3; ++reset) {
        model.init();
        bool q = false, r = false, previousClock = false;
        for (unsigned step = 0; step < 4096; ++step) {
            random ^= random << 13; random ^= random >> 7; random ^= random << 17;
            model.a = step & 1; model.b = (step >> 1) & 1; model.c = (step >> 2) & 1;
            model.clock = (step >> 3) & 1;
            model.condition = step % 3 ? (step / 3) & 255 : 0;
            model.mask64 = random; model.a64 = ~random ^ step; model.b64 = random * 17;
            model.mask5 = signed5(random); model.a5 = signed5(random >> 8); model.b5 = signed5(random >> 16);
            const bool selected = model.c ? model.a : model.b;
            const bool nested = selected ? model.b : model.c;
            if (!previousClock && model.clock) { r = q; q = nested; }
            previousClock = model.clock;
            // Independent per-bit scoreboard for mask semantics, including the top bit.
            std::uint64_t selected64 = 0;
            unsigned selected5 = 0;
            for (unsigned bit = 0; bit < 64; ++bit) {
                const auto source = (model.mask64 >> bit) & 1 ? model.a64 : model.b64;
                selected64 |= ((source >> bit) & UINT64_C(1)) << bit;
            }
            for (unsigned bit = 0; bit < 5; ++bit) {
                const unsigned source = (static_cast<unsigned>(model.mask5) >> bit) & 1 ? model.a5 : model.b5;
                selected5 |= ((source >> bit) & 1) << bit;
            }
            for (unsigned repeat = 0; repeat < 2; ++repeat) {
                model.eval();
                if (model.selected != selected || model.nested != nested ||
                    model.nonbool != (model.condition ? model.a : model.b) ||
                    model.selected64 != selected64 || model.selected5 != signed5(selected5) ||
                    model.registered != q || model.delayed != r)
                    throw std::runtime_error("bitSelect changed selection, sign, shared dependency or commit snapshot");
                ++checks;
            }
        }
    }
    std::cout << "bitSelect checks=" << checks << " init=3\n";
}
