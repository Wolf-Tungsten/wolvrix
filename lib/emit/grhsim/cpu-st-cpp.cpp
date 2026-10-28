#include "emit/grhsim/cpu-st-cpp.hpp"

#include "function-writer.hpp"
#include "json-writer.hpp"
#include "emit/readmem.hpp"
#include "core/op_descriptor.hpp"
#include "core/state.hpp"
#include "core/store.hpp"
#include "transform/grhsim/ops.hpp"
#include "transform/grhsim/sink-schedule.hpp"
#include "slang/numeric/SVInt.h"
#include "slang/text/Json.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace wolvrix::lib::emit {
namespace {
namespace sim = transform::grhsim;
using namespace grhsim_detail;
using grh::OperationId;
using grh::OperationKind;
using grh::ValueId;
using grh::LogicalType;
using Key = sim::PartitionKey;
const std::vector<std::string> namedStores{
    "smallSinkStore", "largeSinkStore", "inputStore", "nonSinkBoundaryStore",
    "smallSinkEnableBoundaryStore", "smallSinkPayloadBoundaryStore",
    "largeSinkEnableBoundaryStore", "largeSinkPayloadBoundaryStore",
    "prevEventStore", "eventActStore", "timeslotTriggerStore", "sampledStore",
    "onceStore", "largeBeforeStore", "largeWholeBeforeStore",
    "largeWholeDirtyStore", "memoryReadIndexStore", "memoryTouchedStore"};

std::string identifier(std::string_view text) {
    std::string result;
    for (const unsigned char ch : text) result += std::isalnum(ch) || ch == '_' ? char(ch) : '_';
    if (result.empty() || std::isdigit(static_cast<unsigned char>(result.front()))) result.insert(0, "n_");
    return result;
}
std::string quote(std::string_view text) {
    std::ostringstream out; out << '"';
    for (const unsigned char ch : text) {
        if (ch == '\n') out << "\\n";
        else if (ch == '\r') out << "\\r";
        else if (ch == '\t') out << "\\t";
        else if (ch == '"' || ch == '\\') out << '\\' << char(ch);
        else if (ch < 32 || ch >= 127 || ch == '@') out << '\\' << std::oct << std::setw(3) << std::setfill('0') << unsigned(ch) << std::dec;
        else out << char(ch);
    }
    return out.str() + '"';
}
std::size_t option(const EmitOptions& options, std::string_view name, std::size_t fallback) {
    const auto found = options.attributes.find(name);
    if (found == options.attributes.end()) return fallback;
    std::size_t result = 0;
    const auto [end, error] = std::from_chars(found->second.data(), found->second.data() + found->second.size(), result);
    if (error != std::errc{} || end != found->second.data() + found->second.size() || !result)
        throw std::invalid_argument("Invalid emit option " + std::string(name) + ": " + found->second);
    return result;
}
bool enabledOption(const EmitOptions& options, std::string_view name, bool fallback = true) {
    const auto found = options.attributes.find(name);
    if (found == options.attributes.end()) return fallback;
    if (found->second != "on" && found->second != "off")
        throw std::invalid_argument(std::string(name) + " must be on or off");
    return found->second == "on";
}
std::string readableIdentifier(std::string_view text) {
    std::string result;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char ch : text) {
        hash = (hash ^ ch) * UINT64_C(1099511628211);
        const bool ascii = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
        if (ascii) result += static_cast<char>(ch);
        else if (!result.empty() && result.back() != '_') result += '_';
    }
    if (!result.empty() && result.back() == '_') result.pop_back();
    if (result.empty()) result = "anonymous";
    if (result.size() > 88) {
        std::ostringstream suffix; suffix << std::hex << hash;
        result = result.substr(0, 38) + "_" + result.substr(result.size() - 28) + "_h" + suffix.str();
    }
    return result;
}
struct OperationScope {
    OperationId& current;
    OperationId previous;
    OperationScope(OperationId& current, OperationId op) : current(current), previous(current) { current = op; }
    ~OperationScope() { current = previous; }
};
struct CodeOrigin {
    std::vector<Code>& code;
    std::size_t first;
    std::string operation, comment;
    ~CodeOrigin() {
        if (code.size() > first) {
            const auto tag = [&](auto&& self, Code& statement) -> void {
                if (statement.sourceOperation.empty()) statement.sourceOperation = operation;
                for (auto& child : statement.body) self(self, child);
            };
            for (std::size_t i = first; i < code.size(); ++i) tag(tag, code[i]);
            code[first].comment = std::move(comment);
        }
    }
};
std::string unsignedType(int64_t width) {
    if (width > 64) return "unsigned _BitInt(" + std::to_string((width + 63) / 64 * 64) + ")";
    return "std::uint" + std::to_string(width <= 8 ? 8 : width <= 16 ? 16 : width <= 32 ? 32 : 64) + "_t";
}
std::string cppType(const LogicalType& type) {
    type.validate();
    if (type.kind == grh::ValueType::String) return "std::string";
    if (type.kind == grh::ValueType::Real) return "double";
    if (type.domain != grh::LogicDomain::TwoState) throw std::invalid_argument("CPU supports only explicit TwoState Logic; run grh-to-grhsim with its 2state policy");
    const auto width = type.leafWidth();
    std::string result = width == 1 && !type.isSigned ? "bool" : unsignedType(width);
    if (width <= 64 && type.isSigned) result = "std::int" + std::to_string(width <= 8 ? 8 : width <= 16 ? 16 : width <= 32 ? 32 : 64) + "_t";
    for (auto it = type.shape.rbegin(); it != type.shape.rend(); ++it)
        result = "std::array<" + result + "," + std::to_string(*it) + ">";
    return result;
}
LogicalType scalarType(LogicalType type) { type.shape.clear(); return type; }
LogicalType leafType(LogicalType type) { type.width = type.leafWidth(); type.shape.clear(); return type; }
bool packedArrayImage(const LogicalType& type) {
    if (type.kind != grh::ValueType::Logic || type.shape.empty()) return false;
    const auto width = type.leafWidth();
    return width == 8 || width == 16 || width == 32 || width % 64 == 0;
}
std::string mask(int64_t width) {
    if (width <= 64) {
        std::ostringstream out;
        out << "UINT64_C(0x" << std::hex << (width == 64 ? ~uint64_t{} : (uint64_t{1} << width) - 1) << ')';
        return out.str();
    }
    const int64_t container = (width + 63) / 64 * 64;
    return "((~(" + unsignedType(width) + ")0)>>" + std::to_string(container - width) + ")";
}
std::string clean(std::string text, const LogicalType& type) {
    if (type.kind != grh::ValueType::Logic) return text;
    const auto scalar = scalarType(type);
    return "static_cast<" + cppType(scalar) + ">((static_cast<" + unsignedType(type.width) + ">(" + text + "))&" + mask(type.width) + ")";
}
std::string signedValue(const std::string& text, int64_t width) {
    // Sign interpretation is derived from the semantic width, never the host's padding.
    const auto container = width <= 64 ? 64 : (width + 63) / 64 * 64;
    const auto signedType = width <= 64 ? "std::int64_t" : "_BitInt(" + std::to_string(container) + ")";
    return "(static_cast<" + signedType + ">((static_cast<" + unsignedType(container) + ">(" + text + "))<<" +
        std::to_string(container - width) + ")>>" + std::to_string(container - width) + ")";
}
std::string convert(const std::string& text, const LogicalType& source, const LogicalType& target) {
    if (source.kind != grh::ValueType::Logic || target.kind != grh::ValueType::Logic) return text;
    return clean(source.isSigned && source.width < target.width ? signedValue(text, source.width) : text, target);
}
std::string row(const std::string& base, std::span<const int64_t> shape, std::string index) {
    std::string result = base;
    int64_t stride = 1;
    for (const auto size : shape) stride *= size;
    for (const auto size : shape) {
        stride /= size;
        result += "[(" + index + (stride > 1 ? "/" + std::to_string(stride) : "") + ")%" + std::to_string(size) + "]";
    }
    return result;
}
slang::SVInt bits(const std::string& text, int32_t width, bool sign) {
    slang::SVInt result;
    if (text == "'1") result = ~slang::SVInt(width, 0, sign);
    else if (text == "'0") result = slang::SVInt(width, 0, sign);
    else result = slang::SVInt::fromString(text);
    result.setSigned(sign); result = result.resize(width);
    if (result.hasUnknown()) throw std::invalid_argument("Unknown bits reached TwoState CPU literal: " + text);
    return result;
}
std::string literal(const slang::SVInt& value, const LogicalType& type) {
    const auto count = (type.width + 63) / 64;
    std::string result = "(" + unsignedType(type.width) + ")0";
    for (int64_t i = count; i-- > 0;) {
        std::ostringstream out; out << "UINT64_C(0x" << std::hex << value.getRawPtr()[i] << ')';
        if (i == count - 1) result = "((" + unsignedType(type.width) + ")" + out.str() + ")";
        else result = "((" + result + "<<64)|" + out.str() + ")";
    }
    return clean(result, scalarType(type));
}

class CpuStEmitContext {
public:
    CpuStEmitContext(const grh::Graph& graph, const sim::SinkSchedule& schedule, const EmitOptions& options,
                     std::string counterSnapshot = {})
        : graph_(graph), schedule_(schedule), options_(options),
          maxLines_(option(options, "max-function-lines", 1024)),
          tuBytes_(option(options, "max-tu-bytes", 1024 * 1024)),
          maxRounds_(option(options, "max-round", 100000)),
          prefix_("grhsim_" + identifier(graph.symbol())),
          class_("GrhSIM_" + identifier(graph.symbol())), counterSnapshot_(std::move(counterSnapshot)) {}

    std::vector<std::string> generate(const std::filesystem::path& staging);
private:
    const grh::Graph& graph_;
    const sim::SinkSchedule& schedule_;
    const EmitOptions& options_;
    std::size_t maxLines_, tuBytes_, maxRounds_;
    std::string prefix_, class_;
    std::map<Key, std::vector<OperationId>> members_;
    std::set<Key> executionKeys_;
    std::vector<const Key*> executionOwners_;
    std::map<Key, std::size_t> ordinal_;
    std::vector<Key> nonsink_;
    std::unordered_map<ValueId, std::string, grh::ValueIdHash> storage_;
    std::unordered_map<ValueId, std::string, grh::ValueIdHash> smallEnableBoundary_, smallPayloadBoundary_;
    std::unordered_map<ValueId, std::size_t, grh::ValueIdHash> smallEnableOrdinal_;
    std::unordered_map<ValueId, std::set<std::size_t>, grh::ValueIdHash> fanout_;
    std::unordered_map<OperationId, std::set<std::size_t>, grh::OperationIdHash> stateFanout_;
    struct IndexedState {
        int64_t cells = 0;
        std::set<std::size_t> fullFanout;
        std::map<std::size_t, std::vector<OperationId>> readers;
    };
    struct IndexedRead { OperationId state; int64_t count = 0; };
    std::unordered_map<OperationId, IndexedState, grh::OperationIdHash> indexedStates_;
    std::unordered_map<OperationId, IndexedRead, grh::OperationIdHash> indexedReads_;
    std::unordered_set<OperationId, grh::OperationIdHash> stateRound_;
    std::unordered_map<OperationId, std::size_t, grh::OperationIdHash> stateOrdinal_;
    std::unordered_set<OperationId, grh::OperationIdHash> wholeArrayStates_;
    std::unordered_map<OperationId, std::string, grh::OperationIdHash> stateNames_;
    std::unordered_map<OperationId, std::size_t, grh::OperationIdHash> largeWriteOrdinal_;
    std::unordered_map<ValueId, std::string, grh::ValueIdHash> fireExpressions_;
    std::unordered_map<ValueId, bool, grh::ValueIdHash> viewOnly_;
    std::unordered_map<ValueId, ValueId, grh::ValueIdHash> snapshotViews_;
    std::unordered_map<ValueId, std::string, grh::ValueIdHash> constantSources_;
    std::unordered_map<ValueId, std::string, grh::ValueIdHash> constantStrings_;
    std::unordered_set<ValueId, grh::ValueIdHash> publishedConstants_;
    std::unordered_set<OperationId, grh::OperationIdHash> identityElisions_;
    struct LeafPlan {
        enum class Kind { Source, Concat, Repeat, Mux };
        Kind kind = Kind::Source;
        ValueId source;
        int64_t width = 0;
        std::vector<LeafPlan> inputs;
    };
    std::unordered_map<OperationId, LeafPlan, grh::OperationIdHash> leafPlans_;
    std::unordered_set<OperationId, grh::OperationIdHash> leafElisions_;
    std::vector<OperationId> states_, edges_, histories_;
    std::map<std::string, std::vector<Local>> fields_;
    std::map<std::string, std::set<std::string>> fieldDeclarations_;
    std::map<std::string, ValueStructLayout> fieldLayouts_;
    std::vector<std::string> functionNames_;
    std::function<void(Function)> writeFunction_;
    std::set<std::string> publicFunctions_;
    std::string dpiDeclarations_;
    std::map<std::string, std::string> initFiles_;
    Function* function_ = nullptr;
    Key owner_;
    struct PublicationGroup {
        std::size_t expected = 0;
        std::size_t emitted = 0;
        std::string dirty;
    };
    bool groupPublications_ = false;
    std::map<std::set<std::size_t>, PublicationGroup> publicationGroups_;
    std::set<ValueId, bool(*)(ValueId, ValueId)> packed_{[](ValueId a, ValueId b) { return a.index < b.index; }};
    std::set<std::string> localNames_;
    std::size_t temporary_ = 0;
    bool readableNames_ = true;
    bool sourceComments_ = true;
    bool readableFrames_ = true;
    OperationId currentOperation_;
    mutable std::unordered_map<OperationId, std::string, grh::OperationIdHash> operationNames_;
    mutable std::unordered_map<ValueId, std::string, grh::ValueIdHash> valueNames_;
    bool waveform_ = false;
    bool hasTasks_ = false;
    bool activateOnWrite_ = false;
    bool enableBitmap_ = true;
    struct CounterSite { OperationId operation; std::string category, function; };
    std::string counterSnapshot_;
    std::string counterBinding_;
    std::vector<std::string> counterBindingFiles_;
    std::vector<CounterSite> counterSites_;
    std::unordered_map<ValueId, std::string, grh::ValueIdHash> packedNames_;
    std::string counterBinding(const std::filesystem::path& staging) const;
    std::string counterMetadata() const;
    void codeMap(std::ostream& output, const std::vector<WrittenFunctions>& written,
                 const std::map<std::string, std::string>& sources) const;
    void countOperation(std::vector<Code>& code, OperationId op, std::string category = "operation") {
        if (counterSnapshot_.empty()) return;
        code.push_back(Code::line("++opExecCounts_[" + std::to_string(counterSites_.size()) + "];"));
        counterSites_.push_back({op, std::move(category), function_->name});
    }

    const Key& executionOwner(OperationId op) const {
        return *executionOwners_.at(op.index);
    }
    const std::string& name(OperationId op) const;
    const std::string& valueName(ValueId id) const;
    void analyzeNames();
    std::string qualifiedName(OperationId op, std::string semantic) const;
    std::string operationDescription(OperationId op) const;
    std::string valueDescription(ValueId value) const;
    std::string functionDescription(const Function& function, const Key& key) const;
    std::string captureName(OperationId op, std::optional<std::size_t> operand = {}) const;
    std::string subscriptionName(OperationId op) const {
        return readableNames_ ? name(op) + "_last_read_leaf" : "r" + std::to_string(op.index);
    }
    std::string packedPrefix(ValueId id) const {
        return readableNames_ ? valueName(id) + "_packed_" : "packed" + std::to_string(id.index) + "_";
    }
    std::string attr(OperationId op, std::string_view key, std::string fallback = {}) const {
        return sim::attribute<std::string>(graph_, op, key).value_or(std::move(fallback));
    }
    bool large(OperationId state) const { return attr(state, "grhsim.statePolicy") == "large-sink"; }
    bool writeActivation(OperationId state) const { return activateOnWrite_ && graph_.opKind(state) != OperationKind::kLatch; }
    std::string fieldRef(const std::string& store, const std::string& member, std::string_view instance = {}) const {
        return std::string(instance.empty() ? std::string_view(store) : instance) + "." + fieldLayouts_.at(store).references.at(member);
    }
    std::string stateRef(OperationId state) const { return fieldRef(large(state) ? "largeSinkStore" : "smallSinkStore", name(state)); }
    std::string nextStateRef(OperationId state) const { return fieldRef("smallSinkStore", name(state), "smallSinkStoreNext"); }
    std::vector<OperationId> topological(const std::vector<OperationId>& ops) const;
    void analyze();
    void analyzeIndexedReads();
    void activateState(std::vector<Code>& code, OperationId state);
    void touchState(std::vector<Code>& code, OperationId state, const std::string& start, int64_t count);
    void analyzeSnapshotViews();
    void analyzeLeafPlans();
    void emitLeaves(std::vector<Code>& code, const std::string& destination, const LogicalType& target,
                    const LeafPlan& plan, const std::string& offset = "0");
    ValueId arrayView(ValueId value) const;
    bool viewOnly(ValueId value);
    void field(std::string store, const std::string& type, const std::string& name, std::string comment = {}) {
        const auto declaration = type + " " + name;
        if (fieldDeclarations_[store].insert(declaration).second) {
            if (sourceComments_ && comment.empty()) comment = store + " bookkeeping field";
            fields_[store].push_back({type, name, "{}", false, sourceComments_ ? std::move(comment) : std::string{}});
        }
    }
    void field(std::string store, const std::string& type, OperationId op) {
        field(std::move(store), type, name(op), sourceComments_ ? operationDescription(op) : std::string{});
    }
    void field(std::string store, const std::string& type, ValueId value) {
        field(std::move(store), type, valueName(value), sourceComments_ ? valueDescription(value) : std::string{});
    }
    std::string local(const std::string& type, const std::string& name, std::string initializer = "{}", std::string comment = {}) {
        if (localNames_.insert(name).second)
            function_->locals.push_back({type, name, std::move(initializer), false, sourceComments_ ? std::move(comment) : std::string{}});
        return "@" + name + "@";
    }
    std::string temporary(const std::string& type, std::string_view role = "scratch", ValueId value = {}) {
        const auto ordinal = temporary_++;
        const auto stem = value ? valueName(value) : currentOperation_ ? name(currentOperation_) :
            "runtime_" + readableIdentifier(function_->name);
        const auto member = readableNames_ ? stem + "_" + std::string(role) + "_t" + std::to_string(ordinal) :
            "tmp" + std::to_string(ordinal);
        auto comment = sourceComments_ ? std::string(role) + "; " +
            (currentOperation_ ? operationDescription(currentOperation_) : "generated runtime helper in " + function_->name) : std::string{};
        return local(type, member, "{}", std::move(comment));
    }
    std::string read(ValueId id, bool payload = false) {
        if (owner_.size() == 3 && owner_[1] == "small-sink") {
            if (payload)
                if (const auto found = smallPayloadBoundary_.find(id); found != smallPayloadBoundary_.end()) return found->second;
            if (const auto found = smallEnableBoundary_.find(id); found != smallEnableBoundary_.end()) return found->second;
            if (const auto found = smallPayloadBoundary_.find(id); found != smallPayloadBoundary_.end()) return found->second;
        }
        if (const auto found = storage_.find(id); found != storage_.end()) return found->second;
        if (const auto found = snapshotViews_.find(id); found != snapshotViews_.end()) return read(found->second, payload);
        if (const auto found = constantStrings_.find(id); found != constantStrings_.end()) return found->second + "()";
        const auto& member = valueName(id);
        if (localNames_.contains(member)) return "@" + member + "@";
        return local(cppType(graph_.valueLogicalType(id)), member, "{}",
                     sourceComments_ ? valueDescription(id) : std::string{});
    }
    std::string readSource(ValueId id, bool payload = false) {
        const auto found = constantSources_.find(id);
        if (found != constantSources_.end() && owner_.size() == 3 && owner_[0] == "general") {
            const auto& producer = executionOwner(graph_.valueDef(id));
            // Only the original defining op is immutable. Do not look through
            // held samples, role views or assigns. General is topological, and
            // every nonsink runs before the ordered sink batches after init.
            // Cross-owner sources must publish before any throwing operation
            // in that producer: an interrupted owner has consumed its activity
            // bit, so a later eval may otherwise observe its initial boundary.
            if (producer == owner_ || (publishedConstants_.contains(id) && ordinal_.contains(producer) &&
                (!ordinal_.contains(owner_) || ordinal_.at(producer) < ordinal_.at(owner_))))
                return found->second;
        }
        return read(id, payload);
    }
    std::string packed(ValueId id, std::vector<Code>& code, bool addressable = false);
    void copy(std::vector<Code>& code, const std::string& destination, const LogicalType& target,
              const std::string& source, const LogicalType& sourceType);
    void unpack(std::vector<Code>& code, const std::string& destination, const LogicalType& type,
                const std::string& scalar, int64_t sourceWidth);
    std::string pack(std::vector<Code>& code, const std::string& source, const LogicalType& type, const std::string& destination);
    void activate(std::vector<Code>& code, const std::set<std::size_t>& fanout, std::optional<std::size_t> current = {});
    void markState(std::vector<Code>& code, OperationId state);
    bool operationIsElided(OperationId op) const;
    bool scalarPublication(ValueId value) const;
    void beginPublications(std::vector<Code>& code, const std::vector<OperationId>& ops);
    void finishPublications(std::vector<Code>& code);
    void publish(std::vector<Code>& code, ValueId value, const std::string& source);
    void syncSmallBoundary(std::vector<Code>& code, ValueId value, const std::string& source);
    std::string fireExpression(ValueId value);
    std::string expression(OperationId op, ValueId result, std::vector<Code>& code);
    void operation(std::vector<Code>& code, OperationId op);
    void writer(std::vector<Code>& code, OperationId op, bool commit);
    void dpi(std::vector<Code>& code, OperationId op);
    void task(std::vector<Code>& code, OperationId op);
    void buildPhases();
    void buildInit();
    void buildConstantSources();
    void buildObservers();
    void buildRuntime();
    void addFunction(Function function, bool publicFunction = false) {
        function.readableFrame = readableFrames_;
        if (sourceComments_ && function.comment.empty()) function.comment = functionDescription(function, {});
        if (publicFunction) publicFunctions_.insert(function.name);
        functionNames_.push_back(function.name);
        writeFunction_(std::move(function));
    }
    void inFunction(Function& function, const Key& key, const std::function<void(std::vector<Code>&)>& action) {
        function_ = &function; owner_ = key; packed_.clear(); packedNames_.clear(); localNames_.clear();
        groupPublications_ = false; publicationGroups_.clear();
        currentOperation_ = {};
        if (sourceComments_) function.comment = functionDescription(function, key);
        for (const auto& local : function.locals) localNames_.insert(local.name);
        action(function.body); function_ = nullptr;
        groupPublications_ = false; publicationGroups_.clear();
    }
};

void CpuStEmitContext::analyzeNames() {
    readableNames_ = enabledOption(options_, "readable-names");
    sourceComments_ = enabledOption(options_, "source-comments");
    readableFrames_ = enabledOption(options_, "readable-frames");
    // The graph can contain millions of elided operations and values. Cache a
    // name only when the generated source or map references it, and attach
    // comments directly to declarations instead of retaining a second global
    // copy indexed by each long C++ name.
}

std::string CpuStEmitContext::qualifiedName(OperationId op, std::string semantic) const {
    const auto origins = sim::attribute<std::vector<std::string>>(graph_, op, "originModules");
    std::string provenance = origins && !origins->empty() ? origins->front() : "generated";
    if (origins && !origins->empty() && semantic.starts_with(origins->front() + "$"))
        semantic.erase(0, origins->front().size() + 1);
    if (origins && origins->size() > 1) provenance += "_shared" + std::to_string(origins->size());
    return provenance + "_" + semantic;
}

const std::string& CpuStEmitContext::name(OperationId op) const {
    if (const auto found = operationNames_.find(op); found != operationNames_.end()) return found->second;
    std::string result = "s" + std::to_string(op.index);
    if (readableNames_) {
        const auto symbol = graph_.symbolText(graph_.operationSymbol(op));
        const auto kind = std::string(grh::toString(graph_.opKind(op)));
        const auto semantic = graph_.isDeclaredSymbol(graph_.operationSymbol(op)) || (!symbol.empty() && symbol.front() != '_') ?
            std::string(symbol) : attr(op, "regSymbol", attr(op, "latchSymbol", kind));
        result = "op_" + readableIdentifier(qualifiedName(op, semantic)) +
            "_" + readableIdentifier(kind) + "_o" + std::to_string(op.index);
    }
    return operationNames_.emplace(op, std::move(result)).first->second;
}

const std::string& CpuStEmitContext::valueName(ValueId value) const {
    if (const auto found = valueNames_.find(value); found != valueNames_.end()) return found->second;
    std::string result = "v" + std::to_string(value.index);
    if (readableNames_) {
        const auto symbol = graph_.symbolText(graph_.valueSymbol(value));
        const auto producer = graph_.valueDef(value);
        std::string provenance;
        if (graph_.valueIsInput(value)) provenance = std::string(graph_.symbol()) + "_input_" + std::string(symbol);
        else if (graph_.isDeclaredSymbol(graph_.valueSymbol(value)))
            provenance = producer ? qualifiedName(producer, std::string(symbol)) : "generated_unbound_" + std::string(symbol);
        else if (producer) provenance = name(producer);
        else provenance = "generated_unbound_" + std::string(symbol);
        result = "val_" + readableIdentifier(provenance) + "_v" + std::to_string(value.index);
    }
    return valueNames_.emplace(value, std::move(result)).first->second;
}

std::string CpuStEmitContext::valueDescription(ValueId value) const {
    const auto producer = graph_.valueDef(value);
    return "value v" + std::to_string(value.index) + " symbol=" + std::string(graph_.symbolText(graph_.valueSymbol(value))) +
        (producer ? "; " + operationDescription(producer) : "; input/external value");
}

std::string CpuStEmitContext::operationDescription(OperationId op) const {
    std::string text = "op o" + std::to_string(op.index) + " " + std::string(grh::toString(graph_.opKind(op))) +
        " symbol=" + std::string(graph_.symbolText(graph_.operationSymbol(op)));
    if (const auto origins = sim::attribute<std::vector<std::string>>(graph_, op, "originModules"); origins && !origins->empty()) {
        text += "; origin=" + origins->front();
        if (origins->size() > 1) text += " (+" + std::to_string(origins->size() - 1) + " origins; see grhsim.txt)";
    } else text += "; origin unavailable/generated";
    if (const auto location = graph_.opSrcLoc(op)) {
        if (!location->file.empty()) text += "; source=" + location->file + ":" + std::to_string(location->line);
        if (!location->pass.empty()) text += "; pass=" + location->pass;
    }
    if (sim::isWriter(graph_.opKind(op))) text += "; target=" + attr(op, "regSymbol", attr(op, "latchSymbol"));
    if (const auto order = sim::attribute<int64_t>(graph_, op, "writerOrder")) text += "; writerOrder=" + std::to_string(*order);
    if (text.size() > 420) text = text.substr(0, 390) + "... (full origin in grhsim.txt)";
    return text;
}

