#include "grhsim/ir/model.hpp"

#include <atomic>
#include <chrono>
#include <limits>
#include <stdexcept>

namespace wolvrix::lib::grhsim
{

    namespace
    {
        template <typename T>
        std::span<const T> checkedSpan(const std::vector<T> &pool, Range range,
                                       std::string_view context)
        {
            const std::size_t offset = range.offset;
            const std::size_t count = range.count;
            if (offset > pool.size() || count > pool.size() - offset)
            {
                throw std::out_of_range(std::string(context) + " range is out of bounds");
            }
            return std::span<const T>(pool.data() + offset, count);
        }

        template <typename IdType>
        IdType nextId(std::size_t size, std::string_view context)
        {
            if (size >= std::numeric_limits<uint32_t>::max())
            {
                throw std::overflow_error(std::string(context) + " exceeds 32-bit ID capacity");
            }
            return IdType{static_cast<uint32_t>(size + 1), 0};
        }
    } // namespace

    std::size_t StringInterner::Hash::operator()(std::string_view value) const noexcept
    {
        return std::hash<std::string_view>{}(value);
    }

    std::size_t StringInterner::Hash::operator()(const std::string &value) const noexcept
    {
        return operator()(std::string_view(value));
    }

    bool StringInterner::Equal::operator()(std::string_view lhs, std::string_view rhs) const noexcept
    {
        return lhs == rhs;
    }

    StringId StringInterner::intern(std::string_view text)
    {
        if (text.empty())
        {
            return {};
        }
        if (auto it = idByText_.find(text); it != idByText_.end())
        {
            return it->second;
        }
        const StringId id = nextId<StringId>(textById_.size(), "string table");
        textById_.emplace_back(text);
        idByText_.emplace(textById_.back(), id);
        return id;
    }

    StringId StringInterner::lookup(std::string_view text) const noexcept
    {
        if (text.empty())
        {
            return {};
        }
        auto it = idByText_.find(text);
        return it == idByText_.end() ? StringId{} : it->second;
    }

    std::string_view StringInterner::text(StringId id) const
    {
        if (!valid(id))
        {
            if (!id.valid())
            {
                return {};
            }
            throw std::out_of_range("StringId is out of range");
        }
        return textById_[id.index - 1];
    }

    bool StringInterner::valid(StringId id) const noexcept
    {
        return id.generation == 0 && id.index != 0 && id.index <= textById_.size();
    }

    void StringInterner::reserve(std::size_t count)
    {
        idByText_.reserve(count);
    }

    ObjectRef ObjectRef::input(InputId id) noexcept
    {
        return ObjectRef{ObjectKind::Input, id.index, id.generation};
    }

    ObjectRef ObjectRef::output(OutputId id) noexcept
    {
        return ObjectRef{ObjectKind::Output, id.index, id.generation};
    }

    ObjectRef ObjectRef::state(StateId id) noexcept
    {
        return ObjectRef{ObjectKind::State, id.index, id.generation};
    }

    ObjectRef ObjectRef::function(FuncId id) noexcept
    {
        return ObjectRef{ObjectKind::Function, id.index, id.generation};
    }

    ModelIdentity GrhSimModel::nextIdentity() noexcept
    {
        static std::atomic<uint64_t> sequence{1};
        const uint64_t low = sequence.fetch_add(1, std::memory_order_relaxed);
        const uint64_t now = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const uint64_t addressSalt = reinterpret_cast<uintptr_t>(&sequence);
        return ModelIdentity{now ^ (addressSalt << 1), low};
    }

    std::size_t GrhSimModel::ArrayTypeKeyHash::operator()(ArrayTypeKey key) const noexcept
    {
        std::size_t seed = std::hash<uint64_t>{}(key.count);
        seed ^= std::hash<uint32_t>{}(key.element) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
        return seed;
    }

    GrhSimModel::GrhSimModel(std::string_view name)
        : identity_(nextIdentity())
    {
        if (!name.empty())
        {
            name_ = intern(name);
        }
    }

