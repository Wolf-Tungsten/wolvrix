#include "emit/grhsim_runtime.hpp"

#include <ostream>

namespace wolvrix::lib::emit
{
    void writeGrhSimRuntime(std::ostream &output, const GrhSimRuntimeOptions &options)
    {
        auto *stream = &output;
            *stream << "#pragma once\n\n";
            *stream << "#include <algorithm>\n";
            *stream << "#include <array>\n";
            *stream << "#include <cmath>\n";
            *stream << "#include <cstddef>\n";
            *stream << "#include <cstdlib>\n";
            *stream << "#include <cstdint>\n";
            *stream << "#include <cstdarg>\n";
            *stream << "#include <cstring>\n";
            *stream << "#include <limits>\n";
            *stream << "#include <string>\n";
            *stream << "#include <utility>\n";
            *stream << "#include <vector>\n\n";
            if (options.waveform)
            {
                *stream << "#include <ctime>\n";
                *stream << "#include <string_view>\n";
                *stream << "#include \"fstapi.h\"\n\n";
                *stream << "class grhsim_fst_writer {\n";
                *stream << "public:\n";
                *stream << "    grhsim_fst_writer() = default;\n";
                *stream << "    ~grhsim_fst_writer() { close(); }\n";
                *stream << "    grhsim_fst_writer(const grhsim_fst_writer &) = delete;\n";
                *stream << "    grhsim_fst_writer &operator=(const grhsim_fst_writer &) = delete;\n";
                *stream << "    [[nodiscard]] bool open(const std::string &path, std::string_view topName)\n";
                *stream << "    {\n";
                *stream << "        close();\n";
                *stream << "        ctx_ = fstWriterCreate(path.c_str(), 1);\n";
                *stream << "        if (ctx_ == nullptr) {\n";
                *stream << "            return false;\n";
                *stream << "        }\n";
                *stream << "        fstWriterSetPackType(ctx_, FST_WR_PT_ZLIB);\n";
                *stream << "        fstWriterSetFileType(ctx_, FST_FT_VERILOG);\n";
                *stream << "        fstWriterSetTimescale(ctx_, -9);\n";
                *stream << "        fstWriterSetVersion(ctx_, \"wolvrix grhsim\");\n";
                *stream << "        std::time_t now = std::time(nullptr);\n";
                *stream << "        if (const char *date = std::ctime(&now)) {\n";
                *stream << "            fstWriterSetDate(ctx_, date);\n";
                *stream << "        }\n";
                *stream << "        const std::string scope(topName.empty() ? std::string(\"top\") : std::string(topName));\n";
                *stream << "        fstWriterSetScope(ctx_, FST_ST_VCD_MODULE, scope.c_str(), nullptr);\n";
                *stream << "        scope_open_ = true;\n";
                *stream << "        return true;\n";
                *stream << "    }\n";
                *stream << "    void close()\n";
                *stream << "    {\n";
                *stream << "        if (ctx_ == nullptr) {\n";
                *stream << "            return;\n";
                *stream << "        }\n";
                *stream << "        if (scope_open_) {\n";
                *stream << "            fstWriterSetUpscope(ctx_);\n";
                *stream << "            scope_open_ = false;\n";
                *stream << "        }\n";
                *stream << "        fstWriterClose(ctx_);\n";
                *stream << "        ctx_ = nullptr;\n";
                *stream << "    }\n";
                *stream << "    [[nodiscard]] bool is_open() const noexcept { return ctx_ != nullptr; }\n";
                *stream << "    [[nodiscard]] fstHandle register_logic(std::string_view name, std::uint32_t width)\n";
                *stream << "    {\n";
                *stream << "        return fstWriterCreateVar(ctx_, FST_VT_SV_LOGIC, FST_VD_IMPLICIT, width == 0 ? 1u : width, std::string(name).c_str(), 0);\n";
                *stream << "    }\n";
                *stream << "    [[nodiscard]] fstHandle register_real(std::string_view name)\n";
                *stream << "    {\n";
                *stream << "        return fstWriterCreateVar(ctx_, FST_VT_VCD_REAL, FST_VD_IMPLICIT, 64u, std::string(name).c_str(), 0);\n";
                *stream << "    }\n";
                *stream << "    [[nodiscard]] fstHandle register_string(std::string_view name)\n";
                *stream << "    {\n";
                *stream << "        return fstWriterCreateVar(ctx_, FST_VT_GEN_STRING, FST_VD_IMPLICIT, 0u, std::string(name).c_str(), 0);\n";
                *stream << "    }\n";
                *stream << "    void emit_time(std::uint64_t time) { fstWriterEmitTimeChange(ctx_, time); }\n";
                *stream << "    void emit_logic_u64(fstHandle handle, std::uint32_t width, std::uint64_t value)\n";
                *stream << "    {\n";
                *stream << "        fstWriterEmitValueChange64(ctx_, handle, width == 0 ? 1u : width, value);\n";
                *stream << "    }\n";
                *stream << "    void emit_logic_words(fstHandle handle, std::uint32_t width, const std::uint64_t *words)\n";
                *stream << "    {\n";
                *stream << "        fstWriterEmitValueChangeVec64(ctx_, handle, width == 0 ? 1u : width, words);\n";
                *stream << "    }\n";
                *stream << "    template <std::size_t N>\n";
                *stream << "    void emit_logic_words(fstHandle handle, std::uint32_t width, const std::array<std::uint64_t, N> &value)\n";
                *stream << "    {\n";
                *stream << "        emit_logic_words(handle, width, value.data());\n";
                *stream << "    }\n";
                *stream << "    void emit_real(fstHandle handle, double value)\n";
                *stream << "    {\n";
                *stream << "        fstWriterEmitValueChange(ctx_, handle, &value);\n";
                *stream << "    }\n";
                *stream << "    void emit_string(fstHandle handle, const std::string &value)\n";
                *stream << "    {\n";
                *stream << "        fstWriterEmitVariableLengthValueChange(ctx_, handle, value.data(), static_cast<std::uint32_t>(value.size()));\n";
                *stream << "    }\n";
                *stream << "private:\n";
                *stream << "    fstWriterContext *ctx_ = nullptr;\n";
                *stream << "    bool scope_open_ = false;\n";
                *stream << "};\n\n";
            }
            if (options.systemTasks)
            {
                *stream << "#include <fstream>\n";
                *stream << "#include <iomanip>\n";
                *stream << "#include <initializer_list>\n";
                *stream << "#include <iostream>\n";
                *stream << "#include <span>\n";
                *stream << "#include <sstream>\n";
                *stream << "#include <string_view>\n";
                *stream << "#include <unordered_map>\n\n";
            }
            *stream << "inline std::uint64_t grhsim_mask(std::size_t width)\n{\n";
            *stream << "    if (width == 0) return UINT64_C(0);\n";
            *stream << "    if (width >= 64) return ~UINT64_C(0);\n";
            *stream << "    return (UINT64_C(1) << width) - UINT64_C(1);\n";
            *stream << "}\n\n";
            *stream << "inline std::uint64_t grhsim_trunc_u64(std::uint64_t value, std::size_t width)\n{\n";
            *stream << "    return value & grhsim_mask(width);\n";
            *stream << "}\n\n";
            *stream << "inline std::int64_t grhsim_sign_extend_i64(std::uint64_t value, std::size_t width)\n{\n";
            *stream << "    if (width == 0) return 0;\n";
            *stream << "    value = grhsim_trunc_u64(value, width);\n";
            *stream << "    if (width >= 64) return static_cast<std::int64_t>(value);\n";
            *stream << "    const std::uint64_t sign = UINT64_C(1) << (width - 1u);\n";
            *stream << "    if ((value & sign) == 0) return static_cast<std::int64_t>(value);\n";
            *stream << "    return static_cast<std::int64_t>(value | ~grhsim_mask(width));\n";
            *stream << "}\n\n";
            *stream << "inline std::uint64_t grhsim_cast_u64(std::uint64_t value,\n";
            *stream << "                                   std::size_t srcWidth,\n";
            *stream << "                                   std::size_t destWidth,\n";
            *stream << "                                   bool srcSigned)\n{\n";
            *stream << "    value = grhsim_trunc_u64(value, srcWidth);\n";
            *stream << "    if (srcSigned) {\n";
            *stream << "        value = static_cast<std::uint64_t>(grhsim_sign_extend_i64(value, srcWidth));\n";
            *stream << "    }\n";
            *stream << "    return grhsim_trunc_u64(value, destWidth);\n";
            *stream << "}\n\n";
            *stream << "inline std::uint64_t grhsim_mux_u64(std::uint64_t cond, std::uint64_t trueValue, std::uint64_t falseValue)\n{\n";
            *stream << "    const std::uint64_t trueMask = static_cast<std::uint64_t>(-static_cast<std::int64_t>(cond != 0));\n";
            *stream << "    return (trueValue & trueMask) | (falseValue & ~trueMask);\n";
            *stream << "}\n\n";
            *stream << "inline int grhsim_compare_unsigned_u64(std::uint64_t lhs, std::uint64_t rhs, std::size_t width)\n{\n";
            *stream << "    lhs = grhsim_trunc_u64(lhs, width);\n";
            *stream << "    rhs = grhsim_trunc_u64(rhs, width);\n";
            *stream << "    if (lhs < rhs) return -1;\n";
            *stream << "    if (lhs > rhs) return 1;\n";
            *stream << "    return 0;\n";
            *stream << "}\n\n";
            *stream << "inline int grhsim_compare_signed_u64(std::uint64_t lhs, std::uint64_t rhs, std::size_t width)\n{\n";
            *stream << "    const std::int64_t lhsSigned = grhsim_sign_extend_i64(lhs, width);\n";
            *stream << "    const std::int64_t rhsSigned = grhsim_sign_extend_i64(rhs, width);\n";
            *stream << "    if (lhsSigned < rhsSigned) return -1;\n";
            *stream << "    if (lhsSigned > rhsSigned) return 1;\n";
            *stream << "    return 0;\n";
            *stream << "}\n\n";
            *stream << "inline std::uint64_t grhsim_concat_u64(std::uint64_t lhs, std::size_t lhsWidth, std::uint64_t rhs, std::size_t rhsWidth)\n{\n";
            *stream << "    const std::uint64_t lhsBits = grhsim_trunc_u64(lhs, lhsWidth);\n";
            *stream << "    const std::uint64_t rhsBits = grhsim_trunc_u64(rhs, rhsWidth);\n";
            *stream << "    if (rhsWidth >= 64) return rhsBits;\n";
            *stream << "    return grhsim_trunc_u64((lhsBits << rhsWidth) | rhsBits, lhsWidth + rhsWidth);\n";
            *stream << "}\n\n";
            *stream << "inline std::uint64_t grhsim_cat(std::uint64_t lhs,\n";
            *stream << "                               std::size_t lhsWidth,\n";
            *stream << "                               std::uint64_t rhs,\n";
            *stream << "                               std::size_t rhsWidth)\n{\n";
            *stream << "    return grhsim_concat_u64(lhs, lhsWidth, rhs, rhsWidth);\n";
            *stream << "}\n\n";
            *stream << "inline std::uint64_t grhsim_cat_prefix(std::uint64_t lhs,\n";
            *stream << "                                      std::size_t lhsWidth,\n";
            *stream << "                                      std::size_t rhsWidth)\n{\n";
            *stream << "    const std::uint64_t lhsBits = grhsim_trunc_u64(lhs, lhsWidth);\n";
            *stream << "    if (rhsWidth >= 64) {\n";
            *stream << "        return 0;\n";
            *stream << "    }\n";
            *stream << "    return grhsim_trunc_u64(lhsBits << rhsWidth, lhsWidth + rhsWidth);\n";
            *stream << "}\n\n";
            *stream << "inline std::uint64_t grhsim_cat_rhs(std::uint64_t prefix,\n";
            *stream << "                                   std::uint64_t rhs,\n";
            *stream << "                                   std::size_t totalWidth,\n";
            *stream << "                                   std::size_t rhsWidth)\n{\n";
            *stream << "    return grhsim_trunc_u64(prefix | grhsim_trunc_u64(rhs, rhsWidth), totalWidth);\n";
            *stream << "}\n\n";
            *stream << "inline std::uint64_t grhsim_slice_dynamic_u64(std::uint64_t value, std::uint64_t start, std::size_t width)\n{\n";
            *stream << "    if (start >= 64) return 0;\n";
            *stream << "    return grhsim_trunc_u64(value >> start, width);\n";
            *stream << "}\n\n";
            *stream << "template <std::size_t N> inline std::uint64_t grhsim_slice_words_u64(const std::array<std::uint64_t, N> &value, std::uint64_t start, std::size_t width)\n{\n";
            *stream << "    if (start >= N * 64u || width == 0) return 0;\n";
            *stream << "    const std::size_t word = static_cast<std::size_t>(start / 64u);\n";
            *stream << "    const std::size_t shift = static_cast<std::size_t>(start & 63u);\n";
            *stream << "    std::uint64_t result = value[word] >> shift;\n";
            *stream << "    if (shift != 0 && word + 1u < N) result |= value[word + 1u] << (64u - shift);\n";
            *stream << "    return grhsim_trunc_u64(result, width);\n";
            *stream << "}\n\n";
            *stream << "inline std::uint64_t grhsim_clog2_u64(std::uint64_t value, std::size_t width)\n{\n";
            *stream << "    value = grhsim_trunc_u64(value, width);\n";
            *stream << "    if (value <= 1) return 0;\n";
            *stream << "    std::uint64_t result = 0;\n";
            *stream << "    value -= 1;\n";
            *stream << "    while (value != 0) {\n";
            *stream << "        value >>= 1u;\n";
            *stream << "        ++result;\n";
            *stream << "    }\n";
            *stream << "    return result;\n";
            *stream << "}\n\n";
            *stream << "inline std::uint64_t grhsim_shl_u64(std::uint64_t value, std::uint64_t shift, std::size_t width)\n{\n";
            *stream << "    if (shift >= 64) return 0;\n";
            *stream << "    return grhsim_trunc_u64(value << shift, width);\n";
            *stream << "}\n\n";
            *stream << "inline std::uint64_t grhsim_lshr_u64(std::uint64_t value, std::uint64_t shift, std::size_t width)\n{\n";
            *stream << "    if (shift >= 64) return 0;\n";
            *stream << "    return grhsim_trunc_u64(grhsim_trunc_u64(value, width) >> shift, width);\n";
            *stream << "}\n\n";
            *stream << "inline std::uint64_t grhsim_ashr_u64(std::uint64_t value, std::uint64_t shift, std::size_t width)\n{\n";
            *stream << "    if (width == 0) return 0;\n";
            *stream << "    const std::uint64_t bounded = shift >= 64 ? 63 : shift;\n";
            *stream << "    const std::int64_t signedValue = grhsim_sign_extend_i64(value, width);\n";
            *stream << "    return grhsim_trunc_u64(static_cast<std::uint64_t>(signedValue >> bounded), width);\n";
            *stream << "}\n\n";
            *stream << "inline std::uint64_t grhsim_udiv_u64(std::uint64_t lhs, std::uint64_t rhs, std::size_t width)\n{\n";
            *stream << "    const std::uint64_t divisor = grhsim_trunc_u64(rhs, width);\n";
            *stream << "    if (divisor == 0) return 0;\n";
            *stream << "    return grhsim_trunc_u64(grhsim_trunc_u64(lhs, width) / divisor, width);\n";
            *stream << "}\n\n";
            *stream << "inline std::uint64_t grhsim_umod_u64(std::uint64_t lhs, std::uint64_t rhs, std::size_t width)\n{\n";
            *stream << "    const std::uint64_t divisor = grhsim_trunc_u64(rhs, width);\n";
            *stream << "    if (divisor == 0) return 0;\n";
            *stream << "    return grhsim_trunc_u64(grhsim_trunc_u64(lhs, width) % divisor, width);\n";
            *stream << "}\n\n";
            *stream << "inline std::uint64_t grhsim_sdiv_u64(std::uint64_t lhs, std::uint64_t rhs, std::size_t width)\n{\n";
            *stream << "    const std::int64_t divisor = grhsim_sign_extend_i64(rhs, width);\n";
            *stream << "    if (divisor == 0) return 0;\n";
            *stream << "    const std::int64_t dividend = grhsim_sign_extend_i64(lhs, width);\n";
            *stream << "    if (dividend == std::numeric_limits<std::int64_t>::min() && divisor == -1) return 0;\n";
            *stream << "    return grhsim_trunc_u64(static_cast<std::uint64_t>(dividend / divisor), width);\n";
            *stream << "}\n\n";
            *stream << "inline std::uint64_t grhsim_smod_u64(std::uint64_t lhs, std::uint64_t rhs, std::size_t width)\n{\n";
            *stream << "    const std::int64_t divisor = grhsim_sign_extend_i64(rhs, width);\n";
            *stream << "    if (divisor == 0) return 0;\n";
            *stream << "    const std::int64_t dividend = grhsim_sign_extend_i64(lhs, width);\n";
            *stream << "    if (dividend == std::numeric_limits<std::int64_t>::min() && divisor == -1) return 0;\n";
            *stream << "    return grhsim_trunc_u64(static_cast<std::uint64_t>(dividend % divisor), width);\n";
            *stream << "}\n\n";
            *stream << "inline bool grhsim_reduce_and_u64(std::uint64_t value, std::size_t width)\n{\n";
            *stream << "    if (width == 0) return false;\n";
            *stream << "    return grhsim_trunc_u64(value, width) == grhsim_mask(width);\n";
            *stream << "}\n\n";
            *stream << "inline bool grhsim_reduce_nand_u64(std::uint64_t value, std::size_t width)\n{\n";
            *stream << "    return !grhsim_reduce_and_u64(value, width);\n";
            *stream << "}\n\n";
            *stream << "inline bool grhsim_reduce_or_u64(std::uint64_t value, std::size_t width)\n{\n";
            *stream << "    return grhsim_trunc_u64(value, width) != 0;\n";
            *stream << "}\n\n";
            *stream << "inline bool grhsim_reduce_nor_u64(std::uint64_t value, std::size_t width)\n{\n";
            *stream << "    return !grhsim_reduce_or_u64(value, width);\n";
            *stream << "}\n\n";
            *stream << "inline bool grhsim_reduce_xor_u64(std::uint64_t value, std::size_t width)\n{\n";
            *stream << "    return (__builtin_popcountll(grhsim_trunc_u64(value, width)) & 1) != 0;\n";
            *stream << "}\n\n";
            *stream << "inline bool grhsim_reduce_xnor_u64(std::uint64_t value, std::size_t width)\n{\n";
            *stream << "    return !grhsim_reduce_xor_u64(value, width);\n";
            *stream << "}\n\n";
            *stream << "enum class grhsim_event_edge_kind : std::uint8_t {\n";
            *stream << "    none,\n";
            *stream << "    posedge,\n";
            *stream << "    negedge,\n";
            *stream << "};\n\n";
            *stream << "template <typename PrevT, typename CurrT>\n";
            *stream << "inline grhsim_event_edge_kind grhsim_classify_edge(PrevT prev, CurrT curr)\n{\n";
            *stream << "    const bool prevBool = prev != 0;\n";
            *stream << "    const bool currBool = curr != 0;\n";
            *stream << "    if (!prevBool && currBool) return grhsim_event_edge_kind::posedge;\n";
            *stream << "    if (prevBool && !currBool) return grhsim_event_edge_kind::negedge;\n";
            *stream << "    return grhsim_event_edge_kind::none;\n";
            *stream << "}\n\n";
            *stream << "inline bool grhsim_event_posedge(std::uint64_t curr, std::uint64_t prev)\n{\n";
            *stream << "    return curr != 0 && prev == 0;\n";
            *stream << "}\n\n";
            *stream << "inline bool grhsim_event_negedge(std::uint64_t curr, std::uint64_t prev)\n{\n";
            *stream << "    return curr == 0 && prev != 0;\n";
            *stream << "}\n\n";
            *stream << "template <std::size_t N>\n";
            *stream << "inline bool grhsim_event_posedge_words(const std::array<std::uint64_t, N> &curr,\n";
            *stream << "                                     const std::array<std::uint64_t, N> &prev,\n";
            *stream << "                                     std::size_t width)\n{\n";
            *stream << "    const std::size_t live_words = (width + 63u) / 64u;\n";
            *stream << "    bool currAny = false;\n";
            *stream << "    bool prevAny = false;\n";
            *stream << "    for (std::size_t i = 0; i < live_words; ++i) {\n";
            *stream << "        const std::size_t bits = (i + 1u == live_words) ? (width - i * 64u) : 64u;\n";
            *stream << "        const std::uint64_t mask = bits < 64u ? ((UINT64_C(1) << bits) - 1u) : ~UINT64_C(0);\n";
            *stream << "        currAny = currAny || ((curr[i] & mask) != 0);\n";
            *stream << "        prevAny = prevAny || ((prev[i] & mask) != 0);\n";
            *stream << "    }\n";
            *stream << "    return currAny && !prevAny;\n";
            *stream << "}\n\n";
            *stream << "template <std::size_t N>\n";
            *stream << "inline bool grhsim_event_negedge_words(const std::array<std::uint64_t, N> &curr,\n";
            *stream << "                                     const std::array<std::uint64_t, N> &prev,\n";
            *stream << "                                     std::size_t width)\n{\n";
            *stream << "    const std::size_t live_words = (width + 63u) / 64u;\n";
            *stream << "    bool currAny = false;\n";
            *stream << "    bool prevAny = false;\n";
            *stream << "    for (std::size_t i = 0; i < live_words; ++i) {\n";
            *stream << "        const std::size_t bits = (i + 1u == live_words) ? (width - i * 64u) : 64u;\n";
            *stream << "        const std::uint64_t mask = bits < 64u ? ((UINT64_C(1) << bits) - 1u) : ~UINT64_C(0);\n";
            *stream << "        currAny = currAny || ((curr[i] & mask) != 0);\n";
            *stream << "        prevAny = prevAny || ((prev[i] & mask) != 0);\n";
            *stream << "    }\n";
            *stream << "    return !currAny && prevAny;\n";
            *stream << "}\n\n";
            *stream << "inline std::uint64_t grhsim_splitmix64_next(std::uint64_t &state)\n{\n";
            *stream << "    std::uint64_t z = (state += UINT64_C(0x9E3779B97F4A7C15));\n";
            *stream << "    z = (z ^ (z >> 30u)) * UINT64_C(0xBF58476D1CE4E5B9);\n";
            *stream << "    z = (z ^ (z >> 27u)) * UINT64_C(0x94D049BB133111EB);\n";
            *stream << "    return z ^ (z >> 31u);\n";
            *stream << "}\n\n";
            *stream << "inline std::uint64_t grhsim_random_u64(std::uint64_t &state, std::size_t width)\n{\n";
            *stream << "    return grhsim_trunc_u64(grhsim_splitmix64_next(state), width);\n";
            *stream << "}\n\n";
            *stream << "template <std::size_t N>\n";
            *stream << "inline void grhsim_trunc_words(std::array<std::uint64_t, N> &value, std::size_t width)\n{\n";
            *stream << "    const std::size_t liveWords = (width + 63u) / 64u;\n";
            *stream << "    for (std::size_t i = liveWords; i < N; ++i) {\n";
            *stream << "        value[i] = 0;\n";
            *stream << "    }\n";
            *stream << "    if constexpr (N > 0) {\n";
            *stream << "        if (liveWords != 0) {\n";
            *stream << "            const std::size_t tailWidth = width - ((liveWords - 1u) * 64u);\n";
            *stream << "            value[liveWords - 1u] = grhsim_trunc_u64(value[liveWords - 1u], tailWidth);\n";
            *stream << "        }\n";
            *stream << "    }\n";
            *stream << "}\n\n";
            *stream << "template <std::size_t Bits>\n";
            *stream << "inline constexpr std::uint64_t grhsim_const_mask()\n{\n";
            *stream << "    if constexpr (Bits == 0) {\n";
            *stream << "        return UINT64_C(0);\n";
            *stream << "    }\n";
            *stream << "    else if constexpr (Bits >= 64) {\n";
            *stream << "        return ~UINT64_C(0);\n";
            *stream << "    }\n";
            *stream << "    else {\n";
            *stream << "        return (UINT64_C(1) << Bits) - UINT64_C(1);\n";
            *stream << "    }\n";
            *stream << "}\n\n";
            *stream << "template <std::size_t Width>\n";
            *stream << "inline std::array<std::uint64_t, 2> grhsim_trunc_words_2(std::array<std::uint64_t, 2> value)\n{\n";
            *stream << "    static_assert(Width > 64 && Width <= 128);\n";
            *stream << "    if constexpr (Width < 128) {\n";
            *stream << "        value[1] &= grhsim_const_mask<Width - 64u>();\n";
            *stream << "    }\n";
            *stream << "    return value;\n";
            *stream << "}\n\n";
            *stream << "template <std::size_t Width>\n";
            *stream << "inline bool grhsim_assign_words_2(std::array<std::uint64_t, 2> &dst,\n";
            *stream << "                                  const std::array<std::uint64_t, 2> &src)\n{\n";
            *stream << "    static_assert(Width > 64 && Width <= 128);\n";
            *stream << "    const std::uint64_t next0 = src[0];\n";
            *stream << "    const std::uint64_t next1 = src[1] & grhsim_const_mask<Width - 64u>();\n";
            *stream << "    const bool changed = (dst[0] != next0) | (dst[1] != next1);\n";
            *stream << "    dst[0] = next0;\n";
            *stream << "    dst[1] = next1;\n";
            *stream << "    return changed;\n";
            *stream << "}\n\n";
            *stream << "template <std::size_t N>\n";
            *stream << "inline bool grhsim_equal_words(const std::array<std::uint64_t, N> &lhs, const std::array<std::uint64_t, N> &rhs)\n{\n";
            *stream << "    return lhs == rhs;\n";
            *stream << "}\n\n";
            *stream << "template <std::size_t N>\n";
            *stream << "inline bool grhsim_assign_words(std::array<std::uint64_t, N> &dst,\n";
            *stream << "                               const std::array<std::uint64_t, N> &src,\n";
            *stream << "                               std::size_t width)\n{\n";
            *stream << "    bool changed = false;\n";
            *stream << "    const std::size_t liveWords = (width + 63u) / 64u;\n";
            *stream << "    for (std::size_t i = 0; i < liveWords && i < N; ++i) {\n";
            *stream << "        const std::size_t wordWidth = (i + 1u == liveWords) ? (width - i * 64u) : 64u;\n";
            *stream << "        const std::uint64_t next = grhsim_trunc_u64(src[i], wordWidth);\n";
            *stream << "        changed = changed || (dst[i] != next);\n";
            *stream << "        dst[i] = next;\n";
            *stream << "    }\n";
            *stream << "    for (std::size_t i = liveWords; i < N; ++i) {\n";
            *stream << "        changed = changed || (dst[i] != 0);\n";
            *stream << "        dst[i] = 0;\n";
            *stream << "    }\n";
            *stream << "    return changed;\n";
            *stream << "}\n\n";
            *stream << "#ifndef GRHSIM_ALWAYS_INLINE\n";
            *stream << "#if defined(__GNUC__) || defined(__clang__)\n";
            *stream << "#define GRHSIM_ALWAYS_INLINE inline __attribute__((always_inline))\n";
            *stream << "#else\n";
            *stream << "#define GRHSIM_ALWAYS_INLINE inline\n";
            *stream << "#endif\n";
            *stream << "#endif\n\n";
            if (options.oneBitBitwiseBytes)
            {
                *stream << "GRHSIM_ALWAYS_INLINE std::uint8_t grhsim_assume_bit_u8(std::uint8_t value)\n{\n";
                *stream << "#if defined(__clang__)\n";
                *stream << "    __builtin_assume(value <= UINT8_C(1));\n";
                *stream << "#elif defined(__GNUC__)\n";
                *stream << "    if (value > UINT8_C(1)) {\n";
                *stream << "        __builtin_unreachable();\n";
                *stream << "    }\n";
                *stream << "#endif\n";
                *stream << "    return value;\n";
                *stream << "}\n\n";
            }
            *stream << "template <std::size_t N>\n";
            *stream << "GRHSIM_ALWAYS_INLINE bool grhsim_assign_words_full(std::array<std::uint64_t, N> &dst,\n";
            *stream << "                                                  const std::array<std::uint64_t, N> &src)\n{\n";
            *stream << "    bool changed = false;\n";
            *stream << "    for (std::size_t i = 0; i < N; ++i) {\n";
            *stream << "        const std::uint64_t next = src[i];\n";
            *stream << "        changed = static_cast<bool>(changed | (dst[i] != next));\n";
            *stream << "        dst[i] = next;\n";
            *stream << "    }\n";
            *stream << "    return changed;\n";
            *stream << "}\n\n";
            *stream << "template <typename T, std::size_t N>\n";
            *stream << "inline T &grhsim_value_storage_ref(std::array<std::byte, N> &storage, std::size_t offset)\n{\n";
            *stream << "    return *reinterpret_cast<T *>(storage.data() + offset);\n";
            *stream << "}\n\n";
            *stream << "template <std::size_t N>\n";
            *stream << "inline std::uint8_t &grhsim_value_u8_ref(std::array<std::byte, N> &storage, std::size_t offset)\n{\n";
            *stream << "    return grhsim_value_storage_ref<std::uint8_t>(storage, offset);\n";
            *stream << "}\n\n";
            *stream << "template <std::size_t N>\n";
            *stream << "inline std::uint16_t &grhsim_value_u16_ref(std::array<std::byte, N> &storage, std::size_t offset)\n{\n";
            *stream << "    return grhsim_value_storage_ref<std::uint16_t>(storage, offset);\n";
            *stream << "}\n\n";
            *stream << "template <std::size_t N>\n";
            *stream << "inline std::uint32_t &grhsim_value_u32_ref(std::array<std::byte, N> &storage, std::size_t offset)\n{\n";
            *stream << "    return grhsim_value_storage_ref<std::uint32_t>(storage, offset);\n";
            *stream << "}\n\n";
            *stream << "template <std::size_t N>\n";
            *stream << "inline std::uint64_t &grhsim_value_u64_ref(std::array<std::byte, N> &storage, std::size_t offset)\n{\n";
            *stream << "    return grhsim_value_storage_ref<std::uint64_t>(storage, offset);\n";
            *stream << "}\n\n";
            *stream << "template <std::size_t Words, std::size_t N>\n";
            *stream << "inline std::array<std::uint64_t, Words> &grhsim_value_words_ref(std::array<std::byte, N> &storage,\n";
            *stream << "                                                                   std::size_t offset)\n{\n";
            *stream << "    return grhsim_value_storage_ref<std::array<std::uint64_t, Words>>(storage, offset);\n";
            *stream << "}\n\n";
            *stream << "template <std::size_t N>\n";
            *stream << "inline std::array<std::uint64_t, N> grhsim_merge_words_masked(const std::array<std::uint64_t, N> &base,\n";
            *stream << "                                                           const std::array<std::uint64_t, N> &data,\n";
            *stream << "                                                           const std::array<std::uint64_t, N> &mask,\n";
            *stream << "                                                           std::size_t width)\n{\n";
            *stream << "    std::array<std::uint64_t, N> out{};\n";
            *stream << "    for (std::size_t i = 0; i < N; ++i) {\n";
            *stream << "        out[i] = (base[i] & ~mask[i]) | (data[i] & mask[i]);\n";
            *stream << "    }\n";
            *stream << "    grhsim_trunc_words(out, width);\n";
            *stream << "    return out;\n";
            *stream << "}\n\n";
            *stream << "template <std::size_t N>\n";
            *stream << "inline bool grhsim_apply_masked_words_inplace(std::array<std::uint64_t, N> &dst,\n";
            *stream << "                                             const std::array<std::uint64_t, N> &data,\n";
            *stream << "                                             const std::array<std::uint64_t, N> &mask,\n";
            *stream << "                                             std::size_t width)\n{\n";
            *stream << "    bool changed = false;\n";
            *stream << "    const std::size_t liveWords = (width + 63u) / 64u;\n";
            *stream << "    for (std::size_t i = 0; i < liveWords && i < N; ++i) {\n";
            *stream << "        const std::size_t wordWidth = (i + 1u == liveWords) ? (width - i * 64u) : 64u;\n";
            *stream << "        const std::uint64_t wordMask = grhsim_trunc_u64(mask[i], wordWidth);\n";
            *stream << "        const std::uint64_t next = (dst[i] & ~wordMask) | (grhsim_trunc_u64(data[i], wordWidth) & wordMask);\n";
            *stream << "        changed = changed || (dst[i] != next);\n";
            *stream << "        dst[i] = next;\n";
            *stream << "    }\n";
            *stream << "    for (std::size_t i = liveWords; i < N; ++i) {\n";
            *stream << "        changed = changed || (dst[i] != 0);\n";
            *stream << "        dst[i] = 0;\n";
            *stream << "    }\n";
            *stream << "    return changed;\n";
            *stream << "}\n\n";
            *stream << R"CPP(
inline void grhsim_trunc_words_buffer(std::uint64_t *value,
                                      std::size_t wordCount,
                                      std::size_t width)
{
    const std::size_t liveWords = (width + 63u) / 64u;
    const std::size_t keptWords = liveWords < wordCount ? liveWords : wordCount;
    for (std::size_t i = keptWords; i < wordCount; ++i) {
        value[i] = 0;
    }
    if (keptWords == 0) {
        return;
    }
    const std::size_t lastBits = width & 63u;
    if (lastBits != 0) {
        value[keptWords - 1u] &= grhsim_mask(lastBits);
    }
}

inline void grhsim_clear_range_words_buffer(std::uint64_t *value,
                                            std::size_t wordCount,
                                            std::size_t start,
                                            std::size_t width)
{
    if (width == 0 || wordCount == 0) {
        return;
    }
    const std::size_t totalBits = wordCount * 64u;
    if (start >= totalBits) {
        return;
    }
    const std::size_t end = std::min(totalBits, start + width);
    if (end <= start) {
        return;
    }
    const std::size_t startWord = start / 64u;
    const std::size_t endWord = (end - 1u) / 64u;
    const std::size_t startBit = start & 63u;
    const std::size_t endBits = ((end - 1u) & 63u) + 1u;
    if (startWord == endWord) {
        const std::uint64_t lowMask = startBit == 0 ? UINT64_C(0) : grhsim_mask(startBit);
        const std::uint64_t highMask = endBits >= 64 ? UINT64_C(0) : ~grhsim_mask(endBits);
        value[startWord] &= (lowMask | highMask);
        return;
    }
    value[startWord] &= (startBit == 0 ? UINT64_C(0) : grhsim_mask(startBit));
    for (std::size_t i = startWord + 1u; i < endWord && i < wordCount; ++i) {
        value[i] = 0;
    }
    if (endWord < wordCount) {
        value[endWord] &= (endBits >= 64 ? UINT64_C(0) : ~grhsim_mask(endBits));
    }
}

inline void grhsim_insert_scalar_words_buffer(std::uint64_t *dest,
                                              std::size_t destWords,
                                              std::size_t destLsb,
                                              std::uint64_t src,
                                              std::size_t srcWidth)
{
    if (srcWidth == 0 || destWords == 0) {
        return;
    }
    grhsim_clear_range_words_buffer(dest, destWords, destLsb, srcWidth);
    const std::size_t destWord = destLsb / 64u;
    if (destWord >= destWords) {
        return;
    }
    const std::size_t bitShift = destLsb & 63u;
    const std::uint64_t bits = grhsim_trunc_u64(src, srcWidth);
    dest[destWord] |= (bitShift == 0 ? bits : (bits << bitShift));
    if (bitShift != 0 && destWord + 1u < destWords && srcWidth + bitShift > 64u) {
        dest[destWord + 1u] |= (bits >> (64u - bitShift));
    }
}

inline void grhsim_cast_scalar_words(std::uint64_t value,
                                     std::size_t srcWidth,
                                     std::size_t destWidth,
                                     bool srcSigned,
                                     std::uint64_t *out,
                                     std::size_t destWords)
{
    std::fill_n(out, destWords, UINT64_C(0));
    if (destWords > 0) {
        out[0] = grhsim_trunc_u64(value, srcWidth);
        if (srcSigned && srcWidth != 0 &&
            ((out[0] >> ((srcWidth >= 64u ? 63u : srcWidth - 1u))) & UINT64_C(1)) != 0) {
            if (srcWidth < 64u) {
                out[0] |= ~grhsim_mask(srcWidth);
            }
            for (std::size_t i = 1; i < destWords; ++i) {
                out[i] = ~UINT64_C(0);
            }
        }
    }
    grhsim_trunc_words_buffer(out, destWords, destWidth);
}

inline void grhsim_cast_words(const std::uint64_t *value,
                              std::size_t srcWords,
                              std::size_t srcWidth,
                              std::size_t destWidth,
                              bool srcSigned,
                              std::uint64_t *out,
                              std::size_t destWords)
{
    std::fill_n(out, destWords, UINT64_C(0));
    const std::size_t liveSrcWords = (srcWidth + 63u) / 64u;
    const std::size_t limit = std::min(srcWords, std::min(liveSrcWords, destWords));
    for (std::size_t i = 0; i < limit; ++i) {
        out[i] = value[i];
    }
    if (srcSigned && srcWidth != 0) {
        const std::size_t signWord = (srcWidth - 1u) / 64u;
        const std::size_t signBit = (srcWidth - 1u) & 63u;
        const bool neg = signWord < destWords && ((out[signWord] >> signBit) & UINT64_C(1)) != 0;
        if (neg) {
            if (signWord < destWords && signBit != 63u) {
                out[signWord] |= (~UINT64_C(0)) << (signBit + 1u);
            }
            for (std::size_t i = signWord + 1u; i < destWords; ++i) {
                out[i] = ~UINT64_C(0);
            }
        }
    }
    grhsim_trunc_words_buffer(out, destWords, destWidth);
}

inline void grhsim_concat_scalars_words(const std::uint64_t *values,
                                        const std::size_t *widths,
                                        std::size_t count,
                                        std::size_t totalWidth,
                                        std::uint64_t *out,
                                        std::size_t destWords)
{
    std::fill_n(out, destWords, UINT64_C(0));
    std::size_t cursor = totalWidth;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t width = widths[i];
        if (width == 0) {
            continue;
        }
        if (width > cursor) {
            cursor = 0;
            continue;
        }
        cursor -= width;
        grhsim_insert_scalar_words_buffer(out, destWords, cursor, values[i], width);
    }
    grhsim_trunc_words_buffer(out, destWords, totalWidth);
}

inline void grhsim_concat_uniform_scalars_words(const std::uint64_t *values,
                                                std::size_t count,
                                                std::size_t elemWidth,
                                                std::size_t totalWidth,
                                                std::uint64_t *out,
                                                std::size_t destWords)
{
    std::fill_n(out, destWords, UINT64_C(0));
    std::size_t cursor = totalWidth;
    for (std::size_t i = 0; i < count; ++i) {
        if (elemWidth == 0) {
            continue;
        }
        if (elemWidth > cursor) {
            cursor = 0;
            continue;
        }
        cursor -= elemWidth;
        grhsim_insert_scalar_words_buffer(out, destWords, cursor, values[i], elemWidth);
    }
    grhsim_trunc_words_buffer(out, destWords, totalWidth);
}

inline void grhsim_slice_words(const std::uint64_t *src,
                               std::size_t srcWords,
                               std::size_t start,
                               std::size_t width,
                               std::uint64_t *out,
                               std::size_t destWords)
{
    std::fill_n(out, destWords, UINT64_C(0));
    if (width == 0 || destWords == 0 || srcWords == 0) {
        return;
    }
    const std::size_t srcWord = start / 64u;
    const std::size_t bitShift = start & 63u;
    const std::size_t outWords = (width + 63u) / 64u;
    for (std::size_t i = 0; i < outWords && i < destWords; ++i) {
        const std::uint64_t low = (srcWord + i < srcWords) ? src[srcWord + i] : UINT64_C(0);
        if (bitShift == 0) {
            out[i] = low;
        }
        else {
            const std::uint64_t high = (srcWord + i + 1u < srcWords) ? src[srcWord + i + 1u] : UINT64_C(0);
            out[i] = (low >> bitShift) | (high << (64u - bitShift));
        }
    }
    grhsim_trunc_words_buffer(out, destWords, width);
}

inline bool grhsim_any_bits_words(const std::uint64_t *value,
                                  std::size_t wordCount,
                                  std::size_t width)
{
    const std::size_t liveWords = (width + 63u) / 64u;
    for (std::size_t i = 0; i < liveWords && i < wordCount; ++i) {
        const std::uint64_t word = (i + 1u == liveWords) ? grhsim_trunc_u64(value[i], width - i * 64u) : value[i];
        if (word != 0) {
            return true;
        }
    }
    return false;
}

inline bool grhsim_sign_bit_words(const std::uint64_t *value,
                                  std::size_t wordCount,
                                  std::size_t width)
{
    if (width == 0) {
        return false;
    }
    const std::size_t index = width - 1u;
    const std::size_t wordIndex = index / 64u;
    if (wordIndex >= wordCount) {
        return false;
    }
    return ((value[wordIndex] >> (index & 63u)) & UINT64_C(1)) != 0;
}

inline int grhsim_compare_unsigned_words(const std::uint64_t *lhs,
                                         std::size_t lhsWords,
                                         const std::uint64_t *rhs,
                                         std::size_t rhsWords)
{
    const std::size_t words = std::max(lhsWords, rhsWords);
    for (std::size_t i = words; i-- > 0;) {
        const std::uint64_t lhsWord = i < lhsWords ? lhs[i] : UINT64_C(0);
        const std::uint64_t rhsWord = i < rhsWords ? rhs[i] : UINT64_C(0);
        if (lhsWord < rhsWord) {
            return -1;
        }
        if (lhsWord > rhsWord) {
            return 1;
        }
    }
    return 0;
}

inline int grhsim_compare_signed_words(const std::uint64_t *lhs,
                                       std::size_t lhsWords,
                                       const std::uint64_t *rhs,
                                       std::size_t rhsWords,
                                       std::size_t width)
{
    const bool lhsNeg = grhsim_sign_bit_words(lhs, lhsWords, width);
    const bool rhsNeg = grhsim_sign_bit_words(rhs, rhsWords, width);
    if (lhsNeg != rhsNeg) {
        return lhsNeg ? -1 : 1;
    }
    return grhsim_compare_unsigned_words(lhs, lhsWords, rhs, rhsWords);
}

inline int grhsim_compare_extended_words(const std::uint64_t *lhs,
                                          std::size_t lhsWords,
                                          std::size_t lhsWidth,
                                          const std::uint64_t *rhs,
                                          std::size_t rhsWords,
                                          std::size_t rhsWidth,
                                          bool signedMode)
{
    const bool lhsNeg = signedMode && grhsim_sign_bit_words(lhs, lhsWords, lhsWidth);
    const bool rhsNeg = signedMode && grhsim_sign_bit_words(rhs, rhsWords, rhsWidth);
    if (lhsNeg != rhsNeg) return lhsNeg ? -1 : 1;
    // Extend one word at a time, masking padding without materializing wide casts.
    const auto word = [](const std::uint64_t *data, std::size_t count, std::size_t width,
                         bool negative, std::size_t index) {
        const std::uint64_t fill = negative ? ~UINT64_C(0) : UINT64_C(0);
        const std::size_t begin = index * 64u;
        if (begin >= width) return fill;
        const auto mask = grhsim_mask(std::min<std::size_t>(64u, width - begin));
        return ((index < count ? data[index] : UINT64_C(0)) & mask) | (fill & ~mask);
    };
    for (std::size_t i = (std::max(lhsWidth, rhsWidth) + 63u) / 64u; i-- > 0;) {
        const auto a = word(lhs, lhsWords, lhsWidth, lhsNeg, i);
        const auto b = word(rhs, rhsWords, rhsWidth, rhsNeg, i);
        if (a != b) return a < b ? -1 : 1;
    }
    return 0;
}

inline bool grhsim_reduce_and_words(const std::uint64_t *value,
                                    std::size_t wordCount,
                                    std::size_t width)
{
    if (width == 0) {
        return false;
    }
    const std::size_t liveWords = (width + 63u) / 64u;
    for (std::size_t i = 0; i < liveWords && i < wordCount; ++i) {
        const std::size_t wordWidth = (i + 1u == liveWords) ? (width - i * 64u) : 64u;
        if (grhsim_trunc_u64(value[i], wordWidth) != grhsim_mask(wordWidth)) {
            return false;
        }
    }
    return true;
}

inline bool grhsim_reduce_nand_words(const std::uint64_t *value,
                                     std::size_t wordCount,
                                     std::size_t width)
{
    return !grhsim_reduce_and_words(value, wordCount, width);
}

inline bool grhsim_reduce_or_words(const std::uint64_t *value,
                                   std::size_t wordCount,
                                   std::size_t width)
{
    return grhsim_any_bits_words(value, wordCount, width);
}

inline bool grhsim_reduce_nor_words(const std::uint64_t *value,
                                    std::size_t wordCount,
                                    std::size_t width)
{
    return !grhsim_reduce_or_words(value, wordCount, width);
}

inline bool grhsim_reduce_xor_words(const std::uint64_t *value,
                                    std::size_t wordCount,
                                    std::size_t width)
{
    unsigned parity = 0;
    const std::size_t liveWords = (width + 63u) / 64u;
    for (std::size_t i = 0; i < liveWords && i < wordCount; ++i) {
        const std::size_t wordWidth = (i + 1u == liveWords) ? (width - i * 64u) : 64u;
        parity ^= static_cast<unsigned>(__builtin_popcountll(grhsim_trunc_u64(value[i], wordWidth)) & 1u);
    }
    return (parity & 1u) != 0;
}

inline bool grhsim_reduce_xnor_words(const std::uint64_t *value,
                                     std::size_t wordCount,
                                     std::size_t width)
{
    return !grhsim_reduce_xor_words(value, wordCount, width);
}

inline void grhsim_not_words(const std::uint64_t *value,
                             std::size_t valueWords,
                             std::size_t width,
                             std::uint64_t *out,
                             std::size_t outWords)
{
    for (std::size_t i = 0; i < outWords; ++i) {
        out[i] = ~(i < valueWords ? value[i] : UINT64_C(0));
    }
    grhsim_trunc_words_buffer(out, outWords, width);
}

inline void grhsim_and_words(const std::uint64_t *lhs,
                             std::size_t lhsWords,
                             const std::uint64_t *rhs,
                             std::size_t rhsWords,
                             std::size_t width,
                             std::uint64_t *out,
                             std::size_t outWords)
{
    for (std::size_t i = 0; i < outWords; ++i) {
        const std::uint64_t lhsWord = i < lhsWords ? lhs[i] : UINT64_C(0);
        const std::uint64_t rhsWord = i < rhsWords ? rhs[i] : UINT64_C(0);
        out[i] = lhsWord & rhsWord;
    }
    grhsim_trunc_words_buffer(out, outWords, width);
}

inline void grhsim_or_words(const std::uint64_t *lhs,
                            std::size_t lhsWords,
                            const std::uint64_t *rhs,
                            std::size_t rhsWords,
                            std::size_t width,
                            std::uint64_t *out,
                            std::size_t outWords)
{
    for (std::size_t i = 0; i < outWords; ++i) {
        const std::uint64_t lhsWord = i < lhsWords ? lhs[i] : UINT64_C(0);
        const std::uint64_t rhsWord = i < rhsWords ? rhs[i] : UINT64_C(0);
        out[i] = lhsWord | rhsWord;
    }
    grhsim_trunc_words_buffer(out, outWords, width);
}

inline void grhsim_xor_words(const std::uint64_t *lhs,
                             std::size_t lhsWords,
                             const std::uint64_t *rhs,
                             std::size_t rhsWords,
                             std::size_t width,
                             std::uint64_t *out,
                             std::size_t outWords)
{
    for (std::size_t i = 0; i < outWords; ++i) {
        const std::uint64_t lhsWord = i < lhsWords ? lhs[i] : UINT64_C(0);
        const std::uint64_t rhsWord = i < rhsWords ? rhs[i] : UINT64_C(0);
        out[i] = lhsWord ^ rhsWord;
    }
    grhsim_trunc_words_buffer(out, outWords, width);
}

inline void grhsim_xnor_words(const std::uint64_t *lhs,
                              std::size_t lhsWords,
                              const std::uint64_t *rhs,
                              std::size_t rhsWords,
                              std::size_t width,
                              std::uint64_t *out,
                              std::size_t outWords)
{
    grhsim_xor_words(lhs, lhsWords, rhs, rhsWords, width, out, outWords);
    for (std::size_t i = 0; i < outWords; ++i) {
        out[i] = ~out[i];
    }
    grhsim_trunc_words_buffer(out, outWords, width);
}

inline void grhsim_add_words(const std::uint64_t *lhs,
                             std::size_t lhsWords,
                             const std::uint64_t *rhs,
                             std::size_t rhsWords,
                             std::size_t width,
                             std::uint64_t *out,
                             std::size_t outWords)
{
    unsigned __int128 carry = 0;
    for (std::size_t i = 0; i < outWords; ++i) {
        const unsigned __int128 lhsWord = i < lhsWords ? lhs[i] : UINT64_C(0);
        const unsigned __int128 rhsWord = i < rhsWords ? rhs[i] : UINT64_C(0);
        const unsigned __int128 sum = lhsWord + rhsWord + carry;
        out[i] = static_cast<std::uint64_t>(sum);
        carry = sum >> 64u;
    }
    grhsim_trunc_words_buffer(out, outWords, width);
}

inline void grhsim_sub_words(const std::uint64_t *lhs,
                             std::size_t lhsWords,
                             const std::uint64_t *rhs,
                             std::size_t rhsWords,
                             std::size_t width,
                             std::uint64_t *out,
                             std::size_t outWords)
{
    std::uint64_t borrow = 0;
    for (std::size_t i = 0; i < outWords; ++i) {
        const std::uint64_t lhsWord = i < lhsWords ? lhs[i] : UINT64_C(0);
        const std::uint64_t rhsBase = i < rhsWords ? rhs[i] : UINT64_C(0);
        const std::uint64_t rhsWord = rhsBase + borrow;
        borrow = (rhsWord < rhsBase || lhsWord < rhsWord) ? 1 : 0;
        out[i] = lhsWord - rhsWord;
    }
    grhsim_trunc_words_buffer(out, outWords, width);
}

inline void grhsim_fill_range_words_buffer(std::uint64_t *value,
                                           std::size_t wordCount,
                                           std::size_t start,
                                           std::size_t width)
{
    if (width == 0 || wordCount == 0) {
        return;
    }
    const std::size_t totalBits = wordCount * 64u;
    if (start >= totalBits) {
        return;
    }
    const std::size_t end = std::min(totalBits, start + width);
    if (end <= start) {
        return;
    }
    const std::size_t startWord = start / 64u;
    const std::size_t endWord = (end - 1u) / 64u;
    const std::size_t startBit = start & 63u;
    const std::size_t endBits = ((end - 1u) & 63u) + 1u;
    if (startWord == endWord) {
        const std::uint64_t lowMask = startBit == 0 ? ~UINT64_C(0) : ~grhsim_mask(startBit);
        const std::uint64_t highMask = endBits >= 64 ? ~UINT64_C(0) : grhsim_mask(endBits);
        value[startWord] |= (lowMask & highMask);
        return;
    }
    value[startWord] |= (startBit == 0 ? ~UINT64_C(0) : ~grhsim_mask(startBit));
    for (std::size_t i = startWord + 1u; i < endWord && i < wordCount; ++i) {
        value[i] = ~UINT64_C(0);
    }
    if (endWord < wordCount) {
        value[endWord] |= (endBits >= 64 ? ~UINT64_C(0) : grhsim_mask(endBits));
    }
}

inline void grhsim_shl_words(const std::uint64_t *value,
                             std::size_t valueWords,
                             std::size_t amount,
                             std::size_t width,
                             std::uint64_t *out,
                             std::size_t outWords)
{
    std::fill_n(out, outWords, UINT64_C(0));
    if (amount >= width) {
        return;
    }
    const std::size_t wordShift = amount / 64u;
    const std::size_t bitShift = amount & 63u;
    for (std::size_t i = outWords; i-- > 0;) {
        if (i < wordShift) {
            continue;
        }
        const std::size_t srcIndex = i - wordShift;
        const std::uint64_t low = srcIndex < valueWords ? value[srcIndex] : UINT64_C(0);
        out[i] = (bitShift == 0 ? low : (low << bitShift));
        if (bitShift != 0 && srcIndex > 0 && srcIndex - 1u < valueWords) {
            out[i] |= (value[srcIndex - 1u] >> (64u - bitShift));
        }
    }
    grhsim_trunc_words_buffer(out, outWords, width);
}

inline void grhsim_lshr_words(const std::uint64_t *value,
                              std::size_t valueWords,
                              std::size_t amount,
                              std::size_t width,
                              std::uint64_t *out,
                              std::size_t outWords)
{
    std::fill_n(out, outWords, UINT64_C(0));
    if (amount >= width) {
        return;
    }
    const std::size_t wordShift = amount / 64u;
    const std::size_t bitShift = amount & 63u;
    for (std::size_t i = 0; i < outWords; ++i) {
        if (i + wordShift >= valueWords) {
            break;
        }
        const std::uint64_t high = value[i + wordShift];
        out[i] = (bitShift == 0 ? high : (high >> bitShift));
        if (bitShift != 0 && i + wordShift + 1u < valueWords) {
            out[i] |= (value[i + wordShift + 1u] << (64u - bitShift));
        }
    }
    grhsim_trunc_words_buffer(out, outWords, width);
}

inline void grhsim_ashr_words(const std::uint64_t *value,
                              std::size_t valueWords,
                              std::size_t amount,
                              std::size_t width,
                              std::uint64_t *out,
                              std::size_t outWords)
{
    const bool sign = grhsim_sign_bit_words(value, valueWords, width);
    if (amount >= width) {
        std::fill_n(out, outWords, UINT64_C(0));
        if (sign) {
            grhsim_fill_range_words_buffer(out, outWords, 0, width);
        }
        grhsim_trunc_words_buffer(out, outWords, width);
        return;
    }
    grhsim_lshr_words(value, valueWords, amount, width, out, outWords);
    if (sign) {
        grhsim_fill_range_words_buffer(out, outWords, width - amount, amount);
    }
    grhsim_trunc_words_buffer(out, outWords, width);
}

template <std::size_t DestN, typename T>
inline std::array<std::uint64_t, DestN> grhsim_cast_words(T value,
                                                          std::size_t srcWidth,
                                                          std::size_t destWidth,
                                                          bool srcSigned)
{
    std::array<std::uint64_t, DestN> out{};
    if constexpr (DestN > 0) {
        out[0] = grhsim_trunc_u64(static_cast<std::uint64_t>(value), srcWidth);
        if (srcSigned && srcWidth != 0 &&
            ((out[0] >> ((srcWidth >= 64u ? 63u : srcWidth - 1u))) & UINT64_C(1)) != 0) {
            if (srcWidth < 64u) {
                out[0] |= ~grhsim_mask(srcWidth);
            }
            for (std::size_t i = 1; i < DestN; ++i) {
                out[i] = ~UINT64_C(0);
            }
        }
    }
    grhsim_trunc_words(out, destWidth);
    return out;
}

template <std::size_t DestN, std::size_t SrcN>
inline std::array<std::uint64_t, DestN> grhsim_cast_words(const std::array<std::uint64_t, SrcN> &value,
                                                          std::size_t srcWidth,
                                                          std::size_t destWidth,
                                                          bool srcSigned)
{
    std::array<std::uint64_t, DestN> out{};
    const std::size_t srcWords = (srcWidth + 63u) / 64u;
    const std::size_t limit = srcWords < SrcN ? srcWords : SrcN;
    for (std::size_t i = 0; i < limit && i < DestN; ++i) {
        out[i] = value[i];
    }
    if (srcSigned && srcWidth != 0) {
        const std::size_t signWord = (srcWidth - 1u) / 64u;
        const std::size_t signBit = (srcWidth - 1u) & 63u;
        const bool neg = signWord < DestN && ((out[signWord] >> signBit) & UINT64_C(1)) != 0;
        if (neg) {
            if (signWord < DestN && signBit != 63u) {
                out[signWord] |= (~UINT64_C(0)) << (signBit + 1u);
            }
            for (std::size_t i = signWord + 1u; i < DestN; ++i) {
                out[i] = ~UINT64_C(0);
            }
        }
    }
    grhsim_trunc_words(out, destWidth);
    return out;
}

template <std::size_t N>
inline std::array<std::uint64_t, N> grhsim_random_words(std::uint64_t &state, std::size_t width)
{
    std::array<std::uint64_t, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        out[i] = grhsim_splitmix64_next(state);
    }
    grhsim_trunc_words(out, width);
    return out;
}

