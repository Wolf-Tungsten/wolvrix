#include "grhsim/backend/cpu_shape_share.hpp"

#include "grhsim/backend/cpu_shape_scan.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace wolvrix::lib::grhsim
{
    namespace
    {
        const std::vector<std::string> kTaskPrefixes = {"cpu_cached_value_", "cpu_cached_state_", "cpu_men_"};
    } // namespace

    ShapeShareResult foldShapeTwins(const std::vector<std::string> &taskTexts,
                                    const std::vector<std::string> &taskFuncNames,
                                    const std::string &className,
                                    std::size_t minSourceBytes)
    {
        using namespace shapescan;
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
            scans.push_back(scanText(taskTexts[i], taskFuncNames[i], kTaskPrefixes));
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
            std::string func = substituteSlots(host.norm.substr(split), host.tokens, rank32, rank64,
                                               "cpu_shape_p32", "cpu_shape_p64");
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
            func = unmaskStrings(std::move(func), host.strings, host.comments);
            sharedDefs.push_back(unmaskStrings(boiler, host.strings, host.comments) + func);
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
                std::string wrapper = unmaskStrings(boiler, host.strings, host.comments);
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
