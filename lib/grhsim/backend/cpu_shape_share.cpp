#include "grhsim/backend/cpu_shape_share.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace wolvrix::lib::grhsim
{
    namespace
    {
        constexpr std::size_t kNoPos = std::numeric_limits<std::size_t>::max();

        std::string letterEncode(std::size_t value)
        {
            std::string out;
            while (true)
            {
                out.insert(out.begin(), static_cast<char>('A' + (value % 26)));
                if (value < 26)
                    break;
                value = value / 26 - 1;
            }
            return out;
        }

        bool identChar(char c)
        {
            const unsigned char u = static_cast<unsigned char>(c);
            return std::isalnum(u) != 0 || c == '_' || c == '$';
        }

        struct Token
        {
            std::string spelling;
            std::uint64_t value = 0;
            char kind = 'i'; // 'i' int, 'l' long, '8'/'6'/'2'/'4' uint8/16/32/64
        };

        struct ScanResult
        {
            std::string key;        // normalized text with collapsed slots
            std::string norm;       // normalized text with \x01<i>\x01 slots
            std::vector<Token> tokens;
            std::vector<std::string> strings;
        };

        void maskStringsAndComments(std::string &text, std::vector<std::string> &strings)
        {
            std::string out;
            out.reserve(text.size());
            std::size_t i = 0;
            while (i < text.size())
            {
                const char c = text[i];
                if (c == '"')
                {
                    std::size_t j = i + 1;
                    while (j < text.size() && text[j] != '"')
                    {
                        if (text[j] == '\\' && j + 1 < text.size())
                            j += 2;
                        else
                            ++j;
                    }
                    if (j < text.size())
                        ++j;
                    strings.push_back(text.substr(i, j - i));
                    out += '\x00';
                    out += letterEncode(strings.size() - 1);
                    out += '\x00';
                    i = j;
                    continue;
                }
                if (c == '/' && i + 1 < text.size() && text[i + 1] == '/')
                {
                    while (i < text.size() && text[i] != '\n')
                        ++i;
                    continue;
                }
                out += c;
                ++i;
            }
            text = std::move(out);
        }

        void canonicalizeIds(std::string &text)
        {
            static const char *const prefixes[] = {"cpu_cached_value_", "cpu_cached_state_", "cpu_men_"};
            std::unordered_map<std::string, std::string> ids;
            for (const char *prefix : prefixes)
            {
                const std::size_t plen = std::string(prefix).size();
                std::size_t pos = 0;
                while ((pos = text.find(prefix, pos)) != std::string::npos)
                {
                    const std::size_t digitBegin = pos + plen;
                    if (digitBegin >= text.size() || !std::isdigit(static_cast<unsigned char>(text[digitBegin])))
                    {
                        pos = digitBegin;
                        continue;
                    }
                    std::size_t digitEnd = digitBegin;
                    while (digitEnd < text.size() && std::isdigit(static_cast<unsigned char>(text[digitEnd])))
                        ++digitEnd;
                    const bool leftIdent = pos > 0 && identChar(text[pos - 1]);
                    const bool rightIdent = digitEnd < text.size() && identChar(text[digitEnd]);
                    if (leftIdent || rightIdent)
                    {
                        pos = digitEnd;
                        continue;
                    }
                    const std::string original = text.substr(pos, digitEnd - pos);
                    auto [it, inserted] = ids.emplace(original, std::string());
                    if (inserted)
                        it->second = std::string(prefix) + "Q" + letterEncode(ids.size() - 1);
                    text.replace(pos, digitEnd - pos, it->second);
                    pos += it->second.size();
                }
            }
        }

        // Extends spans with [begin,end) ranges of compile-time literal contexts on one line.
        void collectProtectedSpans(const std::string &line, std::vector<std::pair<std::size_t, std::size_t>> &spans)
        {
            const auto protectTo = [&](const std::string &needle, char close) {
                std::size_t pos = 0;
                while ((pos = line.find(needle, pos)) != std::string::npos)
                {
                    const std::size_t end = line.find(close, pos + needle.size());
                    if (end == std::string::npos)
                        break;
                    spans.emplace_back(pos, end + 1);
                    pos = end + 1;
                }
            };
            protectTo("std::array<", '>');
            protectTo("cpu_write_cell<", '>');
            protectTo("grhsim_slice_words<", '>');
            protectTo("cpu_replicate_words_changed<", '>');
            protectTo("alignas(", ')');
            // std::byte cpu_local[N] declaration bound (whitespace-flexible)
            std::size_t pos = 0;
            while ((pos = line.find("std::byte", pos)) != std::string::npos)
            {
                std::size_t cursor = pos + 9;
                while (cursor < line.size() && line[cursor] == ' ')
                    ++cursor;
                if (line.compare(cursor, 9, "cpu_local") == 0)
                {
                    cursor += 9;
                    if (cursor < line.size() && line[cursor] == '[')
                    {
                        const std::size_t end = line.find(']', cursor);
                        if (end != std::string::npos)
                        {
                            spans.emplace_back(pos, end + 1);
                            pos = end + 1;
                            continue;
                        }
                    }
                }
                pos += 9;
            }
            if (line.find("constexpr") != std::string::npos)
                spans.emplace_back(0, line.size());
        }

        bool inSpans(const std::vector<std::pair<std::size_t, std::size_t>> &spans, std::size_t pos)
        {
            for (const auto &[a, b] : spans)
                if (a <= pos && pos < b)
                    return true;
            return false;
        }

        ScanResult scanTaskText(const std::string &raw, const std::string &funcName)
        {
            ScanResult result;
            std::string text = raw;
            maskStringsAndComments(text, result.strings);
            canonicalizeIds(text);
            // task function name -> generic (exactly one definition per file)
            std::size_t namePos = 0;
            while ((namePos = text.find(funcName, namePos)) != std::string::npos)
                text.replace(namePos, funcName.size(), "cpu_task_");

            std::string norm;
            norm.reserve(text.size());
            std::size_t lineBegin = 0;
            while (lineBegin <= text.size())
            {
                const std::size_t nl = text.find('\n', lineBegin);
                const std::size_t lineEnd = nl == std::string::npos ? text.size() : nl;
                const std::string line = text.substr(lineBegin, lineEnd - lineBegin);
                std::vector<std::pair<std::size_t, std::size_t>> spans;
                collectProtectedSpans(line, spans);
                std::sort(spans.begin(), spans.end());

                std::size_t i = 0;
                while (i < line.size())
                {
                    if (inSpans(spans, i))
                    {
                        std::size_t end = i;
                        for (const auto &[a, b] : spans)
                            if (a <= i && i < b)
                                end = std::max(end, b);
                        norm += line.substr(i, end - i);
                        i = end;
                        continue;
                    }
                    // UINT(8|16|32|64)_C(digits)
                    if (line.compare(i, 4, "UINT") == 0)
                    {
                        std::size_t cursor = i + 4;
                        std::size_t bits = 0;
                        while (cursor < line.size() && std::isdigit(static_cast<unsigned char>(line[cursor])))
                            bits = bits * 10 + static_cast<std::size_t>(line[cursor++] - '0');
                        if ((bits == 8 || bits == 16 || bits == 32 || bits == 64) &&
                            line.compare(cursor, 3, "_C(") == 0)
                        {
                            std::size_t valueBegin = cursor + 3;
                            std::size_t valueEnd = valueBegin;
                            while (valueEnd < line.size() && std::isdigit(static_cast<unsigned char>(line[valueEnd])))
                                ++valueEnd;
                            if (valueEnd > valueBegin && valueEnd < line.size() && line[valueEnd] == ')')
                            {
                                const std::uint64_t value = std::stoull(line.substr(valueBegin, valueEnd - valueBegin));
                                if (bits < 64 && value >= (std::uint64_t{1} << bits))
                                    throw std::runtime_error("UINT_C literal overflows its width in shape scan");
                                Token token;
                                token.spelling = line.substr(i, valueEnd + 1 - i);
                                token.value = value;
                                token.kind = bits == 8 ? '8' : bits == 16 ? '6' : bits == 32 ? '2' : '4';
                                result.tokens.push_back(std::move(token));
                                norm += '\x01' + std::to_string(result.tokens.size() - 1) + '\x01';
                                i = valueEnd + 1;
                                continue;
                            }
                        }
                    }
                    if (std::isdigit(static_cast<unsigned char>(line[i])))
                    {
                        std::size_t end = i;
                        while (end < line.size() && std::isdigit(static_cast<unsigned char>(line[end])))
                            ++end;
                        const char prev = i > 0 ? line[i - 1] : '\0';
                        const char next = end < line.size() ? line[end] : '\0';
                        if (identChar(prev) || identChar(next) || next == '.' || prev == '\x00' || next == '\x00')
                        {
                            norm += line.substr(i, end - i);
                            i = end;
                            continue;
                        }
                        const std::uint64_t value = std::stoull(line.substr(i, end - i));
                        if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                            throw std::runtime_error("decimal literal too large in shape scan: " +
                                                     line.substr(i, end - i) + " in " + funcName + " line: " + line.substr(0, 400));
                        Token token;
                        token.spelling = line.substr(i, end - i);
                        token.value = value;
                        token.kind = value <= 2147483647ull ? 'i' : 'l';
                        result.tokens.push_back(std::move(token));
                        norm += '\x01' + std::to_string(result.tokens.size() - 1) + '\x01';
                        i = end;
                        continue;
                    }
                    norm += line[i];
                    ++i;
                }
                if (nl == std::string::npos)
                    break;
                norm += '\n';
                lineBegin = nl + 1;
            }

            // round-trip verification
            std::string check;
            std::size_t pos = 0;
            while (pos < norm.size())
            {
                if (norm[pos] == '\x01')
                {
                    const std::size_t close = norm.find('\x01', pos + 1);
                    if (close == std::string::npos)
                        throw std::runtime_error("shape scan slot marker unbalanced");
                    const std::size_t slot = static_cast<std::size_t>(
                        std::stoul(norm.substr(pos + 1, close - pos - 1)));
                    check += result.tokens[slot].spelling;
                    pos = close + 1;
                    continue;
                }
                check += norm[pos];
                ++pos;
            }
            if (check != text)
                throw std::runtime_error("shape scan round-trip mismatch");

            std::string key = norm;
            std::size_t kpos = 0;
            while ((kpos = key.find('\x01', kpos)) != std::string::npos)
            {
                const std::size_t close = key.find('\x01', kpos + 1);
                if (close == std::string::npos)
                    break;
                key.replace(kpos, close - kpos, "\x01");
                kpos += 2;
            }
            result.norm = std::move(norm);
            result.key = std::move(key);
            return result;
        }

        const char *castType(char kind)
        {
            switch (kind)
            {
            case 'i': return "int";
            case 'l': return "long";
            case '8': return "std::uint8_t";
            case '6': return "std::uint16_t";
            case '2': return "std::uint32_t";
            default: return "std::uint64_t";
            }
        }

        std::string substituteSlots(const std::string &text, const std::vector<Token> &tokens,
                                    const std::vector<std::size_t> &rank32, const std::vector<std::size_t> &rank64)
        {
            std::string out;
            std::size_t pos = 0;
            while (pos < text.size())
            {
                if (text[pos] == '\x01')
                {
                    const std::size_t close = text.find('\x01', pos + 1);
                    const std::size_t slot = static_cast<std::size_t>(std::stoul(text.substr(pos + 1, close - pos - 1)));
                    if (rank32[slot] != kNoPos)
                        out += "static_cast<" + std::string(castType(tokens[slot].kind)) + ">(cpu_shape_p32[" +
                               std::to_string(rank32[slot]) + "])";
                    else if (rank64[slot] != kNoPos)
                        out += "static_cast<" + std::string(castType(tokens[slot].kind)) + ">(cpu_shape_p64[" +
                               std::to_string(rank64[slot]) + "])";
                    else
                        out += tokens[slot].spelling;
                    pos = close + 1;
                    continue;
                }
                out += text[pos];
                ++pos;
            }
            return out;
        }

        std::string unmaskStrings(std::string text, const std::vector<std::string> &strings)
        {
            std::string out;
            std::size_t pos = 0;
            while (pos < text.size())
            {
                if (text[pos] == '\x00')
                {
                    const std::size_t close = text.find('\x00', pos + 1);
                    if (close == std::string::npos)
                        throw std::runtime_error("shape string sentinel unbalanced");
                    std::size_t index = 0;
                    for (std::size_t k = pos + 1; k < close; ++k)
                        index = index * 26 + static_cast<std::size_t>(text[k] - 'A' + 1);
                    out += strings.at(index - 1);
                    pos = close + 1;
                    continue;
                }
                out += text[pos];
                ++pos;
            }
            return out;
        }

    } // namespace

    ShapeShareResult foldShapeTwins(const std::vector<std::string> &taskTexts,
                                    const std::vector<std::string> &taskFuncNames,
                                    const std::string &className,
                                    std::size_t minSourceBytes)
    {
        if (taskTexts.size() != taskFuncNames.size())
            throw std::runtime_error("shape share: task text/name count mismatch");
        ShapeShareResult result;
        result.taskTexts = taskTexts;

        std::vector<ScanResult> scans;
        scans.reserve(taskTexts.size());
        std::unordered_map<std::string, std::vector<std::size_t>> groups;
        for (std::size_t i = 0; i < taskTexts.size(); ++i)
        {
            if (taskTexts[i].find("::cpu_helper_") != std::string::npos)
            {
                ++result.skippedHelpers;
                scans.emplace_back();
                continue;
            }
            scans.push_back(scanTaskText(taskTexts[i], taskFuncNames[i]));
            groups[scans.back().key].push_back(i);
        }

        struct Candidate
        {
            std::vector<std::size_t> members;
            std::size_t sourceBytes = 0;
        };
        std::vector<Candidate> candidates;
        for (auto &[key, members] : groups)
        {
            if (members.size() < 2)
                continue;
            const ScanResult &host = scans[members.front()];
            const std::size_t ntok = host.tokens.size();
            bool ok = true;
            std::string boiler;
            for (const std::size_t member : members)
            {
                const ScanResult &scan = scans[member];
                if (scan.tokens.size() != ntok)
                {
                    ok = false;
                    break;
                }
                for (std::size_t slot = 0; slot < ntok && ok; ++slot)
                {
                    const char a = host.tokens[slot].kind;
                    const char b = scan.tokens[slot].kind;
                    const bool numeric = (a == 'i' || a == 'l') && (b == 'i' || b == 'l');
                    if (a != b && !numeric)
                        ok = false;
                }
                const std::size_t split = scan.norm.find("::cpu_task_");
                const std::string memberBoiler = scan.norm.substr(0, split);
                if (boiler.empty())
                    boiler = memberBoiler;
                else if (memberBoiler != boiler)
                    ok = false;
            }
            if (!ok)
                continue;
            const std::size_t hostBytes = taskTexts[members.front()].size();
            if (hostBytes < minSourceBytes)
                continue;
            Candidate candidate;
            candidate.members = std::move(members);
            candidate.sourceBytes = hostBytes;
            candidates.push_back(std::move(candidate));
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
            if (a.sourceBytes * a.members.size() != b.sourceBytes * b.members.size())
                return a.sourceBytes * a.members.size() > b.sourceBytes * b.members.size();
            return a.members.front() < b.members.front();
        });

        std::vector<std::string> sharedDefs;
        for (std::size_t gid = 0; gid < candidates.size(); ++gid)
        {
            const Candidate &candidate = candidates[gid];
            const ScanResult &host = scans[candidate.members.front()];
            const std::size_t ntok = host.tokens.size();
            std::vector<bool> varies(ntok, false);
            std::vector<bool> wide(ntok, false);
            for (std::size_t slot = 0; slot < ntok; ++slot)
            {
                std::uint64_t maxValue = 0;
                for (const std::size_t member : candidate.members)
                {
                    const Token &token = scans[member].tokens[slot];
                    if (token.spelling != host.tokens[slot].spelling)
                        varies[slot] = true;
                    maxValue = std::max(maxValue, token.value);
                }
                wide[slot] = maxValue > std::numeric_limits<std::uint32_t>::max();
                if (varies[slot])
                {
                    // widen int/long to fit every instance value
                    char kind = host.tokens[slot].kind;
                    if (kind == 'i' || kind == 'l')
                        for (const std::size_t member : candidate.members)
                            if (scans[member].tokens[slot].value > 2147483647ull)
                                kind = 'l';
                    for (const std::size_t member : candidate.members)
                        scans[member].tokens[slot].kind = kind;
                }
            }
            std::vector<std::size_t> rank32(ntok, kNoPos), rank64(ntok, kNoPos);
            std::size_t count32 = 0, count64 = 0;
            for (std::size_t slot = 0; slot < ntok; ++slot)
            {
                if (!varies[slot])
                    continue;
                if (wide[slot])
                    rank64[slot] = count64++;
                else
                    rank32[slot] = count32++;
            }

            const std::string defPrefix = "void " + className + "::cpu_task_";
            const std::size_t split = host.norm.find(defPrefix);
            if (split == std::string::npos)
                throw std::runtime_error("shape share: task definition prefix missing");
            const std::string boiler = host.norm.substr(0, split);
            std::string func = substituteSlots(host.norm.substr(split), host.tokens, rank32, rank64);
            const std::string marker = "::cpu_task_(){";
            const std::size_t markerPos = func.find(marker);
            if (markerPos == std::string::npos)
                throw std::runtime_error("shape share: task definition marker missing");
            func.replace(markerPos, marker.size(),
                         "::cpu_shape_" + std::to_string(gid) +
                             "(const std::uint32_t *cpu_shape_p32, const std::uint64_t *cpu_shape_p64){");
            func.insert(0, "__attribute__((noinline)) ");
            if (count32 == 0 && count64 == 0)
            {
                const std::size_t brace = func.find("){");
                func.insert(brace + 2, "\n    (void)cpu_shape_p32;(void)cpu_shape_p64;");
            }
            func = unmaskStrings(std::move(func), host.strings);
            sharedDefs.push_back(unmaskStrings(boiler, host.strings) + func);
            result.decls.push_back("    __attribute__((noinline)) void cpu_shape_" + std::to_string(gid) +
                                   "(const std::uint32_t *, const std::uint64_t *);");

            for (const std::size_t member : candidate.members)
            {
                const ScanResult &scan = scans[member];
                std::string values32, values64;
                for (std::size_t slot = 0; slot < ntok; ++slot)
                {
                    if (rank32[slot] != kNoPos)
                    {
                        if (!values32.empty())
                            values32 += ',';
                        values32 += std::to_string(scan.tokens[slot].value);
                    }
                    if (rank64[slot] != kNoPos)
                    {
                        if (!values64.empty())
                            values64 += ',';
                        values64 += std::to_string(scan.tokens[slot].value);
                    }
                    result.varyingSlots += (rank32[slot] != kNoPos || rank64[slot] != kNoPos) ? 1 : 0;
                }
                std::string wrapper = unmaskStrings(boiler, host.strings);
                wrapper += "void " + className + "::" + taskFuncNames[member] + "(){\n";
                if (!values32.empty())
                    wrapper += "    static const std::uint32_t cpu_shape_params32[]={" + values32 + "};\n";
                if (!values64.empty())
                    wrapper += "    static const std::uint64_t cpu_shape_params64[]={" + values64 + "};\n";
                wrapper += "    cpu_shape_" + std::to_string(gid) + "(" +
                           (values32.empty() ? "nullptr" : "cpu_shape_params32") + "," +
                           (values64.empty() ? "nullptr" : "cpu_shape_params64") + ");\n}\n";
                result.taskTexts[member] = std::move(wrapper);
                ++result.instances;
            }
            ++result.groups;
        }

        for (const std::string &def : sharedDefs)
            result.shapesCpp += def;
        result.summary = "shape_twin_groups=" + std::to_string(result.groups) +
                         " shape_twin_instances=" + std::to_string(result.instances) +
                         " shape_twin_varying_slots=" + std::to_string(result.varyingSlots) +
                         " shape_twin_skipped_helpers=" + std::to_string(result.skippedHelpers);
        return result;
    }

} // namespace wolvrix::lib::grhsim