    GrhSimModel GrhSimModel::clone() const
    {
        GrhSimModel result;
        result.strings_ = strings_;
        result.name_ = name_;
        result.dialects_ = dialects_;
        result.types_ = types_;
        result.logicTypes_ = logicTypes_;
        result.arrayTypes_ = arrayTypes_;
        result.realType_ = realType_;
        result.stringType_ = stringType_;
        result.interfacePorts_ = interfacePorts_;
        result.inputs_ = inputs_;
        result.outputs_ = outputs_;
        result.states_ = states_;
        result.functions_ = functions_;
        result.functionArguments_ = functionArguments_;
        result.values_ = values_;
        result.operations_ = operations_;
        result.operandPool_ = operandPool_;
        result.resultPool_ = resultPool_;
        result.objectRefPool_ = objectRefPool_;
        result.parameterPool_ = parameterPool_;
        result.initRecords_ = initRecords_;
        result.initSteps_ = initSteps_;
        result.initParameterPool_ = initParameterPool_;
        result.mappings_ = mappings_;
        result.mappingParameterPool_ = mappingParameterPool_;
        result.origins_ = origins_;
        result.declaredSymbols_ = declaredSymbols_;
        result.declaredSymbolSet_ = declaredSymbolSet_;
        result.generateGroups_ = generateGroups_;
        result.poisoned_ = poisoned_;
        for (auto &mapping : result.mappings_)
        {
            mapping.sourceIdentity = result.identity_;
            mapping.sourceSemanticRevision = result.semanticRevision_;
        }
        return result;
    }

    void GrhSimModel::reserve(const ModelReserve &counts)
    {
        strings_.reserve(counts.strings);
        dialects_.reserve(counts.dialects);
        types_.reserve(counts.types);
        logicTypes_.reserve(counts.types);
        arrayTypes_.reserve(counts.types);
        inputs_.reserve(counts.inputs);
        outputs_.reserve(counts.outputs);
        states_.reserve(counts.states);
        functions_.reserve(counts.functions);
        functionArguments_.reserve(counts.functionArguments);
        interfacePorts_.reserve(counts.interfacePorts);
        values_.reserve(counts.values);
        operations_.reserve(counts.operations);
        operandPool_.reserve(counts.operands);
        resultPool_.reserve(counts.results);
        objectRefPool_.reserve(counts.objectRefs);
        parameterPool_.reserve(counts.parameters);
        initRecords_.reserve(counts.initRecords);
        initSteps_.reserve(counts.initSteps);
        initParameterPool_.reserve(counts.initParameters);
        mappings_.reserve(counts.mappings);
        mappingParameterPool_.reserve(counts.mappingParameters);
        origins_.reserve(counts.origins);
        declaredSymbols_.reserve(counts.declaredSymbols);
        declaredSymbolSet_.reserve(counts.declaredSymbols);
        generateGroups_.reserve(counts.generateGroups);
    }

    void GrhSimModel::commitSemanticMutation()
    {
        ++semanticRevision_;
        mappings_.clear();
        mappingParameterPool_.clear();
    }

    void GrhSimModel::addDialect(std::string_view name, std::string_view version,
                                 std::string_view schemaFingerprint)
    {
        const StringId nameId = intern(name);
        for (const DialectUse &dialect : dialects_)
        {
            if (dialect.name == nameId)
            {
                if (text(dialect.version) != version ||
                    text(dialect.schemaFingerprint) != schemaFingerprint)
                {
                    throw std::invalid_argument("dialect manifest contains conflicting entries for " +
                                                std::string(name));
                }
                return;
            }
        }
        dialects_.push_back(DialectUse{nameId, intern(version), intern(schemaFingerprint)});
    }

    TypeId GrhSimModel::logicType(uint32_t width, bool isSigned, LogicDomain domain)
    {
        const uint64_t key = (static_cast<uint64_t>(width) << 2) |
                             (static_cast<uint64_t>(isSigned) << 1) |
                             static_cast<uint64_t>(domain == LogicDomain::FourState);
        if (auto it = logicTypes_.find(key); it != logicTypes_.end()) return it->second;
        Type type;
        type.id = nextId<TypeId>(types_.size(), "type table");
        type.typeRef = intern("core.logic");
        type.kind = TypeKind::Logic;
        type.width = width;
        type.isSigned = isSigned;
        type.domain = domain;
        types_.push_back(type);
        logicTypes_.emplace(key, type.id);
        return type.id;
    }

    TypeId GrhSimModel::realType()
    {
        if (realType_.valid()) return realType_;
        Type type;
        type.id = nextId<TypeId>(types_.size(), "type table");
        type.typeRef = intern("core.real");
        type.kind = TypeKind::Real;
        types_.push_back(type);
        realType_ = type.id;
        return realType_;
    }

    TypeId GrhSimModel::stringType()
    {
        if (stringType_.valid()) return stringType_;
        Type type;
        type.id = nextId<TypeId>(types_.size(), "type table");
        type.typeRef = intern("core.string");
        type.kind = TypeKind::String;
        types_.push_back(type);
        stringType_ = type.id;
        return stringType_;
    }