template <std::size_t N, typename ShiftT>
inline std::array<std::uint64_t, N> grhsim_shl_words(const std::array<std::uint64_t, N> &value,
                                                     const ShiftT &shift,
                                                     std::size_t width);

template <std::size_t N, typename ShiftT>
inline std::array<std::uint64_t, N> grhsim_lshr_words(const std::array<std::uint64_t, N> &value,
                                                      const ShiftT &shift,
                                                      std::size_t width);

template <std::size_t N>
inline bool grhsim_try_u128_words(const std::array<std::uint64_t, N> &value,
                                  std::size_t width,
                                  unsigned __int128 &out);

template <std::size_t N>
inline std::array<std::uint64_t, N> grhsim_from_u128_words(unsigned __int128 value, std::size_t width);

template <std::size_t N>
inline bool grhsim_any_bits_words(const std::array<std::uint64_t, N> &value, std::size_t width)
{
    const std::size_t liveWords = (width + 63u) / 64u;
    for (std::size_t i = 0; i < liveWords && i < N; ++i) {
        const std::uint64_t word = (i + 1u == liveWords) ? grhsim_trunc_u64(value[i], width - i * 64u) : value[i];
        if (word != 0) {
            return true;
        }
    }
    return false;
}

template <std::size_t N>
inline bool grhsim_get_bit_words(const std::array<std::uint64_t, N> &value, std::size_t index)
{
    if (index / 64u >= N) {
        return false;
    }
    return (value[index / 64u] & (UINT64_C(1) << (index & 63u))) != 0;
}

