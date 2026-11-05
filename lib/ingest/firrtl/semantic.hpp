#ifndef WOLVRIX_FIRRTL_SEMANTIC_HPP
#define WOLVRIX_FIRRTL_SEMANTIC_HPP

#include "internal.hpp"

#include <functional>
#include <map>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace wolvrix::lib::ingest::firrtl::detail {

struct Literal {
    std::string bits; // MSB first, minimal two's-complement representation.
    int64_t minimum = 1;
    bool negative = false;
};
Literal literal(std::string_view text, bool isSigned, Range range);

struct SemanticType;
using TypePtr = std::shared_ptr<SemanticType>;
struct SemanticField {
    std::string name;
    bool flip = false;
    TypePtr type;
};
struct SemanticType {
    TypeKind kind = TypeKind::Unknown;
    uint32_t width = 0;
    uint32_t reset = 0;
    bool constant = false;
    int64_t extent = 0;
    TypePtr element;
    std::vector<SemanticField> fields;
    std::string layer;
};

struct Width {
    int64_t value = -1;
    bool fixed = false;
    Range range;
    std::vector<std::size_t> users;
};
struct WidthConstraint {
    uint32_t result = 0;
    std::vector<uint32_t> arguments;
    std::function<int64_t(const std::vector<int64_t>&)> calculate;
    Range range;
};

struct InterfaceLeaf {
    PortLeafSignature port;
    std::string declaration;
    std::string target; // Empty for this module's own ports.
    std::vector<std::string> fields; // Instance port path, without the instance name.
    uint32_t width = 0;
    uint32_t resetGroup = 0;
    TypeKind kind = TypeKind::Unknown;
    Range range;
};
struct InterfaceEvaluation {
    std::vector<int64_t> widths;
    std::vector<unsigned> resets;
};
struct InterfaceCheck {
    enum class Kind { ExactlyOne, AtMostOne, Equal, ResetConstant } kind = Kind::ExactlyOne;
    uint32_t left = 0;
    uint32_t right = 0;
    Range range;
    std::string_view message;
    TypeKind resetKind = TypeKind::Unknown;
    ExprId expression = 0;
    bool provenConstant = true;
};
struct InterfaceWidthLimit {
    uint32_t width = 0;
    int64_t maximum = 0;
    Range range;
};

// Immutable inference network. Unlike Semantics, this retains no declaration
// trees, expression types, guards, connections, effects, or deferred checks.
// Width transfer functions are the very same functions used by strict analysis.
class InterfaceSummary {
public:
    std::vector<InterfaceLeaf> leaves;
    std::vector<std::string> dependencies; // Enabled instance targets, in source order.
    InterfaceEvaluation evaluate(const std::map<std::string, ModuleSignature>& interfaces) const;
private:
    std::string module_;
    std::vector<Width> widths_;
    std::vector<WidthConstraint> constraints_;
    std::vector<unsigned> resets_;
    std::vector<InterfaceCheck> checks_;
    std::vector<InterfaceWidthLimit> limits_;
    std::exception_ptr failure_;
    friend class Semantics;
    friend InterfaceSummary summarizeInterface(const Source&, const ModuleBody&, const FirrtlOptions&);
};
InterfaceSummary summarizeInterface(const Source& source, const ModuleBody& body,
                                     const FirrtlOptions& options);
std::shared_ptr<const InterfaceSummary> sharedInterfaceSummary(std::shared_ptr<const Source> source,
                                                              std::size_t module,
                                                              const FirrtlOptions& options);

// Reduced Boolean DAG used for path coverage, independently of emitted RTL ops.
class Guards {
public:
    struct Branch { int variable = -1; int low = 0; int high = 0; };
    std::vector<Branch> branches{{}, {}};
    std::vector<ExprId> expressions;
    int variable(std::string key, ExprId expression);
    int conjunction(int left, int right);
    int disjunction(int left, int right);
    int negate(int value);
private:
    std::map<std::string, int> variables_;
    std::map<std::tuple<int, int, int>, int> unique_;
    std::map<std::tuple<bool, int, int>, int> memo_;
    int make(int variable, int low, int high);
    int apply(bool conjunction, int left, int right);
};

