#include "grhsim_cpu_helper_read_cache.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>

int main()
{
    GrhSIM_cpu_helper_read_cache model;
    unsigned checks = 0;
    for (unsigned reset = 0; reset < 3; ++reset)
    {
        model.init();
        std::uint8_t q0 = 0, q1 = 0;
        bool previousClock = false;
        for (unsigned step = 0; step < 4096; ++step)
        {
            model.a = (step * 37 + reset) & 255;
            model.b = (step / 7) & 255;
            model.clock = step % 3 != 0;
            model.enable = step % 5 != 0;
            const auto sum = static_cast<std::uint8_t>(model.a + model.b);
            if (!previousClock && model.clock)
            {
                if (model.enable) q0 = sum;
                else q1 = sum;
            }
            previousClock = model.clock;
            for (unsigned repeat = 0; repeat < 2; ++repeat)
            {
                model.eval();
                if (model.sum != sum || model.equal != 0 || model.not_equal ||
                    model.ab != static_cast<std::uint8_t>(model.a - model.b) ||
                    model.ba != static_cast<std::uint8_t>(model.b - model.a) ||
                    model.small != (sum & 15) || model.signed_sum != static_cast<std::int8_t>(sum) ||
                    model.slice0 != (model.a & 15) || model.slice1 != ((model.a >> 1) & 15) ||
                    model.q0 != q0 || model.q1 != q1)
                    throw std::runtime_error("helper read cache lost a value, normalization, or independent commit notification");
                ++checks;
            }
        }
    }
    std::cout << "helper read cache checks=" << checks << " init=3\n";
}