template <std::size_t N>
inline void grhsim_put_bit_words(std::array<std::uint64_t, N> &value, std::size_t index, bool bit)
{
    if (index / 64u >= N) {
        return;
    }
    const std::uint64_t mask = UINT64_C(1) << (index & 63u);
    if (bit) {
        value[index / 64u] |= mask;
    }
    else {
        value[index / 64u] &= ~mask;
    }
}

template <std::size_t N>
inline void grhsim_clear_range_words(std::array<std::uint64_t, N> &value, std::size_t start, std::size_t width)
{
    if (width == 0 || N == 0) {
        return;
    }
    const std::size_t totalBits = N * 64u;
    if (start >= totalBits) {
        return;
    }
    const std::size_t end = std::min(totalBits, start + width);
    if (end <= start) {
        return;
    }
    const std::size_t startWord = start / 64u;
    const std::size_t endWord = (end - 1u) / 64u;
    const std::size_t startBit = start & 63u;
    const std::size_t endBits = ((end - 1u) & 63u) + 1u;
    if (startWord == endWord) {
        const std::uint64_t lowMask = startBit == 0 ? UINT64_C(0) : grhsim_mask(startBit);
        const std::uint64_t highMask = endBits >= 64 ? UINT64_C(0) : ~grhsim_mask(endBits);
        value[startWord] &= (lowMask | highMask);
        return;
    }
    value[startWord] &= (startBit == 0 ? UINT64_C(0) : grhsim_mask(startBit));
    for (std::size_t i = startWord + 1u; i < endWord && i < N; ++i) {
        value[i] = 0;
    }
    if (endWord < N) {
        value[endWord] &= (endBits >= 64 ? UINT64_C(0) : ~grhsim_mask(endBits));
    }
}