std::string CpuStEmitContext::functionDescription(const Function& function, const Key& key) const {
    static const std::map<std::string, std::string> phases{
        {"pInput", "Phase 1 Input: snapshot inputs and activate changed consumers."},
        {"pEvent", "Phase 2 Event: detect edges from current state; update detector history."},
        {"pGeneral", "Phase 3 General: execute active nonsinks, then ordered writer sampling runs."},
        {"pLargeSink", "Phase 4 LargeSink: commit captured payloads in writer order."},
        {"pPublish", "Phase 5 Publish: publish committed small/large state and schedule another round."},
        {"pOutput", "Phase 6 Output: refresh observable ports after fixed-point convergence."},
        {"eval", "Evaluate one timeslot through the six phases until convergence, then Output."},
        {"init", "Initialize model state, named stores and initial activity."}};
    if (const auto found = phases.find(function.name); found != phases.end()) return found->second;
    std::string text = function.name;
    if (!key.empty()) {
        text += "; execution owner=";
        for (std::size_t i = 0; i < key.size(); ++i) text += (i ? "/" : "") + key[i];
    }
    if (function.name.starts_with("writer_run")) text += "; sample writer inputs before large-state commit; preserve schedule order";
    else if (function.name.starts_with("commit_run")) text += "; commit captured large-state writes in schedule order";
    else if (function.name.starts_with("sn_")) text += "; active nonsink computation; see code-map.json for original operations";
    else text += "; generated scheduling/runtime helper";
    return text;
}

std::string CpuStEmitContext::captureName(OperationId op, std::optional<std::size_t> operand) const {
    if (!readableNames_) return "w" + std::to_string(op.index) + (operand ? "_" + std::to_string(*operand) : "");
    if (!operand) return name(op) + "_captured_enable";
    const auto args = graph_.opOperands(op);
    const auto kind = graph_.opKind(op);
    const bool masked = kind == sim::kLatchWrite || kind == sim::kWriteMask || kind == sim::kWriteArrayMask;
    const auto data = args.size() - (masked ? 2 : 1);
    const auto role = *operand == data ? "data" : masked && *operand == args.size() - 1 ? "mask" : "index";
    return name(op) + "_captured_" + role + "_arg" + std::to_string(*operand);
}

std::vector<OperationId> CpuStEmitContext::topological(const std::vector<OperationId>& ops) const {
    std::unordered_map<OperationId, std::size_t, grh::OperationIdHash> degree;
    std::unordered_map<OperationId, std::vector<OperationId>, grh::OperationIdHash> successors;
    for (const auto op : ops) degree.emplace(op, 0);
    for (const auto op : ops) for (const auto value : graph_.opOperands(op)) {
        const auto producer = graph_.valueDef(value);
        if (degree.contains(producer)) { ++degree[op]; successors[producer].push_back(op); }
    }
    std::map<int64_t, OperationId> effects;
    for (const auto op : ops)
        if (const auto order = sim::attribute<int64_t>(graph_, op, "grhsim.effectOrder")) effects.emplace(*order, op);
    OperationId previous;
    for (const auto& [order, op] : effects) {
        if (previous) { ++degree[op]; successors[previous].push_back(op); } previous = op;
    }
    const auto compare = [](OperationId a, OperationId b) { return a.index > b.index; };
    std::priority_queue<OperationId, std::vector<OperationId>, decltype(compare)> ready(compare);
    for (const auto op : ops) if (!degree[op]) ready.push(op);
    std::vector<OperationId> result;
    while (!ready.empty()) {
        const auto op = ready.top(); ready.pop(); result.push_back(op);
        for (const auto next : successors[op]) if (--degree[next] == 0) ready.push(next);
    }
    if (result.size() != ops.size()) throw std::invalid_argument("CPU emitter encountered a cyclic operation/effect dependency");
    return result;
}

void CpuStEmitContext::analyze() {
    analyzeNames();
    // requireSinkSchedule already verified this immutable graph and schedule.
    if (graph_.metadata(sim::kStage) != std::optional<grh::AttributeValue>(std::string("sink-partitioned")))
        throw std::invalid_argument("grhsim-cpu-st-cpp requires the sink-partitioned stage");
    if (!graph_.inoutPorts().empty()) throw std::invalid_argument("CPU top-level inout ports are not implemented");
    uint32_t lastOperation = 0;
    for (const auto op : graph_.operations()) lastOperation = std::max(lastOperation, op.index);
    executionOwners_.resize(static_cast<std::size_t>(lastOperation) + 1);
    for (const auto op : graph_.operations()) {
        auto key = sim::attribute<Key>(graph_, op, sim::kPartition).value_or(Key{});
        // The leaf limits rewrites; execution uses its enclosing supernode.
        if (key.size() == 4 && key[0] == "general" && key[1] == "nonsink") key.resize(3);
        executionOwners_[op.index] = &*executionKeys_.insert(std::move(key)).first;
    }
    if (const auto found = options_.attributes.find("mem-write-activate"); found != options_.attributes.end()) {
        if (found->second != "on-change" && found->second != "on-write") throw std::invalid_argument("mem-write-activate must be on-change or on-write");
        activateOnWrite_ = found->second == "on-write";
    }
    if (const auto found = options_.attributes.find("mem-enable-bitmap"); found != options_.attributes.end()) {
        if (found->second != "on" && found->second != "off") throw std::invalid_argument("mem-enable-bitmap must be on or off");
        enableBitmap_ = found->second == "on";
    }
    for (const auto& run : schedule_.largeCommitRuns) for (const auto writer : run.orderedWriters)
        if (!largeWriteOrdinal_.contains(writer)) largeWriteOrdinal_.emplace(writer, largeWriteOrdinal_.size());
    if (enableBitmap_ && !largeWriteOrdinal_.empty())
        field("largeSinkEnableBoundaryStore", "std::array<std::uint64_t," + std::to_string((largeWriteOrdinal_.size() + 63) / 64) + ">", "words");
    waveform_ = options_.attributes.contains("waveform") && options_.attributes.at("waveform") != "none";
    for (const auto value : graph_.values()) (void)cppType(graph_.valueLogicalType(value));
    std::set<std::string> ports;
    const auto bindPort = [&](const grh::Port& port, bool input) {
        const auto sanitized = identifier(port.name);
        if (!ports.insert(sanitized).second) throw std::invalid_argument("CPU port names collide after C++ sanitization: " + port.name);
        if (input) {
            field("inputStore", cppType(graph_.valueLogicalType(port.value)), port.value);
            storage_[port.value] = "inputStore." + valueName(port.value);
        }
    };
    for (const auto& port : graph_.inputPorts()) bindPort(port, true);
    for (const auto& port : graph_.outputPorts()) bindPort(port, false);
    for (const auto op : graph_.operations()) {
        if (const auto problem = grh::verifyOpDescriptor(graph_, op); !problem.empty()) throw std::invalid_argument(problem);
        const auto kind = graph_.opKind(op);
        if (kind == OperationKind::kRegister || kind == OperationKind::kLatch) {
            const auto type = grh::stateLogicalType(graph_, op);
            if (type.kind != grh::ValueType::Logic && attr(op, "grhsim.history") != "output-end")
                throw std::invalid_argument("CPU real/string state is not implemented");
            (void)cppType(type);
            if (grh::stateInitSpec(graph_, op).empty()) throw std::invalid_argument("CPU state initializer is missing");
            stateOrdinal_.emplace(op, states_.size());
            states_.push_back(op);
            field(large(op) ? "largeSinkStore" : "smallSinkStore", cppType(type), op);
            if (large(op)) {
                if (writeActivation(op)) field("largeBeforeStore", "bool", op);
                else field("largeBeforeStore", "std::map<std::size_t," + cppType(leafType(type)) + ">", op);
            }
            continue;
        }
        if (kind == OperationKind::kDpicImport) continue;
        const auto& key = executionOwner(op);
        if (key.empty()) throw std::invalid_argument("CPU executable op is missing its virtual partition");
        members_[key].push_back(op);
        if (kind == sim::kEdgeDetect) {
            edges_.push_back(op); field("prevEventStore", "bool", op);
        }
        if (kind == sim::kHistoryWrite) histories_.push_back(op);
        if (kind == sim::kSystemTask) {
            hasTasks_ = true;
            if (attr(op, "grhsim.timeslot") == "event") field("timeslotTriggerStore", "bool", op);
            if (attr(op, "procKind") == "initial") field("onceStore", "bool", op);
        }
        if (kind == sim::kSystemFunction && sim::systemFunctionEffect(graph_, op) != "pure") field("sampledStore", "bool", op);
    }
    // Whole-array writes can use the native std::array assignment route.  A
    // large on-change state needs one aggregate before-image so the publish
    // phase can compare the complete write without materializing one map entry
    // per element.
    for (const auto op : graph_.operations()) {
        if (!sim::isWriter(graph_.opKind(op))) continue;
        const auto state = sim::writerTarget(graph_, op);
        const auto type = grh::stateLogicalType(graph_, state);
        const auto args = graph_.opOperands(op);
        const bool latch = graph_.opKind(op) == sim::kLatchWrite;
        const bool masked = latch || graph_.opKind(op) == sim::kWriteMask || graph_.opKind(op) == sim::kWriteArrayMask;
        const bool indexed = graph_.opKind(op) == sim::kWriteArray || graph_.opKind(op) == sim::kWriteArrayMask;
        const auto dataIndex = args.size() - (masked ? 2 : 1);
        if (type.shape.empty() || indexed || masked || dataIndex >= args.size()) continue;
        const auto dataType = graph_.valueLogicalType(args[dataIndex]);
        if ((graph_.opKind(op) == sim::kWrite || graph_.opKind(op) == sim::kFill) && dataType == type)
            wholeArrayStates_.insert(state);
    }
    for (const auto state : states_) if (wholeArrayStates_.contains(state) && large(state) && !writeActivation(state)) {
        const auto type = grh::stateLogicalType(graph_, state);
        field("largeWholeBeforeStore", cppType(type), state);
        field("largeWholeDirtyStore", "bool", state);
    }
    std::map<Key, std::set<Key>> dependencies;
    for (const auto& [key, ops] : members_) if (key.size() > 1 && key[0] == "general" && key[1] == "nonsink") {
        dependencies[key];
        for (const auto op : ops) for (const auto value : graph_.opOperands(op)) {
            const auto producer = graph_.valueDef(value); if (!producer) continue;
            const auto& from = executionOwner(producer);
            if (from != key && from.size() > 1 && from[0] == "general" && from[1] == "nonsink") dependencies[key].insert(from);
        }
    }
    std::map<int64_t, Key> effects;
    for (const auto& [key, ops] : members_) for (const auto op : ops)
        if (const auto order = sim::attribute<int64_t>(graph_, op, "grhsim.effectOrder")) effects.emplace(*order, key);
    Key previous;
    for (const auto& [order, key] : effects) {
        if (!previous.empty() && previous != key && dependencies.contains(key) && dependencies.contains(previous)) dependencies[key].insert(previous);
        previous = key;
    }
    std::map<Key, std::size_t> dependencyIndex;
    std::vector<const Key*> keys;
    for (const auto& [key, inputs] : dependencies) {
        dependencyIndex.emplace(key, keys.size()); keys.push_back(&key);
    }
    std::vector<std::size_t> degree(keys.size());
    std::vector<std::vector<std::size_t>> successors(keys.size());
    std::set<std::size_t> ready, nextSweep;
    for (const auto& [key, inputs] : dependencies) {
        const auto target = dependencyIndex.at(key);
        degree[target] = inputs.size();
        for (const auto& source : inputs) successors[dependencyIndex.at(source)].push_back(target);
        if (inputs.empty()) ready.insert(target);
    }
    // Preserve the previous lexicographic sweep order, visiting only ready
    // nodes instead of rescanning the whole DAG for every dependency layer.
    while (!ready.empty()) {
        const auto current = *ready.begin(); ready.erase(ready.begin());
        const auto& key = *keys[current];
        ordinal_[key] = nonsink_.size(); nonsink_.push_back(key);
        for (const auto target : successors[current]) if (--degree[target] == 0)
            (target > current ? ready : nextSweep).insert(target);
        if (ready.empty()) ready.swap(nextSweep);
    }
    if (nonsink_.size() != dependencies.size()) throw std::invalid_argument("CPU nonsink virtual partition DAG is cyclic");
    // Classify crossing values by the consumer cone, not just the producer.
    // A value shared by enable, payload and nonsink users needs each view.
    std::unordered_map<ValueId, unsigned, grh::ValueIdHash> smallRoles;
    std::map<Key, std::unordered_map<ValueId, unsigned, grh::ValueIdHash>> visitedRoles;
    std::function<void(ValueId, const Key&, unsigned)> classify = [&](ValueId value, const Key& key, unsigned role) {
        auto& visited = visitedRoles[key][value];
        if ((visited & role) || sim::isFire(graph_, value)) return;
        visited |= role;
        const auto producer = graph_.valueDef(value);
        if (!producer || executionOwner(producer) != key) { smallRoles[value] |= role; return; }
        for (const auto arg : graph_.opOperands(producer)) classify(arg, key, role);
    };
    for (const auto& run : schedule_.generalRuns) if (run.partitionKey[1] == "small-sink")
        for (const auto writer : run.orderedWriters) {
            const auto args = graph_.opOperands(writer);
            const auto enableIndex = graph_.opKind(writer) == sim::kLatchWrite ? 0u : 1u;
            classify(args[enableIndex], run.partitionKey, 1);
            for (std::size_t i = enableIndex + 1; i < args.size(); ++i) classify(args[i], run.partitionKey, 2);
        }
    for (const auto value : graph_.values()) {
        const auto users = graph_.valueUsers(value);
        const auto producer = graph_.valueDef(value);
        const auto role = smallRoles.find(value);
        if (role != smallRoles.end()) {
            const auto member = valueName(value), type = cppType(graph_.valueLogicalType(value));
            if (role->second & 1) {
                field("smallSinkEnableBoundaryStore", type, value);
                smallEnableBoundary_[value] = "smallSinkEnableBoundaryStore." + member;
            }
            if (role->second & 2) {
                field("smallSinkPayloadBoundaryStore", type, value);
                smallPayloadBoundary_[value] = "smallSinkPayloadBoundaryStore." + member;
            }
            // Only direct, externally published writer enables are safe to
            // probe before entering the run. Local enables must be evaluated.
            if (enableBitmap_ && graph_.valueWidth(value) == 1 &&
                (!producer || graph_.opKind(producer) != OperationKind::kConstant))
                for (const auto& user : users) {
                    const auto& key = executionOwner(user.operation);
                    if (key.size() == 3 && key[1] == "small-sink" && sim::isWriter(graph_.opKind(user.operation)) &&
                        user.operandIndex == (graph_.opKind(user.operation) == sim::kLatchWrite ? 0u : 1u) &&
                        (!producer || executionOwner(producer) != key)) {
                        smallEnableOrdinal_.emplace(value, smallEnableOrdinal_.size()); break;
                    }
                }
        }
        for (const auto& user : users) {
            const auto& target = executionOwner(user.operation);
            if (ordinal_.contains(target) && (!producer || executionOwner(producer) != target)) fanout_[value].insert(ordinal_.at(target));
        }
        if (graph_.valueIsInput(value)) continue;
        if (!producer) {
            if (users.empty() && !graph_.valueIsOutput(value)) continue;
            throw std::invalid_argument("CPU value has no input or producer");
        }
        const auto& key = executionOwner(producer);
        bool boundary = sim::attribute<bool>(graph_, producer, "grhsim.sampleBoundary").value_or(false);
        boundary |= waveform_ && graph_.isDeclaredSymbol(graph_.valueSymbol(value));
        for (const auto& use : users) {
            const auto& target = executionOwner(use.operation);
            // Small consumers have dedicated fields. Other crossings and
            // held/observed values retain their ordinary boundary view.
            boundary |= target != key && !(target.size() == 3 && target[1] == "small-sink" && role != smallRoles.end());
        }
        // Random/DPI results must retain their sample even in an inactive leaf.
        boundary |= graph_.opKind(producer) == sim::kDpiCall ||
            (graph_.opKind(producer) == sim::kSystemFunction && sim::systemFunctionEffect(graph_, producer) != "pure");
        if (graph_.opKind(producer) == sim::kEdgeDetect) boundary = true;
        if (boundary) {
            const std::string store = graph_.opKind(producer) == sim::kEdgeDetect ? "eventActStore" : "nonSinkBoundaryStore";
            storage_[value] = store + "." + valueName(value);
            field(store, cppType(graph_.valueLogicalType(value)), value);
        } else if (smallEnableBoundary_.contains(value)) storage_[value] = smallEnableBoundary_.at(value);
        else if (smallPayloadBoundary_.contains(value)) storage_[value] = smallPayloadBoundary_.at(value);
    }
    if (!smallEnableOrdinal_.empty())
        field("smallSinkEnableBoundaryStore", "std::array<std::uint64_t," + std::to_string((smallEnableOrdinal_.size() + 63) / 64) + ">", "words");
    for (const auto op : graph_.operations()) {
        const auto kind = graph_.opKind(op);
        if (kind != OperationKind::kRegisterReadPort && kind != OperationKind::kRegisterReadArrayPort && kind != OperationKind::kLatchReadPort) continue;
        const auto state = graph_.findOperation(attr(op, kind == OperationKind::kLatchReadPort ? "latchSymbol" : "regSymbol"));
        if (!state) throw std::invalid_argument("CPU state read target is missing");
        const auto key = executionOwner(op);
        if (ordinal_.contains(key)) stateFanout_[state].insert(ordinal_.at(key));
        // Event-owned reads have no activity bit. Publishing their state must
        // still run the next detector round to observe NBA-derived edges.
        else if (key == Key{"event"} || (key.size() == 3 && key[0] == "general")) stateRound_.insert(state);
    }
    analyzeIndexedReads();
    analyzeSnapshotViews();
    for (const auto op : graph_.operations()) if (graph_.opKind(op) == OperationKind::kAssign)
        (void)viewOnly(graph_.opResults(op)[0]);
    // Remove only dead local projections into the proven identity views.
    bool removed = true;
    while (removed) {
        removed = false;
        for (const auto op : graph_.operations()) {
            const auto kind = graph_.opKind(op);
            if (identityElisions_.contains(op) || (kind != OperationKind::kAssign && kind != OperationKind::kSliceStatic)) continue;
            const auto results = graph_.opResults(op);
            if (results.size() != 1) continue;
            const auto id = results[0];
            if (storage_.contains(id) || smallEnableBoundary_.contains(id) || smallPayloadBoundary_.contains(id) || graph_.valueIsOutput(id)) continue;
            const auto users = graph_.valueUsers(id);
            if (users.empty()) continue;
            bool dead = true;
            for (const auto& use : users)
                dead &= executionOwner(use.operation) == executionOwner(op) && identityElisions_.contains(use.operation);
            if (dead) { identityElisions_.insert(op); removed = true; }
        }
    }
    analyzeLeafPlans();
    for (const auto op : graph_.operations()) {
        if (graph_.opKind(op) != OperationKind::kConstant) continue;
        const auto id = graph_.opResults(op)[0];
        const auto type = graph_.valueLogicalType(id);
        if (type.kind == grh::ValueType::String)
            constantStrings_.emplace(id, "constant_" + valueName(id));
        else if (type.kind == grh::ValueType::Logic && type.shape.empty() && type.width <= 64)
            constantSources_.emplace(id, literal(bits(attr(op, "constValue"), type.width, type.isSigned), type));
    }
    // Register capture fields before rendering accesses. Every function must
    // use the same layout regardless of the order in which writers are emitted.
    for (const auto op : graph_.operations()) if (sim::isWriter(graph_.opKind(op)) && large(sim::writerTarget(graph_, op))) {
        if (!enableBitmap_) field("largeSinkEnableBoundaryStore", "bool", captureName(op),
                                 sourceComments_ ? "captured writer enable; " + operationDescription(op) : std::string{});
        const auto args = graph_.opOperands(op);
        for (std::size_t i = graph_.opKind(op) == sim::kLatchWrite ? 1 : 2; i < args.size(); ++i) {
            field("largeSinkPayloadBoundaryStore", cppType(graph_.valueLogicalType(args[i])),
                  captureName(op, i), sourceComments_ ? "captured argument " + std::to_string(i) + " from " + valueName(args[i]) +
                  "; " + operationDescription(op) : std::string{});
        }
    }
    // Named stores expose semantic state and boundary fields directly.  The
    // indexed layout is reserved for emitter-private chunk frames; sharing it
    // here turns a store into opaque cells_* arrays and breaks its ABI/readability.
    for (const auto& [store, fields] : fields_)
        fieldLayouts_.emplace(store, writeDirectValueLayout(store + "Type", fields));
    for (auto& [value, reference] : storage_) {
        const auto separator = reference.find('.');
        reference = fieldRef(reference.substr(0, separator), reference.substr(separator + 1));
    }
}

void CpuStEmitContext::analyzeIndexedReads() {
    // Subscriptions belong to the last executed read, including held samples.
    // Only pure nonsink owners may lose a state-triggered activation; effects
    // retain the original on-write scheduling even when their data is unchanged.
    std::set<std::size_t> effectOwners;
    for (const auto& [key, ops] : members_) if (ordinal_.contains(key))
        for (const auto op : ops) if (grh::operationEffect(graph_, op) == grh::OpEffect::External) {
            effectOwners.insert(ordinal_.at(key)); break;
        }
    std::unordered_map<OperationId, IndexedState, grh::OperationIdHash> candidates;
    for (const auto op : graph_.operations()) {
        const auto kind = graph_.opKind(op);
        if (kind != OperationKind::kRegisterReadPort && kind != OperationKind::kRegisterReadArrayPort) continue;
        const auto state = graph_.findOperation(attr(op, "regSymbol"));
        const auto& key = executionOwner(op);
        if (!large(state) || !writeActivation(state) || !ordinal_.contains(key)) continue;
        const auto type = grh::stateLogicalType(graph_, state);
        if (type.shape.empty()) continue;
        const auto owner = ordinal_.at(key);
        auto& plan = candidates[state];
        plan.cells = type.width / type.leafWidth();
        if (graph_.opOperands(op).empty() || effectOwners.contains(owner)) plan.fullFanout.insert(owner);
        else plan.readers[owner].push_back(op);
    }
    for (const auto state : states_) {
        const auto found = candidates.find(state);
        if (found == candidates.end()) continue;
        auto& plan = found->second;
        for (const auto owner : stateFanout_[state])
            if (!plan.readers.contains(owner)) plan.fullFanout.insert(owner);
        for (const auto owner : plan.fullFanout) plan.readers.erase(owner);
        if (plan.readers.empty() || plan.cells >= std::numeric_limits<int64_t>::max() - 1) continue;
        // Two sentinels: N means an executed out-of-range read; N+1 means
        // no sample yet and must conservatively match every valid write.
        const auto indexType = unsignedType(std::bit_width(static_cast<uint64_t>(plan.cells + 1)));
        for (const auto& [owner, reads] : plan.readers) for (const auto read : reads) {
            int64_t count = plan.cells;
            const auto args = graph_.opOperands(read);
            const auto type = grh::stateLogicalType(graph_, state);
            for (std::size_t i = 0; i < args.size(); ++i) count /= type.shape[i];
            indexedReads_.emplace(read, IndexedRead{state, count});
            field("memoryReadIndexStore", indexType, subscriptionName(read), sourceComments_ ?
                  "last sampled leaf; N=invalid, N+1=not sampled; " + operationDescription(read) : std::string{});
        }
        field("memoryTouchedStore", "std::array<std::uint64_t," + std::to_string((plan.cells + 63) / 64) + ">", state);
        indexedStates_.emplace(state, std::move(plan));
    }
}

void CpuStEmitContext::analyzeSnapshotViews() {
    const auto localView = [&](ValueId value, const Key& key) {
        const auto producer = graph_.valueDef(value);
        return producer && executionOwner(producer) == key && !storage_.contains(value) &&
            !smallEnableBoundary_.contains(value) && !smallPayloadBoundary_.contains(value) && !graph_.valueIsOutput(value);
    };
    for (const auto op : graph_.operations()) {
        if (graph_.opKind(op) != OperationKind::kArrayCreate) continue;
        const auto id = graph_.opResults(op)[0];
        const auto type = graph_.valueLogicalType(id);
        const auto& key = executionOwner(op);
        if (type.shape.empty() || type.kind != grh::ValueType::Logic || !localView(id, key)) continue;
        ValueId source; int64_t offset = 0; bool valid = true;
        for (auto arg : graph_.opOperands(op)) {
            while (localView(arg, key) && graph_.opKind(graph_.valueDef(arg)) == OperationKind::kAssign) {
                const auto input = graph_.opOperands(graph_.valueDef(arg))[0];
                if (graph_.valueLogicalType(input) != graph_.valueLogicalType(arg)) break;
                arg = input;
            }
            const auto slice = graph_.valueDef(arg);
            const auto partType = graph_.valueLogicalType(arg);
            if (!localView(arg, key) || !partType.shape.empty() || partType != leafType(type) ||
                graph_.opKind(slice) != OperationKind::kSliceStatic ||
                sim::attribute<int64_t>(graph_, slice, "sliceStart").value_or(-1) != offset ||
                sim::attribute<int64_t>(graph_, slice, "sliceEnd").value_or(-1) != offset + partType.width - 1) { valid = false; break; }
            const auto input = graph_.opOperands(slice)[0];
            if (!storage_.contains(input) || graph_.valueLogicalType(input) != type || (source && source != input)) { valid = false; break; }
            source = input; offset += partType.width;
        }
        if (valid && source && offset == type.width) { snapshotViews_.emplace(id, source); identityElisions_.insert(op); }
    }
    // Preserve scalar view handling, while forwarding shaped aliases all the
    // way to scalar consumers such as mux through their original snapshot.
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto op : graph_.operations()) {
            if (graph_.opKind(op) != OperationKind::kAssign) continue;
            const auto id = graph_.opResults(op)[0]; const auto type = graph_.valueLogicalType(id);
            const auto& key = executionOwner(op);
            if (type.shape.empty() || snapshotViews_.contains(id) || !localView(id, key)) continue;
            auto value = graph_.opOperands(op)[0];
            while (graph_.valueShape(value).empty() && localView(value, key) &&
                   graph_.opKind(graph_.valueDef(value)) == OperationKind::kAssign) {
                const auto input = graph_.opOperands(graph_.valueDef(value))[0];
                if (scalarType(graph_.valueLogicalType(value)) != scalarType(type) ||
                    scalarType(graph_.valueLogicalType(input)) != scalarType(type)) break;
                value = input;
            }
            if (const auto found = snapshotViews_.find(value); found != snapshotViews_.end() && graph_.valueLogicalType(value) == type && localView(value, key)) {
                snapshotViews_.emplace(id, found->second); identityElisions_.insert(op); changed = true;
            }
        }
    }
}

