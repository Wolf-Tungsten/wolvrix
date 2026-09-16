#ifndef WOLVRIX_GRHSIM_IR_MODEL_HPP
#define WOLVRIX_GRHSIM_IR_MODEL_HPP

#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace wolvrix::lib::grhsim
{

    template <typename Tag>
    struct Id
    {
        uint32_t index = 0;
        uint32_t generation = 0;

        constexpr bool valid() const noexcept { return index != 0; }
        static constexpr Id invalid() noexcept { return {}; }
        explicit constexpr operator bool() const noexcept { return valid(); }
        friend constexpr bool operator==(Id, Id) noexcept = default;
    };

    struct StringTag;
    struct TypeTag;
    struct InputTag;
    struct OutputTag;
    struct StateTag;
    struct FuncTag;
    struct ValueTag;
    struct OpTag;
    struct OriginTag;
    struct PartitionTag;
    struct CpuTypeTag;
    struct CpuTaskTag;

    using StringId = Id<StringTag>;
    using TypeId = Id<TypeTag>;
    using InputId = Id<InputTag>;
    using OutputId = Id<OutputTag>;
    using StateId = Id<StateTag>;
    using FuncId = Id<FuncTag>;
    using ValueId = Id<ValueTag>;
    using OpId = Id<OpTag>;
    using OriginId = Id<OriginTag>;
    using PartitionId = Id<PartitionTag>;
    using CpuTypeId = Id<CpuTypeTag>;
    using CpuTaskId = Id<CpuTaskTag>;

    struct Range
    {
        uint32_t offset = 0;
        uint32_t count = 0;
    };

    using ParameterValue = std::variant<
        bool,
        int64_t,
        double,
        std::string,
        std::vector<bool>,
        std::vector<int64_t>,
        std::vector<double>,
        std::vector<std::string>>;

    struct Parameter
    {
        StringId name;
        ParameterValue value;
    };

    class StringInterner
    {
    public:
        StringId intern(std::string_view text);
        StringId lookup(std::string_view text) const noexcept;
        std::string_view text(StringId id) const;
        bool valid(StringId id) const noexcept;
        std::size_t size() const noexcept { return textById_.size(); }
        const std::deque<std::string> &strings() const noexcept { return textById_; }
        void reserve(std::size_t count);

    private:
        struct Hash
        {
            using is_transparent = void;
            std::size_t operator()(std::string_view value) const noexcept;
            std::size_t operator()(const std::string &value) const noexcept;
        };

        struct Equal
        {
            using is_transparent = void;
            bool operator()(std::string_view lhs, std::string_view rhs) const noexcept;
        };

        std::unordered_map<std::string, StringId, Hash, Equal> idByText_;
        std::deque<std::string> textById_;
    };

    struct ModelIdentity
    {
        uint64_t high = 0;
        uint64_t low = 0;

        friend constexpr bool operator==(ModelIdentity, ModelIdentity) noexcept = default;
    };

    enum class LogicDomain : uint8_t
    {
        TwoState,
        FourState
    };

    enum class TypeKind : uint8_t
    {
        Logic,
        Real,
        String,
        Array
    };

    struct Type
    {
        TypeId id;
        StringId typeRef;
        TypeKind kind = TypeKind::Logic;
        uint32_t width = 0;
        bool isSigned = false;
        LogicDomain domain = LogicDomain::FourState;
        TypeId elementType;
        uint64_t count = 0;
    };

    struct DialectUse
    {
        StringId name;
        StringId version;
        StringId schemaFingerprint;
    };

    struct InputObject
    {
        InputId id;
        StringId name;
        TypeId type;
        OriginId origin;
    };

    struct OutputObject
    {
        OutputId id;
        StringId name;
        TypeId type;
        OriginId origin;
    };

    struct StateObject
    {
        StateId id;
        StringId name;
        TypeId type;
        OriginId origin;
    };

    enum class DpiDirection : uint8_t
    {
        Input,
        Output,
        Inout
    };

    struct DpiArgument
    {
        StringId name;
        DpiDirection direction = DpiDirection::Input;
        TypeId type;
    };

    struct ExternFunction
    {
        FuncId id;
        StringId name;
        StringId declRef;
        StringId symbol;
        Range arguments;
        TypeId returnType;
        OriginId origin;
    };

    enum class ObjectKind : uint8_t
    {
        Input,
        Output,
        State,
        Function
    };

    struct ObjectRef
    {
        ObjectKind kind = ObjectKind::Input;
        uint32_t index = 0;
        uint32_t generation = 0;

        bool valid() const noexcept { return index != 0; }
        friend bool operator==(const ObjectRef &, const ObjectRef &) = default;

        static ObjectRef input(InputId id) noexcept;
        static ObjectRef output(OutputId id) noexcept;
        static ObjectRef state(StateId id) noexcept;
        static ObjectRef function(FuncId id) noexcept;
    };

    enum class InterfaceDirection : uint8_t
    {
        Input,
        Output,
        Inout
    };

    struct InterfacePort
    {
        StringId name;
        InterfaceDirection direction = InterfaceDirection::Input;
        InputId input;
        OutputId output;
        OutputId outputEnable;
    };

    struct Origin
    {
        OriginId id;
        StringId sourceKind;
        StringId symbol;
        uint32_t sourceIndex = 0;
        StringId file;
        uint32_t line = 0;
        uint32_t column = 0;
        uint32_t endLine = 0;
        uint32_t endColumn = 0;
        StringId pass;
        StringId note;
    };

    struct SimValue
    {
        ValueId id;
        TypeId type;
        StringId name;
        OriginId origin;
    };

    struct SimOp
    {
        OpId id;
        StringId opType;
        StringId name;
        Range operands;
        Range results;
        Range objectRefs;
        Range parameters;
        OriginId origin;
    };

    struct InitStep
    {
        StringId kind;
        Range parameters;
    };

    struct InitRecord
    {
        StateId state;
        Range steps;
    };

    enum class CpuPhase : uint8_t { None, Compute, Commit };
    enum class CpuPartitionKind : uint8_t { Root, Phase, EventDomain, Supernode, Node, ActiveWord, EmitFunction };
    enum class CpuEventSource : uint8_t { Input, Derived };
    enum class CpuEventEdge : uint8_t { Posedge, Negedge };
    enum class CpuMappingStage : uint8_t { SplitPhase, EventDomains, ComputeNodes, ComputeSupernodes, ActiveWords, EmitFunctions, DataLayout, Schedule };

    struct CpuEvent
    {
        ValueId value;
        CpuEventEdge edge = CpuEventEdge::Posedge;
        friend bool operator==(const CpuEvent &, const CpuEvent &) = default;
    };

    struct CpuEventGate
    {
        CpuEventSource source = CpuEventSource::Derived;
        std::vector<CpuEvent> events;
        friend bool operator==(const CpuEventGate &, const CpuEventGate &) = default;
    };

    struct CpuPartitionAttrs
    {
        CpuPartitionKind kind = CpuPartitionKind::Root;
        CpuPhase phase = CpuPhase::None;
        // An event-domain partition without a gate is scanned every round.
        std::optional<CpuEventGate> eventGate;
        std::optional<uint32_t> activeId;
        std::optional<uint32_t> activeWord;
        // Ranges in the supernode's flattened operation order, not model OpIds.
        std::vector<Range> helperChunks;
    };

    struct CpuPartition
    {
        PartitionId id;
        PartitionId parent;
        std::vector<PartitionId> children;
        std::vector<OpId> ops;
        CpuPartitionAttrs attrs;
    };

    struct CpuPartitionTree
    {
        PartitionId root;
        std::vector<CpuPartition> partitions;
    };

    enum class CpuTypeKind : uint8_t { Bool, UInt, SInt, F32, F64, String, Array };
    enum class CpuStorageKind : uint8_t { Object, PartitionLocal, Boundary };
    enum class CpuRuntimeKind : uint8_t { ActiveWord, DomainArm, EventEdge };

    struct CpuType
    {
        CpuTypeId id;
        CpuTypeKind kind = CpuTypeKind::Bool;
        uint32_t width = 0;
        CpuTypeId elementType;
        uint64_t count = 0;
        uint64_t size = 0;
        uint32_t alignment = 1;
        friend bool operator==(const CpuType &, const CpuType &) = default;
    };

    struct CpuDataSlot
    {
        CpuTypeId type;
        CpuStorageKind kind = CpuStorageKind::Object;
        PartitionId owner;
        uint64_t offset = 0;
        friend bool operator==(const CpuDataSlot &, const CpuDataSlot &) = default;
    };

    struct CpuObjectLayout
    {
        ObjectRef object;
        CpuDataSlot slot;
        friend bool operator==(const CpuObjectLayout &, const CpuObjectLayout &) = default;
    };

    struct CpuLocalFrame
    {
        PartitionId owner;
        uint64_t size = 0;
        uint32_t alignment = 1;
        friend bool operator==(const CpuLocalFrame &, const CpuLocalFrame &) = default;
    };

    struct CpuRuntimeSlot
    {
        CpuRuntimeKind kind = CpuRuntimeKind::ActiveWord;
        PartitionId owner;
        ValueId value;
        CpuEventEdge edge = CpuEventEdge::Posedge;
        uint64_t offset = 0;
        friend bool operator==(const CpuRuntimeSlot &, const CpuRuntimeSlot &) = default;
    };

    struct CpuHelperReadCache
    {
        OpId firstOp;
        std::vector<ValueId> values;
        friend bool operator==(const CpuHelperReadCache &, const CpuHelperReadCache &) = default;
    };

    struct CpuDataLayout
    {
        uint32_t pointerBytes = 8;
        std::vector<CpuType> types;
        std::vector<CpuObjectLayout> objects;
        // Dense ValueId order; local offsets refer to the owning supernode frame.
        std::vector<CpuDataSlot> values;
        std::vector<CpuLocalFrame> localFrames;
        std::vector<CpuRuntimeSlot> runtime;
        uint64_t objectBytes = 0;
        uint64_t boundaryBytes = 0;
        uint64_t runtimeBytes = 0;
        // Optional for old checkpoints. Cache stable scalar boundary inputs
        // referenced more than once in the helper beginning at firstOp.
        std::optional<std::vector<CpuHelperReadCache>> helperReadCaches;
        friend bool operator==(const CpuDataLayout &, const CpuDataLayout &) = default;
    };

    enum class CpuExecution : uint8_t { ActivityDrivenCompute, DomainGatedCommit, AlwaysScanCommit };

    struct CpuActivationTargets
    {
        std::vector<PartitionId> activate;
        std::vector<PartitionId> arm;
        friend bool operator==(const CpuActivationTargets &, const CpuActivationTargets &) = default;
    };

    template <typename SourceId>
    struct CpuFanoutEntry
    {
        SourceId source;
        CpuActivationTargets targets;
        friend bool operator==(const CpuFanoutEntry &, const CpuFanoutEntry &) = default;
    };

    struct CpuScheduledTask
    {
        CpuTaskId id;
        PartitionId partition;
        std::vector<CpuTaskId> waitsFor;
        CpuExecution execution = CpuExecution::ActivityDrivenCompute;
        friend bool operator==(const CpuScheduledTask &, const CpuScheduledTask &) = default;
    };

    struct CpuCoreSchedule
    {
        uint32_t core = 0;
        std::vector<CpuScheduledTask> tasks;
        friend bool operator==(const CpuCoreSchedule &, const CpuCoreSchedule &) = default;
    };

    struct CpuNumaSchedule
    {
        uint32_t numaNode = 0;
        std::vector<CpuCoreSchedule> cores;
        friend bool operator==(const CpuNumaSchedule &, const CpuNumaSchedule &) = default;
    };

    struct CpuInputShadow
    {
        ValueId value;
        CpuTypeId type;
        uint64_t offset = 0;
        friend bool operator==(const CpuInputShadow &, const CpuInputShadow &) = default;
    };

    struct CpuSchedulePlan
    {
        std::vector<CpuNumaSchedule> numaNodes;
        std::vector<CpuFanoutEntry<ValueId>> inputFanout;
        std::vector<CpuFanoutEntry<ValueId>> computeSupernodeFanout;
        // Commit fanout covers every state read by a compute partition; the key set
        // is no longer limited to the output/event state dependency closure E.
        std::vector<CpuFanoutEntry<StateId>> commitStateFanout;
        // State-indexed bitmap of the closure E: decides a pending record's
        // convergence flag, not reader arming.
        std::vector<bool> quiescenceProjection;
        std::vector<PartitionId> roundSeeds;
        std::vector<CpuInputShadow> inputShadows;
        uint64_t inputShadowBytes = 0;
        friend bool operator==(const CpuSchedulePlan &, const CpuSchedulePlan &) = default;
    };

    struct CpuBackendMapping
    {
        CpuMappingStage stage = CpuMappingStage::SplitPhase;
        CpuPartitionTree partitionTree;
        std::optional<CpuDataLayout> dataLayout;
        std::optional<CpuSchedulePlan> schedule;
    };

    struct BackendMapping
    {
        StringId backend;
        StringId schema;
        bool complete = false;
        Range parameters;
        ModelIdentity sourceIdentity;
        uint64_t sourceSemanticRevision = 0;
        std::optional<CpuBackendMapping> cpu;
    };

    struct ModelReserve
    {
        std::size_t strings = 0;
        std::size_t dialects = 0;
        std::size_t types = 0;
        std::size_t inputs = 0;
        std::size_t outputs = 0;
        std::size_t states = 0;
        std::size_t functions = 0;
        std::size_t functionArguments = 0;
        std::size_t interfacePorts = 0;
        std::size_t values = 0;
        std::size_t operations = 0;
        std::size_t operands = 0;
        std::size_t results = 0;
        std::size_t objectRefs = 0;
        std::size_t parameters = 0;
        std::size_t initRecords = 0;
        std::size_t initSteps = 0;
        std::size_t initParameters = 0;
        std::size_t mappings = 0;
        std::size_t mappingParameters = 0;
        std::size_t origins = 0;
    };

    class GrhSimModel
    {
    public:
        explicit GrhSimModel(std::string_view name = {});
        GrhSimModel(GrhSimModel &&) noexcept = default;
        GrhSimModel &operator=(GrhSimModel &&) noexcept = default;
        GrhSimModel(const GrhSimModel &) = delete;
        GrhSimModel &operator=(const GrhSimModel &) = delete;

        GrhSimModel clone() const;
        void reserve(const ModelReserve &counts);

        ModelIdentity identity() const noexcept { return identity_; }
        uint64_t semanticRevision() const noexcept { return semanticRevision_; }
        uint64_t metadataRevision() const noexcept { return metadataRevision_; }
        bool poisoned() const noexcept { return poisoned_; }
        void poison() noexcept { poisoned_ = true; }
        void commitSemanticMutation();
        void commitMetadataMutation() noexcept { ++metadataRevision_; }

        StringInterner &strings() noexcept { return strings_; }
        const StringInterner &strings() const noexcept { return strings_; }
        StringId intern(std::string_view text) { return strings_.intern(text); }
        std::string_view text(StringId id) const { return strings_.text(id); }
        StringId name() const noexcept { return name_; }
        void setName(std::string_view name) { name_ = intern(name); }

        void addDialect(std::string_view name, std::string_view version,
                        std::string_view schemaFingerprint = {});
        TypeId logicType(uint32_t width, bool isSigned, LogicDomain domain);
        TypeId realType();
        TypeId stringType();
        TypeId arrayType(TypeId elementType, uint64_t count);

        OriginId addOrigin(Origin origin);
        InputId addInput(std::string_view name, TypeId type, OriginId origin = {});
        OutputId addOutput(std::string_view name, TypeId type, OriginId origin = {});
        StateId addState(std::string_view name, TypeId type, OriginId origin = {});
        FuncId addExternFunction(std::string_view name, std::string_view declRef,
                                 std::string_view symbol, std::span<const DpiArgument> arguments,
                                 TypeId returnType = {}, OriginId origin = {});
        void addInterfacePort(InterfacePort port);
        ValueId addValue(TypeId type, std::string_view name = {}, OriginId origin = {});
        OpId addOperation(std::string_view opType,
                          std::span<const ValueId> operands,
                          std::span<const ValueId> results,
                          std::span<const ObjectRef> objectRefs = {},
                          std::span<const Parameter> parameters = {},
                          std::string_view name = {}, OriginId origin = {});
        // Keep the operation ID/name/origin; spans must not alias this model's pools.
        void replaceOperation(OpId id, std::string_view opType,
                              std::span<const ValueId> operands,
                              std::span<const ValueId> results,
                              std::span<const ObjectRef> objectRefs = {},
                              std::span<const Parameter> parameters = {});
        // Masks include unused slot zero. Removed results must have no retained users.
        // Rebuilds dense IDs/pools and drops mappings; the pass manager commits revision.
        void compact(std::span<const uint8_t> removeOps,
                     std::span<const uint8_t> removeStates);
        void addInit(StateId state, std::span<const InitStep> steps,
                     std::span<const Parameter> stepParameters);
        void addMapping(std::string_view backend, std::string_view schema, bool complete,
                        std::span<const Parameter> parameters = {});
        const CpuBackendMapping *cpuMapping() const noexcept;
        void setCpuMapping(CpuBackendMapping mapping);

        const std::vector<DialectUse> &dialects() const noexcept { return dialects_; }
        const std::vector<Type> &types() const noexcept { return types_; }
        const std::vector<InterfacePort> &interfacePorts() const noexcept { return interfacePorts_; }
        const std::vector<InputObject> &inputs() const noexcept { return inputs_; }
        const std::vector<OutputObject> &outputs() const noexcept { return outputs_; }
        const std::vector<StateObject> &states() const noexcept { return states_; }
        const std::vector<ExternFunction> &functions() const noexcept { return functions_; }
        const std::vector<DpiArgument> &functionArguments() const noexcept { return functionArguments_; }
        const std::vector<SimValue> &values() const noexcept { return values_; }
        const std::vector<SimOp> &operations() const noexcept { return operations_; }
        const std::vector<ValueId> &operandPool() const noexcept { return operandPool_; }
        const std::vector<ValueId> &resultPool() const noexcept { return resultPool_; }
        const std::vector<ObjectRef> &objectRefPool() const noexcept { return objectRefPool_; }
        const std::vector<Parameter> &parameterPool() const noexcept { return parameterPool_; }
        const std::vector<InitRecord> &initRecords() const noexcept { return initRecords_; }
        const std::vector<InitStep> &initSteps() const noexcept { return initSteps_; }
        const std::vector<Parameter> &initParameterPool() const noexcept { return initParameterPool_; }
        const std::vector<BackendMapping> &mappings() const noexcept { return mappings_; }
        const std::vector<Parameter> &mappingParameterPool() const noexcept { return mappingParameterPool_; }
        const std::vector<Origin> &origins() const noexcept { return origins_; }

        std::span<const ValueId> operands(const SimOp &op) const;
        std::span<const ValueId> results(const SimOp &op) const;
        std::span<const ObjectRef> objectRefs(const SimOp &op) const;
        std::span<const Parameter> parameters(const SimOp &op) const;
        std::span<const DpiArgument> arguments(const ExternFunction &function) const;
        std::span<const InitStep> steps(const InitRecord &record) const;
        std::span<const Parameter> parameters(const InitStep &step) const;
        std::span<const Parameter> parameters(const BackendMapping &mapping) const;

    private:
        struct ArrayTypeKey
        {
            uint32_t element = 0;
            uint64_t count = 0;
            friend bool operator==(ArrayTypeKey, ArrayTypeKey) noexcept = default;
        };

        struct ArrayTypeKeyHash
        {
            std::size_t operator()(ArrayTypeKey key) const noexcept;
        };

        static ModelIdentity nextIdentity() noexcept;
        template <typename T>
        static Range appendRange(std::vector<T> &pool, std::span<const T> values);

        ModelIdentity identity_;
        uint64_t semanticRevision_ = 1;
        uint64_t metadataRevision_ = 1;
        bool poisoned_ = false;
        StringInterner strings_;
        StringId name_;
        std::vector<DialectUse> dialects_;
        std::vector<Type> types_;
        std::unordered_map<uint64_t, TypeId> logicTypes_;
        std::unordered_map<ArrayTypeKey, TypeId, ArrayTypeKeyHash> arrayTypes_;
        TypeId realType_;
        TypeId stringType_;
        std::vector<InterfacePort> interfacePorts_;
        std::vector<InputObject> inputs_;
        std::vector<OutputObject> outputs_;
        std::vector<StateObject> states_;
        std::vector<ExternFunction> functions_;
        std::vector<DpiArgument> functionArguments_;
        std::vector<SimValue> values_;
        std::vector<SimOp> operations_;
        std::vector<ValueId> operandPool_;
        std::vector<ValueId> resultPool_;
        std::vector<ObjectRef> objectRefPool_;
        std::vector<Parameter> parameterPool_;
        std::vector<InitRecord> initRecords_;
        std::vector<InitStep> initSteps_;
        std::vector<Parameter> initParameterPool_;
        std::vector<BackendMapping> mappings_;
        std::vector<Parameter> mappingParameterPool_;
        std::vector<Origin> origins_;
    };

    std::string_view toString(LogicDomain domain) noexcept;
    std::optional<LogicDomain> parseLogicDomain(std::string_view text) noexcept;
    std::string_view toString(TypeKind kind) noexcept;
    std::optional<TypeKind> parseTypeKind(std::string_view text) noexcept;
    std::string_view toString(ObjectKind kind) noexcept;
    std::optional<ObjectKind> parseObjectKind(std::string_view text) noexcept;
    std::string_view toString(InterfaceDirection direction) noexcept;
    std::optional<InterfaceDirection> parseInterfaceDirection(std::string_view text) noexcept;
    std::string_view toString(DpiDirection direction) noexcept;
    std::optional<DpiDirection> parseDpiDirection(std::string_view text) noexcept;

} // namespace wolvrix::lib::grhsim

#endif // WOLVRIX_GRHSIM_IR_MODEL_HPP