template <std::size_t N>
inline void grhsim_fill_range_words(std::array<std::uint64_t, N> &value, std::size_t start, std::size_t width)
{
    if (width == 0 || N == 0) {
        return;
    }
    const std::size_t totalBits = N * 64u;
    if (start >= totalBits) {
        return;
    }
    const std::size_t end = std::min(totalBits, start + width);
    if (end <= start) {
        return;
    }
    const std::size_t startWord = start / 64u;
    const std::size_t endWord = (end - 1u) / 64u;
    const std::size_t startBit = start & 63u;
    const std::size_t endBits = ((end - 1u) & 63u) + 1u;
    if (startWord == endWord) {
        const std::uint64_t lowMask = startBit == 0 ? ~UINT64_C(0) : ~grhsim_mask(startBit);
        const std::uint64_t highMask = endBits >= 64 ? ~UINT64_C(0) : grhsim_mask(endBits);
        value[startWord] |= (lowMask & highMask);
        return;
    }
    value[startWord] |= (startBit == 0 ? ~UINT64_C(0) : ~grhsim_mask(startBit));
    for (std::size_t i = startWord + 1u; i < endWord && i < N; ++i) {
        value[i] = ~UINT64_C(0);
    }
    if (endWord < N) {
        value[endWord] |= (endBits >= 64 ? ~UINT64_C(0) : grhsim_mask(endBits));
    }
}

template <std::size_t DestN, std::size_t SrcN>
inline void grhsim_insert_words(std::array<std::uint64_t, DestN> &dest,
                                std::size_t destLsb,
                                const std::array<std::uint64_t, SrcN> &src,
                                std::size_t srcWidth)
{
    if (srcWidth == 0 || DestN == 0 || SrcN == 0) {
        return;
    }
    grhsim_clear_range_words(dest, destLsb, srcWidth);
    const std::size_t srcWords = (srcWidth + 63u) / 64u;
    const std::size_t destWord = destLsb / 64u;
    const std::size_t bitShift = destLsb & 63u;
    for (std::size_t i = 0; i < srcWords && i < SrcN; ++i) {
        std::uint64_t word = src[i];
        const std::size_t wordWidth = (i + 1u == srcWords) ? (srcWidth - i * 64u) : 64u;
        word = grhsim_trunc_u64(word, wordWidth);
        if (destWord + i < DestN) {
            dest[destWord + i] |= (bitShift == 0 ? word : (word << bitShift));
        }
        if (bitShift != 0 && destWord + i + 1u < DestN) {
            dest[destWord + i + 1u] |= (word >> (64u - bitShift));
        }
    }
}

template <std::size_t DestLsb, std::size_t SrcWidth>
inline void grhsim_insert_words_2_1(std::array<std::uint64_t, 2> &dest,
                                    const std::array<std::uint64_t, 1> &src)
{
    static_assert(SrcWidth <= 64);
    static_assert(DestLsb < 128);
    constexpr std::size_t bitShift = DestLsb & 63u;
    constexpr std::size_t destWord = DestLsb / 64u;
    constexpr std::uint64_t bitsMask = grhsim_const_mask<SrcWidth>();
    const std::uint64_t bits = src[0] & bitsMask;
    if constexpr (destWord < 2) {
        if constexpr (bitShift == 0) {
            dest[destWord] |= bits;
        }
        else {
            dest[destWord] |= bits << bitShift;
        }
    }
    if constexpr (bitShift != 0 && destWord + 1u < 2 && SrcWidth + bitShift > 64u) {
        dest[destWord + 1u] |= bits >> (64u - bitShift);
    }
}

template <std::size_t DestLsb, std::size_t SrcWidth>
inline void grhsim_insert_words_2_2(std::array<std::uint64_t, 2> &dest,
                                    const std::array<std::uint64_t, 2> &src)
{
    static_assert(SrcWidth > 64 && SrcWidth <= 128);
    static_assert(DestLsb < 128);
    constexpr std::size_t bitShift = DestLsb & 63u;
    constexpr std::size_t destWord = DestLsb / 64u;
    const std::uint64_t word0 = src[0];
    const std::uint64_t word1 = src[1] & grhsim_const_mask<SrcWidth - 64u>();
    if constexpr (destWord < 2) {
        if constexpr (bitShift == 0) {
            dest[destWord] |= word0;
        }
        else {
            dest[destWord] |= word0 << bitShift;
        }
    }
    if constexpr (destWord + 1u < 2) {
        if constexpr (bitShift == 0) {
            dest[destWord + 1u] |= word1;
        }
        else {
            dest[destWord + 1u] |= (word0 >> (64u - bitShift)) | (word1 << bitShift);
        }
    }
}

template <std::size_t DestN>
inline void grhsim_insert_scalar_words(std::array<std::uint64_t, DestN> &dest,
                                       std::size_t destLsb,
                                       std::uint64_t src,
                                       std::size_t srcWidth)
{
    if (srcWidth == 0 || DestN == 0) {
        return;
    }
    grhsim_clear_range_words(dest, destLsb, srcWidth);
    const std::size_t destWord = destLsb / 64u;
    if (destWord >= DestN) {
        return;
    }
    const std::size_t bitShift = destLsb & 63u;
    const std::uint64_t bits = grhsim_trunc_u64(src, srcWidth);
    dest[destWord] |= (bitShift == 0 ? bits : (bits << bitShift));
    if (bitShift != 0 && destWord + 1u < DestN && srcWidth + bitShift > 64u) {
        dest[destWord + 1u] |= (bits >> (64u - bitShift));
    }
}

template <std::size_t DestN, std::size_t Count>
inline std::array<std::uint64_t, DestN> grhsim_concat_scalars_words(const std::array<std::uint64_t, Count> &values,
                                                                    const std::array<std::size_t, Count> &widths,
                                                                    std::size_t totalWidth)
{
    std::array<std::uint64_t, DestN> out{};
    std::size_t cursor = totalWidth;
    for (std::size_t i = 0; i < Count; ++i) {
        const std::size_t width = widths[i];
        if (width == 0) {
            continue;
        }
        if (width > cursor) {
            cursor = 0;
            continue;
        }
        cursor -= width;
        grhsim_insert_scalar_words(out, cursor, values[i], width);
    }
    grhsim_trunc_words(out, totalWidth);
    return out;
}