void CpuStEmitContext::analyzeLeafPlans() {
    std::unordered_set<OperationId, grh::OperationIdHash> candidates;
    const auto dataUse = [&](OperationId op, std::size_t index) {
        return graph_.opKind(op) != OperationKind::kMux || index != 0;
    };
    for (const auto root : graph_.operations()) {
        const auto kind = graph_.opKind(root);
        if (kind != OperationKind::kAssign && kind != OperationKind::kConcat &&
            kind != OperationKind::kReplicate && kind != OperationKind::kMux) continue;
        const auto results = graph_.opResults(root);
        if (results.size() != 1 || identityElisions_.contains(root)) continue;
        const auto target = graph_.valueLogicalType(results[0]);
        if (target.kind != grh::ValueType::Logic || target.shape.empty() ||
            target.width <= 64 || target.leafWidth() > 64) continue;
        const auto& key = executionOwner(root);
        const auto compatible = [&](const LogicalType& type) {
            return type.kind == target.kind && type.domain == target.domain &&
                type.width % target.leafWidth() == 0;
        };
        std::unordered_set<OperationId, grh::OperationIdHash> followed;
        std::size_t nodes = 0;
        std::function<std::optional<LeafPlan>(ValueId, std::size_t)> valuePlan;
        std::function<std::optional<LeafPlan>(OperationId, std::size_t)> operationPlan;
        operationPlan = [&](OperationId op, std::size_t depth) -> std::optional<LeafPlan> {
            if (depth > 64 || ++nodes > 4096) return {};
            const auto args = graph_.opOperands(op);
            const auto type = graph_.valueLogicalType(graph_.opResults(op)[0]);
            if (!compatible(type)) return {};
            const auto kind = graph_.opKind(op);
            if (kind == OperationKind::kAssign) {
                if (args.size() != 1 || graph_.valueWidth(args[0]) != type.width) return {};
                return valuePlan(args[0], depth + 1);
            }
            LeafPlan plan; plan.width = type.width;
            if (kind == OperationKind::kConcat) {
                plan.kind = LeafPlan::Kind::Concat;
                int64_t width = 0;
                // The last concat operand supplies the low array leaves.
                for (std::size_t i = args.size(); i-- > 0;) {
                    auto input = valuePlan(args[i], depth + 1);
                    if (!input) return {};
                    width += input->width; plan.inputs.push_back(std::move(*input));
                }
                if (width != type.width || plan.inputs.empty()) return {};
            } else if (kind == OperationKind::kReplicate) {
                if (args.size() != 1) return {};
                auto input = valuePlan(args[0], depth + 1);
                const auto count = sim::attribute<int64_t>(graph_, op, "rep").value_or(0);
                if (!input || count <= 0 || type.width / count != input->width || type.width % count) return {};
                plan.kind = LeafPlan::Kind::Repeat; plan.inputs.push_back(std::move(*input));
            } else if (kind == OperationKind::kMux) {
                if (args.size() != 3 || graph_.valueWidth(args[1]) != type.width || graph_.valueWidth(args[2]) != type.width) return {};
                auto yes = valuePlan(args[1], depth + 1), no = valuePlan(args[2], depth + 1);
                if (!yes || !no) return {};
                plan.kind = LeafPlan::Kind::Mux; plan.source = args[0];
                plan.inputs.push_back(std::move(*yes)); plan.inputs.push_back(std::move(*no));
            } else return {};
            return plan;
        };
        valuePlan = [&](ValueId value, std::size_t depth) -> std::optional<LeafPlan> {
            const auto type = graph_.valueLogicalType(value);
            if (!compatible(type)) return {};
            if (!type.shape.empty()) {
                if (type.leafWidth() != target.leafWidth()) return {};
                return LeafPlan{LeafPlan::Kind::Source, value, type.width, {}};
            }
            const auto producer = graph_.valueDef(value);
            const bool privateValue = producer && executionOwner(producer) == key && !storage_.contains(value) &&
                !smallEnableBoundary_.contains(value) && !smallPayloadBoundary_.contains(value) && !graph_.valueIsOutput(value);
            if (privateValue) {
                auto plan = operationPlan(producer, depth);
                if (plan) { followed.insert(producer); return plan; }
            }
            // Materialized scalar snapshots remain terminals; do not chase
            // their producers or create wide right-shift loops for them.
            if (type.width <= 64) return LeafPlan{LeafPlan::Kind::Source, value, type.width, {}};
            return {};
        };
        auto plan = operationPlan(root, 0);
        if (!plan || plan->kind == LeafPlan::Kind::Source) continue;
        leafPlans_.emplace(root, std::move(*plan));
        candidates.insert(followed.begin(), followed.end());
    }
    // One target may expand a private scalar while another leaf width must
    // read its materialized value. Pin every terminal in the final plans,
    // including mux controls; a successful root alone is not use coverage.
    std::unordered_set<OperationId, grh::OperationIdHash> materialized;
    const auto retain = [&](auto&& self, const LeafPlan& plan) -> void {
        if (plan.source) materialized.insert(graph_.valueDef(plan.source));
        for (const auto& input : plan.inputs) self(self, input);
    };
    for (const auto& [op, plan] : leafPlans_) retain(retain, plan);
    // A mux condition is still read at the consumer. Only data operands of
    // a planned root or an already eliminated private node are covered.
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto op : candidates) {
            if (leafElisions_.contains(op) || materialized.contains(op)) continue;
            const auto users = graph_.valueUsers(graph_.opResults(op)[0]);
            if (users.empty()) continue;
            bool dead = true;
            for (const auto& use : users)
                dead &= executionOwner(use.operation) == executionOwner(op) && dataUse(use.operation, use.operandIndex) &&
                    (leafPlans_.contains(use.operation) || leafElisions_.contains(use.operation));
            if (dead) { leafElisions_.insert(op); changed = true; }
        }
    }
}

void CpuStEmitContext::emitLeaves(std::vector<Code>& code, const std::string& destination, const LogicalType& target,
                                const LeafPlan& plan, const std::string& offset) {
    const auto leaf = leafType(target);
    const auto count = plan.width / leaf.width;
    if (plan.kind == LeafPlan::Kind::Source) {
        const auto type = graph_.valueLogicalType(plan.source);
        const auto source = readSource(plan.source);
        const auto i = temporary("std::size_t", "leaf_index");
        const auto value = type.shape.empty() ?
            "(static_cast<std::uint64_t>(" + clean(source, type) + ") >> (" + i + " * " + std::to_string(leaf.width) + "))" :
            clean(row(source, type.shape, i), leafType(type));
        code.push_back(Code::block("for (" + i + " = 0; " + i + " < " + std::to_string(count) + "; ++" + i + ")", {
            Code::line(row(destination, target.shape, "(" + offset + "+" + i + ")") + " = " + clean(value, leaf) + ";")
        }));
    } else if (plan.kind == LeafPlan::Kind::Concat) {
        int64_t first = 0;
        for (const auto& input : plan.inputs) {
            emitLeaves(code, destination, target, input, "(" + offset + "+" + std::to_string(first) + ")");
            first += input.width / leaf.width;
        }
    } else if (plan.kind == LeafPlan::Kind::Repeat) {
        const auto i = temporary("std::size_t", "leaf_index");
        std::vector<Code> body;
        emitLeaves(body, destination, target, plan.inputs[0], "(" + offset + "+" + i + "*" + std::to_string(plan.inputs[0].width / leaf.width) + ")");
        code.push_back(Code::block("for (" + i + " = 0; " + i + " < " + std::to_string(plan.width / plan.inputs[0].width) + "; ++" + i + ")", std::move(body)));
    } else {
        const auto condition = temporary("bool", "condition");
        code.push_back(Code::line(condition + " = static_cast<bool>(" + readSource(plan.source) + ");"));
        std::vector<Code> yes, no;
        emitLeaves(yes, destination, target, plan.inputs[0], offset);
        emitLeaves(no, destination, target, plan.inputs[1], offset);
        // Each branch is a standalone statement, including when the text
        // writer moves them to separate bounded helpers. Sample once.
        code.push_back(Code::block("if (" + condition + ")", std::move(yes)));
        code.push_back(Code::block("if (!" + condition + ")", std::move(no)));
    }
}

ValueId CpuStEmitContext::arrayView(ValueId value) const {
    // Follow only equal-width views inside this execution boundary. Stored
    // scalar values are snapshots and must not be replaced by a fresh read.
    for (;;) {
        if (!graph_.valueShape(value).empty()) return value;
        if (storage_.contains(value)) return {};
        const auto producer = graph_.valueDef(value);
        if (!producer || graph_.opKind(producer) != OperationKind::kAssign) return {};
        value = graph_.opOperands(producer)[0];
    }
}

bool CpuStEmitContext::viewOnly(ValueId value) {
    if (const auto found = viewOnly_.find(value); found != viewOnly_.end()) return found->second;
    viewOnly_[value] = false;
    if (!graph_.valueShape(value).empty() || storage_.contains(value) || graph_.valueIsOutput(value)) return false;
    const auto source = arrayView(value);
    if (!source) return false;
    const auto width = graph_.valueLogicalType(source).leafWidth();
    for (const auto& user : graph_.valueUsers(value)) {
        if (graph_.opKind(user.operation) != OperationKind::kAssign) return false;
        const auto target = graph_.opResults(user.operation)[0];
        const auto type = graph_.valueLogicalType(target);
        if (type.shape.empty() ? !viewOnly(target) : type.leafWidth() != width) return false;
    }
    viewOnly_[value] = true;
    return true;
}

void CpuStEmitContext::activate(std::vector<Code>& code, const std::set<std::size_t>& destinations, std::optional<std::size_t> current) {
    std::map<std::pair<bool, std::size_t>, std::uint32_t> masks;
    for (const auto target : destinations) {
        const bool next = current && target <= *current;
        masks[{next, target / 8}] |= std::uint32_t{1} << (target % 8);
    }
    for (const auto& [key, mask] : masks) {
        const auto flags = key.first ? "activeNext" : "activeCurrent";
        const auto word = key.second;
        code.push_back(Code::line(std::string(flags) + "[" + std::to_string(word) + "] |= UINT8_C(" +
            std::to_string(mask) + ");"));
    }
}

void CpuStEmitContext::markState(std::vector<Code>& code, OperationId state) {
    const auto ordinal = stateOrdinal_.at(state);
    code.push_back(Code::line("dirtyStateFlags[" + std::to_string(ordinal / 8) + "] |= UINT8_C(" +
        std::to_string(std::uint32_t{1} << (ordinal % 8)) + ");"));
}
void CpuStEmitContext::touchState(std::vector<Code>& code, OperationId state, const std::string& start, int64_t count) {
    if (!indexedStates_.contains(state)) return;
    const auto touched = fieldRef("memoryTouchedStore", name(state));
    if (start == "0" && count == indexedStates_.at(state).cells)
        code.push_back(Code::line(touched + ".fill(~UINT64_C(0));"));
    else if (count == 1)
        code.push_back(Code::line(touched + "[(" + start + ") / 64] |= UINT64_C(1) << ((" + start + ") % 64);"));
    else code.push_back(Code::line("memory_touch_range(" + touched + ".data(), " + start + ", " + std::to_string(count) + ");"));
}
void CpuStEmitContext::activateState(std::vector<Code>& code, OperationId state) {
    const auto found = indexedStates_.find(state);
    if (found == indexedStates_.end()) {
        activate(code, stateFanout_[state], std::numeric_limits<std::size_t>::max());
        return;
    }
    const auto& plan = found->second;
    activate(code, plan.fullFanout, std::numeric_limits<std::size_t>::max());
    const auto touched = fieldRef("memoryTouchedStore", name(state));
    for (const auto& [owner, reads] : plan.readers) {
        std::string condition;
        for (const auto read : reads) {
            if (!condition.empty()) condition += " || ";
            const auto start = fieldRef("memoryReadIndexStore", subscriptionName(read));
            const auto cells = std::to_string(plan.cells);
            if (indexedReads_.at(read).count == 1)
                condition += "(" + start + " > " + cells + " || (" + start + " < " + cells + " && (" + touched +
                    "[" + start + " / 64] & (UINT64_C(1) << (" + start + " % 64)))))";
            else condition += "memory_read_touched(" + touched + ".data(), " + start + ", " +
                std::to_string(indexedReads_.at(read).count) + ", " + cells + ")";
        }
        std::vector<Code> hit;
        activate(hit, {owner}, std::numeric_limits<std::size_t>::max());
        code.push_back(Code::block("if (" + condition + ")", std::move(hit)));
    }
}
void CpuStEmitContext::syncSmallBoundary(std::vector<Code>& code, ValueId id, const std::string& source) {
    const auto sync = [&](const auto& boundaries) {
        const auto found = boundaries.find(id);
        if (found != boundaries.end() && found->second != source)
            copy(code, found->second, graph_.valueLogicalType(id), source, graph_.valueLogicalType(id));
    };
    sync(smallEnableBoundary_);
    sync(smallPayloadBoundary_);
    if (const auto found = smallEnableOrdinal_.find(id); found != smallEnableOrdinal_.end()) {
        const auto bit = found->second;
        const auto word = "smallSinkEnableBoundaryStore.words[" + std::to_string(bit / 64) + "]";
        const auto mask = "(UINT64_C(1)<<" + std::to_string(bit % 64) + ")";
        code.push_back(Code::line(word + " = (" + word + " & ~" + mask + ") | (static_cast<std::uint64_t>(" + source + ") << " + std::to_string(bit % 64) + ");"));
    }
}
bool CpuStEmitContext::operationIsElided(OperationId op) const {
    const auto kind = graph_.opKind(op);
    if (sim::isWriter(kind) || kind == sim::kHistoryWrite || identityElisions_.contains(op) || leafElisions_.contains(op)) return true;
    const auto results = graph_.opResults(op);
    return kind == OperationKind::kAssign && results.size() == 1 && viewOnly_.at(results[0]);
}
bool CpuStEmitContext::scalarPublication(ValueId id) const {
    if (!storage_.contains(id) || smallEnableOrdinal_.contains(id)) return false;
    const auto type = graph_.valueLogicalType(id);
    return type.kind == grh::ValueType::Logic && type.domain == grh::LogicDomain::TwoState &&
        type.shape.empty() && type.width > 0 && type.width <= 64;
}
void CpuStEmitContext::beginPublications(std::vector<Code>& code, const std::vector<OperationId>& ops) {
    // An external call or string allocation can leave an owner before its
    // activation flush. Keep every publication in such owners immediate.
    for (const auto op : ops) {
        if (grh::operationEffect(graph_, op) == grh::OpEffect::External) return;
        for (const auto value : graph_.opOperands(op))
            if (graph_.valueLogicalType(value).kind == grh::ValueType::String) return;
        for (const auto value : graph_.opResults(op))
            if (graph_.valueLogicalType(value).kind == grh::ValueType::String) return;
    }
    groupPublications_ = true;
    // Count actual publication sites, using the same elisions as operation().
    // The complete fanout preserves both current- and next-round routing.
    for (const auto op : ops) {
        if (operationIsElided(op)) continue;
        const auto results = graph_.opResults(op);
        if (results.size() == 1 && scalarPublication(results[0]))
            ++publicationGroups_[fanout_[results[0]]].expected;
    }
    for (auto& [destinations, group] : publicationGroups_) {
        if (destinations.empty() || group.expected < 2) continue;
        group.dirty = temporary("bool", "publication_changed");
        // This ordered assignment also initializes flags shared by chunks.
        code.push_back(Code::line(group.dirty + " = false;"));
    }
}
void CpuStEmitContext::finishPublications(std::vector<Code>& code) {
    if (!groupPublications_) return;
    for (const auto& [destinations, group] : publicationGroups_) {
        if (group.emitted != group.expected)
            throw std::logic_error("CPU publication group site count mismatch in " + function_->name);
        if (group.dirty.empty()) continue;
        std::vector<Code> changed;
        activate(changed, destinations, ordinal_.at(owner_));
        code.push_back(Code::block("if (" + group.dirty + ")", std::move(changed)));
    }
}
void CpuStEmitContext::publish(std::vector<Code>& code, ValueId id, const std::string& source) {
    const auto destination = storage_.contains(id) ? storage_.at(id) : read(id);
    if (!storage_.contains(id)) { copy(code, destination, graph_.valueLogicalType(id), source, graph_.valueLogicalType(id)); return; }
    if (owner_ == Key{"event"} && sim::isFire(graph_, id)) {
        // An event pulse activates its consumers when asserted, including
        // consecutive asserted rounds. Clearing it is not a data change.
        copy(code, destination, graph_.valueLogicalType(id), source, graph_.valueLogicalType(id));
        syncSmallBoundary(code, id, destination);
        std::vector<Code> triggered;
        activate(triggered, fanout_[id]);
        if (!triggered.empty()) code.push_back(Code::block("if (" + source + ")", std::move(triggered)));
        return;
    }
    if (groupPublications_ && scalarPublication(id)) {
        const auto& destinations = fanout_[id];
        const auto found = publicationGroups_.find(destinations);
        if (found == publicationGroups_.end())
            throw std::logic_error("CPU publication group has an unexpected site in " + function_->name);
        auto& group = found->second;
        ++group.emitted;
        if (destinations.empty() || !group.dirty.empty()) {
            if (!group.dirty.empty())
                code.push_back(Code::line(group.dirty + " |= " + destination + " != " + source + ";"));
            copy(code, destination, graph_.valueLogicalType(id), source, graph_.valueLogicalType(id));
            syncSmallBoundary(code, id, destination);
            return;
        }
    }
    std::vector<Code> changed;
    copy(changed, destination, graph_.valueLogicalType(id), source, graph_.valueLogicalType(id));
    syncSmallBoundary(changed, id, destination);
    activate(changed, fanout_[id], ordinal_.contains(owner_) ? std::optional(ordinal_.at(owner_)) : std::nullopt);
    code.push_back(Code::block("if (" + destination + " != " + source + ")", std::move(changed)));
}

std::string CpuStEmitContext::fireExpression(ValueId value) {
    if (const auto found = fireExpressions_.find(value); found != fireExpressions_.end()) return found->second;
    const auto producer = graph_.valueDef(value);
    const auto kind = graph_.opKind(producer);
    const auto args = graph_.opOperands(producer);
    std::string result;
    if (kind == sim::kEdgeDetect) result = read(value);
    else if (kind == OperationKind::kConstant) result = bits(attr(producer, "constValue"), 1, false).getRawPtr()[0] ? "true" : "false";
    else if (kind == OperationKind::kAssign) result = fireExpression(args[0]);
    else if (kind == OperationKind::kOr) result = "(" + fireExpression(args[0]) + " || " + fireExpression(args[1]) + ")";
    else throw std::invalid_argument("CPU fire has an invalid producer");
    fireExpressions_.emplace(value, result);
    return result;
}

std::string CpuStEmitContext::pack(std::vector<Code>& code, const std::string& source, const LogicalType& type, const std::string& destination) {
    if (type.shape.empty()) return source;
    code.push_back(Code::line(destination + " = 0;"));
    if (packedArrayImage(type)) {
        // Match the legacy contiguous little-endian array route. Moving the
        // whole _BitInt once per element makes this conversion quadratic.
        code.push_back(Code::line("static_assert(sizeof(" + source + ") == " + std::to_string(type.width / 8) + ");"));
        code.push_back(Code::line("std::memcpy(&(" + destination + "), &(" + source + "), " + std::to_string(type.width / 8) + ");"));
        return destination;
    }
    const auto i = temporary("std::size_t", "leaf_index");
    code.push_back(Code::block("for (" + i + " = 0; " + i + " < " + std::to_string(type.width / type.leafWidth()) + "; ++" + i + ")", {
        Code::line(destination + " |= static_cast<" + unsignedType(type.width) + ">(" + clean(row(source, type.shape, i), leafType(type)) + ") << (" + i + " * " + std::to_string(type.leafWidth()) + ");")
    }));
    return destination;
}
std::string CpuStEmitContext::packed(ValueId id, std::vector<Code>& code, bool addressable) {
    const auto type = graph_.valueLogicalType(id);
    if (type.shape.empty()) return addressable ? read(id) : readSource(id);
    const auto producer = graph_.valueDef(id);
    if (!addressable && type.kind == grh::ValueType::Logic && producer && graph_.opKind(producer) == OperationKind::kConstant) {
        auto packedType = scalarType(type); packedType.isSigned = false;
        // Constants are immutable; concatenating their cleaned leaf patterns
        // is their original normalized bit pattern, with no signed host cast.
        return literal(bits(attr(producer, "constValue"), type.width, type.isSigned), packedType);
    }
    if (const auto found = packedNames_.find(id); found != packedNames_.end()) return found->second;
    // A value can be packed independently in several static sink runs.  The
    // runs may later be grouped into one writer batch, so value-id-only names
    // would collide when their locals are merged into that batch Function.
    const auto packedName = packedPrefix(id) + std::to_string(temporary_++);
    const auto destination = local(unsignedType(type.width), packedName, "{}",
                                   sourceComments_ ? "packed bit image of " + valueName(id) : std::string{});
    packedNames_.emplace(id, destination);
    packed_.insert(id);
    pack(code, read(id), type, destination);
    return destination;
}
void CpuStEmitContext::unpack(std::vector<Code>& code, const std::string& destination, const LogicalType& type,
                             const std::string& scalar, int64_t sourceWidth) {
    if (type.shape.empty()) { code.push_back(Code::line(destination + " = " + clean(scalar, type) + ";")); return; }
    if (packedArrayImage(type) && sourceWidth >= type.width) {
        code.push_back(Code::line("static_assert(sizeof(" + destination + ") == " + std::to_string(type.width / 8) + ");"));
        code.push_back(Code::line("std::memcpy(&(" + destination + "), &(" + scalar + "), " + std::to_string(type.width / 8) + ");"));
        return;
    }
    const auto i = temporary("std::size_t", "leaf_index");
    code.push_back(Code::block("for (" + i + " = 0; " + i + " < " + std::to_string(type.width / type.leafWidth()) + "; ++" + i + ")", {
        Code::line(row(destination, type.shape, i) + " = " + clean("(" + scalar + ") >> (" + i + " * " + std::to_string(type.leafWidth()) + ")", leafType(type)) + ";")
    }));
}
void CpuStEmitContext::copy(std::vector<Code>& code, const std::string& destination, const LogicalType& target,
                           const std::string& source, const LogicalType& sourceType) {
    if (target == sourceType) { code.push_back(Code::line(destination + " = " + source + ";")); return; }
    if (target.shape.empty() && sourceType.shape.empty()) {
        code.push_back(Code::line(destination + " = " + convert(source, sourceType, target) + ";")); return;
    }
    if (target.width == sourceType.width && target.leafWidth() == sourceType.leafWidth()) {
        const auto i = temporary("std::size_t", "leaf_index");
        code.push_back(Code::block("for (" + i + " = 0; " + i + " < " + std::to_string(target.width / target.leafWidth()) + "; ++" + i + ")", {
            Code::line(row(destination, target.shape, i) + " = " + convert(row(source, sourceType.shape, i), leafType(sourceType), leafType(target)) + ";")
        })); return;
    }
    const auto scalar = sourceType.shape.empty() ? source : pack(code, source, sourceType, temporary(unsignedType(sourceType.width), "packed_source"));
    unpack(code, destination, target, scalar, sourceType.width);
}

