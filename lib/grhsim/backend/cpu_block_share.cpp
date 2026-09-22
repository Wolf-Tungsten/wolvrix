#include "grhsim/backend/cpu_block_share.hpp"

#include "grhsim/backend/cpu_shape_scan.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace wolvrix::lib::grhsim
{
    namespace
    {
        const std::vector<std::string> kBlockPrefixes = {"cpu_cached_value_", "cpu_cached_state_", "cpu_men_",
                                                         "cpu_changed_"};
        constexpr std::size_t kChunkTargetBytes = 12 * 1024 * 1024;

        struct Block
        {
            std::size_t task = 0;
            std::size_t openBrace = 0;  // index of '{' in the task text
            std::size_t closeBrace = 0; // index of matching '}'
            std::size_t bodyBegin = 0;  // past leading blanks and the bit-clear line
            std::string bit;            // guard bit digits (for re-emitting the bit clear)
            bool hasBitclear = false;
        };

        bool wordAt(const std::string &text, std::size_t pos, const std::string &word)
        {
            if (pos + word.size() > text.size() || text.compare(pos, word.size(), word) != 0)
                return false;
            const bool left = pos > 0 && shapescan::identChar(text[pos - 1]);
            const bool right = pos + word.size() < text.size() && shapescan::identChar(text[pos + word.size()]);
            return !left && !right;
        }

        bool containsWord(const std::string &text, const std::string &word)
        {
            std::size_t pos = 0;
            while ((pos = text.find(word, pos)) != std::string::npos)
            {
                if (wordAt(text, pos, word))
                    return true;
                pos += word.size();
            }
            return false;
        }

        // String/comment aware brace matcher; returns index of the matching '}'.
        std::size_t matchBrace(const std::string &text, std::size_t openIdx)
        {
            std::size_t depth = 0;
            bool inStr = false, inComment = false;
            for (std::size_t i = openIdx; i < text.size(); ++i)
            {
                const char c = text[i];
                if (inComment)
                {
                    if (c == '\n')
                        inComment = false;
                    continue;
                }
                if (inStr)
                {
                    if (c == '\\' && i + 1 < text.size())
                        ++i;
                    else if (c == '"')
                        inStr = false;
                    continue;
                }
                if (c == '/' && i + 1 < text.size() && text[i + 1] == '/')
                    inComment = true;
                else if (c == '"')
                    inStr = true;
                else if (c == '{')
                    ++depth;
                else if (c == '}' && --depth == 0)
                    return i;
            }
            return std::string::npos;
        }

        // Collects guard blocks `if(cpu_active_word&<digits>){...}` inside the
        // task function body, skipping nested guards and guards inside loops.
        std::vector<Block> extractBlocks(const std::string &text, std::size_t taskIndex,
                                         const std::string &funcMarker)
        {
            std::vector<Block> blocks;
            const std::size_t funcPos = text.find(funcMarker);
            if (funcPos == std::string::npos)
                return blocks;
            const std::size_t funcBrace = text.find('{', funcPos + funcMarker.size());
            if (funcBrace == std::string::npos)
                return blocks;
            const std::size_t funcEnd = matchBrace(text, funcBrace);
            if (funcEnd == std::string::npos)
                return blocks;

            std::vector<bool> loopScopes;
            bool pendingLoop = false;
            bool inStr = false, inComment = false;
            int parenDepth = 0;
            const std::string guard = "if(cpu_active_word&";
            std::size_t i = funcBrace;
            while (i < funcEnd)
            {
                const char c = text[i];
                if (inComment)
                {
                    if (c == '\n')
                        inComment = false;
                    ++i;
                    continue;
                }
                if (inStr)
                {
                    if (c == '\\' && i + 1 < funcEnd)
                        i += 2;
                    else if (c == '"')
                    {
                        inStr = false;
                        ++i;
                    }
                    else
                        ++i;
                    continue;
                }
                if (c == '/' && i + 1 < funcEnd && text[i + 1] == '/')
                {
                    inComment = true;
                    i += 2;
                    continue;
                }
                if (c == '"')
                {
                    inStr = true;
                    ++i;
                    continue;
                }
                if (c == '(')
                {
                    ++parenDepth;
                    ++i;
                    continue;
                }
                if (c == ')')
                {
                    if (parenDepth > 0)
                        --parenDepth;
                    ++i;
                    continue;
                }
                if (c == '{')
                {
                    loopScopes.push_back(pendingLoop);
                    pendingLoop = false;
                    ++i;
                    continue;
                }
                if (c == '}')
                {
                    if (!loopScopes.empty())
                        loopScopes.pop_back();
                    ++i;
                    continue;
                }
                if (c == ';')
                {
                    if (parenDepth == 0)
                        pendingLoop = false;
                    ++i;
                    continue;
                }
                if (shapescan::identChar(c))
                {
                    std::size_t wordEnd = i;
                    while (wordEnd < funcEnd && shapescan::identChar(text[wordEnd]))
                        ++wordEnd;
                    const std::size_t wordBegin = i;
                    i = wordEnd;
                    const std::string word = text.substr(wordBegin, wordEnd - wordBegin);
                    if (word == "for" || word == "while" || word == "do")
                        pendingLoop = true;
                    else if (word == "if" && text.compare(wordBegin, guard.size(), guard) == 0)
                    {
                        std::size_t cursor = wordBegin + guard.size();
                        std::size_t bitEnd = cursor;
                        while (bitEnd < funcEnd && std::isdigit(static_cast<unsigned char>(text[bitEnd])))
                            ++bitEnd;
                        if (bitEnd == cursor || text.compare(bitEnd, 2, "){") != 0)
                            continue;
                        const std::size_t openBrace = bitEnd + 1;
                        const std::size_t closeBrace = matchBrace(text, openBrace);
                        if (closeBrace == std::string::npos || closeBrace > funcEnd)
                            break;
                        const bool inLoop = std::find(loopScopes.begin(), loopScopes.end(), true) != loopScopes.end();
                        if (!inLoop)
                        {
                            Block block;
                            block.task = taskIndex;
                            block.openBrace = openBrace;
                            block.closeBrace = closeBrace;
                            block.bit = text.substr(cursor, bitEnd - cursor);
                            std::size_t bodyBegin = openBrace + 1;
                            while (bodyBegin < closeBrace &&
                                   (text[bodyBegin] == ' ' || text[bodyBegin] == '\n' || text[bodyBegin] == '\t'))
                                ++bodyBegin;
                            if (text.compare(bodyBegin, 16, "cpu_active_word&") == 0)
                            {
                                std::size_t clearEnd = bodyBegin + 16;
                                while (clearEnd < closeBrace && text[clearEnd] != ';')
                                    ++clearEnd;
                                if (clearEnd < closeBrace &&
                                    text.compare(bodyBegin, 17, "cpu_active_word&=") == 0)
                                {
                                    block.hasBitclear = true;
                                    bodyBegin = clearEnd + 1;
                                }
                            }
                            block.bodyBegin = bodyBegin;
                            blocks.push_back(std::move(block));
                        }
                        i = closeBrace + 1; // nested guards fold with the outer body
                        continue;
                    }
                    continue;
                }
                ++i;
            }
            return blocks;
        }

        bool isSafeExternalId(const std::string &id)
        {
            static const std::unordered_set<std::string> kSafe = {
                "cpu_at",       "cpu_flags",  "cpu_pflags", "cpu_read_offsets", "cpu_active_word",
                "cpu_obj_",     "cpu_bnd_",   "cpu_shadow_", "cpu_objects",     "cpu_boundary",
                "cpu_shadow",   "cpu_write_cell"};
            if (kSafe.count(id) != 0)
                return true;
            return id.compare(0, 11, "cpu_helper_") == 0;
        }

        // Every used cpu_ identifier must be declared inside the body or be a
        // known member/helper; anything else makes the block ineligible.
        bool undeclaredIdFree(const std::string &norm)
        {
            static const std::vector<std::string> kTypeKeywords = {
                "const auto ", "auto &",     "auto ",         "bool ",          "std::byte ",
                "std::size_t ", "std::uint8_t ", "std::uint16_t ", "std::uint32_t ", "std::uint64_t "};
            std::unordered_set<std::string> declared;
            for (const std::string &ty : kTypeKeywords)
            {
                std::size_t pos = 0;
                while ((pos = norm.find(ty, pos)) != std::string::npos)
                {
                    std::size_t cursor = pos + ty.size();
                    while (cursor < norm.size() && norm[cursor] == ' ')
                        ++cursor;
                    std::size_t idEnd = cursor;
                    while (idEnd < norm.size() && shapescan::identChar(norm[idEnd]))
                        ++idEnd;
                    if (idEnd > cursor)
                        declared.insert(norm.substr(cursor, idEnd - cursor));
                    pos = idEnd;
                }
            }
            std::size_t arrayPos = 0;
            while ((arrayPos = norm.find("std::array<", arrayPos)) != std::string::npos)
            {
                const std::size_t close = norm.find('>', arrayPos);
                if (close == std::string::npos)
                    break;
                std::size_t cursor = close + 1;
                while (cursor < norm.size() && norm[cursor] == ' ')
                    ++cursor;
                std::size_t idEnd = cursor;
                while (idEnd < norm.size() && shapescan::identChar(norm[idEnd]))
                    ++idEnd;
                if (idEnd > cursor)
                    declared.insert(norm.substr(cursor, idEnd - cursor));
                arrayPos = close + 1;
            }

            std::size_t i = 0;
            while (i < norm.size())
            {
                if (norm[i] == '\x02')
                {
                    // Comment sentinel: skip to the closing marker.
                    const std::size_t close = norm.find('\x02', i + 1);
                    if (close == std::string::npos)
                        break;
                    i = close + 1;
                    continue;
                }
                if (shapescan::identChar(norm[i]) && !std::isdigit(static_cast<unsigned char>(norm[i])) &&
                    (i == 0 || !shapescan::identChar(norm[i - 1])) && norm[i] != '\x01')
                {
                    std::size_t idEnd = i;
                    while (idEnd < norm.size() && shapescan::identChar(norm[idEnd]))
                        ++idEnd;
                    const std::string id = norm.substr(i, idEnd - i);
                    i = idEnd;
                    if (id.compare(0, 4, "cpu_") != 0)
                        continue;
                    if (declared.count(id) != 0 || isSafeExternalId(id))
                        continue;
                    return false;
                }
                ++i;
            }
            return true;
        }

    } // namespace

    std::unordered_map<std::string, double> loadTaskHotnessFile(const std::filesystem::path &path)
    {
        std::ifstream input(path);
        if (!input)
            throw std::runtime_error("block share: cannot open hotness file " + path.string());
        std::unordered_map<std::string, double> hotness;
        std::string line;
        while (std::getline(input, line))
        {
            const std::size_t tab = line.find('\t');
            if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size())
                continue;
            try
            {
                hotness[line.substr(0, tab)] = std::stod(line.substr(tab + 1));
            }
            catch (const std::exception &)
            {
                continue;
            }
        }
        return hotness;
    }

    BlockShareResult foldBranchBlocks(const std::vector<std::string> &taskTexts,
                                      const std::vector<std::string> &taskFuncNames,
                                      const std::string &className,
                                      const BlockShareOptions &options)
    {
        using namespace shapescan;
        const std::size_t minSourceBytes = options.minSourceBytes;
        if (taskTexts.size() != taskFuncNames.size())
            throw std::runtime_error("block share: task text/name count mismatch");
        BlockShareResult result;
        result.taskTexts = taskTexts;

        std::vector<ScanResult> scans;
        std::vector<Block> blocks;
        std::vector<bool> eligible;
        std::unordered_map<std::string, std::vector<std::size_t>> groups;
        for (std::size_t i = 0; i < taskTexts.size(); ++i)
        {
            if (taskTexts[i].find("cpu_active_word&") == std::string::npos)
                continue;
            const std::string funcMarker = "void " + className + "::" + taskFuncNames[i] + "(){";
            for (const Block &block : extractBlocks(taskTexts[i], i, funcMarker))
            {
                const std::string body = taskTexts[i].substr(block.bodyBegin, block.closeBrace - block.bodyBegin);
                ScanResult scan = scanText(body, "", kBlockPrefixes);
                bool ok = !containsWord(scan.norm, "return") && !containsWord(scan.norm, "goto") &&
                          !containsWord(scan.norm, "break") && !containsWord(scan.norm, "continue") &&
                          scan.norm.find("cpu_shape_p") == std::string::npos;
                if (ok)
                    ok = undeclaredIdFree(scan.norm);
                scans.push_back(std::move(scan));
                blocks.push_back(block);
                eligible.push_back(ok);
                if (ok)
                    groups[scans.back().key].push_back(blocks.size() - 1);
                else
                    ++result.skippedBlocks;
            }
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
            }
            if (!ok)
                continue;
            const Block &hostBlock = blocks[members.front()];
            const std::size_t hostBytes = hostBlock.closeBrace - hostBlock.bodyBegin;
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

        // Hotness-guided greedy exclusion (cold outlining): estimate each
        // candidate's parameter-load growth as hotness% x loadBloat and drop
        // the worst growth-per-saved-byte groups until the total fits the
        // budget. Group hotness sums member task sample percents, which
        // overestimates block share for multi-block tasks; that direction is
        // conservative (excludes more).
        if (options.taskHotness && options.growthBudget >= 0.0)
        {
            constexpr double kSrcToText = 0.105;   // shared-body .text bytes per source byte (nm-calibrated)
            constexpr double kBytesPerInstr = 4.7; // x86-64 average inside shared bodies
            struct Metric
            {
                double growth = 0.0;
                double ratio = 0.0;
                std::size_t index = 0;
            };
            std::vector<Metric> metrics;
            metrics.reserve(candidates.size());
            double totalGrowth = 0.0;
            for (std::size_t index = 0; index < candidates.size(); ++index)
            {
                const Candidate &candidate = candidates[index];
                const ScanResult &host = scans[candidate.members.front()];
                const std::size_t ntok = host.tokens.size();
                std::size_t distinct = 0;
                for (std::size_t slot = 0; slot < ntok; ++slot)
                {
                    for (const std::size_t member : candidate.members)
                    {
                        if (scans[member].tokens[slot].spelling != host.tokens[slot].spelling)
                        {
                            ++distinct;
                            break;
                        }
                    }
                }
                double hotness = 0.0;
                for (const std::size_t member : candidate.members)
                {
                    const auto it = options.taskHotness->find(taskFuncNames[blocks[member].task]);
                    if (it != options.taskHotness->end())
                        hotness += it->second;
                }
                const double instr =
                    std::max(static_cast<double>(candidate.sourceBytes) * kSrcToText / kBytesPerInstr, 1.0);
                const double bloat =
                    static_cast<double>(distinct) / std::max(instr - static_cast<double>(distinct), 1.0);
                const double saved = static_cast<double>(candidate.sourceBytes) *
                                     static_cast<double>(candidate.members.size() - 1);
                Metric metric;
                metric.growth = hotness * bloat;
                metric.ratio = metric.growth / std::max(saved, 1.0);
                metric.index = index;
                totalGrowth += metric.growth;
                metrics.push_back(metric);
            }
            std::sort(metrics.begin(), metrics.end(), [](const Metric &a, const Metric &b) {
                if (a.ratio != b.ratio)
                    return a.ratio > b.ratio;
                return a.index < b.index;
            });
            std::vector<bool> excluded(candidates.size(), false);
            double growth = totalGrowth;
            for (const Metric &metric : metrics)
            {
                if (growth <= options.growthBudget)
                    break;
                excluded[metric.index] = true;
                growth -= metric.growth;
                ++result.excludedGroups;
            }
            result.estimatedGrowth = growth;
            std::vector<Candidate> selected;
            selected.reserve(candidates.size() - result.excludedGroups);
            for (std::size_t index = 0; index < candidates.size(); ++index)
                if (!excluded[index])
                    selected.push_back(std::move(candidates[index]));
            candidates = std::move(selected);
        }

        struct Replacement
        {
            std::size_t begin = 0;
            std::size_t end = 0;
            std::string text;
        };
        std::vector<std::vector<Replacement>> replacements(taskTexts.size());
        std::string boiler;
        std::vector<std::string> helperDefs;
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

            const bool usesWord = containsWord(host.norm, "cpu_active_word");
            const std::string wordParam = usesWord ? ", std::uint8_t &cpu_active_word" : "";
            const std::string wordArg = usesWord ? ",cpu_active_word" : "";
            std::string helper = "__attribute__((noinline)) void " + className + "::cpu_blk_" +
                                 std::to_string(gid) +
                                 "(const std::uint32_t *cpu_blk_p32, const std::uint64_t *cpu_blk_p64" +
                                 wordParam + "){\n"
                                 "    [[maybe_unused]] std::byte *__restrict const cpu_obj_=cpu_objects.get();\n"
                                 "    [[maybe_unused]] std::byte *__restrict const cpu_bnd_=cpu_boundary.get();\n"
                                 "    [[maybe_unused]] std::byte *__restrict const cpu_shadow_=cpu_shadow.get();\n";
            if (count32 == 0 && count64 == 0)
                helper += "    (void)cpu_blk_p32;(void)cpu_blk_p64;\n";
            helper += substituteSlots(host.norm, host.tokens, rank32, rank64, "cpu_blk_p32", "cpu_blk_p64");
            helper += "}\n";
            helperDefs.push_back(unmaskStrings(std::move(helper), host.strings, host.comments));
            result.decls.push_back("    __attribute__((noinline)) void cpu_blk_" + std::to_string(gid) +
                                   "(const std::uint32_t *, const std::uint64_t *" + wordParam + ");");

            for (const std::size_t member : candidate.members)
            {
                const ScanResult &scan = scans[member];
                const Block &block = blocks[member];
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
                if (boiler.empty())
                {
                    const std::string &memberText = taskTexts[block.task];
                    const std::size_t split = memberText.find("void " + className + "::");
                    if (split == std::string::npos)
                        throw std::runtime_error("block share: task boiler prefix missing");
                    boiler = memberText.substr(0, split);
                }
                const std::string &memberText = taskTexts[block.task];
                const std::size_t guardPos = memberText.rfind("if(cpu_active_word&", block.openBrace);
                const std::size_t lineBegin = memberText.rfind('\n', block.openBrace);
                const std::size_t lineStart = lineBegin == std::string::npos ? 0 : lineBegin + 1;
                const std::string indent = guardPos == std::string::npos || guardPos < lineStart
                                               ? std::string()
                                               : memberText.substr(lineStart, guardPos - lineStart);
                const std::string inner = indent + "    ";
                std::string call = "{\n";
                if (block.hasBitclear)
                    call += inner + "cpu_active_word&=~" + block.bit + ";\n";
                if (!values32.empty())
                    call += inner + "static const std::uint32_t cpu_blk_params32[]={" + values32 + "};\n";
                if (!values64.empty())
                    call += inner + "static const std::uint64_t cpu_blk_params64[]={" + values64 + "};\n";
                call += inner + "cpu_blk_" + std::to_string(gid) + "(" +
                        (values32.empty() ? "nullptr" : "cpu_blk_params32") + "," +
                        (values64.empty() ? "nullptr" : "cpu_blk_params64") + wordArg + ");\n";
                call += indent + "}";
                Replacement replacement;
                replacement.begin = block.openBrace;
                replacement.end = block.closeBrace + 1;
                replacement.text = std::move(call);
                replacements[block.task].push_back(std::move(replacement));
                result.foldedSourceBytes += block.closeBrace - block.bodyBegin;
                ++result.instances;
            }
            ++result.groups;
        }

        for (std::size_t i = 0; i < taskTexts.size(); ++i)
        {
            std::vector<Replacement> &per = replacements[i];
            if (per.empty())
                continue;
            std::sort(per.begin(), per.end(), [](const Replacement &a, const Replacement &b) {
                return a.begin > b.begin;
            });
            std::string &text = result.taskTexts[i];
            for (const Replacement &replacement : per)
                text.replace(replacement.begin, replacement.end - replacement.begin, replacement.text);
        }

        if (!helperDefs.empty())
        {
            std::string chunk = boiler;
            for (std::string &def : helperDefs)
            {
                if (chunk.size() > kChunkTargetBytes)
                {
                    result.blocksChunks.push_back(std::move(chunk));
                    chunk = boiler;
                }
                chunk += def;
            }
            result.blocksChunks.push_back(std::move(chunk));
        }
        result.summary = "branch_block_groups=" + std::to_string(result.groups) +
                         " branch_block_instances=" + std::to_string(result.instances) +
                         " branch_block_varying_slots=" + std::to_string(result.varyingSlots) +
                         " branch_block_skipped=" + std::to_string(result.skippedBlocks) +
                         " branch_block_excluded_groups=" + std::to_string(result.excludedGroups) +
                         " branch_block_est_growth=" + std::to_string(result.estimatedGrowth) +
                         " branch_block_folded_source_bytes=" + std::to_string(result.foldedSourceBytes);
        return result;
    }

} // namespace wolvrix::lib::grhsim
