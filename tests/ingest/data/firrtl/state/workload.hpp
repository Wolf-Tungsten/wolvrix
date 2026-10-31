#ifndef WOLVRIX_FIRRTL_STATE_WORKLOAD_HPP
#define WOLVRIX_FIRRTL_STATE_WORKLOAD_HPP

#include <array>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>

// Independent cycle scoreboards: no graph, parser, simulator or emitter API.
namespace firrtl_state_test {
using Values = std::map<std::string, uint64_t>;

template<class Check> void renameTrace(Check check) {
    std::array<uint64_t, 4> state{};
    bool previous = false;
    for (unsigned cycle = 0; cycle < 384; ++cycle) {
        // Explicitly alternate collisions, different addresses and OOB.
        const auto addr0 = (cycle / 4) % 8;
        const auto addr1 = cycle % 3 == 0 ? addr0 : (addr0 + 1) % 8;
        Values in{{"reset", cycle < 2 || cycle % 31 == 0}, {"initialValue", UINT64_C(0x3b291705) ^ (cycle * 0x01010101u)},
                  {"en0", cycle & 1}, {"en1", (cycle >> 1) & 1}, {"copy", (cycle >> 2) & 1},
                  {"addr0", addr0}, {"addr1", addr1}, {"data0", (cycle * 19 + 7) & 255}, {"data1", (cycle * 37 + 11) & 255}};
        for (unsigned phase = 0; phase < 4; ++phase) {
            in["clk"] = phase == 1 || phase == 2;
            const auto old = state;
            if (in.at("clk") && !previous) {
                if (in.at("reset")) for (unsigned i = 0; i < 4; ++i) state[i] = (in.at("initialValue") >> (8 * i)) & 255;
                else {
                    if (in.at("en0") && addr0 < 4) state[addr0] = in.at("data0");
                    if (in.at("en1") && addr1 < 4) state[addr1] = in.at("copy") ? (addr0 < 4 ? old[addr0] : 0) : in.at("data1");
                }
            }
            previous = in.at("clk");
            uint64_t packed = 0; for (unsigned i = 0; i < 4; ++i) packed |= state[i] << (8 * i);
            check(in, Values{{"tableOut", packed}, {"readOut", addr1 < 4 ? state[addr1] : 0}});
        }
    }
}

template<class Check> void priorityTrace(Check check) {
    uint64_t a = 0, s = 0, data = 0, flags = 0;
    bool clock = false, reset = false;
    const auto put = [](uint64_t& target, unsigned offset, unsigned width, uint64_t value) {
        const uint64_t mask = ((UINT64_C(1) << width) - 1) << offset;
        target = (target & ~mask) | ((value << offset) & mask);
    };
    for (unsigned cycle = 0; cycle < 512; ++cycle) {
        Values in{{"arst", cycle == 0 || cycle % 23 == 0}, {"srst", cycle < 2 || cycle % 29 == 0},
                  {"resetData", (cycle * 7 + 1) & 15}, {"value", (cycle * 3 + 2) & 15},
                  {"enable", cycle & 1}, {"whole", (cycle >> 1) & 1}, {"row", (cycle >> 2) & 1}, {"last", (cycle >> 3) & 1},
                  {"overwrite", cycle % 11 == 0},
                  {"i", cycle % 4}, {"j", (cycle / 4) % 4},
                  {"source__DOT__data", (cycle * 127 + 0x3010) & 65535}, {"source__DOT__flag", (cycle >> 4) & 3}};
        for (unsigned phase = 0; phase < 5; ++phase) {
            in["clk"] = phase == 2 || phase == 3;
            // Change data while reset is held, deassert reset at high clock,
            // and assert it without a clock edge. None is a second clock edge.
            if (phase == 1) in["value"] ^= 3;
            if (phase == 3 && cycle % 7 == 0) in["arst"] ^= 1;
            const bool rising = in.at("clk") && !clock;
            const auto oldData = data;
            if (rising || (in.at("arst") && !reset)) {
                if (in.at("arst")) a = 3;
                else if (in.at("enable")) a = (a + 1) & 15;
            }
            if (rising) {
                if (in.at("srst")) s = in.at("resetData");
                else if (in.at("enable")) s = (s + in.at("value")) & 15;
                if (in.at("whole")) { data = in.at("source__DOT__data"); flags = in.at("source__DOT__flag"); }
                const auto i = in.at("i"), j = in.at("j");
                if (in.at("row") && i < 2) {
                    put(data, i * 8, 8, in.at("source__DOT__data") & 255);
                    put(flags, i, 1, in.at("source__DOT__flag") & 1);
                }
                if (in.at("last")) {
                    const auto oldIndex = oldData & 15;
                    if (oldIndex < 2 && j < 2) put(data, (oldIndex * 2 + j) * 4, 4, in.at("value"));
                    if (i < 2) put(flags, i, 1, 1);
                }
                if (in.at("overwrite")) { data = in.at("source__DOT__data"); flags = in.at("source__DOT__flag"); }
            }
            clock = in.at("clk"); reset = in.at("arst");
            check(in, Values{{"asyncOut", a}, {"syncOut", s}, {"nextOut", (a + 1) & 15},
                            {"result__DOT__data", data}, {"result__DOT__flag", flags}});
        }
    }
}
}
#endif