std::string CpuStEmitContext::expression(OperationId op, ValueId result, std::vector<Code>& code) {
    const auto kind = graph_.opKind(op);
    const auto args = graph_.opOperands(op);
    const auto type = scalarType(graph_.valueLogicalType(result));
    const auto a = [&](std::size_t i) { return packed(args[i], code); };
    const auto t = [&](std::size_t i) { return scalarType(graph_.valueLogicalType(args[i])); };
    const auto c = [&](std::size_t i) { return convert(a(i), t(i), type); };
    const auto number = [&](std::string_view name) { return sim::attribute<int64_t>(graph_, op, name).value(); };
    const auto staticSlice = [&](ValueId value, int64_t offset, int64_t width) -> std::optional<std::string> {
        const auto sourceType = graph_.valueLogicalType(value);
        if (sourceType.kind != grh::ValueType::Logic || offset < 0 || width <= 0 ||
            width > sourceType.width || offset > sourceType.width - width) return std::nullopt;
        const auto leaf = leafType(sourceType);
        const auto start = sourceType.shape.empty() ? offset : offset % leaf.width;
        // Large bit arrays retain the compact pack route rather than emit
        // thousands of repeated leaf expressions for a whole-array window.
        constexpr int64_t maxWindowLeaves = 32;
        if (!sourceType.shape.empty() &&
            (offset + width - 1) / leaf.width - offset / leaf.width >= maxWindowLeaves) return std::nullopt;
        auto target = scalarType(sourceType); target.width = width; target.isSigned = false;
        const auto saved = readSource(value);
        const auto fragment = [&](int64_t index, int64_t shift) {
            const auto source = sourceType.shape.empty() ? saved :
                row(saved, sourceType.shape, std::to_string(index));
            return "(static_cast<" + unsignedType(leaf.width) + ">(" + clean(source, leaf) +
                ") >> " + std::to_string(shift) + ")";
        };
        // Array row zero contains the least significant leaf. Keep the saved
        // view and mask each leaf's semantic width before an unsigned shift.
        if (width <= leaf.width - start) return clean(fragment(offset / leaf.width, start), target);
        std::vector<std::string> pieces;
        const auto container = unsignedType(std::max<int64_t>(64, width));
        for (int64_t consumed = 0; consumed < width;) {
            const auto position = offset + consumed;
            const auto shift = position % leaf.width;
            auto piece = "static_cast<" + container + ">(" + fragment(position / leaf.width, shift) + ")";
            if (consumed) piece = "(" + piece + " << " + std::to_string(consumed) + ")";
            pieces.push_back(std::move(piece));
            consumed += std::min(width - consumed, leaf.width - shift);
        }
        // Balance the expression without allocating a source-width packed
        // temporary. Only the leaves intersecting the requested window load.
        while (pieces.size() > 1) {
            std::vector<std::string> merged;
            for (std::size_t i = 0; i < pieces.size(); i += 2)
                merged.push_back(i + 1 == pieces.size() ? std::move(pieces[i]) :
                    "(" + pieces[i] + " | " + pieces[i + 1] + ")");
            pieces = std::move(merged);
        }
        return clean(pieces.front(), target);
    };
    static const std::map<OperationKind, std::string> binary{
        {OperationKind::kAdd, "+"}, {OperationKind::kSub, "-"}, {OperationKind::kMul, "*"},
        {OperationKind::kAnd, "&"}, {OperationKind::kOr, "|"}, {OperationKind::kXor, "^"},
        {OperationKind::kLogicAnd, "&&"}, {OperationKind::kLogicOr, "||"}
    };
    if (const auto found = binary.find(kind); found != binary.end()) {
        if (type.kind == grh::ValueType::Real) {
            if (kind != OperationKind::kAdd && kind != OperationKind::kSub && kind != OperationKind::kMul)
                throw std::invalid_argument("Unsupported CPU Real operation");
            return "(" + a(0) + found->second + a(1) + ")";
        }
        const bool logical = kind == OperationKind::kLogicAnd || kind == OperationKind::kLogicOr;
        // Promote narrow operands before arithmetic to avoid signed C++ integer
        // promotion and keep unsigned overflow at the semantic result width.
        const auto lhs = logical ? a(0) : "static_cast<" + unsignedType(std::max(type.width, 64)) + ">(" + c(0) + ")";
        const auto rhs = logical ? a(1) : "static_cast<" + unsignedType(std::max(type.width, 64)) + ">(" + c(1) + ")";
        return "(" + lhs + found->second + rhs + ")";
    }
    if (kind == OperationKind::kNot || kind == OperationKind::kLogicNot) return "(" + std::string(kind == OperationKind::kNot ? "~" : "!") + a(0) + ")";
    if (kind == OperationKind::kXnor) return "~(" + c(0) + "^" + c(1) + ")";
    const auto select = [&](const std::string& condition, const std::string& yes, const std::string& no) {
        if (type.kind == grh::ValueType::Logic && type.width <= 64 && graph_.valueShape(result).empty())
            return "numeric_select(" + condition + ", " + yes + ", " + no + ")";
        return "(" + condition + " ? " + yes + " : " + no + ")";
    };
    if (kind == OperationKind::kMux) return select(a(0), c(1), c(2));
    if (kind == sim::kBitSelect) return "((" + c(0) + "&" + c(1) + ")|(~" + c(0) + "&" + c(2) + "))";
    if (kind == sim::kPrioritySelect) {
        const auto count = (args.size() - 1) / 2;
        std::string text = c(args.size() - 1);
        for (std::size_t i = count; i-- > 0;) text = select(a(i), c(count + i), text);
        return text;
    }
    static const std::map<OperationKind, std::string> compare{
        {OperationKind::kEq, "=="}, {OperationKind::kNe, "!="}, {OperationKind::kCaseEq, "=="},
        {OperationKind::kCaseNe, "!="}, {OperationKind::kWildcardEq, "=="}, {OperationKind::kWildcardNe, "!="},
        {OperationKind::kLt, "<"}, {OperationKind::kLe, "<="}, {OperationKind::kGt, ">"}, {OperationKind::kGe, ">="}
    };
    if (const auto found = compare.find(kind); found != compare.end()) {
        if (t(0).kind != grh::ValueType::Logic) return "(" + a(0) + found->second + a(1) + ")";
        const auto width = std::max(t(0).width, t(1).width);
        auto target = t(0); target.width = width; target.isSigned = t(0).isSigned && t(1).isSigned;
        const auto extend = [&](std::size_t i) {
            const auto text = convert(a(i), t(i), target);
            return target.isSigned ? signedValue(text, width) : "static_cast<" + unsignedType(width) + ">(" + text + ")";
        };
        return "(" + extend(0) + found->second + extend(1) + ")";
    }
    if (kind == OperationKind::kDiv || kind == OperationKind::kMod) {
        if (type.kind == grh::ValueType::Real) return "(" + a(0) + (kind == OperationKind::kDiv ? "/" : "%") + a(1) + ")";
        const auto lhs = "static_cast<" + unsignedType(std::max(type.width, 64)) + ">(" + c(0) + ")";
        const auto rhs = "static_cast<" + unsignedType(std::max(type.width, 64)) + ">(" + c(1) + ")";
        if (!t(0).isSigned || !t(1).isSigned)
            return "(" + rhs + " == 0 ? 0 : " + lhs + (kind == OperationKind::kDiv ? "/" : "%") + rhs + ")";
        const auto negativeLhs = "(" + signedValue(c(0), type.width) + "<0)";
        const auto negativeRhs = "(" + signedValue(c(1), type.width) + "<0)";
        const auto magnitudeLhs = "(" + negativeLhs + " ? ((0-" + lhs + ")&" + mask(type.width) + ") : " + lhs + ")";
        const auto magnitudeRhs = "(" + negativeRhs + " ? ((0-" + rhs + ")&" + mask(type.width) + ") : " + rhs + ")";
        const auto magnitude = "(" + magnitudeLhs + (kind == OperationKind::kDiv ? "/" : "%") + magnitudeRhs + ")";
        const auto negative = kind == OperationKind::kDiv ? "(" + negativeLhs + "!=" + negativeRhs + ")" : negativeLhs;
        return "(" + rhs + " == 0 ? 0 : (" + negative + " ? 0-" + magnitude + " : " + magnitude + "))";
    }
    if (kind == OperationKind::kShl || kind == OperationKind::kLShr || kind == OperationKind::kAShr) {
        const auto amount = "static_cast<" + unsignedType(t(1).width) + ">(" + a(1) + ")";
        const auto source = a(0);
        const auto converted = convert(source, t(0), type);
        const auto shifted = kind == OperationKind::kAShr ? signedValue(converted, type.width) :
            "static_cast<" + unsignedType(std::max(type.width, 64)) + ">(" + converted + ")";
        const auto overflow = kind == OperationKind::kAShr ? "(" + shifted + "<0 ? " + mask(type.width) + " : 0)" : "0";
        auto guardWidth = type.width;
        if (kind == OperationKind::kLShr && !type.isSigned && type.width < 64) {
            const auto literal = constantSources_.find(args[0]);
            // A zero-extended constant naturally shifts to zero at its logical
            // width. Guard the uint64 container instead: Clang 22 can fail
            // instruction selection for vectorized narrow constant selects.
            if (literal != constantSources_.end() && source == literal->second) guardWidth = 64;
        }
        return "(" + amount + ">=" + std::to_string(guardWidth) + " ? " + overflow + " : (" + shifted +
            (kind == OperationKind::kShl ? " << " : " >> ") + "static_cast<std::size_t>(" + amount + ")))";
    }
    if (kind >= OperationKind::kReduceAnd && kind <= OperationKind::kReduceXnor) {
        const auto raw = a(0); const auto width = t(0).width;
        if (kind == OperationKind::kReduceAnd || kind == OperationKind::kReduceNand)
            return "(" + raw + (kind == OperationKind::kReduceAnd ? "==" : "!=") + mask(width) + ")";
        if (kind == OperationKind::kReduceOr || kind == OperationKind::kReduceNor)
            return "(" + raw + (kind == OperationKind::kReduceOr ? "!=0)" : "==0)");
        std::string parity = "__builtin_parityll(static_cast<std::uint64_t>(" + raw + "))";
        for (int64_t shift = 64; shift < width; shift += 64)
            parity = "(" + parity + "^__builtin_parityll(static_cast<std::uint64_t>((" + raw + ")>>" + std::to_string(shift) + ")))";
        return kind == OperationKind::kReduceXor ? parity : "!(" + parity + ")";
    }
    if (kind == OperationKind::kConcat || kind == OperationKind::kReplicate) {
        if (kind == OperationKind::kReplicate && t(0).width == 1) return "(0-static_cast<" + unsignedType(std::max(type.width, 64)) + ">(" + a(0) + "))";
        const auto count = kind == OperationKind::kConcat ? args.size() : std::size_t(number("rep"));
        std::vector<std::pair<std::string, int64_t>> terms;
        terms.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto index = kind == OperationKind::kConcat ? i : 0;
            terms.emplace_back("static_cast<" + unsignedType(type.width) + ">(" + a(index) + ")", t(index).width);
        }
        // Keep native wide expressions, but bound parser/AST depth for large
        // concats. A left fold over XS's 1216 inputs overflows Clang's stack.
        while (terms.size() > 1) {
            std::size_t next = 0;
            for (std::size_t i = 0; i < terms.size(); i += 2) {
                if (i + 1 == terms.size()) terms[next++] = std::move(terms[i]);
                else {
                    const auto width = terms[i].second + terms[i + 1].second;
                    auto text = "((" + terms[i].first + " << " + std::to_string(terms[i + 1].second) + ") | " + terms[i + 1].first + ")";
                    terms[next++] = {std::move(text), width};
                }
            }
            terms.resize(next);
        }
        return terms.empty() ? "((" + unsignedType(type.width) + ")0)" : std::move(terms[0].first);
    }
    if (kind == OperationKind::kSliceStatic || kind == OperationKind::kSliceDynamic) {
        if (kind == OperationKind::kSliceStatic && !graph_.valueLogicalType(args[0]).shape.empty())
            if (const auto selected = staticSlice(args[0], number("sliceStart"), type.width)) return *selected;
        const auto offset = kind == OperationKind::kSliceStatic ? std::to_string(number("sliceStart")) :
            "static_cast<" + unsignedType(t(1).width) + ">(" + a(1) + ")";
        const auto sourceType = graph_.valueLogicalType(args[0]);
        if (kind == OperationKind::kSliceDynamic && !sourceType.shape.empty() && type.width == 1) {
            const auto leaf = leafType(sourceType);
            const auto index = "static_cast<std::size_t>(" + offset + ")";
            const auto source = row(readSource(args[0]), sourceType.shape, "(" + index + " / " + std::to_string(leaf.width) + ")");
            // Preserve the saved array view. Check the full-width unsigned
            // offset before narrowing it, including the packed padding bits.
            const auto selected = "(static_cast<" + unsignedType(leaf.width) + ">(" + clean(source, leaf) +
                ") >> (" + index + " % " + std::to_string(leaf.width) + "))";
            return "((" + offset + ") >= " + std::to_string(sourceType.width) + " ? 0 : (" + selected + " & 1))";
        }
        const auto source = a(0);
        const auto container = t(0).width <= 64 ? 64 : (t(0).width + 63) / 64 * 64;
        // Narrow extraction from wide storage retains the measured aligned
        // word-window route instead of materializing the full shifted object.
        const auto shifted = type.width <= 64 && t(0).width > 64 ?
            "(static_cast<unsigned __int128>(" + source + " >> (static_cast<std::size_t>(" + offset + ")&~std::size_t(63))) >> (static_cast<std::size_t>(" + offset + ")&63))" :
            "(static_cast<" + unsignedType(container) + ">(" + source + ") >> static_cast<std::size_t>(" + offset + "))";
        return "((" + offset + ") >= " + std::to_string(container) + " ? 0 : " + shifted + ")";
    }
    if (kind == sim::kMaskedOr) {
        const auto accumulator = temporary(unsignedType(type.width), "packed_bits");
        code.push_back(Code::line(accumulator + " = 0;"));
        if (const auto views = sim::attribute<std::vector<int64_t>>(graph_, op, "views")) {
            for (std::size_t i = 0; i < views->size(); i += 4) {
                const auto control = staticSlice(args[(*views)[i]], (*views)[i + 1], 1).value();
                const auto selected = staticSlice(args[(*views)[i + 2]], (*views)[i + 3], type.width);
                const auto source = selected ? *selected :
                    clean("(" + a((*views)[i + 2]) + ") >> " + std::to_string((*views)[i + 3]), type);
                code.push_back(Code::block("if (" + control + ")", {
                    Code::line(accumulator + " |= " + source + ";")
                }));
            }
        } else {
            const auto control = a(0), source = readSource(args[1]); const auto dataType = graph_.valueLogicalType(args[1]);
            const auto i = temporary("std::size_t", "leaf_index");
            code.push_back(Code::block("for (" + i + " = 0; " + i + " < " + std::to_string(t(0).width) + "; ++" + i + ")", {
                Code::block("if ((" + control + " >> " + i + ") & 1)", {
                    Code::line(accumulator + " |= " + row(source, dataType.shape, i) + ";")
                })
            }));
        }
        return accumulator;
    }
    throw std::invalid_argument("CPU operation is not implemented: " + std::string(grh::toString(kind)) + " at " + std::string(graph_.getOperation(op).symbolText()));
}

void CpuStEmitContext::operation(std::vector<Code>& code, OperationId op) {
    OperationScope origin(currentOperation_, op);
    CodeOrigin annotation{code, code.size(), "o" + std::to_string(op.index), sourceComments_ ? operationDescription(op) : std::string{}};
    const auto kind = graph_.opKind(op);
    const auto args = graph_.opOperands(op), results = graph_.opResults(op);
    if (operationIsElided(op)) return;
    countOperation(code, op);
    if (kind == sim::kSystemTask) { task(code, op); return; }
    if (kind == sim::kDpiCall) { dpi(code, op); return; }
    if (results.size() != 1) throw std::invalid_argument("CPU expects one result for compute/read operation");
    const auto id = results[0]; const auto type = graph_.valueLogicalType(id);
    if (const auto source = constantStrings_.find(id); source != constantStrings_.end()) {
        // Call at the original operation, not in hoisted local/frame initializers.
        // Each instance still publishes into its resettable boundary fields.
        if (storage_.contains(id)) publish(code, id, source->second + "()");
        else code.push_back(Code::line("(void)" + source->second + "();"));
        return;
    }
    const auto destination = storage_.contains(id) ? temporary(cppType(type), "computed", id) : read(id);
    if (const auto plan = leafPlans_.find(op); plan != leafPlans_.end()) {
        emitLeaves(code, destination, type, plan->second);
    } else if (kind == OperationKind::kConstant) {
        if (type.kind == grh::ValueType::String) code.push_back(Code::line(destination + " = " + quote(attr(op, "constValue")) + ";"));
        else if (type.kind == grh::ValueType::Real) {
                const auto raw = graph_.getOperation(op).attr("constValue");
            const auto text = std::holds_alternative<double>(*raw) ? std::to_string(std::get<double>(*raw)) : std::get<std::string>(*raw);
            code.push_back(Code::line(destination + " = " + text + ";"));
        } else {
            const auto value = bits(attr(op, "constValue"), type.width, type.isSigned);
            if (type.shape.empty()) code.push_back(Code::line(destination + " = " + literal(value, type) + ";"));
            else {
                const auto element = leafType(type);
                for (int64_t i = 0; i < type.width / element.width; ++i)
                    code.push_back(Code::line(row(destination, type.shape, std::to_string(i)) + " = " + literal(value.slice((i + 1) * element.width - 1, i * element.width), element) + ";"));
            }
        }
    } else if (kind == OperationKind::kAssign) {
        const auto view = type.shape.empty() ? ValueId{} : arrayView(args[0]);
        const auto source = view && graph_.valueLogicalType(view).leafWidth() == type.leafWidth() ? view : args[0];
        // Packed array copies may take the source address for memcpy.
        // Keep their physical scalar source instead of an rvalue literal.
        copy(code, destination, type, type.shape.empty() ? readSource(source) : read(source), graph_.valueLogicalType(source));
    }
    else if (kind == OperationKind::kArrayCreate) {
        int64_t first = 0;
        for (const auto arg : args) {
            const auto sourceType = graph_.valueLogicalType(arg);
            const auto rows = sourceType.width / sourceType.leafWidth();
            const auto i = temporary("std::size_t", "leaf_index");
            const auto source = readSource(arg);
            code.push_back(Code::block("for (" + i + " = 0; " + i + " < " + std::to_string(rows) + "; ++" + i + ")", {
                Code::line(row(destination, type.shape, "(" + i + "+" + std::to_string(first) + ")") + " = " +
                    convert(sourceType.shape.empty() ? source : row(source, sourceType.shape, i), leafType(sourceType), leafType(type)) + ";")
            })); first += rows;
        }
    } else if (kind == OperationKind::kRegisterReadPort || kind == OperationKind::kRegisterReadArrayPort || kind == OperationKind::kLatchReadPort) {
        const auto state = graph_.findOperation(attr(op, kind == OperationKind::kLatchReadPort ? "latchSymbol" : "regSymbol"));
        auto sourceType = grh::stateLogicalType(graph_, state);
        std::string source = stateRef(state), condition = "true";
        const auto subscription = indexedReads_.find(op);
        std::string flatStart = "0";
        int64_t stride = sourceType.width / sourceType.leafWidth();
        for (std::size_t i = 0; i < args.size(); ++i) {
            const auto index = "static_cast<" + unsignedType(graph_.valueWidth(args[i])) + ">(" + readSource(args[i]) + ")";
            condition += " && " + index + " < " + std::to_string(sourceType.shape[i]);
            source += "[static_cast<std::size_t>(" + index + ")]";
            stride /= sourceType.shape[i];
            if (subscription != indexedReads_.end())
                flatStart += " + static_cast<std::size_t>(" + index + ") * " + std::to_string(stride);
        }
        for (std::size_t i = 0; i < args.size(); ++i) sourceType.width /= sourceType.shape[i];
        sourceType.shape.erase(sourceType.shape.begin(), sourceType.shape.begin() + args.size());
        if (!args.empty()) {
            code.push_back(Code::line(destination + " = {};"));
            std::vector<Code> valid; copy(valid, destination, type, source, sourceType);
            if (subscription != indexedReads_.end()) {
                const auto slot = fieldRef("memoryReadIndexStore", subscriptionName(op));
                code.push_back(Code::line(slot + " = " + std::to_string(indexedStates_.at(state).cells) + ";"));
                valid.push_back(Code::line(slot + " = " + flatStart + ";"));
            }
            code.push_back(Code::block("if (" + condition + ")", std::move(valid)));
        } else copy(code, destination, type, source, sourceType);
    } else if (kind == OperationKind::kSliceArray) {
        const auto sourceType = graph_.valueLogicalType(args[0]);
        int64_t blocks = 1;
        for (std::size_t i = 1; i < args.size(); ++i) blocks *= sourceType.shape[i - 1];
        const auto index = temporary("std::size_t", "leaf_index");
        code.push_back(Code::line(index + " = 0;"));
        for (std::size_t i = 1; i < args.size(); ++i) {
            const auto raw = "static_cast<" + unsignedType(graph_.valueWidth(args[i])) + ">(" + readSource(args[i]) + ")";
            code.push_back(Code::line(index + " = (" + index + " >= " + std::to_string(blocks) + " || " + raw + " >= " + std::to_string(blocks) +
                ") ? " + std::to_string(blocks) + " : " + index + " * " + std::to_string(sourceType.shape[i - 1]) + " + static_cast<std::size_t>(" + raw + ");"));
        }
        code.push_back(Code::line(destination + " = {};"));
        const auto source = readSource(args[0]);
        std::vector<Code> valid;
        if (type.shape.empty()) valid.push_back(Code::line(destination + " = " + clean(row(source, sourceType.shape, index), type) + ";"));
        else {
            const auto i = temporary("std::size_t", "leaf_index"); const auto count = type.width / type.leafWidth();
            valid.push_back(Code::block("for (" + i + " = 0; " + i + " < " + std::to_string(count) + "; ++" + i + ")", {
                Code::line(row(destination, type.shape, i) + " = " + row(source, sourceType.shape, "(" + index + "*" + std::to_string(count) + "+" + i + ")") + ";")
            }));
        }
        code.push_back(Code::block("if (" + index + " < " + std::to_string(blocks) + ")", std::move(valid)));
    } else if (kind == sim::kEdgeDetect) {
        const auto previous = fieldRef("prevEventStore", name(op)), current = read(args[0]);
        const auto mode = attr(op, "edge");
        const auto text = mode == "posedge" ? "(!" + previous + " && " + current + ")" : mode == "negedge" ? "(" + previous + " && !" + current + ")" : "(" + previous + " != " + current + ")";
        code.push_back(Code::line(destination + " = " + text + ";"));
        code.push_back(Code::line(previous + " = " + current + ";"));
    } else if (kind == sim::kSystemFunction) {
        const auto name = attr(op, "name");
        if (name != "random" || !args.empty() || type.width > 32 || !type.shape.empty())
            throw std::invalid_argument("CPU system function is not implemented: " + name);
        // The held result is a semantic sample boundary, independent of chunking.
        std::vector<Code> sample;
        countOperation(sample, op, "effect");
        sample.push_back(Code::line(destination + " = " + clean("random_next(cpu_rng)", type) + ";"));
        publish(sample, id, destination);
        sample.push_back(Code::line(fieldRef("sampledStore", this->name(op)) + " = true;"));
        code.push_back(Code::block("if (!" + fieldRef("sampledStore", this->name(op)) + ")", std::move(sample)));
        return;
    } else {
        const auto text = expression(op, id, code);
        if (type.shape.empty()) code.push_back(Code::line(destination + " = " + clean(text, type) + ";"));
        else {
            const auto scalar = temporary(unsignedType(type.width), "packed_bits");
            code.push_back(Code::line(scalar + " = " + clean(text, scalarType(type)) + ";"));
            unpack(code, destination, type, scalar, type.width);
        }
    }
    if (storage_.contains(id)) publish(code, id, destination);
}

void CpuStEmitContext::writer(std::vector<Code>& code, OperationId op, bool commit) {
    OperationScope origin(currentOperation_, op);
    CodeOrigin annotation{code, code.size(), "o" + std::to_string(op.index), sourceComments_ ?
        std::string(commit ? "commit captured writer; " : "sample writer; ") + operationDescription(op) : std::string{}};
    countOperation(code, op, commit ? "writer-commit" : "writer-sample");
    const auto kind = graph_.opKind(op);
    const auto args = graph_.opOperands(op);
    const auto state = sim::writerTarget(graph_, op);
    const auto type = grh::stateLogicalType(graph_, state);
    const bool latch = kind == sim::kLatchWrite;
    const bool masked = latch || kind == sim::kWriteMask || kind == sim::kWriteArrayMask;
    const bool indexed = kind == sim::kWriteArray || kind == sim::kWriteArrayMask;
    const auto dataIndex = args.size() - (masked ? 2 : 1);
    const auto indexCount = indexed ? dataIndex - 2 : 0;
    const auto dataType = graph_.valueLogicalType(args[dataIndex]);
    const bool wholeArray = !type.shape.empty() && !indexed && !masked &&
        (kind == sim::kWrite || kind == sim::kFill) && dataType == type;
    const bool inPlace = large(state);
    const auto bit = inPlace && enableBitmap_ ? largeWriteOrdinal_.at(op) : 0;
    const std::string enableWord = "largeSinkEnableBoundaryStore.words[" + std::to_string(bit / 64) + "]";
    const std::string enableMask = "(UINT64_C(1)<<" + std::to_string(bit % 64) + ")";
    const std::string enableSlot = inPlace ? (enableBitmap_ ? "(" + enableWord + " & " + enableMask + ")" :
        fieldRef("largeSinkEnableBoundaryStore", captureName(op))) : std::string{};
    if (inPlace && !commit) {
        // pGeneral resets all enables before its clock-guarded sink runs.
        std::vector<Code> capture;
        for (std::size_t i = latch ? 1 : 2; i < args.size(); ++i) {
            const auto argType = graph_.valueLogicalType(args[i]);
            const std::string member = captureName(op, i);
            copy(capture, fieldRef("largeSinkPayloadBoundaryStore", member), argType, readSource(args[i]), argType);
        }
        capture.push_back(Code::line(enableBitmap_ ? enableWord + " |= " + enableMask + ";" : enableSlot + " = true;"));
        code.push_back(Code::block("if (" + (latch ? readSource(args[0]) : readSource(args[0]) + " && " + readSource(args[1])) + ")", std::move(capture)));
        return;
    }
    const auto value = [&](std::size_t i) {
        return inPlace ? fieldRef("largeSinkPayloadBoundaryStore", captureName(op, i)) : readSource(args[i], true);
    };
    std::string condition = inPlace ? enableSlot : latch ? readSource(args[0]) : "(" + readSource(args[0]) + " && " + readSource(args[1]) + ")";
    std::string start = "0";
    int64_t count = type.width / type.leafWidth();
    for (std::size_t i = 0; i < indexCount; ++i) {
        const auto index = "static_cast<" + unsignedType(graph_.valueWidth(args[2 + i])) + ">(" + value(2 + i) + ")";
        condition += " && " + index + " < " + std::to_string(type.shape[i]);
        count /= type.shape[i];
        start = "(" + start + " + static_cast<std::size_t>(" + index + ") * " + std::to_string(count) + ")";
    }
    std::vector<Code> body;
    body.push_back(Code::line("GRHSIM_PERF_COUNT(commitBatchExecCount);"));
    const auto element = leafType(type);
    const auto data = value(dataIndex);
    const auto dataPacked = dataType.shape.empty() ? data :
        dataType.leafWidth() == type.leafWidth() ? std::string{} : pack(body, data, dataType, temporary(unsignedType(dataType.width), "packed_data"));
    std::string maskData, packedMask;
    LogicalType maskType;
    if (masked) {
        maskType = graph_.valueLogicalType(args.back()); maskData = value(args.size() - 1);
        if (!maskType.shape.empty() && maskType.leafWidth() != element.width)
            packedMask = pack(body, maskData, maskType, temporary(unsignedType(maskType.width), "packed_mask"));
    }
    const auto i = temporary("std::size_t", "leaf_index");
    const auto addressedRow = "(" + start + " + " + i + ")";
    const auto destination = type.shape.empty() ? (inPlace ? stateRef(state) : nextStateRef(state)) :
        row(inPlace ? stateRef(state) : nextStateRef(state), type.shape, addressedRow);
    if (wholeArray) {
        const auto wholeDestination = inPlace ? stateRef(state) : nextStateRef(state);
        std::vector<Code> whole;
        whole.push_back(Code::line("GRHSIM_PERF_COUNT(touchedWriteCount);"));
        if (!inPlace) {
            whole.push_back(Code::line(wholeDestination + " = " + data + ";"));
            markState(whole, state);
        } else if (writeActivation(state)) {
            touchState(whole, state, "0", type.width / type.leafWidth());
            whole.push_back(Code::line(fieldRef("largeBeforeStore", name(state)) + " = true;"));
            whole.push_back(Code::line(wholeDestination + " = " + data + ";"));
            markState(whole, state);
        } else {
            const auto before = fieldRef("largeWholeBeforeStore", name(state));
            const auto dirty = fieldRef("largeWholeDirtyStore", name(state));
            whole.push_back(Code::block("if (" + data + " != " + wholeDestination + ")", {
                Code::block("if (!" + dirty + ")", {
                    Code::line(before + " = " + wholeDestination + ";")
                }),
                Code::line(wholeDestination + " = " + data + ";"),
                Code::line(dirty + " = true;"),
                Code::line("dirtyStateFlags[" + std::to_string(stateOrdinal_.at(state) / 8) + "] |= UINT8_C(" +
                    std::to_string(std::uint32_t{1} << (stateOrdinal_.at(state) % 8)) + ");")
            }));
        }
        code.push_back(Code::block("if (" + condition + ")", std::move(whole)));
        return;
    }
    const auto elementFrom = [&](const std::string& source, const LogicalType& sourceType, const std::string& packedSource, bool broadcast) {
        if (broadcast) return clean(source, element);
        if (!sourceType.shape.empty() && sourceType.leafWidth() == element.width) return clean(row(source, sourceType.shape, i), element);
        return clean("(" + (sourceType.shape.empty() ? source : packedSource) + ") >> (" + i + " * " + std::to_string(element.width) + ")", element);
    };
    const bool broadcast = kind == sim::kFill && dataType.width == type.leafWidth();
    const auto payload = elementFrom(data, dataType, dataPacked, broadcast);
    const auto changed = temporary(cppType(element), "previous_leaf");
    const auto replacement = masked ? clean("(" + destination + " & ~" + elementFrom(maskData, maskType, packedMask, false) + ") | (" + payload + " & " + elementFrom(maskData, maskType, packedMask, false) + ")", element) : payload;
    std::vector<Code> write{Code::line(changed + " = " + replacement + ";"), Code::line("GRHSIM_PERF_COUNT(touchedWriteCount);")};
    if (inPlace) {
        if (writeActivation(state)) {
            write.push_back(Code::line(fieldRef("largeBeforeStore", name(state)) + " = true;"));
            write.push_back(Code::line(destination + " = " + changed + ";"));
            markState(write, state);
        } else {
            std::vector<Code> changedWrite{
                Code::line(fieldRef("largeBeforeStore", name(state)) + ".try_emplace(" + addressedRow + ", " + destination + ");"),
                Code::line(destination + " = " + changed + ";")
            };
            markState(changedWrite, state);
            write.push_back(Code::block("if (" + changed + " != " + destination + ")", std::move(changedWrite)));
        }
    } else {
        write.push_back(Code::line(destination + " = " + changed + ";"));
        markState(write, state);
    }
    if (inPlace && writeActivation(state)) touchState(body, state, start, count);
    body.push_back(Code::block("for (" + i + " = 0; " + i + " < " + std::to_string(count) + "; ++" + i + ")", std::move(write)));
    code.push_back(Code::block("if (" + condition + ")", std::move(body)));
}