struct Projection {
    ExprId expression = 0;
    std::vector<std::string> fields;
};
struct Connection {
    Projection sink;
    Projection source;
    int guard = 1;
    bool invalid = false;
    Range range;
    bool define = false;
};
struct Declaration {
    struct Register {
        ExprId clock = 0;
        std::optional<ExprId> reset;
        std::optional<ExprId> initial;
    };
    std::string name;
    TypePtr type;
    Range range;
    int flow = 3; // source=1, sink=2, duplex=3; bundle flips exchange source/sink.
    std::optional<ExprId> definition;
    bool port = false;
    int guard = 1;
    std::optional<Register> state;
    std::size_t conditionDepth = 0;
    std::string layer;
};
struct Leaf {
    TypePtr type;
    std::vector<int64_t> shape;
    std::vector<std::string> fields;
    int flow = 3;
    TypeKind probe = TypeKind::Unknown;
    std::vector<std::size_t> shapeFieldDepths;
};

// Both standard memory ports and CHIRRTL accesses use this frontend-only model.
struct MemoryPort {
    std::string name;
    bool read = false;
    bool write = false;
    bool inferred = false;
    std::optional<ExprId> address;
    std::optional<ExprId> clock;
    int guard = 1;
    Range range;
};
struct Memory {
    std::string name;
    TypePtr data;
    int64_t depth = 0;
    int64_t readLatency = 0;
    int64_t writeLatency = 1;
    std::string readUnderWrite = "undefined";
    bool standard = false;
    std::vector<MemoryPort> ports;
    Range range;
};
struct Effect {
    StmtId statement = 0;
    int guard = 1;
};
struct Instance {
    std::string name;
    std::string target;
    Range range;
};

struct SemanticNameHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view value) const noexcept { return std::hash<std::string_view>{}(value); }
    std::size_t operator()(const std::string& value) const noexcept { return (*this)(std::string_view(value)); }
};
struct SemanticNameEqual {
    using is_transparent = void;
    bool operator()(std::string_view left, std::string_view right) const noexcept { return left == right; }
};

class Semantics {
public:
    Semantics(const Source& source, const ModuleBody& body, const FirrtlOptions& options,
              const std::map<std::string, ModuleSignature>* interfaces = nullptr);
    void analyze(bool allowUnknownWidths = false);
    InterfaceSummary interfaceSummary();
    bool analyzed() const noexcept { return analyzed_; }
    TypePtr expression(ExprId id);
    std::vector<Leaf> leaves(TypePtr type, int flow = 1) const;
    const std::vector<Leaf>& leavesCached(TypePtr type, int flow = 1) const;
    int64_t width(TypePtr type) const;
    int64_t total(const Leaf& leaf) const;
    TypeKind resolvedKind(TypePtr type) const;
    uint32_t resetGroup(TypePtr type) const;
    unsigned resetConstraint(TypePtr type) const;
    TypePtr projected(TypePtr type, const std::vector<std::string>& fields) const;
    std::optional<int64_t> staticIndex(ExprId id) const;
    std::string expressionKey(ExprId id) const;
    bool constantValue(ExprId id) const;
    const Declaration& declaration(std::string_view name) const;

