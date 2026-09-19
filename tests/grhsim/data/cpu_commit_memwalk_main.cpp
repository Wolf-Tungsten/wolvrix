#include "grhsim_cpu_commit_memwalk.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>

int main() {
    GrhSIM_cpu_commit_memwalk model;
    unsigned checks = 0;
    std::uint64_t random = UINT64_C(0x9e3779b97f4a7c15);
    const auto next = [&]() { random ^= random << 13; random ^= random >> 7; random ^= random << 17; return random; };
    for (unsigned reset = 0; reset < 3; ++reset) {
        model.init();
        std::uint64_t mem[16] = {}, mem2[16] = {};
        bool previousClock = false, previousClock2 = false;
        for (unsigned step = 0; step < 4096; ++step) {
            model.enw = next();
            model.dw = next();
            model.aw = static_cast<std::uint8_t>(next());
            model.clock = (step >> 3) & 1;
            model.clock2 = ((step >> 2) & 1) ^ 1;
            if (!previousClock && model.clock) {
                for (unsigned i = 0; i < 12; ++i) {
                    const unsigned bit = i < 4 ? 0 : i;
                    const unsigned addr = (static_cast<unsigned>(model.aw) + i) & 0xff;
                    if (((model.enw >> bit) & 1) && addr < 16) mem[addr] = model.dw + i;
                }
            }
            previousClock = model.clock;
            if (!previousClock2 && model.clock2) {
                for (unsigned i = 0; i < 3; ++i) {
                    const unsigned addr = (static_cast<unsigned>(model.aw) + i) & 0xff;
                    if (((model.enw >> (16 + i)) & 1) && addr < 16) mem2[addr] = model.dw + i;
                }
            }
            previousClock2 = model.clock2;
            std::uint64_t expect = 0;
            for (unsigned i = 0; i < 16; ++i) expect ^= mem2[i];
            for (unsigned i = 0; i < 16; ++i) expect ^= mem[i];
            for (unsigned repeat = 0; repeat < 2; ++repeat) {
                model.eval();
                if (model.x != expect)
                    throw std::runtime_error("commit mem walk changed guarded memory write semantics");
                ++checks;
            }
        }
    }
    std::cout << "commitMemWalk checks=" << checks << " init=3\n";
}