void CpuStEmitContext::dpi(std::vector<Code>& code, OperationId op) {
    const auto imported = graph_.findOperation(attr(op, "targetImportSymbol"));
    const auto directions = sim::attribute<std::vector<std::string>>(graph_, imported, "argsDirection").value();
    const auto widths = sim::attribute<std::vector<int64_t>>(graph_, imported, "argsWidth").value();
    const auto signs = sim::attribute<std::vector<bool>>(graph_, imported, "argsSigned").value();
    const auto kinds = sim::attribute<std::vector<std::string>>(graph_, imported, "argsType").value_or(std::vector<std::string>(directions.size(), "logic"));
    const auto args = graph_.opOperands(op), results = graph_.opResults(op);
    const bool returned = sim::attribute<bool>(graph_, imported, "hasReturn").value_or(false);
    const auto dpiType = [](const LogicalType& type) { return type.kind == grh::ValueType::Logic && type.width == 1 ? "std::uint8_t" : cppType(type); };
    const auto symbol = identifier(graph_.getOperation(imported).symbolText());
    std::string declaration = "extern \"C\" " + (returned ? dpiType(graph_.valueLogicalType(results[0])) : "void") + " " + symbol + "(";
    std::string call = "::" + symbol + "(";
    std::size_t input = 2, inoutInput = 2 + std::count(directions.begin(), directions.end(), "input");
    std::size_t output = returned, inoutOutput = returned + std::count(directions.begin(), directions.end(), "output");
    std::vector<Code> body;
    countOperation(body, op, "effect");
    std::vector<std::pair<ValueId, std::string>> produced;
    for (std::size_t i = 0; i < directions.size(); ++i) {
        if (i) { declaration += ","; call += ","; }
        LogicalType target{*sim::parseDpiValueType(kinds[i]), int32_t(widths[i]), signs[i], grh::LogicDomain::TwoState, {}};
        const auto base = dpiType(target);
        if (directions[i] == "input") {
            declaration += target.kind == grh::ValueType::String ? "const char*" : target.width > 64 && target.kind == grh::ValueType::Logic ? "const " + base + "&" : base;
            const auto source = args[input++];
            if (target.kind == grh::ValueType::String) call += "(" + readSource(source) + ").c_str()";
            else {
                const auto argument = temporary(base, "dpi_argument");
                body.push_back(Code::line(argument + " = " + convert(readSource(source), graph_.valueLogicalType(source), target) + ";"));
                call += argument;
            }
        } else {
            declaration += base + "*";
            const auto argument = temporary(base, "dpi_argument");
            const auto initial = directions[i] == "inout" ? convert(readSource(args[inoutInput]), graph_.valueLogicalType(args[inoutInput]), target) :
                target.kind == grh::ValueType::String ? "std::string{}" : "0";
            body.push_back(Code::line(argument + " = " + initial + ";"));
            if (directions[i] == "inout") ++inoutInput;
            produced.emplace_back(results[directions[i] == "output" ? output++ : inoutOutput++], argument);
            call += "&" + argument;
        }
    }
    declaration += ");\n"; call += ")";
    if (dpiDeclarations_.find(declaration) == std::string::npos) dpiDeclarations_ += declaration;
    if (returned) {
        const auto result = temporary(dpiType(graph_.valueLogicalType(results[0])), "dpi_return", results[0]);
        body.push_back(Code::line(result + " = " + call + ";")); produced.insert(produced.begin(), {results[0], result});
    } else body.push_back(Code::line(call + ";"));
    for (const auto& [result, source] : produced) {
        const auto normalized = temporary(cppType(graph_.valueLogicalType(result)), "normalized_return", result);
        body.push_back(Code::line(normalized + " = " + clean(source, graph_.valueLogicalType(result)) + ";"));
        publish(body, result, normalized);
    }
    code.push_back(Code::block("if (" + fireExpression(args[0]) + " && " + readSource(args[1]) + ")", std::move(body)));
}

void CpuStEmitContext::task(std::vector<Code>& code, OperationId op) {
    const auto name = attr(op, "name");
    static const std::set<std::string> supported{"display", "write", "strobe", "monitor", "fdisplay", "fwrite", "info", "warning", "error", "fatal", "finish", "stop"};
    if (!supported.contains(name) || attr(op, "procKind") == "final") throw std::invalid_argument("CPU system task is not implemented: " + name);
    const auto args = graph_.opOperands(op);
    const bool timeslot = attr(op, "grhsim.timeslot") == "event";
    std::string condition = (timeslot ? fieldRef("timeslotTriggerStore", this->name(op)) : fireExpression(args[0])) + " && " + readSource(args[1]);
    const bool once = attr(op, "procKind") == "initial";
    if (once) condition += " && !" + fieldRef("onceStore", this->name(op));
    std::vector<Code> body;
    countOperation(body, op, "effect");
    std::string call = "system_task(" + quote(name) + ", {";
    for (std::size_t i = 2; i < args.size(); ++i) {
        if (i != 2) call += ",";
        const auto type = scalarType(graph_.valueLogicalType(args[i]));
        const auto source = packed(args[i], body, true);
        call += "TaskArg{'" + std::string(type.kind == grh::ValueType::Logic ? "l" : type.kind == grh::ValueType::Real ? "r" : "s") +
            "', &" + source + ", sizeof(" + source + "), " + std::to_string(type.width) + ", " + (type.isSigned ? "true" : "false") + "}";
    }
    body.push_back(Code::line(call + "});"));
    if (once) body.push_back(Code::line(fieldRef("onceStore", this->name(op)) + " = true;"));
    code.push_back(Code::block("if (" + condition + ")", std::move(body)));
}

void CpuStEmitContext::buildPhases() {
    Function input{"pInput"};
    inFunction(input, {}, [&](auto& code) {
        for (const auto& port : graph_.inputPorts()) {
            const auto type = graph_.valueLogicalType(port.value);
            const auto incoming = temporary(cppType(type), "input_snapshot", port.value);
            if (type.shape.empty()) code.push_back(Code::line(incoming + " = " + clean("this->" + identifier(port.name), type) + ";"));
            else {
                const auto i = temporary("std::size_t", "leaf_index");
                code.push_back(Code::block("for (" + i + " = 0; " + i + " < " + std::to_string(type.width / type.leafWidth()) + "; ++" + i + ")", {
                    Code::line(row(incoming, type.shape, i) + " = " + clean(row("this->" + identifier(port.name), type.shape, i), leafType(type)) + ";")
                }));
            }
            const auto destination = read(port.value);
            std::vector<Code> changed{Code::line(destination + " = " + incoming + ";")};
            syncSmallBoundary(changed, port.value, destination);
            activate(changed, fanout_[port.value]);
            code.push_back(Code::block("if (" + destination + " != " + incoming + ")", std::move(changed)));
        }
    }); addFunction(std::move(input));

    Function event{"pEvent"};
    inFunction(event, {"event"}, [&](auto& code) {
        if (members_.contains({"event"})) for (const auto op : topological(members_.at({"event"}))) operation(code, op);
        // Expand only the verified fire OR/assign grammar here. This samples
        // sticky triggers each round even if their OR is computed in Output.
        for (const auto& [key, ops] : members_) for (const auto op : ops)
            if (attr(op, "grhsim.timeslot") == "event")
                code.push_back(Code::line(fieldRef("timeslotTriggerStore", name(op)) + " |= " + fireExpression(graph_.opOperands(op)[0]) + ";"));
    }); addFunction(std::move(event));

    Function general{"pGeneral"};
    // Keep sink locals in bounded, ordered batches. Merging every run
    // into pGeneral creates a model-wide frame when line splitting
    // moves their definitions and uses into separate helpers.
    general.zeroInitializeLocals = false;
    if (!largeWriteOrdinal_.empty()) general.body.push_back(Code::line("largeSinkEnableBoundaryStore = {};"));
    for (std::size_t ordinal = 0; ordinal < nonsink_.size(); ++ordinal) {
        const auto& key = nonsink_[ordinal];
        Function function{"sn_" + std::to_string(ordinal)};
        inFunction(function, key, [&](auto& code) {
            const auto time = local("std::uint64_t", "start");
            code.push_back(Code::block("if constexpr (WOLVRIX_GRHSIM_SN_TIMING)", {
                Code::line(time + " = profileEnabled_ ? profile_clock() : 0;")
            }));
            const auto ops = topological(members_.at(key));
            bool nonthrowingPrefix = true;
            for (const auto op : ops) {
                if (nonthrowingPrefix) for (const auto value : graph_.opResults(op))
                    if (constantSources_.contains(value)) publishedConstants_.insert(value);
                const auto effect = grh::operationEffect(graph_, op);
                if ((effect != grh::OpEffect::Pure && effect != grh::OpEffect::ReadState) ||
                    graph_.opKind(op) == sim::kSystemFunction)
                    nonthrowingPrefix = false;
                for (const auto value : graph_.opOperands(op))
                    if (graph_.valueLogicalType(value).kind != grh::ValueType::Logic) nonthrowingPrefix = false;
                for (const auto value : graph_.opResults(op))
                    if (graph_.valueLogicalType(value).kind != grh::ValueType::Logic) nonthrowingPrefix = false;
            }
            beginPublications(code, ops);
            for (const auto op : ops) operation(code, op);
            finishPublications(code);
            code.push_back(Code::block("if constexpr (WOLVRIX_GRHSIM_SN_TIMING)", {
                Code::block("if (profileEnabled_)", {
                    Code::line("snTimeTicks_[" + std::to_string(ordinal) + "] += profile_clock() - " + time + ";"),
                    Code::line("++snTimeFires_[" + std::to_string(ordinal) + "];")
                })
            }));
            code.push_back(Code::line("GRHSIM_PERF_COUNT(computeBatchExecCount);"));
        }); addFunction(std::move(function));
    }
    const auto activeWords = (nonsink_.size() + 7) / 8;
    // Keep the phase driver small.  A flat byte/bit dispatcher is still
    // ordered and observes same-round activations, but putting all words
    // directly in pGeneral would split the driver hundreds of times even
    // after sink runs have been isolated.  Each helper owns a contiguous
    // word range, so the call sequence preserves the original scan order.
    constexpr std::size_t activeBatchWords = 24;
    std::vector<std::string> activeBatchNames;
    for (std::size_t firstWord = 0, batch = 0; firstWord < activeWords; firstWord += activeBatchWords, ++batch) {
        Function active{"pGeneral_active_batch" + std::to_string(batch)};
        active.zeroInitializeLocals = false;
        const auto lastWord = std::min(activeWords, firstWord + activeBatchWords);
        for (std::size_t word = firstWord; word < lastWord; ++word) {
            std::vector<Code> wordBody;
            const auto begin = word * 8;
            const auto end = std::min(nonsink_.size(), begin + 8);
            for (std::size_t ordinal = begin; ordinal < end; ++ordinal) {
                const auto bit = std::uint32_t{1} << (ordinal % 8);
                wordBody.push_back(Code::block("if (activeCurrent[" + std::to_string(word) + "] & UINT8_C(" +
                    std::to_string(bit) + "))", {
                    Code::line("activeCurrent[" + std::to_string(word) + "] &= static_cast<std::uint8_t>(~UINT8_C(" +
                        std::to_string(bit) + "));"),
                    Code::line("sn_" + std::to_string(ordinal) + "();")
                }));
            }
            active.body.push_back(Code::block("if (activeCurrent[" + std::to_string(word) + "])" , std::move(wordBody)));
        }
        const auto name = active.name;
        addFunction(std::move(active));
        activeBatchNames.push_back(name);
    }
    std::map<Key, const sim::SinkGroup*> sinkGroups;
    for (const auto& group : schedule_.sinkGroups) sinkGroups.emplace(group.partitionKey, &group);
    const auto smallEnableBit = [&](OperationId writerOp, const Key& key) -> std::optional<std::size_t> {
        if (key[1] != "small-sink") return {};
        const auto args = graph_.opOperands(writerOp);
        const auto enable = args[graph_.opKind(writerOp) == sim::kLatchWrite ? 0 : 1];
        const auto found = smallEnableOrdinal_.find(enable);
        const auto producer = graph_.valueDef(enable);
        if (found == smallEnableOrdinal_.end() || (producer && executionOwner(producer) == key)) return {};
        return found->second;
    };
    const auto smallEnableProbe = [](const std::map<std::size_t, std::uint64_t>& masks) {
        std::string result;
        for (const auto& [word, mask] : masks) {
            if (!result.empty()) result += " | ";
            result += "(smallSinkEnableBoundaryStore.words[" + std::to_string(word) +
                "] & UINT64_C(" + std::to_string(mask) + "))";
        }
        return "(" + result + ")";
    };
    constexpr std::size_t runBatchSize = 128;
    std::size_t batchIndex = 0, batchRuns = 0;
    Function runBatch{"writer_run_batch0"};
    runBatch.zeroInitializeLocals = false;
    std::vector<std::string> writerBatchNames;
    std::set<std::string> batchFires;
    bool batchUnclocked = false;
    const auto finishBatch = [&] {
        if (batchRuns == 0) return;
        // Check stable event pulses before entering split writer helpers. A
        // guard inside every helper still fetches all of those helpers on
        // falling edges and NBA settling rounds. Never hoist payload enables:
        // an earlier run may publish a boundary read by a later run.
        if (!batchUnclocked) {
            std::string condition;
            for (const auto& fire : batchFires)
                condition += std::string(condition.empty() ? "" : " || ") + "(" + fire + ")";
            std::vector<Code> guarded;
            guarded.push_back(Code::block("if (" + condition + ")", std::move(runBatch.body)));
            runBatch.body = std::move(guarded);
        }
        const auto batchName = runBatch.name;
        addFunction(std::move(runBatch));
        writerBatchNames.push_back(batchName);
        ++batchIndex;
        batchRuns = 0;
        batchFires.clear();
        batchUnclocked = false;
        runBatch = Function{"writer_run_batch" + std::to_string(batchIndex)};
        runBatch.zeroInitializeLocals = false;
    };
    for (std::size_t index = 0; index < schedule_.generalRuns.size(); ++index) {
        const auto& run = schedule_.generalRuns[index];
        const auto batchFire = sinkGroups.at(run.partitionKey)->fire;
        const auto batchFireExpression = batchFire ? fireExpression(batchFire) : std::string{};
        if (batchFires.size() == 16 && batchFire && !batchFires.contains(batchFireExpression)) finishBatch();
        if (batchFire) batchFires.insert(batchFireExpression);
        else batchUnclocked = true;
        Function runFunction{"writer_run" + std::to_string(index)};
        runFunction.zeroInitializeLocals = false;
        bool hasObservedPayload = false;
        inFunction(runFunction, run.partitionKey, [&](auto& code) {
            std::set<uint32_t> seen;
            std::vector<OperationId> cone;
            std::function<void(ValueId)> visit = [&](ValueId value) {
                const auto producer = graph_.valueDef(value);
                if (!producer || executionOwner(producer) != run.partitionKey || !seen.insert(producer.index).second) return;
                for (const auto arg : graph_.opOperands(producer)) visit(arg);
                cone.push_back(producer);
            };
            const auto& group = *sinkGroups.at(run.partitionKey);
            for (const auto guard : group.guards) visit(guard);
            for (const auto op : topological(cone)) operation(code, op);
            cone.clear();
            for (const auto writerOp : run.orderedWriters)
                for (const auto value : graph_.opOperands(writerOp)) visit(value);
            std::vector<Code> enabled;
            const auto ordered = topological(cone);
            using EnableSet = std::map<uint32_t, ValueId>;
            struct Demand { EnableSet enables; bool unconditional = false; };
            std::unordered_map<OperationId, Demand, grh::OperationIdHash> demand;
            for (const auto op : ordered) demand.emplace(op, Demand{});
            const auto require = [&](ValueId value, const Demand& needed) {
                const auto found = demand.find(graph_.valueDef(value));
                if (found == demand.end()) return;
                auto& target = found->second;
                target.unconditional |= needed.unconditional;
                target.enables.insert(needed.enables.begin(), needed.enables.end());
            };
            for (const auto writerOp : run.orderedWriters) {
                const auto args = graph_.opOperands(writerOp);
                const bool latch = graph_.opKind(writerOp) == sim::kLatchWrite;
                const auto enable = args[latch ? 0 : 1];
                for (std::size_t i = 0; i < args.size(); ++i) {
                    Demand needed;
                    needed.enables.emplace(enable.index, enable);
                    needed.unconditional = i < (latch ? 1u : 2u);
                    require(args[i], needed);
                }
            }
            for (auto it = ordered.rbegin(); it != ordered.rend(); ++it) {
                auto& needed = demand.at(*it);
                for (const auto result : graph_.opResults(*it)) {
                    const bool observed = storage_.contains(result);
                    needed.unconditional |= observed;
                    hasObservedPayload |= observed;
                }
                if (needed.enables.empty()) needed.unconditional = true;
                for (const auto value : graph_.opOperands(*it)) require(value, needed);
            }
            for (const auto op : ordered) {
                const auto& needed = demand.at(op);
                if (needed.unconditional) operation(enabled, op);
            }
            const auto unconditionalPacked = packed_;
            const auto unconditionalPackedNames = packedNames_;
            EnableSet payloadEnables;
            std::vector<Code> sharedPayload;
            const auto flushPayload = [&] {
                if (sharedPayload.empty()) return;
                std::string condition;
                for (const auto& [index, enable] : payloadEnables)
                    condition += (condition.empty() ? "" : " || ") + readSource(enable);
                enabled.push_back(Code::block("if (" + condition + ")", std::move(sharedPayload)));
                sharedPayload.clear();
            };
            // Demand flows backwards, so every producer's guard contains its
            // consumer's guards. Keep forward order and guard-local packing.
            for (const auto op : ordered) {
                const auto& needed = demand.at(op);
                if (needed.unconditional) { flushPayload(); continue; }
                if (needed.enables != payloadEnables) flushPayload();
                if (sharedPayload.empty()) {
                    payloadEnables = needed.enables;
                    packed_ = unconditionalPacked;
                    packedNames_ = unconditionalPackedNames;
                }
                operation(sharedPayload, op);
            }
            flushPayload();
            std::vector<Code> segment;
            std::map<std::size_t, std::uint64_t> segmentMasks;
            const auto flushSegment = [&] {
                if (segment.empty()) return;
                enabled.push_back(Code::block("if (" + smallEnableProbe(segmentMasks) + ")", std::move(segment)));
                segment.clear(); segmentMasks.clear();
            };
            for (const auto writerOp : run.orderedWriters) {
                packed_ = unconditionalPacked;
                packedNames_ = unconditionalPackedNames;
                const auto args = graph_.opOperands(writerOp);
                const auto enable = args[graph_.opKind(writerOp) == sim::kLatchWrite ? 0 : 1];
                const auto bit = smallEnableBit(writerOp, run.partitionKey);
                // Keep probes bounded and never bridge a constant or a local
                // enable. In particular A/B/A priority runs stay A/B/A.
                if (!bit || (!segmentMasks.contains(*bit / 64) && segmentMasks.size() == 8)) flushSegment();
                auto& target = bit ? segment : enabled;
                if (bit) segmentMasks[*bit / 64] |= std::uint64_t{1} << (*bit % 64);
                std::vector<Code> payload;
                writer(payload, writerOp, false);
                if (std::find(group.guards.begin(), group.guards.end(), enable) != group.guards.end()) {
                    target.insert(target.end(), std::make_move_iterator(payload.begin()), std::make_move_iterator(payload.end()));
                    continue;
                }
                const auto condition = "if (" + readSource(enable) + ")";
                if (!target.empty() && target.back().scope && target.back().text == condition)
                    target.back().body.insert(target.back().body.end(), std::make_move_iterator(payload.begin()), std::make_move_iterator(payload.end()));
                else target.push_back(Code::block(condition, std::move(payload)));
            }
            flushSegment();
            if (group.guards.empty()) code.insert(code.end(), std::make_move_iterator(enabled.begin()), std::make_move_iterator(enabled.end()));
            else {
                std::string condition;
                for (const auto guard : group.guards) condition += (condition.empty() ? "" : " && ") + readSource(guard);
                code.push_back(Code::block("if (" + condition + ")", std::move(enabled)));
            }
        });
        runBatch.locals.insert(runBatch.locals.end(),
            std::make_move_iterator(runFunction.locals.begin()),
            std::make_move_iterator(runFunction.locals.end()));
        auto runBody = std::move(runFunction.body);
        const auto& group = *sinkGroups.at(run.partitionKey);
        std::string guard = group.fire ? fireExpression(group.fire) : std::string{};
        const auto boundaryGuard = [&](ValueId value) {
            const auto producer = graph_.valueDef(value);
            return storage_.contains(value) && (!producer || executionOwner(producer) != run.partitionKey);
        };
        if (!hasObservedPayload && run.partitionKey[1] == "small-sink") {
            std::map<std::size_t, std::uint64_t> masks;
            bool eligible = true;
            for (const auto writerOp : run.orderedWriters) {
                const auto bit = smallEnableBit(writerOp, run.partitionKey);
                if (!bit) { eligible = false; break; }
                masks[*bit / 64] |= std::uint64_t{1} << (*bit % 64);
            }
            if (eligible && masks.size() <= 8) guard += (guard.empty() ? "" : " && ") + smallEnableProbe(masks);
        }
        if (std::all_of(group.guards.begin(), group.guards.end(), boundaryGuard))
            for (const auto value : group.guards) guard += (guard.empty() ? "" : " && ") + storage_.at(value);
        if (group.guards.empty() && !hasObservedPayload) {
            const auto enableOf = [&](OperationId writer) {
                return graph_.opOperands(writer)[graph_.opKind(writer) == sim::kLatchWrite ? 0 : 1];
            };
            const auto enable = enableOf(run.orderedWriters.front());
            if (boundaryGuard(enable) && std::all_of(run.orderedWriters.begin(), run.orderedWriters.end(),
                    [&](OperationId writer) { return enableOf(writer) == enable; }))
                guard += (guard.empty() ? "" : " && ") + storage_.at(enable);
            else {
                // Distinct local enables often share a cheap boundary gate:
                // gate & wide_bits[index]. Probe their common bool factors
                // before evaluating any private enable or payload cone. Keep
                // the probe at this run's original position so earlier runs
                // can still publish boundary values used by a later run.
                const auto factors = [&](ValueId root) {
                    std::map<uint32_t, ValueId> result;
                    std::set<uint32_t> seen;
                    const auto visitFactor = [&](auto&& self, ValueId value) -> void {
                        const auto type = graph_.valueLogicalType(value);
                        if (type.kind != grh::ValueType::Logic || type.domain != grh::LogicDomain::TwoState ||
                            type.width != 1 || !type.shape.empty() || !seen.insert(value.index).second) return;
                        const auto producer = graph_.valueDef(value);
                        if (producer && graph_.opKind(producer) == OperationKind::kConstant) return;
                        if (boundaryGuard(value)) {
                            result.emplace(value.index, value);
                            return;
                        }
                        if (!producer || executionOwner(producer) != run.partitionKey) return;
                        const auto kind = graph_.opKind(producer);
                        if (kind != OperationKind::kAnd && kind != OperationKind::kLogicAnd &&
                            kind != OperationKind::kAssign) return;
                        for (const auto value : graph_.opOperands(producer)) self(self, value);
                    };
                    visitFactor(visitFactor, root);
                    return result;
                };
                auto common = factors(enable);
                for (std::size_t writer = 1; writer < run.orderedWriters.size() && !common.empty(); ++writer) {
                    const auto next = factors(enableOf(run.orderedWriters[writer]));
                    std::erase_if(common, [&](const auto& factor) { return !next.contains(factor.first); });
                }
                for (const auto& [index, factor] : common)
                    guard += (guard.empty() ? "" : " && ") + storage_.at(factor);
            }
        }
        if (guard.empty()) runBatch.body.insert(runBatch.body.end(),
            std::make_move_iterator(runBody.begin()), std::make_move_iterator(runBody.end()));
        else {
            const auto condition = "if (" + guard + ")";
            if (!runBatch.body.empty() && runBatch.body.back().scope && runBatch.body.back().text == condition)
                runBatch.body.back().body.insert(runBatch.body.back().body.end(),
                    std::make_move_iterator(runBody.begin()), std::make_move_iterator(runBody.end()));
            else runBatch.body.push_back(Code::block(condition, std::move(runBody)));
        }
        if (++batchRuns == runBatchSize) finishBatch();
    }
    finishBatch();
    // Keep pGeneral itself a small phase driver.  A direct call per batch
    // is a valid sequence, but a large generated list would itself exceed
    // max-function-lines and make the writer create pGeneral_chunkN with
    // an empty frame.  The initializer-list loop preserves the exact
    // static order while keeping the driver at a constant size.
    const auto appendDispatch = [&](const std::vector<std::string>& names) {
        if (names.empty()) return;
        std::string header = "for (const auto phase : {";
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (i) header += ",";
            header += "&" + class_ + "::" + names[i];
        }
        header += "})";
        general.body.push_back(Code::block(header, {Code::line("(this->*phase)();")}));
    };
    activeBatchNames.insert(activeBatchNames.end(), writerBatchNames.begin(), writerBatchNames.end());
    appendDispatch(activeBatchNames);
    addFunction(std::move(general));

    Function largeCommit{"pLargeSink"};
    // Keep commit locals in bounded batches as well, so pLargeSink
    // remains an ordered driver without a model-wide spill frame.
    largeCommit.zeroInitializeLocals = false;
    constexpr std::size_t commitBatchSize = 128;
    std::size_t commitBatchIndex = 0, commitBatchRuns = 0;
    Function commitBatch{"commit_run_batch0"};
    commitBatch.zeroInitializeLocals = false;
    std::vector<std::string> commitBatchNames;
    std::set<std::size_t> commitBatchWords;
    const auto finishCommitBatch = [&] {
        if (commitBatchRuns == 0) return;
        if (enableBitmap_) {
            std::string condition;
            for (const auto word : commitBatchWords)
                condition += std::string(condition.empty() ? "" : " | ") +
                    "largeSinkEnableBoundaryStore.words[" + std::to_string(word) + "]";
            std::vector<Code> guarded;
            guarded.push_back(Code::block("if (" + condition + ")", std::move(commitBatch.body)));
            commitBatch.body = std::move(guarded);
        }
        const auto batchName = commitBatch.name;
        addFunction(std::move(commitBatch));
        commitBatchNames.push_back(batchName);
        ++commitBatchIndex;
        commitBatchRuns = 0;
        commitBatchWords.clear();
        commitBatch = Function{"commit_run_batch" + std::to_string(commitBatchIndex)};
        commitBatch.zeroInitializeLocals = false;
    };
    for (std::size_t index = 0; index < schedule_.largeCommitRuns.size(); ++index) {
        const auto& run = schedule_.largeCommitRuns[index];
        std::set<std::size_t> runWords;
        for (const auto writerOp : run.orderedWriters) runWords.insert(largeWriteOrdinal_.at(writerOp) / 64);
        if (enableBitmap_ && commitBatchRuns != 0) {
            auto combinedWords = commitBatchWords;
            combinedWords.insert(runWords.begin(), runWords.end());
            if (combinedWords.size() > 8) finishCommitBatch();
        }
        commitBatchWords.insert(runWords.begin(), runWords.end());
        Function runFunction{"commit_run" + std::to_string(index)};
        runFunction.zeroInitializeLocals = false;
        inFunction(runFunction, run.partitionKey, [&](auto& code) {
            for (const auto writerOp : run.orderedWriters) {
                std::vector<Code> committed;
                writer(committed, writerOp, true);
                if (enableBitmap_) {
                    const auto word = largeWriteOrdinal_.at(writerOp) / 64;
                    const auto condition = "if (largeSinkEnableBoundaryStore.words[" + std::to_string(word) + "] != 0)";
                    if (!code.empty() && code.back().scope && code.back().text == condition)
                        code.back().body.insert(code.back().body.end(), std::make_move_iterator(committed.begin()), std::make_move_iterator(committed.end()));
                    else code.push_back(Code::block(condition, std::move(committed)));
                } else code.insert(code.end(), std::make_move_iterator(committed.begin()), std::make_move_iterator(committed.end()));
            }
        });
        commitBatch.locals.insert(commitBatch.locals.end(),
            std::make_move_iterator(runFunction.locals.begin()),
            std::make_move_iterator(runFunction.locals.end()));
        auto runBody = std::move(runFunction.body);
        auto* dispatch = &commitBatch.body;
        const auto fire = sinkGroups.at(run.partitionKey)->fire;
        if (fire) {
            const auto condition = "if (" + fireExpression(fire) + ")";
            if (dispatch->empty() || !dispatch->back().scope || dispatch->back().text != condition)
                dispatch->push_back(Code::block(condition, {}));
            dispatch = &dispatch->back().body;
        }
        const auto firstWord = largeWriteOrdinal_.at(run.orderedWriters.front()) / 64;
        const auto lastWord = largeWriteOrdinal_.at(run.orderedWriters.back()) / 64;
        if (enableBitmap_ && firstWord == lastWord) {
            const auto condition = "if (largeSinkEnableBoundaryStore.words[" + std::to_string(firstWord) + "] != 0)";
            if (!dispatch->empty() && dispatch->back().scope && dispatch->back().text == condition)
                dispatch->back().body.insert(dispatch->back().body.end(),
                    std::make_move_iterator(runBody.begin()), std::make_move_iterator(runBody.end()));
            else dispatch->push_back(Code::block(condition, std::move(runBody)));
        } else dispatch->insert(dispatch->end(), std::make_move_iterator(runBody.begin()),
            std::make_move_iterator(runBody.end()));
        if (++commitBatchRuns == commitBatchSize) finishCommitBatch();
    }
    finishCommitBatch();
    if (!commitBatchNames.empty()) {
        std::string header = "for (const auto phase : {";
        for (std::size_t i = 0; i < commitBatchNames.size(); ++i) {
            if (i) header += ",";
            header += "&" + class_ + "::" + commitBatchNames[i];
        }
        header += "})";
        largeCommit.body.push_back(Code::block(header, {Code::line("(this->*phase)();")}));
    }
    addFunction(std::move(largeCommit));

    Function publishState{"pPublish"};
    publishState.body.push_back(Code::line("pending_ = false;"));
    // Keep cold state-publish bodies behind a compact dirty-byte probe.
    // Splitting one model-wide publish function otherwise calls every chunk
    // each round, even when almost all of their state bytes are clean.
    constexpr std::size_t publishBatchWords = 16;
    const auto dirtyWords = (states_.size() + 7) / 8;
    for (std::size_t firstWord = 0, batch = 0; firstWord < dirtyWords; firstWord += publishBatchWords, ++batch) {
        const auto lastWord = std::min(dirtyWords, firstWord + publishBatchWords);
        Function dirtyBatch{"pPublish_dirty_batch" + std::to_string(batch)};
        dirtyBatch.zeroInitializeLocals = false;
        inFunction(dirtyBatch, {}, [&](auto& code) {
            std::vector<std::pair<std::size_t, std::vector<Code>>> dirtyCases;
            for (std::size_t ordinal = firstWord * 8; ordinal < std::min(states_.size(), lastWord * 8); ++ordinal) {
                const auto state = states_[ordinal];
                OperationScope origin(currentOperation_, state);
                const auto index = stateOrdinal_.at(state);
                if (attr(state, "grhsim.history") == "output-end") {
                    // Keep the ordinal table dense.  Output-end history states do
                    // not publish, but their reserved bit still has to be
                    // consumed by the packed scan if a writer ever marks it.
                    dirtyCases.emplace_back(index, std::vector<Code>{Code::line(
                        "dirtyStateFlags[" + std::to_string(index / 8) + "] &= static_cast<std::uint8_t>(~UINT8_C(" +
                        std::to_string(std::uint32_t{1} << (index % 8)) + "));")});
                    continue;
                }
                std::vector<Code> body;
                body.push_back(Code::line("dirtyStateFlags[" + std::to_string(index / 8) + "] &= static_cast<std::uint8_t>(~UINT8_C(" +
                    std::to_string(std::uint32_t{1} << (index % 8)) + "));"));
                const bool observed = !stateFanout_[state].empty() || stateRound_.contains(state);
                // A write-activated memory is an observable scheduling event even
                // when this particular state currently has no value fanout.  Keep
                // the extra fixed-point round so the activation semantics remain
                // stable as readers are added or optimized away.
                const bool requiresRound = observed || writeActivation(state);
                const auto changed = [&] {
                    std::vector<Code> result{Code::line("GRHSIM_PERF_COUNT(touchedStateShadowCount);")};
                    if (requiresRound) {
                        result.push_back(Code::line("pending_ = true;"));
                        activateState(result, state);
                    }
                    return result;
                };
                if (large(state)) {
                    const auto type = grh::stateLogicalType(graph_, state);
                    const auto before = fieldRef("largeBeforeStore", name(state));
                    if (writeActivation(state)) {
                        body.push_back(Code::block("if (" + before + ")", changed()));
                        body.push_back(Code::line(before + " = false;"));
                        if (indexedStates_.contains(state))
                            body.push_back(Code::line(fieldRef("memoryTouchedStore", name(state)) + ".fill(0);"));
                    } else {
                        if (wholeArrayStates_.contains(state)) {
                            const auto wholeBefore = fieldRef("largeWholeBeforeStore", name(state));
                            const auto wholeDirty = fieldRef("largeWholeDirtyStore", name(state));
                            body.push_back(Code::block("if (" + wholeDirty + ")", {
                                Code::block("if (" + stateRef(state) + " != " + wholeBefore + ")", changed()),
                                Code::line(wholeDirty + " = false;")
                            }));
                        }
                        const auto iterator = temporary("std::map<std::size_t," + cppType(leafType(type)) + ">::const_iterator", "before_image_iterator");
                        const auto cell = type.shape.empty() ? stateRef(state) : row(stateRef(state), type.shape, iterator + "->first");
                        body.push_back(Code::block("for (" + iterator + " = " + before + ".begin(); " + iterator + " != " + before + ".end(); ++" + iterator + ")", {
                            Code::block("if (" + cell + " != " + iterator + "->second)", changed())
                        }));
                        body.push_back(Code::line(before + ".clear();"));
                    }
                } else {
                    auto stateChanged = changed();
                    stateChanged.push_back(Code::line(stateRef(state) + " = " + nextStateRef(state) + ";"));
                    body.push_back(Code::block("if (" + stateRef(state) + " != " + nextStateRef(state) + ")", std::move(stateChanged)));
                }
                CodeOrigin{body, 0, "o" + std::to_string(state.index), sourceComments_ ?
                    "publish dirty state; " + operationDescription(state) : std::string{}};
                dirtyCases.emplace_back(index, std::move(body));
            }
            // State dirty bits use the same flag-only discipline as gsim's
            // activity bitmap.  Writers set a bit; publish scans packed bytes and
            // clears each bit before publishing it.  There is no side queue whose
            // ordering could diverge from the static state ordinal order.
            for (std::size_t word = firstWord; word < lastWord; ++word) {
                std::vector<Code> wordBody;
                const auto begin = word * 8;
                const auto end = std::min(states_.size(), begin + 8);
                for (std::size_t ordinal = begin; ordinal < end; ++ordinal) {
                    auto body = std::move(dirtyCases[ordinal - firstWord * 8].second);
                    const auto bit = std::uint32_t{1} << (ordinal % 8);
                    wordBody.push_back(Code::block("if (dirtyStateFlags[" + std::to_string(word) + "] & UINT8_C(" +
                        std::to_string(bit) + "))", std::move(body)));
                }
                code.push_back(Code::block("if (dirtyStateFlags[" + std::to_string(word) + "])" , std::move(wordBody)));
            }
        });
        const auto batchName = dirtyBatch.name;
        addFunction(std::move(dirtyBatch));
        std::string probe;
        for (std::size_t word = firstWord; word < lastWord; ++word) {
            if (!probe.empty()) probe += " | ";
            probe += "dirtyStateFlags[" + std::to_string(word) + "]";
        }
        publishState.body.push_back(Code::block("if (" + probe + ")", {Code::line(batchName + "();")}));
    }
    // pGeneral consumes all current activity bits.  Move the deferred
    // bitmap into the next round and leave its source clear.  The byte
    // scan keeps this transfer proportional to the bitmap words and does
    // not construct an activation queue.
    publishState.body.push_back(Code::block("for (std::size_t activeWord = 0; activeWord < activeCurrent.size(); ++activeWord)", {
        Code::line("activeCurrent[activeWord] = activeNext[activeWord];"),
        Code::line("activeNext[activeWord] = 0;"),
        Code::line("pending_ |= activeCurrent[activeWord] != 0;")
    }));
    addFunction(std::move(publishState));

    Function output{"pOutput"};
    inFunction(output, {"output"}, [&](auto& code) {
        if (members_.contains({"output"})) for (const auto op : topological(members_.at({"output"}))) operation(code, op);
        for (const auto& port : graph_.outputPorts()) copy(code, "this->" + identifier(port.name), graph_.valueLogicalType(port.value), read(port.value), graph_.valueLogicalType(port.value));
        std::vector<std::pair<OperationId, std::string>> staged;
        for (const auto op : histories_) {
            OperationScope origin(currentOperation_, op);
            CodeOrigin annotation{code, code.size(), "o" + std::to_string(op.index), sourceComments_ ?
                "snapshot history after all output observations; " + operationDescription(op) : std::string{}};
            countOperation(code, op, "history-write");
            const auto state = sim::writerTarget(graph_, op);
            const auto type = grh::stateLogicalType(graph_, state);
            const auto snapshot = temporary(cppType(type), "history_snapshot");
            copy(code, snapshot, type, read(graph_.opOperands(op)[0]), type);
            staged.emplace_back(op, snapshot);
        }
        for (const auto& [op, snapshot] : staged) {
            const auto state = sim::writerTarget(graph_, op);
            CodeOrigin annotation{code, code.size(), "o" + std::to_string(op.index), sourceComments_ ?
                "commit staged history; " + operationDescription(op) : std::string{}};
            code.push_back(Code::line(stateRef(state) + " = " + snapshot + ";"));
            if (!large(state)) code.push_back(Code::line(nextStateRef(state) + " = " + snapshot + ";"));
        }
    }); addFunction(std::move(output));

    Function eval{"eval"};
    inFunction(eval, {}, [&](auto& code) {
        if (!counterSnapshot_.empty()) code.push_back(Code::line("++opExecEvals_;"));
        code.push_back(Code::line("sampledStore = {};"));
        code.push_back(Code::line("timeslotTriggerStore = {};"));
        code.push_back(Code::line("GRHSIM_PERF_COUNT(evalCount);"));
        for (std::size_t ordinal = 0; ordinal < nonsink_.size(); ++ordinal) {
            const auto& ops = members_.at(nonsink_[ordinal]);
            if (std::any_of(ops.begin(), ops.end(), [&](OperationId op) {
                    return graph_.opKind(op) == sim::kSystemFunction && sim::systemFunctionEffect(graph_, op) != "pure";
                }))
                activate(code, {ordinal});
        }
        code.push_back(Code::line("pInput();"));
        const auto round = local("std::size_t", "round");
        code.push_back(Code::line(round + " = 0;"));
        code.push_back(Code::line("pending_ = true;"));
        code.push_back(Code::block("while (pending_)", {
            Code::line("if (++" + round + " > " + std::to_string(maxRounds_) + ") throw std::runtime_error(\"grhsim fixed-point round limit exceeded\");"),
            Code::line("pEvent();"), Code::line("pGeneral();"), Code::line("pLargeSink();"), Code::line("pPublish();"),
            Code::line("GRHSIM_PERF_COUNT(totalRoundCount);")
        }));
        code.push_back(Code::line("if (" + round + " == 1) GRHSIM_PERF_COUNT(round1Count);"));
        code.push_back(Code::line("if (" + round + " == 2) GRHSIM_PERF_COUNT(round2Count);"));
        code.push_back(Code::line("pOutput();"));
        if (waveform_) code.push_back(Code::line("dump_waveform();"));
    }); addFunction(std::move(eval), true);
}