    TypeId GrhSimModel::arrayType(TypeId elementType, uint64_t count)
    {
        const ArrayTypeKey key{elementType.index, count};
        if (auto it = arrayTypes_.find(key); it != arrayTypes_.end()) return it->second;
        Type type;
        type.id = nextId<TypeId>(types_.size(), "type table");
        type.typeRef = intern("core.array");
        type.kind = TypeKind::Array;
        type.elementType = elementType;
        type.count = count;
        types_.push_back(type);
        arrayTypes_.emplace(key, type.id);
        return type.id;
    }

    OriginId GrhSimModel::addOrigin(Origin origin)
    {
        origin.id = nextId<OriginId>(origins_.size(), "origin table");
        origins_.push_back(origin);
        return origin.id;
    }

    InputId GrhSimModel::addInput(std::string_view name, TypeId type, OriginId origin)
    {
        const InputId id = nextId<InputId>(inputs_.size(), "input table");
        inputs_.push_back(InputObject{id, intern(name), type, origin});
        return id;
    }

    OutputId GrhSimModel::addOutput(std::string_view name, TypeId type, OriginId origin)
    {
        const OutputId id = nextId<OutputId>(outputs_.size(), "output table");
        outputs_.push_back(OutputObject{id, intern(name), type, origin});
        return id;
    }

    StateId GrhSimModel::addState(std::string_view name, TypeId type, OriginId origin)
    {
        const StateId id = nextId<StateId>(states_.size(), "state table");
        states_.push_back(StateObject{id, intern(name), type, origin});
        return id;
    }

    FuncId GrhSimModel::addExternFunction(std::string_view name, std::string_view declRef,
                                          std::string_view symbol,
                                          std::span<const DpiArgument> arguments,
                                          TypeId returnType, OriginId origin)
    {
        const FuncId id = nextId<FuncId>(functions_.size(), "function table");
        const Range argumentRange = appendRange(functionArguments_, arguments);
        functions_.push_back(ExternFunction{id, intern(name), intern(declRef), intern(symbol),
                                            argumentRange, returnType, origin});
        return id;
    }

    void GrhSimModel::addInterfacePort(InterfacePort port)
    {
        interfacePorts_.push_back(port);
    }

    ValueId GrhSimModel::addValue(TypeId type, std::string_view name, OriginId origin)
    {
        const ValueId id = nextId<ValueId>(values_.size(), "value table");
        values_.push_back(SimValue{id, type, intern(name), origin});
        return id;
    }

    OpId GrhSimModel::addOperation(std::string_view opType,
                                   std::span<const ValueId> operands,
                                   std::span<const ValueId> results,
                                   std::span<const ObjectRef> objectRefs,
                                   std::span<const Parameter> parameters,
                                   std::string_view name, OriginId origin)
    {
        const OpId id = nextId<OpId>(operations_.size(), "operation table");
        operations_.push_back(SimOp{id, intern(opType), intern(name),
                                    appendRange(operandPool_, operands),
                                    appendRange(resultPool_, results),
                                    appendRange(objectRefPool_, objectRefs),
                                    appendRange(parameterPool_, parameters), origin});
        return id;
    }

    void GrhSimModel::setOperationPhase(OpId id, SimPhase phase)
    {
        if (id.generation != 0 || !id.index || id.index > operations_.size())
            throw std::out_of_range("setOperationPhase ID is invalid");
        operations_[id.index - 1].phase = phase;
    }

    void GrhSimModel::addInit(StateId state, std::span<const InitStep> steps,
                              std::span<const Parameter> stepParameters)
    {
        const uint32_t parameterBase = static_cast<uint32_t>(initParameterPool_.size());
        appendRange(initParameterPool_, stepParameters);
        const uint32_t stepBase = static_cast<uint32_t>(initSteps_.size());
        for (InitStep step : steps)
        {
            if (step.parameters.offset > stepParameters.size() ||
                step.parameters.count > stepParameters.size() - step.parameters.offset)
            {
                throw std::out_of_range("init step parameter range is out of bounds");
            }
            step.parameters.offset += parameterBase;
            initSteps_.push_back(step);
        }
        initRecords_.push_back(InitRecord{
            state, Range{stepBase, static_cast<uint32_t>(steps.size())}});
    }

