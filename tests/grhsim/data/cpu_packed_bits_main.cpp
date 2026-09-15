#include "grhsim_cpu_packed_bits.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>

extern "C" void packed_private(std::uint8_t a, std::uint8_t b)
{
    std::cout.put(static_cast<char>((a & 1) | ((b & 1) << 1)));
}

int main()
{
    GrhSIM_cpu_packed_bits model;
    std::mt19937_64 random(324911);
    constexpr std::array counts{2u, 64u, 130u, 3u, 3u};
    unsigned checks = 0;
    for (unsigned reset = 0; reset < 3; ++reset)
    {
        model.clock = reset % 2; model.aux = !(reset % 2);
        model.enable = model.enable2 = false; model.mask = false; model.data = 0;
        model.init();
        std::array<std::array<bool, 130>, 5> q{};
        std::array<bool, 5> oldClock{false, false, false, true, false};
        bool oldAux = true;
        for (unsigned group = 0; group < counts.size(); ++group)
            for (unsigned i = 0; i < counts[group]; ++i) q[group][i] = i % 2;
        for (unsigned step = 0; step < 4096; ++step)
        {
            if (step)
            {
                if (step % 3 == 0) model.clock = !model.clock;
                if (step % 5 == 0) model.aux = !model.aux;
                model.enable = step % 7 != 0; model.enable2 = step % 4 != 0;
                model.mask = step % 6 != 0; model.data = random();
            }
            for (unsigned group = 0; group < counts.size(); ++group)
            {
                bool edge = group == 1 ? oldClock[group] && !model.clock : !oldClock[group] && model.clock;
                if (group == 2) edge |= oldAux && !model.aux;
                const auto previous = q[group];
                if (edge && model.mask && (group == 4 ? model.enable2 : model.enable))
                    for (unsigned i = 0; i < counts[group]; ++i)
                        q[group][i] = ((model.data >> (i % 64)) & 1) ^ (group == 2 && previous[(i + 1) % counts[group]]);
                oldClock[group] = model.clock;
            }
            oldAux = model.aux;
            for (unsigned repeat = 0; repeat < 2; ++repeat)
            {
                model.eval();
#define SAMPLE(I) std::cout.put(static_cast<char>(model.excluded##I##_0)); std::cout.put(static_cast<char>(model.excluded##I##_1))
                SAMPLE(0); SAMPLE(1); SAMPLE(2); SAMPLE(3); SAMPLE(4); SAMPLE(5); SAMPLE(6); SAMPLE(7);
#undef SAMPLE
                std::cout.put(static_cast<char>(model.excluded5_0_hist));
                std::cout.put(static_cast<char>(model.excluded5_1_hist));
                const std::array<std::uint64_t, 5> scalars{model.out0, model.out1, 0, model.out3, model.out4};
                for (unsigned group = 0; group < counts.size(); ++group)
                    for (unsigned i = 0; i < counts[group]; ++i)
                    {
                        const auto word = group == 2 ? model.out2[i / 64] : scalars[group];
                        if (bool((word >> (i % 64)) & 1) != q[group][i])
                            throw std::runtime_error("packed register lost snapshot, edge, enable, mask, initialization, or high bit");
                    }
                ++checks;
            }
        }
    }
    std::cerr << "packed bit scoreboard checks=" << checks << " init=3\n";
}
