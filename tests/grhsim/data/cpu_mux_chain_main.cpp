#include "grhsim_cpu_mux_chain.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>

int main() {
    GrhSIM_cpu_mux_chain model;
    unsigned checks = 0;
    std::uint64_t random = UINT64_C(0x9e3779b97f4a7c15);
    const auto next = [&]() { random ^= random << 13; random ^= random >> 7; random ^= random << 17; return random; };
    for (unsigned reset = 0; reset < 3; ++reset) {
        model.init();
        std::uint8_t q = 0, qd = 0;
        bool previousClock = false;
        for (unsigned step = 0; step < 4096; ++step) {
            const std::uint64_t value = next();
            const bool dense = (value & 1) != 0;
            const auto pick = [&](unsigned shift) {
                return dense ? ((value >> shift) & 1) != 0 : ((value >> shift) & 15) == 1;
            };
            model.c0 = pick(1); model.c1 = pick(2); model.c2 = pick(3); model.c3 = pick(4);
            model.e0 = pick(5); model.e1 = pick(6); model.e2 = pick(7); model.e3 = pick(8); model.e4 = pick(9);
            model.f0 = pick(10); model.f1 = pick(11);
            model.a0 = value >> 16; model.a1 = value >> 24; model.a2 = value >> 32;
            model.a3 = value >> 40; model.a4 = value >> 48; model.d = value >> 56;
            model.clock = (step >> 3) & 1;
            const std::uint8_t sel = model.c0 ? model.a0 : model.c1 ? model.a1 : model.c2 ? model.a2 : model.c3 ? model.a3 : model.d;
            const std::uint8_t pair = model.f0 ? model.a0 : model.f1 ? model.a1 : model.d;
            const std::uint8_t tapped = model.e2 ? model.a2 : model.e3 ? model.a3 : model.e4 ? model.a4 : model.d;
            if (!previousClock && model.clock) { qd = q; q = sel; }
            previousClock = model.clock;
            for (unsigned repeat = 0; repeat < 2; ++repeat) {
                model.eval();
                if (model.sel != sel || model.pair != pair || model.tapped != tapped ||
                    model.qt != q || model.qd != qd)
                    throw std::runtime_error("mux chain fold changed priority selection or commit snapshots");
                ++checks;
            }
        }
    }
    std::cout << "muxChainFold checks=" << checks << " init=3\n";
}