template <std::size_t DestN, std::size_t Count>
inline std::array<std::uint64_t, DestN> grhsim_concat_uniform_scalars_words(const std::array<std::uint64_t, Count> &values,
                                                                            std::size_t elemWidth,
                                                                            std::size_t totalWidth)
{
    std::array<std::uint64_t, DestN> out{};
    std::size_t cursor = totalWidth;
    for (std::size_t i = 0; i < Count; ++i) {
        if (elemWidth == 0) {
            continue;
        }
        if (elemWidth > cursor) {
            cursor = 0;
            continue;
        }
        cursor -= elemWidth;
        grhsim_insert_scalar_words(out, cursor, values[i], elemWidth);
    }
    grhsim_trunc_words(out, totalWidth);
    return out;
}

template <std::size_t LhsWidth, std::size_t RhsWidth, std::size_t TotalWidth>
inline std::array<std::uint64_t, 2> grhsim_concat_words_2_1_1(const std::array<std::uint64_t, 1> &lhs,
                                                              const std::array<std::uint64_t, 1> &rhs)
{
    static_assert(LhsWidth <= 64 && RhsWidth <= 64);
    static_assert(TotalWidth > 64 && TotalWidth <= 128);
    std::array<std::uint64_t, 2> out{};
    grhsim_insert_words_2_1<0, (RhsWidth < TotalWidth ? RhsWidth : TotalWidth)>(out, rhs);
    if constexpr (RhsWidth < TotalWidth) {
        grhsim_insert_words_2_1<RhsWidth, (LhsWidth < TotalWidth - RhsWidth ? LhsWidth : TotalWidth - RhsWidth)>(
            out,
            lhs);
    }
    return grhsim_trunc_words_2<TotalWidth>(out);
}

template <std::size_t LhsWidth, std::size_t RhsWidth, std::size_t TotalWidth>
inline std::array<std::uint64_t, 2> grhsim_concat_words_2_1_2(const std::array<std::uint64_t, 1> &lhs,
                                                              const std::array<std::uint64_t, 2> &rhs)
{
    static_assert(LhsWidth <= 64 && RhsWidth > 64 && RhsWidth <= 128);
    static_assert(TotalWidth > 64 && TotalWidth <= 128);
    std::array<std::uint64_t, 2> out{};
    grhsim_insert_words_2_2<0, (RhsWidth < TotalWidth ? RhsWidth : TotalWidth)>(out, rhs);
    if constexpr (RhsWidth < TotalWidth) {
        grhsim_insert_words_2_1<RhsWidth, (LhsWidth < TotalWidth - RhsWidth ? LhsWidth : TotalWidth - RhsWidth)>(
            out,
            lhs);
    }
    return grhsim_trunc_words_2<TotalWidth>(out);
}

template <std::size_t Index,
          std::size_t EmitRep,
          std::size_t ElemWidth,
          std::size_t TotalWidth>
inline void grhsim_replicate_words_2_1_impl(std::array<std::uint64_t, 2> &out,
                                            const std::array<std::uint64_t, 1> &value)
{
    if constexpr (Index < EmitRep) {
        constexpr std::size_t offset = Index * ElemWidth;
        constexpr std::size_t width = ElemWidth < TotalWidth - offset ? ElemWidth : TotalWidth - offset;
        grhsim_insert_words_2_1<offset, width>(out, value);
        grhsim_replicate_words_2_1_impl<Index + 1u, EmitRep, ElemWidth, TotalWidth>(out, value);
    }
}

template <std::size_t DestN, std::size_t LhsN, std::size_t RhsN>
inline std::array<std::uint64_t, DestN> grhsim_concat_words(const std::array<std::uint64_t, LhsN> &lhs,
                                                            std::size_t lhsWidth,
                                                            const std::array<std::uint64_t, RhsN> &rhs,
                                                            std::size_t rhsWidth,
                                                            std::size_t totalWidth)
{
    std::array<std::uint64_t, DestN> out{};
    const std::size_t boundedRhsWidth = std::min(rhsWidth, totalWidth);
    grhsim_insert_words(out, 0, rhs, boundedRhsWidth);
    if (boundedRhsWidth < totalWidth) {
        grhsim_insert_words(out, boundedRhsWidth, lhs, std::min(lhsWidth, totalWidth - boundedRhsWidth));
    }
    grhsim_trunc_words(out, totalWidth);
    return out;
}

template <std::size_t ElemWidth, std::size_t Rep, std::size_t TotalWidth>
inline std::array<std::uint64_t, 2> grhsim_replicate_words_2_1(const std::array<std::uint64_t, 1> &value)
{
    static_assert(ElemWidth <= 64);
    static_assert(TotalWidth > 64 && TotalWidth <= 128);
    std::array<std::uint64_t, 2> out{};
    if constexpr (ElemWidth != 0) {
        constexpr std::size_t liveRep = (TotalWidth + ElemWidth - 1u) / ElemWidth;
        constexpr std::size_t emitRep = liveRep < Rep ? liveRep : Rep;
        grhsim_replicate_words_2_1_impl<0, emitRep, ElemWidth, TotalWidth>(out, value);
    }
    return grhsim_trunc_words_2<TotalWidth>(out);
}

template <std::size_t DestN, std::size_t TotalWidth, std::size_t... Indices>
inline std::array<std::uint64_t, DestN> grhsim_replicate_bit_words_impl(
    const std::array<std::uint64_t, 1> &value,
    std::index_sequence<Indices...>)
{
    static_assert(DestN > 0);
    static_assert(TotalWidth > 0 && TotalWidth <= DestN * 64u);
    static_assert((TotalWidth + 63u) / 64u == DestN);
    const std::uint64_t fill = UINT64_C(0) - (value[0] & UINT64_C(1));
    std::array<std::uint64_t, DestN> out{((void)Indices, fill)...};
    if constexpr ((TotalWidth & 63u) != 0) {
        out[DestN - 1u] &= (UINT64_C(1) << (TotalWidth & 63u)) - UINT64_C(1);
    }
    return out;
}

template <std::size_t DestN, std::size_t TotalWidth>
inline std::array<std::uint64_t, DestN> grhsim_replicate_bit_words(
    const std::array<std::uint64_t, 1> &value)
{
    return grhsim_replicate_bit_words_impl<DestN, TotalWidth>(
        value,
        std::make_index_sequence<DestN>{});
}

template <std::size_t DestN, std::size_t SrcN>
inline std::array<std::uint64_t, DestN> grhsim_replicate_words(const std::array<std::uint64_t, SrcN> &value,
                                                               std::size_t elemWidth,
                                                               std::size_t rep,
                                                               std::size_t totalWidth)
{
    std::array<std::uint64_t, DestN> out{};
    for (std::size_t i = 0; i < rep; ++i) {
        const std::size_t offset = i * elemWidth;
        if (offset >= totalWidth) {
            break;
        }
        grhsim_insert_words(out, offset, value, std::min(elemWidth, totalWidth - offset));
    }
    grhsim_trunc_words(out, totalWidth);
    return out;
}

template <std::size_t DestN, std::size_t SrcN, typename Scalar,
          std::enable_if_t<std::is_integral_v<Scalar>, int> = 0>
inline std::array<std::uint64_t, DestN> grhsim_replicate_words(Scalar value,
                                                               std::size_t elemWidth,
                                                               std::size_t rep,
                                                               std::size_t totalWidth)
{
    std::array<std::uint64_t, 1> words{static_cast<std::uint64_t>(value)};
    return grhsim_replicate_words<DestN, 1>(words, elemWidth, rep, totalWidth);
}

template <std::size_t DestN, std::size_t SrcN>
inline std::array<std::uint64_t, DestN> grhsim_slice_words(const std::array<std::uint64_t, SrcN> &src,
                                                           std::size_t start,
                                                           std::size_t width)
{
    std::array<std::uint64_t, DestN> out{};
    if (width == 0 || DestN == 0 || SrcN == 0) {
        return out;
    }
    const std::size_t srcWord = start / 64u;
    const std::size_t bitShift = start & 63u;
    const std::size_t outWords = (width + 63u) / 64u;
    for (std::size_t i = 0; i < outWords && i < DestN; ++i) {
        const std::uint64_t low = (srcWord + i < SrcN) ? src[srcWord + i] : UINT64_C(0);
        if (bitShift == 0) {
            out[i] = low;
        }
        else {
            const std::uint64_t high = (srcWord + i + 1u < SrcN) ? src[srcWord + i + 1u] : UINT64_C(0);
            out[i] = (low >> bitShift) | (high << (64u - bitShift));
        }
    }
    grhsim_trunc_words(out, width);
    return out;
}

template <typename T>
inline std::size_t grhsim_index_words(T value, std::size_t cap)
{
    const std::uint64_t raw = static_cast<std::uint64_t>(value);
    if (raw >= cap) {
        return cap;
    }
    return static_cast<std::size_t>(raw);
}

template <std::size_t N>
inline std::size_t grhsim_index_words(const std::array<std::uint64_t, N> &value, std::size_t cap)
{
    for (std::size_t i = 1; i < N; ++i) {
        if (value[i] != 0) {
            return cap;
        }
    }
    if (value[0] >= cap) {
        return cap;
    }
    return static_cast<std::size_t>(value[0]);
}

template <typename T>
inline std::size_t grhsim_index_in_range_words(T value)
{
    return static_cast<std::size_t>(static_cast<std::uint64_t>(value));
}

template <std::size_t N>
inline std::size_t grhsim_index_in_range_words(const std::array<std::uint64_t, N> &value)
{
    return N == 0 ? 0 : static_cast<std::size_t>(value[0]);
}

template <typename T>
inline std::size_t grhsim_index_pow2_words(T value, std::size_t mask)
{
    return static_cast<std::size_t>(static_cast<std::uint64_t>(value)) & mask;
}

template <std::size_t N>
inline std::size_t grhsim_index_pow2_words(const std::array<std::uint64_t, N> &value, std::size_t mask)
{
    return (N == 0 ? 0 : static_cast<std::size_t>(value[0])) & mask;
}

template <std::size_t DestN, std::size_t SrcN, typename ShiftT>
inline std::array<std::uint64_t, DestN> grhsim_slice_words(const std::array<std::uint64_t, SrcN> &src,
                                                           const ShiftT &start,
                                                           std::size_t srcWidth,
                                                           std::size_t width)
{
    return grhsim_slice_words<DestN>(src, grhsim_index_words(start, srcWidth), width);
}

template <std::size_t N>
GRHSIM_ALWAYS_INLINE std::array<std::uint64_t, N> grhsim_not_words_full(const std::array<std::uint64_t, N> &value)
{
    std::array<std::uint64_t, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        out[i] = ~value[i];
    }
    return out;
}

template <std::size_t N>
inline std::array<std::uint64_t, N> grhsim_not_words(const std::array<std::uint64_t, N> &value, std::size_t width)
{
    std::array<std::uint64_t, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        out[i] = ~value[i];
    }
    grhsim_trunc_words(out, width);
    return out;
}

template <std::size_t N>
inline std::array<std::uint64_t, N> grhsim_mux_words(std::uint64_t cond,
                                                     const std::array<std::uint64_t, N> &trueValue,
                                                     const std::array<std::uint64_t, N> &falseValue,
                                                     std::size_t width)
{
    const std::uint64_t trueMask = static_cast<std::uint64_t>(-static_cast<std::int64_t>(cond != 0));
    std::array<std::uint64_t, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        out[i] = (trueValue[i] & trueMask) | (falseValue[i] & ~trueMask);
    }
    grhsim_trunc_words(out, width);
    return out;
}

template <std::size_t N>
GRHSIM_ALWAYS_INLINE std::array<std::uint64_t, N> grhsim_and_words_full(const std::array<std::uint64_t, N> &lhs,
                                                                        const std::array<std::uint64_t, N> &rhs)
{
    std::array<std::uint64_t, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        out[i] = lhs[i] & rhs[i];
    }
    return out;
}

template <std::size_t N>
inline std::array<std::uint64_t, N> grhsim_and_words(const std::array<std::uint64_t, N> &lhs,
                                                     const std::array<std::uint64_t, N> &rhs,
                                                     std::size_t width)
{
    std::array<std::uint64_t, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        out[i] = lhs[i] & rhs[i];
    }
    grhsim_trunc_words(out, width);
    return out;
}

template <std::size_t N>
GRHSIM_ALWAYS_INLINE std::array<std::uint64_t, N> grhsim_or_words_full(const std::array<std::uint64_t, N> &lhs,
                                                                       const std::array<std::uint64_t, N> &rhs)
{
    std::array<std::uint64_t, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        out[i] = lhs[i] | rhs[i];
    }
    return out;
}

template <std::size_t N>
inline std::array<std::uint64_t, N> grhsim_or_words(const std::array<std::uint64_t, N> &lhs,
                                                    const std::array<std::uint64_t, N> &rhs,
                                                    std::size_t width)
{
    std::array<std::uint64_t, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        out[i] = lhs[i] | rhs[i];
    }
    grhsim_trunc_words(out, width);
    return out;
}

template <std::size_t N>
GRHSIM_ALWAYS_INLINE std::array<std::uint64_t, N> grhsim_xor_words_full(const std::array<std::uint64_t, N> &lhs,
                                                                        const std::array<std::uint64_t, N> &rhs)
{
    std::array<std::uint64_t, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        out[i] = lhs[i] ^ rhs[i];
    }
    return out;
}

template <std::size_t N>
inline std::array<std::uint64_t, N> grhsim_xor_words(const std::array<std::uint64_t, N> &lhs,
                                                     const std::array<std::uint64_t, N> &rhs,
                                                     std::size_t width)
{
    std::array<std::uint64_t, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        out[i] = lhs[i] ^ rhs[i];
    }
    grhsim_trunc_words(out, width);
    return out;
}

template <std::size_t N>
GRHSIM_ALWAYS_INLINE std::array<std::uint64_t, N> grhsim_xnor_words_full(const std::array<std::uint64_t, N> &lhs,
                                                                         const std::array<std::uint64_t, N> &rhs)
{
    return grhsim_not_words_full(grhsim_xor_words_full(lhs, rhs));
}

template <std::size_t N>
inline std::array<std::uint64_t, N> grhsim_xnor_words(const std::array<std::uint64_t, N> &lhs,
                                                      const std::array<std::uint64_t, N> &rhs,
                                                      std::size_t width)
{
    return grhsim_not_words(grhsim_xor_words(lhs, rhs, width), width);
}

template <std::size_t N>
inline bool grhsim_sign_bit_words(const std::array<std::uint64_t, N> &value, std::size_t width)
{
    if (width == 0) {
        return false;
    }
    return grhsim_get_bit_words(value, width - 1u);
}

template <std::size_t N>
inline int grhsim_compare_unsigned_words(const std::array<std::uint64_t, N> &lhs,
                                         const std::array<std::uint64_t, N> &rhs)
{
    unsigned __int128 lhs128 = 0;
    unsigned __int128 rhs128 = 0;
    if (grhsim_try_u128_words(lhs, N * 64u, lhs128) && grhsim_try_u128_words(rhs, N * 64u, rhs128)) {
        if (lhs128 < rhs128) {
            return -1;
        }
        if (lhs128 > rhs128) {
            return 1;
        }
        return 0;
    }
    for (std::size_t i = N; i-- > 0;) {
        if (lhs[i] < rhs[i]) {
            return -1;
        }
        if (lhs[i] > rhs[i]) {
            return 1;
        }
    }
    return 0;
}

template <std::size_t N>
inline bool grhsim_try_u64_words(const std::array<std::uint64_t, N> &value,
                                 std::size_t width,
                                 std::uint64_t &out)
{
    const std::size_t liveWords = (width + 63u) / 64u;
    if (liveWords == 0 || N == 0) {
        out = 0;
        return true;
    }
    const std::size_t limit = liveWords < N ? liveWords : N;
    for (std::size_t i = 1; i < limit; ++i) {
        if (value[i] != 0) {
            return false;
        }
    }
    out = grhsim_trunc_u64(value[0], width >= 64 ? 64u : width);
    return true;
}

