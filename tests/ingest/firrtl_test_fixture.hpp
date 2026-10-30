#ifndef WOLVRIX_FIRRTL_TEST_FIXTURE_HPP
#define WOLVRIX_FIRRTL_TEST_FIXTURE_HPP

#include "node_test_fixture.hpp"
#include "ingest/firrtl.hpp"
#include "ingest/firrtl_syntax.hpp"
#include "transform/simplify.hpp"

#include <bit>
#include <fstream>
#include <map>

namespace firrtl_test {
using namespace node_test;
namespace ingest = wolvrix::lib::ingest;

inline std::filesystem::path artifact(std::string_view name) {
    auto path = std::filesystem::path(WOLVRIX_FIRRTL_TEST_ARTIFACT_DIR) / name;
    std::filesystem::create_directories(path); return path;
}
inline ingest::IngestResult read(const std::filesystem::path& path, bool annotations = true,
                                ingest::FirrtlInvalidPolicy policy = ingest::FirrtlInvalidPolicy::Zero) {
    ingest::IngestRequest request;
    request.sources.push_back({ingest::SourceLanguage::Firrtl, path, "fixture"});
    request.firrtl.nodeAnnotations = annotations; request.firrtl.invalidPolicy = policy;
    return ingest::readSources(request);
}
inline std::string diagnostics(const ingest::IngestResult& result) {
    std::string text;
    for (const auto& entry : result.diagnostics) text += entry.message + '\n';
    return text;
}
inline grh::Design design(const std::filesystem::path& path, bool annotations = true) {
    auto result = read(path, annotations);
    require(result.success(), diagnostics(result)); return std::move(*result.design);
}
inline ingest::IngestResult text(std::string_view name, std::string_view body,
                                ingest::FirrtlInvalidPolicy policy = ingest::FirrtlInvalidPolicy::Zero) {
    const auto path = artifact("firrtl-fixtures") / (std::string(name) + ".fir");
    std::ofstream(path) << "FIRRTL version 6.0.0\ncircuit top :\n  public module top :\n" << body;
    return read(path, true, policy);
}
inline void error(std::string_view name, std::string_view body, std::string_view message, uint64_t line = 0) {
    auto result = text(name, body);
    require(!result.success() && !result.design, "invalid FIRRTL returned a partial design");
    const auto found = std::find_if(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& entry) { return entry.message.find(message) != std::string::npos; });
    require(found != result.diagnostics.end(), std::string(message) + ": " + diagnostics(result));
    require(found->location && found->location->line && found->location->column && !found->location->sourceLine.empty(), "unlocated semantic error");
    if (line) require(found->location->line == line, "semantic diagnostic has wrong line");
    result.sourceOwners.clear();
    require(!found->location->sourceLine.empty(), "diagnostic snippet borrowed source storage");
}