    void GrhSimModel::replaceOperation(OpId id, std::string_view opType,
                                       std::span<const ValueId> operands,
                                       std::span<const ValueId> results,
                                       std::span<const ObjectRef> objectRefs,
                                       std::span<const Parameter> parameters)
    {
        if (id.generation != 0 || !id.index || id.index > operations_.size())
            throw std::out_of_range("replaceOperation ID is invalid");
        SimOp replacement = operations_[id.index - 1];
        replacement.opType = intern(opType);
        replacement.operands = appendRange(operandPool_, operands);
        replacement.results = appendRange(resultPool_, results);
        replacement.objectRefs = appendRange(objectRefPool_, objectRefs);
        replacement.parameters = appendRange(parameterPool_, parameters);
        operations_[id.index - 1] = replacement;
    }

    void GrhSimModel::compact(std::span<const uint8_t> removeOps,
                              std::span<const uint8_t> removeStates)
    {
        if (removeOps.size() != operations_.size() + 1 || removeStates.size() != states_.size() + 1)
            throw std::invalid_argument("compact masks must include slot zero and every entity");
        std::vector<ValueId> valueMap(values_.size() + 1);
        std::vector<StateId> stateMap(states_.size() + 1);
        std::vector<uint8_t> liveValues(values_.size() + 1);
        for (const auto &op : operations_)
            if (!removeOps[op.id.index])
                for (auto value : results(op)) liveValues.at(value.index) = 1;
        std::vector<SimValue> newValues;
        for (auto value : values_)
        {
            if (!liveValues[value.id.index]) continue;
            const auto old = value.id;
            value.id = nextId<ValueId>(newValues.size(), "compact values");
            valueMap[old.index] = value.id;
            newValues.push_back(value);
        }
        std::vector<StateObject> newStates;
        for (auto state : states_)
        {
            if (removeStates[state.id.index]) continue;
            const auto old = state.id;
            state.id = nextId<StateId>(newStates.size(), "compact states");
            stateMap[old.index] = state.id;
            newStates.push_back(state);
        }
        std::vector<SimOp> newOps;
        std::vector<ValueId> newOperands, newResults;
        std::vector<ObjectRef> newRefs;
        std::vector<Parameter> newParameters;
        const auto remapValues = [&](std::span<const ValueId> source, std::vector<ValueId> &pool) {
            Range range{static_cast<uint32_t>(pool.size()), static_cast<uint32_t>(source.size())};
            for (auto value : source)
            {
                if (value.generation != 0 || !valueMap.at(value.index).valid())
                    throw std::invalid_argument("compact would remove a retained value's producer");
                pool.push_back(valueMap[value.index]);
            }
            return range;
        };
        for (auto op : operations_)
        {
            if (removeOps[op.id.index]) continue;
            const auto refs = objectRefs(op);
            const auto params = parameters(op);
            op.operands = remapValues(operands(op), newOperands);
            op.results = remapValues(results(op), newResults);
            op.objectRefs = {static_cast<uint32_t>(newRefs.size()), static_cast<uint32_t>(refs.size())};
            for (auto ref : refs)
            {
                if (ref.kind == ObjectKind::State)
                {
                    if (ref.generation != 0 || !stateMap.at(ref.index).valid())
                        throw std::invalid_argument("compact would remove a referenced state");
                    ref = ObjectRef::state(stateMap[ref.index]);
                }
                newRefs.push_back(ref);
            }
            op.parameters = appendRange(newParameters, params);
            op.id = nextId<OpId>(newOps.size(), "compact operations");
            newOps.push_back(op);
        }
        std::vector<InitRecord> newInits;
        std::vector<InitStep> newSteps;
        std::vector<Parameter> newInitParameters;
        for (const auto &record : initRecords_)
        {
            if (removeStates[record.state.index]) continue;
            const auto source = steps(record);
            Range range{static_cast<uint32_t>(newSteps.size()), static_cast<uint32_t>(source.size())};
            for (auto step : source)
            {
                step.parameters = appendRange(newInitParameters, parameters(step));
                newSteps.push_back(step);
            }
            newInits.push_back({stateMap[record.state.index], range});
        }
        values_.swap(newValues); states_.swap(newStates); operations_.swap(newOps);
        operandPool_.swap(newOperands); resultPool_.swap(newResults);
        objectRefPool_.swap(newRefs); parameterPool_.swap(newParameters);
        initRecords_.swap(newInits); initSteps_.swap(newSteps); initParameterPool_.swap(newInitParameters);
        mappings_.clear(); mappingParameterPool_.clear();
    }