template <std::size_t N>
inline bool grhsim_try_u128_words(const std::array<std::uint64_t, N> &value,
                                  std::size_t width,
                                  unsigned __int128 &out)
{
    if (width > 128u) {
        return false;
    }
    const std::size_t liveWords = (width + 63u) / 64u;
    if (liveWords == 0 || N == 0) {
        out = 0;
        return true;
    }
    if (liveWords > 2u || liveWords > N) {
        return false;
    }
    const std::uint64_t lo = value[0];
    const std::uint64_t hi = liveWords >= 2u ? value[1] : UINT64_C(0);
    out = static_cast<unsigned __int128>(lo) | (static_cast<unsigned __int128>(hi) << 64u);
    return true;
}

template <std::size_t N>
inline std::array<std::uint64_t, N> grhsim_from_u128_words(unsigned __int128 value, std::size_t width)
{
    std::array<std::uint64_t, N> out{};
    if constexpr (N > 0) {
        out[0] = static_cast<std::uint64_t>(value);
    }
    if constexpr (N > 1) {
        out[1] = static_cast<std::uint64_t>(value >> 64u);
    }
    grhsim_trunc_words(out, width);
    return out;
}

template <std::size_t N>
inline bool grhsim_try_single_bit_words(const std::array<std::uint64_t, N> &value,
                                        std::size_t width,
                                        std::size_t &bitIndex)
{
    const std::size_t liveWords = (width + 63u) / 64u;
    bool found = false;
    bitIndex = 0;
    for (std::size_t i = 0; i < liveWords && i < N; ++i) {
        const std::size_t wordWidth = (i + 1u == liveWords) ? (width - i * 64u) : 64u;
        const std::uint64_t word = grhsim_trunc_u64(value[i], wordWidth);
        if (word == 0) {
            continue;
        }
        if ((word & (word - 1u)) != 0) {
            return false;
        }
        if (found) {
            return false;
        }
        found = true;
        bitIndex = i * 64u + static_cast<std::size_t>(__builtin_ctzll(word));
    }
    return found;
}

template <std::size_t N>
inline int grhsim_compare_signed_words(const std::array<std::uint64_t, N> &lhs,
                                       const std::array<std::uint64_t, N> &rhs,
                                       std::size_t width)
{
    const bool lhsNeg = grhsim_sign_bit_words(lhs, width);
    const bool rhsNeg = grhsim_sign_bit_words(rhs, width);
    if (lhsNeg != rhsNeg) {
        return lhsNeg ? -1 : 1;
    }
    return grhsim_compare_unsigned_words(lhs, rhs);
}

template <std::size_t N>
inline bool grhsim_reduce_and_words(const std::array<std::uint64_t, N> &value, std::size_t width)
{
    if (width == 0) {
        return false;
    }
    const std::size_t liveWords = (width + 63u) / 64u;
    for (std::size_t i = 0; i < liveWords && i < N; ++i) {
        const std::size_t wordWidth = (i + 1u == liveWords) ? (width - i * 64u) : 64u;
        if (grhsim_trunc_u64(value[i], wordWidth) != grhsim_mask(wordWidth)) {
            return false;
        }
    }
    return true;
}

template <std::size_t N>
inline bool grhsim_reduce_nand_words(const std::array<std::uint64_t, N> &value, std::size_t width)
{
    return !grhsim_reduce_and_words(value, width);
}

template <std::size_t N>
inline bool grhsim_reduce_or_words(const std::array<std::uint64_t, N> &value, std::size_t width)
{
    return grhsim_any_bits_words(value, width);
}

template <std::size_t N>
inline bool grhsim_reduce_nor_words(const std::array<std::uint64_t, N> &value, std::size_t width)
{
    return !grhsim_reduce_or_words(value, width);
}

template <std::size_t N>
inline bool grhsim_reduce_xor_words(const std::array<std::uint64_t, N> &value, std::size_t width)
{
    unsigned parity = 0;
    const std::size_t liveWords = (width + 63u) / 64u;
    for (std::size_t i = 0; i < liveWords && i < N; ++i) {
        const std::size_t wordWidth = (i + 1u == liveWords) ? (width - i * 64u) : 64u;
        parity ^= static_cast<unsigned>(__builtin_popcountll(grhsim_trunc_u64(value[i], wordWidth)) & 1u);
    }
    return (parity & 1u) != 0;
}

template <std::size_t N>
inline bool grhsim_reduce_xnor_words(const std::array<std::uint64_t, N> &value, std::size_t width)
{
    return !grhsim_reduce_xor_words(value, width);
}

template <std::size_t N>
inline std::array<std::uint64_t, N> grhsim_add_words(const std::array<std::uint64_t, N> &lhs,
                                                     const std::array<std::uint64_t, N> &rhs,
                                                     std::size_t width)
{
    unsigned __int128 lhs128 = 0;
    unsigned __int128 rhs128 = 0;
    if (grhsim_try_u128_words(lhs, width, lhs128) && grhsim_try_u128_words(rhs, width, rhs128)) {
        return grhsim_from_u128_words<N>(lhs128 + rhs128, width);
    }
    std::array<std::uint64_t, N> out{};
    unsigned __int128 carry = 0;
    for (std::size_t i = 0; i < N; ++i) {
        const unsigned __int128 sum =
            static_cast<unsigned __int128>(lhs[i]) + static_cast<unsigned __int128>(rhs[i]) + carry;
        out[i] = static_cast<std::uint64_t>(sum);
        carry = sum >> 64u;
    }
    grhsim_trunc_words(out, width);
    return out;
}

template <std::size_t Width>
inline std::array<std::uint64_t, 2> grhsim_add_words_2(const std::array<std::uint64_t, 2> &lhs,
                                                       const std::array<std::uint64_t, 2> &rhs)
{
    static_assert(Width > 64 && Width <= 128);
    const unsigned __int128 lhs128 =
        static_cast<unsigned __int128>(lhs[0]) |
        (static_cast<unsigned __int128>(lhs[1] & grhsim_const_mask<Width - 64u>()) << 64u);
    const unsigned __int128 rhs128 =
        static_cast<unsigned __int128>(rhs[0]) |
        (static_cast<unsigned __int128>(rhs[1] & grhsim_const_mask<Width - 64u>()) << 64u);
    const unsigned __int128 sum = lhs128 + rhs128;
    return grhsim_trunc_words_2<Width>(std::array<std::uint64_t, 2>{
        static_cast<std::uint64_t>(sum),
        static_cast<std::uint64_t>(sum >> 64u)});
}

template <std::size_t N>
inline std::array<std::uint64_t, N> grhsim_sub_words(const std::array<std::uint64_t, N> &lhs,
                                                     const std::array<std::uint64_t, N> &rhs,
                                                     std::size_t width)
{
    unsigned __int128 lhs128 = 0;
    unsigned __int128 rhs128 = 0;
    if (grhsim_try_u128_words(lhs, width, lhs128) && grhsim_try_u128_words(rhs, width, rhs128)) {
        return grhsim_from_u128_words<N>(lhs128 - rhs128, width);
    }
    std::array<std::uint64_t, N> out{};
    std::uint64_t borrow = 0;
    for (std::size_t i = 0; i < N; ++i) {
        const std::uint64_t rhsWord = rhs[i] + borrow;
        borrow = (rhsWord < rhs[i] || lhs[i] < rhsWord) ? 1 : 0;
        out[i] = lhs[i] - rhsWord;
    }
    grhsim_trunc_words(out, width);
    return out;
}

template <std::size_t N>
inline std::array<std::uint64_t, N> grhsim_mul_words(const std::array<std::uint64_t, N> &lhs,
                                                     const std::array<std::uint64_t, N> &rhs,
                                                     std::size_t width)
{
    unsigned __int128 lhs128 = 0;
    unsigned __int128 rhs128 = 0;
    if (grhsim_try_u128_words(lhs, width, lhs128) && grhsim_try_u128_words(rhs, width, rhs128)) {
        return grhsim_from_u128_words<N>(lhs128 * rhs128, width);
    }

    auto mulByWord = [&](const std::array<std::uint64_t, N> &value,
                         std::uint64_t rhsWord) -> std::array<std::uint64_t, N>
    {
        std::array<std::uint64_t, N> out{};
        unsigned __int128 carry = 0;
        for (std::size_t i = 0; i < N; ++i) {
            const unsigned __int128 accum =
                static_cast<unsigned __int128>(value[i]) * static_cast<unsigned __int128>(rhsWord) + carry;
            out[i] = static_cast<std::uint64_t>(accum);
            carry = accum >> 64u;
        }
        grhsim_trunc_words(out, width);
        return out;
    };

    std::uint64_t rhsWord = 0;
    if (grhsim_try_u64_words(rhs, width, rhsWord)) {
        return mulByWord(lhs, rhsWord);
    }
    std::uint64_t lhsWord = 0;
    if (grhsim_try_u64_words(lhs, width, lhsWord)) {
        return mulByWord(rhs, lhsWord);
    }
    std::size_t rhsBitIndex = 0;
    if (grhsim_try_single_bit_words(rhs, width, rhsBitIndex)) {
        return grhsim_shl_words(lhs, rhsBitIndex, width);
    }
    std::size_t lhsBitIndex = 0;
    if (grhsim_try_single_bit_words(lhs, width, lhsBitIndex)) {
        return grhsim_shl_words(rhs, lhsBitIndex, width);
    }

    std::array<std::uint64_t, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        unsigned __int128 carry = 0;
        for (std::size_t j = 0; j + i < N; ++j) {
            const unsigned __int128 accum =
                static_cast<unsigned __int128>(out[i + j]) +
                static_cast<unsigned __int128>(lhs[i]) * static_cast<unsigned __int128>(rhs[j]) +
                carry;
            out[i + j] = static_cast<std::uint64_t>(accum);
            carry = accum >> 64u;
        }
    }
    grhsim_trunc_words(out, width);
    return out;
}

template <std::size_t N>
inline std::array<std::uint64_t, N> grhsim_negate_words(const std::array<std::uint64_t, N> &value, std::size_t width)
{
    std::array<std::uint64_t, N> out = grhsim_not_words(value, width);
    std::array<std::uint64_t, N> one{};
    if constexpr (N > 0) {
        one[0] = 1;
    }
    return grhsim_add_words(out, one, width);
}

template <std::size_t N>
inline void grhsim_shl1_words_inplace(std::array<std::uint64_t, N> &value, std::size_t width)
{
    std::uint64_t carry = 0;
    for (std::size_t i = 0; i < N; ++i) {
        const std::uint64_t nextCarry = value[i] >> 63u;
        value[i] = (value[i] << 1u) | carry;
        carry = nextCarry;
    }
    grhsim_trunc_words(value, width);
}

template <std::size_t N>
inline std::size_t grhsim_highest_bit_words(const std::array<std::uint64_t, N> &value, std::size_t width)
{
    const std::size_t liveWords = (width + 63u) / 64u;
    for (std::size_t i = liveWords; i-- > 0;) {
        const std::size_t wordWidth = (i + 1u == liveWords) ? (width - i * 64u) : 64u;
        const std::uint64_t word = grhsim_trunc_u64(value[i], wordWidth);
        if (word != 0) {
            return i * 64u + (63u - static_cast<std::size_t>(__builtin_clzll(word)));
        }
    }
    return 0;
}

template <std::size_t N>
inline std::uint64_t grhsim_clog2_words(const std::array<std::uint64_t, N> &value, std::size_t width)
{
    if (!grhsim_any_bits_words(value, width)) {
        return 0;
    }
    std::array<std::uint64_t, N> one{};
    if constexpr (N > 0) {
        one[0] = 1;
    }
    const auto valueMinusOne = grhsim_sub_words(value, one, width);
    if (!grhsim_any_bits_words(valueMinusOne, width)) {
        return 0;
    }
    return static_cast<std::uint64_t>(grhsim_highest_bit_words(valueMinusOne, width) + 1u);
}

template <std::size_t N>
inline std::array<std::uint64_t, N> grhsim_udiv_words(const std::array<std::uint64_t, N> &lhs,
                                                      const std::array<std::uint64_t, N> &rhs,
                                                      std::size_t width)
{
    unsigned __int128 lhs128 = 0;
    unsigned __int128 rhs128 = 0;
    if (grhsim_try_u128_words(lhs, width, lhs128) && grhsim_try_u128_words(rhs, width, rhs128)) {
        if (rhs128 == 0) {
            return {};
        }
        return grhsim_from_u128_words<N>(lhs128 / rhs128, width);
    }

    std::uint64_t rhsWord = 0;
    if (grhsim_try_u64_words(rhs, width, rhsWord)) {
        if (rhsWord == 0) {
            return {};
        }
        std::array<std::uint64_t, N> quotient{};
        unsigned __int128 remainder = 0;
        const std::size_t liveWords = (width + 63u) / 64u;
        for (std::size_t i = liveWords; i-- > 0;) {
            const unsigned __int128 dividend =
                (remainder << 64u) | static_cast<unsigned __int128>(lhs[i]);
            quotient[i] = static_cast<std::uint64_t>(dividend / rhsWord);
            remainder = dividend % rhsWord;
        }
        grhsim_trunc_words(quotient, width);
        return quotient;
    }
    std::size_t rhsBitIndex = 0;
    if (grhsim_try_single_bit_words(rhs, width, rhsBitIndex)) {
        return grhsim_lshr_words(lhs, rhsBitIndex, width);
    }
    if (!grhsim_any_bits_words(rhs, width)) {
        return {};
    }
    std::array<std::uint64_t, N> quotient{};
    std::array<std::uint64_t, N> remainder = lhs;
    const std::size_t rhsHighestBit = grhsim_highest_bit_words(rhs, width);
    while (grhsim_compare_unsigned_words(remainder, rhs) >= 0) {
        const std::size_t remainderHighestBit = grhsim_highest_bit_words(remainder, width);
        std::size_t shift = remainderHighestBit - rhsHighestBit;
        auto shiftedDivisor = grhsim_shl_words(rhs, shift, width);
        if (grhsim_compare_unsigned_words(remainder, shiftedDivisor) < 0) {
            if (shift == 0) {
                break;
            }
            --shift;
            shiftedDivisor = grhsim_shl_words(rhs, shift, width);
        }
        remainder = grhsim_sub_words(remainder, shiftedDivisor, width);
        grhsim_put_bit_words(quotient, shift, true);
    }
    grhsim_trunc_words(quotient, width);
    return quotient;
}

template <std::size_t N>
inline std::array<std::uint64_t, N> grhsim_umod_words(const std::array<std::uint64_t, N> &lhs,
                                                      const std::array<std::uint64_t, N> &rhs,
                                                      std::size_t width)
{
    unsigned __int128 lhs128 = 0;
    unsigned __int128 rhs128 = 0;
    if (grhsim_try_u128_words(lhs, width, lhs128) && grhsim_try_u128_words(rhs, width, rhs128)) {
        if (rhs128 == 0) {
            return {};
        }
        return grhsim_from_u128_words<N>(lhs128 % rhs128, width);
    }

    std::uint64_t rhsWord = 0;
    if (grhsim_try_u64_words(rhs, width, rhsWord)) {
        if (rhsWord == 0) {
            return {};
        }
        unsigned __int128 remainder = 0;
        const std::size_t liveWords = (width + 63u) / 64u;
        for (std::size_t i = liveWords; i-- > 0;) {
            const unsigned __int128 dividend =
                (remainder << 64u) | static_cast<unsigned __int128>(lhs[i]);
            remainder = dividend % rhsWord;
        }
        std::array<std::uint64_t, N> out{};
        if constexpr (N > 0) {
            out[0] = static_cast<std::uint64_t>(remainder);
        }
        grhsim_trunc_words(out, width);
        return out;
    }
    std::size_t rhsBitIndex = 0;
    if (grhsim_try_single_bit_words(rhs, width, rhsBitIndex)) {
        std::array<std::uint64_t, N> out{};
        if (rhsBitIndex != 0) {
            out = grhsim_slice_words<N>(lhs, 0, rhsBitIndex);
        }
        grhsim_trunc_words(out, width);
        return out;
    }
    if (!grhsim_any_bits_words(rhs, width)) {
        return {};
    }
    std::array<std::uint64_t, N> remainder = lhs;
    const std::size_t rhsHighestBit = grhsim_highest_bit_words(rhs, width);
    while (grhsim_compare_unsigned_words(remainder, rhs) >= 0) {
        const std::size_t remainderHighestBit = grhsim_highest_bit_words(remainder, width);
        std::size_t shift = remainderHighestBit - rhsHighestBit;
        auto shiftedDivisor = grhsim_shl_words(rhs, shift, width);
        if (grhsim_compare_unsigned_words(remainder, shiftedDivisor) < 0) {
            if (shift == 0) {
                break;
            }
            --shift;
            shiftedDivisor = grhsim_shl_words(rhs, shift, width);
        }
        remainder = grhsim_sub_words(remainder, shiftedDivisor, width);
    }
    grhsim_trunc_words(remainder, width);
    return remainder;
}

