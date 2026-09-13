#include "grhsim_cpu_identity_assign.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>

int main()
{
    GrhSIM_cpu_identity_assign model;
    std::uint64_t random = 0x39db236df712ae91;
    const auto next = [&]() {
        random ^= random << 13; random ^= random >> 7; random ^= random << 17; return random;
    };
    constexpr std::array<std::uint64_t, 3> masks{1, 31, UINT64_MAX};
    unsigned checks = 0;
    for (unsigned reset = 0; reset < 4; ++reset)
    {
        model.init();
        std::array<std::uint64_t, 3> q0{}, q1{};
        bool previousClock = false;
        for (unsigned step = 0; step < 2048; ++step)
        {
            const std::array a{next() & masks[0], std::uint64_t(step & 31), next()};
            const std::array b{next() & masks[0], std::uint64_t((step >> 5) & 31), next()};
            model.a0 = a[0]; model.b0 = b[0];
            model.a1 = static_cast<std::int8_t>(a[1] >= 16 ? int(a[1]) - 32 : int(a[1]));
            model.b1 = static_cast<std::int8_t>(b[1] >= 16 ? int(b[1]) - 32 : int(b[1]));
            model.a2 = a[2]; model.b2 = b[2];
            model.clock = (step % 3) != 0; model.enable = (step % 7) != 0;
            std::array<std::uint64_t, 3> sum{}, mix{}, combined{};
            for (unsigned i = 0; i < 3; ++i)
            {
                sum[i] = (a[i] + b[i]) & masks[i];
                mix[i] = (sum[i] ^ b[i]) & masks[i];
                combined[i] = (sum[i] + mix[i]) & masks[i];
                if (!previousClock && model.clock && model.enable) { q1[i] = q0[i]; q0[i] = sum[i]; }
            }
            previousClock = model.clock;
            for (unsigned repeat = 0; repeat < 2; ++repeat)
            {
                model.eval();
                const auto check = [&](unsigned i, auto actualSum, auto actualMix, auto actualCombined, auto actualQ0, auto actualQ1) {
                    const auto bits = [&](auto value) { return std::uint64_t(value) & masks[i]; };
                    if (bits(actualSum) != sum[i] || bits(actualMix) != mix[i] || bits(actualCombined) != combined[i] ||
                        bits(actualQ0) != q0[i] || bits(actualQ1) != q1[i])
                        throw std::runtime_error("identity assignment value or clock snapshot differs");
                    if (i == 1 && (int(actualSum) != (sum[i] >= 16 ? int(sum[i]) - 32 : int(sum[i])) ||
                        int(actualCombined) != (combined[i] >= 16 ? int(combined[i]) - 32 : int(combined[i]))))
                        throw std::runtime_error("identity assignment lost signed normalization");
                };
                check(0, model.sum0, model.mix0, model.combined0, model.q0_0, model.q1_0);
                check(1, model.sum1, model.mix1, model.combined1, model.q0_1, model.q1_1);
                check(2, model.sum2, model.mix2, model.combined2, model.q0_2, model.q1_2);
                ++checks;
            }
        }
    }
    std::cout << "identity assignment checks=" << checks << " init=4\n";
}
