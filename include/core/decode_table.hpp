#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <functional>
#include <map>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace wolvrix::lib::decode
{
using Words = std::vector<uint64_t>;
struct Row
{
    Words mask, value, output, care;
};
struct Table
{
    bool priority;
    int input, output;
    std::vector<int> fields;
    Words defaultValue, defaultCare;
    std::vector<Row> rows;
};
inline std::vector<std::string> split(std::string_view s, char separator)
{
    std::vector<std::string> out;
    size_t start = 0;
    do
    {
        auto end = s.find(separator, start);
        out.emplace_back(
            s.substr(start, end == s.npos ? s.size() - start : end - start));
        if (end == s.npos)
            break;
        start = end + 1;
    } while (true);
    return out;
}
inline int positive(std::string_view s)
{
    if (s.empty() || s.front() == '0' || s.size() > 8)
        throw std::runtime_error("invalid decode table size");
    int n = 0;
    for (char c : s)
    {
        if (c < '0' || c > '9')
            throw std::runtime_error("invalid decode table integer");
        n = n * 10 + c - '0';
    }
    return n;
}
inline Words fromHex(std::string_view s, int width)
{
    if (s.empty() || s.size() > size_t((width + 3) / 4))
        throw std::runtime_error("decode table literal width");
    Words out((width + 63) / 64, 0);
    for (size_t i = 0; i < s.size(); ++i)
    {
        char c = s[s.size() - 1 - i];
        int v = c >= '0' && c <= '9'   ? c - '0'
                : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                       : -1;
        if (v < 0)
            throw std::runtime_error("decode table hexadecimal literal");
        out[i / 16] |= uint64_t(v) << ((i % 16) * 4);
    }
    if (width % 64 && (out.back() >> (width % 64)))
        throw std::runtime_error("decode table nonzero padding");
    return out;
}
inline bool matches(const Words &in, const Row &r)
{
    for (size_t i = 0; i < r.mask.size(); ++i)
        if ((in[i] & r.mask[i]) != r.value[i])
            return false;
    return true;
}
inline Words evaluate(const Table &t, const Words &in)
{
    Words out((t.output + 63) / 64, 0);
    bool found = false;
    for (const auto &r : t.rows)
        if (matches(in, r))
        {
            found = true;
            for (size_t i = 0; i < out.size(); ++i)
                out[i] |= r.output[i] & r.care[i];
            if (t.priority)
                return out;
        }
    if (!found)
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = t.defaultValue[i] & t.defaultCare[i];
    return out;
}

// A sufficient proof for both priority and merged-constraint tables. Compare
// deterministic outputs, including zero-filled don't-care bits and the default.
inline std::optional<Words> constantOutput(const Table &table) {
    Words value = table.defaultValue;
    for (size_t w = 0; w < value.size(); ++w) {
        value[w] &= table.defaultCare[w];
    }
    for (const auto &row : table.rows) {
        for (size_t w = 0; w < value.size(); ++w) {
            if ((row.output[w] & row.care[w]) != value[w]) {
                return std::nullopt;
            }
        }
    }
    return value;
}
inline Table parse(std::string_view payload)
{
    auto p = split(payload, '|');
    if (p.size() != 8 || p[0] != "WDT1" || (p[1] != "P" && p[1] != "T"))
        throw std::runtime_error("decode table contract version or mode");
    Table t;
    t.priority = p[1] == "P";
    t.input = positive(p[2]);
    t.output = positive(p[3]);
    for (auto &f : split(p[4], ','))
        t.fields.push_back(positive(f));
    if (std::accumulate(t.fields.begin(), t.fields.end(), int64_t(0)) !=
        t.output)
        throw std::runtime_error("decode table field widths");
    t.defaultValue = fromHex(p[5], t.output);
    t.defaultCare = fromHex(p[6], t.output);
    if (!p[7].empty())
        for (auto &text : split(p[7], ';'))
        {
            auto r = split(text, ',');
            if (r.size() != 4)
                throw std::runtime_error("decode table row arity");
            Row row{fromHex(r[0], t.input), fromHex(r[1], t.input),
                    fromHex(r[2], t.output), fromHex(r[3], t.output)};
            for (size_t i = 0; i < row.mask.size(); ++i)
                if (row.value[i] & ~row.mask[i])
                    throw std::runtime_error("decode table key outside mask");
            t.rows.push_back(std::move(row));
        }
    if (!t.priority)
        for (size_t i = 0; i < t.rows.size(); ++i)
            for (size_t j = 0; j < i; ++j)
            {
                const auto &a = t.rows[i];
                const auto &b = t.rows[j];
                bool overlap = true;
                for (size_t w = 0; w < a.mask.size(); ++w)
                    if ((a.value[w] ^ b.value[w]) & a.mask[w] & b.mask[w])
                    {
                        overlap = false;
                        break;
                    }
                if (overlap)
                    for (size_t w = 0; w < a.care.size(); ++w)
                        if ((a.output[w] ^ b.output[w]) & a.care[w] & b.care[w])
                            throw std::runtime_error(
                                "conflicting decode table constraints");
            }
    return t;
}
inline std::string name(std::string_view payload)
{
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : payload)
    {
        h ^= c;
        h *= 1099511628211ull;
    }
    std::ostringstream o;
    o << "grhsim_decode_" << std::hex << h;
    return o.str();
}
inline std::string hex(uint64_t x)
{
    std::ostringstream s;
    s << "0x" << std::hex << x << "ULL";
    return s.str();
}
inline std::string cppType(int width)
{
    return width <= 64 ? "std::uint64_t"
                       : "std::array<std::uint64_t, " +
                             std::to_string((width + 63) / 64) + ">";
}
inline std::string literal(const Words &value, int width)
{
    if (width <= 64)
        return hex(value[0]);
    std::string s = cppType(width) + "{";
    for (size_t i = 0; i < value.size(); ++i)
    {
        if (i)
            s += ",";
        s += hex(value[i]);
    }
    return s + "}";
}
struct Stats
{
    size_t nodes = 0, depth = 0, maxLeaf = 0, storage = 0;
    bool dense = false;
    bool bitset = false;
};
inline Stats emit(std::ostream &out, const Table &t, const std::string &symbol)
{
    Stats stats;
    std::string type = cppType(t.output);
    auto inputWord = [&](size_t w)
    {
        return t.input <= 64 ? std::string("x")
                             : "x[" + std::to_string(w) + "]";
    };
    auto resultWord = [&](size_t w)
    {
        return t.output <= 64 ? std::string("result")
                              : "result[" + std::to_string(w) + "]";
    };
    Words relevant((t.input + 63) / 64, 0);
    for (const auto &r : t.rows)
        for (size_t w = 0; w < relevant.size(); ++w)
            relevant[w] |= r.mask[w];
    std::vector<int> bits;
    for (int b = 0; b < t.input; ++b)
        if ((relevant[b / 64] >> (b % 64)) & 1)
            bits.push_back(b);
    out << "#ifndef WOLVRIX_" << symbol << "\n#define WOLVRIX_" << symbol
        << "\n";
    out << "inline " << type << " " << symbol << "(";
    if (t.input > 64)
        out << "const ";
    out << cppType(t.input);
    if (t.input > 64)
        out << " &";
    out << " x) {\n";
    size_t entries = bits.size() <= 16 ? (size_t(1) << bits.size()) : 0;
    size_t bytes = entries * ((t.output + 63) / 64) * 8;
    if (entries && bytes <= 1024 * 1024)
    {
        stats.dense = true;
        std::map<Words, size_t> intern;
        std::vector<Words> records;
        std::vector<size_t> indices;
        for (size_t k = 0; k < entries; ++k)
        {
            Words x(relevant.size(), 0);
            for (size_t i = 0; i < bits.size(); ++i)
                if ((k >> i) & 1)
                    x[bits[i] / 64] |= uint64_t(1) << (bits[i] % 64);
            auto value = evaluate(t, x);
            auto [it, inserted] = intern.emplace(value, records.size());
            if (inserted)
                records.push_back(std::move(value));
            indices.push_back(it->second);
        }
        if (t.output == 1 && entries <= 64) {
            // A whole Boolean truth table fits in one register. Clang may widen
            // an inlined byte load because BT ignores upper index bits. Keep the
            // full scalar value in a register before extracting the lookup index.
            uint64_t bitmap = 0;
            for (size_t k = 0; k < entries; ++k) {
                bitmap |= records[indices[k]][0] << k;
            }
            if (t.input < 32) {
                out << "#if defined(__clang__) && defined(__x86_64__)\n"
                    << "  // Pure register value barrier: no volatile access or memory clobber.\n"
                    << "  __asm__(\"\" : \"+r\"(x));\n#endif\n";
            }
            out << "  std::size_t index = 0;\n";
            for (size_t i = 0; i < bits.size(); ++i) {
                out << "  index |= ((" << inputWord(bits[i] / 64) << " >> "
                    << bits[i] % 64 << ") & 1ULL) << " << i << ";\n";
            }
            out << "  return (" << hex(bitmap) << " >> index) & 1ULL;\n}\n";
            stats.storage = sizeof(uint64_t);
            stats.bitset = true;
            out << "// decode-stats " << symbol << " dense=1 nodes=0 depth=0 max_leaf=0 storage="
                << stats.storage << " bitset=1\n#endif\n";
            return stats;
        }
        const auto indexBytes = records.size() <= 256     ? 1
                                : records.size() <= 65536 ? 2
                                                          : 4;
        stats.storage = records.size() * ((t.output + 63) / 64) * 8 +
                        indices.size() * indexBytes;
        out << "  static constexpr " << type << " values[" << records.size()
            << "] = {\n";
        for (const auto &value : records)
            out << literal(value, t.output) << ",\n";
        out << "  };\n  static constexpr std::uint" << indexBytes * 8
            << "_t indices[" << entries << "] = {\n";
        for (size_t i = 0; i < indices.size(); ++i)
            out << indices[i] << "," << (i % 32 == 31 ? "\n" : "");
        out << "  };\n  std::size_t index = 0;\n";
        for (size_t i = 0; i < bits.size(); ++i)
            out << "  index |= ((" << inputWord(bits[i] / 64) << " >> "
                << bits[i] % 64 << ") & 1ULL) << " << i << ";\n";
        out << "  return values[indices[index]];\n}\n";
    }
    else
    {
        Words def = t.defaultValue;
        for (size_t i = 0; i < def.size(); ++i)
            def[i] &= t.defaultCare[i];
        out << "  " << type << " result{};\n  bool matched = false;\n";
        std::vector<size_t> indices(t.rows.size());
        std::iota(indices.begin(), indices.end(), 0);
        std::function<void(const std::vector<size_t> &, const Words &, size_t)>
            tree;
        tree = [&](const std::vector<size_t> &ids, const Words &remaining,
                   size_t depth)
        {
            ++stats.nodes;
            stats.depth = std::max(stats.depth, depth);
            Words common = remaining;
            for (size_t i : ids)
                for (size_t w = 0; w < common.size(); ++w)
                    common[w] &= t.rows[i].mask[w];
            bool any = std::any_of(common.begin(), common.end(),
                                   [](uint64_t x) { return x != 0; });
            if (ids.size() > 4 && any)
            {
                std::map<Words, std::vector<size_t>> groups;
                for (size_t i : ids)
                {
                    Words key = common;
                    for (size_t w = 0; w < key.size(); ++w)
                        key[w] &= t.rows[i].value[w];
                    groups[key].push_back(i);
                }
                Words next = remaining;
                for (size_t w = 0; w < next.size(); ++w)
                    next[w] &= ~common[w];
                bool firstGroup = true;
                for (const auto &[key, group] : groups)
                {
                    out << (firstGroup ? "  if (" : "  else if (");
                    firstGroup = false;
                    bool first = true;
                    for (size_t w = 0; w < key.size(); ++w)
                        if (common[w])
                        {
                            if (!first)
                                out << " && ";
                            first = false;
                            out << "(" << inputWord(w) << " & "
                                << hex(common[w]) << ") == " << hex(key[w]);
                        }
                    out << ") {\n";
                    tree(group, next, depth + 1);
                    out << "  }\n";
                }
            }
            else
            {
                stats.maxLeaf = std::max(stats.maxLeaf, ids.size());
                for (size_t i : ids)
                {
                    const auto &r = t.rows[i];
                    out << "  if (true";
                    for (size_t w = 0; w < remaining.size(); ++w)
                        if (r.mask[w] & remaining[w])
                            out << " && (" << inputWord(w) << " & "
                                << hex(r.mask[w] & remaining[w])
                                << ") == " << hex(r.value[w] & remaining[w]);
                    out << ") {\n";
                    if (t.priority)
                    {
                        Words v = r.output;
                        for (size_t w = 0; w < v.size(); ++w)
                            v[w] &= r.care[w];
                        out << "    return " << literal(v, t.output) << ";\n";
                    }
                    else
                    {
                        out << "    matched = true;\n";
                        for (size_t w = 0; w < r.output.size(); ++w)
                            if (r.output[w] & r.care[w])
                                out << "    " << resultWord(w)
                                    << " |= " << hex(r.output[w] & r.care[w])
                                    << ";\n";
                    }
                    out << "  }\n";
                }
            }
        };
        tree(indices, relevant, 0);
        out << "  return " << (t.priority ? "" : "matched ? result : ")
            << literal(def, t.output) << ";\n}\n";
    }
    out << "// decode-stats " << symbol << " dense=" << stats.dense
        << " nodes=" << stats.nodes << " depth=" << stats.depth
        << " max_leaf=" << stats.maxLeaf << " storage=" << stats.storage
        << "\n";
    out << "#endif\n";
    return stats;
}
} // namespace wolvrix::lib::decode