template <std::size_t N>
inline std::array<std::uint64_t, N> grhsim_sdiv_words(const std::array<std::uint64_t, N> &lhs,
                                                      const std::array<std::uint64_t, N> &rhs,
                                                      std::size_t width)
{
    const bool lhsNeg = grhsim_sign_bit_words(lhs, width);
    const bool rhsNeg = grhsim_sign_bit_words(rhs, width);
    const auto lhsAbs = lhsNeg ? grhsim_negate_words(lhs, width) : lhs;
    const auto rhsAbs = rhsNeg ? grhsim_negate_words(rhs, width) : rhs;
    auto quotient = grhsim_udiv_words(lhsAbs, rhsAbs, width);
    if (lhsNeg != rhsNeg) {
        quotient = grhsim_negate_words(quotient, width);
    }
    grhsim_trunc_words(quotient, width);
    return quotient;
}

template <std::size_t N>
inline std::array<std::uint64_t, N> grhsim_smod_words(const std::array<std::uint64_t, N> &lhs,
                                                      const std::array<std::uint64_t, N> &rhs,
                                                      std::size_t width)
{
    const bool lhsNeg = grhsim_sign_bit_words(lhs, width);
    const bool rhsNeg = grhsim_sign_bit_words(rhs, width);
    const auto lhsAbs = lhsNeg ? grhsim_negate_words(lhs, width) : lhs;
    const auto rhsAbs = rhsNeg ? grhsim_negate_words(rhs, width) : rhs;
    auto remainder = grhsim_umod_words(lhsAbs, rhsAbs, width);
    if (lhsNeg) {
        remainder = grhsim_negate_words(remainder, width);
    }
    grhsim_trunc_words(remainder, width);
    return remainder;
}

template <std::size_t N, typename ShiftT>
inline std::array<std::uint64_t, N> grhsim_shl_words(const std::array<std::uint64_t, N> &value,
                                                     const ShiftT &shift,
                                                     std::size_t width)
{
    const std::size_t amount = grhsim_index_words(shift, width);
    if (amount >= width) {
        return {};
    }
    std::array<std::uint64_t, N> out{};
    const std::size_t wordShift = amount / 64u;
    const std::size_t bitShift = amount & 63u;
    for (std::size_t i = N; i-- > 0;) {
        if (i < wordShift) {
            continue;
        }
        const std::uint64_t low = value[i - wordShift];
        out[i] = (bitShift == 0 ? low : (low << bitShift));
        if (bitShift != 0 && i > wordShift) {
            out[i] |= (value[i - wordShift - 1u] >> (64u - bitShift));
        }
    }
    grhsim_trunc_words(out, width);
    return out;
}

template <std::size_t N, typename ShiftT>
inline std::array<std::uint64_t, N> grhsim_lshr_words(const std::array<std::uint64_t, N> &value,
                                                      const ShiftT &shift,
                                                      std::size_t width)
{
    const std::size_t amount = grhsim_index_words(shift, width);
    if (amount >= width) {
        return {};
    }
    std::array<std::uint64_t, N> out{};
    const std::size_t wordShift = amount / 64u;
    const std::size_t bitShift = amount & 63u;
    for (std::size_t i = 0; i < N; ++i) {
        if (i + wordShift >= N) {
            break;
        }
        const std::uint64_t high = value[i + wordShift];
        out[i] = (bitShift == 0 ? high : (high >> bitShift));
        if (bitShift != 0 && i + wordShift + 1u < N) {
            out[i] |= (value[i + wordShift + 1u] << (64u - bitShift));
        }
    }
    grhsim_trunc_words(out, width);
    return out;
}

template <std::size_t N, typename ShiftT>
inline std::array<std::uint64_t, N> grhsim_ashr_words(const std::array<std::uint64_t, N> &value,
                                                      const ShiftT &shift,
                                                      std::size_t width)
{
    const std::size_t amount = grhsim_index_words(shift, width);
    const bool sign = grhsim_sign_bit_words(value, width);
    if (amount >= width) {
        std::array<std::uint64_t, N> fill{};
        if (sign) {
            grhsim_fill_range_words(fill, 0, width);
        }
        grhsim_trunc_words(fill, width);
        return fill;
    }
    std::array<std::uint64_t, N> out = grhsim_lshr_words(value, shift, width);
    if (sign) {
        grhsim_fill_range_words(out, width - amount, amount);
    }
    grhsim_trunc_words(out, width);
    return out;
}

)CPP";
            *stream << "struct grhsim_active_mask_entry\n{\n";
            *stream << "    std::uint32_t word_index;\n";
            *stream << "    std::uint8_t mask;\n";
            *stream << "};\n\n";
            *stream << "inline void grhsim_or_active_u16(std::uint8_t *activeFlags, std::size_t index, std::uint16_t mask)\n{\n";
            *stream << "    std::uint16_t value = 0;\n";
            *stream << "    std::memcpy(&value, activeFlags + index, sizeof(value));\n";
            *stream << "    value = static_cast<std::uint16_t>(value | mask);\n";
            *stream << "    std::memcpy(activeFlags + index, &value, sizeof(value));\n";
            *stream << "}\n\n";
            *stream << "inline void grhsim_or_active_u32(std::uint8_t *activeFlags, std::size_t index, std::uint32_t mask)\n{\n";
            *stream << "    std::uint32_t value = 0;\n";
            *stream << "    std::memcpy(&value, activeFlags + index, sizeof(value));\n";
            *stream << "    value |= mask;\n";
            *stream << "    std::memcpy(activeFlags + index, &value, sizeof(value));\n";
            *stream << "}\n\n";
            *stream << "inline void grhsim_or_active_u64(std::uint8_t *activeFlags, std::size_t index, std::uint64_t mask)\n{\n";
            *stream << "    std::uint64_t value = 0;\n";
            *stream << "    std::memcpy(&value, activeFlags + index, sizeof(value));\n";
            *stream << "    value |= mask;\n";
            *stream << "    std::memcpy(activeFlags + index, &value, sizeof(value));\n";
            *stream << "}\n\n";
            *stream << "inline std::uint8_t grhsim_popcount_u8(std::uint8_t value)\n{\n";
            *stream << "    return static_cast<std::uint8_t>(__builtin_popcount(static_cast<unsigned>(value)));\n";
            *stream << "}\n\n";
            *stream << "template <typename ActiveFlags>\n";
            *stream << "inline bool grhsim_any_active_flags(const ActiveFlags &activeFlags)\n{\n";
            *stream << "    const auto *bytes = activeFlags.data();\n";
            *stream << "    const std::size_t byteCount = activeFlags.size();\n";
            *stream << "    std::size_t index = 0;\n";
            *stream << "    for (; index + 32u <= byteCount; index += 32u) {\n";
            *stream << "        std::uint64_t word0 = 0;\n";
            *stream << "        std::uint64_t word1 = 0;\n";
            *stream << "        std::uint64_t word2 = 0;\n";
            *stream << "        std::uint64_t word3 = 0;\n";
            *stream << "        std::memcpy(&word0, bytes + index, sizeof(word0));\n";
            *stream << "        std::memcpy(&word1, bytes + index + 8u, sizeof(word1));\n";
            *stream << "        std::memcpy(&word2, bytes + index + 16u, sizeof(word2));\n";
            *stream << "        std::memcpy(&word3, bytes + index + 24u, sizeof(word3));\n";
            *stream << "#if defined(__GNUC__) || defined(__clang__)\n";
            *stream << "        // Preserve wide probes instead of re-forming a byte-at-a-time early-exit loop.\n";
            *stream << "        __asm__ __volatile__(\"\" : \"+r\"(word0), \"+r\"(word1), \"+r\"(word2), \"+r\"(word3));\n";
            *stream << "#endif\n";
            *stream << "        if ((word0 | word1 | word2 | word3) != UINT64_C(0)) {\n";
            *stream << "            return true;\n";
            *stream << "        }\n";
            *stream << "    }\n";
            *stream << "    for (; index + 8u <= byteCount; index += 8u) {\n";
            *stream << "        std::uint64_t word = 0;\n";
            *stream << "        std::memcpy(&word, bytes + index, sizeof(word));\n";
            *stream << "#if defined(__GNUC__) || defined(__clang__)\n";
            *stream << "        __asm__ __volatile__(\"\" : \"+r\"(word));\n";
            *stream << "#endif\n";
            *stream << "        if (word != UINT64_C(0)) {\n";
            *stream << "            return true;\n";
            *stream << "        }\n";
            *stream << "    }\n";
            *stream << "    for (; index < byteCount; ++index) {\n";
            *stream << "        if (bytes[index] != UINT8_C(0)) {\n";
            *stream << "            return true;\n";
            *stream << "        }\n";
            *stream << "    }\n";
            *stream << "    return false;\n";
            *stream << "}\n\n";
            *stream << "template <typename ActiveFlags>\n";
            *stream << "inline std::size_t grhsim_count_active_supernodes(const ActiveFlags &activeFlags)\n{\n";
            *stream << "    std::size_t total = 0;\n";
            *stream << "    for (const auto word : activeFlags) {\n";
            *stream << "        total += static_cast<std::size_t>(grhsim_popcount_u8(word));\n";
            *stream << "    }\n";
            *stream << "    return total;\n";
            *stream << "}\n\n";
            *stream << "template <typename ActiveFlags>\n";
            *stream << "inline std::size_t grhsim_count_nonzero_active_words(const ActiveFlags &activeFlags)\n{\n";
            *stream << "    std::size_t total = 0;\n";
            *stream << "    for (const auto word : activeFlags) {\n";
            *stream << "        total += word == UINT8_C(0) ? 0u : 1u;\n";
            *stream << "    }\n";
            *stream << "    return total;\n";
            *stream << "}\n\n";
            *stream << "template <typename Sample>\n";
            *stream << "inline Sample grhsim_percentile_sample(std::vector<Sample> samples, std::size_t num, std::size_t den)\n{\n";
            *stream << "    if (samples.empty() || den == 0) {\n";
            *stream << "        return Sample{};\n";
            *stream << "    }\n";
            *stream << "    std::sort(samples.begin(), samples.end());\n";
            *stream << "    const std::size_t idx = ((samples.size() - 1u) * num) / den;\n";
            *stream << "    return samples[idx];\n";
            *stream << "}\n\n";
            *stream << "template <typename Sample>\n";
            *stream << "inline void grhsim_print_sample_summary(std::FILE *fp,\n";
            *stream << "                                       const char *label,\n";
            *stream << "                                       const std::vector<Sample> &samples)\n{\n";
            *stream << "    if (samples.empty()) {\n";
            *stream << "        std::fprintf(fp, \"[grhsim-activity] %s samples=0\\n\", label);\n";
            *stream << "        return;\n";
            *stream << "    }\n";
            *stream << "    Sample minValue = samples.front();\n";
            *stream << "    Sample maxValue = samples.front();\n";
            *stream << "    long double total = 0.0;\n";
            *stream << "    for (const Sample sample : samples) {\n";
            *stream << "        if (sample < minValue) {\n";
            *stream << "            minValue = sample;\n";
            *stream << "        }\n";
            *stream << "        if (sample > maxValue) {\n";
            *stream << "            maxValue = sample;\n";
            *stream << "        }\n";
            *stream << "        total += static_cast<long double>(sample);\n";
            *stream << "    }\n";
            *stream << "    std::fprintf(fp,\n";
            *stream << "                 \"[grhsim-activity] %s samples=%zu avg=%.2Lf min=%llu p50=%llu p90=%llu p99=%llu max=%llu\\n\",\n";
            *stream << "                 label,\n";
            *stream << "                 samples.size(),\n";
            *stream << "                 total / static_cast<long double>(samples.size()),\n";
            *stream << "                 static_cast<unsigned long long>(minValue),\n";
            *stream << "                 static_cast<unsigned long long>(grhsim_percentile_sample(samples, 50u, 100u)),\n";
            *stream << "                 static_cast<unsigned long long>(grhsim_percentile_sample(samples, 90u, 100u)),\n";
            *stream << "                 static_cast<unsigned long long>(grhsim_percentile_sample(samples, 99u, 100u)),\n";
            *stream << "                 static_cast<unsigned long long>(maxValue));\n";
            *stream << "}\n\n";
            *stream << "template <std::size_t N>\n";
            *stream << "inline void grhsim_mark_pending_write(std::array<std::uint32_t, N> &touchedIndices,\n";
            *stream << "                                      std::array<std::uint8_t, N> &touchedFlags,\n";
            *stream << "                                      std::size_t &touchedCount,\n";
            *stream << "                                      std::uint32_t writeIndex)\n{\n";
            *stream << "    if constexpr (N > 0) {\n";
            *stream << "        if (touchedFlags[writeIndex] == 0) {\n";
            *stream << "            touchedFlags[writeIndex] = 1;\n";
            *stream << "            touchedIndices[touchedCount++] = writeIndex;\n";
            *stream << "        }\n";
            *stream << "    } else {\n";
            *stream << "        (void)touchedIndices;\n";
            *stream << "        (void)touchedFlags;\n";
            *stream << "        (void)touchedCount;\n";
            *stream << "        (void)writeIndex;\n";
            *stream << "    }\n";
            *stream << "}\n";
            if (options.systemTasks)
            {
                *stream << R"CPP(

enum class grhsim_task_arg_kind {
    Logic,
    Real,
    String,
};

struct grhsim_task_arg {
    grhsim_task_arg_kind kind = grhsim_task_arg_kind::Logic;
    std::size_t width = 0;
    bool isSigned = false;
    bool isWide = false;
    std::uint64_t scalarValue = 0;
    std::vector<std::uint64_t> words;
    double realValue = 0.0;
    std::string stringValue;
};

struct grhsim_scalar_task_arg {
    std::uint64_t value = 0;
    std::size_t width = 0;
    bool isSigned = false;
};

inline grhsim_task_arg grhsim_make_task_arg(std::uint64_t value, std::size_t width, bool isSigned)
{
    grhsim_task_arg arg;
    arg.kind = grhsim_task_arg_kind::Logic;
    arg.width = width;
    arg.isSigned = isSigned;
    arg.isWide = false;
    arg.scalarValue = grhsim_trunc_u64(value, width == 0 ? 64u : width);
    return arg;
}

template <std::size_t N>
inline grhsim_task_arg grhsim_make_task_arg(const std::array<std::uint64_t, N> &value,
                                            std::size_t width,
                                            bool isSigned)
{
    grhsim_task_arg arg;
    arg.kind = grhsim_task_arg_kind::Logic;
    arg.width = width;
    arg.isSigned = isSigned;
    arg.isWide = true;
    arg.words.assign(value.begin(), value.end());
    const std::size_t liveWords = (width + 63u) / 64u;
    if (arg.words.size() < liveWords) {
        arg.words.resize(liveWords, 0);
    }
    if (width != 0 && !arg.words.empty()) {
        const std::size_t tailWidth = width - ((liveWords - 1u) * 64u);
        arg.words[liveWords - 1u] = grhsim_trunc_u64(arg.words[liveWords - 1u], tailWidth);
    }
    return arg;
}

// Wide values are C23 _BitInt objects: accept the deduced integer type and
// unpack 64-bit words (the value is width-clean by the padding invariant).
template <typename T>
inline grhsim_task_arg grhsim_make_task_arg_wide(const T &value, std::size_t width, bool isSigned)
{
    grhsim_task_arg arg;
    arg.kind = grhsim_task_arg_kind::Logic;
    arg.width = width;
    arg.isSigned = isSigned;
    arg.isWide = true;
    const std::size_t liveWords = (width + 63u) / 64u;
    arg.words.resize(liveWords);
    for (std::size_t i = 0; i < liveWords; ++i) {
        arg.words[i] = static_cast<std::uint64_t>(value >> (i * 64u));
    }
    if (width != 0 && liveWords != 0) {
        const std::size_t tailWidth = width - ((liveWords - 1u) * 64u);
        arg.words[liveWords - 1u] = grhsim_trunc_u64(arg.words[liveWords - 1u], tailWidth);
    }
    return arg;
}

inline grhsim_task_arg grhsim_make_task_arg(double value)
{
    grhsim_task_arg arg;
    arg.kind = grhsim_task_arg_kind::Real;
    arg.realValue = value;
    return arg;
}

inline grhsim_task_arg grhsim_make_task_arg(const std::string &value)
{
    grhsim_task_arg arg;
    arg.kind = grhsim_task_arg_kind::String;
    arg.stringValue = value;
    return arg;
}

inline grhsim_task_arg grhsim_make_task_arg(std::string &&value)
{
    grhsim_task_arg arg;
    arg.kind = grhsim_task_arg_kind::String;
    arg.stringValue = std::move(value);
    return arg;
}

inline grhsim_task_arg grhsim_make_task_arg(const char *value)
{
    return grhsim_make_task_arg(std::string(value == nullptr ? "" : value));
}

inline void grhsim_task_trunc_words(std::vector<std::uint64_t> &words, std::size_t width)
{
    const std::size_t liveWords = (width + 63u) / 64u;
    if (words.size() < liveWords) {
        words.resize(liveWords, 0);
    }
    if (liveWords == 0) {
        words.clear();
        return;
    }
    words.resize(liveWords);
    const std::size_t tailWidth = width - ((liveWords - 1u) * 64u);
    words[liveWords - 1u] = grhsim_trunc_u64(words[liveWords - 1u], tailWidth);
}

inline bool grhsim_task_words_is_zero(const std::vector<std::uint64_t> &words)
{
    for (std::uint64_t word : words) {
        if (word != 0) {
            return false;
        }
    }
    return true;
}

inline bool grhsim_task_sign_bit(const std::vector<std::uint64_t> &words, std::size_t width)
{
    if (width == 0 || words.empty()) {
        return false;
    }
    const std::size_t wordIndex = (width - 1u) / 64u;
    const std::size_t bitIndex = (width - 1u) & 63u;
    if (wordIndex >= words.size()) {
        return false;
    }
    return ((words[wordIndex] >> bitIndex) & UINT64_C(1)) != 0;
}

inline void grhsim_task_negate_words(std::vector<std::uint64_t> &words, std::size_t width)
{
    for (std::uint64_t &word : words) {
        word = ~word;
    }
    std::uint64_t carry = 1;
    for (std::uint64_t &word : words) {
        const std::uint64_t next = word + carry;
        carry = (next < word) ? 1 : 0;
        word = next;
        if (carry == 0) {
            break;
        }
    }
    grhsim_task_trunc_words(words, width);
}

inline std::uint32_t grhsim_task_divmod_words(std::vector<std::uint64_t> &words, std::uint32_t base)
{
    unsigned __int128 rem = 0;
    for (std::size_t i = words.size(); i-- > 0;) {
        const unsigned __int128 cur = (rem << 64u) | words[i];
        words[i] = static_cast<std::uint64_t>(cur / base);
        rem = cur % base;
    }
    return static_cast<std::uint32_t>(rem);
}

inline std::string grhsim_task_unsigned_words_to_base(std::vector<std::uint64_t> words,
                                                      std::size_t width,
                                                      std::uint32_t base,
                                                      bool uppercase)
{
    grhsim_task_trunc_words(words, width);
    if (words.empty() || grhsim_task_words_is_zero(words)) {
        return "0";
    }
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    std::string out;
    while (!grhsim_task_words_is_zero(words)) {
        const std::uint32_t rem = grhsim_task_divmod_words(words, base);
        out.push_back(digits[rem]);
    }
    std::reverse(out.begin(), out.end());
    return out;
}

inline std::string grhsim_task_logic_to_base(const grhsim_task_arg &arg,
                                             std::uint32_t base,
                                             bool uppercase)
{
    if (!arg.isWide) {
        if (base == 10u) {
            return std::to_string(grhsim_trunc_u64(arg.scalarValue, arg.width == 0 ? 64u : arg.width));
        }
        if (base == 16u) {
            std::ostringstream out;
            out << std::hex << (uppercase ? std::uppercase : std::nouppercase)
                << grhsim_trunc_u64(arg.scalarValue, arg.width == 0 ? 64u : arg.width);
            return out.str();
        }
        if (base == 8u) {
            std::ostringstream out;
            out << std::oct << grhsim_trunc_u64(arg.scalarValue, arg.width == 0 ? 64u : arg.width);
            return out.str();
        }
        if (base == 2u) {
            const std::size_t width = arg.width == 0 ? 1u : arg.width;
            std::string out;
            out.reserve(width);
            const std::uint64_t value = grhsim_trunc_u64(arg.scalarValue, width >= 64u ? 64u : width);
            for (std::size_t i = 0; i < width; ++i) {
                out.push_back(((value >> (width - i - 1u)) & UINT64_C(1)) != 0 ? '1' : '0');
            }
            const std::size_t pos = out.find_first_not_of('0');
            return pos == std::string::npos ? "0" : out.substr(pos);
        }
        return std::to_string(grhsim_trunc_u64(arg.scalarValue, arg.width == 0 ? 64u : arg.width));
    }
    return grhsim_task_unsigned_words_to_base(arg.words, arg.width, base, uppercase);
}

inline std::string grhsim_task_logic_to_decimal(const grhsim_task_arg &arg, bool signedMode)
{
    if (!signedMode) {
        return grhsim_task_logic_to_base(arg, 10u, false);
    }
    if (!arg.isWide) {
        return std::to_string(grhsim_sign_extend_i64(arg.scalarValue, arg.width == 0 ? 64u : arg.width));
    }
    std::vector<std::uint64_t> words = arg.words;
    grhsim_task_trunc_words(words, arg.width);
    const bool negative = grhsim_task_sign_bit(words, arg.width);
    if (negative) {
        grhsim_task_negate_words(words, arg.width);
    }
    std::string out = grhsim_task_unsigned_words_to_base(words, arg.width, 10u, false);
    if (negative && out != "0") {
        out.insert(out.begin(), '-');
    }
    return out;
}

inline std::string grhsim_task_default_arg_text(const grhsim_task_arg &arg)
{
    switch (arg.kind) {
    case grhsim_task_arg_kind::String:
        return arg.stringValue;
    case grhsim_task_arg_kind::Real: {
        std::ostringstream out;
        out << std::defaultfloat << arg.realValue;
        return out.str();
    }
    case grhsim_task_arg_kind::Logic:
    default:
        return grhsim_task_logic_to_decimal(arg, arg.isSigned);
    }
}

inline std::uint64_t grhsim_task_arg_u64(const grhsim_task_arg &arg)
{
    switch (arg.kind) {
    case grhsim_task_arg_kind::Real:
        return static_cast<std::uint64_t>(arg.realValue);
    case grhsim_task_arg_kind::String:
        return arg.stringValue.empty() ? 0 : static_cast<std::uint64_t>(static_cast<unsigned char>(arg.stringValue.front()));
    case grhsim_task_arg_kind::Logic:
    default:
        return arg.isWide ? (arg.words.empty() ? 0 : arg.words.front())
                          : grhsim_trunc_u64(arg.scalarValue, arg.width == 0 ? 64u : arg.width);
    }
}

inline std::string grhsim_task_apply_width(std::string text,
                                           int width,
                                           bool leftJustify,
                                           bool zeroPad)
{
    if (width <= 0 || static_cast<int>(text.size()) >= width) {
        return text;
    }
    const std::size_t padCount = static_cast<std::size_t>(width - static_cast<int>(text.size()));
    const char pad = zeroPad && !leftJustify ? '0' : ' ';
    if (leftJustify) {
        text.append(padCount, pad);
        return text;
    }
    if (pad == '0' && !text.empty() && text.front() == '-') {
        return std::string("-") + std::string(padCount, '0') + text.substr(1);
    }
    return std::string(padCount, pad) + text;
}

inline std::string grhsim_task_format_one(const grhsim_task_arg &arg,
                                          char spec,
                                          int width,
                                          int precision,
                                          bool leftJustify,
                                          bool zeroPad)
{
    std::string text;
    switch (spec) {
    case 'd':
    case 'i':
        if (arg.kind == grhsim_task_arg_kind::Logic) {
            text = grhsim_task_logic_to_decimal(arg, arg.isSigned);
        }
        else if (arg.kind == grhsim_task_arg_kind::Real) {
            text = std::to_string(static_cast<long long>(arg.realValue));
        }
        else {
            text = grhsim_task_default_arg_text(arg);
        }
        break;
    case 'u':
        text = (arg.kind == grhsim_task_arg_kind::Logic)
                   ? grhsim_task_logic_to_decimal(arg, false)
                   : std::to_string(grhsim_task_arg_u64(arg));
        break;
    case 'h':
    case 'x':
        text = (arg.kind == grhsim_task_arg_kind::Logic)
                   ? grhsim_task_logic_to_base(arg, 16u, false)
                   : grhsim_task_default_arg_text(arg);
        break;
    case 'H':
    case 'X':
        text = (arg.kind == grhsim_task_arg_kind::Logic)
                   ? grhsim_task_logic_to_base(arg, 16u, true)
                   : grhsim_task_default_arg_text(arg);
        break;
    case 'b':
        text = (arg.kind == grhsim_task_arg_kind::Logic)
                   ? grhsim_task_logic_to_base(arg, 2u, false)
                   : grhsim_task_default_arg_text(arg);
        break;
    case 'o':
        text = (arg.kind == grhsim_task_arg_kind::Logic)
                   ? grhsim_task_logic_to_base(arg, 8u, false)
                   : grhsim_task_default_arg_text(arg);
        break;
    case 'c':
        text.assign(1, static_cast<char>(grhsim_task_arg_u64(arg) & UINT64_C(0xff)));
        break;
    case 's':
        text = (arg.kind == grhsim_task_arg_kind::String) ? arg.stringValue : grhsim_task_default_arg_text(arg);
        break;
    case 'e':
    case 'E':
    case 'f':
    case 'F':
    case 'g':
    case 'G': {
        const double value = (arg.kind == grhsim_task_arg_kind::Real)
                                 ? arg.realValue
                                 : static_cast<double>(grhsim_task_arg_u64(arg));
        std::ostringstream out;
        if (precision >= 0) {
            out << std::setprecision(precision);
        }
        switch (spec) {
        case 'e':
            out << std::scientific << std::nouppercase;
            break;
        case 'E':
            out << std::scientific << std::uppercase;
            break;
        case 'f':
            out << std::fixed << std::nouppercase;
            break;
        case 'F':
            out << std::fixed << std::uppercase;
            break;
        case 'G':
            out << std::uppercase;
            [[fallthrough]];
        case 'g':
        default:
            break;
        }
        out << value;
        text = out.str();
        break;
    }
    case 't':
        text = std::to_string(grhsim_task_arg_u64(arg));
        break;
    case 'v':
        text = grhsim_task_default_arg_text(arg);
        break;
    default:
        text = grhsim_task_default_arg_text(arg);
        break;
    }
    return grhsim_task_apply_width(std::move(text), width, leftJustify, zeroPad);
}

inline std::string grhsim_task_format_one(grhsim_scalar_task_arg arg,
                                          char spec,
                                          int width,
                                          int precision,
                                          bool leftJustify,
                                          bool zeroPad)
{
    grhsim_task_arg full;
    full.kind = grhsim_task_arg_kind::Logic;
    full.width = arg.width;
    full.isSigned = arg.isSigned;
    full.isWide = false;
    full.scalarValue = grhsim_trunc_u64(arg.value, arg.width == 0 ? 64u : arg.width);
    return grhsim_task_format_one(full, spec, width, precision, leftJustify, zeroPad);
}

template <typename FormatOne>
inline std::string grhsim_format_task_message_impl(std::string_view fmt,
                                                   std::size_t argCount,
                                                   FormatOne formatOne)
{
    std::string out;
    std::size_t argIndex = 0;
    for (std::size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%') {
            out.push_back(fmt[i]);
            continue;
        }
        if (i + 1u >= fmt.size()) {
            out.push_back('%');
            break;
        }
        if (fmt[i + 1u] == '%') {
            out.push_back('%');
            ++i;
            continue;
        }
        ++i;
        bool leftJustify = false;
        bool zeroPad = false;
        while (i < fmt.size()) {
            if (fmt[i] == '-') {
                leftJustify = true;
                ++i;
                continue;
            }
            if (fmt[i] == '0') {
                zeroPad = true;
                ++i;
                continue;
            }
            break;
        }
        int fieldWidth = 0;
        while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9') {
            fieldWidth = fieldWidth * 10 + static_cast<int>(fmt[i] - '0');
            ++i;
        }
        int precision = -1;
        if (i < fmt.size() && fmt[i] == '.') {
            ++i;
            precision = 0;
            while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9') {
                precision = precision * 10 + static_cast<int>(fmt[i] - '0');
                ++i;
            }
        }
        while (i < fmt.size() && (fmt[i] == 'l' || fmt[i] == 'L' || fmt[i] == 'z')) {
            ++i;
        }
        if (i >= fmt.size()) {
            break;
        }
        const char spec = fmt[i];
        if (spec == 'm') {
            out += "top";
            continue;
        }
        if (argIndex >= argCount) {
            out.push_back('%');
            out.push_back(spec);
            continue;
        }
        out += formatOne(argIndex++, spec, fieldWidth, precision, leftJustify, zeroPad);
    }
    return out;
}

