#include "grhsim_cpu_commit_batch.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>

int main() {
    GrhSIM_cpu_commit_batch model;
    unsigned checks = 0;
    std::uint64_t random = UINT64_C(0x9e3779b97f4a7c15);
    const auto next = [&]() { random ^= random << 13; random ^= random >> 7; random ^= random << 17; return random; };
    for (unsigned reset = 0; reset < 3; ++reset) {
        model.init();
        std::uint64_t q[64] = {}, qm = 0, qc = 0;
        bool previousClock = false;
        for (unsigned step = 0; step < 4096; ++step) {
            const std::uint64_t dense = next();
            model.enw = (dense & 1) ? next() : next() & UINT64_C(0x0101010101010101);
            model.dw = next();
            model.mm = next();
            model.dm = next();
            model.enm = (next() & 3) == 1;
            model.dc = next();
            model.clock = (step >> 3) & 1;
            if (!previousClock && model.clock) {
                for (unsigned i = 0; i < 64; ++i)
                    if ((model.enw >> i) & 1) q[i] = model.dw + i;
                if (model.enm) qm = (qm & ~model.mm) | (model.dm & model.mm);
                qc = model.dc;
            }
            previousClock = model.clock;
            std::uint64_t expect = qm ^ qc;
            for (unsigned i = 0; i < 64; ++i) expect ^= q[i];
            for (unsigned repeat = 0; repeat < 2; ++repeat) {
                model.eval();
                if (model.x != expect)
                    throw std::runtime_error("commit compact walk changed masked u64 commit semantics");
                ++checks;
            }
        }
    }
    std::cout << "commitCompactWalk checks=" << checks << " init=3\n";
}