    void GrhSimModel::addMapping(std::string_view backend, std::string_view schema,
                                 bool complete, std::span<const Parameter> parameters)
    {
        mappings_.push_back(BackendMapping{
            intern(backend), intern(schema), complete,
            appendRange(mappingParameterPool_, parameters), identity_, semanticRevision_});
    }

    const CpuBackendMapping *GrhSimModel::cpuMapping() const noexcept
    {
        for (const auto &mapping : mappings_)
            if (mapping.cpu) return &*mapping.cpu;
        return nullptr;
    }

    void GrhSimModel::setCpuMapping(CpuBackendMapping cpu)
    {
        const auto backend = intern("cpu");
        const auto schema = intern("cpu.st.v1");
        for (auto &mapping : mappings_)
        {
            if (mapping.backend != backend) continue;
            mapping.schema = schema;
            mapping.complete = cpu.stage == CpuMappingStage::Schedule;
            mapping.sourceIdentity = identity_;
            mapping.sourceSemanticRevision = semanticRevision_;
            mapping.cpu = std::move(cpu);
            return;
        }
        addMapping("cpu", "cpu.st.v1", cpu.stage == CpuMappingStage::Schedule);
        mappings_.back().cpu = std::move(cpu);
    }

    void GrhSimModel::addDeclaredSymbol(StringId symbol)
    {
        if (!strings_.valid(symbol))
            throw std::out_of_range("declared symbol is not a valid model string");
        if (declaredSymbolSet_.insert(symbol.index).second)
            declaredSymbols_.push_back(symbol);
    }

    bool GrhSimModel::isDeclaredSymbol(StringId symbol) const noexcept
    {
        return symbol.valid() && declaredSymbolSet_.contains(symbol.index);
    }

    std::size_t GrhSimModel::addGenerateGroup(StringId scope, StringId name)
    {
        if (!strings_.valid(scope) || !strings_.valid(name))
            throw std::out_of_range("generate group scope/name is not a valid model string");
        generateGroups_.push_back(GenerateGroup{scope, name, {}});
        return generateGroups_.size() - 1;
    }

    void GrhSimModel::addGenerateGroupSymbol(std::size_t group, StringId symbol)
    {
        if (group >= generateGroups_.size())
            throw std::out_of_range("generate group index is out of range");
        if (!strings_.valid(symbol))
            throw std::out_of_range("generate group symbol is not a valid model string");
        generateGroups_[group].symbols.push_back(symbol);
    }

    std::span<const ValueId> GrhSimModel::operands(const SimOp &op) const
    {
        return checkedSpan(operandPool_, op.operands, "operation operand");
    }

    std::span<const ValueId> GrhSimModel::results(const SimOp &op) const
    {
        return checkedSpan(resultPool_, op.results, "operation result");
    }

    std::span<const ObjectRef> GrhSimModel::objectRefs(const SimOp &op) const
    {
        return checkedSpan(objectRefPool_, op.objectRefs, "operation object reference");
    }

    std::span<const Parameter> GrhSimModel::parameters(const SimOp &op) const
    {
        return checkedSpan(parameterPool_, op.parameters, "operation parameter");
    }

    std::span<const DpiArgument> GrhSimModel::arguments(const ExternFunction &function) const
    {
        return checkedSpan(functionArguments_, function.arguments, "function argument");
    }

    std::span<const InitStep> GrhSimModel::steps(const InitRecord &record) const
    {
        return checkedSpan(initSteps_, record.steps, "init step");
    }

    std::span<const Parameter> GrhSimModel::parameters(const InitStep &step) const
    {
        return checkedSpan(initParameterPool_, step.parameters, "init parameter");
    }

    std::span<const Parameter> GrhSimModel::parameters(const BackendMapping &mapping) const
    {
        return checkedSpan(mappingParameterPool_, mapping.parameters, "mapping parameter");
    }

    template <typename T>
    Range GrhSimModel::appendRange(std::vector<T> &pool, std::span<const T> values)
    {
        if (pool.size() > std::numeric_limits<uint32_t>::max() ||
            values.size() > std::numeric_limits<uint32_t>::max() - pool.size())
        {
            throw std::overflow_error("GrhSIM flat pool exceeds 32-bit range capacity");
        }
        const Range range{static_cast<uint32_t>(pool.size()), static_cast<uint32_t>(values.size())};
        pool.insert(pool.end(), values.begin(), values.end());
        return range;
    }

    std::string_view toString(LogicDomain domain) noexcept
    {
        return domain == LogicDomain::TwoState ? "2-state" : "4-state";
    }