inline std::string grhsim_format_task_message(std::span<const grhsim_task_arg> items)
{
    if (items.empty()) {
        return {};
    }
    if (items.front().kind != grhsim_task_arg_kind::String) {
        std::string out;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (i != 0) {
                out.push_back(' ');
            }
            out += grhsim_task_default_arg_text(items[i]);
        }
        return out;
    }

    const std::string &fmt = items.front().stringValue;
    return grhsim_format_task_message_impl(fmt,
                                           items.size() - 1u,
                                           [&](std::size_t argIndex,
                                               char spec,
                                               int fieldWidth,
                                               int precision,
                                               bool leftJustify,
                                               bool zeroPad)
                                           {
                                               return grhsim_task_format_one(items[argIndex + 1u],
                                                                             spec,
                                                                             fieldWidth,
                                                                             precision,
                                                                             leftJustify,
                                                                             zeroPad);
                                           });
}

inline std::string grhsim_format_task_message(std::initializer_list<grhsim_task_arg> args)
{
    return grhsim_format_task_message(std::span<const grhsim_task_arg>(args.begin(), args.size()));
}

inline std::string grhsim_format_task_message(std::string_view fmt,
                                              std::span<const grhsim_scalar_task_arg> items)
{
    return grhsim_format_task_message_impl(fmt,
                                           items.size(),
                                           [&](std::size_t argIndex,
                                               char spec,
                                               int fieldWidth,
                                               int precision,
                                               bool leftJustify,
                                               bool zeroPad)
                                           {
                                               return grhsim_task_format_one(items[argIndex],
                                                                             spec,
                                                                             fieldWidth,
                                                                             precision,
                                                                             leftJustify,
                                                                             zeroPad);
                                           });
}

inline std::string grhsim_format_task_message(std::string_view fmt,
                                              std::initializer_list<grhsim_scalar_task_arg> args)
{
    return grhsim_format_task_message(fmt, std::span<const grhsim_scalar_task_arg>(args.begin(), args.size()));
}

inline void grhsim_format_scalar_task_message_direct(std::ostream &out, std::string_view fmt, va_list args)
{
    for (std::size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%') {
            out.put(fmt[i]);
            continue;
        }
        if (i + 1u >= fmt.size()) {
            out.put('%');
            break;
        }
        if (fmt[i + 1u] == '%') {
            out.put('%');
            ++i;
            continue;
        }
        ++i;
        bool leftJustify = false;
        bool zeroPad = false;
        while (i < fmt.size()) {
            if (fmt[i] == '-') {
                leftJustify = true;
                ++i;
                continue;
            }
            if (fmt[i] == '0') {
                zeroPad = true;
                ++i;
                continue;
            }
            break;
        }
        int fieldWidth = 0;
        while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9') {
            fieldWidth = fieldWidth * 10 + static_cast<int>(fmt[i] - '0');
            ++i;
        }
        int precision = -1;
        if (i < fmt.size() && fmt[i] == '.') {
            ++i;
            precision = 0;
            while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9') {
                precision = precision * 10 + static_cast<int>(fmt[i] - '0');
                ++i;
            }
        }
        while (i < fmt.size() && (fmt[i] == 'l' || fmt[i] == 'L' || fmt[i] == 'z')) {
            ++i;
        }
        if (i >= fmt.size()) {
            break;
        }
        const char spec = fmt[i];
        if (spec == 'm') {
            out << "top";
            continue;
        }
        const unsigned widthAndSign = va_arg(args, unsigned);
        const std::uint64_t value = va_arg(args, std::uint64_t);
        const grhsim_scalar_task_arg arg{
            value,
            static_cast<std::size_t>(widthAndSign & 0x7fffffffu),
            (widthAndSign & 0x80000000u) != 0};
        out << grhsim_task_format_one(arg, spec, fieldWidth, precision, leftJustify, zeroPad);
    }
}
)CPP";
            }
    }
}
