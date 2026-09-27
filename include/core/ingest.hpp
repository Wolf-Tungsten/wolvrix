#ifndef WOLVRIX_INGEST_HPP
#define WOLVRIX_INGEST_HPP

#include "core/grh.hpp"
#include "core/logging.hpp"
#include "core/diagnostics.hpp"
#include "slang/text/SourceLocation.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace slang::ast {
class Compilation;
} // namespace slang::ast

namespace slang::ast {
class RootSymbol;
class InstanceBodySymbol;
class InstanceSymbol;
class DefinitionSymbol;
class Symbol;
} // namespace slang::ast

namespace wolvrix::lib::ingest {

using ValueId = wolvrix::lib::grh::ValueId;
using OperationId = wolvrix::lib::grh::OperationId;
using SymbolId = wolvrix::lib::grh::SymbolId;

class InstanceRegistry;

using ConvertDiagnosticKind = wolvrix::lib::diag::DiagnosticKind;
using ConvertDiagnostic = wolvrix::lib::diag::Diagnostic;

class ConvertDiagnostics : public wolvrix::lib::diag::Diagnostics {
public:
    using wolvrix::lib::diag::Diagnostics::Diagnostics;
    using wolvrix::lib::diag::Diagnostics::error;
    using wolvrix::lib::diag::Diagnostics::warning;
    using wolvrix::lib::diag::Diagnostics::info;
    using wolvrix::lib::diag::Diagnostics::debug;

    void error(const slang::ast::Symbol& symbol, std::string message);
    void warn(const slang::ast::Symbol& symbol, std::string message);
    void error(const slang::SourceLocation& location, std::string message,
               std::string originSymbol = {});
    void warn(const slang::SourceLocation& location, std::string message,
              std::string originSymbol = {});
};

class ConvertAbort final : public std::exception {
public:
    const char* what() const noexcept override { return "convert aborted"; }
};

using PlanIndex = uint32_t;
constexpr PlanIndex kInvalidPlanIndex = std::numeric_limits<PlanIndex>::max();

struct PlanSymbolId {
    PlanIndex index = kInvalidPlanIndex;

    bool valid() const noexcept { return index != kInvalidPlanIndex; }
};

struct StringViewHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }
};

struct StringViewEq {
    using is_transparent = void;
    bool operator()(std::string_view lhs, std::string_view rhs) const noexcept {
        return lhs == rhs;
    }
};

class PlanSymbolTable {
public:
    PlanSymbolId intern(std::string_view text);
    PlanSymbolId lookup(std::string_view text) const;
    std::string_view text(PlanSymbolId id) const;
    std::size_t size() const noexcept { return storage_.size(); }

private:
    std::deque<std::string> storage_;
    std::unordered_map<std::string_view, PlanSymbolId, StringViewHash, StringViewEq> index_;
};

struct ConvertOptions {
    bool abortOnError = true;
    bool enableLogging = false;
    bool enableTiming = false;
    LogLevel logLevel = LogLevel::Warn;
    uint32_t maxLoopIterations = 131072;
    uint32_t threadCount = 32;
    bool singleThread = false;
};

class PlanCache;
class PlanTaskQueue;

struct ConvertContext {
    const slang::ast::Compilation* compilation = nullptr;
    const slang::ast::RootSymbol* root = nullptr;
    ConvertOptions options{};
    ConvertDiagnostics* diagnostics = nullptr;
    Logger* logger = nullptr;
    PlanCache* planCache = nullptr;
    PlanTaskQueue* planQueue = nullptr;
    InstanceRegistry* instanceRegistry = nullptr;
    std::atomic<std::size_t>* taskCounter = nullptr;
    std::atomic<bool>* cancelFlag = nullptr;
};

enum class PortDirection {
    Input,
    Output,
    Inout
};

enum class SignalKind {
    Net,
    Variable,
    Memory,
    Port
};

enum class ControlDomain {
    Combinational,
    Sequential,
    Latch,
    Unknown
};