    std::optional<LogicDomain> parseLogicDomain(std::string_view text) noexcept
    {
        if (text == "2-state") return LogicDomain::TwoState;
        if (text == "4-state") return LogicDomain::FourState;
        return std::nullopt;
    }

    std::string_view toString(TypeKind kind) noexcept
    {
        switch (kind)
        {
        case TypeKind::Logic: return "logic";
        case TypeKind::Real: return "real";
        case TypeKind::String: return "string";
        case TypeKind::Array: return "array";
        }
        return "unknown";
    }

    std::optional<TypeKind> parseTypeKind(std::string_view text) noexcept
    {
        if (text == "logic") return TypeKind::Logic;
        if (text == "real") return TypeKind::Real;
        if (text == "string") return TypeKind::String;
        if (text == "array") return TypeKind::Array;
        return std::nullopt;
    }

    std::string_view toString(ObjectKind kind) noexcept
    {
        switch (kind)
        {
        case ObjectKind::Input: return "input";
        case ObjectKind::Output: return "output";
        case ObjectKind::State: return "state";
        case ObjectKind::Function: return "function";
        }
        return "unknown";
    }

    std::optional<ObjectKind> parseObjectKind(std::string_view text) noexcept
    {
        if (text == "input") return ObjectKind::Input;
        if (text == "output") return ObjectKind::Output;
        if (text == "state") return ObjectKind::State;
        if (text == "function") return ObjectKind::Function;
        return std::nullopt;
    }

    std::string_view toString(InterfaceDirection direction) noexcept
    {
        switch (direction)
        {
        case InterfaceDirection::Input: return "input";
        case InterfaceDirection::Output: return "output";
        case InterfaceDirection::Inout: return "inout";
        }
        return "unknown";
    }

    std::optional<InterfaceDirection> parseInterfaceDirection(std::string_view text) noexcept
    {
        if (text == "input") return InterfaceDirection::Input;
        if (text == "output") return InterfaceDirection::Output;
        if (text == "inout") return InterfaceDirection::Inout;
        return std::nullopt;
    }

    std::string_view toString(DpiDirection direction) noexcept
    {
        switch (direction)
        {
        case DpiDirection::Input: return "input";
        case DpiDirection::Output: return "output";
        case DpiDirection::Inout: return "inout";
        }
        return "unknown";
    }

    std::optional<DpiDirection> parseDpiDirection(std::string_view text) noexcept
    {
        if (text == "input") return DpiDirection::Input;
        if (text == "output") return DpiDirection::Output;
        if (text == "inout") return DpiDirection::Inout;
        return std::nullopt;
    }

    std::string_view toString(SimPhase phase) noexcept
    {
        switch (phase)
        {
        case SimPhase::None: return "none";
        case SimPhase::Event: return "event";
        case SimPhase::General: return "general";
        case SimPhase::Mem: return "mem";
        case SimPhase::Output: return "output";
        }
        return "unknown";
    }

    std::optional<SimPhase> parseSimPhase(std::string_view text) noexcept
    {
        if (text == "none") return SimPhase::None;
        if (text == "event") return SimPhase::Event;
        if (text == "general") return SimPhase::General;
        if (text == "mem") return SimPhase::Mem;
        if (text == "output") return SimPhase::Output;
        return std::nullopt;
    }

    std::string_view toString(CpuNamedStoreKind kind) noexcept
    {
        switch (kind)
        {
        case CpuNamedStoreKind::RegLatch: return "regLatch";
        case CpuNamedStoreKind::Mem: return "mem";
        case CpuNamedStoreKind::Boundary: return "boundary";
        case CpuNamedStoreKind::PrevEvent: return "prevEvent";
        case CpuNamedStoreKind::EventAct: return "eventAct";
        case CpuNamedStoreKind::TimeslotTrigger: return "timeslotTrigger";
        }
        return "unknown";
    }

    std::optional<CpuNamedStoreKind> parseCpuNamedStoreKind(std::string_view text) noexcept
    {
        if (text == "regLatch") return CpuNamedStoreKind::RegLatch;
        if (text == "mem") return CpuNamedStoreKind::Mem;
        if (text == "boundary") return CpuNamedStoreKind::Boundary;
        if (text == "prevEvent") return CpuNamedStoreKind::PrevEvent;
        if (text == "eventAct") return CpuNamedStoreKind::EventAct;
        if (text == "timeslotTrigger") return CpuNamedStoreKind::TimeslotTrigger;
        return std::nullopt;
    }

} // namespace wolvrix::lib::grhsim
