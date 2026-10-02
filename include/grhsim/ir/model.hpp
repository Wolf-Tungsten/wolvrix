#ifndef WOLVRIX_GRHSIM_IR_MODEL_HPP
#define WOLVRIX_GRHSIM_IR_MODEL_HPP

#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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

    // Semantic store classification (M5d-4, pass grhsim.select-state-stores):
    // the single decision point that assigns every state its store class.
    // None means the model predates classification; consumers must reject
    // None once any state is classified (the verifier enforces totality).
    // RegLatch states are small/scalar: writes merge into a next buffer and
    // P_publish commits them with one whole-block copy (NBA). Mem states are
    // large contiguous arrays: write parameters are sampled before P_mem,
    // which commits them in place in priority order. The class is a semantic
    // annotation; backends must consume it instead of re-deriving the
    // classification from the state's type (e.g. TypeKind::Array).
    enum class StateStoreClass : uint8_t
    {
        None,
        RegLatch,
        Mem
    };

    struct StateObject
    {
        StateId id;
        StringId name;
        TypeId type;
        OriginId origin;
        StateStoreClass storeClass = StateStoreClass::None;
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

    // Six-phase simulation model (P_input/P_event/P_general/P_mem/P_publish/
    // P_output) op attribution. None means the op is not yet attributed; the
    // lowering passes assign phases (P_input and P_publish have no ops).
    enum class SimPhase : uint8_t
    {
        None,
        Event,
        General,
        Mem,
        Output
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
        SimPhase phase = SimPhase::None;
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

    // Compute/Commit are the removed legacy compute/commit pipeline's phases
    // (M5d-6); the six-phase pipeline uses Event/General/Mem/Output for its
    // four phase branches. Old enum values are kept stable for checkpoint
    // compatibility.
    enum class CpuPhase : uint8_t { None, Compute, Commit, Event, General, Mem, Output };
    // EventDomain/ActiveWord are legacy partition kinds (M5d-6 removed the
    // producers); values kept stable for checkpoint compatibility.
    enum class CpuPartitionKind : uint8_t { Root, Phase, EventDomain, Supernode, Node, ActiveWord, EmitFunction };
    // SplitPhase..Schedule was the legacy two-phase pipeline (removed in
    // M5d-6). The six-phase pipeline appends SplitPhases (four-way split,
    // no longer produced since M5d-6: cpu.st.build-general-nodes initializes
    // the mapping at GeneralNodes) -> GeneralNodes -> GeneralSupernodes; the
    // M4 layout/schedule stages append LayoutNamedStores -> EventBitmaps ->
    // MemWritePlan -> PhaseSchedule, and M5d-6 moved GeneralFunctions
    // (function packing) between MemWritePlan and PhaseSchedule. M5d-7
    // appends TranslationUnits (C8 TU planning) as the terminal stage.
    // Enum VALUES stay stable; the pipeline order is no longer the numeric
    // order — use cpuMappingStageRank for stage comparisons.
    enum class CpuMappingStage : uint8_t { SplitPhase, EventDomains, ComputeNodes, ComputeSupernodes, ActiveWords, EmitFunctions, DataLayout, Schedule, SplitPhases, GeneralNodes, GeneralSupernodes, GeneralFunctions, LayoutNamedStores, EventBitmaps, MemWritePlan, PhaseSchedule, TranslationUnits };

    // Six-phase pipeline order rank (M5d-6): GeneralNodes < GeneralSupernodes
    // < LayoutNamedStores < EventBitmaps < MemWritePlan < GeneralFunctions <
    // PhaseSchedule, with the compat-only SplitPhases at 0. M5d-7 appends
    // TranslationUnits (C8 cpu.st.plan-translation-units) as the terminal
    // emit-planning stage. Legacy two-phase stages (rejected by
    // verifyCpuMapping before any comparison) rank 0 as well, so rank checks
    // fail closed. Never compare stages numerically.
    inline unsigned cpuMappingStageRank(CpuMappingStage stage) noexcept
    {
        switch (stage)
        {
        case CpuMappingStage::GeneralNodes: return 1;
        case CpuMappingStage::GeneralSupernodes: return 2;
        case CpuMappingStage::LayoutNamedStores: return 3;
        case CpuMappingStage::EventBitmaps: return 4;
        case CpuMappingStage::MemWritePlan: return 5;
        case CpuMappingStage::GeneralFunctions: return 6;
        case CpuMappingStage::PhaseSchedule: return 7;
        case CpuMappingStage::TranslationUnits: return 8;
        default: return 0; // SplitPhases and the removed legacy stages
        }
    }
    inline bool cpuMappingStageAtLeast(CpuMappingStage stage, CpuMappingStage required) noexcept
    {
        return cpuMappingStageRank(stage) >= cpuMappingStageRank(required);
    }

    struct CpuPartitionAttrs
    {
        CpuPartitionKind kind = CpuPartitionKind::Root;
        CpuPhase phase = CpuPhase::None;
        // Ranges in the supernode's flattened operation order, not model OpIds.
        std::vector<Range> helperChunks;
        // Optional for old checkpoints. M3 six-phase pipeline: sorted unique
        // union of the supernode's event_acts cluster indices (engaged, with
        // an empty array for event-free supernodes, on every General-branch
        // supernode from the GeneralSupernodes stage on; disengaged elsewhere).
        std::optional<std::vector<int64_t>> eventActs;
        // M5d-6 (resolution 2): a General-branch EmitFunction is a leaf that
        // only records the contiguous supernode ordinal interval it holds
        // (supernodes stay direct General-branch children in C2 order).
        // Engaged exactly on the General branch's trailing EmitFunction
        // leaves from the GeneralFunctions stage on.
        std::optional<Range> supernodeRange;
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
    enum class CpuNamedStoreKind : uint8_t { RegLatch, Mem, Boundary, PrevEvent, EventAct, TimeslotTrigger, ActiveFlags };

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

    struct CpuStoreField
    {
        StringId name; // sanitized, unique across the whole store layout
        CpuTypeId type;
        uint64_t offset = 0; // byte offset inside the owning store struct
        StateId state;       // RegLatch/Mem entries; invalid otherwise
        ValueId value;       // Boundary/PrevEvent source value; invalid otherwise
        uint32_t aux = 0;    // (event,edge) cluster index / bit index / pack-word note
        friend bool operator==(const CpuStoreField &, const CpuStoreField &) = default;
    };

    struct CpuNamedStore
    {
        CpuNamedStoreKind kind = CpuNamedStoreKind::RegLatch;
        std::vector<CpuStoreField> fields;
        uint64_t sizeBytes = 0;
        friend bool operator==(const CpuNamedStore &, const CpuNamedStore &) = default;
    };

    struct CpuDataLayout
    {
        uint32_t pointerBytes = 8;
        std::vector<CpuType> types;
        // Six-phase named store layout (regLatchStore/memStore/
        // boundaryValueStore/prevEventStore/eventActStore/timeslotTriggerFlag/
        // activeFlags), filled by cpu.st.layout-named-stores (C3). The legacy
        // object/value/frame/runtime arenas were removed in M5d-6.
        std::optional<std::vector<CpuNamedStore>> namedStores;
        friend bool operator==(const CpuDataLayout &, const CpuDataLayout &) = default;
    };

    // ActivityDrivenCompute/DomainGatedCommit served the removed legacy
    // schedule (M5d-6); the six-phase tasks use AlwaysScanCommit (Event/Mem
    // branches), EventDataGated (General emit functions) and EvalEnd
    // (Output). Old enum values are kept stable for checkpoint compatibility.
    enum class CpuExecution : uint8_t { ActivityDrivenCompute, DomainGatedCommit, AlwaysScanCommit, EventDataGated, EvalEnd };

    struct CpuActivationTargets
    {
        std::vector<PartitionId> activate;
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

    struct CpuEventBitmap
    {
        uint32_t cluster = 0; // == the edgeDet op's act index
        std::vector<uint64_t> supernodeWords; // P_general supernode bitmap
        friend bool operator==(const CpuEventBitmap &, const CpuEventBitmap &) = default;
    };

    struct CpuMemReader
    {
        PartitionId owner;
        std::optional<uint64_t> staticRow; // engaged => exact static address match
        friend bool operator==(const CpuMemReader &, const CpuMemReader &) = default;
    };

    struct CpuMemWritePlanEntry
    {
        OpId writeOp;
        uint32_t priority = 0;
        bool eventFree = false;
        std::vector<CpuMemReader> readers;
        friend bool operator==(const CpuMemWritePlanEntry &, const CpuMemWritePlanEntry &) = default;
    };

    // M4 timeslot trigger mapping: firing event act cluster `act` sets
    // timeslot flag `flag` (from the Output-phase timeslot tasks'
    // timeslotFlag x event_acts Cartesian expansion).
    struct CpuTimeslotTrigger
    {
        uint32_t act = 0;
        uint32_t flag = 0;
        friend bool operator==(const CpuTimeslotTrigger &, const CpuTimeslotTrigger &) = default;
    };

    struct CpuSchedulePlan
    {
        std::vector<CpuNumaSchedule> numaNodes;
        std::vector<CpuFanoutEntry<ValueId>> inputFanout;
        std::vector<CpuFanoutEntry<ValueId>> computeSupernodeFanout;
        // Commit fanout covers every General-phase state read (regLatch-class
        // memReads included); the key set is no longer limited to the
        // output/event state dependency closure E.
        std::vector<CpuFanoutEntry<StateId>> commitStateFanout;
        // Six-phase static tables, filled by cpu.st.build-event-bitmaps (C4)
        // / cpu.st.build-mem-write-plan (C5) / cpu.st.build-phase-schedule
        // (C7): (event,edge) cluster -> P_general supernode bitmaps rebuilt by
        // P_event, the P_mem write plan (priority order, reader tables,
        // event-free writes), and the event act -> timeslot flag triggers
        // collected from the Output-phase timeslot tasks. The legacy
        // quiescence/round-seed/input-shadow/demonitor/fold-residue payloads
        // were removed in M5d-6.
        std::optional<std::vector<CpuEventBitmap>> eventBitmaps;
        std::optional<std::vector<CpuMemWritePlanEntry>> memWritePlan;
        std::optional<std::vector<CpuTimeslotTrigger>> timeslotTriggers;
        friend bool operator==(const CpuSchedulePlan &, const CpuSchedulePlan &) = default;
    };

    // ----- M5d-7 multi-TU emit plan (C8 cpu.st.plan-translation-units) -----

    // One emitted function (or one small fixed group) inside a generated
    // translation unit. Ranges index the kind's canonical stream:
    //   Core       — the fixed small core: eval, pInput, pPublish, the phase
    //                drivers, the system-task driver (exactly one chunk).
    //   Init       — init() chunk over the init stream: flattened init steps,
    //                then constant-boundary preloads, then prevEvent inits,
    //                then the regLatchStoreNext sync (offset/count in items).
    //   Event      — pEvent cone chunk: range of the Event branch's flat ops.
    //   GeneralScan— pGeneral scan chunk: supernode ordinal range, aligned to
    //                the C6 EmitFunction intervals unless one interval alone
    //                exceeds the chunk cap.
    //   Supernode  — one General supernode (offset = ordinal, count = 1):
    //                its sn_<ordinal> driver plus the C6 helperChunks member
    //                functions all live in this unit.
    //   Mem        — pMem chunk: range of the memWritePlan entries.
    //   Output     — pOutput chunk: range of the Output branch's flat ops.
    //   Dump       — dumpState chunk: range of the canonical dump item list
    //                (inputs, outputs, then the named-store fields in store
    //                order: regLatch, mem, boundary, prevEvent, eventAct,
    //                timeslot, activeFlags).
    enum class CpuEmitChunkKind : uint8_t { Core, Init, Event, GeneralScan, Supernode, Mem, Output, Dump };

    struct CpuEmitChunk
    {
        CpuEmitChunkKind kind = CpuEmitChunkKind::Core;
        uint32_t offset = 0;          // range begin in the kind's stream (Supernode: ordinal)
        uint32_t count = 0;           // range length (Supernode: 1, Core: 0)
        uint64_t estimatedLines = 0;  // C8 size heuristic
        friend bool operator==(const CpuEmitChunk &, const CpuEmitChunk &) = default;
    };

    struct CpuTranslationUnit
    {
        std::string name; // file base name suffix; emit writes <prefix>_<name>.cpp
        std::vector<CpuEmitChunk> chunks;
        uint64_t estimatedLines = 0;
        friend bool operator==(const CpuTranslationUnit &, const CpuTranslationUnit &) = default;
    };

    struct CpuTranslationUnitPlan
    {
        // Caps recorded so verification replans deterministically.
        uint64_t chunkMaxEstimatedLines = 0;
        uint64_t unitMaxEstimatedLines = 0;
        std::vector<CpuTranslationUnit> units;
        friend bool operator==(const CpuTranslationUnitPlan &, const CpuTranslationUnitPlan &) = default;
    };

    struct CpuBackendMapping
    {
        // The six-phase pipeline enters at GeneralNodes (M5d-6: C1
        // cpu.st.build-general-nodes initializes the mapping) and terminates
        // at TranslationUnits (M5d-7: C8 records the emit TU plan).
        CpuMappingStage stage = CpuMappingStage::GeneralNodes;
        CpuPartitionTree partitionTree;
        std::optional<CpuDataLayout> dataLayout;
        std::optional<CpuSchedulePlan> schedule;
        // Engaged exactly at the TranslationUnits stage (C8): the emit-side
        // translation-unit plan consumed by cpu.st.emit-cpp.
        std::optional<CpuTranslationUnitPlan> translationUnits;
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

    // Read-only provenance annotation carried over from the GRH graph: one group
    // per generate-scope declaration, holding the names of its per-round
    // elaboration copies in round order. Pure metadata; see the maintenance
    // contract on GrhSimModel::declaredSymbols.
    struct GenerateGroup
    {
        StringId scope;                // scope path text, '$'-joined (flattened: instance prefix included)
        StringId name;                 // bare declaration name
        std::vector<StringId> symbols; // per-round copy symbols, index = generate-for round
    };

    // How one slice of a declaration is realized by the current model.
    enum class DeclProvenanceKind : uint8_t
    {
        Direct, // the target slice holds the declaration range itself
        Alias,  // the declaration was folded/aliased; the target holds an equivalent value
        Merged  // the declaration is packed into the target together with other declarations
    };

    enum class DeclProvenanceTarget : uint8_t
    {
        Value,
        State,
        Function
    };

    // Bit offsets are linear: for an array target (or array declaration) the
    // address of element (i, j, ...) is (flatElementIndex * elementWidth +
    // bitInElement), with the shape flattened row-major (outermost first).
    // width == 0 marks a whole-object association and is the only valid form
    // for non-logic targets (real/string values, DPI functions); both offsets
    // must be zero then.
    struct DeclProvenanceSlice
    {
        DeclProvenanceKind kind = DeclProvenanceKind::Direct;
        DeclProvenanceTarget target = DeclProvenanceTarget::Value;
        uint32_t targetIndex = 0;  // ValueId/StateId/FuncId index (generation is always 0)
        uint64_t targetOffset = 0; // linear bit offset within the target
        uint64_t declOffset = 0;   // linear bit offset within the declaration
        uint64_t width = 0;        // bits covered; 0 = whole object
        friend bool operator==(const DeclProvenanceSlice &, const DeclProvenanceSlice &) = default;
    };

    // Maintainable association between one source declaration and the model
    // entities that currently realize it. One record per declared symbol;
    // `shape`/`width` describe the declaration itself (shape empty = scalar),
    // `slices` the current realization: a folded alias redirects the slice to
    // the surviving value, a merge points multiple records at slices of one
    // shared target, a split gives one record several slices with disjoint
    // declOffset ranges. A record with no slices names a declaration that is
    // currently not realized (optimized away); the name list entry in
    // declaredSymbols still anchors it. Generate-group membership joins by
    // symbol name against GenerateGroup::symbols.
    struct DeclProvenance
    {
        StringId symbol;             // member of GrhSimModel::declaredSymbols
        OriginId origin;             // declaration site; may be invalid
        uint64_t width = 0;          // scalar / array-element bit width; 0 for non-logic
        std::vector<uint64_t> shape; // array dimensions, outermost first; empty = scalar
        std::vector<DeclProvenanceSlice> slices;
        friend bool operator==(const DeclProvenance &, const DeclProvenance &) = default;
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
        std::size_t declaredSymbols = 0;
        std::size_t generateGroups = 0;
        std::size_t declProvenances = 0;
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
        // Sets the op's six-phase attribution; revision commits stay with the caller.
        void setOperationPhase(OpId id, SimPhase phase);
        // Sets the state's store classification (see StateStoreClass); revision
        // commits stay with the caller. Passes that create or rebuild states in
        // an already-classified model (hasStateStoreClassification) must
        // classify them incrementally to keep the classification total.
        void setStateStoreClass(StateId id, StateStoreClass storeClass);
        bool hasStateStoreClassification() const noexcept;
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

        // Read-only provenance metadata: source-level declared symbol names and
        // generate-scope copy groups, filled by the GRH lowering. These are pure
        // name (group) lists — no entity IDs, no semantic constraint. Passes may
        // read them but have no maintenance obligation: compact() and passes
        // renumber or rewrite entities freely, and after such rewrites an anchor
        // name may no longer resolve to a live value, which is expected.
        // Editing these lists is a metadata mutation (commitMetadataMutation,
        // never semantic); like the entity builders, the add* methods leave the
        // revision commit to the caller.
        void addDeclaredSymbol(StringId symbol);
        bool isDeclaredSymbol(StringId symbol) const noexcept;
        const std::vector<StringId> &declaredSymbols() const noexcept { return declaredSymbols_; }
        std::size_t addGenerateGroup(StringId scope, StringId name);
        void addGenerateGroupSymbol(std::size_t group, StringId symbol);
        const std::vector<GenerateGroup> &generateGroups() const noexcept { return generateGroups_; }

        // Declaration provenance: the maintainable association from a declared
        // symbol to the values/states/functions that currently realize it (see
        // DeclProvenance). Unlike the pure name lists above, slices hold entity
        // indices, so structural mutations must keep them valid:
        // - the GRH lowering populates one Direct full-range slice per resolved
        //   declaration;
        // - a pass that folds/aliases, merges or splits a declared entity
        //   redirects or re-slices the record via upsertDeclProvenance before
        //   the old entity disappears;
        // - compact() remaps slice targets to the renumbered entities and drops
        //   slices whose target was removed, leaving a possibly empty record;
        // - edits are metadata mutations (commitMetadataMutation, never
        //   semantic); compact() performs its remap inside the rebuild.
        // upsertDeclProvenance requires the symbol to be a declared member and
        // replaces any existing record for it.
        void upsertDeclProvenance(DeclProvenance record);
        const DeclProvenance *findDeclProvenance(StringId symbol) const noexcept;
        const std::vector<DeclProvenance> &declProvenances() const noexcept { return declProvenances_; }

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
        std::vector<StringId> declaredSymbols_;
        std::unordered_set<uint32_t> declaredSymbolSet_;
        std::vector<GenerateGroup> generateGroups_;
        std::vector<DeclProvenance> declProvenances_;
        std::unordered_map<uint32_t, std::size_t> declProvenanceBySymbol_;
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
    std::string_view toString(SimPhase phase) noexcept;
    std::optional<SimPhase> parseSimPhase(std::string_view text) noexcept;
    std::string_view toString(StateStoreClass storeClass) noexcept;
    std::optional<StateStoreClass> parseStateStoreClass(std::string_view text) noexcept;
    std::string_view toString(CpuNamedStoreKind kind) noexcept;
    std::optional<CpuNamedStoreKind> parseCpuNamedStoreKind(std::string_view text) noexcept;
    std::string_view toString(DeclProvenanceKind kind) noexcept;
    std::optional<DeclProvenanceKind> parseDeclProvenanceKind(std::string_view text) noexcept;
    std::string_view toString(DeclProvenanceTarget target) noexcept;
    std::optional<DeclProvenanceTarget> parseDeclProvenanceTarget(std::string_view text) noexcept;

} // namespace wolvrix::lib::grhsim

#endif // WOLVRIX_GRHSIM_IR_MODEL_HPP