enum class ProcKind {
    Initial,
    Final,
    AlwaysComb,
    AlwaysLatch,
    AlwaysFF,
    Always,
    Unknown
};

using PortId = PlanIndex;
using SignalId = PlanIndex;
using InstanceId = PlanIndex;
using ExprNodeId = PlanIndex;

struct PortInfo {
    PlanSymbolId symbol;
    PortDirection direction = PortDirection::Input;
    int32_t width = 0;
    bool isSigned = false;
    wolvrix::lib::grh::ValueType valueType = wolvrix::lib::grh::ValueType::Logic;
    struct InoutBinding {
        PlanSymbolId inSymbol;
        PlanSymbolId outSymbol;
        PlanSymbolId oeSymbol;
    };
    std::optional<InoutBinding> inoutSymbol;
};

struct InoutSignalInfo {
    PlanSymbolId symbol;
    PortInfo::InoutBinding binding;
};

struct UnpackedDimInfo {
    int32_t extent = 1;
    int32_t left = 0;
    int32_t right = 0;
};

struct SignalInfo {
    PlanSymbolId symbol;
    SignalKind kind = SignalKind::Net;
    int32_t width = 0;
    bool isSigned = false;
    wolvrix::lib::grh::ValueType valueType = wolvrix::lib::grh::ValueType::Logic;
    int64_t memoryRows = 0;
    std::vector<int32_t> packedDims;
    std::vector<UnpackedDimInfo> unpackedDims;
};

struct InstanceParameter {
    PlanSymbolId symbol;
    std::string value;
};

struct InstanceInfo {
    const slang::ast::InstanceSymbol* instance = nullptr;
    PlanSymbolId instanceSymbol;
    PlanSymbolId moduleSymbol;
    bool isBlackbox = false;
    std::vector<InstanceParameter> parameters;
    std::string paramSignature;
};

// Plan-level record of one generate-scope declaration: all elaboration
// copies of the same declaration, in elaboration (round) order.
struct GenerateGroupPlan {
    std::string scope;                  // generate scope path, '$'-joined block names
    std::string name;                   // bare declaration name
    std::vector<PlanSymbolId> symbols;  // per-round copies in elaboration order
};

struct ModulePlan {
    const slang::ast::InstanceBodySymbol* body = nullptr;
    PlanSymbolTable symbolTable;
    PlanSymbolId moduleSymbol;
    uint32_t nextInternalSymbol = 0;
    std::vector<PortInfo> ports;
    std::vector<SignalInfo> signals;
    std::vector<InstanceInfo> instances;
    std::vector<InoutSignalInfo> inoutSignals;
    // Signals interned under scope-qualified generate names cannot be found by
    // bare-name text lookup; resolve them by slang symbol identity instead.
    std::unordered_map<const slang::ast::Symbol*, PlanSymbolId> signalBySlangSymbol;
    std::vector<GenerateGroupPlan> generateGroups;
    std::unordered_map<std::string, std::size_t> generateGroupByKey;
};

inline const PortInfo* findPortByName(const ModulePlan& plan, std::string_view name)
{
    const PlanSymbolId id = plan.symbolTable.lookup(name);
    if (!id.valid())
    {
        return nullptr;
    }
    for (const auto& port : plan.ports)
    {
        if (port.symbol.index == id.index)
        {
            return &port;
        }
    }
    return nullptr;
}

inline const PortInfo* findPortByInoutName(const ModulePlan& plan, std::string_view name)
{
    const PlanSymbolId id = plan.symbolTable.lookup(name);
    if (!id.valid())
    {
        return nullptr;
    }
    for (const auto& port : plan.ports)
    {
        if (!port.inoutSymbol)
        {
            continue;
        }
        const PortInfo::InoutBinding& inout = *port.inoutSymbol;
        if (inout.inSymbol.index == id.index || inout.outSymbol.index == id.index ||
            inout.oeSymbol.index == id.index)
        {
            return &port;
        }
    }
    return nullptr;
}

enum class ExprNodeKind {
    Invalid,
    Constant,
    Symbol,
    XmrRead,
    Operation
};

