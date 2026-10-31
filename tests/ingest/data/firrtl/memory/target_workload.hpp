#ifndef WOLVRIX_FIRRTL_TARGET_WORKLOAD_HPP
#define WOLVRIX_FIRRTL_TARGET_WORKLOAD_HPP

#include "workload.hpp"

namespace firrtl_memory_test {
template<class Check> void jtagTrace(Check check) {
    uint64_t state = 0;
    for (unsigned cycle = 0; cycle < 200; ++cycle) {
        const uint64_t action = cycle % 4, data = (cycle / 3) & 1;
        Values inputs{{"clock",0},{"reset",cycle < 2},{"io__DOT__chainIn__DOT__capture",action==0},
            {"io__DOT__chainIn__DOT__shift",action==1},{"io__DOT__chainIn__DOT__update",action==2},{"io__DOT__chainIn__DOT__data",data}};
        Values expected{{"io__DOT__chainOut__DOT__capture",action==0},{"io__DOT__chainOut__DOT__shift",action==1},
            {"io__DOT__chainOut__DOT__update",action==2},{"io__DOT__chainOut__DOT__data",state}};
        check(inputs, expected);
        if (action==0) state=0; else if (action==1) state=data;
        expected["io__DOT__chainOut__DOT__data"]=state;
        check(Values{{"clock",1}}, expected);
        check(Values{{"clock",1}}, expected);
    }
}
template<class Check> void sramTrace(Check check) {
    // Two independently masked 69-bit lanes. Values vary in both the low
    // 64 bits and the high five bits, preserving the original 138-bit ABI.
    std::array<std::array<uint64_t, 4>, 128> memory{};
    uint32_t random = 881991;
    std::array<uint64_t, 4> expected{};
    for (unsigned cycle = 0; cycle < 220; ++cycle) {
        random = random * 1664525u + 1013904223u;
        const uint64_t addr = (random >> 7) & 127, mask = (random >> 15) & 3;
        const bool enable = random & 1, write = random & 2;
        std::array<uint64_t, 4> data{uint64_t(random) * 3232381, (random >> 10) & 31, uint64_t(random) * 9137317, (random >> 20) & 31};
        Values inputs{{"RW0__DOT__clk",0},{"RW0__DOT__addr",addr},{"RW0__DOT__en",enable},{"RW0__DOT__wmode",write},{"RW0__DOT__wmask",mask},
            {"lo0",data[0]},{"hi0",data[1]},{"lo1",data[2]},{"hi1",data[3]}};
        check(inputs, Values{});
        expected = enable && (!write || !mask) ? memory[addr] : std::array<uint64_t, 4>{};
        if (enable && write) {
            if (mask & 1) { memory[addr][0]=data[0]; memory[addr][1]=data[1]; }
            if (mask & 2) { memory[addr][2]=data[2]; memory[addr][3]=data[3]; }
        }
        check(Values{{"RW0__DOT__clk",1}}, Values{{"lo0",expected[0]},{"hi0",expected[1]},{"lo1",expected[2]},{"hi1",expected[3]}});
    }
}
template<class Check> void queueTrace(Check check) {
    std::deque<Values> queue;
    uint32_t random = 102713;
    const std::map<std::string, unsigned> widths{{"opcode",4},{"param",3},{"size",3},{"source",7},{"address",48},{"user__DOT__reqSource",5},{"mask",32},{"data",32},{"corrupt",1}};
    for (unsigned cycle = 0; cycle < 250; ++cycle) {
        const bool reset = cycle < 2 || cycle % 61 == 0;
        random = random * 1664525u + 1013904223u;
        Values incoming, inputs{{"clock",0},{"reset",reset},{"io__DOT__enq__DOT__valid",(random >> 7)&1},{"io__DOT__deq__DOT__ready",(random >> 9)&1}};
        for (const auto& [field, width] : widths) {
            random = random * 1664525u + 1013904223u;
            incoming[field] = uint64_t(random) & ((uint64_t{1} << width) - 1);
            inputs["io__DOT__enq__DOT__bits__DOT__" + field] = incoming[field];
        }
        const auto outputs = [&] {
            Values expected{{"io__DOT__enq__DOT__ready",queue.size()!=2},{"io__DOT__deq__DOT__valid",!queue.empty()},{"io__DOT__count",queue.size()}};
            if (!queue.empty()) for (const auto& [field, value] : queue.front()) expected["io__DOT__deq__DOT__bits__DOT__"+field]=value;
            return expected;
        };
        check(inputs, outputs());
        const bool enqueue = queue.size() != 2 && inputs.at("io__DOT__enq__DOT__valid"), dequeue = !queue.empty() && inputs.at("io__DOT__deq__DOT__ready");
        if (reset) queue.clear();
        else { if (dequeue) queue.pop_front(); if (enqueue) queue.push_back(incoming); }
        check(Values{{"clock",1}}, outputs());
        check(Values{{"clock",1}}, outputs());
    }
}
}
#endif
