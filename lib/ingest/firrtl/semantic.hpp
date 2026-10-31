#ifndef WOLVRIX_FIRRTL_SEMANTIC_HPP
#define WOLVRIX_FIRRTL_SEMANTIC_HPP

#include "internal.hpp"

#include <functional>
#include <map>
#include <tuple>

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

class Semantics {
public:
    Semantics(const Source& source, const ModuleBody& body, const FirrtlOptions& options);
    void analyze();
    TypePtr expression(ExprId id);
    std::vector<Leaf> leaves(TypePtr type, int flow = 1) const;
    int64_t width(TypePtr type) const;
    int64_t total(const Leaf& leaf) const;
    TypeKind resolvedKind(TypePtr type) const;
    TypePtr projected(TypePtr type, const std::vector<std::string>& fields) const;
    std::optional<int64_t> staticIndex(ExprId id) const;
    std::string expressionKey(ExprId id) const;

    const Source& source;
    const ModuleBody& body;
    std::map<std::string, Declaration> declarations;
    std::vector<std::string> declarationOrder;
    std::vector<Connection> connections;
    std::vector<TypePtr> expressions;
    Guards guards;
    std::map<std::string, Memory> memories;
    std::map<std::string, std::pair<std::string, std::size_t>> memoryPorts;
    std::vector<Effect> effects;
    std::unordered_set<StmtId> activeStatements;
    bool layerEnabled(std::string_view path) const;

private:
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
    void solve();
    void normalizeGuards();
    void collectMemory(const Statement& statement);
    void collectMemoryPort(const Statement& statement);
    void inferMemoryPorts(std::span<const StmtId> statements);
};

ModuleSignature lowerModule(const Source& source, const ModuleBody& body,
                            grh::Design& design, const FirrtlOptions& options);

} // namespace wolvrix::lib::ingest::firrtl::detail
#endif
