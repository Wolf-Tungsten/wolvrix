#include "grhsim_cpu_constants.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
    unsigned calls = 0;

    void require(bool condition) {
        if (!condition) throw std::runtime_error("scalar constant scoreboard mismatch");
    }

    int signedFive(unsigned value) {
        value &= 31;
        return value < 16 ? int(value) : int(value) - 32;
    }
}

extern "C" void cpu_constant_inout(std::uint8_t *value) {
    require(*value == 5);
    *value = 12;
    ++calls;
}

int main() {
    GrhSIM_cpu_constants model;
    for (unsigned reset = 0; reset < 4; ++reset) {
        model.init();
        calls = 0;
        unsigned expectedCalls = 0, masked = 0xa0;
        const unsigned firstData = 19 + reset * 23;
        bool previous = false;
        for (unsigned step = 0; step < 4096; ++step) {
            model.clock = (step % 5) >= 2;
            model.data = step == 0 ? firstData : (step * 29 + reset * 17) & 255;
            model.unsigned_data = UINT64_C(0xfedcba9876543210) * step + reset;
            model.signed_data = signedFive(step);
            if (model.clock && !previous) {
                masked = 0xa0 | (model.data & 15);
                ++expectedCalls;
            }
            for (unsigned repeat = 0; repeat < 2; ++repeat) {
                model.eval();
                require(model.u1 && model.s1 == -1 && model.u5 == 31 && model.s5 == -3);
                require(model.u64 == UINT64_MAX && model.s64 == std::numeric_limits<std::int64_t>::min());
                require(model.xz == 0 && model.inout_initial == 5);
                require(model.unsigned_sum == model.unsigned_data - UINT64_C(1));
                require(model.signed_sum == signedFive(unsigned(int(model.signed_data) - 3)));
                require(model.masked == masked && model.constant_event == firstData);
                require(calls == expectedCalls && model.inout_result == (expectedCalls ? 12 : 0));
            }
            previous = model.clock;
        }
    }
    std::cout << "Scalar constants: 32768 evals / 4 init, arithmetic, events, masks and DPI passed\n";
}
