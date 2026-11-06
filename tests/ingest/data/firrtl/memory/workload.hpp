#ifndef WOLVRIX_FIRRTL_MEMORY_WORKLOAD_HPP
#define WOLVRIX_FIRRTL_MEMORY_WORKLOAD_HPP

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace firrtl_memory_test {
using Values = std::map<std::string, uint64_t>;
struct Request {
    uint64_t address = 0, data = 0;
    bool enabled = false;
};
template<class Check> void chirrtlTrace(Check check) {
    std::array<uint64_t, 3> memory{};
    uint32_t random = 700031;
    for (unsigned cycle = 0; cycle < 180; ++cycle) {
        random = random * 1664525u + 1013904223u;
        const uint64_t addr = (random >> 7) & 3, lane = (random >> 9) & 3;
        const uint64_t data = (random >> 16) & 255, part = (random >> 13) & 15;
        const bool whole = random & 1, partial = random & 2;
        check(Values{{"clk",0},{"addr",addr},{"lane",lane},{"data",data},{"part",part},{"whole",whole},{"partial",partial}}, Values{});
        if (addr < memory.size()) {
            if (whole) memory[addr] = data;
            if (partial && lane < 2) {
                const auto shift = lane * 4;
                memory[addr] = (memory[addr] & ~(uint64_t{15} << shift)) | (part << shift);
            }
        }
        check(Values{{"clk",1}}, Values{{"y",addr < memory.size() ? memory[addr] : 0}});
    }
}
// Requests occupy latency-1 pending slots after their initiating edge. The
// final edge completes the operation; completed reads no longer overlap writes.
template<class Check> void independentTrace(int readLatency, int writeLatency, const std::string& policy, Check check) {
    std::array<uint64_t, 3> memory{};
    std::deque<Request> writes(writeLatency - 1), reads(readLatency - 1);
    Request completed;
    uint64_t rclk = 0, wclk = 0;
    uint32_t random = 982451653;
    for (unsigned tick = 0; tick < 480; ++tick) {
        random = random * 1664525u + 1013904223u;
        const uint64_t nextRead = (tick / 3) & 1, nextWrite = (tick / 2) & 1;
        const bool readEdge = !rclk && nextRead, writeEdge = !wclk && nextWrite;
        const uint64_t raddr = tick < 60 ? 1 : (random >> 9) & 3;
        const uint64_t waddr = tick < 60 ? 1 : (random >> 11) & 3;
        const bool ren = tick < 60 || ((random >> 2) & 1);
        const bool wen = tick < 60 || (random & 1), mask = tick < 60 || ((random >> 3) & 1);
        const uint64_t data = (random >> 16) & 255;
        Request read{raddr, raddr < memory.size() ? memory[raddr] : 0, ren && raddr < memory.size()};
        Request write{waddr, data, wen && mask && waddr < memory.size()};
        if (writeEdge) {
            const auto committed = writes.empty() ? write : writes.front();
            if (!writes.empty()) { writes.pop_front(); writes.push_back(write); }
            if (committed.enabled) memory[committed.address] = committed.data;
        }
        if (readEdge) {
            completed = reads.empty() ? read : reads.front();
            if (!reads.empty()) { reads.pop_front(); reads.push_back(read); }
        }
        const uint64_t expected = policy == "undefined" ?
            (completed.address < memory.size() ? memory[completed.address] : 0) : completed.enabled ?
            (policy == "new" ? memory[completed.address] : completed.data) : 0;
        rclk = nextRead; wclk = nextWrite;
        Values inputs{{"rclk", rclk}, {"wclk", wclk}, {"ren", ren}, {"wen", wen}, {"mask", mask},
                      {"raddr", raddr}, {"waddr", waddr}, {"data", data}};
        check(inputs, Values{{"y", expected}});
        check(inputs, Values{{"y", expected}});
    }
}
template<class Check> void multiClockTrace(Check check) {
    std::array<uint64_t, 3> memory{};
    uint64_t c0 = 0, c1 = 0;
    uint32_t random = 5359;
    for (unsigned cycle = 0; cycle < 240; ++cycle) {
        random = random * 1664525u + 1013904223u;
        const uint64_t n0 = (random >> 9) & 1, n1 = (random >> 11) & 1;
        const uint64_t a0 = (random >> 14) & 3, a1 = (random >> 16) & 3, r = (random >> 18) & 3;
        const bool w0 = !c0 && n0 && (random & 1) && a0 < 3, w1 = !c1 && n1 && (random & 2) && a1 < 3;
        const uint64_t d0 = (random >> 20) & 255, d1 = (random >> 12) & 255;
        if (w0) memory[a0] = d0;
        if (w1) memory[a1] = d1;
        c0 = n0; c1 = n1;
        check(Values{{"c0", c0}, {"c1", c1}, {"a0", a0}, {"a1", a1}, {"d0", d0}, {"d1", d1}, {"e0", random & 1}, {"e1", (random >> 1) & 1}, {"readAddr", r}}, Values{{"y", r < 3 ? memory[r] : 0}});
    }
}
template<class Check> void maskTrace(Check check) {
    std::array<uint64_t, 3> a{}, b{};
    uint32_t random = 78213;
    for (unsigned cycle = 0; cycle < 160; ++cycle) {
        random = random * 1664525u + 1013904223u;
        const uint64_t addr = (random >> 7) & 3, data = (random >> 16) & 255, sign = (random >> 12) & 31, mask = (random >> 9) & 3;
        const bool wen = random & 1, bm = random & 2;
        check(Values{{"clk", 0}, {"wen", wen}, {"addr", addr}, {"data__DOT__a", data}, {"data__DOT__b", sign}, {"mask__DOT__a", mask}, {"mask__DOT__b", bm}}, Values{});
        if (wen && addr < 3) {
            if (mask & 1) a[addr] = (a[addr] & 0xf0) | (data & 15);
            if (mask & 2) a[addr] = (a[addr] & 15) | (data & 0xf0);
            if (bm) b[addr] = sign;
        }
        check(Values{{"clk", 1}}, Values{{"y__DOT__a", addr < 3 ? a[addr] : 0}, {"y__DOT__b", addr < 3 ? b[addr] : 0}});
    }
}
template<class Check> void bundleRdwrTrace(Check check) {
    std::array<uint64_t, 3> a{}, b{};
    for (unsigned cycle = 0; cycle < 120; ++cycle) {
        const uint64_t addr = cycle % 4, av = (cycle * 13 + 7) & 255, bv = (cycle * 11 + 9) & 255;
        const bool wa = cycle % 3 == 0, wb = cycle % 5 == 0;
        const auto expected = [&] {
            const uint64_t qa = addr < 3 ? a[addr] : 0, qb = addr < 3 ? b[addr] : 0;
            return Values{{"pa", qa}, {"pb", qb}, {"qa", qa}, {"qb", qb}};
        };
        check(Values{{"clk", 0}, {"addr", addr}, {"wa", wa}, {"wb", wb}, {"a", av}, {"b", bv}}, expected());
        if (addr < 3) { if (wa) a[addr] = av; if (wb) b[addr] = bv; }
        check(Values{{"clk", 1}}, expected());
        check(Values{}, expected());
    }
}
// Independent cycle-level memory specification, not a GRH interpreter.
template<class Check> void standardTrace(int readLatency, int writeLatency, const std::string& policy, Check check) {
    std::array<uint64_t, 3> memory{};
    std::deque<Request> writes(writeLatency - 1), reads(readLatency ? readLatency - 1 : 0);
    uint32_t random = 73521;
    for (unsigned cycle = 0; cycle < 140; ++cycle) {
        random = random * 1664525u + 1013904223u;
        const uint64_t raddr = (random >> 9) & 3, waddr = (random >> 11) & 3;
        const bool wen = cycle < 130 && (random & 1), ren = (random >> 2) & 1, mask = (random >> 3) & 1;
        const uint64_t data = (random >> 16) & 255;
        Values inputs{{"clk", 0}, {"wen", wen}, {"ren", ren}, {"mask", mask}, {"raddr", raddr}, {"waddr", waddr}, {"data", data}};
        check(inputs, Values{});
        Request write{waddr, data, wen && mask && waddr < memory.size()};
        Request read{raddr, raddr < memory.size() ? memory[raddr] : 0, ren && raddr < memory.size()};
        const auto committed = writes.empty() ? write : writes.front();
        if (!writes.empty()) { writes.pop_front(); writes.push_back(write); }
        if (committed.enabled) memory[committed.address] = committed.data;
        Request completed = read;
        if (!reads.empty()) { completed = reads.front(); reads.pop_front(); reads.push_back(read); }
        uint64_t expected = 0;
        if (policy == "undefined") {
            const auto& request = readLatency ? completed : read;
            expected = request.address < memory.size() ? memory[request.address] : 0;
        } else if (!readLatency) expected = ren && raddr < memory.size() ? memory[raddr] : 0;
        else if (completed.enabled) expected = policy == "new" ? memory[completed.address] : completed.data;
        check(Values{{"clk", 1}}, Values{{"y", expected}});
        check(Values{{"clk", 1}}, Values{{"y", expected}}); // repeated eval cannot duplicate a write
    }
}
}
#endif