struct ExprNode {
    ExprNodeKind kind = ExprNodeKind::Invalid;
    wolvrix::lib::grh::OperationKind op = wolvrix::lib::grh::OperationKind::kConstant;
    PlanSymbolId symbol;
    PlanSymbolId tempSymbol;
    std::string literal;
    std::string systemName;
    std::string xmrPath;
    std::vector<ExprNodeId> operands;
    int32_t widthHint = 0;
    int32_t sliceWidth = 0;
    bool isSigned = false;
    wolvrix::lib::grh::ValueType valueType = wolvrix::lib::grh::ValueType::Logic;
    bool hasSideEffects = false;
    ProcKind procKind = ProcKind::Unknown;
    bool hasTiming = false;
    slang::SourceLocation location{};
};


enum class WriteSliceKind {
    None,
    BitSelect,
    RangeSelect,
    MemberSelect
};

enum class WriteRangeKind {
    Simple,
    IndexedUp,
    IndexedDown
};

struct WriteSlice {
    WriteSliceKind kind = WriteSliceKind::None;
    WriteRangeKind rangeKind = WriteRangeKind::Simple;
    ExprNodeId index = kInvalidPlanIndex;
    ExprNodeId left = kInvalidPlanIndex;
    ExprNodeId right = kInvalidPlanIndex;
    PlanSymbolId member;
    slang::SourceLocation location{};
};

struct WriteIntent {
    PlanSymbolId target;
    std::vector<WriteSlice> slices;
    ExprNodeId value = kInvalidPlanIndex;
    ExprNodeId guard = kInvalidPlanIndex;
    ControlDomain domain = ControlDomain::Unknown;
    bool isNonBlocking = false;
    bool coversAllTwoState = false;
    bool isXmr = false;
    std::string xmrPath;
    slang::SourceLocation location{};
};

enum class EventEdge {
    Posedge,
    Negedge
};

struct SystemTaskStmt {
    std::string name;
    std::vector<ExprNodeId> args;
};

struct DpiCallStmt {
    std::string targetImportSymbol;
    std::vector<std::string> inArgNames;
    std::vector<std::string> outArgNames;
    std::vector<ExprNodeId> inArgs;
    std::vector<PlanSymbolId> results;
    bool hasReturn = false;
};

struct DpiImportInfo {
    std::string symbol;
    std::vector<std::string> argsDirection;
    std::vector<int64_t> argsWidth;
    std::vector<std::string> argsName;
    std::vector<bool> argsSigned;
    std::vector<std::string> argsType;
    bool hasReturn = false;
    int64_t returnWidth = 0;
    bool returnSigned = false;
    std::string returnType;
};

enum class LoweredStmtKind {
    Write,
    SystemTask,
    DpiCall
};

struct LoweredStmt {
    LoweredStmtKind kind = LoweredStmtKind::Write;
    wolvrix::lib::grh::OperationKind op = wolvrix::lib::grh::OperationKind::kAssign;
    ExprNodeId updateCond = kInvalidPlanIndex;
    ProcKind procKind = ProcKind::Unknown;
    bool hasTiming = false;
    std::vector<EventEdge> eventEdges;
    std::vector<ExprNodeId> eventOperands;
    slang::SourceLocation location{};
    WriteIntent write;
    SystemTaskStmt systemTask;
    DpiCallStmt dpiCall;
};

struct MemoryReadPort {
    PlanSymbolId memory;
    SignalId signal = kInvalidPlanIndex;
    ExprNodeId address = kInvalidPlanIndex;
    ExprNodeId data = kInvalidPlanIndex;
    ExprNodeId replacement = kInvalidPlanIndex;
    bool isSync = false;
    ExprNodeId updateCond = kInvalidPlanIndex;
    std::vector<EventEdge> eventEdges;
    std::vector<ExprNodeId> eventOperands;
    slang::SourceLocation location{};
};