// A small independent bit-vector interpreter for ordinary GRH, used against
// mathematical FIRRTL expectations. Shaped values are packed low-element first.
class Oracle {
public:
    explicit Oracle(const grh::Graph& graph) : graph_(graph) {}
    uint64_t output(std::string_view name, const std::map<std::string, uint64_t>& inputs) {
        inputs_ = &inputs; cache_.clear(); return eval(graph_.outputPortValue(name));
    }
    static uint64_t mask(int width) { return width >= 64 ? ~uint64_t{0} : (uint64_t{1} << width) - 1; }
private:
    const grh::Graph& graph_;
    const std::map<std::string, uint64_t>* inputs_ = nullptr;
    std::unordered_map<grh::ValueId, uint64_t, grh::ValueIdHash> cache_;
    int64_t signedValue(grh::ValueId id, uint64_t value) const {
        const auto width = graph_.valueWidth(id);
        if (graph_.valueSigned(id) && width < 64 && (value & (uint64_t{1} << (width - 1)))) value |= ~mask(width);
        return static_cast<int64_t>(value);
    }
    uint64_t eval(grh::ValueId id) {
        require(bool(id), "oracle missing output"); require(graph_.valueWidth(id) <= 64, "oracle fixture exceeds 64 bits");
        if (const auto found = cache_.find(id); found != cache_.end()) return found->second;
        if (graph_.valueIsInput(id)) return cache_[id] = inputs_->at(std::string(graph_.symbolText(graph_.valueSymbol(id)))) & mask(graph_.valueWidth(id));
        const auto op = graph_.valueDef(id); require(bool(op), "oracle encountered undriven value");
        const auto args = graph_.opOperands(op); const auto kind = graph_.opKind(op);
        uint64_t result = 0;
        if (kind == OperationKind::kConstant) {
            const auto attrs = graph_.getOperation(op).attr("constValue");
            auto bits = std::get<std::string>(*attrs); const auto quote = bits.find('\'');
            if (quote == std::string::npos) result = std::stoull(bits);
            else {
                auto at = quote + 1; if (bits[at] == 's') ++at;
                const char radix = bits[at++];
                if (bits[at] != 'x' && bits[at] != 'z') result = std::stoull(bits.substr(at), nullptr, radix == 'b' ? 2 : radix == 'h' ? 16 : 10);
            }
        } else if (kind == OperationKind::kMux) result = eval(args[eval(args[0]) ? 1 : 2]);
        else if (kind == OperationKind::kConcat || kind == OperationKind::kArrayCreate) {
            unsigned offset = 0;
            for (std::size_t i = 0; i < args.size(); ++i) {
                const auto arg = kind == OperationKind::kConcat ? args[args.size() - 1 - i] : args[i];
                if (offset < 64) result |= eval(arg) << offset;
                offset += graph_.valueWidth(arg);
            }
        } else {
            const auto a = eval(args[0]), b = args.size() > 1 ? eval(args[1]) : 0;
            const auto sa = signedValue(args[0], a), sb = args.size() > 1 ? signedValue(args[1], b) : 0;
            const bool sign = graph_.valueSigned(args[0]);
            switch (kind) {
            case OperationKind::kAssign: result = a; break;
            case OperationKind::kAdd: result = a + b; break;
            case OperationKind::kSub: result = a - b; break;
            case OperationKind::kMul: result = sign ? static_cast<uint64_t>(sa * sb) : a * b; break;
            case OperationKind::kDiv: result = b ? (sign ? static_cast<uint64_t>(sa / sb) : a / b) : 0; break;
            case OperationKind::kMod: result = b ? (sign ? static_cast<uint64_t>(sa % sb) : a % b) : 0; break;
            case OperationKind::kEq: result = a == b; break;
            case OperationKind::kNe: result = a != b; break;
            case OperationKind::kLt: result = sign ? sa < sb : a < b; break;
            case OperationKind::kLe: result = sign ? sa <= sb : a <= b; break;
            case OperationKind::kGt: result = sign ? sa > sb : a > b; break;
            case OperationKind::kGe: result = sign ? sa >= sb : a >= b; break;
            case OperationKind::kAnd: result = a & b; break;
            case OperationKind::kOr: result = a | b; break;
            case OperationKind::kXor: result = a ^ b; break;
            case OperationKind::kNot: result = ~a; break;
            case OperationKind::kLogicNot: result = !a; break;
            case OperationKind::kLogicAnd: result = a && b; break;
            case OperationKind::kLogicOr: result = a || b; break;
            case OperationKind::kShl: result = b >= 64 ? 0 : a << b; break;
            case OperationKind::kLShr: result = b >= 64 ? 0 : a >> b; break;
            case OperationKind::kAShr: result = static_cast<uint64_t>(sa >> std::min<uint64_t>(b, 63)); break;
            case OperationKind::kReduceAnd: result = a == mask(graph_.valueWidth(args[0])); break;
            case OperationKind::kReduceOr: result = a != 0; break;
            case OperationKind::kReduceXor: result = std::popcount(a) & 1; break;
            case OperationKind::kSliceStatic: result = a >> std::get<int64_t>(*graph_.getOperation(op).attr("sliceStart")); break;
            case OperationKind::kSliceArray: {
                uint64_t offset = 0, stride = graph_.valueWidth(args[0]);
                for (std::size_t i = 1; i < args.size(); ++i) { stride /= graph_.valueShape(args[0])[i - 1]; offset += eval(args[i]) * stride; }
                result = offset >= 64 ? 0 : a >> offset; break;
            }
            case OperationKind::kReplicate: {
                const auto repeat = std::get<int64_t>(*graph_.getOperation(op).attr("rep"));
                for (int64_t i = 0; i < repeat; ++i) result |= a << (i * graph_.valueWidth(args[0])); break;
            }
            default: throw std::runtime_error("oracle unsupported " + std::string(grh::toString(kind)));
            }
        }
        return cache_[id] = result & mask(graph_.valueWidth(id));
    }
};
}
#endif