void CpuStEmitContext::buildConstantSources() {
    // Definitions alone do not construct the local static. The original
    // constant operation remains the first call in each execution path.
    for (const auto op : graph_.operations()) {
        if (graph_.opKind(op) != OperationKind::kConstant) continue;
        const auto id = graph_.opResults(op)[0];
        const auto found = constantStrings_.find(id);
        if (found == constantStrings_.end()) continue;
        Function function{found->second, "", "const std::string&"};
        function.body = {
            Code::line("static const std::string value(" + quote(attr(op, "constValue")) + ");"),
            Code::line("return value;", true)
        };
        CodeOrigin{function.body, 0, "o" + std::to_string(op.index),
            sourceComments_ ? operationDescription(op) : std::string{}};
        addFunction(std::move(function));
    }
}

void CpuStEmitContext::buildInit() {
    Function init{"init"};
    inFunction(init, {}, [&](auto& code) {
        if (!counterSnapshot_.empty()) {
            code.push_back(Code::line("opExecCounts_.fill(0);"));
            code.push_back(Code::line("opExecEvals_ = 0;"));
        }
        code.push_back(Code::line("cpu_rng = UINT64_C(0x6a09e667f3bcc909);"));
        code.push_back(Code::line("perf_ = {};"));
        code.push_back(Code::line("snTimeTicks_.fill(0);"));
        code.push_back(Code::line("snTimeFires_.fill(0);"));
        code.push_back(Code::line("activeCurrent.fill(0);"));
        code.push_back(Code::line("activeNext.fill(0);"));
        for (std::size_t ordinal = 0; ordinal < nonsink_.size(); ++ordinal) {
            const auto word = ordinal / 8;
            const auto bit = std::uint32_t{1} << (ordinal % 8);
            code.push_back(Code::line("activeCurrent[" + std::to_string(word) + "] |= UINT8_C(" +
                std::to_string(bit) + ");"));
        }
        code.push_back(Code::line("dirtyStateFlags.fill(0);"));
        code.push_back(Code::line("sampledStore = {};"));
        code.push_back(Code::line("timeslotTriggerStore = {};"));
        code.push_back(Code::line("onceStore = {};"));
        const auto resetPort = [&](const grh::Port& port) {
            const auto target = "this->" + identifier(port.name);
            if (graph_.valueType(port.value) == grh::ValueType::String)
                code.push_back(Code::line(target + ".clear();"));
            else code.push_back(Code::line("std::memset(&" + target + ", 0, sizeof(" + target + "));"));
        };
        for (const auto& port : graph_.inputPorts()) resetPort(port);
        for (const auto& port : graph_.outputPorts()) resetPort(port);
        for (const auto& [store, fields] : fields_) {
            if (store == "smallSinkStore" || store == "largeSinkStore" || store == "prevEventStore" || store == "largeBeforeStore") continue;
            for (const auto& field : fieldLayouts_.at(store).physicalFields) {
                const auto target = store + "." + field.name;
                if (field.type == "std::string") code.push_back(Code::line(target + ".clear();"));
                else if (field.type.find("std::string") != std::string::npos) code.push_back(Code::line(target + ".fill({});"));
                else code.push_back(Code::line("std::memset(&" + target + ", 0, sizeof(" + target + "));"));
            }
        }
        for (const auto& [op, subscription] : indexedReads_) {
            CodeOrigin annotation{code, code.size(), "o" + std::to_string(op.index), sourceComments_ ?
                "initialize indexed-read subscription; " + operationDescription(op) : std::string{}};
            code.push_back(Code::line(fieldRef("memoryReadIndexStore", subscriptionName(op)) + " = " +
                std::to_string(indexedStates_.at(subscription.state).cells + 1) + ";"));
        }
        for (const auto state : states_) {
            OperationScope origin(currentOperation_, state);
            CodeOrigin annotation{code, code.size(), "o" + std::to_string(state.index), sourceComments_ ?
                "initialize state; " + operationDescription(state) : std::string{}};
            const auto type = grh::stateLogicalType(graph_, state);
            const auto element = leafType(type);
            const auto slot = stateRef(state);
            if (type.kind != grh::ValueType::Logic) {
                if (attr(state, "grhsim.history") != "output-end") throw std::invalid_argument("Unsupported non-Logic initializer");
                code.push_back(Code::line(slot + " = {};")); continue;
            }
            code.push_back(Code::line("std::memset(&" + slot + ", 0, sizeof(" + slot + "));"));
            if (large(state)) code.push_back(Code::line(fieldRef("largeBeforeStore", name(state)) + (writeActivation(state) ? " = false;" : ".clear();")));
            const auto rows = type.width / element.width;
            std::vector<std::pair<int64_t, int64_t>> covered;
            bool dirty = false;
            for (const auto& step : grh::stateInitSpec(graph_, state)) {
                const auto first = step.start.value_or(0), count = step.count.value_or(rows - first);
                const auto finish = first + count;
                if (first < 0 || first > rows || count < 0 || count > rows - first) throw std::invalid_argument("CPU initializer range exceeds state");
                if (step.kind == grh::InitKind::Constant) {
                    covered.emplace_back(0, rows);
                    const auto value = bits(step.value, type.width, type.isSigned);
                    if (value == 0 && !dirty) continue;
                    for (int64_t i = 0; i < rows; ++i) {
                        const auto cell = type.shape.empty() ? slot : row(slot, type.shape, std::to_string(i));
                        code.push_back(Code::line(cell + " = " + literal(value.slice((i + 1) * element.width - 1, i * element.width), element) + ";"));
                    }
                    dirty = true;
                } else if (step.kind == grh::InitKind::Fill || step.kind == grh::InitKind::Random) {
                    covered.emplace_back(first, finish);
                    const bool random = step.kind == grh::InitKind::Random || step.random;
                    const auto value = random ? slang::SVInt(element.width, 0, element.isSigned) : bits(step.value, element.width, element.isSigned);
                    if (!random && value == 0 && !dirty) continue;
                    const auto index = temporary("std::size_t", "leaf_index");
                    const auto cell = type.shape.empty() ? slot : row(slot, type.shape, index);
                    std::string rng = "cpu_rng";
                    if (step.seed) {
                        rng = temporary("std::uint64_t", "initial_rng");
                        code.push_back(Code::line(rng + " = UINT64_C(" + std::to_string(uint64_t(*step.seed)) + ");"));
                    }
                    std::vector<Code> fill;
                    if (random && element.width > 64) {
                        fill.push_back(Code::line(cell + " = 0;"));
                        for (int64_t shift = 0; shift < element.width; shift += 64)
                            fill.push_back(Code::line(cell + " |= static_cast<" + unsignedType(element.width) + ">(random_next(" + rng + ")) << " + std::to_string(shift) + ";"));
                        fill.push_back(Code::line(cell + " = " + clean(cell, element) + ";"));
                    } else fill.push_back(Code::line(cell + " = " + (random ? clean("random_next(" + rng + ")", element) : literal(value, element)) + ";"));
                    code.push_back(Code::block("for (" + index + " = " + std::to_string(first) + "; " + index + " < " + std::to_string(finish) + "; ++" + index + ")", std::move(fill)));
                    dirty = true;
                } else if (step.kind == grh::InitKind::Readmem) {
                    std::ifstream stream(step.file, std::ios::binary);
                    if (!stream) throw std::invalid_argument("CPU readmem failed to open: " + step.file);
                    const std::string text{std::istreambuf_iterator<char>(stream), {}};
                    if (stream.bad()) throw std::invalid_argument("CPU readmem failed to read: " + step.file);
                    uint64_t hash = 14695981039346656037ull;
                    for (const unsigned char ch : text) { hash ^= ch; hash *= 1099511628211ull; }
                    initFiles_[step.file] = std::to_string(hash);
                    std::vector<std::string> tokens; std::string error;
                    if (!tokenizeReadmemText(text, tokens, error)) throw std::invalid_argument(step.file + ": " + error);
                    std::size_t address = first;
                    for (const auto& token : tokens) {
                        if (token.starts_with('@')) {
                            const auto parsed = parseReadmemAddress(std::string_view(token).substr(1));
                            if (!parsed) throw std::invalid_argument("Invalid readmem address in " + step.file + ": " + token);
                            address = *parsed; continue;
                        }
                        bool digit = false;
                        for (const unsigned char ch : token) {
                            if (ch == '_') continue;
                            if (ch != 'x' && ch != 'X' && ch != 'z' && ch != 'Z' && ch != '?' &&
                                !(step.format == "hex" ? std::isxdigit(ch) : ch == '0' || ch == '1'))
                                throw std::invalid_argument("Invalid readmem data in " + step.file + ": " + token);
                            digit = true;
                        }
                        if (!digit) throw std::invalid_argument("Empty readmem token in " + step.file);
                        if (address >= std::size_t(first) && address < std::size_t(finish)) {
                            auto value = slang::SVInt::fromString(std::to_string(element.width) + (step.format == "hex" ? "'h" : "'b") + token);
                            value.flattenUnknowns();
                            const auto cell = row(slot, type.shape, std::to_string(address));
                            code.push_back(Code::line(cell + " = " + literal(value, element) + ";"));
                            covered.emplace_back(address, address + 1);
                        }
                        if (address != SIZE_MAX) ++address;
                    }
                    dirty = true;
                }
            }
            std::sort(covered.begin(), covered.end()); int64_t end = 0;
            for (const auto& [first, last] : covered) { if (first > end) break; end = std::max(end, last); }
            if (end != rows) throw std::invalid_argument("CPU initializer leaves uncovered rows: " + std::string(graph_.getOperation(state).symbolText()));
        }
        code.push_back(Code::line("smallSinkStoreNext = smallSinkStore;"));
        for (const auto edge : edges_) {
            CodeOrigin annotation{code, code.size(), "o" + std::to_string(edge.index), sourceComments_ ?
                "initialize edge detector history; " + operationDescription(edge) : std::string{}};
            code.push_back(Code::line(fieldRef("prevEventStore", name(edge)) + " = " +
                (bits(attr(edge, "prevInit"), 1, false).getRawPtr()[0] ? "true" : "false") + ";"));
        }
        if (waveform_) code.push_back(Code::line("waveform_time_ = 0;"));
    }); addFunction(std::move(init), true);
}