struct MemoryWritePort {
    PlanSymbolId memory;
    SignalId signal = kInvalidPlanIndex;
    ExprNodeId address = kInvalidPlanIndex;
    ExprNodeId data = kInvalidPlanIndex;
    ExprNodeId mask = kInvalidPlanIndex;
    ExprNodeId updateCond = kInvalidPlanIndex;
    bool isMasked = false;
    std::vector<EventEdge> eventEdges;
    std::vector<ExprNodeId> eventOperands;
    slang::SourceLocation location{};
};

struct MemoryFillPort {
    PlanSymbolId memory;
    SignalId signal = kInvalidPlanIndex;
    ExprNodeId data = kInvalidPlanIndex;
    ExprNodeId updateCond = kInvalidPlanIndex;
    std::vector<EventEdge> eventEdges;
    std::vector<ExprNodeId> eventOperands;
    slang::SourceLocation location{};
};

struct MemoryInit {
    PlanSymbolId memory;
    std::string kind;  // "readmemh", "readmemb", "literal"
    std::string file;  // for readmemh/readmemb
    std::string initValue;  // for literal: "0", "1", "8'hAB", "$random", "$random(12345)"
    int64_t start = -1;  // <0 means omit address range / full init
    int64_t len = 0;  // <=0 means "no finish" for readmem; ignored when start < 0
    slang::SourceLocation location{};
};

struct RegisterInit {
    PlanSymbolId reg;
    std::string initValue;  // "0", "1", "8'hAB", "$random", "$random(12345)"
    slang::SourceLocation location{};
};

struct LoweringPlan {
    std::vector<ExprNode> values;
    std::vector<PlanSymbolId> tempSymbols;
    std::vector<WriteIntent> writes;
    std::vector<LoweredStmt> loweredStmts;
    std::vector<DpiImportInfo> dpiImports;
    std::vector<MemoryReadPort> memoryReads;
    std::vector<MemoryWritePort> memoryWrites;
    std::vector<MemoryFillPort> memoryFills;
    std::vector<MemoryInit> memoryInits;
    std::vector<RegisterInit> registerInits;
};

struct WriteBackPlan {
    struct Entry {
        PlanSymbolId target;
        SignalId signal = kInvalidPlanIndex;
        ControlDomain domain = ControlDomain::Unknown;
        ExprNodeId updateCond = kInvalidPlanIndex;
        ExprNodeId nextValue = kInvalidPlanIndex;
        bool hasStaticSlice = false;
        int64_t sliceLow = 0;
        int64_t sliceWidth = 0;
        std::vector<EventEdge> eventEdges;
        std::vector<ExprNodeId> eventOperands;
        slang::SourceLocation location{};
    };

    std::vector<Entry> entries;
};

struct PlanArtifacts {
    std::optional<LoweringPlan> loweringPlan;
    std::optional<WriteBackPlan> writeBackPlan;
};

struct PlanKey {
    const slang::ast::DefinitionSymbol* definition = nullptr;
    const slang::ast::InstanceBodySymbol* body = nullptr;
    std::string paramSignature;

    bool operator==(const PlanKey& other) const noexcept {
        if (definition || other.definition) {
            return definition == other.definition && paramSignature == other.paramSignature;
        }
        return body == other.body && paramSignature == other.paramSignature;
    }
};