    const Source& source;
    const ModuleBody& body;
    std::unordered_map<std::string, Declaration, SemanticNameHash, SemanticNameEqual> declarations;
    std::vector<std::string> declarationOrder;
    std::vector<Connection> connections;
    std::vector<TypePtr> expressions;
    Guards guards;
    std::map<std::string, Memory> memories;
    std::unordered_map<std::string, std::pair<std::string, std::size_t>,
                       SemanticNameHash, SemanticNameEqual> memoryPorts;
    std::vector<Effect> effects;
    std::vector<Instance> instances;
    std::unordered_set<StmtId> activeStatements;
    bool layerEnabled(std::string_view path) const;

private:
    using ConstantLeaf = std::pair<std::string, std::vector<std::string>>;
    using ConstantSelection = std::tuple<std::string, std::vector<std::string>, std::vector<std::optional<int64_t>>>;
    struct ConstantDriver { std::size_t connection; std::vector<ExprId> indices; };
    mutable bool constantDriversReady_ = false;
    mutable std::map<ConstantLeaf, std::vector<ConstantDriver>> constantDrivers_;
    mutable std::map<ConstantSelection, bool> constantValues_;
    mutable std::map<std::pair<TypePtr, int>, std::vector<Leaf>> leavesCache_;
    // Map nodes own both names and declarations and are never erased during
    // this analysis. Cache only hits so later declarations remain visible.
    mutable std::unordered_map<std::string_view, const Declaration*> declarationCache_;
    const Declaration* findDeclaration(std::string_view name) const;
    std::vector<Width> widths_;
    std::vector<WidthConstraint> constraints_;
    std::vector<uint32_t> resetParents_;
    std::vector<unsigned> resetFlags_;
    std::vector<std::function<void()>> checks_;
    std::vector<TypePtr> conditions_;
    std::vector<int> conditionGuards_;
    std::unordered_map<std::string, StmtId> nodeDeclarations_;
    std::unordered_map<std::string, std::string> nodeLayers_;
    std::unordered_set<std::string> activeNodes_;
    std::unordered_set<std::string> knownLayers_;
    std::vector<std::string> enabledLayers_;
    std::string activeLayer_;
    bool analyzed_ = false;
    bool summarizing_ = false;
    bool captureSummaryConnections_ = false;
    std::vector<InterfaceCheck> summaryChecks_;
    const std::map<std::string, ModuleSignature>* interfaces_;
    std::vector<Leaf> computeLeaves(TypePtr type, int flow) const;
    // The interface map is owned by Transaction and may be replaced between
    // Semantics objects. Keep only borrowed first-match pointers for this
    // analysis, indexing only interfaces used by this module. Never retain
    // this cache past the current Semantics object.
    mutable std::unordered_map<std::string_view,
        std::map<std::vector<std::string>, const PortLeafSignature*>> interfacePorts_;
    const PortLeafSignature* resolvedPort(std::string_view module,
                                          const std::vector<std::string>& fields) const;
    std::string layerPath(const Statement& statement) const;
    uint32_t newWidth(int64_t value, Range range);
    uint32_t newReset(unsigned flags);
    uint32_t resetRoot(uint32_t id);
    unsigned resetFlags(uint32_t id) const;
    void unifyReset(TypePtr left, TypePtr right, Range range);
    void constrain(uint32_t result, std::vector<uint32_t> args,
                   std::function<int64_t(const std::vector<int64_t>&)> calculate, Range range);
    TypePtr ground(TypeKind kind, int64_t width, Range range, bool constant = false);
    TypePtr declaredType(const Arena& arena, TypeId id, Range range, bool constant = false);
    void collect(std::span<const StmtId> statements);
    void statements(std::span<const StmtId> statements, int guard);
    void connect(Projection left, TypePtr leftType, Projection right, TypePtr rightType,
                 int guard, Range range, bool invalid = false);
    void equivalent(TypePtr left, TypePtr right, Range range);
    TypePtr merge(TypePtr left, TypePtr right, Range range);
    bool passive(TypePtr type) const;
    int flow(ExprId id) const;
    void solve(bool allowUnknownWidths);
    void prepare();
    template <typename Check> void defer(Check&& check) {
        if (!summarizing_) checks_.emplace_back(std::forward<Check>(check));
    }
    void normalizeGuards();
    void collectMemory(const Statement& statement);
    void collectMemoryPort(const Statement& statement);
    void inferMemoryPorts(std::span<const StmtId> statements);
};

ModuleSignature lowerModule(const Source& source, const ModuleBody& body,
                            grh::Design& design, const FirrtlOptions& options,
                            const std::map<std::string, ModuleSignature>* interfaces = nullptr);
ModuleSignature lowerModule(const Source& source, const ModuleBody& body,
                            grh::Graph& graph, const FirrtlOptions& options,
                            const std::map<std::string, ModuleSignature>* interfaces = nullptr);
ModuleSignature lowerModule(Semantics& semantics, grh::Graph& graph,
                            const FirrtlOptions& options);

} // namespace wolvrix::lib::ingest::firrtl::detail
#endif
