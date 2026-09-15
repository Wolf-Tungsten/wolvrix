#include "grhsim_cpu_clones.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>

int main()
{
    GrhSIM_cpu_clones model;
    unsigned checks = 0;
    for (unsigned reset = 0; reset < 3; ++reset)
    {
        model.init();
        std::uint8_t q0 = 0, q1 = 0;
        bool oldClock = false;
        for (unsigned step = 0; step < 4096; ++step)
        {
            model.a = (step * 37 + reset) & 255;
            model.b = (step / 7) & 255;
            model.clock = step % 3 != 0;
            model.inhibit = step % 5 == 0;
            model.word = (std::uint64_t(step) * UINT64_C(0x9e3779b97f4a7c15)) ^ (std::uint64_t(reset) << 63);
            if (!oldClock && model.clock && !model.inhibit)
            {
                q1 = static_cast<std::uint8_t>(~q0);
                q0 = model.a;
            }
            oldClock = model.clock;
            for (unsigned repeat = 0; repeat < 2; ++repeat)
            {
                model.eval();
                const auto inverted = static_cast<std::uint8_t>(~q0);
                if (model.q0 != q0 || model.q1 != q1 || model.left != (inverted & model.a) ||
                    model.right != (inverted | model.b) || model.duplicate != 0 ||
                    model.enabled_clock != (!model.inhibit && model.clock) ||
                    model.enabled_or_clock != (!model.inhibit || model.clock) || model.raw_inhibit != model.inhibit)
                    throw std::runtime_error("localized producer lost a wakeup, a commit snapshot, or an operand use");
                const auto word = model.word;
                const std::array<std::uint64_t, 5> expected{~word, word ^ 5, word + 5, word - 5, 5 - word};
                const std::array ands{model.and0, model.and1, model.and2, model.and3, model.and4};
                const std::array xors{model.xor0, model.xor1, model.xor2, model.xor3, model.xor4};
                for (std::size_t i = 0; i < expected.size(); ++i)
                    if (ands[i] != (expected[i] & word) || xors[i] != (expected[i] ^ word))
                        throw std::runtime_error("localized scalar bijection lost wraparound or operand order");
                const auto chained = ~word + UINT64_C(5);
                if (model.and_chain != (chained & word) || model.xor_chain != (chained ^ word))
                    throw std::runtime_error("localized bijection chain lost a dependency");
                ++checks;
            }
        }
    }
    std::cout << "shared compute clone checks=" << checks << " init=3\n";
}
