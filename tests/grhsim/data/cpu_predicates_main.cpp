#include "grhsim_cpu_predicates.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>

int main()
{
    GrhSIM_cpu_predicates model;
    unsigned checks = 0;
    for (unsigned reset = 0; reset < 3; ++reset)
    {
        model.init();
        bool q = false, previousClock = false;
        for (unsigned step = 0; step < 65536; ++step)
        {
            model.a = step & 1; model.b = (step >> 1) & 1; model.c = (step >> 2) & 1;
            model.x = step & 255; model.y = step >> 8;
            model.s = (step & 8) ? -1 : 0;
            const bool land = model.a && model.b, lor = model.a || model.b;
            if (!previousClock && model.c && lor) q = land;
            previousClock = model.c;
            for (unsigned repeat = 0; repeat < 2; ++repeat)
            {
                model.eval();
                if (model.land != land || model.lor != lor || model.chain != ((lor && model.c) || land) ||
                    model.wide_and != ((model.x != 0) && (model.y != 0)) ||
                    model.wide_or != ((model.x != 0) || (model.y != 0)) ||
                    model.signed_and != ((model.s != 0) && model.a) ||
                    model.q != q || model.state_and != (q && model.a))
                    throw std::runtime_error("predicate normalization changed truth table, wakeup, or edge write");
                ++checks;
            }
        }
    }
    std::cout << "bitwise predicate PASS checks=" << checks << " resets=3\n";
}
