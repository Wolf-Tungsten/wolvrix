#include "grhsim_cpu_replicate_broadcast.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>

int main() {
    GrhSIM_cpu_replicate_broadcast model;
    unsigned checks = 0;
    std::uint64_t random = UINT64_C(0x9e3779b97f4a7c15);
    for (unsigned reset = 0; reset < 3; ++reset) {
        model.init();
        bool q = false, previousClock = false;
        for (unsigned step = 0; step < 4096; ++step) {
            random ^= random << 13; random ^= random >> 7; random ^= random << 17;
            model.b = (random >> 5) & 1;
            model.clock = step & 1;
            model.w8 = random & 255;
            if (!previousClock && model.clock) q = model.b;
            previousClock = model.clock;
            const std::uint64_t wide = model.b ? UINT64_MAX : 0;
            for (unsigned repeat = 0; repeat < 2; ++repeat) {
                model.eval();
                if (model.r1 != (model.b != 0) || model.r3 != (model.b ? 7u : 0u) ||
                    model.r32 != (model.b ? 0xffffffffu : 0u) ||
                    model.r64 != wide || model.r66[0] != wide || model.r66[1] != (model.b ? 3u : 0u) ||
                    model.r130[0] != wide || model.r130[1] != wide || model.r130[2] != (model.b ? 3u : 0u) ||
                    model.q != q || model.q32 != (q ? 0xffffffffu : 0u) ||
                    model.wide9[0] != model.w8 * UINT64_C(0x0101010101010101) || model.wide9[1] != model.w8)
                    throw std::runtime_error("replicate broadcast changed behavior");
                ++checks;
            }
        }
    }
    std::cout << "replicate broadcast checks=" << checks << " init=3\n";
}