void CpuStEmitContext::buildRuntime() {
    const auto line = [](std::string text) { return Code::line(std::move(text)); };
    const auto block = [](std::string text, std::vector<Code> body) { return Code::block(std::move(text), std::move(body)); };
    if (!indexedStates_.empty()) {
        Function touch{"memory_touch_range", "std::uint64_t *words, std::size_t first, std::size_t count"};
        touch.locals = {{"std::uint64_t*", "words", "{}", true}, {"std::size_t", "first", "{}", true},
            {"std::size_t", "count", "{}", true}, {"std::size_t", "word"}, {"std::size_t", "offset"},
            {"std::size_t", "take"}, {"std::uint64_t", "bits"}};
        touch.body = {block("while (@count@)", {
            line("@word@ = @first@ / 64;"), line("@offset@ = @first@ % 64;"),
            line("@take@ = std::min<std::size_t>(@count@, 64 - @offset@);"),
            line("@bits@ = ~UINT64_C(0) >> (64 - @take@);"),
            line("@words@[@word@] |= @bits@ << @offset@;"),
            line("@first@ += @take@;"), line("@count@ -= @take@;")
        })};
        addFunction(std::move(touch));
        Function intersects{"memory_read_touched", "const std::uint64_t *words, std::size_t first, std::size_t count, std::size_t cells", "bool"};
        intersects.locals = {{"const std::uint64_t*", "words", "{}", true}, {"std::size_t", "first", "{}", true},
            {"std::size_t", "count", "{}", true}, {"std::size_t", "cells", "{}", true},
            {"std::size_t", "word"}, {"std::size_t", "offset"}, {"std::size_t", "take"}, {"std::uint64_t", "bits"}};
        intersects.body = {block("if (@first@ >= @cells@)", {Code::line("return @first@ > @cells@;", true)}),
            block("while (@count@)", {
                line("@word@ = @first@ / 64;"), line("@offset@ = @first@ % 64;"),
                line("@take@ = std::min<std::size_t>(@count@, 64 - @offset@);"),
                line("@bits@ = ~UINT64_C(0) >> (64 - @take@);"),
                block("if (@words@[@word@] & (@bits@ << @offset@))", {Code::line("return true;", true)}),
                line("@first@ += @take@;"), line("@count@ -= @take@;")
            }), Code::line("return false;", true)};
        addFunction(std::move(intersects));
    }
    Function random{"random_next", "std::uint64_t &rng", "std::uint64_t"};
    random.locals = {{"std::uint64_t", "z"}};
    random.body = {line("@z@ = (rng += UINT64_C(0x9e3779b97f4a7c15));"),
        line("@z@ = (@z@ ^ (@z@ >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);"),
        line("@z@ = (@z@ ^ (@z@ >> 27)) * UINT64_C(0x94d049bb133111eb);"),
        Code::line("return @z@ ^ (@z@ >> 31);", true)};
    addFunction(std::move(random));
    Function clock{"profile_clock", "", "std::uint64_t", true};
    clock.locals = {{"unsigned", "aux"}};
    clock.body = {Code::line("return GRHSIM_PROFILE_CLOCK(@aux@);", true)};
    addFunction(std::move(clock));
    if (!hasTasks_) return;
    Function u64{"task_u64", "TaskArg arg", "std::uint64_t"};
    u64.locals = {{"std::uint64_t", "result"}};
    u64.body = {line("std::memcpy(&@result@, arg.data, std::min(arg.bytes, sizeof(@result@)));"), Code::line("return @result@;", true)};
    addFunction(std::move(u64));
    Function number{"task_number", "TaskArg arg, unsigned base, bool signedMode", "std::string"};
    number.locals = {{"TaskArg", "arg", "{}", true}, {"unsigned", "base", "{}", true}, {"bool", "signedMode", "{}", true},
        {"std::vector<unsigned char>", "bytes"}, {"std::string", "text"}, {"bool", "negative"}, {"unsigned", "carry"},
        {"std::size_t", "i"}, {"std::size_t", "n"}, {"unsigned", "remainder"}, {"unsigned", "next"}};
    number.body = {
        line("@bytes@.resize((@arg@.width + 7) / 8);"), line("std::memcpy(@bytes@.data(), @arg@.data, @bytes@.size());"),
        line("@negative@ = @signedMode@ && @arg@.isSigned && ((@bytes@.back() >> ((@arg@.width - 1) % 8)) & 1);"),
        block("if (@negative@)", {line("@carry@ = 1;"),
            block("for (@i@ = 0; @i@ < @bytes@.size(); ++@i@)", {
                line("@next@ = static_cast<unsigned char>(~@bytes@[@i@]) + @carry@;"),
                line("@bytes@[@i@] = static_cast<unsigned char>(@next@);"), line("@carry@ = @next@ >> 8;")
            })}),
        line("@bytes@.back() &= static_cast<unsigned char>(255u >> ((8 - @arg@.width % 8) % 8));"),
        line("@n@ = @bytes@.size();"),
        block("while (@n@ != 0)", {
            line("@remainder@ = 0;"),
            block("for (@i@ = @n@; @i@ > 0; --@i@)", {
                line("@next@ = (@remainder@ << 8) | @bytes@[@i@ - 1];"),
                line("@bytes@[@i@ - 1] = static_cast<unsigned char>(@next@ / @base@);"),
                line("@remainder@ = @next@ % @base@;")
            }),
            line("@text@.push_back(\"0123456789abcdef\"[@remainder@]);"),
            block("while (@n@ && @bytes@[@n@ - 1] == 0)", {line("--@n@;")})
        }),
        line("if (@text@.empty()) @text@ = \"0\";"), line("if (@negative@) @text@.push_back('-');"),
        line("std::reverse(@text@.begin(), @text@.end());"), Code::line("return @text@;", true)
    }; addFunction(std::move(number));
    Function defaultText{"task_default", "TaskArg arg", "std::string"};
    defaultText.locals = {{"TaskArg", "arg", "{}", true}, {"std::string", "text"}, {"std::ostringstream", "stream"}};
    defaultText.body = {
        block("if (@arg@.kind == 'l')", {line("@text@ = task_number(@arg@, 10, true);")}),
        block("if (@arg@.kind == 's')", {line("@text@ = *static_cast<const std::string*>(@arg@.data);")}),
        block("if (@arg@.kind == 'r')", {line("@stream@ << *static_cast<const double*>(@arg@.data);"), line("@text@ = @stream@.str();")}),
        Code::line("return @text@;", true)
    }; addFunction(std::move(defaultText));
    Function formatOne{"task_format_one", "TaskArg arg, char spec, int width, int precision, bool left, bool zero", "std::string"};
    formatOne.locals = {{"TaskArg", "arg", "{}", true}, {"char", "spec", "{}", true}, {"int", "width", "{}", true},
        {"int", "precision", "{}", true}, {"bool", "left", "{}", true}, {"bool", "zero", "{}", true},
        {"std::string", "text"}, {"unsigned", "base"}, {"double", "real"}, {"std::string", "format"},
        {"int", "size"}, {"std::size_t", "i"}, {"std::string", "padding"}};
    formatOne.body = {
        line("@text@ = task_default(@arg@);"),
        block("if (@arg@.kind == 'l')", {
            line("@base@ = (@spec@ == 'b') ? 2 : (@spec@ == 'o') ? 8 : (@spec@ == 'h' || @spec@ == 'x' || @spec@ == 'H' || @spec@ == 'X') ? 16 : 10;"),
            line("@text@ = task_number(@arg@, @base@, @spec@ != 'u' && @spec@ != 't' && @base@ == 10);")
        }),
        block("if (@spec@ == 'H' || @spec@ == 'X')", {
            block("for (@i@ = 0; @i@ < @text@.size(); ++@i@)", {line("@text@[@i@] = static_cast<char>(std::toupper(static_cast<unsigned char>(@text@[@i@])));")})
        }),
        line("if (@spec@ == 'c') @text@.assign(1, static_cast<char>(task_u64(@arg@) & 255));"),
        block("if (@spec@ == 'f' || @spec@ == 'F' || @spec@ == 'e' || @spec@ == 'E' || @spec@ == 'g' || @spec@ == 'G')", {
            line("@real@ = @arg@.kind == 'r' ? *static_cast<const double*>(@arg@.data) : static_cast<double>(task_u64(@arg@));"),
            line("@format@ = std::string(\"%.*\") + @spec@;"),
            line("@size@ = std::snprintf(nullptr, 0, @format@.c_str(), @precision@ < 0 ? 6 : @precision@, @real@);"),
            line("if (@size@ < 0) throw std::runtime_error(\"task real formatting failed\");"),
            line("@text@.resize(static_cast<std::size_t>(@size@) + 1);"),
            line("std::snprintf(@text@.data(), @text@.size(), @format@.c_str(), @precision@ < 0 ? 6 : @precision@, @real@);"),
            line("@text@.resize(static_cast<std::size_t>(@size@));")
        }),
        block("if (@width@ > 0 && static_cast<std::size_t>(@width@) > @text@.size())", {
            line("@padding@.assign(static_cast<std::size_t>(@width@) - @text@.size(), @zero@ && !@left@ ? '0' : ' ');"),
            line("@text@ = @left@ ? @text@ + @padding@ : @padding@ + @text@;")
        }), Code::line("return @text@;", true)
    }; addFunction(std::move(formatOne));
    Function digits{"task_digits", "const std::string *format, std::size_t *position", "int"};
    digits.locals = {{"const std::string*", "format", "{}", true}, {"std::size_t*", "position", "{}", true}, {"int", "number"}};
    digits.body = {block("while (*@position@ < @format@->size() && std::isdigit(static_cast<unsigned char>((*@format@)[*@position@])))", {
        line("if (@number@ > 1000000) throw std::runtime_error(\"task format width/precision exceeds limit\");"),
        line("@number@ = @number@ * 10 + ((*@format@)[*@position@] - '0');"), line("++*@position@;")
    }), Code::line("return @number@;", true)}; addFunction(std::move(digits));
    Function spec{"task_spec", "const std::string *format, std::size_t *position, std::span<const TaskArg> args, std::size_t *argument", "std::string"};
    spec.locals = {{"const std::string*", "format", "{}", true}, {"std::size_t*", "position", "{}", true},
        {"std::span<const TaskArg>", "args", "{}", true}, {"std::size_t*", "argument", "{}", true},
        {"bool", "left"}, {"bool", "zero"}, {"int", "width"}, {"int", "precision", "{-1}"}, {"char", "spec"}, {"std::string", "text"}};
    spec.body = {
        line("++*@position@;"),
        block("while (*@position@ < @format@->size() && ((*@format@)[*@position@] == '-' || (*@format@)[*@position@] == '0'))", {
            line("@left@ |= (*@format@)[*@position@] == '-';"), line("@zero@ |= (*@format@)[*@position@] == '0';"), line("++*@position@;")
        }), line("@width@ = task_digits(@format@, @position@);"),
        block("if (*@position@ < @format@->size() && (*@format@)[*@position@] == '.')", {
            line("++*@position@;"), line("@precision@ = task_digits(@format@, @position@);")
        }),
        block("while (*@position@ < @format@->size() && ((*@format@)[*@position@] == 'l' || (*@format@)[*@position@] == 'L' || (*@format@)[*@position@] == 'z'))", {line("++*@position@;")}),
        block("if (*@position@ < @format@->size())", {
            line("@spec@ = (*@format@)[*@position@];"),
            block("if (@spec@ == 'm')", {line("@text@ = \"top\";")}),
            block("if (@spec@ == '%')", {line("@text@ = \"%\";")}),
            block("if (@spec@ != 'm' && @spec@ != '%')", {
                line("@text@ = (*@argument@ < @args@.size()) ? task_format_one(@args@[(*@argument@)++], @spec@, @width@, @precision@, @left@, @zero@) : std::string(\"%\") + @spec@;")
            })
        }), Code::line("return @text@;", true)
    }; addFunction(std::move(spec));
    Function message{"task_message", "std::span<const TaskArg> args", "std::string"};
    message.locals = {{"std::span<const TaskArg>", "args", "{}", true}, {"std::string", "text"}, {"std::size_t", "i"},
        {"std::size_t", "argument", "{1}"}, {"const std::string*", "format"}};
    message.body = {
        block("if (!@args@.empty() && @args@[0].kind == 's')", {
            line("@format@ = static_cast<const std::string*>(@args@[0].data);"),
            block("for (@i@ = 0; @i@ < @format@->size(); ++@i@)", {
                line("@text@ += ((*@format@)[@i@] == '%') ? task_spec(@format@, &@i@, @args@, &@argument@) : std::string(1, (*@format@)[@i@]);")
            })
        }),
        block("if (!@args@.empty() && @args@[0].kind != 's')", {
            block("for (@i@ = 0; @i@ < @args@.size(); ++@i@)", {
                line("if (@i@) @text@.push_back(' ');"), line("@text@ += task_default(@args@[@i@]);")
            })
        }), Code::line("return @text@;", true)
    }; addFunction(std::move(message));
    Function driver{"system_task", "std::string_view name, std::initializer_list<TaskArg> items"};
    driver.locals = {{"std::string_view", "name", "{}", true}, {"std::initializer_list<TaskArg>", "items", "{}", true},
        {"std::span<const TaskArg>", "args"}, {"std::ostream*", "stream"}, {"std::uint64_t", "handle"},
        {"bool", "terminal"}, {"int", "exitCode"}, {"std::string", "text"}};
    driver.body = {
        line("@args@ = std::span<const TaskArg>(@items@.begin(), @items@.size());"),
        line("@stream@ = (@name@ == \"warning\" || @name@ == \"error\" || @name@ == \"fatal\") ? &std::cerr : &std::cout;"),
        block("if (@name@ == \"fwrite\" || @name@ == \"fdisplay\")", {
            line("if (@args@.empty()) throw std::runtime_error(\"CPU file task is missing its handle\");"),
            line("@handle@ = task_u64(@args@.front());"), line("@args@ = @args@.subspan(1);"),
            line("if (@handle@ != 1 && @handle@ != UINT64_C(0x80000001) && @handle@ != 2 && @handle@ != UINT64_C(0x80000002)) throw std::runtime_error(\"CPU file handle is not implemented\");"),
            line("@stream@ = (@handle@ == 1 || @handle@ == UINT64_C(0x80000001)) ? &std::cout : &std::cerr;")
        }),
        line("@terminal@ = @name@ == \"fatal\" || @name@ == \"finish\" || @name@ == \"stop\";"),
        line("@exitCode@ = @name@ == \"fatal\" ? 1 : 0;"),
        block("if (@terminal@ && !@args@.empty() && @args@.front().kind == 'l')", {
            line("@exitCode@ = static_cast<int>(task_u64(@args@.front()));"), line("@args@ = @args@.subspan(1);")
        }), line("@text@ = task_message(@args@);"),
        line("if (@name@ == \"info\" || @name@ == \"warning\" || @name@ == \"error\" || @name@ == \"fatal\") @text@ = \"[\" + std::string(@name@) + \"] \" + @text@;"),
        block("if (!@terminal@ || !@text@.empty())", {
            line("*@stream@ << @text@;"), line("if (@name@ != \"write\" && @name@ != \"fwrite\") *@stream@ << '\\n';")
        }),
        block("if (@terminal@)", {line("std::cout.flush();"), line("std::cerr.flush();"), line("std::exit(@exitCode@);")})
    }; addFunction(std::move(driver));
}

void CpuStEmitContext::buildObservers() {
    if (!counterSnapshot_.empty()) {
        Function counts{"dump_execution_counts", "std::FILE *stream", "void", true};
        counts.locals = {{"std::FILE*", "stream", "{}", true}, {"std::size_t", "i"}, {"std::uint64_t", "total"}};
        counts.body = {Code::block("if (@stream@)", {
            Code::line("std::fputs(" + quote("grhsim-execution-counts.v1\t__GRHSIM_EXECUTION_BINDING__\n") + ", @stream@);"),
            Code::line("std::fprintf(@stream@, \"evals\\t%llu\\n\", static_cast<unsigned long long>(opExecEvals_));"),
            Code::line("@total@ = 0;"),
            Code::block("for (@i@ = 0; @i@ < opExecCounts_.size(); ++@i@)", {
                Code::line("std::fprintf(@stream@, \"%llu\\t%llu\\n\", static_cast<unsigned long long>(@i@), static_cast<unsigned long long>(opExecCounts_[@i@]));"),
                Code::line("@total@ += opExecCounts_[@i@];")
            }),
            Code::line("std::fprintf(@stream@, \"total\\t%llu\\nend\\t%llu\\n\", static_cast<unsigned long long>(@total@), static_cast<unsigned long long>(opExecCounts_.size()));")
        })};
        addFunction(std::move(counts), true);
    }
    Function dump{"dumpState", "std::FILE *stream", "void", true};
    dump.locals.push_back({"std::FILE*", "stream", "{}", true});
    inFunction(dump, {}, [&](auto& code) {
        std::vector<Code> body;
        const auto dumpValue = [&](const std::string& label, const std::string& value, const LogicalType& type) {
            body.push_back(Code::line("std::fputs(" + quote(label + "=") + ", @stream@);"));
            if (type.kind == grh::ValueType::String) body.push_back(Code::line("std::fputs(" + value + ".c_str(), @stream@);"));
            else if (type.kind == grh::ValueType::Real) {
                const auto raw = temporary("std::uint64_t", "real_bit_image");
                body.push_back(Code::line("std::memcpy(&" + raw + ", &" + value + ", sizeof(" + raw + "));"));
                body.push_back(Code::line("std::fprintf(@stream@, \"%016llx\", static_cast<unsigned long long>(" + raw + "));"));
            } else {
                const auto element = leafType(type);
                const auto rows = type.width / element.width;
                const auto i = temporary("std::size_t", "leaf_index");
                const auto cell = type.shape.empty() ? value : row(value, type.shape, "(" + i + " - 1)");
                std::vector<Code> print;
                if (element.width == 1 && !element.isSigned) print.push_back(Code::line("std::fprintf(@stream@, \"%u\", " + cell + " ? 1u : 0u);"));
                else if (element.width <= 64) print.push_back(Code::line("std::fprintf(@stream@, \"%0*llx\", int(sizeof(" + cell + ") * 2), static_cast<unsigned long long>(static_cast<" + unsignedType(element.width) + ">(" + cell + ")));"));
                else {
                    const auto word = temporary("std::size_t", "leaf_index");
                    print.push_back(Code::block("for (" + word + " = " + std::to_string((element.width + 63) / 64) + "; " + word + " > 0; --" + word + ")", {
                        Code::line("std::fprintf(@stream@, \"%016llx\", static_cast<unsigned long long>(" + cell + " >> ((" + word + " - 1) * 64)));")
                    }));
                }
                body.push_back(Code::block("for (" + i + " = " + std::to_string(rows) + "; " + i + " > 0; --" + i + ")", std::move(print)));
            }
            body.push_back(Code::line("std::fputc('\\n', @stream@);"));
        };
        for (const auto& port : graph_.inputPorts()) dumpValue("input/" + port.name, "this->" + identifier(port.name), graph_.valueLogicalType(port.value));
        for (const auto& port : graph_.outputPorts()) dumpValue("output/" + port.name, "this->" + identifier(port.name), graph_.valueLogicalType(port.value));
        for (const auto state : states_) dumpValue("state/" + std::string(graph_.getOperation(state).symbolText()), stateRef(state), grh::stateLogicalType(graph_, state));
        code.push_back(Code::block("if (@stream@)", std::move(body)));
    }); addFunction(std::move(dump), true);
    Function counters{"perf_counters", "", class_ + "::PerfCounters", true};
    counters.body = {Code::line("return perf_;", true)}; addFunction(std::move(counters), true);
    Function enableProfile{"set_runtime_profile_enabled", "bool enabled"};
    enableProfile.body = {Code::line("profileEnabled_ = enabled;")}; addFunction(std::move(enableProfile), true);
    Function profile{"dump_runtime_profile", "", "void", true};
    profile.locals = {{"std::FILE*", "out", "{stderr}"}, {"const char*", "path"}, {"std::size_t", "i"}};
    profile.body = {
        Code::line("@path@ = std::getenv(\"WOLVRIX_GRHSIM_SN_PROFILE\");"),
        Code::block("if (@path@)", {Code::line("@out@ = std::fopen(@path@, \"w\");"), Code::line("if (!@out@) throw std::runtime_error(\"cannot open WOLVRIX_GRHSIM_SN_PROFILE\");")}),
        Code::line("std::fprintf(@out@, \"supernode_id\\tphase\\tfires\\t\" GRHSIM_PROFILE_UNIT \"\\n\");"),
        Code::block("for (@i@ = 0; @i@ < snTimeFires_.size(); ++@i@)", {
            Code::block("if (snTimeFires_[@i@])", {
                Code::line("std::fprintf(@out@, \"%llu\\tGeneral\\t%llu\\t%llu\\n\", static_cast<unsigned long long>(@i@), static_cast<unsigned long long>(snTimeFires_[@i@]), static_cast<unsigned long long>(snTimeTicks_[@i@]));")
            })
        }), Code::line("if (@out@ != stderr) std::fclose(@out@);"), Code::line("if (@out@ == stderr) std::fflush(@out@);")
    };
    profile.body = {Code::block("if constexpr (WOLVRIX_GRHSIM_SN_TIMING)", std::move(profile.body))};
    addFunction(std::move(profile), true);
    Function configureDefault{"configure_waveform", "bool enabled"};
    configureDefault.body = {Code::line("configure_waveform(enabled, \"grhsim.fst\");")}; addFunction(std::move(configureDefault), true);
    Function configure{"configure_waveform", "bool enabled, const char *path"};
    if (waveform_) configure.body = {
        Code::line("if (waveform_ctx_) fstWriterClose(waveform_ctx_);"), Code::line("waveform_ctx_ = nullptr;"),
        Code::line("waveform_entries_.clear();"), Code::line("waveform_path_ = path ? path : \"grhsim.fst\";"),
        Code::line("waveform_enabled_ = enabled;")
    };
    else configure.body = {Code::line("(void)path;"), Code::line("if (enabled) throw std::runtime_error(\"waveform was not enabled at emit time\");")};
    addFunction(std::move(configure), true);
    Function waveformEnabled{"waveform_enabled", "", "bool", true};
    waveformEnabled.body = {Code::line(std::string("return ") + (waveform_ ? "waveform_enabled_;" : "false;"), true)}; addFunction(std::move(waveformEnabled), true);
    if (!waveform_) return;
#if !WOLVRIX_HAVE_LIBFST
    throw std::invalid_argument("waveform emit requires the configured libfst support");
#endif
    Function destructor{"~" + class_, "", ""};
    destructor.body = {Code::line("if (waveform_ctx_) fstWriterClose(waveform_ctx_);")}; addFunction(std::move(destructor), true);
    Function registerSignal{"wave_register", "const char *name, const void *data, std::size_t bytes, std::uint32_t width, bool real"};
    registerSignal.locals = {{"const char*", "name", "{}", true}, {"const void*", "data", "{}", true}, {"std::size_t", "bytes", "{}", true},
        {"std::uint32_t", "width", "{}", true}, {"bool", "real", "{}", true}, {"fstHandle", "handle"}};
    registerSignal.body = {
        Code::line("@handle@ = fstWriterCreateVar(waveform_ctx_, @real@ ? FST_VT_VCD_REAL : FST_VT_SV_LOGIC, FST_VD_IMPLICIT, @width@, @name@, 0);"),
        Code::line("waveform_entries_.push_back(WaveEntry{@data@, @bytes@, @width@, @real@, @handle@, std::vector<unsigned char>(@bytes@), false});")
    }; addFunction(std::move(registerSignal));
    Function setup{"ensure_waveform_open"};
    inFunction(setup, {}, [&](auto& code) {
        std::vector<Code> open{
            Code::line("waveform_ctx_ = fstWriterCreate(waveform_path_.c_str(), 1);"),
            Code::line("if (!waveform_ctx_) throw std::runtime_error(\"cannot open FST waveform\");"),
            Code::line("fstWriterSetPackType(waveform_ctx_, FST_WR_PT_ZLIB);"),
            Code::line("fstWriterSetFileType(waveform_ctx_, FST_FT_VERILOG);"),
            Code::line("fstWriterSetTimescale(waveform_ctx_, -9);"),
            Code::line("fstWriterSetVersion(waveform_ctx_, \"wolvrix grhsim-cpu-st-cpp\");"),
            Code::line("fstWriterSetScope(waveform_ctx_, FST_ST_VCD_MODULE, " + quote(graph_.symbol()) + ", nullptr);")
        };
        for (const auto symbol : graph_.declaredSymbols()) {
            const auto text = std::string(graph_.symbolText(symbol));
            if (!options_.traceUnderscoreValues && !text.empty() && text.front() == '_') continue;
            std::string source; LogicalType type;
            if (const auto state = graph_.findOperation(symbol); state && (graph_.opKind(state) == OperationKind::kRegister || graph_.opKind(state) == OperationKind::kLatch)) {
                type = grh::stateLogicalType(graph_, state); source = stateRef(state);
            } else if (const auto value = graph_.findValue(symbol); value && storage_.contains(value)) {
                type = graph_.valueLogicalType(value); source = storage_.at(value);
            } else continue;
            if (type.kind == grh::ValueType::String) continue;
            const auto element = leafType(type);
            const auto hostWidth = element.kind == grh::ValueType::Real ? 64 : element.width <= 8 ? 8 : element.width <= 16 ? 16 : element.width <= 32 ? 32 : (element.width + 63) / 64 * 64;
            const auto rows = type.kind == grh::ValueType::Logic ? type.width / element.width : 1;
            if (rows * hostWidth / 8 > 512) continue;
            for (int64_t i = 0; i < rows; ++i) {
                const auto cell = type.shape.empty() ? source : row(source, type.shape, std::to_string(i));
                std::string label = text;
                int64_t stride = rows;
                for (const auto size : type.shape) { stride /= size; label += '[' + std::to_string((i / stride) % size) + ']'; }
                open.push_back(Code::line("wave_register(" + quote(label) + ", &" + cell + ", sizeof(" + cell + "), " +
                    std::to_string(element.kind == grh::ValueType::Real ? 64 : element.width) + ", " + (element.kind == grh::ValueType::Real ? "true" : "false") + ");"));
            }
        }
        open.push_back(Code::line("fstWriterSetUpscope(waveform_ctx_);"));
        code.push_back(Code::block("if (waveform_enabled_ && !waveform_ctx_)", std::move(open)));
    }); addFunction(std::move(setup));
    Function trace{"dump_waveform"};
    trace.locals = {{"std::size_t", "i"}, {"WaveEntry*", "entry"}, {"std::uint64_t", "scalar"}};
    trace.body = {
        Code::line("ensure_waveform_open();"),
        Code::block("if (waveform_ctx_)", {
            Code::line("fstWriterEmitTimeChange(waveform_ctx_, waveform_time_++);"),
            Code::block("for (@i@ = 0; @i@ < waveform_entries_.size(); ++@i@)", {
                Code::line("@entry@ = &waveform_entries_[@i@];"),
                Code::block("if (!@entry@->valid || std::memcmp(@entry@->previous.data(), @entry@->data, @entry@->bytes) != 0)", {
                    Code::block("if (@entry@->real)", {Code::line("fstWriterEmitValueChange(waveform_ctx_, @entry@->handle, @entry@->data);")}),
                    Code::block("if (!@entry@->real && @entry@->width <= 64)", {
                        Code::line("@scalar@ = 0;"), Code::line("std::memcpy(&@scalar@, @entry@->data, @entry@->bytes);"),
                        Code::line("fstWriterEmitValueChange64(waveform_ctx_, @entry@->handle, @entry@->width, @scalar@);")
                    }),
                    Code::block("if (!@entry@->real && @entry@->width > 64)", {
                        Code::line("fstWriterEmitValueChangeVec64(waveform_ctx_, @entry@->handle, @entry@->width, static_cast<const std::uint64_t*>(@entry@->data));")
                    }),
                    Code::line("std::memcpy(@entry@->previous.data(), @entry@->data, @entry@->bytes);"),
                    Code::line("@entry@->valid = true;")
                })
            })
        })
    }; addFunction(std::move(trace));
}

std::string CpuStEmitContext::counterBinding(const std::filesystem::path& staging) const {
    uint64_t hash = 14695981039346656037ull;
    const auto append = [&](std::string_view text) {
        for (const unsigned char ch : text) { hash ^= ch; hash *= 1099511628211ull; }
    };
    append(counterSnapshot_);
    for (const auto& site : counterSites_)
        for (const auto& text : {std::string(graph_.getOperation(site.operation).symbolText()), site.category, site.function}) {
            append(std::to_string(text.size())); append(":"); append(text);
        }
    for (const auto& name : counterBindingFiles_) {
        append(std::to_string(name.size())); append(":"); append(name);
        append(std::to_string(std::filesystem::file_size(staging / name))); append(":");
        std::ifstream input(staging / name, std::ios::binary);
        input.exceptions(std::ios::badbit);
        if (!input) throw std::runtime_error("Cannot read execution binding source: " + name);
        char buffer[65536];
        while (input.read(buffer, sizeof(buffer)) || input.gcount())
            append(std::string_view(buffer, static_cast<std::size_t>(input.gcount())));
    }
    return std::to_string(hash);
}

std::string CpuStEmitContext::counterMetadata() const {
    slang::JsonWriter json;
    json.startObject();
    json.writeProperty("schema"); json.writeValue(std::string_view("grhsim-execution-counts.v1"));
    json.writeProperty("graph_file"); json.writeValue(std::string_view("execution-counter-graph.json"));
    json.writeProperty("binding_fnv1a64"); json.writeValue(counterBinding_);
    json.writeProperty("binding_files"); json.startArray();
    for (const auto& file : counterBindingFiles_) json.writeValue(file);
    json.endArray();
    json.writeProperty("sites"); json.startArray();
    for (const auto& site : counterSites_) {
        json.startObject();
        json.writeProperty("operation"); json.writeValue(graph_.getOperation(site.operation).symbolText());
        json.writeProperty("category"); json.writeValue(site.category);
        json.writeProperty("function"); json.writeValue(site.function);
        json.endObject();
    }
    json.endArray();
    json.writeProperty("elided_operations"); json.startArray();
    for (const auto op : graph_.operations())
        if ((identityElisions_.contains(op) || leafElisions_.contains(op) || (graph_.opKind(op) == OperationKind::kAssign && graph_.opResults(op).size() == 1 &&
            viewOnly_.at(graph_.opResults(op)[0]))) && sim::attribute<Key>(graph_, op, sim::kPartition))
            json.writeValue(graph_.getOperation(op).symbolText());
    json.endArray(); json.endObject();
    return std::string(json.view());
}