struct PlanKeyHash {
    std::size_t operator()(const PlanKey& key) const noexcept {
        const void* defKey = key.definition ? static_cast<const void*>(key.definition)
                                            : static_cast<const void*>(key.body);
        const std::size_t h1 = std::hash<const void*>{}(defKey);
        const std::size_t h2 = std::hash<std::string>{}(key.paramSignature);
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};

enum class PlanStatus {
    Pending,
    Planning,
    Done,
    Failed
};

struct PlanEntry {
    PlanStatus status = PlanStatus::Pending;
    std::optional<ModulePlan> plan;
    PlanArtifacts artifacts;
};

class PlanCache {
public:
    bool tryClaim(const PlanKey& key);
    void storePlan(const PlanKey& key, ModulePlan plan);
    void markFailed(const PlanKey& key);
    std::optional<ModulePlan> findReady(const PlanKey& key) const;
    void clear();
    bool setLoweringPlan(const PlanKey& key, LoweringPlan plan);
    bool setWriteBackPlan(const PlanKey& key, WriteBackPlan plan);
    std::optional<LoweringPlan> getLoweringPlan(const PlanKey& key) const;
    std::optional<WriteBackPlan> getWriteBackPlan(const PlanKey& key) const;
    bool withLoweringPlan(const PlanKey& key,
                          const std::function<void(const LoweringPlan&)>& fn) const;
    bool withWriteBackPlan(const PlanKey& key,
                           const std::function<void(const WriteBackPlan&)>& fn) const;
    bool withLoweringPlanMut(const PlanKey& key,
                             const std::function<void(LoweringPlan&)>& fn);
    bool withWriteBackPlanMut(const PlanKey& key,
                              const std::function<void(WriteBackPlan&)>& fn);

private:
    PlanEntry& getOrCreateEntryLocked(const PlanKey& key);
    PlanEntry* findEntryLocked(const PlanKey& key);
    const PlanEntry* findEntryLocked(const PlanKey& key) const;

    mutable std::mutex mutex_;
    std::unordered_map<PlanKey, PlanEntry, PlanKeyHash> entries_;
};

class PlanTaskQueue {
public:
    void push(PlanKey key);
    bool tryPush(PlanKey key);
    bool tryPop(PlanKey& out);
    bool waitPop(PlanKey& out, const std::atomic<bool>* cancelFlag = nullptr);
    void close();
    std::size_t drain();
    bool closed() const noexcept;
    std::size_t size() const;
    void reset();

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<PlanKey> queue_;
    bool closed_ = false;
};

class ModulePlanner {
public:
    explicit ModulePlanner(ConvertContext& context) : context_(context) {}

    ModulePlan plan(const slang::ast::InstanceBodySymbol& body);

private:
    ConvertContext& context_;
};

class StmtLowererPass {
public:
    explicit StmtLowererPass(ConvertContext& context) : context_(context) {}

    void lower(ModulePlan& plan, LoweringPlan& lowering);

private:
    ConvertContext& context_;
};

class WriteBackPass {
public:
    explicit WriteBackPass(ConvertContext& context) : context_(context) {}

    WriteBackPlan lower(ModulePlan& plan, LoweringPlan& lowering);

private:
    ConvertContext& context_;
};

class MemoryPortLowererPass {
public:
    explicit MemoryPortLowererPass(ConvertContext& context) : context_(context) {}

    void lower(ModulePlan& plan, LoweringPlan& lowering);

private:
    ConvertContext& context_;
};

class GraphAssembler {
public:
    explicit GraphAssembler(ConvertContext& context, wolvrix::lib::grh::Design& design,
                            std::mutex* designMutex = nullptr)
        : context_(context), design_(design), designMutex_(designMutex) {}

    const std::string& resolveGraphName(const PlanKey& key, std::string_view moduleName);

    wolvrix::lib::grh::Graph& build(const PlanKey& key, const ModulePlan& plan, LoweringPlan& lowering,
                          const WriteBackPlan& writeBack);

private:
    ConvertContext& context_;
    wolvrix::lib::grh::Design& design_;
    std::mutex* designMutex_ = nullptr;
    std::size_t nextAnonymousId_ = 0;
    std::unordered_map<PlanKey, std::string, PlanKeyHash> graphNames_{};
    std::unordered_set<std::string> reservedGraphNames_{};
    mutable std::mutex nameMutex_{};
};

class ConvertDriver {
public:
    explicit ConvertDriver(ConvertOptions options = {});

    wolvrix::lib::grh::Design convert(const slang::ast::RootSymbol& root);

    ConvertDiagnostics& diagnostics() noexcept { return diagnostics_; }
    Logger& logger() noexcept { return logger_; }

private:
    ConvertOptions options_{};
    ConvertDiagnostics diagnostics_{};
    Logger logger_{};
    PlanCache planCache_{};
    PlanTaskQueue planQueue_{};
};

} // namespace wolvrix::lib::ingest

#endif // WOLVRIX_INGEST_HPP