void CpuStEmitContext::codeMap(std::ostream& output, const std::vector<WrittenFunctions>& written,
                               const std::map<std::string, std::string>& sources) const {
    StreamingJsonWriter json(output);
    const auto text = [&](std::string_view key, std::string_view value) { json.writeProperty(key); json.writeValue(value); };
    const auto strings = [&](std::string_view key, const auto& values) {
        json.writeProperty(key); json.startArray();
        for (const auto& value : values) json.writeValue(std::string_view(value));
        json.endArray();
    };
    const auto location = [&](const std::optional<grh::SrcLoc>& loc) {
        json.writeProperty("source_location"); json.startObject();
        if (loc) {
            text("file", loc->file);
            json.writeProperty("line"); json.writeValue(uint64_t(loc->line));
            json.writeProperty("column"); json.writeValue(uint64_t(loc->column));
            json.writeProperty("end_line"); json.writeValue(uint64_t(loc->endLine));
            json.writeProperty("end_column"); json.writeValue(uint64_t(loc->endColumn));
            text("origin", loc->origin); text("pass", loc->pass); text("note", loc->note);
        }
        json.endObject();
    };
    const auto owner = [&](OperationId op) {
        strings("logical_partition", sim::attribute<Key>(graph_, op, sim::kPartition).value_or(Key{}));
        strings("execution_owner", executionOwner(op));
    };
    const auto reference = [&](std::string_view role, const std::string& ref) {
        json.startObject(); text("role", role); text("reference", ref); json.endObject();
    };
    json.startObject();
    text("schema", "grhsim-code-map.v1"); text("top", graph_.symbol());
    text("identity_scope", "op/value IDs identify this frozen graph only; full symbols and origin sets are in grhsim.txt");
    json.writeProperty("readable_names"); json.writeValue(readableNames_);
    std::map<std::vector<std::string>, std::size_t> originSets;
    for (const auto op : graph_.operations()) {
        const auto origins = sim::attribute<std::vector<std::string>>(graph_, op, "originModules").value_or(std::vector<std::string>{});
        if (!originSets.contains(origins)) originSets.emplace(origins, originSets.size());
    }
    json.writeProperty("origin_sets"); json.startArray();
    for (const auto& [origins, id] : originSets) {
        json.startObject(); json.writeProperty("id"); json.writeValue(uint64_t(id)); strings("modules", origins); json.endObject();
    }
    json.endArray();
    json.writeProperty("values"); json.startArray();
    for (const auto value : graph_.values()) {
        json.startObject(); text("value_id", "v" + std::to_string(value.index)); text("cpp_name", valueName(value));
        text("packed_prefix", packedPrefix(value));
        text("symbol", graph_.symbolText(graph_.valueSymbol(value)));
        text("cpp_type", cppType(graph_.valueLogicalType(value)));
        json.writeProperty("generation"); json.writeValue(uint64_t(value.generation));
        const auto producer = graph_.valueDef(value);
        if (producer) text("producer", "o" + std::to_string(producer.index));
        location(graph_.valueSrcLoc(value));
        json.writeProperty("storage"); json.startArray();
        if (const auto found = storage_.find(value); found != storage_.end()) reference("primary", found->second);
        if (const auto found = smallEnableBoundary_.find(value); found != smallEnableBoundary_.end()) reference("small-sink-enable", found->second);
        if (const auto found = smallPayloadBoundary_.find(value); found != smallPayloadBoundary_.end()) reference("small-sink-payload", found->second);
        if (const auto found = snapshotViews_.find(value); found != snapshotViews_.end()) {
            auto source = found->second;
            while (snapshotViews_.contains(source)) source = snapshotViews_.at(source);
            reference("snapshot-view", storage_.at(source));
        }
        json.endArray();
        if (const auto found = snapshotViews_.find(value); found != snapshotViews_.end())
            text("snapshot_source", "v" + std::to_string(found->second.index));
        if (const auto found = smallEnableOrdinal_.find(value); found != smallEnableOrdinal_.end()) {
            json.writeProperty("enable_bit"); json.writeValue(uint64_t(found->second));
        }
        json.endObject();
    }
    json.endArray();
    json.writeProperty("operations"); json.startArray();
    for (const auto op : graph_.operations()) {
        json.startObject(); text("operation_id", "o" + std::to_string(op.index)); text("cpp_name", name(op));
        text("symbol", graph_.symbolText(graph_.operationSymbol(op))); text("kind", grh::toString(graph_.opKind(op)));
        json.writeProperty("generation"); json.writeValue(uint64_t(op.generation));
        json.writeProperty("origin_set"); json.writeValue(uint64_t(originSets.at(
            sim::attribute<std::vector<std::string>>(graph_, op, "originModules").value_or(std::vector<std::string>{}))));
        json.writeProperty("operands"); json.startArray();
        for (const auto arg : graph_.opOperands(op)) json.writeValue("v" + std::to_string(arg.index));
        json.endArray();
        json.writeProperty("results"); json.startArray();
        for (const auto result : graph_.opResults(op)) json.writeValue("v" + std::to_string(result.index));
        json.endArray();
        location(graph_.opSrcLoc(op)); owner(op);
        const bool elided = identityElisions_.contains(op) || leafElisions_.contains(op) ||
            (graph_.opKind(op) == OperationKind::kAssign && graph_.opResults(op).size() == 1 && viewOnly_.at(graph_.opResults(op)[0]));
        json.writeProperty("elided"); json.writeValue(elided);
        if (const auto ordinal = stateOrdinal_.find(op); ordinal != stateOrdinal_.end()) {
            text("state_reference", stateRef(op));
            if (!large(op)) text("next_reference", nextStateRef(op));
        }
        if (indexedReads_.contains(op)) text("subscription", fieldRef("memoryReadIndexStore", subscriptionName(op)));
        if (const auto bit = largeWriteOrdinal_.find(op); bit != largeWriteOrdinal_.end()) {
            json.writeProperty("enable_bit"); json.writeValue(uint64_t(bit->second));
            json.writeProperty("captures"); json.startArray();
            for (std::size_t i = graph_.opKind(op) == sim::kLatchWrite ? 1 : 2; i < graph_.opOperands(op).size(); ++i)
                reference("argument-" + std::to_string(i), fieldRef("largeSinkPayloadBoundaryStore", captureName(op, i)));
            json.endArray();
        }
        json.writeProperty("storage"); json.startArray();
        for (const auto& [store, layout] : fieldLayouts_) {
            if (layout.references.contains(name(op))) reference(store, fieldRef(store, name(op)));
            if (store == "smallSinkStore" && layout.references.contains(name(op)))
                reference("smallSinkStoreNext", nextStateRef(op));
        }
        if (indexedReads_.contains(op)) reference("memoryReadIndexStore", fieldRef("memoryReadIndexStore", subscriptionName(op)));
        if (largeWriteOrdinal_.contains(op)) {
            if (enableBitmap_) {
                const auto bit = largeWriteOrdinal_.at(op);
                reference("captured-enable", "(largeSinkEnableBoundaryStore.words[" + std::to_string(bit / 64) +
                    "] & (UINT64_C(1)<<" + std::to_string(bit % 64) + "))");
            } else reference("captured-enable", fieldRef("largeSinkEnableBoundaryStore", captureName(op)));
            for (std::size_t i = graph_.opKind(op) == sim::kLatchWrite ? 1 : 2; i < graph_.opOperands(op).size(); ++i)
                reference("captured-argument-" + std::to_string(i), fieldRef("largeSinkPayloadBoundaryStore", captureName(op, i)));
        }
        json.endArray();
        json.endObject();
    }
    json.endArray();
    json.writeProperty("stores"); json.startArray();
    auto storeInstances = namedStores;
    storeInstances.push_back("smallSinkStoreNext");
    for (const auto& instance : storeInstances) {
        const auto store = instance == "smallSinkStoreNext" ? "smallSinkStore" : instance;
        json.startObject(); text("name", instance); text("type", store + "Type");
        if (instance != store) text("layout", store);
        json.writeProperty("fields"); json.startArray();
        if (const auto found = fields_.find(store); found != fields_.end()) for (const auto& field : found->second) {
            json.startObject(); text("name", field.name); text("type", field.type);
            text("reference", fieldRef(store, field.name, instance)); json.endObject();
        }
        json.endArray(); json.endObject();
    }
    json.endArray();
    json.writeProperty("functions"); json.startArray();
    for (std::size_t i = 0; i < written.size(); ++i) for (const auto& function : written[i].functions) {
        json.startObject(); text("name", function.name); text("source", sources.at(function.name));
        text("root_function", functionNames_[i]); strings("operations", function.operations);
        json.writeProperty("locals"); json.startArray();
        for (const auto& [name, reference] : function.localReferences) {
            json.startObject(); text("name", name); text("reference", reference);
            text("type", function.localTypes.at(name)); json.endObject();
        }
        json.endArray(); json.endObject();
    }
    json.endArray();
    json.writeProperty("nonsink_functions"); json.startArray();
    for (std::size_t i = 0; i < nonsink_.size(); ++i) {
        json.startObject(); text("name", "sn_" + std::to_string(i)); strings("execution_owner", nonsink_[i]); json.endObject();
    }
    json.endArray(); json.endObject();
    output.put('\n');
}

std::vector<std::string> CpuStEmitContext::generate(const std::filesystem::path& staging) {
    // Consume each source function as it is built. The large structured bodies
    // and rendered definitions never accumulate alongside the whole model.
    struct Definition { std::uint64_t bytes; };
    std::vector<Definition> definitions;
    std::vector<WrittenFunctions> written;
    const auto bodyPath = staging / ".function-bodies";
    std::ofstream bodies(bodyPath, std::ios::binary);
    bodies.exceptions(std::ios::badbit | std::ios::failbit);
    writeFunction_ = [&](Function function) {
        auto group = writeFunction(std::move(function), class_, maxLines_);
        for (auto& emitted : group.functions) {
            definitions.push_back({emitted.definition.size()});
            bodies << emitted.definition;
            std::string().swap(emitted.definition);
        }
        written.push_back(std::move(group));
    };
    analyze();
    buildConstantSources();
    buildPhases();
    buildInit();
    for (auto& [store, layout] : fieldLayouts_) std::vector<Local>().swap(layout.physicalFields);
    buildRuntime();
    buildObservers();
    writeFunction_ = {};
    bodies.close();
    std::vector<std::string> files;
    const auto finished = [&](const std::string& path) {
        if (std::filesystem::file_size(staging / path) > options_.maxOutputFileBytes)
            throw std::invalid_argument("Generated artifact exceeds maxOutputFileBytes: " + path);
        files.push_back(path);
    };
    const auto save = [&](const std::string& path, std::string_view contents) {
        std::ofstream output(staging / path, std::ios::binary);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output << contents; output.close(); finished(path);
    };
    std::ofstream header(staging / (prefix_ + ".hpp"), std::ios::binary);
    header.exceptions(std::ios::badbit | std::ios::failbit);
    header << "#pragma once\n#include <algorithm>\n#include <array>\n#include <chrono>\n#include <cctype>\n#include <cstdint>\n#include <cstdio>\n#include <cstdlib>\n#include <cstring>\n#include <functional>\n#include <initializer_list>\n#include <iostream>\n#include <map>\n#include <span>\n#include <sstream>\n#include <stdexcept>\n#include <string>\n#include <string_view>\n#include <vector>\n";
    if (waveform_) header << "#include \"fstapi.h\"\n";
    header << "#if !defined(__clang__) || !defined(__BITINT_MAXWIDTH__)\n#error \"grhsim CPU requires clang with unsigned _BitInt support\"\n#endif\n"
        << "#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__\n#error \"grhsim CPU requires little-endian word storage\"\n#endif\n"
        << "#ifndef WOLVRIX_GRHSIM_SN_TIMING\n#define WOLVRIX_GRHSIM_SN_TIMING 0\n#endif\n"
        << "#ifndef WOLVRIX_GRHSIM_PERF\n#define WOLVRIX_GRHSIM_PERF 0\n#endif\n"
        << "#if WOLVRIX_GRHSIM_PERF\n#define GRHSIM_PERF_COUNT(field) (++perf_.field)\n#else\n#define GRHSIM_PERF_COUNT(field) ((void)0)\n#endif\n"
        << "#if defined(__x86_64__)\n#include <x86intrin.h>\n#define GRHSIM_PROFILE_CLOCK(aux) __rdtscp(&(aux))\n#define GRHSIM_PROFILE_UNIT \"cycles\"\n"
        << "#else\n#define GRHSIM_PROFILE_CLOCK(aux) static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count())\n#define GRHSIM_PROFILE_UNIT \"nanoseconds\"\n#endif\n"
        << "static_assert(sizeof(unsigned _BitInt(65)) == 16 && alignof(unsigned _BitInt(65)) == 8);\n"
        << "class " << class_ << " {\npublic:\n";
    std::set<std::string> memberNames{class_, "PerfCounters", "TaskArg", "activeCurrent", "activeNext", "dirtyStateFlags", "pending_", "cpu_rng", "perf_", "profileEnabled_", "snTimeTicks_", "snTimeFires_", "numeric_select"};
    if (!counterSnapshot_.empty()) { memberNames.insert("opExecCounts_"); memberNames.insert("opExecEvals_"); }
    for (const auto& [store, fields] : fields_) memberNames.insert(store);
    for (const auto& name : functionNames_) memberNames.insert(name);
    const auto port = [&](const grh::Port& port) {
        const auto name = identifier(port.name);
        if (!memberNames.insert(name).second) throw std::invalid_argument("CPU port collides with a generated member: " + name);
        header << "    " << cppType(graph_.valueLogicalType(port.value)) << ' ' << name << "{};\n";
    };
    for (const auto& input : graph_.inputPorts()) port(input);
    for (const auto& output : graph_.outputPorts()) port(output);
    header << "    " << class_ << "() = default;\n"
        << "    " << class_ << "(const " << class_ << "&) = delete;\n"
        << "    " << class_ << "& operator=(const " << class_ << "&) = delete;\n"
        << "    struct PerfCounters {\n        std::uint64_t evalCount=0, round1Count=0, round2Count=0, totalRoundCount=0, computeBatchExecCount=0, commitBatchExecCount=0, touchedStateShadowCount=0, touchedWriteCount=0;\n    };\n";
    for (std::size_t i = 0; i < functionNames_.size(); ++i) if (publicFunctions_.contains(functionNames_[i])) header << written[i].functions[0].declaration;
    header << "private:\n";
    // Both scalar operands are already available under the consumer's guard.
    // Passing them by value avoids conditional loads in numeric mux chains.
    header << "    template<class T> static T numeric_select(bool condition, T yes, T no) noexcept {\n"
        << "        return __builtin_unpredictable(condition) ? yes : no;\n    }\n";
    header << "    struct TaskArg { char kind; const void *data; std::size_t bytes; std::size_t width; bool isSigned; };\n";
    for (const auto& store : namedStores) {
        const auto layout = fieldLayouts_.find(store);
        if (layout == fieldLayouts_.end()) header << writeValueStruct(store + "Type", {});
        else { header << layout->second.declaration; std::string().swap(layout->second.declaration); }
        header << "    " << store << "Type " << store << "{};\n";
    }
    const auto activeWords = std::max<std::size_t>(1, (nonsink_.size() + 7) / 8);
    const auto dirtyWords = std::max<std::size_t>(1, (states_.size() + 7) / 8);
    header << "    smallSinkStoreType smallSinkStoreNext{};\n"
        << "    std::array<std::uint8_t," << activeWords << "> activeCurrent{}, activeNext{};\n"
        << "    std::array<std::uint8_t," << dirtyWords << "> dirtyStateFlags{};\n"
        << "    bool pending_ = false;\n    std::uint64_t cpu_rng = UINT64_C(0x6a09e667f3bcc909);\n"
        << "    PerfCounters perf_{};\n    bool profileEnabled_ = WOLVRIX_GRHSIM_SN_TIMING;\n"
        << "    std::array<std::uint64_t,(WOLVRIX_GRHSIM_SN_TIMING ? " << nonsink_.size() << " : 0)> snTimeTicks_{}, snTimeFires_{};\n";
    if (!counterSnapshot_.empty()) header << "    std::array<std::uint64_t," << counterSites_.size() << "> opExecCounts_{};\n    std::uint64_t opExecEvals_ = 0;\n";
    if (waveform_) header << "    struct WaveEntry { const void *data; std::size_t bytes; std::uint32_t width; bool real; fstHandle handle; std::vector<unsigned char> previous; bool valid; };\n"
        << "    fstWriterContext *waveform_ctx_ = nullptr;\n    std::vector<WaveEntry> waveform_entries_;\n"
        << "    bool waveform_enabled_ = false;\n    std::string waveform_path_ = \"grhsim.fst\";\n    std::uint64_t waveform_time_ = 0;\n";
    for (std::size_t i = 0; i < functionNames_.size(); ++i) {
        header << written[i].frameDeclaration;
        for (std::size_t j = 0; j < written[i].functions.size(); ++j)
            if (j || !publicFunctions_.contains(functionNames_[i])) header << written[i].functions[j].declaration;
    }
    header << "};\n";
    header.close(); finished(prefix_ + ".hpp");
    // Declarations have been written. Keep only metadata needed by code-map.
    for (auto& group : written) {
        std::string().swap(group.frameDeclaration);
        for (auto& function : group.functions) std::string().swap(function.declaration);
    }
    const auto preamble = "#include \"" + prefix_ + ".hpp\"\n" + dpiDeclarations_;
    std::vector<std::string> sources;
    std::map<std::string, std::string> functionSources;
    std::ifstream bodyInput(bodyPath, std::ios::binary);
    bodyInput.exceptions(std::ios::badbit);
    if (!bodyInput) throw std::runtime_error("Cannot read generated function bodies");
    std::ofstream unit;
    unit.exceptions(std::ios::badbit | std::ios::failbit);
    std::uint64_t unitBytes = preamble.size();
    std::string unitPath;
    std::ostringstream manifest;
    manifest << "{\n  \"emitter\": \"grhsim-cpu-st-cpp\",\n  \"top\": " << quote(graph_.symbol())
        << ",\n  \"code_map\": \"code-map.json\",\n  \"readable_names\": " << (readableNames_ ? "true" : "false")
        << ",\n  \"source_comments\": " << (sourceComments_ ? "true" : "false")
        << ",\n  \"readable_frames\": " << (readableFrames_ ? "true" : "false")
        << ",\n  \"max_function_lines\": " << maxLines_ << ",\n  \"max_tu_bytes\": " << tuBytes_
        << ",\n  \"array_view_elisions\": " << std::count_if(viewOnly_.begin(), viewOnly_.end(), [](const auto& entry) { return entry.second; })
        << ",\n  \"functions\": [\n";
    bool first = true;
    const auto flush = [&] {
        if (!unit.is_open()) return;
        unit.close(); finished(unitPath); sources.push_back(unitPath); unitBytes = preamble.size();
    };
    std::size_t definitionIndex = 0;
    for (const auto& group : written) for (const auto& function : group.functions) {
        const auto bytes = definitions.at(definitionIndex++).bytes;
        if (unitBytes + bytes > tuBytes_) flush();
        if (!unit.is_open()) {
            unitPath = prefix_ + "_tu" + std::to_string(sources.size()) + ".cpp";
            unit.open(staging / unitPath, std::ios::binary); unit << preamble;
        }
        functionSources.emplace(function.name, prefix_ + "_tu" + std::to_string(sources.size()) + ".cpp");
        auto remaining = bytes;
        char buffer[65536];
        while (remaining) {
            const auto count = static_cast<std::streamsize>(std::min<std::uint64_t>(remaining, sizeof(buffer)));
            bodyInput.read(buffer, count);
            if (bodyInput.gcount() != count) throw std::runtime_error("Truncated generated function bodies");
            unit.write(buffer, count); remaining -= count;
        }
        unit.put('\n'); unitBytes += bytes + 1;
        if (!first) manifest << ",\n"; first = false;
        manifest << "    {\"name\":" << quote(function.name) << ",\"lines\":" << function.lines << "}";
    }
    flush();
    bodyInput.close(); std::filesystem::remove(bodyPath);
    {
        std::ofstream output(staging / "code-map.json", std::ios::binary);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        codeMap(output, written, functionSources); output.close(); finished("code-map.json");
    }
    std::vector<WrittenFunctions>().swap(written);
    if (!counterSnapshot_.empty()) {
        // Bind executable sources only; inspection artifacts are not part of
        // the counter ABI and may be exported independently.
        counterBindingFiles_ = sources; counterBindingFiles_.push_back(prefix_ + ".hpp");
        std::sort(counterBindingFiles_.begin(), counterBindingFiles_.end());
        counterBinding_ = counterBinding(staging);
        std::size_t replacements = 0;
        for (const auto& file : counterBindingFiles_) {
            std::ifstream input(staging / file, std::ios::binary);
            input.exceptions(std::ios::badbit);
            std::string contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            constexpr std::string_view placeholder = "__GRHSIM_EXECUTION_BINDING__";
            if (const auto offset = contents.find(placeholder); offset != std::string::npos) {
                contents.replace(offset, placeholder.size(), counterBinding_); ++replacements;
                if (contents.find(placeholder) != std::string::npos) ++replacements;
                std::ofstream output(staging / file, std::ios::binary);
                output.exceptions(std::ios::badbit | std::ios::failbit); output << contents; output.close();
            }
        }
        if (replacements != 1) throw std::invalid_argument("Execution counter source binding placeholder must be unique");
    }
    manifest << "\n  ],\n  \"sources\": [";
    for (std::size_t i = 0; i < sources.size(); ++i) manifest << (i ? "," : "") << quote(sources[i]);
    manifest << "],\n  \"readmem_fnv1a64\": {";
    first = true;
    for (const auto& [file, hash] : initFiles_) { manifest << (first ? "" : ",") << quote(file) << ':' << quote(hash); first = false; }
    manifest << "},\n  \"profile_time_unit\": \"cycles-on-x86_64-nanoseconds-elsewhere\",\n  \"mem_write_activate\": " << quote(activateOnWrite_ ? "on-write" : "on-change")
        << ",\n  \"mem_enable_bitmap\": " << (enableBitmap_ ? "true" : "false") << ",\n  \"nonsink_functions\": [";
    for (std::size_t ordinal = 0; ordinal < nonsink_.size(); ++ordinal) {
        const auto& key = nonsink_[ordinal];
        manifest << (ordinal ? "," : "") << "{\"ordinal\":" << ordinal << ",\"partition\":[";
        for (std::size_t i = 0; i < key.size(); ++i) manifest << (i ? "," : "") << quote(key[i]);
        manifest << "],\"operation_symbols\":[";
        const auto& operations = members_.at(key);
        for (std::size_t i = 0; i < operations.size(); ++i)
            manifest << (i ? "," : "") << quote(graph_.symbolText(graph_.operationSymbol(operations[i])));
        manifest << "]}";
    }
    std::set<Key> leafOwners;
    for (const auto op : graph_.operations())
        for (const auto& entry : graph_.opAttrs(op)) if (entry.key == sim::kPartition)
            leafOwners.insert(std::get<Key>(entry.value));
    manifest << "],\n  \"virtual_partition_count\": " << leafOwners.size();
    if (!counterSnapshot_.empty()) {
        manifest << ",\n  \"execution_counters\": " << counterMetadata();
        save("execution-counter-graph.json", counterSnapshot_);
    }
    manifest << "\n}\n";
    save("manifest.json", manifest.str());
    std::ostringstream make;
    make << ".DEFAULT_GOAL := all\nifeq ($(origin CXX),default)\nCXX := clang++\nendif\n"
        << "CXXFLAGS ?= -std=c++20 -O3 -fno-strict-aliasing\nAR ?= ar\nSOURCES :=";
    for (const auto& source : sources) make << ' ' << source;
    make << "\nOBJECTS := $(SOURCES:.cpp=.o)\nLIB := lib" << prefix_ << ".a\nPCH := " << prefix_ << ".hpp.pch\n";
    if (waveform_) {
#ifdef WOLVRIX_SOURCE_DIR
        make << "LIBFST_SRC_DIR := " << (std::filesystem::path(WOLVRIX_SOURCE_DIR) / "external/libfst/src").string() << '\n';
#else
        throw std::invalid_argument("FST emitter requires WOLVRIX_SOURCE_DIR");
#endif
        make << "CPPFLAGS += -I$(LIBFST_SRC_DIR)\nFST_OBJECTS := fstapi.o fastlz.o lz4.o\nCFLAGS ?= -O2 -D_GNU_SOURCE\n";
    }
    make << "all: $(LIB)\n$(LIB): $(OBJECTS) $(FST_OBJECTS)\n\t$(AR) rcs $@ $^\n"
        << "$(PCH): " << prefix_ << ".hpp\n\t$(CXX) $(CPPFLAGS) $(CXXFLAGS) -x c++-header $< -o $@\n"
        << "%.o: %.cpp $(PCH)\n\t$(CXX) $(CPPFLAGS) $(CXXFLAGS) -include-pch $(PCH) -c $< -o $@\n";
    if (waveform_) for (const auto source : {"fstapi", "fastlz", "lz4"}) make << source << ".o: $(LIBFST_SRC_DIR)/" << source << ".c\n\t$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@\n";
    make << ".PHONY: all\n"; save("Makefile", make.str());
    return files;
}

} // namespace

EmitResult GrhSimCpuStCpp::emitImpl(const grh::Design& design, std::span<const grh::Graph* const> tops, const EmitOptions& options) {
    std::filesystem::path staging;
    bool ownsStaging = false;
    try {
        if (tops.size() != 1) throw std::invalid_argument("grhsim-cpu-st-cpp requires exactly one flattened top graph");
        const auto& graph = *tops[0];
        const auto& schedule = sim::requireSinkSchedule(options.session, graph);
        const auto destination = resolveOutputDir(options);
        if (std::filesystem::exists(destination) && !std::filesystem::is_empty(destination))
            throw std::invalid_argument("grhsim-cpu-st-cpp output directory must be empty: " + destination.string());
        // Publish only a complete, validated model. Large artifacts are streamed
        // into private staging and removed together if any check fails.
        std::string counterSnapshot;
        if (const auto found = options.attributes.find("execution-counters"); found != options.attributes.end()) {
            if (found->second != "on" && found->second != "off") throw std::invalid_argument("execution-counters must be on or off");
            if (found->second == "on") {
                store::StoreJson serializer;
                store::StoreOptions storeOptions;
                storeOptions.topOverrides = {std::string(graph.symbol())};
                auto snapshot = serializer.storeToString(design, storeOptions);
                if (!snapshot) throw std::invalid_argument("Cannot bind execution counters to the native GRH snapshot");
                counterSnapshot = std::move(*snapshot);
            }
        }
        static std::atomic<uint64_t> nonce{0};
        do {
            staging = destination.parent_path() / (destination.filename().string() + ".staging-" + std::to_string(++nonce));
        } while (!std::filesystem::create_directories(staging));
        ownsStaging = true;
        std::vector<std::string> files;
        {
            CpuStEmitContext context(graph, schedule, options, std::move(counterSnapshot));
            files = context.generate(staging);
        }
        if (enabledOption(options, "readable-dump")) {
            store::StoreText serializer(nullptr, options.session);
            store::StoreOptions snapshotOptions;
            snapshotOptions.topOverrides = {std::string(graph.symbol())};
            snapshotOptions.outputDir = staging.string();
            snapshotOptions.outputFilename = "grhsim.txt";
            if (!serializer.store(design, snapshotOptions).success)
                throw std::invalid_argument("Cannot export readable GRH snapshot");
            if (std::filesystem::file_size(staging / "grhsim.txt") > options.maxOutputFileBytes)
                throw std::invalid_argument("grhsim.txt exceeds maxOutputFileBytes");
            files.push_back("grhsim.txt");
        }
        EmitResult result;
        for (const auto& path : files) {
            result.artifacts.push_back((destination / path).string());
        }
        if (std::filesystem::exists(destination)) std::filesystem::remove(destination);
        std::filesystem::rename(staging, destination); staging.clear();
        return result;
    } catch (const std::exception& error) {
        if (ownsStaging && !staging.empty()) { std::error_code ignored; std::filesystem::remove_all(staging, ignored); }
        reportError(error.what(), "grhsim-cpu-st-cpp"); return {false, {}};
    }
}
} // namespace wolvrix::lib::emit
