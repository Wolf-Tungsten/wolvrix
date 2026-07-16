#include "core/grh.hpp"
#include "core/transform.hpp"
#include "transform/activity_schedule.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace wolvrix::lib::transform;

namespace
{

    int fail(const std::string &message)
    {
        std::cerr << "[activity-schedule-tests] " << message << '\n';
        return 1;
    }

    template <typename T>
    const T *getSessionValue(const SessionStore &session, std::string_view key)
    {
        auto it = session.find(std::string(key));
        if (it == session.end())
        {
            return nullptr;
        }
        auto *typed = dynamic_cast<const SessionSlotValue<T> *>(it->second.get());
        return typed == nullptr ? nullptr : &typed->value;
    }

    wolvrix::lib::grh::ValueId makeValue(wolvrix::lib::grh::Graph &graph,
                                         const std::string &name,
                                         int32_t width,
                                         bool isSigned = false)
    {
        return graph.createValue(graph.internSymbol(name), width, isSigned);
    }

    wolvrix::lib::grh::ValueId makeConstant(wolvrix::lib::grh::Graph &graph,
                                            const std::string &opName,
                                            const std::string &valueName,
                                            int32_t width,
                                            std::string literal)
    {
        const auto value = makeValue(graph, valueName, width);
        const auto op = graph.createOperation(wolvrix::lib::grh::OperationKind::kConstant,
                                              graph.internSymbol(opName));
        graph.addResult(op, value);
        graph.setAttr(op, "constValue", std::move(literal));
        return value;
    }

    bool isCommitPhaseOp(const wolvrix::lib::grh::Operation &op)
    {
        using wolvrix::lib::grh::OperationKind;
        switch (op.kind())
        {
        case OperationKind::kRegisterWritePort:
        case OperationKind::kLatchWritePort:
        case OperationKind::kMemoryWritePort:
        case OperationKind::kMemoryFillPort:
            return true;
        default:
            return false;
        }
    }

    struct ScheduleView
    {
        const ActivityScheduleSupernodeToOps *supernodeToOps = nullptr;
        const ActivityScheduleOpToSupernode *opToSupernode = nullptr;
        const ActivityScheduleDag *dag = nullptr;
        const ActivityScheduleValueFanout *valueFanout = nullptr;
        const ActivityScheduleTopoOrder *topoOrder = nullptr;
        const ActivityScheduleStateReadSupernodes *stateReadSupernodes = nullptr;
        const ActivityScheduleSupernodeKinds *supernodeKinds = nullptr;
        const ActivityScheduleComputeNodesBySupernode *computeNodesBySupernode = nullptr;
        const std::string *summaryStats = nullptr;
    };

    ScheduleView loadSchedule(const SessionStore &session, const std::string &graphName)
    {
        const std::string prefix = graphName + ".activity_schedule.";
        return ScheduleView{
            getSessionValue<ActivityScheduleSupernodeToOps>(session, prefix + "supernode_to_ops"),
            getSessionValue<ActivityScheduleOpToSupernode>(session, prefix + "op_to_supernode"),
            getSessionValue<ActivityScheduleDag>(session, prefix + "dag"),
            getSessionValue<ActivityScheduleValueFanout>(session, prefix + "value_fanout"),
            getSessionValue<ActivityScheduleTopoOrder>(session, prefix + "topo_order"),
            getSessionValue<ActivityScheduleStateReadSupernodes>(session, prefix + "state_read_supernodes"),
            getSessionValue<ActivityScheduleSupernodeKinds>(session, prefix + "supernode_kind"),
            getSessionValue<ActivityScheduleComputeNodesBySupernode>(session, prefix + "compute_nodes_by_supernode"),
            getSessionValue<std::string>(session, prefix + "summary_stats"),
        };
    }

    bool schedulesEqual(const ScheduleView &lhs, const ScheduleView &rhs)
    {
        if (lhs.supernodeToOps == nullptr || rhs.supernodeToOps == nullptr ||
            lhs.opToSupernode == nullptr || rhs.opToSupernode == nullptr ||
            lhs.dag == nullptr || rhs.dag == nullptr ||
            lhs.valueFanout == nullptr || rhs.valueFanout == nullptr ||
            lhs.topoOrder == nullptr || rhs.topoOrder == nullptr ||
            lhs.stateReadSupernodes == nullptr || rhs.stateReadSupernodes == nullptr ||
            lhs.supernodeKinds == nullptr || rhs.supernodeKinds == nullptr ||
            lhs.computeNodesBySupernode == nullptr || rhs.computeNodesBySupernode == nullptr ||
            lhs.summaryStats == nullptr || rhs.summaryStats == nullptr)
        {
            return false;
        }
        return *lhs.supernodeToOps == *rhs.supernodeToOps &&
               *lhs.opToSupernode == *rhs.opToSupernode &&
               *lhs.dag == *rhs.dag &&
               *lhs.valueFanout == *rhs.valueFanout &&
               *lhs.topoOrder == *rhs.topoOrder &&
               *lhs.stateReadSupernodes == *rhs.stateReadSupernodes &&
               *lhs.supernodeKinds == *rhs.supernodeKinds &&
               *lhs.computeNodesBySupernode == *rhs.computeNodesBySupernode &&
               *lhs.summaryStats == *rhs.summaryStats;
    }

    bool hasFanoutTo(const ActivityScheduleValueFanout &fanout,
                     wolvrix::lib::grh::ValueId value,
                     uint32_t supernode)
    {
        if (value.index == 0 || value.index > fanout.size())
        {
            return false;
        }
        const auto &succs = fanout[value.index - 1];
        return std::find(succs.begin(), succs.end(), supernode) != succs.end();
    }

    bool supernodeContains(const ActivityScheduleSupernodeToOps &supernodeToOps,
                           uint32_t supernode,
                           wolvrix::lib::grh::OperationId opId)
    {
        if (supernode == kInvalidActivitySupernodeId || supernode >= supernodeToOps.size())
        {
            return false;
        }
        const auto &ops = supernodeToOps[supernode];
        return std::find(ops.begin(), ops.end(), opId) != ops.end();
    }

    bool stateReadHasSupernode(const ActivityScheduleStateReadSupernodes &stateReadSupernodes,
                               const std::string &stateSymbol,
                               uint32_t supernode)
    {
        const auto it = stateReadSupernodes.find(stateSymbol);
        if (it == stateReadSupernodes.end())
        {
            return false;
        }
        const auto &supernodes = it->second;
        return std::find(supernodes.begin(), supernodes.end(), supernode) != supernodes.end();
    }

    std::size_t parseStatField(const std::string &stats, const std::string &name)
    {
        const std::string needle = name + "=";
        const std::size_t pos = stats.find(needle);
        if (pos == std::string::npos)
        {
            return 0;
        }
        std::size_t end = pos + needle.size();
        while (end < stats.size() && stats[end] >= '0' && stats[end] <= '9')
        {
            ++end;
        }
        return static_cast<std::size_t>(std::stoull(stats.substr(pos + needle.size(), end - pos - needle.size())));
    }

    double parseJsonDoubleField(const std::string &json, const std::string &name)
    {
        const std::string needle = "\"" + name + "\":";
        const std::size_t pos = json.find(needle);
        if (pos == std::string::npos)
        {
            return -1.0;
        }
        std::size_t end = pos + needle.size();
        while (end < json.size() &&
               (json[end] == '-' || json[end] == '+' || json[end] == '.' ||
                json[end] == 'e' || json[end] == 'E' ||
                (json[end] >= '0' && json[end] <= '9')))
        {
            ++end;
        }
        return std::stod(json.substr(pos + needle.size(), end - pos - needle.size()));
    }

    void setIntentShape(wolvrix::lib::grh::Graph &graph,
                        wolvrix::lib::grh::OperationId opId,
                        const std::string &group,
                        const std::string &role,
                        int64_t elementWidth,
                        int64_t elementCount)
    {
        graph.setAttr(opId, "regToMem.intent.group", group);
        graph.setAttr(opId, "regToMem.intent.mode", std::string("array-index"));
        graph.setAttr(opId, "regToMem.intent.role", role);
        graph.setAttr(opId, "regToMem.intent.elementWidth", elementWidth);
        graph.setAttr(opId, "regToMem.intent.elementCount", elementCount);
    }

    int validateCommonScheduleShape(const wolvrix::lib::grh::Graph &graph,
                                    const ScheduleView &schedule)
    {
        if (schedule.supernodeToOps == nullptr || schedule.opToSupernode == nullptr ||
            schedule.dag == nullptr || schedule.valueFanout == nullptr ||
            schedule.topoOrder == nullptr || schedule.stateReadSupernodes == nullptr ||
            schedule.supernodeKinds == nullptr || schedule.computeNodesBySupernode == nullptr ||
            schedule.summaryStats == nullptr)
        {
            return fail("Expected all activity-schedule session outputs to exist");
        }
        if (schedule.supernodeToOps->size() != schedule.supernodeKinds->size() ||
            schedule.supernodeToOps->size() != schedule.topoOrder->size())
        {
            return fail("Expected supernode outputs to have matching sizes");
        }
        for (uint32_t supernodeId = 0; supernodeId < schedule.supernodeToOps->size(); ++supernodeId)
        {
            const bool commitKind =
                (*schedule.supernodeKinds)[supernodeId] == ActivityScheduleSupernodeKind::Commit;
            for (const auto opId : (*schedule.supernodeToOps)[supernodeId])
            {
                const bool commitOp = isCommitPhaseOp(graph.getOperation(opId));
                if (commitKind != commitOp)
                {
                    return fail("Expected explicit supernode_kind to match contained ops");
                }
            }
        }
        return 0;
    }

    int validateLevelOpTopoOrder(const ScheduleView &schedule)
    {
        const auto &supernodeToOps = *schedule.supernodeToOps;
        const auto &dag = *schedule.dag;
        const auto &topoOrder = *schedule.topoOrder;
        std::vector<std::size_t> minOpIndex(supernodeToOps.size(),
                                            std::numeric_limits<std::size_t>::max());
        for (std::size_t supernodeId = 0; supernodeId < supernodeToOps.size(); ++supernodeId)
        {
            for (const auto opId : supernodeToOps[supernodeId])
            {
                minOpIndex[supernodeId] =
                    std::min(minOpIndex[supernodeId], static_cast<std::size_t>(opId.index));
            }
        }

        std::vector<uint32_t> indegree(dag.size(), 0);
        for (const auto &succs : dag)
        {
            for (const uint32_t succ : succs)
            {
                if (succ >= indegree.size())
                {
                    return fail("Expected final topo DAG successors to be in range");
                }
                ++indegree[succ];
            }
        }
        std::vector<uint32_t> frontier;
        for (uint32_t node = 0; node < indegree.size(); ++node)
        {
            if (indegree[node] == 0)
            {
                frontier.push_back(node);
            }
        }

        std::size_t topoOffset = 0;
        while (!frontier.empty())
        {
            std::sort(frontier.begin(),
                      frontier.end(),
                      [&](uint32_t lhs, uint32_t rhs)
                      {
                          if (minOpIndex[lhs] != minOpIndex[rhs])
                          {
                              return minOpIndex[lhs] < minOpIndex[rhs];
                          }
                          return lhs < rhs;
                      });
            if (topoOffset + frontier.size() > topoOrder.size() ||
                !std::equal(frontier.begin(), frontier.end(), topoOrder.begin() + topoOffset))
            {
                return fail("Expected level-op final topo to sort each Kahn layer by minimum op index");
            }

            std::vector<uint32_t> nextFrontier;
            for (const uint32_t node : frontier)
            {
                for (const uint32_t succ : dag[node])
                {
                    if (--indegree[succ] == 0)
                    {
                        nextFrontier.push_back(succ);
                    }
                }
            }
            topoOffset += frontier.size();
            frontier = std::move(nextFrontier);
        }
        if (topoOffset != topoOrder.size())
        {
            return fail("Expected level-op final topo to contain every supernode");
        }
        return 0;
    }

    int validateReadyOpTopoOrder(const ScheduleView &schedule)
    {
        const auto &supernodeToOps = *schedule.supernodeToOps;
        const auto &dag = *schedule.dag;
        const auto &topoOrder = *schedule.topoOrder;
        std::vector<std::size_t> minOpIndex(supernodeToOps.size(),
                                            std::numeric_limits<std::size_t>::max());
        for (std::size_t supernodeId = 0; supernodeId < supernodeToOps.size(); ++supernodeId)
        {
            for (const auto opId : supernodeToOps[supernodeId])
            {
                minOpIndex[supernodeId] =
                    std::min(minOpIndex[supernodeId], static_cast<std::size_t>(opId.index));
            }
        }
        auto lessByKey = [&](uint32_t lhs, uint32_t rhs)
        {
            if (minOpIndex[lhs] != minOpIndex[rhs])
            {
                return minOpIndex[lhs] < minOpIndex[rhs];
            }
            return lhs < rhs;
        };

        std::vector<uint32_t> indegree(dag.size(), 0);
        for (const auto &succs : dag)
        {
            for (const uint32_t succ : succs)
            {
                if (succ >= indegree.size())
                {
                    return fail("Expected ready-op DAG successors to be in range");
                }
                ++indegree[succ];
            }
        }
        std::vector<uint32_t> readyStack;
        for (uint32_t node = 0; node < indegree.size(); ++node)
        {
            if (indegree[node] == 0)
            {
                readyStack.push_back(node);
            }
        }
        std::sort(readyStack.begin(), readyStack.end(), lessByKey);

        std::size_t topoOffset = 0;
        while (!readyStack.empty())
        {
            const uint32_t node = readyStack.back();
            readyStack.pop_back();
            if (topoOffset >= topoOrder.size() || topoOrder[topoOffset++] != node)
            {
                return fail("Expected ready-op final topo to follow stable ready-stack order");
            }
            std::vector<uint32_t> orderedSuccs = dag[node];
            std::sort(orderedSuccs.begin(), orderedSuccs.end(), lessByKey);
            for (const uint32_t succ : orderedSuccs)
            {
                if (indegree[succ] == 0)
                {
                    return fail("Expected ready-op DAG edges to be unique");
                }
                if (--indegree[succ] == 0)
                {
                    readyStack.push_back(succ);
                }
            }
        }
        if (topoOffset != topoOrder.size())
        {
            return fail("Expected ready-op final topo to contain every supernode");
        }
        return 0;
    }

    int validateScheduleTopoOrder(const ScheduleView &schedule)
    {
        if (schedule.dag == nullptr || schedule.topoOrder == nullptr ||
            schedule.dag->size() != schedule.topoOrder->size())
        {
            return fail("Expected schedule DAG and topo order to have matching sizes");
        }
        std::vector<uint32_t> position(schedule.topoOrder->size(), kInvalidActivitySupernodeId);
        for (uint32_t pos = 0; pos < schedule.topoOrder->size(); ++pos)
        {
            const uint32_t supernode = (*schedule.topoOrder)[pos];
            if (supernode >= position.size() ||
                position[supernode] != kInvalidActivitySupernodeId)
            {
                return fail("Expected schedule topo order to be a permutation");
            }
            position[supernode] = pos;
        }
        for (uint32_t from = 0; from < schedule.dag->size(); ++from)
        {
            for (const uint32_t to : (*schedule.dag)[from])
            {
                if (to >= position.size() || position[from] >= position[to])
                {
                    return fail("Expected every schedule DAG edge to follow topo order");
                }
            }
        }
        return 0;
    }

    std::string readFile(const std::filesystem::path &path)
    {
        std::ifstream in(path);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

} // namespace

int main()
{
    std::string currentCase;
    try
    {
    {
        currentCase = "reg-to-mem intent group";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("intent_top");
        design.markAsTop("intent_top");

        const auto idxReg = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegister,
                                                  graph.internSymbol("idx_reg"));
        graph.setAttr(idxReg, "width", int64_t{2});
        graph.setAttr(idxReg, "isSigned", false);

        const auto idx = makeValue(graph, "idx", 2);
        const auto idxRead = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterReadPort,
                                                   graph.internSymbol("idx_read_op"));
        graph.addResult(idxRead, idx);
        graph.setAttr(idxRead, "regSymbol", std::string("idx_reg"));

        std::vector<wolvrix::lib::grh::OperationId> reads;
        std::vector<wolvrix::lib::grh::ValueId> readValues;
        reads.reserve(4);
        readValues.reserve(4);
        for (int row = 0; row < 4; ++row)
        {
            const std::string reg = "r" + std::to_string(row);
            const auto regOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegister,
                                                     graph.internSymbol(reg));
            graph.setAttr(regOp, "width", int64_t{8});
            graph.setAttr(regOp, "isSigned", false);
            graph.setAttr(regOp, "regToMem.intent.group", std::string("rtm_intent_test"));
            graph.setAttr(regOp, "regToMem.intent.mode", std::string("array-index"));
            graph.setAttr(regOp, "regToMem.intent.role", std::string("register"));
            graph.setAttr(regOp, "regToMem.intent.row", static_cast<int64_t>(row));
            graph.setAttr(regOp, "regToMem.intent.elementWidth", int64_t{8});
            graph.setAttr(regOp, "regToMem.intent.elementCount", int64_t{4});

            const auto readValue = makeValue(graph, reg + "_read", 8);
            const auto readOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterReadPort,
                                                      graph.internSymbol(reg + "_read_op"));
            graph.addResult(readOp, readValue);
            graph.setAttr(readOp, "regSymbol", reg);
            graph.setAttr(readOp, "regToMem.intent.group", std::string("rtm_intent_test"));
            graph.setAttr(readOp, "regToMem.intent.mode", std::string("array-index"));
            graph.setAttr(readOp, "regToMem.intent.role", std::string("read"));
            graph.setAttr(readOp, "regToMem.intent.row", static_cast<int64_t>(row));
            reads.push_back(readOp);
            readValues.push_back(readValue);
        }

        const auto packed = makeValue(graph, "packed", 32);
        const auto concat = graph.createOperation(wolvrix::lib::grh::OperationKind::kConcat,
                                                  graph.internSymbol("packed_concat"));
        for (int row = 3; row >= 0; --row)
        {
            graph.addOperand(concat, readValues[static_cast<std::size_t>(row)]);
        }
        graph.addResult(concat, packed);
        graph.setAttr(concat, "regToMem.intent.group", std::string("rtm_intent_test"));
        graph.setAttr(concat, "regToMem.intent.mode", std::string("array-index"));
        graph.setAttr(concat, "regToMem.intent.role", std::string("concat"));
        graph.setAttr(concat, "regToMem.intent.elementWidth", int64_t{8});
        graph.setAttr(concat, "regToMem.intent.elementCount", int64_t{4});
        graph.setAttr(concat, "regToMem.intent.regSymbols",
                      std::vector<std::string>{"r0", "r1", "r2", "r3"});
        graph.setAttr(concat, "regToMem.intent.operandRows",
                      std::vector<int64_t>{3, 2, 1, 0});

        const auto selected = makeValue(graph, "selected", 8);
        const auto slice = graph.createOperation(wolvrix::lib::grh::OperationKind::kSliceArray,
                                                 graph.internSymbol("selected_slice"));
        graph.addOperand(slice, packed);
        graph.addOperand(slice, idx);
        graph.addResult(slice, selected);
        graph.setAttr(slice, "sliceWidth", int64_t{8});
        graph.setAttr(slice, "regToMem.intent.group", std::string("rtm_intent_test"));
        graph.setAttr(slice, "regToMem.intent.mode", std::string("array-index"));
        graph.setAttr(slice, "regToMem.intent.role", std::string("slice"));
        graph.setAttr(slice, "regToMem.intent.sliceKind", std::string("slice-array"));
        graph.setAttr(slice, "regToMem.intent.elementWidth", int64_t{8});
        graph.setAttr(slice, "regToMem.intent.elementCount", int64_t{4});
        graph.bindOutputPort("selected", selected);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(
            ActivityScheduleOptions{.path = "intent_top",
                                    .maxOpInComputeSupernode = 64,
                                    .maxOpInComputeNode = 2,
                                    .enableCoarsen = false}));

        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected activity-schedule pass to succeed for reg-to-mem intent group");
        }
        const auto schedule = loadSchedule(session, "intent_top");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        if (schedule.opToSupernode == nullptr || schedule.supernodeToOps == nullptr)
        {
            return fail("Missing activity-schedule outputs for reg-to-mem intent group");
        }
        if (slice.index == 0 || slice.index > schedule.opToSupernode->size())
        {
            return fail("slice op missing from op-to-supernode map");
        }
        const uint32_t owner = (*schedule.opToSupernode)[slice.index - 1];
        if (owner == kInvalidActivitySupernodeId || owner >= schedule.supernodeToOps->size())
        {
            return fail("slice op has invalid supernode owner");
        }
        const auto &ownerOps = (*schedule.supernodeToOps)[owner];
        if (std::find(ownerOps.begin(), ownerOps.end(), concat) == ownerOps.end())
        {
            return fail("reg-to-mem intent concat was split from slice");
        }
        const auto scheduledConcatOperands = graph.opOperands(concat);
        for (const auto readValue : scheduledConcatOperands)
        {
            const auto read = graph.valueDef(readValue);
            if (std::find(ownerOps.begin(), ownerOps.end(), read) == ownerOps.end())
            {
                return fail("reg-to-mem intent read was split from slice");
            }
        }
        const auto scheduledSliceOperands = graph.opOperands(slice);
        if (scheduledSliceOperands.size() != 2)
        {
            return fail("reg-to-mem intent slice operands were rewritten unexpectedly");
        }
        const auto scheduledIndex = scheduledSliceOperands[1];
        const auto scheduledIndexRead = graph.valueDef(scheduledIndex);
        if (!scheduledIndexRead.valid() ||
            graph.opKind(scheduledIndexRead) != wolvrix::lib::grh::OperationKind::kRegisterReadPort)
        {
            return fail("reg-to-mem intent index should still be defined by a register read");
        }
        if (scheduledIndexRead.index == 0 || scheduledIndexRead.index > schedule.opToSupernode->size())
        {
            return fail("reg-to-mem intent index read missing from op-to-supernode map");
        }
        const uint32_t indexOwner = (*schedule.opToSupernode)[scheduledIndexRead.index - 1];
        if (indexOwner == kInvalidActivitySupernodeId || indexOwner >= schedule.supernodeToOps->size())
        {
            return fail("reg-to-mem intent index read has invalid supernode owner");
        }
        if (indexOwner == owner)
        {
            const auto &mergedOps = (*schedule.supernodeToOps)[owner];
            if (std::find(mergedOps.begin(), mergedOps.end(), scheduledIndexRead) == mergedOps.end())
            {
                return fail("reg-to-mem intent index read owner does not contain the read op");
            }
        }
        else if (!hasFanoutTo(*schedule.valueFanout, scheduledIndex, owner))
        {
            return fail("reg-to-mem intent index boundary value is not scheduled into the intent group");
        }
        for (int row = 0; row < 4; ++row)
        {
            if (!stateReadHasSupernode(*schedule.stateReadSupernodes, "r" + std::to_string(row), owner))
            {
                return fail("reg-to-mem intent slice missing storage-register activation mapping");
            }
        }
    }

    {
        currentCase = "reg-to-mem dynamic intent index";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("intent_dynamic");
        design.markAsTop("intent_dynamic");

        const std::string group = "rtm_intent_dyn";
        const auto idx = makeValue(graph, "idx", 2);
        const auto one = makeConstant(graph, "one_const", "one", 2, "2'd1");
        graph.bindInputPort("idx", idx);

        const auto idxPlus = makeValue(graph, "idx_plus", 2);
        const auto idxAdd = graph.createOperation(wolvrix::lib::grh::OperationKind::kAdd,
                                                  graph.internSymbol("idx_plus_add"));
        graph.addOperand(idxAdd, idx);
        graph.addOperand(idxAdd, one);
        graph.addResult(idxAdd, idxPlus);

        std::vector<wolvrix::lib::grh::ValueId> readValues;
        readValues.reserve(4);
        for (int row = 0; row < 4; ++row)
        {
            const std::string reg = "r" + std::to_string(row);
            const auto regOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegister,
                                                     graph.internSymbol(reg));
            graph.setAttr(regOp, "width", int64_t{8});
            graph.setAttr(regOp, "isSigned", false);
            setIntentShape(graph, regOp, group, "register", 8, 4);
            graph.setAttr(regOp, "regToMem.intent.row", static_cast<int64_t>(row));

            const auto readValue = makeValue(graph, reg + "_read", 8);
            const auto readOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterReadPort,
                                                      graph.internSymbol(reg + "_read_op"));
            graph.addResult(readOp, readValue);
            graph.setAttr(readOp, "regSymbol", reg);
            graph.setAttr(readOp, "regToMem.intent.group", group);
            graph.setAttr(readOp, "regToMem.intent.mode", std::string("array-index"));
            graph.setAttr(readOp, "regToMem.intent.role", std::string("read"));
            graph.setAttr(readOp, "regToMem.intent.row", static_cast<int64_t>(row));
            readValues.push_back(readValue);
        }

        const auto packed = makeValue(graph, "packed", 32);
        const auto concat = graph.createOperation(wolvrix::lib::grh::OperationKind::kConcat,
                                                  graph.internSymbol("packed_concat"));
        for (int row = 3; row >= 0; --row)
        {
            graph.addOperand(concat, readValues[static_cast<std::size_t>(row)]);
        }
        graph.addResult(concat, packed);
        setIntentShape(graph, concat, group, "concat", 8, 4);
        graph.setAttr(concat, "regToMem.intent.regSymbols",
                      std::vector<std::string>{"r0", "r1", "r2", "r3"});
        graph.setAttr(concat, "regToMem.intent.operandRows",
                      std::vector<int64_t>{3, 2, 1, 0});

        const auto elemWidth = makeConstant(graph, "elem_width_const", "elem_width", 4, "4'd8");
        const auto start = makeValue(graph, "start", 4);
        const auto mul = graph.createOperation(wolvrix::lib::grh::OperationKind::kMul,
                                               graph.internSymbol("start_mul"));
        graph.addOperand(mul, idxPlus);
        graph.addOperand(mul, elemWidth);
        graph.addResult(mul, start);

        const auto selected = makeValue(graph, "selected", 8);
        const auto slice = graph.createOperation(wolvrix::lib::grh::OperationKind::kSliceDynamic,
                                                 graph.internSymbol("selected_slice"));
        graph.addOperand(slice, packed);
        graph.addOperand(slice, start);
        graph.addResult(slice, selected);
        graph.setAttr(slice, "sliceWidth", int64_t{8});
        setIntentShape(graph, slice, group, "slice", 8, 4);
        graph.setAttr(slice, "regToMem.intent.sliceKind", std::string("slice-dynamic"));
        graph.bindOutputPort("selected", selected);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(
            ActivityScheduleOptions{.path = "intent_dynamic",
                                    .maxOpInComputeSupernode = 6,
                                    .maxOpInComputeNode = 2,
                                    .enableCoarsen = false}));

        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected activity-schedule pass to succeed for dynamic reg-to-mem intent group");
        }
        const auto schedule = loadSchedule(session, "intent_dynamic");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        const uint32_t sliceOwner = (*schedule.opToSupernode)[slice.index - 1];
        const uint32_t addOwner = (*schedule.opToSupernode)[idxAdd.index - 1];
        const uint32_t mulOwner = (*schedule.opToSupernode)[mul.index - 1];
        if (sliceOwner == kInvalidActivitySupernodeId || addOwner == kInvalidActivitySupernodeId)
        {
            return fail("dynamic intent slice or canonical index producer missing from schedule");
        }
        if (!supernodeContains(*schedule.supernodeToOps, sliceOwner, concat))
        {
            return fail("dynamic reg-to-mem intent concat was split from slice");
        }
        if (supernodeContains(*schedule.supernodeToOps, sliceOwner, idxAdd))
        {
            return fail("dynamic reg-to-mem intent should treat canonical index producer as boundary");
        }
        if (mulOwner != kInvalidActivitySupernodeId)
        {
            return fail("dynamic reg-to-mem intent should not schedule start-mul as the semantic index dependency");
        }
        if (!hasFanoutTo(*schedule.valueFanout, idxPlus, sliceOwner))
        {
            return fail("dynamic reg-to-mem intent missing canonical index fanout into intent group");
        }
        for (int row = 0; row < 4; ++row)
        {
            if (!stateReadHasSupernode(*schedule.stateReadSupernodes, "r" + std::to_string(row), sliceOwner))
            {
                return fail("dynamic reg-to-mem intent slice missing storage-register activation mapping");
            }
        }
    }

    {
        currentCase = "reg-to-mem dynamic input intent index";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("intent_dynamic_input");
        design.markAsTop("intent_dynamic_input");

        const std::string group = "rtm_intent_dyn_input";
        const auto idx = makeValue(graph, "idx", 2);
        graph.bindInputPort("idx", idx);

        std::vector<wolvrix::lib::grh::ValueId> readValues;
        readValues.reserve(4);
        for (int row = 0; row < 4; ++row)
        {
            const std::string reg = "r" + std::to_string(row);
            const auto regOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegister,
                                                     graph.internSymbol(reg));
            graph.setAttr(regOp, "width", int64_t{8});
            graph.setAttr(regOp, "isSigned", false);
            setIntentShape(graph, regOp, group, "register", 8, 4);
            graph.setAttr(regOp, "regToMem.intent.row", static_cast<int64_t>(row));

            const auto readValue = makeValue(graph, reg + "_read", 8);
            const auto readOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterReadPort,
                                                      graph.internSymbol(reg + "_read_op"));
            graph.addResult(readOp, readValue);
            graph.setAttr(readOp, "regSymbol", reg);
            graph.setAttr(readOp, "regToMem.intent.group", group);
            graph.setAttr(readOp, "regToMem.intent.mode", std::string("array-index"));
            graph.setAttr(readOp, "regToMem.intent.role", std::string("read"));
            graph.setAttr(readOp, "regToMem.intent.row", static_cast<int64_t>(row));
            readValues.push_back(readValue);
        }

        const auto packed = makeValue(graph, "packed", 32);
        const auto concat = graph.createOperation(wolvrix::lib::grh::OperationKind::kConcat,
                                                  graph.internSymbol("packed_concat"));
        for (int row = 3; row >= 0; --row)
        {
            graph.addOperand(concat, readValues[static_cast<std::size_t>(row)]);
        }
        graph.addResult(concat, packed);
        setIntentShape(graph, concat, group, "concat", 8, 4);
        graph.setAttr(concat, "regToMem.intent.regSymbols",
                      std::vector<std::string>{"r0", "r1", "r2", "r3"});
        graph.setAttr(concat, "regToMem.intent.operandRows",
                      std::vector<int64_t>{3, 2, 1, 0});

        const auto elemWidth = makeConstant(graph, "elem_width_const", "elem_width", 4, "4'd8");
        const auto start = makeValue(graph, "start", 5);
        const auto mul = graph.createOperation(wolvrix::lib::grh::OperationKind::kMul,
                                               graph.internSymbol("start_mul"));
        graph.addOperand(mul, idx);
        graph.addOperand(mul, elemWidth);
        graph.addResult(mul, start);

        const auto selected = makeValue(graph, "selected", 8);
        const auto slice = graph.createOperation(wolvrix::lib::grh::OperationKind::kSliceDynamic,
                                                 graph.internSymbol("selected_slice"));
        graph.addOperand(slice, packed);
        graph.addOperand(slice, start);
        graph.addResult(slice, selected);
        graph.setAttr(slice, "sliceWidth", int64_t{8});
        setIntentShape(graph, slice, group, "slice", 8, 4);
        graph.setAttr(slice, "regToMem.intent.sliceKind", std::string("slice-dynamic"));
        graph.bindOutputPort("selected", selected);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(
            ActivityScheduleOptions{.path = "intent_dynamic_input",
                                    .maxOpInComputeSupernode = 6,
                                    .maxOpInComputeNode = 2,
                                    .enableCoarsen = false,
                                    .postDpRefinePolicy = "strict",
                                    .kahnLevelPackPolicy = "strict"}));

        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected activity-schedule pass to succeed for dynamic input reg-to-mem intent group");
        }
        const auto schedule = loadSchedule(session, "intent_dynamic_input");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        const uint32_t sliceOwner = (*schedule.opToSupernode)[slice.index - 1];
        const uint32_t mulOwner = (*schedule.opToSupernode)[mul.index - 1];
        if (sliceOwner == kInvalidActivitySupernodeId)
        {
            return fail("dynamic input reg-to-mem intent slice missing from schedule");
        }
        if (!supernodeContains(*schedule.supernodeToOps, sliceOwner, concat))
        {
            return fail("dynamic input reg-to-mem intent concat was split from slice");
        }
        if (mulOwner != kInvalidActivitySupernodeId)
        {
            return fail("dynamic input reg-to-mem intent should not schedule start-mul as dependency");
        }
        if (!hasFanoutTo(*schedule.valueFanout, idx, sliceOwner))
        {
            return fail("dynamic input reg-to-mem intent missing input index fanout into intent group");
        }
        for (int row = 0; row < 4; ++row)
        {
            if (!stateReadHasSupernode(*schedule.stateReadSupernodes, "r" + std::to_string(row), sliceOwner))
            {
                return fail("dynamic input reg-to-mem intent slice missing storage-register activation mapping");
            }
        }
    }

    {
        currentCase = "top";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("top");
        design.markAsTop("top");

        const auto clk = makeValue(graph, "clk", 1);
        const auto en = makeValue(graph, "en", 1);
        const auto a = makeValue(graph, "a", 8);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("en", en);
        graph.bindInputPort("a", a);

        const auto regDecl = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegister,
                                                   graph.internSymbol("q"));
        graph.setAttr(regDecl, "width", static_cast<int64_t>(8));
        graph.setAttr(regDecl, "isSigned", false);

        const auto qReadValue = makeValue(graph, "q_read", 8);
        const auto qReadOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterReadPort,
                                                   graph.internSymbol("q_read_op"));
        graph.addResult(qReadOp, qReadValue);
        graph.setAttr(qReadOp, "regSymbol", std::string("q"));

        const auto maskValue = makeValue(graph, "mask_all", 8);
        const auto maskOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kConstant,
                                                  graph.internSymbol("mask_const"));
        graph.addResult(maskOp, maskValue);
        graph.setAttr(maskOp, "constValue", std::string("8'hFF"));

        const auto sumValue = makeValue(graph, "sum", 8);
        const auto addOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kAdd,
                                                 graph.internSymbol("sum_add"));
        graph.addOperand(addOp, qReadValue);
        graph.addOperand(addOp, a);
        graph.addResult(addOp, sumValue);
        graph.bindOutputPort("y", sumValue);

        const auto writeOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                                                   graph.internSymbol("q_write"));
        graph.addOperand(writeOp, en);
        graph.addOperand(writeOp, sumValue);
        graph.addOperand(writeOp, maskValue);
        graph.addOperand(writeOp, clk);
        graph.setAttr(writeOp, "regSymbol", std::string("q"));
        graph.setAttr(writeOp, "eventEdge", std::vector<std::string>{"posedge"});

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        const std::filesystem::path exportPath =
            std::filesystem::path(WOLF_SV_TEST_ARTIFACT_DIR) / "activity_schedule_top_compute_dag.json";
        manager.addPass(std::make_unique<ActivitySchedulePass>(
            ActivityScheduleOptions{.path = "top",
                                    .maxOpInComputeSupernode = 4,
                                    .exportComputeDagPath = exportPath.string()}));

        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected activity-schedule pass to succeed");
        }
        const auto schedule = loadSchedule(session, "top");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        if (!std::filesystem::exists(exportPath))
        {
            return fail("Expected activity-schedule compute DAG export file to exist");
        }
        const std::string exportedDag = readFile(exportPath);
        if (exportedDag.find("\"format\":\"wolvrix.compute-op-dag.v1\"") == std::string::npos ||
            exportedDag.find("\"graph_id\":\"top.activity_compute\"") == std::string::npos ||
            exportedDag.find("\"nodes\"") == std::string::npos ||
            exportedDag.find("\"edges\"") == std::string::npos)
        {
            return fail("Expected compute DAG export to contain harness JSON fields");
        }

        const uint32_t addSupernode = (*schedule.opToSupernode)[addOp.index - 1];
        const uint32_t writeSupernode = (*schedule.opToSupernode)[writeOp.index - 1];
        if (addSupernode == kInvalidActivitySupernodeId || writeSupernode == kInvalidActivitySupernodeId ||
            addSupernode == writeSupernode)
        {
            return fail("Expected compute and commit ops to map to distinct supernodes");
        }
        if ((*schedule.opToSupernode)[regDecl.index - 1] != kInvalidActivitySupernodeId)
        {
            return fail("Expected declaration op to stay out of schedule");
        }
        if (!hasFanoutTo(*schedule.valueFanout, sumValue, writeSupernode))
        {
            return fail("Expected compute value fanout into commit supernode");
        }
        if (!hasFanoutTo(*schedule.valueFanout, maskValue, writeSupernode))
        {
            return fail("Expected direct source value dependency into commit supernode");
        }
        if (schedule.summaryStats->find("\"compute_commit_value_pairs\":2") == std::string::npos)
        {
            return fail("Expected summary_stats to report two compute->commit value pairs in top case");
        }
        if (schedule.summaryStats->find("\"compute_compute_value_pairs\":0") == std::string::npos)
        {
            return fail("Expected summary_stats to report zero compute->compute value pairs in top case");
        }
        if (schedule.summaryStats->find("\"state_read_activation_edges\":0") == std::string::npos)
        {
            return fail("Expected top case to avoid cross-supernode state-read propagation");
        }
        if (schedule.summaryStats->find("\"memory_read_activation_edges\":0") == std::string::npos)
        {
            return fail("Expected top case to report zero memory-read propagation");
        }
        if (schedule.summaryStats->find("\"constant_activation_edges\":1") == std::string::npos)
        {
            return fail("Expected top case to report one constant propagation edge");
        }
        if (schedule.summaryStats->find("\"other_compute_activation_edges\":1") == std::string::npos)
        {
            return fail("Expected top case to report one compute propagation edge");
        }
        const auto readersIt = schedule.stateReadSupernodes->find("q");
        if (readersIt == schedule.stateReadSupernodes->end() || readersIt->second.empty())
        {
            return fail("Expected register read state mapping to compute supernode");
        }
    }

    {
        currentCase = "source_compute";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("source_compute");
        design.markAsTop("source_compute");

        const auto a = makeValue(graph, "a", 8);
        graph.bindInputPort("a", a);
        const auto c = makeValue(graph, "c", 8);
        const auto cOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kConstant,
                                               graph.internSymbol("const_source"));
        graph.addResult(cOp, c);
        graph.setAttr(cOp, "constValue", std::string("8'h01"));
        const auto y = makeValue(graph, "y", 8);
        const auto addOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kAdd,
                                                 graph.internSymbol("add"));
        graph.addOperand(addOp, a);
        graph.addOperand(addOp, c);
        graph.addResult(addOp, y);
        graph.bindOutputPort("y", y);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(ActivityScheduleOptions{
            .path = "source_compute",
            .maxOpInComputeSupernode = 1,
            .enableCoarsen = false,
        }));
        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected source-to-compute schedule to succeed");
        }
        const auto schedule = loadSchedule(session, "source_compute");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        const uint32_t addSupernode = (*schedule.opToSupernode)[addOp.index - 1];
        if (addSupernode == kInvalidActivitySupernodeId)
        {
            return fail("Expected compute op to map to a supernode");
        }
        bool foundLocalConstClone = false;
        wolvrix::lib::grh::OperationId clonedConstOp = wolvrix::lib::grh::OperationId::invalid();
        for (const auto opId : (*schedule.supernodeToOps)[addSupernode])
        {
            if (opId != cOp && graph.opKind(opId) == wolvrix::lib::grh::OperationKind::kConstant)
            {
                foundLocalConstClone = true;
                clonedConstOp = opId;
            }
        }
        if (!foundLocalConstClone)
        {
            for (const auto opId : graph.operations())
            {
                if (opId != cOp && graph.opKind(opId) == wolvrix::lib::grh::OperationKind::kConstant)
                {
                    const auto supernode = (*schedule.opToSupernode)[opId.index - 1];
                    if (supernode != kInvalidActivitySupernodeId)
                    {
                        foundLocalConstClone = true;
                        clonedConstOp = opId;
                        break;
                    }
                }
            }
        }
        if (!foundLocalConstClone || !clonedConstOp.valid())
        {
            return fail("Expected source constant clone to enter compute scheduling");
        }
        if (graph.opOperands(addOp).size() < 2 ||
            graph.valueDef(graph.opOperands(addOp)[1]) != clonedConstOp)
        {
            return fail("Expected compute op operand to be rewritten to source clone");
        }
    }

    {
        currentCase = "plain trigger equal chain";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("plain_trigger_equal_chain");
        design.markAsTop("plain_trigger_equal_chain");

        const auto a = makeValue(graph, "a", 1);
        graph.bindInputPort("a", a);

        const auto mid = makeValue(graph, "mid", 1);
        const auto first = graph.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                                 graph.internSymbol("first_assign"));
        graph.addOperand(first, a);
        graph.addResult(first, mid);

        const auto out = makeValue(graph, "out", 1);
        const auto second = graph.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                                  graph.internSymbol("second_assign"));
        graph.addOperand(second, mid);
        graph.addResult(second, out);
        graph.bindOutputPort("out", out);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(ActivityScheduleOptions{
            .path = "plain_trigger_equal_chain",
            .maxOpInComputeSupernode = 1,
            .maxOpInComputeNode = 1,
            .enableCoarsen = false,
        }));

        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected plain trigger equal chain schedule to succeed");
        }
        const auto schedule = loadSchedule(session, "plain_trigger_equal_chain");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        const uint32_t firstSupernode = (*schedule.opToSupernode)[first.index - 1];
        const uint32_t secondSupernode = (*schedule.opToSupernode)[second.index - 1];
        if (firstSupernode == kInvalidActivitySupernodeId ||
            secondSupernode == kInvalidActivitySupernodeId ||
            firstSupernode == secondSupernode)
        {
            return fail("Expected trigger chain ops to stay in distinct compute supernodes");
        }
        if (!hasFanoutTo(*schedule.valueFanout, mid, secondSupernode))
        {
            return fail("Expected trigger chain to expose one compute->compute value target");
        }
    }

    {
        currentCase = "plain coarsen chain";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("plain_coarsen_chain");
        design.markAsTop("plain_coarsen_chain");

        const auto a = makeValue(graph, "a", 1);
        graph.bindInputPort("a", a);

        const auto mid = makeValue(graph, "mid", 1);
        const auto first = graph.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                                 graph.internSymbol("first_assign"));
        graph.addOperand(first, a);
        graph.addResult(first, mid);

        const auto out = makeValue(graph, "out", 1);
        const auto second = graph.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                                  graph.internSymbol("second_assign"));
        graph.addOperand(second, mid);
        graph.addResult(second, out);
        graph.bindOutputPort("out", out);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(ActivityScheduleOptions{
            .path = "plain_coarsen_chain",
            .maxOpInComputeSupernode = 2,
            .maxOpInComputeNode = 1,
            .enableCoarsen = true,
        }));

        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected plain coarsen chain schedule to succeed");
        }
        const auto schedule = loadSchedule(session, "plain_coarsen_chain");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        const uint32_t firstSupernode = (*schedule.opToSupernode)[first.index - 1];
        const uint32_t secondSupernode = (*schedule.opToSupernode)[second.index - 1];
        if (firstSupernode == kInvalidActivitySupernodeId ||
            firstSupernode != secondSupernode)
        {
            return fail("Expected plain coarsen to merge the direct chain into one compute supernode");
        }
    }

    {
        currentCase = "plain sibling coarsen";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("plain_sibling_coarsen");
        design.markAsTop("plain_sibling_coarsen");

        const auto a = makeValue(graph, "a", 1);
        graph.bindInputPort("a", a);
        const auto b = makeValue(graph, "b", 1);
        graph.bindInputPort("b", b);

        const auto rootValue = makeValue(graph, "root_value", 1);
        const auto root = graph.createOperation(wolvrix::lib::grh::OperationKind::kAnd,
                                                graph.internSymbol("root_assign"));
        graph.addOperand(root, a);
        graph.addOperand(root, b);
        graph.addResult(root, rootValue);

        const auto leftValue = makeValue(graph, "left_value", 1);
        const auto left = graph.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                                graph.internSymbol("left_assign"));
        graph.addOperand(left, rootValue);
        graph.addResult(left, leftValue);
        graph.bindOutputPort("left_out", leftValue);

        const auto rightValue = makeValue(graph, "right_value", 1);
        const auto right = graph.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                                 graph.internSymbol("right_assign"));
        graph.addOperand(right, rootValue);
        graph.addResult(right, rightValue);
        graph.bindOutputPort("right_out", rightValue);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(ActivityScheduleOptions{
            .path = "plain_sibling_coarsen",
            .maxOpInComputeSupernode = 2,
            .maxOpInComputeNode = 1,
            .enableCoarsen = true,
            .enableChainMerge = false,
        }));

        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected plain sibling coarsen schedule to succeed");
        }
        const auto schedule = loadSchedule(session, "plain_sibling_coarsen");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        const uint32_t rootSupernode = (*schedule.opToSupernode)[root.index - 1];
        const uint32_t leftSupernode = (*schedule.opToSupernode)[left.index - 1];
        const uint32_t rightSupernode = (*schedule.opToSupernode)[right.index - 1];
        if (rootSupernode == kInvalidActivitySupernodeId ||
            leftSupernode == kInvalidActivitySupernodeId ||
            rightSupernode == kInvalidActivitySupernodeId ||
            rootSupernode == leftSupernode ||
            leftSupernode != rightSupernode)
        {
            return fail("Expected plain sibling coarsen to merge sibling consumers only");
        }
    }

    {
        currentCase = "mem_read";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("mem_read");
        design.markAsTop("mem_read");

        const auto addr = makeValue(graph, "addr", 2);
        const auto a = makeValue(graph, "a", 8);
        graph.bindInputPort("addr", addr);
        graph.bindInputPort("a", a);
        const auto memDecl = graph.createOperation(wolvrix::lib::grh::OperationKind::kMemory,
                                                   graph.internSymbol("m"));
        graph.setAttr(memDecl, "width", static_cast<int64_t>(8));
        graph.setAttr(memDecl, "row", static_cast<int64_t>(4));
        graph.setAttr(memDecl, "isSigned", false);
        const auto readValue = makeValue(graph, "r", 8);
        const auto readOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kMemoryReadPort,
                                                  graph.internSymbol("read"));
        graph.setAttr(readOp, "memSymbol", std::string("m"));
        graph.addOperand(readOp, addr);
        graph.addResult(readOp, readValue);
        const auto y = makeValue(graph, "y", 8);
        const auto xorOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kXor,
                                                 graph.internSymbol("xor"));
        graph.addOperand(xorOp, readValue);
        graph.addOperand(xorOp, a);
        graph.addResult(xorOp, y);
        graph.bindOutputPort("y", y);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(ActivityScheduleOptions{
            .path = "mem_read",
            .maxOpInComputeSupernode = 2,
        }));
        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected memory-read schedule to succeed");
        }
        const auto schedule = loadSchedule(session, "mem_read");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        std::size_t memoryReadCount = 0;
        std::size_t scheduledMemoryReadCount = 0;
        for (const auto opId : graph.operations())
        {
            if (graph.opKind(opId) == wolvrix::lib::grh::OperationKind::kMemoryReadPort)
            {
                ++memoryReadCount;
                if (opId.index > 0 &&
                    opId.index - 1 < schedule.opToSupernode->size() &&
                    (*schedule.opToSupernode)[opId.index - 1] != kInvalidActivitySupernodeId)
                {
                    ++scheduledMemoryReadCount;
                }
            }
        }
        if (memoryReadCount != 2)
        {
            return fail("Expected memory read to be cloned as source");
        }
        if (scheduledMemoryReadCount == 0)
        {
            return fail("Expected a memory read clone to be scheduled as compute op");
        }
        if (schedule.summaryStats->find("\"memory_read_activation_edges\":0") == std::string::npos)
        {
            return fail("Expected mem_read case to avoid cross-supernode memory-read propagation");
        }
    }

    {
        currentCase = "common_expr";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("common_expr");
        design.markAsTop("common_expr");

        const auto a = makeValue(graph, "a", 8);
        const auto b = makeValue(graph, "b", 8);
        const auto c = makeValue(graph, "c", 8);
        const auto d = makeValue(graph, "d", 8);
        graph.bindInputPort("a", a);
        graph.bindInputPort("b", b);
        graph.bindInputPort("c", c);
        graph.bindInputPort("d", d);
        const auto shared = makeValue(graph, "shared", 8);
        const auto sharedOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kAdd,
                                                    graph.internSymbol("shared_op"));
        graph.addOperand(sharedOp, a);
        graph.addOperand(sharedOp, b);
        graph.addResult(sharedOp, shared);
        const auto y0 = makeValue(graph, "y0", 8);
        const auto andOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kAnd,
                                                 graph.internSymbol("and"));
        graph.addOperand(andOp, shared);
        graph.addOperand(andOp, c);
        graph.addResult(andOp, y0);
        graph.bindOutputPort("y0", y0);
        const auto y1 = makeValue(graph, "y1", 8);
        const auto xorOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kXor,
                                                 graph.internSymbol("xor"));
        graph.addOperand(xorOp, shared);
        graph.addOperand(xorOp, d);
        graph.addResult(xorOp, y1);
        graph.bindOutputPort("y1", y1);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(ActivityScheduleOptions{
            .path = "common_expr",
            .maxOpInComputeSupernode = 1,
            .enableCoarsen = false,
        }));
        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected common expr schedule to succeed");
        }
        const auto schedule = loadSchedule(session, "common_expr");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        const uint32_t sharedSupernode = (*schedule.opToSupernode)[sharedOp.index - 1];
        const uint32_t andSupernode = (*schedule.opToSupernode)[andOp.index - 1];
        const uint32_t xorSupernode = (*schedule.opToSupernode)[xorOp.index - 1];
        if (sharedSupernode != andSupernode)
        {
            return fail("Expected safe shared expression to be owned by its earliest consumer");
        }
        if (!hasFanoutTo(*schedule.valueFanout, shared, xorSupernode))
        {
            return fail("Expected shared expression fanout to later consumer");
        }
        if (schedule.summaryStats->find("\"other_compute_activation_edges\":1") == std::string::npos)
        {
            return fail("Expected common_expr case to report one remaining compute propagation edge");
        }
        if (schedule.summaryStats->find("\"other_compute_multi_target_values\":0") == std::string::npos)
        {
            return fail("Expected common_expr case to avoid multi-target compute value after ownership selection");
        }
    }

    {
        currentCase = "shared_condition_feedback_cycle";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("shared_condition_feedback_cycle");
        design.markAsTop("shared_condition_feedback_cycle");

        const auto clk = makeValue(graph, "clk", 1);
        const auto en = makeValue(graph, "en", 1);
        const auto req = makeValue(graph, "req", 2);
        const auto flag = makeValue(graph, "flag", 1);
        const auto a = makeValue(graph, "a", 27);
        const auto b = makeValue(graph, "b", 27);
        const auto c = makeValue(graph, "c", 27);
        const auto fallback = makeValue(graph, "fallback", 27);
        const auto idx = makeValue(graph, "idx", 2);
        const auto dummy = makeValue(graph, "dummy", 27);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("en", en);
        graph.bindInputPort("req", req);
        graph.bindInputPort("flag", flag);
        graph.bindInputPort("a", a);
        graph.bindInputPort("b", b);
        graph.bindInputPort("c", c);
        graph.bindInputPort("fallback", fallback);
        graph.bindInputPort("idx", idx);
        graph.bindInputPort("dummy", dummy);

        const auto regDecl = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegister,
                                                   graph.internSymbol("q"));
        graph.setAttr(regDecl, "width", static_cast<int64_t>(27));
        graph.setAttr(regDecl, "isSigned", false);

        const auto two = makeValue(graph, "two", 2);
        const auto twoOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kConstant,
                                                 graph.internSymbol("two_const"));
        graph.addResult(twoOp, two);
        graph.setAttr(twoOp, "constValue", std::string("2'h2"));

        const auto mask = makeValue(graph, "mask", 27);
        const auto maskOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kConstant,
                                                  graph.internSymbol("mask_const"));
        graph.addResult(maskOp, mask);
        graph.setAttr(maskOp, "constValue", std::string("27'h7ffffff"));

        const auto onlyS2 = makeValue(graph, "only_s2", 1);
        const auto onlyS2Op = graph.createOperation(wolvrix::lib::grh::OperationKind::kEq,
                                                    graph.internSymbol("only_s2_eq"));
        graph.addOperand(onlyS2Op, req);
        graph.addOperand(onlyS2Op, two);
        graph.addResult(onlyS2Op, onlyS2);

        const auto cond = makeValue(graph, "write_cond", 1);
        const auto condOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kOr,
                                                  graph.internSymbol("write_cond_or"));
        graph.addOperand(condOp, onlyS2);
        graph.addOperand(condOp, flag);
        graph.addResult(condOp, cond);

        const auto gvpn = makeValue(graph, "gvpn", 27);
        const auto gvpnOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kMux,
                                                  graph.internSymbol("gvpn_mux"));
        graph.addOperand(gvpnOp, onlyS2);
        graph.addOperand(gvpnOp, a);
        graph.addOperand(gvpnOp, b);
        graph.addResult(gvpnOp, gvpn);

        const auto earlyUse = makeValue(graph, "early_use", 27);
        const auto earlyUseOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kXor,
                                                      graph.internSymbol("early_gvpn_use"));
        graph.addOperand(earlyUseOp, gvpn);
        graph.addOperand(earlyUseOp, dummy);
        graph.addResult(earlyUseOp, earlyUse);
        graph.bindOutputPort("early_use", earlyUse);

        const auto packed = makeValue(graph, "packed", 54);
        const auto packedOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kConcat,
                                                    graph.internSymbol("packed_concat"));
        graph.addOperand(packedOp, c);
        graph.addOperand(packedOp, gvpn);
        graph.addResult(packedOp, packed);

        const auto selected = makeValue(graph, "selected", 27);
        const auto selectedOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kSliceDynamic,
                                                      graph.internSymbol("selected_dynamic_slice"));
        graph.addOperand(selectedOp, packed);
        graph.addOperand(selectedOp, idx);
        graph.addResult(selectedOp, selected);
        graph.setAttr(selectedOp, "sliceWidth", static_cast<int64_t>(27));

        const auto rhs = makeValue(graph, "rhs", 27);
        const auto rhsOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kMux,
                                                 graph.internSymbol("rhs_mux"));
        graph.addOperand(rhsOp, cond);
        graph.addOperand(rhsOp, selected);
        graph.addOperand(rhsOp, fallback);
        graph.addResult(rhsOp, rhs);

        const auto writeOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                                                   graph.internSymbol("q_write"));
        graph.addOperand(writeOp, en);
        graph.addOperand(writeOp, rhs);
        graph.addOperand(writeOp, mask);
        graph.addOperand(writeOp, clk);
        graph.setAttr(writeOp, "regSymbol", std::string("q"));
        graph.setAttr(writeOp, "eventEdge", std::vector<std::string>{"posedge"});

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(ActivityScheduleOptions{
            .path = "shared_condition_feedback_cycle",
            .maxOpInComputeSupernode = 1,
            .enableCoarsen = false,
        }));
        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected shared-condition feedback case to schedule without compute-node cycle");
        }
        const auto schedule = loadSchedule(session, "shared_condition_feedback_cycle");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        if (!hasFanoutTo(*schedule.valueFanout, onlyS2, (*schedule.opToSupernode)[gvpnOp.index - 1]))
        {
            return fail("Expected shared condition to remain an explicit dependency of gvpn");
        }
        if (!hasFanoutTo(*schedule.valueFanout, gvpn, (*schedule.opToSupernode)[packedOp.index - 1]))
        {
            return fail("Expected gvpn to remain an explicit dependency of packed write path");
        }
    }

    {
        currentCase = "declared_value_local_compute";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("declared_value_local_compute");
        design.markAsTop("declared_value_local_compute");

        const auto a = makeValue(graph, "a", 8);
        const auto b = makeValue(graph, "b", 8);
        const auto c = makeValue(graph, "c", 8);
        graph.bindInputPort("a", a);
        graph.bindInputPort("b", b);
        graph.bindInputPort("c", c);

        const auto wireSym = graph.internSymbol("declared_wire");
        graph.addDeclaredSymbol(wireSym);
        const auto declaredWire = graph.createValue(wireSym, 8, false);
        const auto declaredProducer = graph.createOperation(wolvrix::lib::grh::OperationKind::kAdd,
                                                            graph.internSymbol("declared_producer"));
        graph.addOperand(declaredProducer, a);
        graph.addOperand(declaredProducer, b);
        graph.addResult(declaredProducer, declaredWire);

        const auto y = makeValue(graph, "y", 8);
        const auto consumer = graph.createOperation(wolvrix::lib::grh::OperationKind::kXor,
                                                    graph.internSymbol("declared_consumer"));
        graph.addOperand(consumer, declaredWire);
        graph.addOperand(consumer, c);
        graph.addResult(consumer, y);
        graph.bindOutputPort("y", y);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(ActivityScheduleOptions{
            .path = "declared_value_local_compute",
            .maxOpInComputeSupernode = 2,
            .enableCoarsen = false,
            .enableChainMerge = false,
        }));
        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected declared-value-local-compute schedule to succeed");
        }
        const auto schedule = loadSchedule(session, "declared_value_local_compute");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        const uint32_t producerSupernode = (*schedule.opToSupernode)[declaredProducer.index - 1];
        const uint32_t consumerSupernode = (*schedule.opToSupernode)[consumer.index - 1];
        if (producerSupernode == kInvalidActivitySupernodeId ||
            consumerSupernode == kInvalidActivitySupernodeId ||
            producerSupernode != consumerSupernode)
        {
            return fail("Expected declared compute value producer and single consumer to stay local");
        }
        if (hasFanoutTo(*schedule.valueFanout, declaredWire, consumerSupernode))
        {
            return fail("Expected local declared compute value to avoid cross-supernode fanout");
        }
        if (schedule.summaryStats->find("\"compute_compute_value_pairs\":0") == std::string::npos)
        {
            return fail("Expected declared_value_local_compute case to report zero compute->compute value pairs");
        }
    }

    {
        currentCase = "declared_value_compute_node_boundary";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("declared_value_compute_node_boundary");
        design.markAsTop("declared_value_compute_node_boundary");

        const auto a = makeValue(graph, "a", 8);
        const auto b = makeValue(graph, "b", 8);
        const auto c = makeValue(graph, "c", 8);
        graph.bindInputPort("a", a);
        graph.bindInputPort("b", b);
        graph.bindInputPort("c", c);

        const auto wireSym = graph.internSymbol("declared_wire");
        graph.addDeclaredSymbol(wireSym);
        const auto declaredWire = graph.createValue(wireSym, 8, false);
        const auto producer = graph.createOperation(wolvrix::lib::grh::OperationKind::kAdd,
                                                    graph.internSymbol("declared_producer"));
        graph.addOperand(producer, a);
        graph.addOperand(producer, b);
        graph.addResult(producer, declaredWire);

        const auto y = makeValue(graph, "y", 8);
        const auto consumer = graph.createOperation(wolvrix::lib::grh::OperationKind::kXor,
                                                    graph.internSymbol("declared_consumer"));
        graph.addOperand(consumer, declaredWire);
        graph.addOperand(consumer, c);
        graph.addResult(consumer, y);
        graph.bindOutputPort("y", y);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        ActivityScheduleOptions options;
        options.path = "declared_value_compute_node_boundary";
        options.maxOpInComputeSupernode = 1;
        options.maxOpInComputeNode = 8;
        options.enableCoarsen = false;
        options.enableChainMerge = false;
        options.declaredValueComputeNodeBoundary = true;
        manager.addPass(std::make_unique<ActivitySchedulePass>(options));

        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected declared-value compute-node boundary schedule to succeed");
        }
        const auto schedule = loadSchedule(session, "declared_value_compute_node_boundary");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        const uint32_t producerSupernode = (*schedule.opToSupernode)[producer.index - 1];
        const uint32_t consumerSupernode = (*schedule.opToSupernode)[consumer.index - 1];
        if (producerSupernode == kInvalidActivitySupernodeId ||
            consumerSupernode == kInvalidActivitySupernodeId ||
            producerSupernode == consumerSupernode)
        {
            return fail("Expected declared value producer and consumer to be separated when boundary option is enabled");
        }
        if (!hasFanoutTo(*schedule.valueFanout, declaredWire, consumerSupernode))
        {
            return fail("Expected declared value to become a cross-supernode fanout");
        }
        if (parseJsonDoubleField(*schedule.summaryStats, "compute_node_boundary_input_declared") != 1.0 ||
            parseJsonDoubleField(*schedule.summaryStats, "compute_node_boundary_declared_values") != 1.0 ||
            parseJsonDoubleField(*schedule.summaryStats, "compute_node_boundary_declared_edges") != 1.0)
        {
            return fail("Expected declared boundary stats to be recorded: " + *schedule.summaryStats);
        }
    }

    {
        currentCase = "declared_source_clone_boundary";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("declared_source_clone_boundary");
        design.markAsTop("declared_source_clone_boundary");
        using K = wolvrix::lib::grh::OperationKind;

        const auto reg = graph.createOperation(K::kRegister, graph.internSymbol("q"));
        graph.setAttr(reg, "width", int64_t{8});
        graph.setAttr(reg, "isSigned", false);

        const auto readSym = graph.internSymbol("q_read");
        graph.addDeclaredSymbol(readSym);
        const auto qRead = graph.createValue(readSym, 8, false);
        const auto readOp = graph.createOperation(K::kRegisterReadPort, graph.internSymbol("q_read_op"));
        graph.addResult(readOp, qRead);
        graph.setAttr(readOp, "regSymbol", std::string("q"));

        const auto a = makeValue(graph, "a", 8);
        graph.bindInputPort("a", a);

        const auto y = makeValue(graph, "y", 8);
        const auto consumer = graph.createOperation(K::kXor, graph.internSymbol("consumer"));
        graph.addOperand(consumer, qRead);
        graph.addOperand(consumer, a);
        graph.addResult(consumer, y);
        graph.bindOutputPort("y", y);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        ActivityScheduleOptions options;
        options.path = "declared_source_clone_boundary";
        options.maxOpInComputeSupernode = 1;
        options.maxOpInComputeNode = 8;
        options.enableCoarsen = false;
        options.enableChainMerge = false;
        options.declaredValueComputeNodeBoundary = true;
        manager.addPass(std::make_unique<ActivitySchedulePass>(options));

        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected declared source-clone boundary schedule to succeed");
        }
        const auto schedule = loadSchedule(session, "declared_source_clone_boundary");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }

        const auto consumerOperands = graph.opOperands(consumer);
        if (consumerOperands.empty() || consumerOperands.front() == qRead)
        {
            return fail("Expected register-read source use to be cloned before compute");
        }
        const auto clonedReadValue = consumerOperands.front();
        const auto clonedReadOp = graph.valueDef(clonedReadValue);
        if (!clonedReadOp.valid() ||
            graph.opKind(clonedReadOp) != K::kRegisterReadPort)
        {
            return fail("Expected cloned source value to be defined by a register read");
        }
        const uint32_t readSupernode = (*schedule.opToSupernode)[clonedReadOp.index - 1];
        const uint32_t consumerSupernode = (*schedule.opToSupernode)[consumer.index - 1];
        if (readSupernode == kInvalidActivitySupernodeId ||
            consumerSupernode == kInvalidActivitySupernodeId ||
            readSupernode == consumerSupernode)
        {
            return fail("Expected declared canonical source clone to be a compute-node boundary");
        }
        if (!hasFanoutTo(*schedule.valueFanout, clonedReadValue, consumerSupernode))
        {
            return fail("Expected cloned declared source value to fan out to consumer supernode");
        }
        if (parseJsonDoubleField(*schedule.summaryStats, "compute_node_boundary_input_declared") != 1.0)
        {
            return fail("Expected declared source clone boundary stat: " + *schedule.summaryStats);
        }
    }

    {
        currentCase = "coarsen_respects_compute_supernode_op_limit";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("coarsen_respects_compute_supernode_op_limit");
        design.markAsTop("coarsen_respects_compute_supernode_op_limit");

        const auto a = makeValue(graph, "a", 8);
        const auto b = makeValue(graph, "b", 8);
        graph.bindInputPort("a", a);
        graph.bindInputPort("b", b);

        std::vector<wolvrix::lib::grh::ValueId> leaves;
        leaves.reserve(10);
        for (int i = 0; i < 10; ++i)
        {
            const auto result = makeValue(graph, "leaf_" + std::to_string(i), 8);
            const auto op = graph.createOperation(wolvrix::lib::grh::OperationKind::kXor,
                                                  graph.internSymbol("leaf_op_" + std::to_string(i)));
            graph.addOperand(op, a);
            graph.addOperand(op, b);
            graph.addResult(op, result);
            leaves.push_back(result);
        }

        wolvrix::lib::grh::ValueId cursor = leaves.front();
        for (std::size_t i = 1; i < leaves.size(); ++i)
        {
            const auto result = makeValue(graph, "reduce_" + std::to_string(i), 8);
            const auto op = graph.createOperation(wolvrix::lib::grh::OperationKind::kOr,
                                                  graph.internSymbol("reduce_op_" + std::to_string(i)));
            graph.addOperand(op, cursor);
            graph.addOperand(op, leaves[i]);
            graph.addResult(op, result);
            cursor = result;
        }
        graph.bindOutputPort("y", cursor);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(ActivityScheduleOptions{
            .path = "coarsen_respects_compute_supernode_op_limit",
            .maxOpInComputeSupernode = 3,
            .maxOpInComputeNode = 1,
            .enableCoarsen = true,
            .enableChainMerge = true,
        }));
        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected coarsen op-limit schedule to succeed");
        }
        const auto schedule = loadSchedule(session, "coarsen_respects_compute_supernode_op_limit");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        for (uint32_t supernodeId = 0; supernodeId < schedule.supernodeToOps->size(); ++supernodeId)
        {
            if ((*schedule.supernodeKinds)[supernodeId] == ActivityScheduleSupernodeKind::Compute &&
                (*schedule.supernodeToOps)[supernodeId].size() > 3)
            {
                return fail("Expected coarsened compute supernodes to obey maxOpInComputeSupernode");
            }
        }
    }

    {
        currentCase = "split_oversize_compute_node";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("split_oversize_compute_node");
        design.markAsTop("split_oversize_compute_node");

        const auto a = makeValue(graph, "a", 8);
        const auto b = makeValue(graph, "b", 8);
        graph.bindInputPort("a", a);
        graph.bindInputPort("b", b);

        std::vector<wolvrix::lib::grh::OperationId> ops;
        wolvrix::lib::grh::ValueId cursor = a;
        for (int i = 0; i < 5; ++i)
        {
            const auto result = makeValue(graph, "chain_" + std::to_string(i), 8);
            const auto op = graph.createOperation(wolvrix::lib::grh::OperationKind::kXor,
                                                  graph.internSymbol("chain_op_" + std::to_string(i)));
            graph.addOperand(op, cursor);
            graph.addOperand(op, b);
            graph.addResult(op, result);
            ops.push_back(op);
            cursor = result;
        }
        graph.bindOutputPort("y", cursor);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(ActivityScheduleOptions{
            .path = "split_oversize_compute_node",
            .maxOpInComputeSupernode = 2,
            .maxOpInComputeNode = 16,
            .enableCoarsen = false,
            .splitOversizeComputeNodes = true,
        }));
        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected oversize compute-node split schedule to succeed");
        }
        const auto schedule = loadSchedule(session, "split_oversize_compute_node");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        const uint32_t firstSupernode = (*schedule.opToSupernode)[ops.front().index - 1];
        const uint32_t lastSupernode = (*schedule.opToSupernode)[ops.back().index - 1];
        if (firstSupernode == kInvalidActivitySupernodeId ||
            lastSupernode == kInvalidActivitySupernodeId ||
            firstSupernode == lastSupernode)
        {
            return fail("Expected oversize compute node to split into multiple final supernodes");
        }
        bool splitReachable = false;
        if (firstSupernode < schedule.dag->size())
        {
            std::vector<uint32_t> stack{firstSupernode};
            std::vector<uint8_t> seen(schedule.dag->size(), 0);
            seen[firstSupernode] = 1;
            while (!stack.empty())
            {
                const uint32_t node = stack.back();
                stack.pop_back();
                if (node == lastSupernode)
                {
                    splitReachable = true;
                    break;
                }
                for (const uint32_t succ : (*schedule.dag)[node])
                {
                    if (succ < seen.size() && seen[succ] == 0)
                    {
                        seen[succ] = 1;
                        stack.push_back(succ);
                    }
                }
            }
        }
        if (!splitReachable)
        {
            return fail("Expected split chunks from the same compute node to stay reachable in DAG");
        }
        for (const auto &supernodeOps : *schedule.supernodeToOps)
        {
            if (supernodeOps.size() > 2)
            {
                return fail("Expected split final compute supernodes to obey maxOpInComputeSupernode");
            }
        }
    }

    {
        currentCase = "commit_chunk";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("commit_chunk");
        design.markAsTop("commit_chunk");

        const auto clk = makeValue(graph, "clk", 1);
        const auto en = makeValue(graph, "en", 1);
        const auto mask = makeValue(graph, "mask", 8);
        const auto d0 = makeValue(graph, "d0", 8);
        const auto d1 = makeValue(graph, "d1", 8);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("en", en);
        graph.bindInputPort("mask", mask);
        graph.bindInputPort("d0", d0);
        graph.bindInputPort("d1", d1);
        for (const char *name : {"q0", "q1"})
        {
            const auto reg = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegister,
                                                   graph.internSymbol(name));
            graph.setAttr(reg, "width", static_cast<int64_t>(8));
            graph.setAttr(reg, "isSigned", false);
        }
        const auto write0 = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                                                  graph.internSymbol("w0"));
        graph.addOperand(write0, en);
        graph.addOperand(write0, d0);
        graph.addOperand(write0, mask);
        graph.addOperand(write0, clk);
        graph.setAttr(write0, "regSymbol", std::string("q0"));
        graph.setAttr(write0, "eventEdge", std::vector<std::string>{"posedge"});
        const auto write1 = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                                                  graph.internSymbol("w1"));
        graph.addOperand(write1, en);
        graph.addOperand(write1, d1);
        graph.addOperand(write1, mask);
        graph.addOperand(write1, clk);
        graph.setAttr(write1, "regSymbol", std::string("q1"));
        graph.setAttr(write1, "eventEdge", std::vector<std::string>{"posedge"});

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(ActivityScheduleOptions{
            .path = "commit_chunk",
            .maxOpInComputeSupernode = 1,
            .maxOpInCommitSupernode = 1,
            .enableCoarsen = false,
            .commitGuardEventBuckets = false,
        }));
        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected commit chunk schedule to succeed");
        }
        const auto schedule = loadSchedule(session, "commit_chunk");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        if ((*schedule.opToSupernode)[write0.index - 1] == (*schedule.opToSupernode)[write1.index - 1])
        {
            return fail("Expected maxOpInCommitSupernode to split commit supernodes");
        }
    }

    {
        currentCase = "commit_guard_event_bucket";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("commit_guard_event_bucket");
        design.markAsTop("commit_guard_event_bucket");

        const auto clk = makeValue(graph, "clk", 1);
        const auto en = makeValue(graph, "en", 1);
        const auto otherEn = makeValue(graph, "other_en", 1);
        const auto thirdEn = makeValue(graph, "third_en", 1);
        const auto mask = makeValue(graph, "mask", 8);
        const auto d0 = makeValue(graph, "d0", 8);
        const auto d1 = makeValue(graph, "d1", 8);
        const auto d2 = makeValue(graph, "d2", 8);
        const auto d3 = makeValue(graph, "d3", 8);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("en", en);
        graph.bindInputPort("other_en", otherEn);
        graph.bindInputPort("third_en", thirdEn);
        graph.bindInputPort("mask", mask);
        graph.bindInputPort("d0", d0);
        graph.bindInputPort("d1", d1);
        graph.bindInputPort("d2", d2);
        graph.bindInputPort("d3", d3);
        for (const char *name : {"q0", "q1", "q2", "q3"})
        {
            const auto reg = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegister,
                                                   graph.internSymbol(name));
            graph.setAttr(reg, "width", static_cast<int64_t>(8));
            graph.setAttr(reg, "isSigned", false);
        }

        const auto write0 = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                                                  graph.internSymbol("w0"));
        graph.addOperand(write0, en);
        graph.addOperand(write0, d0);
        graph.addOperand(write0, mask);
        graph.addOperand(write0, clk);
        graph.setAttr(write0, "regSymbol", std::string("q0"));
        graph.setAttr(write0, "eventEdge", std::vector<std::string>{"posedge"});
        const auto write1 = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                                                  graph.internSymbol("w1"));
        graph.addOperand(write1, otherEn);
        graph.addOperand(write1, d1);
        graph.addOperand(write1, mask);
        graph.addOperand(write1, clk);
        graph.setAttr(write1, "regSymbol", std::string("q1"));
        graph.setAttr(write1, "eventEdge", std::vector<std::string>{"posedge"});
        const auto write2 = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                                                  graph.internSymbol("w2"));
        graph.addOperand(write2, en);
        graph.addOperand(write2, d2);
        graph.addOperand(write2, mask);
        graph.addOperand(write2, clk);
        graph.setAttr(write2, "regSymbol", std::string("q2"));
        graph.setAttr(write2, "eventEdge", std::vector<std::string>{"posedge"});
        const auto write3 = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                                                  graph.internSymbol("w3"));
        graph.addOperand(write3, thirdEn);
        graph.addOperand(write3, d3);
        graph.addOperand(write3, mask);
        graph.addOperand(write3, clk);
        graph.setAttr(write3, "regSymbol", std::string("q3"));
        graph.setAttr(write3, "eventEdge", std::vector<std::string>{"posedge"});

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(ActivityScheduleOptions{
            .path = "commit_guard_event_bucket",
            .maxOpInComputeSupernode = 1,
            .maxOpInCommitSupernode = 3,
            .enableCoarsen = false,
        }));
        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected commit guard event bucket schedule to succeed");
        }
        const auto schedule = loadSchedule(session, "commit_guard_event_bucket");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        const uint32_t write0Supernode = (*schedule.opToSupernode)[write0.index - 1];
        const uint32_t write1Supernode = (*schedule.opToSupernode)[write1.index - 1];
        const uint32_t write2Supernode = (*schedule.opToSupernode)[write2.index - 1];
        const uint32_t write3Supernode = (*schedule.opToSupernode)[write3.index - 1];
        if (write0Supernode != write1Supernode || write0Supernode != write2Supernode)
        {
            return fail("Expected guard buckets to merge while total commit ops stay under cap");
        }
        if (write0Supernode == write3Supernode)
        {
            return fail("Expected guard bucket packing to split before exceeding commit op cap");
        }
        const auto &commitOps = (*schedule.supernodeToOps)[write0Supernode];
        const auto write0It = std::find(commitOps.begin(), commitOps.end(), write0);
        const auto write1It = std::find(commitOps.begin(), commitOps.end(), write1);
        const auto write2It = std::find(commitOps.begin(), commitOps.end(), write2);
        if (write0It == commitOps.end() || write1It == commitOps.end() || write2It == commitOps.end())
        {
            return fail("Expected commit supernode to contain all test writes");
        }
        const auto write0Pos = std::distance(commitOps.begin(), write0It);
        const auto write1Pos = std::distance(commitOps.begin(), write1It);
        const auto write2Pos = std::distance(commitOps.begin(), write2It);
        if (write2Pos != write0Pos + 1 || write1Pos == write0Pos + 1)
        {
            return fail("Expected same-guard writes to stay adjacent inside the event commit supernode");
        }
    }

    {
        currentCase = "commit_guard_event_order_preserving_high_cap";
        constexpr std::string_view kGraphName = "commit_guard_event_order_preserving_high_cap";
        constexpr std::size_t kFirstGuardWrites = 1100;
        constexpr std::size_t kSecondGuardWrites = 2900;
        constexpr std::size_t kThirdGuardWrites = 2000;
        constexpr std::size_t kFourthGuardWrites = 3000;
        constexpr std::size_t kTotalWrites =
            kFirstGuardWrites + kSecondGuardWrites + kThirdGuardWrites + kFourthGuardWrites;
        constexpr std::size_t kHighCommitCap = 6144;

        struct Fixture
        {
            wolvrix::lib::grh::ValueId sharedCommitInput;
            wolvrix::lib::grh::ValueId tailCommitInput;
            std::vector<wolvrix::lib::grh::OperationId> writes;
        };

        const auto buildFixture = [&](wolvrix::lib::grh::Design &design)
        {
            auto &graph = design.createGraph(std::string(kGraphName));
            design.markAsTop(std::string(kGraphName));

            const auto clk = makeValue(graph, "clk", 1);
            const auto guard0 = makeValue(graph, "guard0", 1);
            const auto guard1 = makeValue(graph, "guard1", 1);
            const auto guard2 = makeValue(graph, "guard2", 1);
            const auto guard3 = makeValue(graph, "guard3", 1);
            const auto mask = makeValue(graph, "mask", 8);
            const auto lhs = makeValue(graph, "lhs", 8);
            const auto rhs = makeValue(graph, "rhs", 8);
            const auto tailData = makeValue(graph, "tail_data", 8);
            const auto sharedCommitInput = makeValue(graph, "shared_commit_input", 8);
            graph.bindInputPort("clk", clk);
            graph.bindInputPort("guard0", guard0);
            graph.bindInputPort("guard1", guard1);
            graph.bindInputPort("guard2", guard2);
            graph.bindInputPort("guard3", guard3);
            graph.bindInputPort("mask", mask);
            graph.bindInputPort("lhs", lhs);
            graph.bindInputPort("rhs", rhs);

            const auto xored = graph.createOperation(wolvrix::lib::grh::OperationKind::kXor,
                                                     graph.internSymbol("tail_xor"));
            graph.addOperand(xored, lhs);
            graph.addOperand(xored, rhs);
            graph.addResult(xored, tailData);
            const auto add = graph.createOperation(wolvrix::lib::grh::OperationKind::kAdd,
                                                   graph.internSymbol("shared_add"));
            graph.addOperand(add, lhs);
            graph.addOperand(add, rhs);
            graph.addResult(add, sharedCommitInput);

            Fixture fixture{
                .sharedCommitInput = sharedCommitInput,
                .tailCommitInput = tailData,
            };
            fixture.writes.reserve(kTotalWrites);
            for (std::size_t index = 0; index < kTotalWrites; ++index)
            {
                const std::string suffix = std::to_string(index);
                const std::string regName = "q" + suffix;
                const auto reg = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegister,
                                                       graph.internSymbol(regName));
                graph.setAttr(reg, "width", static_cast<int64_t>(8));
                graph.setAttr(reg, "isSigned", false);

                const auto write = graph.createOperation(
                    wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                    graph.internSymbol("w" + suffix));
                wolvrix::lib::grh::ValueId guard = guard0;
                if (index >= kFirstGuardWrites + kSecondGuardWrites + kThirdGuardWrites)
                {
                    guard = guard3;
                }
                else if (index >= kFirstGuardWrites + kSecondGuardWrites)
                {
                    guard = guard2;
                }
                else if (index >= kFirstGuardWrites)
                {
                    guard = guard1;
                }
                graph.addOperand(write, guard);
                graph.addOperand(write,
                                 index < kFirstGuardWrites + kSecondGuardWrites
                                     ? sharedCommitInput
                                     : tailData);
                graph.addOperand(write, mask);
                graph.addOperand(write, clk);
                graph.setAttr(write, "regSymbol", regName);
                graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});
                fixture.writes.push_back(write);
            }
            return fixture;
        };

        const auto runFixture = [&](wolvrix::lib::grh::Design &design,
                                    SessionStore &session,
                                    std::optional<std::size_t> commitCap)
        {
            PassManager manager;
            manager.options().session = &session;
            ActivityScheduleOptions options{
                .path = std::string(kGraphName),
                .maxOpInComputeSupernode = 8,
                .enableCoarsen = false,
            };
            if (commitCap)
            {
                options.maxOpInCommitSupernode = *commitCap;
            }
            manager.addPass(std::make_unique<ActivitySchedulePass>(std::move(options)));
            PassDiagnostics diags;
            const PassManagerResult runResult = manager.run(design, diags);
            return runResult.success && !diags.hasError();
        };

        wolvrix::lib::grh::Design defaultDesign;
        wolvrix::lib::grh::Design explicit4096Design;
        wolvrix::lib::grh::Design highCapDesign;
        const Fixture defaultFixture = buildFixture(defaultDesign);
        const Fixture explicit4096Fixture = buildFixture(explicit4096Design);
        const Fixture highCapFixture = buildFixture(highCapDesign);
        SessionStore defaultSession;
        SessionStore explicit4096Session;
        SessionStore highCapSession;
        if (!runFixture(defaultDesign, defaultSession, std::nullopt) ||
            !runFixture(explicit4096Design, explicit4096Session, 4096) ||
            !runFixture(highCapDesign, highCapSession, kHighCommitCap))
        {
            return fail("Expected order-preserving high commit cap fixtures to schedule");
        }

        const auto defaultSchedule = loadSchedule(defaultSession, std::string(kGraphName));
        const auto explicit4096Schedule = loadSchedule(explicit4096Session, std::string(kGraphName));
        const auto highCapSchedule = loadSchedule(highCapSession, std::string(kGraphName));
        const auto *defaultGraph = defaultDesign.findGraph(std::string(kGraphName));
        const auto *explicit4096Graph = explicit4096Design.findGraph(std::string(kGraphName));
        const auto *highCapGraph = highCapDesign.findGraph(std::string(kGraphName));
        if (defaultGraph == nullptr || explicit4096Graph == nullptr || highCapGraph == nullptr ||
            validateCommonScheduleShape(*defaultGraph, defaultSchedule) != 0 ||
            validateCommonScheduleShape(*explicit4096Graph, explicit4096Schedule) != 0 ||
            validateCommonScheduleShape(*highCapGraph, highCapSchedule) != 0)
        {
            return 1;
        }
        if (!schedulesEqual(defaultSchedule, explicit4096Schedule))
        {
            return fail("Expected default and explicit 4096 commit caps to be byte-identical");
        }

        const auto commitSupernodes = [](const ScheduleView &schedule)
        {
            std::vector<uint32_t> out;
            for (uint32_t supernode = 0; supernode < schedule.supernodeKinds->size(); ++supernode)
            {
                if ((*schedule.supernodeKinds)[supernode] == ActivityScheduleSupernodeKind::Commit)
                {
                    out.push_back(supernode);
                }
            }
            return out;
        };
        const auto explicit4096Commit = commitSupernodes(explicit4096Schedule);
        const auto highCapCommit = commitSupernodes(highCapSchedule);
        if (explicit4096Commit.size() != 3 || highCapCommit.size() != 2)
        {
            return fail("Expected 6144 cap to coarsen only two complete 4096-baseline commit nodes");
        }

        const auto writeOrdinals = [](const std::vector<wolvrix::lib::grh::OperationId> &ops,
                                      const std::vector<wolvrix::lib::grh::OperationId> &writes)
        {
            std::size_t maxOpIndex = 0;
            for (const auto write : writes)
            {
                maxOpIndex = std::max<std::size_t>(maxOpIndex, write.index);
            }
            std::vector<uint32_t> ordinalByOpIndex(maxOpIndex + 1,
                                                   kInvalidActivitySupernodeId);
            for (uint32_t ordinal = 0; ordinal < writes.size(); ++ordinal)
            {
                ordinalByOpIndex[writes[ordinal].index] = ordinal;
            }
            std::vector<uint32_t> out;
            out.reserve(ops.size());
            for (const auto opId : ops)
            {
                if (opId.index >= ordinalByOpIndex.size() ||
                    ordinalByOpIndex[opId.index] == kInvalidActivitySupernodeId)
                {
                    return std::vector<uint32_t>{};
                }
                out.push_back(ordinalByOpIndex[opId.index]);
            }
            return out;
        };
        const auto ordinalClusters = [&](const ScheduleView &schedule,
                                         const std::vector<uint32_t> &commitNodes,
                                         const Fixture &fixture)
        {
            std::vector<std::vector<uint32_t>> out;
            out.reserve(commitNodes.size());
            for (const uint32_t supernode : commitNodes)
            {
                out.push_back(writeOrdinals((*schedule.supernodeToOps)[supernode],
                                            fixture.writes));
            }
            return out;
        };
        const auto flattenOrdinals = [](const std::vector<std::vector<uint32_t>> &clusters)
        {
            std::vector<uint32_t> out;
            for (const auto &cluster : clusters)
            {
                out.insert(out.end(), cluster.begin(), cluster.end());
            }
            return out;
        };
        const auto baselineOrdinalClusters = ordinalClusters(explicit4096Schedule,
                                                              explicit4096Commit,
                                                              explicit4096Fixture);
        const auto highCapOrdinalClusters = ordinalClusters(highCapSchedule,
                                                             highCapCommit,
                                                             highCapFixture);
        const auto baselineWriteOrdinals = flattenOrdinals(baselineOrdinalClusters);
        const auto highCapWriteOrdinals = flattenOrdinals(highCapOrdinalClusters);
        if (baselineWriteOrdinals.size() != kTotalWrites ||
            baselineWriteOrdinals != highCapWriteOrdinals)
        {
            return fail("High commit cap changed sink membership or execution order");
        }

        const auto packWholeClusters = [&](const std::vector<std::vector<uint32_t>> &input)
        {
            std::vector<std::vector<uint32_t>> out;
            std::vector<uint32_t> mergedCluster;
            const auto flushMergedCluster = [&]()
            {
                if (!mergedCluster.empty())
                {
                    out.push_back(std::move(mergedCluster));
                    mergedCluster = {};
                }
            };
            for (const auto &cluster : input)
            {
                if (cluster.size() > kHighCommitCap)
                {
                    flushMergedCluster();
                    out.push_back(cluster);
                    continue;
                }
                if (!mergedCluster.empty() &&
                    mergedCluster.size() + cluster.size() > kHighCommitCap)
                {
                    flushMergedCluster();
                }
                mergedCluster.insert(mergedCluster.end(), cluster.begin(), cluster.end());
            }
            flushMergedCluster();
            return out;
        };
        const auto expectedHighCapClusters = packWholeClusters(baselineOrdinalClusters);

        const auto guardGroup = [&](uint32_t ordinal)
        {
            if (ordinal < kFirstGuardWrites)
            {
                return uint32_t{0};
            }
            if (ordinal < kFirstGuardWrites + kSecondGuardWrites)
            {
                return uint32_t{1};
            }
            if (ordinal < kFirstGuardWrites + kSecondGuardWrites + kThirdGuardWrites)
            {
                return uint32_t{2};
            }
            return uint32_t{3};
        };
        std::vector<std::vector<uint32_t>> atomicGuardClusters;
        uint32_t previousGuard = std::numeric_limits<uint32_t>::max();
        for (const uint32_t ordinal : baselineWriteOrdinals)
        {
            const uint32_t currentGuard = guardGroup(ordinal);
            if (atomicGuardClusters.empty() || currentGuard != previousGuard)
            {
                atomicGuardClusters.emplace_back();
                previousGuard = currentGuard;
            }
            atomicGuardClusters.back().push_back(ordinal);
        }
        const auto directHighCapClusters = packWholeClusters(atomicGuardClusters);
        if (expectedHighCapClusters.size() >= baselineOrdinalClusters.size() ||
            highCapOrdinalClusters != expectedHighCapClusters ||
            atomicGuardClusters.size() != 4 ||
            directHighCapClusters == expectedHighCapClusters)
        {
            return fail("High commit cap did not preserve complete 4096-baseline node boundaries");
        }

        const auto commitTargets = [](const ScheduleView &schedule,
                                      wolvrix::lib::grh::ValueId value)
        {
            std::vector<uint32_t> out;
            if (value.index == 0 || value.index > schedule.valueFanout->size())
            {
                return out;
            }
            for (const uint32_t target : (*schedule.valueFanout)[value.index - 1])
            {
                if (target < schedule.supernodeKinds->size() &&
                    (*schedule.supernodeKinds)[target] == ActivityScheduleSupernodeKind::Commit)
                {
                    out.push_back(target);
                }
            }
            return out;
        };
        const auto baselineSharedTargets =
            commitTargets(explicit4096Schedule, explicit4096Fixture.sharedCommitInput);
        const auto baselineTailTargets =
            commitTargets(explicit4096Schedule, explicit4096Fixture.tailCommitInput);
        const auto highCapSharedTargets =
            commitTargets(highCapSchedule, highCapFixture.sharedCommitInput);
        const auto highCapTailTargets =
            commitTargets(highCapSchedule, highCapFixture.tailCommitInput);
        if (baselineSharedTargets.size() != 1 || baselineTailTargets.size() != 2 ||
            highCapSharedTargets.size() != 1 || highCapTailTargets.size() != 1)
        {
            return fail("Expected repeated compute input commit targets to coarsen from three to two");
        }

        std::vector<uint32_t> expectedInputValues;
        for (const auto write : highCapFixture.writes)
        {
            for (const auto input : highCapGraph->opOperands(write))
            {
                if (std::find(expectedInputValues.begin(), expectedInputValues.end(), input.index) ==
                    expectedInputValues.end())
                {
                    expectedInputValues.push_back(input.index);
                }
            }
        }
        std::sort(expectedInputValues.begin(), expectedInputValues.end());
        std::vector<uint32_t> definedInputValues;
        for (std::size_t valueIndex = 0; valueIndex < highCapSchedule.valueFanout->size(); ++valueIndex)
        {
            const auto &targets = (*highCapSchedule.valueFanout)[valueIndex];
            const bool reachesCommit = std::any_of(
                targets.begin(),
                targets.end(),
                [&](uint32_t target)
                {
                    return std::find(highCapCommit.begin(), highCapCommit.end(), target) !=
                           highCapCommit.end();
                });
            if (reachesCommit)
            {
                definedInputValues.push_back(static_cast<uint32_t>(valueIndex + 1));
            }
        }
        std::vector<uint32_t> expectedDefinedInputValues{
            highCapFixture.sharedCommitInput.index,
            highCapFixture.tailCommitInput.index,
        };
        std::sort(expectedDefinedInputValues.begin(), expectedDefinedInputValues.end());
        if (expectedInputValues.size() != 8 ||
            definedInputValues != expectedDefinedInputValues)
        {
            return fail("High commit cap did not preserve the exact commit input union");
        }

        const double baselineComputeCommitPairs = parseJsonDoubleField(
            *explicit4096Schedule.summaryStats,
            "compute_commit_value_pairs");
        const double highCapComputeCommitPairs = parseJsonDoubleField(
            *highCapSchedule.summaryStats,
            "compute_commit_value_pairs");
        if (baselineComputeCommitPairs != 3.0 || highCapComputeCommitPairs != 2.0 ||
            parseJsonDoubleField(*explicit4096Schedule.summaryStats, "commit_input_root_values") != 13.0 ||
            parseJsonDoubleField(*highCapSchedule.summaryStats, "commit_input_root_values") != 10.0)
        {
            return fail("Expected high commit cap to union repeated commit input pairs exactly");
        }
        if (parseJsonDoubleField(*explicit4096Schedule.summaryStats, "commit_event_key_runs") != 3.0 ||
            parseJsonDoubleField(*highCapSchedule.summaryStats, "commit_event_key_runs") != 2.0 ||
            parseJsonDoubleField(*highCapSchedule.summaryStats, "commit_event_keys") != 1.0 ||
            parseJsonDoubleField(*highCapSchedule.summaryStats, "commit_sink_ops") !=
                static_cast<double>(kTotalWrites))
        {
            return fail("Expected high commit cap summary to report two order-preserving event runs");
        }
        (void)defaultFixture;
    }

    {
        currentCase = "ordered_memory_write_atomic_chunk";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("ordered_memory_write_atomic_chunk");
        design.markAsTop("ordered_memory_write_atomic_chunk");

        const auto clk = makeValue(graph, "clk", 1);
        const auto en = makeValue(graph, "en", 1);
        const auto addr = makeValue(graph, "addr", 2);
        const auto mask = makeValue(graph, "mask", 8);
        const auto highData = makeValue(graph, "high_data", 8);
        const auto lowData = makeValue(graph, "low_data", 8);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("en", en);
        graph.bindInputPort("addr", addr);
        graph.bindInputPort("mask", mask);
        graph.bindInputPort("high_data", highData);
        graph.bindInputPort("low_data", lowData);

        const auto memory = graph.createOperation(wolvrix::lib::grh::OperationKind::kMemory,
                                                  graph.internSymbol("mem"));
        graph.setAttr(memory, "width", int64_t{8});
        graph.setAttr(memory, "row", int64_t{4});
        graph.setAttr(memory, "isSigned", false);

        const auto addWrite = [&](std::string_view name,
                                  wolvrix::lib::grh::ValueId data,
                                  int64_t priority)
        {
            const auto write = graph.createOperation(wolvrix::lib::grh::OperationKind::kMemoryWritePort,
                                                     graph.internSymbol(name));
            graph.addOperand(write, en);
            graph.addOperand(write, addr);
            graph.addOperand(write, data);
            graph.addOperand(write, mask);
            graph.addOperand(write, clk);
            graph.setAttr(write, "memSymbol", std::string("mem"));
            graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});
            graph.setAttr(write,
                          wolvrix::lib::grh::kMemoryWritePriorityGroupAttr,
                          std::string("mem_writes"));
            graph.setAttr(write, wolvrix::lib::grh::kMemoryWritePriorityAttr, priority);
            return write;
        };
        const auto highWrite = addWrite("high_write", highData, 0);
        const auto lowWrite = addWrite("low_write", lowData, 1);

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(ActivityScheduleOptions{
            .path = "ordered_memory_write_atomic_chunk",
            .maxOpInComputeSupernode = 1,
            .maxOpInCommitSupernode = 1,
            .enableCoarsen = false,
            .commitGuardEventBuckets = false,
        }));
        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected ordered memory write schedule to succeed");
        }
        const auto schedule = loadSchedule(session, "ordered_memory_write_atomic_chunk");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        const uint32_t highSupernode = (*schedule.opToSupernode)[highWrite.index - 1];
        const uint32_t lowSupernode = (*schedule.opToSupernode)[lowWrite.index - 1];
        if (highSupernode != lowSupernode)
        {
            return fail("Ordered memory write group was split by the commit chunk limit");
        }
        const auto &commitOps = (*schedule.supernodeToOps)[highSupernode];
        const auto lowIt = std::find(commitOps.begin(), commitOps.end(), lowWrite);
        const auto highIt = std::find(commitOps.begin(), commitOps.end(), highWrite);
        if (lowIt == commitOps.end() || highIt == commitOps.end() || lowIt >= highIt)
        {
            return fail("Ordered memory writes were not scheduled from low to high priority");
        }
    }

    {
        currentCase = "commit_guard_event_oversize_bucket";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("commit_guard_event_oversize_bucket");
        design.markAsTop("commit_guard_event_oversize_bucket");

        const auto clk = makeValue(graph, "clk", 1);
        const auto en = makeValue(graph, "en", 1);
        const auto otherEn = makeValue(graph, "other_en", 1);
        const auto mask = makeValue(graph, "mask", 8);
        const auto d0 = makeValue(graph, "d0", 8);
        const auto d1 = makeValue(graph, "d1", 8);
        const auto d2 = makeValue(graph, "d2", 8);
        graph.bindInputPort("clk", clk);
        graph.bindInputPort("en", en);
        graph.bindInputPort("other_en", otherEn);
        graph.bindInputPort("mask", mask);
        graph.bindInputPort("d0", d0);
        graph.bindInputPort("d1", d1);
        graph.bindInputPort("d2", d2);
        for (const char *name : {"q0", "q1", "q2"})
        {
            const auto reg = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegister,
                                                   graph.internSymbol(name));
            graph.setAttr(reg, "width", static_cast<int64_t>(8));
            graph.setAttr(reg, "isSigned", false);
        }

        const auto write0 = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                                                  graph.internSymbol("w0"));
        graph.addOperand(write0, en);
        graph.addOperand(write0, d0);
        graph.addOperand(write0, mask);
        graph.addOperand(write0, clk);
        graph.setAttr(write0, "regSymbol", std::string("q0"));
        graph.setAttr(write0, "eventEdge", std::vector<std::string>{"posedge"});
        const auto write1 = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                                                  graph.internSymbol("w1"));
        graph.addOperand(write1, otherEn);
        graph.addOperand(write1, d1);
        graph.addOperand(write1, mask);
        graph.addOperand(write1, clk);
        graph.setAttr(write1, "regSymbol", std::string("q1"));
        graph.setAttr(write1, "eventEdge", std::vector<std::string>{"posedge"});
        const auto write2 = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                                                  graph.internSymbol("w2"));
        graph.addOperand(write2, en);
        graph.addOperand(write2, d2);
        graph.addOperand(write2, mask);
        graph.addOperand(write2, clk);
        graph.setAttr(write2, "regSymbol", std::string("q2"));
        graph.setAttr(write2, "eventEdge", std::vector<std::string>{"posedge"});

        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(ActivityScheduleOptions{
            .path = "commit_guard_event_oversize_bucket",
            .maxOpInComputeSupernode = 1,
            .maxOpInCommitSupernode = 1,
            .enableCoarsen = false,
        }));
        PassDiagnostics diags;
        const PassManagerResult runResult = manager.run(design, diags);
        if (!runResult.success || diags.hasError())
        {
            return fail("Expected commit guard event oversized bucket schedule to succeed");
        }
        const auto schedule = loadSchedule(session, "commit_guard_event_oversize_bucket");
        if (const int rc = validateCommonScheduleShape(graph, schedule); rc != 0)
        {
            return rc;
        }
        const uint32_t write0Supernode = (*schedule.opToSupernode)[write0.index - 1];
        const uint32_t write1Supernode = (*schedule.opToSupernode)[write1.index - 1];
        const uint32_t write2Supernode = (*schedule.opToSupernode)[write2.index - 1];
        if (write0Supernode != write2Supernode)
        {
            return fail("Expected oversized guard bucket to remain a single commit supernode");
        }
        if (write0Supernode == write1Supernode)
        {
            return fail("Expected oversized guard bucket not to merge with following guard bucket");
        }
        const auto &commitOps = (*schedule.supernodeToOps)[write0Supernode];
        if (commitOps.size() != 2 ||
            std::find(commitOps.begin(), commitOps.end(), write0) == commitOps.end() ||
            std::find(commitOps.begin(), commitOps.end(), write2) == commitOps.end())
        {
            return fail("Expected oversized guard bucket supernode to contain exactly the same-guard writes");
        }
    }

    {
        currentCase = "post-DP strict slack move";
        struct FixtureOps
        {
            wolvrix::lib::grh::OperationId p;
            wolvrix::lib::grh::OperationId q;
            wolvrix::lib::grh::OperationId r;
            wolvrix::lib::grh::OperationId t;
            wolvrix::lib::grh::OperationId u;
        };
        const auto buildFixture = [](wolvrix::lib::grh::Design &design)
        {
            auto &graph = design.createGraph("post_dp_strict_slack_move");
            design.markAsTop("post_dp_strict_slack_move");

            const auto a = makeValue(graph, "a", 8);
            const auto b = makeValue(graph, "b", 8);
            const auto c = makeValue(graph, "c", 8);
            graph.bindInputPort("a", a);
            graph.bindInputPort("b", b);
            graph.bindInputPort("c", c);

            const auto makeNot = [&](const std::string &name,
                                     wolvrix::lib::grh::ValueId operand)
            {
                const auto result = makeValue(graph, name + "_value", 8);
                const auto op = graph.createOperation(wolvrix::lib::grh::OperationKind::kNot,
                                                      graph.internSymbol(name));
                graph.addOperand(op, operand);
                graph.addResult(op, result);
                return std::pair{op, result};
            };

            const auto [p, pValue] = makeNot("p", a);
            const auto [q, qValue] = makeNot("q", b);
            const auto [r, rValue] = makeNot("r", c);
            const auto [t, tValue] = makeNot("t", pValue);
            const auto [u, uValue] = makeNot("u", rValue);
            graph.bindOutputPort("q", qValue);
            graph.bindOutputPort("t", tValue);
            graph.bindOutputPort("u", uValue);
            return FixtureOps{p, q, r, t, u};
        };
        const auto runFixture = [](wolvrix::lib::grh::Design &design,
                                   SessionStore &session,
                                   const std::string *policy)
        {
            ActivityScheduleOptions options;
            options.path = "post_dp_strict_slack_move";
            options.maxOpInComputeSupernode = 3;
            options.maxOpInComputeNode = 1;
            options.postDpRefineMaxRounds = 1;
            options.postDpRefineMaxMoves = 16;
            options.postDpRefineMaxMovedOpPpm = 1000000;
            options.enableCoarsen = false;
            if (policy != nullptr)
            {
                options.postDpRefinePolicy = *policy;
            }
            PassManager manager;
            manager.options().session = &session;
            manager.addPass(std::make_unique<ActivitySchedulePass>(options));
            PassDiagnostics diags;
            const PassManagerResult runResult = manager.run(design, diags);
            return runResult.success && !diags.hasError();
        };

        wolvrix::lib::grh::Design defaultDesign;
        const FixtureOps defaultOps = buildFixture(defaultDesign);
        SessionStore defaultSession;
        if (!runFixture(defaultDesign, defaultSession, nullptr))
        {
            return fail("Expected default post-DP slack-move schedule to succeed");
        }

        wolvrix::lib::grh::Design offDesign;
        buildFixture(offDesign);
        SessionStore offSession;
        const std::string offPolicy = "off";
        if (!runFixture(offDesign, offSession, &offPolicy))
        {
            return fail("Expected explicit-off post-DP slack-move schedule to succeed");
        }

        const auto defaultSchedule = loadSchedule(defaultSession, "post_dp_strict_slack_move");
        const auto offSchedule = loadSchedule(offSession, "post_dp_strict_slack_move");
        const auto *defaultGraph = defaultDesign.findGraph("post_dp_strict_slack_move");
        if (defaultGraph == nullptr)
        {
            return fail("Expected post-DP slack-move fixture graph to exist");
        }
        if (const int rc = validateCommonScheduleShape(*defaultGraph, defaultSchedule); rc != 0)
        {
            return rc;
        }
        if (!schedulesEqual(defaultSchedule, offSchedule))
        {
            return fail("Expected default and explicit post-DP refine off session outputs to match");
        }

        const auto defaultOwner = [&](wolvrix::lib::grh::OperationId op)
        {
            return (*defaultSchedule.opToSupernode)[op.index - 1];
        };
        if (defaultOwner(defaultOps.p) != defaultOwner(defaultOps.q) ||
            defaultOwner(defaultOps.p) == defaultOwner(defaultOps.r) ||
            defaultOwner(defaultOps.r) != defaultOwner(defaultOps.t) ||
            defaultOwner(defaultOps.r) != defaultOwner(defaultOps.u))
        {
            return fail("Expected plain DP slack fixture partition {p,q}/{r,t,u}");
        }

        wolvrix::lib::grh::Design strictDesign;
        const FixtureOps strictOps = buildFixture(strictDesign);
        SessionStore strictSession;
        const std::string strictPolicy = "strict";
        if (!runFixture(strictDesign, strictSession, &strictPolicy))
        {
            return fail("Expected strict post-DP slack-move schedule to succeed");
        }
        wolvrix::lib::grh::Design repeatDesign;
        buildFixture(repeatDesign);
        SessionStore repeatSession;
        if (!runFixture(repeatDesign, repeatSession, &strictPolicy))
        {
            return fail("Expected repeated strict post-DP slack-move schedule to succeed");
        }

        const auto strictSchedule = loadSchedule(strictSession, "post_dp_strict_slack_move");
        const auto repeatSchedule = loadSchedule(repeatSession, "post_dp_strict_slack_move");
        const auto *strictGraph = strictDesign.findGraph("post_dp_strict_slack_move");
        if (strictGraph == nullptr)
        {
            return fail("Expected strict post-DP slack-move fixture graph to exist");
        }
        if (const int rc = validateCommonScheduleShape(*strictGraph, strictSchedule); rc != 0)
        {
            return rc;
        }
        if (!schedulesEqual(strictSchedule, repeatSchedule))
        {
            return fail("Expected strict post-DP slack move to be deterministic");
        }
        const auto strictOwner = [&](wolvrix::lib::grh::OperationId op)
        {
            return (*strictSchedule.opToSupernode)[op.index - 1];
        };
        if (strictOwner(strictOps.p) != strictOwner(strictOps.q) ||
            strictOwner(strictOps.p) != strictOwner(strictOps.t) ||
            strictOwner(strictOps.p) == strictOwner(strictOps.r) ||
            strictOwner(strictOps.r) != strictOwner(strictOps.u))
        {
            return fail("Expected strict slack move to produce {p,q,t}/{r,u}");
        }
        if (parseJsonDoubleField(*strictSchedule.summaryStats, "boundary_activation_edges") >=
                parseJsonDoubleField(*defaultSchedule.summaryStats, "boundary_activation_edges") ||
            parseJsonDoubleField(*strictSchedule.summaryStats, "dag_edges") >=
                parseJsonDoubleField(*defaultSchedule.summaryStats, "dag_edges"))
        {
            return fail("Expected strict slack move to reduce BAE and DAG edges");
        }
    }

    {
        currentCase = "post-DP strict full-cap swap";
        struct FixtureOps
        {
            wolvrix::lib::grh::OperationId p;
            wolvrix::lib::grh::OperationId q;
            wolvrix::lib::grh::OperationId r;
            wolvrix::lib::grh::OperationId filler;
            wolvrix::lib::grh::OperationId t;
            wolvrix::lib::grh::OperationId u;
        };
        const auto buildFixture = [](wolvrix::lib::grh::Design &design)
        {
            auto &graph = design.createGraph("post_dp_strict_full_cap_swap");
            design.markAsTop("post_dp_strict_full_cap_swap");

            const auto a = makeValue(graph, "a", 8);
            const auto b = makeValue(graph, "b", 8);
            const auto c = makeValue(graph, "c", 8);
            const auto d = makeValue(graph, "d", 8);
            graph.bindInputPort("a", a);
            graph.bindInputPort("b", b);
            graph.bindInputPort("c", c);
            graph.bindInputPort("d", d);

            const auto makeNot = [&](const std::string &name,
                                     wolvrix::lib::grh::ValueId operand)
            {
                const auto result = makeValue(graph, name + "_value", 8);
                const auto op = graph.createOperation(wolvrix::lib::grh::OperationKind::kNot,
                                                      graph.internSymbol(name));
                graph.addOperand(op, operand);
                graph.addResult(op, result);
                return std::pair{op, result};
            };

            const auto [p, pValue] = makeNot("p", a);
            const auto [q, qValue] = makeNot("q", b);
            const auto [r, rValue] = makeNot("r", c);
            const auto [filler, fillerValue] = makeNot("filler", d);
            const auto [t, tValue] = makeNot("t", pValue);
            const auto [u, uValue] = makeNot("u", rValue);
            graph.bindOutputPort("q", qValue);
            graph.bindOutputPort("filler", fillerValue);
            graph.bindOutputPort("t", tValue);
            graph.bindOutputPort("u", uValue);
            return FixtureOps{p, q, r, filler, t, u};
        };
        const auto runFixture = [](wolvrix::lib::grh::Design &design,
                                   SessionStore &session,
                                   const std::string &policy)
        {
            ActivityScheduleOptions options;
            options.path = "post_dp_strict_full_cap_swap";
            options.maxOpInComputeSupernode = 3;
            options.maxOpInComputeNode = 1;
            options.postDpRefineMaxRounds = 1;
            options.postDpRefineMaxMoves = 16;
            options.postDpRefineMaxMovedOpPpm = 1000000;
            options.enableCoarsen = false;
            options.postDpRefinePolicy = policy;
            PassManager manager;
            manager.options().session = &session;
            manager.addPass(std::make_unique<ActivitySchedulePass>(options));
            PassDiagnostics diags;
            const PassManagerResult runResult = manager.run(design, diags);
            return runResult.success && !diags.hasError();
        };

        wolvrix::lib::grh::Design offDesign;
        const FixtureOps offOps = buildFixture(offDesign);
        SessionStore offSession;
        if (!runFixture(offDesign, offSession, "off"))
        {
            return fail("Expected explicit-off post-DP full-cap schedule to succeed");
        }
        const auto offSchedule = loadSchedule(offSession, "post_dp_strict_full_cap_swap");
        const auto *offGraph = offDesign.findGraph("post_dp_strict_full_cap_swap");
        if (offGraph == nullptr)
        {
            return fail("Expected post-DP full-cap fixture graph to exist");
        }
        if (const int rc = validateCommonScheduleShape(*offGraph, offSchedule); rc != 0)
        {
            return rc;
        }
        const auto offOwner = [&](wolvrix::lib::grh::OperationId op)
        {
            return (*offSchedule.opToSupernode)[op.index - 1];
        };
        if (offOwner(offOps.p) != offOwner(offOps.q) ||
            offOwner(offOps.p) != offOwner(offOps.filler) ||
            offOwner(offOps.p) == offOwner(offOps.r) ||
            offOwner(offOps.r) != offOwner(offOps.t) ||
            offOwner(offOps.r) != offOwner(offOps.u))
        {
            return fail("Expected plain DP full-cap fixture partition {p,q,filler}/{r,t,u}");
        }

        wolvrix::lib::grh::Design strictDesign;
        const FixtureOps strictOps = buildFixture(strictDesign);
        SessionStore strictSession;
        if (!runFixture(strictDesign, strictSession, "strict"))
        {
            return fail("Expected strict post-DP full-cap swap schedule to succeed");
        }
        wolvrix::lib::grh::Design repeatDesign;
        buildFixture(repeatDesign);
        SessionStore repeatSession;
        if (!runFixture(repeatDesign, repeatSession, "strict"))
        {
            return fail("Expected repeated strict post-DP full-cap swap schedule to succeed");
        }
        const auto strictSchedule = loadSchedule(strictSession, "post_dp_strict_full_cap_swap");
        const auto repeatSchedule = loadSchedule(repeatSession, "post_dp_strict_full_cap_swap");
        const auto *strictGraph = strictDesign.findGraph("post_dp_strict_full_cap_swap");
        if (strictGraph == nullptr)
        {
            return fail("Expected strict post-DP full-cap fixture graph to exist");
        }
        if (const int rc = validateCommonScheduleShape(*strictGraph, strictSchedule); rc != 0)
        {
            return rc;
        }
        if (!schedulesEqual(strictSchedule, repeatSchedule))
        {
            return fail("Expected strict post-DP full-cap swap to be deterministic");
        }
        const auto strictOwner = [&](wolvrix::lib::grh::OperationId op)
        {
            return (*strictSchedule.opToSupernode)[op.index - 1];
        };
        if (strictOwner(strictOps.p) != strictOwner(strictOps.q) ||
            strictOwner(strictOps.p) != strictOwner(strictOps.t) ||
            strictOwner(strictOps.p) == strictOwner(strictOps.r) ||
            strictOwner(strictOps.r) != strictOwner(strictOps.filler) ||
            strictOwner(strictOps.r) != strictOwner(strictOps.u))
        {
            return fail("Expected strict full-cap swap to produce {p,q,t}/{r,filler,u}");
        }
        if (parseJsonDoubleField(*strictSchedule.summaryStats, "boundary_activation_edges") >=
                parseJsonDoubleField(*offSchedule.summaryStats, "boundary_activation_edges") ||
            parseJsonDoubleField(*strictSchedule.summaryStats, "dag_edges") >=
                parseJsonDoubleField(*offSchedule.summaryStats, "dag_edges"))
        {
            return fail("Expected strict full-cap swap to reduce BAE and DAG edges");
        }
    }

    {
        currentCase = "post-DP equal-load swap probe";
        struct FixtureOps
        {
            wolvrix::lib::grh::OperationId p;
            wolvrix::lib::grh::OperationId q;
            wolvrix::lib::grh::OperationId x;
            wolvrix::lib::grh::OperationId t;
            wolvrix::lib::grh::OperationId z;
            wolvrix::lib::grh::OperationId filler;
        };
        const auto buildPositive = [](wolvrix::lib::grh::Design &design,
                                      const std::string &name)
        {
            auto &graph = design.createGraph(name);
            design.markAsTop(name);
            const auto a = makeValue(graph, "a", 8);
            const auto b = makeValue(graph, "b", 8);
            const auto c = makeValue(graph, "c", 8);
            graph.bindInputPort("a", a);
            graph.bindInputPort("b", b);
            graph.bindInputPort("c", c);
            const auto makeNot = [&](const std::string &opName,
                                     wolvrix::lib::grh::ValueId operand)
            {
                const auto value = makeValue(graph, opName + "_value", 8);
                const auto op = graph.createOperation(
                    wolvrix::lib::grh::OperationKind::kNot,
                    graph.internSymbol(opName));
                graph.addOperand(op, operand);
                graph.addResult(op, value);
                graph.bindOutputPort(opName, value);
                return std::pair{op, value};
            };
            const auto [p, pValue] = makeNot("p", a);
            const auto [q, qValue] = makeNot("q", b);
            const auto [x, xValue] = makeNot("x", qValue);
            const auto [t, tValue] = makeNot("t", pValue);
            const auto [z, zValue] = makeNot("z", qValue);
            const auto [filler, fillerValue] = makeNot("filler", c);
            return FixtureOps{p, q, x, t, z, filler};
        };
        const auto buildSupportChange = [](wolvrix::lib::grh::Design &design,
                                           const std::string &name)
        {
            auto &graph = design.createGraph(name);
            design.markAsTop(name);
            const auto a = makeValue(graph, "a", 8);
            const auto b = makeValue(graph, "b", 8);
            const auto c = makeValue(graph, "c", 8);
            const auto d = makeValue(graph, "d", 8);
            graph.bindInputPort("a", a);
            graph.bindInputPort("b", b);
            graph.bindInputPort("c", c);
            graph.bindInputPort("d", d);
            const auto makeNot = [&](const std::string &opName,
                                     wolvrix::lib::grh::ValueId operand)
            {
                const auto value = makeValue(graph, opName + "_value", 8);
                const auto op = graph.createOperation(
                    wolvrix::lib::grh::OperationKind::kNot,
                    graph.internSymbol(opName));
                graph.addOperand(op, operand);
                graph.addResult(op, value);
                graph.bindOutputPort(opName, value);
                return std::pair{op, value};
            };
            const auto [p, pValue] = makeNot("p", a);
            const auto [q, qValue] = makeNot("q", b);
            const auto [r, rValue] = makeNot("r", c);
            makeNot("filler", d);
            makeNot("t", pValue);
            makeNot("u", rValue);
        };
        struct SupportKeyOps
        {
            wolvrix::lib::grh::OperationId lhs;
            wolvrix::lib::grh::OperationId rootA;
            wolvrix::lib::grh::OperationId fillerA;
            wolvrix::lib::grh::OperationId rhs;
            wolvrix::lib::grh::OperationId targetB;
            wolvrix::lib::grh::OperationId fillerB;
            wolvrix::lib::grh::OperationId targetC;
            wolvrix::lib::grh::OperationId fillerC1;
            wolvrix::lib::grh::OperationId fillerC2;
        };
        const auto buildSupportKeyChange = [](wolvrix::lib::grh::Design &design,
                                              const std::string &name)
        {
            auto &graph = design.createGraph(name);
            design.markAsTop(name);
            const auto a = makeValue(graph, "a", 8);
            const auto b = makeValue(graph, "b", 8);
            const auto c = makeValue(graph, "c", 8);
            const auto d = makeValue(graph, "d", 8);
            const auto e = makeValue(graph, "e", 8);
            const auto f = makeValue(graph, "f", 8);
            const auto g = makeValue(graph, "g", 8);
            graph.bindInputPort("a", a);
            graph.bindInputPort("b", b);
            graph.bindInputPort("c", c);
            graph.bindInputPort("d", d);
            graph.bindInputPort("e", e);
            graph.bindInputPort("f", f);
            graph.bindInputPort("g", g);
            const auto makeNot = [&](const std::string &opName,
                                     wolvrix::lib::grh::ValueId operand)
            {
                const auto value = makeValue(graph, opName + "_value", 8);
                const auto op = graph.createOperation(
                    wolvrix::lib::grh::OperationKind::kNot,
                    graph.internSymbol(opName));
                graph.addOperand(op, operand);
                graph.addResult(op, value);
                graph.bindOutputPort(opName, value);
                return std::pair{op, value};
            };
            const auto makeXor = [&](const std::string &opName,
                                     wolvrix::lib::grh::ValueId lhs,
                                     wolvrix::lib::grh::ValueId rhs)
            {
                const auto value = makeValue(graph, opName + "_value", 8);
                const auto op = graph.createOperation(
                    wolvrix::lib::grh::OperationKind::kXor,
                    graph.internSymbol(opName));
                graph.addOperand(op, lhs);
                graph.addOperand(op, rhs);
                graph.addResult(op, value);
                graph.bindOutputPort(opName, value);
                return std::pair{op, value};
            };
            const auto [lhs, lhsValue] = makeNot("lhs", a);
            const auto [rootA, rootAValue] = makeNot("root_a", b);
            const auto [fillerA, fillerAValue] = makeNot("filler_a", c);
            const auto [rhs, rhsValue] = makeNot("rhs", d);
            const auto [targetB, targetBValue] =
                makeXor("target_b", lhsValue, rhsValue);
            const auto [fillerB, fillerBValue] = makeNot("filler_b", e);
            const auto [targetC, targetCValue] =
                makeXor("target_c", rhsValue, fillerAValue);
            const auto [fillerC1, fillerC1Value] = makeNot("filler_c1", f);
            const auto [fillerC2, fillerC2Value] = makeNot("filler_c2", g);
            return SupportKeyOps{lhs,
                                 rootA,
                                 fillerA,
                                 rhs,
                                 targetB,
                                 fillerB,
                                 targetC,
                                 fillerC1,
                                 fillerC2};
        };
        const auto runFixture = [](wolvrix::lib::grh::Design &design,
                                   SessionStore &session,
                                   const std::string &name,
                                   const std::string &policy,
                                   std::size_t maxMoves,
                                   std::size_t movedOpPpm,
                                   std::string *log,
                                   std::size_t maxNodeOps,
                                   bool enableCoarsen,
                                   std::size_t maxRounds)
        {
            ActivityScheduleOptions options;
            options.path = name;
            options.maxOpInComputeSupernode = 3;
            options.maxOpInComputeNode = maxNodeOps;
            options.postDpRefinePolicy = policy;
            options.postDpRefineMaxRounds = maxRounds;
            options.postDpRefineMaxMoves = maxMoves;
            options.postDpRefineMaxMovedOpPpm = movedOpPpm;
            options.enableCoarsen = enableCoarsen;
            options.enableChainMerge = !enableCoarsen;
            PassManager manager;
            manager.options().session = &session;
            if (log != nullptr)
            {
                manager.options().logLevel = wolvrix::lib::LogLevel::Info;
                manager.options().logSink =
                    [log](wolvrix::lib::LogLevel,
                          std::string_view,
                          std::string_view message)
                    {
                        log->append(message);
                        log->push_back('\n');
                    };
            }
            manager.addPass(std::make_unique<ActivitySchedulePass>(options));
            PassDiagnostics diags;
            const PassManagerResult result = manager.run(design, diags);
            return std::pair{result, diags.hasError()};
        };

        constexpr std::string_view kPositiveName = "post_dp_equal_load_swap_probe";
        wolvrix::lib::grh::Design offDesign;
        const FixtureOps offOps = buildPositive(offDesign, std::string(kPositiveName));
        SessionStore offSession;
        const auto [offResult, offError] = runFixture(offDesign,
                                                      offSession,
                                                      std::string(kPositiveName),
                                                      "off",
                                                      16,
                                                      1000000,
                                                      nullptr,
                                                      1,
                                                      false,
                                                      1);
        if (!offResult.success || offError)
        {
            return fail("Expected equal-load swap explicit-off fixture to succeed");
        }
        const auto offSchedule = loadSchedule(offSession, std::string(kPositiveName));
        const auto offOwner = [&](wolvrix::lib::grh::OperationId op)
        {
            return (*offSchedule.opToSupernode)[op.index - 1];
        };
        if (offOwner(offOps.p) != offOwner(offOps.q) ||
            offOwner(offOps.p) != offOwner(offOps.filler) ||
            offOwner(offOps.p) == offOwner(offOps.t) ||
            offOwner(offOps.t) != offOwner(offOps.x) ||
            offOwner(offOps.t) != offOwner(offOps.z))
        {
            return fail("Expected equal-load swap baseline partition {p,q,filler}/{x,t,z}: p=" +
                        std::to_string(offOwner(offOps.p)) +
                        " q=" + std::to_string(offOwner(offOps.q)) +
                        " x=" + std::to_string(offOwner(offOps.x)) +
                        " t=" + std::to_string(offOwner(offOps.t)) +
                        " z=" + std::to_string(offOwner(offOps.z)) +
                        " filler=" + std::to_string(offOwner(offOps.filler)));
        }

        wolvrix::lib::grh::Design probeDesign;
        buildPositive(probeDesign, std::string(kPositiveName));
        SessionStore probeSession;
        std::string probeLog;
        const auto [probeResult, probeError] = runFixture(probeDesign,
                                                          probeSession,
                                                          std::string(kPositiveName),
                                                          "swap-probe",
                                                          16,
                                                          1000000,
                                                          &probeLog,
                                                          1,
                                                          false,
                                                          1);
        wolvrix::lib::grh::Design repeatDesign;
        buildPositive(repeatDesign, std::string(kPositiveName));
        SessionStore repeatSession;
        std::string repeatLog;
        const auto [repeatResult, repeatError] = runFixture(repeatDesign,
                                                            repeatSession,
                                                            std::string(kPositiveName),
                                                            "swap-probe",
                                                            16,
                                                            1000000,
                                                            &repeatLog,
                                                            1,
                                                            false,
                                                            1);
        const auto probeSchedule = loadSchedule(probeSession, std::string(kPositiveName));
        if (!probeResult.success || probeError || !repeatResult.success || repeatError ||
            !schedulesEqual(offSchedule, probeSchedule) ||
            !schedulesEqual(probeSchedule,
                            loadSchedule(repeatSession, std::string(kPositiveName))) ||
            parseStatField(probeLog, "capacity_blocked_seeds") == 0 ||
            parseStatField(probeLog, "raw_eligible_swaps") == 0 ||
            parseStatField(repeatLog, "raw_eligible_swaps") !=
                parseStatField(probeLog, "raw_eligible_swaps") ||
            parseStatField(probeLog, "selected_swaps") != 1 ||
            parseStatField(repeatLog, "selected_swaps") !=
                parseStatField(probeLog, "selected_swaps") ||
            parseStatField(probeLog, "selected_moved_clusters") != 2 ||
            parseStatField(probeLog, "swap_rejected_topo") == 0 ||
            parseStatField(probeLog, "rejected_conflict") == 0 ||
            parseStatField(probeLog, "projected_bae_gain") == 0 ||
            parseStatField(probeLog, "actual_bae_gain") !=
                parseStatField(probeLog, "projected_bae_gain") ||
            parseStatField(repeatLog, "projected_bae_gain") !=
                parseStatField(probeLog, "projected_bae_gain") ||
            probeLog.find(" segment_count_valid=true") == std::string::npos ||
            probeLog.find(" segment_ops_valid=true") == std::string::npos ||
            probeLog.find(" dag_support_valid=true") == std::string::npos ||
            probeLog.find(" projected_actual_valid=true") == std::string::npos ||
            probeLog.find(" valid=true") == std::string::npos)
        {
            return fail("Expected deterministic no-mutation equal-load swap probe: " + probeLog);
        }

        wolvrix::lib::grh::Design budgetDesign;
        buildPositive(budgetDesign, std::string(kPositiveName));
        SessionStore budgetSession;
        std::string budgetLog;
        const auto [budgetResult, budgetError] = runFixture(budgetDesign,
                                                            budgetSession,
                                                            std::string(kPositiveName),
                                                            "swap-probe",
                                                            1,
                                                            1000000,
                                                            &budgetLog,
                                                            1,
                                                            false,
                                                            1);
        if (!budgetResult.success || budgetError ||
            !schedulesEqual(offSchedule,
                            loadSchedule(budgetSession, std::string(kPositiveName))) ||
            parseStatField(budgetLog, "raw_eligible_swaps") == 0 ||
            parseStatField(budgetLog, "selected_swaps") != 0 ||
            parseStatField(budgetLog, "rejected_budget") == 0)
        {
            return fail("Expected equal-load swap moved-cluster budget rejection: " + budgetLog);
        }

        wolvrix::lib::grh::Design ppmBudgetDesign;
        buildPositive(ppmBudgetDesign, std::string(kPositiveName));
        SessionStore ppmBudgetSession;
        std::string ppmBudgetLog;
        const auto [ppmBudgetResult, ppmBudgetError] = runFixture(
            ppmBudgetDesign,
            ppmBudgetSession,
            std::string(kPositiveName),
            "swap-probe",
            16,
            0,
            &ppmBudgetLog,
            1,
            false,
            1);
        if (!ppmBudgetResult.success || ppmBudgetError ||
            !schedulesEqual(offSchedule,
                            loadSchedule(ppmBudgetSession, std::string(kPositiveName))) ||
            parseStatField(ppmBudgetLog, "raw_eligible_swaps") == 0 ||
            parseStatField(ppmBudgetLog, "selected_swaps") != 0 ||
            parseStatField(ppmBudgetLog, "selected_moved_ops") != 0 ||
            parseStatField(ppmBudgetLog, "rejected_budget") == 0)
        {
            return fail("Expected equal-load swap moved-op PPM budget rejection: " +
                        ppmBudgetLog);
        }

        wolvrix::lib::grh::Design noRoundsDesign;
        buildPositive(noRoundsDesign, std::string(kPositiveName));
        SessionStore noRoundsSession;
        std::string noRoundsLog;
        const auto [noRoundsResult, noRoundsError] = runFixture(
            noRoundsDesign,
            noRoundsSession,
            std::string(kPositiveName),
            "swap-probe",
            16,
            1000000,
            &noRoundsLog,
            1,
            false,
            0);
        if (!noRoundsResult.success || noRoundsError ||
            !schedulesEqual(offSchedule,
                            loadSchedule(noRoundsSession, std::string(kPositiveName))) ||
            parseStatField(noRoundsLog, "rounds") != 0 ||
            parseStatField(noRoundsLog, "capacity_blocked_seeds") != 0 ||
            parseStatField(noRoundsLog, "enumerated_rhs") != 0 ||
            parseStatField(noRoundsLog, "raw_eligible_swaps") != 0 ||
            parseStatField(noRoundsLog, "selected_swaps") != 0)
        {
            return fail("Expected zero-round swap probe to skip candidate scanning: " +
                        noRoundsLog);
        }

        constexpr std::string_view kSupportName = "post_dp_swap_probe_support_change";
        wolvrix::lib::grh::Design supportDesign;
        buildSupportChange(supportDesign, std::string(kSupportName));
        SessionStore supportSession;
        std::string supportLog;
        const auto [supportResult, supportError] = runFixture(supportDesign,
                                                              supportSession,
                                                              std::string(kSupportName),
                                                              "swap-probe",
                                                              16,
                                                              1000000,
                                                              &supportLog,
                                                              1,
                                                              false,
                                                              1);
        if (!supportResult.success || supportError ||
            parseStatField(supportLog, "rejected_dag_support") == 0 ||
            parseStatField(supportLog, "rejected_dag_edge_count") == 0 ||
            parseStatField(supportLog, "rejected_dag_support_key") != 0 ||
            parseStatField(supportLog, "selected_swaps") != 0)
        {
            return fail("Expected equal-load swap DAG-edge-count rejection: " + supportLog);
        }

        constexpr std::string_view kSupportKeyName =
            "post_dp_swap_probe_support_key_change";
        wolvrix::lib::grh::Design supportKeyOffDesign;
        const SupportKeyOps supportKeyOps =
            buildSupportKeyChange(supportKeyOffDesign, std::string(kSupportKeyName));
        SessionStore supportKeyOffSession;
        const auto [supportKeyOffResult, supportKeyOffError] = runFixture(
            supportKeyOffDesign,
            supportKeyOffSession,
            std::string(kSupportKeyName),
            "off",
            0,
            1000000,
            nullptr,
            1,
            false,
            1);
        if (!supportKeyOffResult.success || supportKeyOffError)
        {
            return fail("Expected DAG-support-key explicit-off fixture to succeed");
        }
        const auto supportKeyOffSchedule =
            loadSchedule(supportKeyOffSession, std::string(kSupportKeyName));
        const auto supportKeyOwner = [&](wolvrix::lib::grh::OperationId op)
        {
            return (*supportKeyOffSchedule.opToSupernode)[op.index - 1];
        };
        if (supportKeyOwner(supportKeyOps.fillerA) !=
                supportKeyOwner(supportKeyOps.fillerB) ||
            supportKeyOwner(supportKeyOps.fillerA) !=
                supportKeyOwner(supportKeyOps.fillerC1) ||
            supportKeyOwner(supportKeyOps.fillerA) == supportKeyOwner(supportKeyOps.lhs) ||
            supportKeyOwner(supportKeyOps.lhs) != supportKeyOwner(supportKeyOps.rhs) ||
            supportKeyOwner(supportKeyOps.lhs) !=
                supportKeyOwner(supportKeyOps.fillerC2) ||
            supportKeyOwner(supportKeyOps.lhs) == supportKeyOwner(supportKeyOps.rootA) ||
            supportKeyOwner(supportKeyOps.rootA) !=
                supportKeyOwner(supportKeyOps.targetB) ||
            supportKeyOwner(supportKeyOps.rootA) !=
                supportKeyOwner(supportKeyOps.targetC))
        {
            return fail("Expected DAG-support-key baseline partition A/B/C: lhs=" +
                        std::to_string(supportKeyOwner(supportKeyOps.lhs)) +
                        " root_a=" +
                        std::to_string(supportKeyOwner(supportKeyOps.rootA)) +
                        " filler_a=" +
                        std::to_string(supportKeyOwner(supportKeyOps.fillerA)) +
                        " rhs=" + std::to_string(supportKeyOwner(supportKeyOps.rhs)) +
                        " target_b=" +
                        std::to_string(supportKeyOwner(supportKeyOps.targetB)) +
                        " filler_b=" +
                        std::to_string(supportKeyOwner(supportKeyOps.fillerB)) +
                        " target_c=" +
                        std::to_string(supportKeyOwner(supportKeyOps.targetC)) +
                        " filler_c1=" +
                        std::to_string(supportKeyOwner(supportKeyOps.fillerC1)) +
                        " filler_c2=" +
                        std::to_string(supportKeyOwner(supportKeyOps.fillerC2)));
        }

        wolvrix::lib::grh::Design supportKeyDesign;
        buildSupportKeyChange(supportKeyDesign, std::string(kSupportKeyName));
        SessionStore supportKeySession;
        std::string supportKeyLog;
        const auto [supportKeyResult, supportKeyError] = runFixture(
            supportKeyDesign,
            supportKeySession,
            std::string(kSupportKeyName),
            "swap-probe",
            0,
            1000000,
            &supportKeyLog,
            1,
            false,
            1);
        if (!supportKeyResult.success || supportKeyError ||
            !schedulesEqual(supportKeyOffSchedule,
                            loadSchedule(supportKeySession,
                                         std::string(kSupportKeyName))) ||
            parseStatField(supportKeyLog, "rejected_dag_support_key") == 0 ||
            parseStatField(supportKeyLog, "swap_dag_edges_before") != 2 ||
            parseStatField(supportKeyLog, "swap_dag_edges_candidate") != 2 ||
            parseStatField(supportKeyLog, "selected_swaps") != 0)
        {
            return fail("Expected same-edge-count DAG-support-key rejection: " +
                        supportKeyLog);
        }

        constexpr std::string_view kUnequalName = "post_dp_swap_probe_unequal_load";
        wolvrix::lib::grh::Design unequalDesign;
        buildPositive(unequalDesign, std::string(kUnequalName));
        SessionStore unequalSession;
        std::string unequalLog;
        const auto [unequalResult, unequalError] = runFixture(
            unequalDesign,
            unequalSession,
            std::string(kUnequalName),
            "swap-probe",
            16,
            1000000,
            &unequalLog,
            2,
            true,
            1);
        if (!unequalResult.success || unequalError ||
            parseStatField(unequalLog, "rejected_equal_load") == 0)
        {
            return fail("Expected unequal-load swap rejection: " + unequalLog);
        }

        wolvrix::lib::grh::Design invalidDesign;
        buildPositive(invalidDesign, "post_dp_swap_probe_invalid_policy");
        SessionStore invalidSession;
        const auto [invalidResult, invalidError] = runFixture(
            invalidDesign,
            invalidSession,
            "post_dp_swap_probe_invalid_policy",
            "swap-probe-invalid",
            16,
            1000000,
            nullptr,
            1,
            false,
            1);
        if (invalidResult.success || !invalidError)
        {
            return fail("Expected invalid post-DP swap policy to fail explicitly");
        }
    }

    {
        currentCase = "local shared compute clone";
        struct FixtureOps
        {
            wolvrix::lib::grh::OperationId shared;
            wolvrix::lib::grh::OperationId first;
            wolvrix::lib::grh::OperationId second;
        };
        const auto buildFixture = [](wolvrix::lib::grh::Design &design,
                                     const std::string &name,
                                     bool exposeOperandBoundary,
                                     bool declaredResult,
                                     bool sideEffect,
                                     std::size_t consumerCount,
                                     int32_t width)
        {
            auto &graph = design.createGraph(name);
            design.markAsTop(name);
            const auto input = makeValue(graph, "input", width);
            graph.bindInputPort("input", input);
            const auto sharedSymbol = graph.internSymbol("shared_value");
            if (declaredResult)
            {
                graph.addDeclaredSymbol(sharedSymbol);
            }
            const auto sharedValue = graph.createValue(sharedSymbol, width, false);
            const auto shared = graph.createOperation(wolvrix::lib::grh::OperationKind::kNot,
                                                      graph.internSymbol("shared"));
            graph.addOperand(shared, input);
            graph.addResult(shared, sharedValue);
            if (sideEffect)
            {
                graph.setAttr(shared, "hasSideEffects", true);
            }

            std::vector<wolvrix::lib::grh::OperationId> consumers;
            for (std::size_t i = 0; i < consumerCount; ++i)
            {
                const std::string suffix = std::to_string(i);
                const auto result = makeValue(graph, "consumer_value_" + suffix, width);
                const auto kind = exposeOperandBoundary
                                      ? wolvrix::lib::grh::OperationKind::kXor
                                      : wolvrix::lib::grh::OperationKind::kNot;
                const auto consumer = graph.createOperation(kind,
                                                            graph.internSymbol("consumer_" + suffix));
                graph.addOperand(consumer, sharedValue);
                if (exposeOperandBoundary)
                {
                    graph.addOperand(consumer, input);
                }
                graph.addResult(consumer, result);
                graph.bindOutputPort("out_" + suffix, result);
                consumers.push_back(consumer);
            }
            return FixtureOps{shared, consumers.at(0), consumers.at(1)};
        };
        const auto runFixture = [](wolvrix::lib::grh::Design &design,
                                   SessionStore &session,
                                   const std::string &name,
                                   std::optional<bool> enabled,
                                   std::size_t maxClones,
                                   std::size_t clonedOpPpm,
                                   std::size_t maxFanout,
                                   std::size_t maxWidth,
                                   std::size_t maxNodeOps,
                                   bool combineRefinements = false,
                                   std::string commonOwnerPolicy = "off",
                                   std::string *capturedLog = nullptr)
        {
            ActivityScheduleOptions options;
            options.path = name;
            options.maxOpInComputeSupernode = 1;
            options.maxOpInComputeNode = maxNodeOps;
            options.enableCoarsen = false;
            options.enableChainMerge = false;
            if (enabled.has_value())
            {
                options.enableLocalSharedCompute = *enabled;
            }
            options.localSharedComputeMaxClones = maxClones;
            options.localSharedComputeMaxClonedOpPpm = clonedOpPpm;
            options.localSharedComputeMaxFanout = maxFanout;
            options.localSharedComputeMaxWidth = maxWidth;
            options.localSharedComputeCommonOwnerPolicy = std::move(commonOwnerPolicy);
            if (combineRefinements)
            {
                options.kahnLevelPackPolicy = "strict";
                options.kahnLevelPackMaxMoves = 16;
                options.kahnLevelPackMaxMovedOpPpm = 1000000;
                options.postDpRefinePolicy = "strict";
                options.postDpRefineMaxMoves = 16;
                options.postDpRefineMaxMovedOpPpm = 1000000;
            }
            PassManager manager;
            manager.options().session = &session;
            if (capturedLog != nullptr)
            {
                manager.options().logLevel = wolvrix::lib::LogLevel::Info;
                manager.options().logSink =
                    [capturedLog](wolvrix::lib::LogLevel,
                                  std::string_view,
                                  std::string_view message)
                    {
                        capturedLog->append(message);
                        capturedLog->push_back('\n');
                    };
            }
            manager.addPass(std::make_unique<ActivitySchedulePass>(options));
            PassDiagnostics diags;
            const PassManagerResult runResult = manager.run(design, diags);
            return runResult.success && !diags.hasError();
        };

        constexpr std::string_view kName = "local_shared_compute_clone";
        wolvrix::lib::grh::Design defaultDesign;
        buildFixture(defaultDesign, std::string(kName), true, false, false, 2, 8);
        SessionStore defaultSession;
        if (!runFixture(defaultDesign, defaultSession, std::string(kName), std::nullopt,
                        4096, 5000, 2, 64, 4))
        {
            return fail("Expected default local shared compute schedule to succeed");
        }
        wolvrix::lib::grh::Design explicitOffDesign;
        buildFixture(explicitOffDesign, std::string(kName), true, false, false, 2, 8);
        SessionStore explicitOffSession;
        if (!runFixture(explicitOffDesign, explicitOffSession, std::string(kName), false,
                        4096, 5000, 2, 64, 4))
        {
            return fail("Expected explicit-off local shared compute schedule to succeed");
        }
        if (!schedulesEqual(loadSchedule(defaultSession, std::string(kName)),
                            loadSchedule(explicitOffSession, std::string(kName))))
        {
            return fail("Expected default and explicit-off local shared compute schedules to match");
        }
        wolvrix::lib::grh::Design probeDesign;
        buildFixture(probeDesign, std::string(kName), true, false, false, 2, 8);
        SessionStore probeSession;
        std::string probeLog;
        if (!runFixture(probeDesign, probeSession, std::string(kName), true,
                        0, 1000000, 2, 64, 4, false, "probe", &probeLog))
        {
            return fail("Expected no-mutation common-owner probe schedule to succeed");
        }
        const auto *defaultGraph = defaultDesign.findGraph(std::string(kName));
        const auto *explicitOffGraph = explicitOffDesign.findGraph(std::string(kName));
        const auto *probeGraph = probeDesign.findGraph(std::string(kName));
        const auto graphConnectivity = [](const wolvrix::lib::grh::Graph &graph)
        {
            std::ostringstream out;
            for (const auto opId : graph.operations())
            {
                out << opId.index << ':' << static_cast<std::size_t>(graph.opKind(opId)) << '(';
                for (const auto operand : graph.opOperands(opId))
                {
                    out << operand.index << ',';
                }
                out << ")->(";
                for (const auto result : graph.opResults(opId))
                {
                    out << result.index << ',';
                }
                out << ");";
            }
            return out.str();
        };
        if (defaultGraph == nullptr || explicitOffGraph == nullptr || probeGraph == nullptr ||
            defaultGraph->operations().size() != explicitOffGraph->operations().size() ||
            defaultGraph->operations().size() != probeGraph->operations().size() ||
            defaultGraph->values().size() != explicitOffGraph->values().size() ||
            defaultGraph->values().size() != probeGraph->values().size() ||
            graphConnectivity(*defaultGraph) != graphConnectivity(*explicitOffGraph) ||
            graphConnectivity(*defaultGraph) != graphConnectivity(*probeGraph) ||
            defaultSession.size() != explicitOffSession.size() ||
            defaultSession.size() != probeSession.size() ||
            !schedulesEqual(loadSchedule(defaultSession, std::string(kName)),
                            loadSchedule(probeSession, std::string(kName))) ||
            probeLog.find("common-owner probe:") == std::string::npos)
        {
            return fail("Expected default/off/probe graph and schedule identity");
        }

        wolvrix::lib::grh::Design enabledDesign;
        const FixtureOps enabledOps =
            buildFixture(enabledDesign, std::string(kName), true, false, false, 2, 8);
        SessionStore enabledSession;
        if (!runFixture(enabledDesign, enabledSession, std::string(kName), true,
                        16, 1000000, 2, 64, 4))
        {
            return fail("Expected local shared compute clone schedule to succeed");
        }
        wolvrix::lib::grh::Design repeatDesign;
        buildFixture(repeatDesign, std::string(kName), true, false, false, 2, 8);
        SessionStore repeatSession;
        if (!runFixture(repeatDesign, repeatSession, std::string(kName), true,
                        16, 1000000, 2, 64, 4))
        {
            return fail("Expected repeated local shared compute clone schedule to succeed");
        }
        const auto enabledSchedule = loadSchedule(enabledSession, std::string(kName));
        const auto repeatSchedule = loadSchedule(repeatSession, std::string(kName));
        const auto *enabledGraph = enabledDesign.findGraph(std::string(kName));
        if (enabledGraph == nullptr)
        {
            return fail("Expected local shared compute clone graph to exist");
        }
        if (const int rc = validateCommonScheduleShape(*enabledGraph, enabledSchedule); rc != 0)
        {
            return rc;
        }
        if (const int rc = validateScheduleTopoOrder(enabledSchedule); rc != 0)
        {
            return rc;
        }
        if (!schedulesEqual(enabledSchedule, repeatSchedule))
        {
            return fail("Expected local shared compute cloning to be deterministic");
        }
        if (enabledSchedule.summaryStats == nullptr ||
            parseJsonDoubleField(*enabledSchedule.summaryStats,
                                 "local_shared_compute_clones_in_compute_nodes") != 1.0)
        {
            return fail("Expected exactly one local shared compute clone");
        }
        std::vector<wolvrix::lib::grh::OperationId> sharedOps;
        for (const auto opId : enabledGraph->operations())
        {
            if (enabledGraph->opKind(opId) == wolvrix::lib::grh::OperationKind::kNot)
            {
                sharedOps.push_back(opId);
            }
        }
        if (sharedOps.size() != 2 || enabledSchedule.opToSupernode == nullptr ||
            enabledSchedule.valueFanout == nullptr)
        {
            return fail("Expected original and cloned shared compute ops");
        }
        const auto owner = [&](wolvrix::lib::grh::OperationId opId)
        {
            return (*enabledSchedule.opToSupernode)[opId.index - 1];
        };
        const uint32_t firstOwner = owner(enabledOps.first);
        const uint32_t secondOwner = owner(enabledOps.second);
        if (firstOwner == secondOwner ||
            !((owner(sharedOps[0]) == firstOwner && owner(sharedOps[1]) == secondOwner) ||
              (owner(sharedOps[1]) == firstOwner && owner(sharedOps[0]) == secondOwner)))
        {
            return fail("Expected original and clone to be local to distinct consumers");
        }
        for (const auto sharedOp : sharedOps)
        {
            const auto results = enabledGraph->opResults(sharedOp);
            if (results.size() != 1 ||
                !(*enabledSchedule.valueFanout)[results.front().index - 1].empty())
            {
                return fail("Expected cloned shared result not to cross a supernode boundary");
            }
        }

        const auto expectNoClone = [&](const std::string &name,
                                       bool exposeBoundary,
                                       bool declaredResult,
                                       bool sideEffect,
                                       std::size_t consumerCount,
                                       int32_t width,
                                       std::size_t maxClones,
                                       std::size_t ppm,
                                       std::size_t maxFanout,
                                       std::size_t maxWidth,
                                       std::size_t maxNodeOps) -> bool
        {
            wolvrix::lib::grh::Design design;
            buildFixture(design, name, exposeBoundary, declaredResult, sideEffect,
                         consumerCount, width);
            SessionStore session;
            if (!runFixture(design, session, name, true, maxClones, ppm,
                            maxFanout, maxWidth, maxNodeOps))
            {
                return false;
            }
            const auto schedule = loadSchedule(session, name);
            const auto *graph = design.findGraph(name);
            return graph != nullptr && graph->operations().size() == consumerCount + 1 &&
                   schedule.summaryStats != nullptr &&
                   parseJsonDoubleField(*schedule.summaryStats,
                                        "local_shared_compute_clones_in_compute_nodes") == 0.0;
        };
        if (!expectNoClone("local_shared_clone_zero_count", true, false, false, 2, 8,
                           0, 1000000, 2, 64, 4) ||
            !expectNoClone("local_shared_clone_zero_ppm", true, false, false, 2, 8,
                           16, 0, 2, 64, 4) ||
            !expectNoClone("local_shared_clone_width", true, false, false, 2, 8,
                           16, 1000000, 2, 4, 4) ||
            !expectNoClone("local_shared_clone_fanout", true, false, false, 3, 8,
                           16, 1000000, 2, 64, 4) ||
            !expectNoClone("local_shared_clone_side_effect", true, false, true, 2, 8,
                           16, 1000000, 2, 64, 4) ||
            !expectNoClone("local_shared_clone_declared", true, true, false, 2, 8,
                           16, 1000000, 2, 64, 4) ||
            !expectNoClone("local_shared_clone_missing_boundary", false, false, false, 2, 8,
                           16, 1000000, 2, 64, 4) ||
            !expectNoClone("local_shared_clone_common_owner", true, false, false, 2, 8,
                           16, 1000000, 2, 64, 1))
        {
            return fail("Expected bounded local shared compute rejection fixture not to clone");
        }

        wolvrix::lib::grh::Design combinedDesign;
        buildFixture(combinedDesign, "local_shared_clone_combined", true, false, false, 2, 8);
        SessionStore combinedSession;
        if (!runFixture(combinedDesign, combinedSession, "local_shared_clone_combined", true,
                        16, 1000000, 2, 64, 4, true))
        {
            return fail("Expected local shared clone with Kahn/post-DP refinements to succeed");
        }
        const auto combinedSchedule = loadSchedule(combinedSession, "local_shared_clone_combined");
        const auto *combinedGraph = combinedDesign.findGraph("local_shared_clone_combined");
        if (combinedGraph == nullptr ||
            validateCommonScheduleShape(*combinedGraph, combinedSchedule) != 0 ||
            combinedSchedule.summaryStats == nullptr ||
            parseJsonDoubleField(*combinedSchedule.summaryStats,
                                 "local_shared_compute_clones_in_compute_nodes") != 1.0)
        {
            return fail("Expected combined refinement schedule to retain the local clone");
        }
    }

    {
        currentCase = "local shared compute common-owner probe";
        struct ProbeFixtureOptions
        {
            bool declaredLeft = true;
            bool upstreamComputeOperand = false;
            bool exposeSourceOperand = true;
            bool addTiedCommitSinks = false;
            std::size_t maxNodeOps = 2;
        };
        const auto buildProbeFixture = [](wolvrix::lib::grh::Design &design,
                                          const std::string &name,
                                          const ProbeFixtureOptions &fixture)
        {
            auto &graph = design.createGraph(name);
            design.markAsTop(name);
            const auto sourceInput = makeValue(graph, "source_input", 8);
            const auto consumerBoundary = fixture.exposeSourceOperand
                                              ? sourceInput
                                              : makeValue(graph, "consumer_boundary", 8);
            graph.bindInputPort("source_input", sourceInput);
            if (!fixture.exposeSourceOperand)
            {
                graph.bindInputPort("consumer_boundary", consumerBoundary);
            }

            auto sourceOperand = sourceInput;
            if (fixture.upstreamComputeOperand)
            {
                const auto upstreamValue = makeValue(graph, "upstream_value", 8);
                const auto upstream = graph.createOperation(
                    wolvrix::lib::grh::OperationKind::kNot,
                    graph.internSymbol("upstream"));
                graph.addOperand(upstream, sourceInput);
                graph.addResult(upstream, upstreamValue);
                sourceOperand = upstreamValue;
            }
            const auto sharedValue = makeValue(graph, "shared_value", 8);
            const auto shared = graph.createOperation(
                wolvrix::lib::grh::OperationKind::kNot,
                graph.internSymbol("shared"));
            graph.addOperand(shared, sourceOperand);
            graph.addResult(shared, sharedValue);
            graph.setAttr(shared, "cloneTestMeta", static_cast<int64_t>(17));
            graph.setOpSrcLoc(shared,
                              wolvrix::lib::grh::SrcLoc{.file = "common_owner_probe.sv",
                                                       .line = 17,
                                                       .column = 3});
            graph.setValueSrcLoc(sharedValue,
                                 wolvrix::lib::grh::SrcLoc{.file = "common_owner_probe.sv",
                                                          .line = 17,
                                                          .column = 9});

            const auto leftSymbol = graph.internSymbol("left_value");
            if (fixture.declaredLeft)
            {
                graph.addDeclaredSymbol(leftSymbol);
            }
            const auto leftValue = graph.createValue(leftSymbol, 8, false);
            const auto left = graph.createOperation(
                wolvrix::lib::grh::OperationKind::kXor,
                graph.internSymbol("left"));
            graph.addOperand(left, sharedValue);
            graph.addOperand(left, consumerBoundary);
            graph.addResult(left, leftValue);

            const auto rightValue = makeValue(graph, "right_value", 8);
            const auto right = graph.createOperation(
                wolvrix::lib::grh::OperationKind::kXor,
                graph.internSymbol("right"));
            graph.addOperand(right, sharedValue);
            graph.addOperand(right, consumerBoundary);
            graph.addResult(right, rightValue);
            graph.bindOutputPort("right", rightValue);

            if (fixture.addTiedCommitSinks)
            {
                const auto enable = makeValue(graph, "commit_enable", 1);
                const auto mask = makeValue(graph, "commit_mask", 8);
                const auto clock = makeValue(graph, "commit_clock", 1);
                graph.bindInputPort("commit_enable", enable);
                graph.bindInputPort("commit_mask", mask);
                graph.bindInputPort("commit_clock", clock);
                const auto reg = graph.createOperation(
                    wolvrix::lib::grh::OperationKind::kRegister,
                    graph.internSymbol("commit_state"));
                graph.setAttr(reg, "width", static_cast<int64_t>(8));
                graph.setAttr(reg, "isSigned", false);
                const auto addWrite = [&](const std::string &name,
                                          wolvrix::lib::grh::ValueId data)
                {
                    const auto write = graph.createOperation(
                        wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                        graph.internSymbol(name));
                    graph.addOperand(write, enable);
                    graph.addOperand(write, data);
                    graph.addOperand(write, mask);
                    graph.addOperand(write, clock);
                    graph.setAttr(write, "regSymbol", std::string("commit_state"));
                    graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});
                };
                addWrite("commit_write_left", leftValue);
                addWrite("commit_write_right", rightValue);
            }

            for (std::size_t i = 0; i < 2; ++i)
            {
                const std::string suffix = std::to_string(i);
                const auto leafValue = makeValue(graph, "left_leaf_value_" + suffix, 8);
                const auto leaf = graph.createOperation(
                    wolvrix::lib::grh::OperationKind::kNot,
                    graph.internSymbol("left_leaf_" + suffix));
                graph.addOperand(leaf, leftValue);
                graph.addResult(leaf, leafValue);
                graph.bindOutputPort("left_out_" + suffix, leafValue);
            }
        };
        const auto runProbeFixture = [&](const std::string &name,
                                         const ProbeFixtureOptions &fixture,
                                         std::string &log) -> bool
        {
            wolvrix::lib::grh::Design design;
            buildProbeFixture(design, name, fixture);
            const auto *beforeGraph = design.findGraph(name);
            const std::size_t beforeOps = beforeGraph == nullptr ? 0 : beforeGraph->operations().size();
            const std::size_t beforeValues = beforeGraph == nullptr ? 0 : beforeGraph->values().size();
            ActivityScheduleOptions options;
            options.path = name;
            options.maxOpInComputeSupernode = 1;
            options.maxOpInComputeNode = fixture.maxNodeOps;
            options.enableCoarsen = false;
            options.enableChainMerge = false;
            options.enableLocalSharedCompute = true;
            options.localSharedComputeMaxClones = 0;
            options.localSharedComputeMaxClonedOpPpm = 1000000;
            options.localSharedComputeMaxFanout = 2;
            options.localSharedComputeMaxWidth = 64;
            options.localSharedComputeCommonOwnerPolicy = "probe";
            options.declaredValueComputeNodeBoundary = fixture.declaredLeft;
            SessionStore session;
            PassManager manager;
            manager.options().session = &session;
            manager.options().logLevel = wolvrix::lib::LogLevel::Info;
            manager.options().logSink =
                [&log](wolvrix::lib::LogLevel,
                       std::string_view,
                       std::string_view message)
                {
                    log.append(message);
                    log.push_back('\n');
                };
            manager.addPass(std::make_unique<ActivitySchedulePass>(options));
            PassDiagnostics diags;
            const PassManagerResult runResult = manager.run(design, diags);
            const auto *afterGraph = design.findGraph(name);
            return runResult.success && !runResult.changed && !diags.hasError() &&
                   afterGraph != nullptr && afterGraph->operations().size() == beforeOps &&
                   afterGraph->values().size() == beforeValues &&
                   loadSchedule(session, name).summaryStats != nullptr;
        };

        std::string positiveLog;
        if (!runProbeFixture("common_owner_probe_positive", {}, positiveLog) ||
            parseStatField(positiveLog, "source_owner_third_common") != 1 ||
            parseStatField(positiveLog, "third_common_singleton") != 1 ||
            parseStatField(positiveLog, "result_boundary_both") != 1 ||
            parseStatField(positiveLog, "operand_locality_both") != 1 ||
            parseStatField(positiveLog, "capacity_both_pass") != 1 ||
            parseStatField(positiveLog, "exact_eligible") != 1 ||
            parseStatField(positiveLog, "projected_removed_pairs") != 2)
        {
            return fail("Expected singleton common-owner probe opportunity: " + positiveLog);
        }

        ProbeFixtureOptions multiOp;
        multiOp.upstreamComputeOperand = true;
        multiOp.maxNodeOps = 3;
        std::string multiOpLog;
        if (!runProbeFixture("common_owner_probe_multiop", multiOp, multiOpLog) ||
            parseStatField(multiOpLog, "third_common_multiop") != 1 ||
            parseStatField(multiOpLog, "exact_eligible") != 0)
        {
            return fail("Expected multi-op common-owner probe rejection: " + multiOpLog);
        }

        ProbeFixtureOptions nonCommonConsumerOwner;
        nonCommonConsumerOwner.declaredLeft = false;
        nonCommonConsumerOwner.maxNodeOps = 4;
        std::string nonCommonConsumerOwnerLog;
        if (!runProbeFixture("common_owner_probe_noncommon_consumer_owner",
                             nonCommonConsumerOwner,
                             nonCommonConsumerOwnerLog) ||
            parseStatField(nonCommonConsumerOwnerLog, "source_owner_is_consumer") == 0 ||
            parseStatField(nonCommonConsumerOwnerLog, "source_owner_third_common") != 0 ||
            parseStatField(nonCommonConsumerOwnerLog, "exact_eligible") != 0)
        {
            return fail("Expected non-common source-owner-is-consumer probe rejection: " +
                        nonCommonConsumerOwnerLog);
        }

        ProbeFixtureOptions missingLocality;
        missingLocality.exposeSourceOperand = false;
        std::string missingLocalityLog;
        if (!runProbeFixture("common_owner_probe_locality", missingLocality, missingLocalityLog) ||
            parseStatField(missingLocalityLog, "operand_locality_neither") != 1 ||
            parseStatField(missingLocalityLog, "exact_eligible") != 0)
        {
            return fail("Expected common-owner operand-locality rejection: " + missingLocalityLog);
        }

        ProbeFixtureOptions noCapacity;
        noCapacity.maxNodeOps = 1;
        std::string noCapacityLog;
        if (!runProbeFixture("common_owner_probe_capacity", noCapacity, noCapacityLog) ||
            parseStatField(noCapacityLog, "capacity_both_fail") != 1 ||
            parseStatField(noCapacityLog, "exact_eligible") != 0)
        {
            return fail("Expected common-owner capacity rejection: " + noCapacityLog);
        }

        const auto runStrictFixture = [&](wolvrix::lib::grh::Design &design,
                                          SessionStore &session,
                                          const std::string &name,
                                          std::size_t maxClones,
                                          std::size_t clonedOpPpm,
                                          std::string &log,
                                          bool combineRefinements = false)
        {
            ActivityScheduleOptions options;
            options.path = name;
            options.maxOpInComputeSupernode = 1;
            options.maxOpInComputeNode = 2;
            options.enableCoarsen = false;
            options.enableChainMerge = false;
            options.enableLocalSharedCompute = true;
            options.localSharedComputeMaxClones = 0;
            options.localSharedComputeMaxClonedOpPpm = 1000000;
            options.localSharedComputeMaxFanout = 2;
            options.localSharedComputeMaxWidth = 64;
            options.localSharedComputeCommonOwnerPolicy = "strict";
            options.localSharedComputeCommonOwnerMaxClones = maxClones;
            options.localSharedComputeCommonOwnerMaxClonedOpPpm = clonedOpPpm;
            options.declaredValueComputeNodeBoundary = true;
            if (combineRefinements)
            {
                options.kahnLevelPackPolicy = "strict";
                options.kahnLevelPackMaxMoves = 16;
                options.kahnLevelPackMaxMovedOpPpm = 1000000;
                options.postDpRefinePolicy = "strict";
                options.postDpRefineMaxMoves = 16;
                options.postDpRefineMaxMovedOpPpm = 1000000;
            }
            PassManager manager;
            manager.options().session = &session;
            manager.options().logLevel = wolvrix::lib::LogLevel::Info;
            manager.options().logSink =
                [&log](wolvrix::lib::LogLevel,
                       std::string_view,
                       std::string_view message)
                {
                    log.append(message);
                    log.push_back('\n');
                };
            manager.addPass(std::make_unique<ActivitySchedulePass>(options));
            PassDiagnostics diags;
            const PassManagerResult runResult = manager.run(design, diags);
            return std::pair{runResult, diags.hasError()};
        };

        wolvrix::lib::grh::Design strictDesign;
        buildProbeFixture(strictDesign, "common_owner_strict_positive", {});
        const auto *strictBefore = strictDesign.findGraph("common_owner_strict_positive");
        const std::size_t strictOpsBefore = strictBefore->operations().size();
        const std::size_t strictValuesBefore = strictBefore->values().size();
        SessionStore strictSession;
        std::string strictLog;
        const auto [strictResult, strictError] =
            runStrictFixture(strictDesign,
                             strictSession,
                             "common_owner_strict_positive",
                             16,
                             1000000,
                             strictLog);
        const auto *strictGraph = strictDesign.findGraph("common_owner_strict_positive");
        const auto strictSchedule = loadSchedule(strictSession, "common_owner_strict_positive");
        if (!strictResult.success || !strictResult.changed || strictError || strictGraph == nullptr ||
            strictGraph->operations().size() != strictOpsBefore + 1 ||
            strictGraph->values().size() != strictValuesBefore + 1 ||
            parseStatField(strictLog, "raw_eligible") != 1 ||
            parseStatField(strictLog, "selected") != 1 ||
            parseStatField(strictLog, "applied") != 1 ||
            parseStatField(strictLog, "actual_localized_pairs") != 2 ||
            parseStatField(strictLog, "graph_ops_delta") != 1 ||
            parseStatField(strictLog, "graph_values_delta") != 1 ||
            strictSchedule.summaryStats == nullptr ||
            parseJsonDoubleField(*strictSchedule.summaryStats,
                                 "local_shared_compute_clones_in_compute_nodes") != 1.0)
        {
            return fail("Expected strict common-owner clone to apply exactly once: " + strictLog);
        }
        if (const int rc = validateCommonScheduleShape(*strictGraph, strictSchedule); rc != 0)
        {
            return rc;
        }
        if (const int rc = validateScheduleTopoOrder(strictSchedule); rc != 0)
        {
            return rc;
        }
        std::vector<wolvrix::lib::grh::OperationId> sharedOps;
        for (const auto opId : strictGraph->operations())
        {
            const auto op = strictGraph->getOperation(opId);
            if (op.kind() == wolvrix::lib::grh::OperationKind::kNot &&
                op.attr("cloneTestMeta").has_value())
            {
                sharedOps.push_back(opId);
            }
        }
        if (sharedOps.size() != 2 || strictSchedule.opToSupernode == nullptr ||
            strictSchedule.valueFanout == nullptr)
        {
            return fail("Expected metadata-preserving original/clone pair");
        }
        for (const auto opId : sharedOps)
        {
            const auto op = strictGraph->getOperation(opId);
            const auto results = op.results();
            if (!op.srcLoc() || op.srcLoc()->line != 17 || results.size() != 1)
            {
                return fail("Expected common-owner clone operation metadata to match");
            }
            const auto value = strictGraph->getValue(results.front());
            if (!value.srcLoc() || value.srcLoc()->column != 9 || value.users().size() != 1 ||
                !(*strictSchedule.valueFanout)[results.front().index - 1].empty())
            {
                return fail("Expected exact local user and no cross-supernode fanout");
            }
            const uint32_t opOwner = (*strictSchedule.opToSupernode)[opId.index - 1];
            const uint32_t userOwner =
                (*strictSchedule.opToSupernode)[value.users().front().operation.index - 1];
            if (opOwner != userOwner)
            {
                return fail("Expected original/clone result to remain local to its user");
            }
        }

        {
            const std::string name = "common_owner_strict_fixed_commit_seed";
            ProbeFixtureOptions tiedFixture;
            tiedFixture.addTiedCommitSinks = true;
            wolvrix::lib::grh::Design baselineDesign;
            buildProbeFixture(baselineDesign, name, tiedFixture);
            ActivityScheduleOptions baselineOptions;
            baselineOptions.path = name;
            baselineOptions.maxOpInComputeSupernode = 1;
            baselineOptions.maxOpInComputeNode = 2;
            baselineOptions.enableCoarsen = false;
            baselineOptions.enableChainMerge = false;
            baselineOptions.enableLocalSharedCompute = true;
            baselineOptions.localSharedComputeMaxClones = 0;
            baselineOptions.localSharedComputeMaxClonedOpPpm = 1000000;
            baselineOptions.localSharedComputeMaxFanout = 2;
            baselineOptions.localSharedComputeMaxWidth = 64;
            baselineOptions.localSharedComputeCommonOwnerPolicy = "probe";
            baselineOptions.declaredValueComputeNodeBoundary = true;
            SessionStore baselineSession;
            PassManager baselineManager;
            baselineManager.options().session = &baselineSession;
            baselineManager.addPass(std::make_unique<ActivitySchedulePass>(baselineOptions));
            PassDiagnostics baselineDiags;
            const PassManagerResult baselineResult =
                baselineManager.run(baselineDesign, baselineDiags);

            wolvrix::lib::grh::Design seededDesign;
            buildProbeFixture(seededDesign, name, tiedFixture);
            SessionStore seededSession;
            std::string seededLog;
            const auto [seededResult, seededError] =
                runStrictFixture(seededDesign,
                                 seededSession,
                                 name,
                                 16,
                                 1000000,
                                 seededLog);
            const auto commitOpPartition = [](const ScheduleView &schedule)
            {
                std::vector<std::vector<uint32_t>> partition;
                if (schedule.supernodeToOps == nullptr || schedule.supernodeKinds == nullptr)
                {
                    return partition;
                }
                for (std::size_t node = 0; node < schedule.supernodeToOps->size(); ++node)
                {
                    if ((*schedule.supernodeKinds)[node] !=
                        ActivityScheduleSupernodeKind::Commit)
                    {
                        continue;
                    }
                    std::vector<uint32_t> ops;
                    for (const auto opId : (*schedule.supernodeToOps)[node])
                    {
                        ops.push_back(opId.index);
                    }
                    partition.push_back(std::move(ops));
                }
                return partition;
            };
            const auto baselineSchedule = loadSchedule(baselineSession, name);
            const auto seededSchedule = loadSchedule(seededSession, name);
            const auto baselineCommit = commitOpPartition(baselineSchedule);
            const auto seededCommit = commitOpPartition(seededSchedule);
            if (!baselineResult.success || baselineDiags.hasError() ||
                !seededResult.success || seededError || baselineCommit != seededCommit ||
                baselineCommit.size() != 1 || baselineCommit.front().size() != 2 ||
                baselineSchedule.summaryStats == nullptr || seededSchedule.summaryStats == nullptr ||
                seededLog.find("fixed_commit_partition_seed_adopted=true") ==
                    std::string::npos)
            {
                return fail("Expected strict clone rebuild to preserve exact tied commit partition: " +
                            seededLog);
            }
            for (const std::string field : {"commit_sink_ops",
                                            "commit_input_root_values",
                                            "commit_event_key_runs",
                                            "commit_event_keys"})
            {
                if (parseJsonDoubleField(*baselineSchedule.summaryStats, field) !=
                    parseJsonDoubleField(*seededSchedule.summaryStats, field))
                {
                    return fail("Expected fixed commit seed to preserve commit summary field: " +
                                std::string(field));
                }
            }
        }

        wolvrix::lib::grh::Design repeatStrictDesign;
        buildProbeFixture(repeatStrictDesign, "common_owner_strict_positive", {});
        SessionStore repeatStrictSession;
        std::string repeatStrictLog;
        const auto [repeatStrictResult, repeatStrictError] =
            runStrictFixture(repeatStrictDesign,
                             repeatStrictSession,
                             "common_owner_strict_positive",
                             16,
                             1000000,
                             repeatStrictLog);
        if (!repeatStrictResult.success || repeatStrictError ||
            !schedulesEqual(strictSchedule,
                            loadSchedule(repeatStrictSession,
                                         "common_owner_strict_positive")))
        {
            return fail("Expected strict common-owner cloning to be deterministic");
        }

        for (const auto &[name, maxClones, ppm] :
             std::vector<std::tuple<std::string, std::size_t, std::size_t>>{
                 {"common_owner_strict_zero_count", 0, 1000000},
                 {"common_owner_strict_zero_ppm", 16, 0},
             })
        {
            wolvrix::lib::grh::Design budgetDesign;
            buildProbeFixture(budgetDesign, name, {});
            const std::size_t beforeOps = budgetDesign.findGraph(name)->operations().size();
            SessionStore budgetSession;
            std::string budgetLog;
            const auto [budgetResult, budgetError] =
                runStrictFixture(budgetDesign,
                                 budgetSession,
                                 name,
                                 maxClones,
                                 ppm,
                                 budgetLog);
            if (!budgetResult.success || budgetResult.changed || budgetError ||
                budgetDesign.findGraph(name)->operations().size() != beforeOps ||
                parseStatField(budgetLog, "raw_eligible") != 1 ||
                parseStatField(budgetLog, "selected") != 0 ||
                parseStatField(budgetLog, "rejected_budget") != 1)
            {
                return fail("Expected strict common-owner zero budget identity: " + budgetLog);
            }
        }

        {
            const std::string name = "common_owner_strict_shared_target_cap";
            wolvrix::lib::grh::Design capDesign;
            auto &graph = capDesign.createGraph(name);
            capDesign.markAsTop(name);
            const auto a = makeValue(graph, "a", 8);
            const auto b = makeValue(graph, "b", 8);
            graph.bindInputPort("a", a);
            graph.bindInputPort("b", b);
            const auto makeShared = [&](const std::string &opName,
                                        wolvrix::lib::grh::ValueId operand)
            {
                const auto value = makeValue(graph, opName + "_value", 8);
                const auto op = graph.createOperation(
                    wolvrix::lib::grh::OperationKind::kNot,
                    graph.internSymbol(opName));
                graph.addOperand(op, operand);
                graph.addResult(op, value);
                return value;
            };
            const auto sharedA = makeShared("shared_a", a);
            const auto sharedB = makeShared("shared_b", b);
            const auto makeTarget = [&](const std::string &opName, bool declared)
            {
                const auto symbol = graph.internSymbol(opName + "_value");
                if (declared)
                {
                    graph.addDeclaredSymbol(symbol);
                }
                const auto value = graph.createValue(symbol, 32, false);
                const auto op = graph.createOperation(
                    wolvrix::lib::grh::OperationKind::kConcat,
                    graph.internSymbol(opName));
                graph.addOperand(op, sharedA);
                graph.addOperand(op, a);
                graph.addOperand(op, sharedB);
                graph.addOperand(op, b);
                graph.addResult(op, value);
                return value;
            };
            const auto leftValue = makeTarget("left", true);
            const auto rightValue = makeTarget("right", false);
            graph.bindOutputPort("right", rightValue);
            for (std::size_t i = 0; i < 2; ++i)
            {
                const std::string suffix = std::to_string(i);
                const auto value = makeValue(graph, "leaf_value_" + suffix, 32);
                const auto op = graph.createOperation(
                    wolvrix::lib::grh::OperationKind::kNot,
                    graph.internSymbol("leaf_" + suffix));
                graph.addOperand(op, leftValue);
                graph.addResult(op, value);
                graph.bindOutputPort("leaf_" + suffix, value);
            }
            const std::size_t beforeOps = graph.operations().size();
            SessionStore capSession;
            std::string capLog;
            const auto [capResult, capError] =
                runStrictFixture(capDesign,
                                 capSession,
                                 name,
                                 16,
                                 1000000,
                                 capLog);
            if (!capResult.success || capError ||
                capDesign.findGraph(name)->operations().size() != beforeOps + 1 ||
                parseStatField(capLog, "raw_eligible") != 2 ||
                parseStatField(capLog, "selected") != 1 ||
                parseStatField(capLog, "rejected_capacity") != 1)
            {
                return fail("Expected shared-target cumulative cap to select one clone: " + capLog);
            }
        }

        wolvrix::lib::grh::Design combinedStrictDesign;
        buildProbeFixture(combinedStrictDesign, "common_owner_strict_combined", {});
        SessionStore combinedStrictSession;
        std::string combinedStrictLog;
        const auto [combinedStrictResult, combinedStrictError] =
            runStrictFixture(combinedStrictDesign,
                             combinedStrictSession,
                             "common_owner_strict_combined",
                             16,
                             1000000,
                             combinedStrictLog,
                             true);
        if (!combinedStrictResult.success || combinedStrictError ||
            parseStatField(combinedStrictLog, "applied") != 1 ||
            validateCommonScheduleShape(*combinedStrictDesign.findGraph(
                                            "common_owner_strict_combined"),
                                        loadSchedule(combinedStrictSession,
                                                     "common_owner_strict_combined")) != 0)
        {
            return fail("Expected strict common-owner clone with Kahn/post-DP shape");
        }
    }

    {
        currentCase = "local shared compute aggregate cap";
        const auto buildFixture = [](wolvrix::lib::grh::Design &design)
        {
            auto &graph = design.createGraph("local_shared_compute_aggregate_cap");
            design.markAsTop("local_shared_compute_aggregate_cap");
            const auto a = makeValue(graph, "a", 8);
            const auto b = makeValue(graph, "b", 8);
            graph.bindInputPort("a", a);
            graph.bindInputPort("b", b);
            const auto makeShared = [&](const std::string &name,
                                        wolvrix::lib::grh::ValueId operand)
            {
                const auto value = makeValue(graph, name + "_value", 8);
                const auto op = graph.createOperation(wolvrix::lib::grh::OperationKind::kNot,
                                                      graph.internSymbol(name));
                graph.addOperand(op, operand);
                graph.addResult(op, value);
                return value;
            };
            const auto s1 = makeShared("s1", a);
            const auto s2 = makeShared("s2", b);
            const auto makeLocal = [&](const std::string &name,
                                       wolvrix::lib::grh::ValueId shared,
                                       wolvrix::lib::grh::ValueId boundary)
            {
                const auto value = makeValue(graph, name + "_value", 8);
                const auto op = graph.createOperation(wolvrix::lib::grh::OperationKind::kXor,
                                                      graph.internSymbol(name));
                graph.addOperand(op, shared);
                graph.addOperand(op, boundary);
                graph.addResult(op, value);
                graph.bindOutputPort(name, value);
            };
            makeLocal("left1", s1, a);
            makeLocal("left2", s2, b);
            const auto rightValue = makeValue(graph, "right_value", 32);
            const auto right = graph.createOperation(wolvrix::lib::grh::OperationKind::kConcat,
                                                     graph.internSymbol("right"));
            graph.addOperand(right, s1);
            graph.addOperand(right, a);
            graph.addOperand(right, s2);
            graph.addOperand(right, b);
            graph.addResult(right, rightValue);
            graph.bindOutputPort("right", rightValue);
        };
        wolvrix::lib::grh::Design design;
        buildFixture(design);
        ActivityScheduleOptions options;
        options.path = "local_shared_compute_aggregate_cap";
        options.maxOpInComputeNode = 2;
        options.maxOpInComputeSupernode = 1;
        options.enableCoarsen = false;
        options.enableChainMerge = false;
        options.enableLocalSharedCompute = true;
        options.localSharedComputeMaxClones = 16;
        options.localSharedComputeMaxClonedOpPpm = 1000000;
        options.localSharedComputeMaxFanout = 2;
        options.localSharedComputeMaxWidth = 64;
        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(options));
        PassDiagnostics diags;
        const PassManagerResult result = manager.run(design, diags);
        const auto schedule = loadSchedule(session, "local_shared_compute_aggregate_cap");
        const auto *graph = design.findGraph("local_shared_compute_aggregate_cap");
        if (!result.success || diags.hasError() || graph == nullptr ||
            graph->operations().size() != 6 || schedule.summaryStats == nullptr ||
            parseJsonDoubleField(*schedule.summaryStats,
                                 "local_shared_compute_clones_in_compute_nodes") != 1.0)
        {
            return fail("Expected aggregate target cap to admit exactly one clone");
        }
        if (const int rc = validateCommonScheduleShape(*graph, schedule); rc != 0)
        {
            return rc;
        }
    }

    {
        currentCase = "local shared compute option validation";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("local_shared_compute_invalid_options");
        design.markAsTop("local_shared_compute_invalid_options");
        const auto input = makeValue(graph, "input", 8);
        graph.bindInputPort("input", input);
        const auto output = makeValue(graph, "output", 8);
        const auto op = graph.createOperation(wolvrix::lib::grh::OperationKind::kNot,
                                              graph.internSymbol("not"));
        graph.addOperand(op, input);
        graph.addResult(op, output);
        graph.bindOutputPort("output", output);
        ActivityScheduleOptions options;
        options.path = "local_shared_compute_invalid_options";
        options.localSharedComputeMaxClonedOpPpm = 1000001;
        SessionStore session;
        PassManager manager;
        manager.options().session = &session;
        manager.addPass(std::make_unique<ActivitySchedulePass>(options));
        PassDiagnostics diags;
        const PassManagerResult result = manager.run(design, diags);
        if (result.success || !diags.hasError())
        {
            return fail("Expected invalid local shared compute cloned-op ppm to fail");
        }

        options.localSharedComputeMaxClonedOpPpm = 5000;
        options.localSharedComputeCommonOwnerPolicy = "strict";
        SessionStore policySession;
        PassManager policyManager;
        policyManager.options().session = &policySession;
        policyManager.addPass(std::make_unique<ActivitySchedulePass>(options));
        PassDiagnostics policyDiags;
        const PassManagerResult policyResult = policyManager.run(design, policyDiags);
        if (policyResult.success || !policyDiags.hasError())
        {
            return fail("Expected invalid local shared compute common-owner policy to fail");
        }

        options.enableLocalSharedCompute = true;
        options.localSharedComputeMaxClones = 1;
        SessionStore mixedSession;
        PassManager mixedManager;
        mixedManager.options().session = &mixedSession;
        mixedManager.addPass(std::make_unique<ActivitySchedulePass>(options));
        PassDiagnostics mixedDiags;
        const PassManagerResult mixedResult = mixedManager.run(design, mixedDiags);
        if (mixedResult.success || !mixedDiags.hasError())
        {
            return fail("Expected strict common-owner plus local-owner clone budget to fail");
        }

        options.localSharedComputeCommonOwnerPolicy = "off";
        options.localSharedComputeCommonOwnerMaxClonedOpPpm = 1000001;
        SessionStore ppmSession;
        PassManager ppmManager;
        ppmManager.options().session = &ppmSession;
        ppmManager.addPass(std::make_unique<ActivitySchedulePass>(options));
        PassDiagnostics ppmDiags;
        const PassManagerResult ppmResult = ppmManager.run(design, ppmDiags);
        if (ppmResult.success || !ppmDiags.hasError())
        {
            return fail("Expected invalid common-owner cloned-op ppm to fail");
        }
    }

    {
        currentCase = "Kahn-level strict packing";
        struct FixtureOps
        {
            wolvrix::lib::grh::OperationId p;
            wolvrix::lib::grh::OperationId q;
            wolvrix::lib::grh::OperationId a;
            wolvrix::lib::grh::OperationId c;
            wolvrix::lib::grh::OperationId b;
            wolvrix::lib::grh::OperationId d;
            wolvrix::lib::grh::OperationId write;
            wolvrix::lib::grh::ValueId aValue;
        };
        const auto buildFixture = [](wolvrix::lib::grh::Design &design)
        {
            auto &graph = design.createGraph("kahn_level_strict_pack");
            design.markAsTop("kahn_level_strict_pack");

            const auto pInput = makeValue(graph, "p_input", 8);
            const auto qInput = makeValue(graph, "q_input", 8);
            const auto aInput = makeValue(graph, "a_input", 8);
            const auto bInput = makeValue(graph, "b_input", 8);
            const auto cInput = makeValue(graph, "c_input", 8);
            const auto dInput = makeValue(graph, "d_input", 8);
            const auto enable = makeValue(graph, "enable", 1);
            const auto mask = makeValue(graph, "mask", 8);
            const auto clock = makeValue(graph, "clock", 1);
            for (const auto &[name, value] :
                 std::vector<std::pair<const char *, wolvrix::lib::grh::ValueId>>{
                     {"p_input", pInput},
                     {"q_input", qInput},
                     {"a_input", aInput},
                     {"b_input", bInput},
                     {"c_input", cInput},
                     {"d_input", dInput},
                     {"enable", enable},
                     {"mask", mask},
                     {"clock", clock},
                 })
            {
                graph.bindInputPort(name, value);
            }

            const auto makeDeclaredProducer = [&](const std::string &name,
                                                  wolvrix::lib::grh::ValueId input)
            {
                const auto valueSymbol = graph.internSymbol(name + "_value");
                graph.addDeclaredSymbol(valueSymbol);
                const auto value = graph.createValue(valueSymbol, 8, false);
                const auto op = graph.createOperation(wolvrix::lib::grh::OperationKind::kNot,
                                                      graph.internSymbol(name));
                graph.addOperand(op, input);
                graph.addResult(op, value);
                return std::pair{op, value};
            };
            const auto makeTarget = [&](const std::string &name,
                                        wolvrix::lib::grh::ValueId shared,
                                        wolvrix::lib::grh::ValueId input)
            {
                const auto value = makeValue(graph, name + "_value", 8);
                const auto op = graph.createOperation(wolvrix::lib::grh::OperationKind::kXor,
                                                      graph.internSymbol(name));
                graph.addOperand(op, shared);
                graph.addOperand(op, input);
                graph.addResult(op, value);
                graph.bindOutputPort(name, value);
                return std::pair{op, value};
            };

            const auto [p, pValue] = makeDeclaredProducer("p", pInput);
            const auto [q, qValue] = makeDeclaredProducer("q", qInput);
            const auto [a, aValue] = makeTarget("t0", pValue, aInput);
            const auto [c, cValue] = makeTarget("t1", qValue, cInput);
            const auto [b, bValue] = makeTarget("t2", pValue, bInput);
            const auto [d, dValue] = makeTarget("t3", qValue, dInput);

            const auto reg = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegister,
                                                   graph.internSymbol("state"));
            graph.setAttr(reg, "width", static_cast<int64_t>(8));
            graph.setAttr(reg, "isSigned", false);
            const auto write = graph.createOperation(wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                                                     graph.internSymbol("state_write"));
            graph.addOperand(write, enable);
            graph.addOperand(write, aValue);
            graph.addOperand(write, mask);
            graph.addOperand(write, clock);
            graph.setAttr(write, "regSymbol", std::string("state"));
            graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});
            return FixtureOps{p, q, a, c, b, d, write, aValue};
        };
        const auto runFixture = [](wolvrix::lib::grh::Design &design,
                                   SessionStore &session,
                                   const std::string *packPolicy,
                                   std::size_t packMaxMoves,
                                   std::size_t packMovedOpPpm,
                                   const std::string *postPolicy)
        {
            ActivityScheduleOptions options;
            options.path = "kahn_level_strict_pack";
            options.maxOpInComputeSupernode = 2;
            options.maxOpInComputeNode = 1;
            options.enableCoarsen = false;
            options.enableChainMerge = false;
            options.declaredValueComputeNodeBoundary = true;
            options.kahnLevelPackMaxMoves = packMaxMoves;
            options.kahnLevelPackMaxMovedOpPpm = packMovedOpPpm;
            options.postDpRefineMaxRounds = 1;
            options.postDpRefineMaxMoves = 16;
            options.postDpRefineMaxMovedOpPpm = 1000000;
            if (packPolicy != nullptr)
            {
                options.kahnLevelPackPolicy = *packPolicy;
            }
            if (postPolicy != nullptr)
            {
                options.postDpRefinePolicy = *postPolicy;
            }
            PassManager manager;
            manager.options().session = &session;
            manager.addPass(std::make_unique<ActivitySchedulePass>(options));
            PassDiagnostics diags;
            const PassManagerResult runResult = manager.run(design, diags);
            return runResult.success && !diags.hasError();
        };

        const std::string offPolicy = "off";
        const std::string strictPolicy = "strict";
        wolvrix::lib::grh::Design defaultDesign;
        buildFixture(defaultDesign);
        SessionStore defaultSession;
        if (!runFixture(defaultDesign, defaultSession, nullptr, 16, 1000000, nullptr))
        {
            return fail("Expected default Kahn-level packing fixture to succeed");
        }
        wolvrix::lib::grh::Design offDesign;
        const FixtureOps offOps = buildFixture(offDesign);
        SessionStore offSession;
        if (!runFixture(offDesign, offSession, &offPolicy, 16, 1000000, nullptr))
        {
            return fail("Expected explicit-off Kahn-level packing fixture to succeed");
        }
        const auto defaultSchedule = loadSchedule(defaultSession, "kahn_level_strict_pack");
        const auto offSchedule = loadSchedule(offSession, "kahn_level_strict_pack");
        if (!schedulesEqual(defaultSchedule, offSchedule))
        {
            return fail("Expected default and explicit-off Kahn-level schedules to match");
        }

        wolvrix::lib::grh::Design zeroBudgetDesign;
        buildFixture(zeroBudgetDesign);
        SessionStore zeroBudgetSession;
        if (!runFixture(zeroBudgetDesign, zeroBudgetSession, &strictPolicy, 0, 1000000, nullptr))
        {
            return fail("Expected zero-move Kahn-level packing fixture to succeed");
        }
        const auto zeroBudgetSchedule = loadSchedule(zeroBudgetSession, "kahn_level_strict_pack");
        if (!schedulesEqual(offSchedule, zeroBudgetSchedule))
        {
            return fail("Expected zero-move Kahn-level packing to preserve the baseline schedule");
        }

        wolvrix::lib::grh::Design zeroMovedOpBudgetDesign;
        buildFixture(zeroMovedOpBudgetDesign);
        SessionStore zeroMovedOpBudgetSession;
        if (!runFixture(zeroMovedOpBudgetDesign,
                        zeroMovedOpBudgetSession,
                        &strictPolicy,
                        16,
                        0,
                        nullptr))
        {
            return fail("Expected zero moved-op Kahn-level packing fixture to succeed");
        }
        const auto zeroMovedOpBudgetSchedule =
            loadSchedule(zeroMovedOpBudgetSession, "kahn_level_strict_pack");
        if (!schedulesEqual(offSchedule, zeroMovedOpBudgetSchedule))
        {
            return fail("Expected zero moved-op Kahn-level packing to preserve the baseline schedule");
        }

        wolvrix::lib::grh::Design strictDesign;
        const FixtureOps strictOps = buildFixture(strictDesign);
        SessionStore strictSession;
        if (!runFixture(strictDesign, strictSession, &strictPolicy, 16, 1000000, nullptr))
        {
            return fail("Expected strict Kahn-level packing fixture to succeed");
        }
        wolvrix::lib::grh::Design repeatDesign;
        buildFixture(repeatDesign);
        SessionStore repeatSession;
        if (!runFixture(repeatDesign, repeatSession, &strictPolicy, 16, 1000000, nullptr))
        {
            return fail("Expected repeated strict Kahn-level packing fixture to succeed");
        }
        const auto strictSchedule = loadSchedule(strictSession, "kahn_level_strict_pack");
        const auto repeatSchedule = loadSchedule(repeatSession, "kahn_level_strict_pack");
        const auto *strictGraph = strictDesign.findGraph("kahn_level_strict_pack");
        if (strictGraph == nullptr)
        {
            return fail("Expected strict Kahn-level fixture graph to exist");
        }
        if (const int rc = validateCommonScheduleShape(*strictGraph, strictSchedule); rc != 0)
        {
            return rc;
        }
        if (const int rc = validateScheduleTopoOrder(strictSchedule); rc != 0)
        {
            return rc;
        }
        if (!schedulesEqual(strictSchedule, repeatSchedule))
        {
            return fail("Expected strict Kahn-level packing to be deterministic");
        }
        const auto owner = [&](const ScheduleView &schedule, wolvrix::lib::grh::OperationId op)
        {
            return (*schedule.opToSupernode)[op.index - 1];
        };
        if (owner(offSchedule, offOps.a) == owner(offSchedule, offOps.b) ||
            owner(offSchedule, offOps.c) == owner(offSchedule, offOps.d))
        {
            return fail("Expected baseline Kahn-level targets to remain interleaved: a=" +
                        std::to_string(owner(offSchedule, offOps.a)) +
                        " b=" + std::to_string(owner(offSchedule, offOps.b)) +
                        " c=" + std::to_string(owner(offSchedule, offOps.c)) +
                        " d=" + std::to_string(owner(offSchedule, offOps.d)));
        }
        if (owner(strictSchedule, strictOps.a) != owner(strictSchedule, strictOps.b) ||
            owner(strictSchedule, strictOps.c) != owner(strictSchedule, strictOps.d))
        {
            return fail("Expected strict Kahn-level packing to colocate shared-value targets");
        }
        if (parseJsonDoubleField(*strictSchedule.summaryStats, "boundary_activation_edges") >=
            parseJsonDoubleField(*offSchedule.summaryStats, "boundary_activation_edges"))
        {
            return fail("Expected strict Kahn-level packing to reduce BAE");
        }
        if (parseJsonDoubleField(*strictSchedule.summaryStats, "dag_edges") >
            parseJsonDoubleField(*offSchedule.summaryStats, "dag_edges"))
        {
            return fail("Expected strict Kahn-level packing not to regress DAG edges");
        }
        const uint32_t writeSupernode = owner(strictSchedule, strictOps.write);
        if (writeSupernode >= strictSchedule.supernodeKinds->size() ||
            (*strictSchedule.supernodeKinds)[writeSupernode] != ActivityScheduleSupernodeKind::Commit ||
            !hasFanoutTo(*strictSchedule.valueFanout, strictOps.aValue, writeSupernode))
        {
            return fail("Expected Kahn-level packing to preserve remapped compute-to-commit fanout");
        }

        wolvrix::lib::grh::Design combinedDesign;
        buildFixture(combinedDesign);
        SessionStore combinedSession;
        if (!runFixture(combinedDesign,
                        combinedSession,
                        &strictPolicy,
                        16,
                        1000000,
                        &strictPolicy))
        {
            return fail("Expected Kahn-level packing plus strict post-DP refine to succeed");
        }
        const auto combinedSchedule = loadSchedule(combinedSession, "kahn_level_strict_pack");
        const auto *combinedGraph = combinedDesign.findGraph("kahn_level_strict_pack");
        if (combinedGraph == nullptr)
        {
            return fail("Expected combined Kahn-level fixture graph to exist");
        }
        if (const int rc = validateCommonScheduleShape(*combinedGraph, combinedSchedule); rc != 0)
        {
            return rc;
        }
        if (const int rc = validateScheduleTopoOrder(combinedSchedule); rc != 0)
        {
            return rc;
        }
    }

    {
        currentCase = "Kahn-level cross-slot topo guard";
        struct FixtureOps
        {
            wolvrix::lib::grh::OperationId a;
            wolvrix::lib::grh::OperationId aChild;
        };
        const auto buildFixture = [](wolvrix::lib::grh::Design &design)
        {
            auto &graph = design.createGraph("kahn_level_topo_guard");
            design.markAsTop("kahn_level_topo_guard");
            const auto rInput = makeValue(graph, "r_input", 8);
            const auto qInput = makeValue(graph, "q_input", 8);
            const auto other = makeValue(graph, "other", 8);
            graph.bindInputPort("r_input", rInput);
            graph.bindInputPort("q_input", qInput);
            graph.bindInputPort("other", other);

            const auto makeDeclaredProducer = [&](const std::string &name,
                                                  wolvrix::lib::grh::ValueId input)
            {
                const auto valueSymbol = graph.internSymbol(name + "_value");
                graph.addDeclaredSymbol(valueSymbol);
                const auto value = graph.createValue(valueSymbol, 8, false);
                const auto op = graph.createOperation(wolvrix::lib::grh::OperationKind::kNot,
                                                      graph.internSymbol(name));
                graph.addOperand(op, input);
                graph.addResult(op, value);
                return value;
            };
            const auto rValue = makeDeclaredProducer("r", rInput);
            const auto qValue = makeDeclaredProducer("q", qInput);
            const auto makeTarget = [&](const std::string &name,
                                        wolvrix::lib::grh::ValueId lhs,
                                        wolvrix::lib::grh::ValueId rhs)
            {
                const auto value = makeValue(graph, name + "_value", 8);
                const auto op = graph.createOperation(wolvrix::lib::grh::OperationKind::kXor,
                                                      graph.internSymbol(name));
                graph.addOperand(op, lhs);
                graph.addOperand(op, rhs);
                graph.addResult(op, value);
                graph.bindOutputPort(name, value);
                return std::pair{op, value};
            };
            const auto [a, aValue] = makeTarget("t0", rValue, other);
            const auto [aChild, aChildValue] = makeTarget("t1", aValue, other);
            const auto [c, cValue] = makeTarget("t2", qValue, other);
            const auto [b, bValue] = makeTarget("t3", rValue, other);
            (void)c;
            (void)cValue;
            (void)b;
            (void)bValue;
            (void)aChildValue;
            return FixtureOps{a, aChild};
        };
        const auto runFixture = [](wolvrix::lib::grh::Design &design,
                                   SessionStore &session,
                                   const std::string &policy)
        {
            ActivityScheduleOptions options;
            options.path = "kahn_level_topo_guard";
            options.maxOpInComputeSupernode = 2;
            options.maxOpInComputeNode = 1;
            options.enableCoarsen = false;
            options.enableChainMerge = false;
            options.declaredValueComputeNodeBoundary = true;
            options.kahnLevelPackPolicy = policy;
            options.kahnLevelPackMaxMoves = 16;
            options.kahnLevelPackMaxMovedOpPpm = 1000000;
            PassManager manager;
            manager.options().session = &session;
            manager.addPass(std::make_unique<ActivitySchedulePass>(options));
            PassDiagnostics diags;
            const PassManagerResult runResult = manager.run(design, diags);
            return runResult.success && !diags.hasError();
        };

        wolvrix::lib::grh::Design offDesign;
        buildFixture(offDesign);
        SessionStore offSession;
        if (!runFixture(offDesign, offSession, "off"))
        {
            return fail("Expected explicit-off Kahn-level topo-guard fixture to succeed");
        }
        wolvrix::lib::grh::Design strictDesign;
        buildFixture(strictDesign);
        SessionStore strictSession;
        if (!runFixture(strictDesign, strictSession, "strict"))
        {
            return fail("Expected strict Kahn-level topo-guard fixture to succeed");
        }
        const auto offSchedule = loadSchedule(offSession, "kahn_level_topo_guard");
        const auto strictSchedule = loadSchedule(strictSession, "kahn_level_topo_guard");
        const auto *strictGraph = strictDesign.findGraph("kahn_level_topo_guard");
        if (strictGraph == nullptr)
        {
            return fail("Expected Kahn-level topo-guard graph to exist");
        }
        if (const int rc = validateCommonScheduleShape(*strictGraph, strictSchedule); rc != 0)
        {
            return rc;
        }
        if (const int rc = validateScheduleTopoOrder(strictSchedule); rc != 0)
        {
            return rc;
        }
        if (parseJsonDoubleField(*strictSchedule.summaryStats, "boundary_activation_edges") >
                parseJsonDoubleField(*offSchedule.summaryStats, "boundary_activation_edges") ||
            parseJsonDoubleField(*strictSchedule.summaryStats, "dag_edges") >
                parseJsonDoubleField(*offSchedule.summaryStats, "dag_edges"))
        {
            return fail("Expected strict topo-guard fixture not to regress exact metrics");
        }
    }

    {
        currentCase = "Kahn-level high fanout bound";
        const auto buildFixture = [](wolvrix::lib::grh::Design &design)
        {
            auto &graph = design.createGraph("kahn_level_high_fanout");
            design.markAsTop("kahn_level_high_fanout");
            const auto pInput = makeValue(graph, "p_input", 8);
            const auto qInput = makeValue(graph, "q_input", 8);
            const auto other = makeValue(graph, "other", 8);
            graph.bindInputPort("p_input", pInput);
            graph.bindInputPort("q_input", qInput);
            graph.bindInputPort("other", other);
            const auto makeDeclaredProducer = [&](const std::string &name,
                                                  wolvrix::lib::grh::ValueId input)
            {
                const auto valueSymbol = graph.internSymbol(name + "_value");
                graph.addDeclaredSymbol(valueSymbol);
                const auto value = graph.createValue(valueSymbol, 8, false);
                const auto op = graph.createOperation(wolvrix::lib::grh::OperationKind::kNot,
                                                      graph.internSymbol(name));
                graph.addOperand(op, input);
                graph.addResult(op, value);
                return value;
            };
            const auto pValue = makeDeclaredProducer("p", pInput);
            const auto qValue = makeDeclaredProducer("q", qInput);
            for (std::size_t i = 0; i < 80; ++i)
            {
                const std::string name = "t" + std::string(i < 10 ? "0" : "") + std::to_string(i);
                const auto value = makeValue(graph, name + "_value", 8);
                const auto op = graph.createOperation(wolvrix::lib::grh::OperationKind::kXor,
                                                      graph.internSymbol(name));
                graph.addOperand(op, (i & 1U) == 0 ? pValue : qValue);
                graph.addOperand(op, other);
                graph.addResult(op, value);
                graph.bindOutputPort(name, value);
            }
        };
        const auto runFixture = [](wolvrix::lib::grh::Design &design, SessionStore &session)
        {
            ActivityScheduleOptions options;
            options.path = "kahn_level_high_fanout";
            options.maxOpInComputeSupernode = 4;
            options.maxOpInComputeNode = 1;
            options.enableCoarsen = false;
            options.enableChainMerge = false;
            options.declaredValueComputeNodeBoundary = true;
            options.kahnLevelPackPolicy = "strict";
            options.kahnLevelPackMaxMoves = 4;
            options.kahnLevelPackMaxMovedOpPpm = 1000000;
            PassManager manager;
            manager.options().session = &session;
            manager.addPass(std::make_unique<ActivitySchedulePass>(options));
            PassDiagnostics diags;
            const PassManagerResult runResult = manager.run(design, diags);
            return runResult.success && !diags.hasError();
        };

        wolvrix::lib::grh::Design design;
        buildFixture(design);
        SessionStore session;
        if (!runFixture(design, session))
        {
            return fail("Expected bounded high-fanout Kahn-level packing to succeed");
        }
        wolvrix::lib::grh::Design repeatDesign;
        buildFixture(repeatDesign);
        SessionStore repeatSession;
        if (!runFixture(repeatDesign, repeatSession))
        {
            return fail("Expected repeated high-fanout Kahn-level packing to succeed");
        }
        const auto schedule = loadSchedule(session, "kahn_level_high_fanout");
        const auto repeatSchedule = loadSchedule(repeatSession, "kahn_level_high_fanout");
        const auto *graph = design.findGraph("kahn_level_high_fanout");
        if (graph == nullptr)
        {
            return fail("Expected high-fanout Kahn-level graph to exist");
        }
        if (const int rc = validateCommonScheduleShape(*graph, schedule); rc != 0)
        {
            return rc;
        }
        if (const int rc = validateScheduleTopoOrder(schedule); rc != 0)
        {
            return rc;
        }
        if (!schedulesEqual(schedule, repeatSchedule))
        {
            return fail("Expected high-fanout Kahn-level packing to be deterministic");
        }
    }

    {
        currentCase = "plain DP segment penalty options";
        const auto buildFixture = [](wolvrix::lib::grh::Design &design)
        {
            auto &graph = design.createGraph("dp_segment_penalty_options");
            design.markAsTop("dp_segment_penalty_options");
            const auto inA = makeValue(graph, "in_a", 8);
            const auto inB1 = makeValue(graph, "in_b1", 8);
            const auto inB2 = makeValue(graph, "in_b2", 8);
            graph.bindInputPort("in_a", inA);
            graph.bindInputPort("in_b1", inB1);
            graph.bindInputPort("in_b2", inB2);
            const auto makeNot = [&](const std::string &name,
                                     wolvrix::lib::grh::ValueId operand)
            {
                const auto value = makeValue(graph, name + "_value", 8);
                const auto op = graph.createOperation(wolvrix::lib::grh::OperationKind::kNot,
                                                      graph.internSymbol(name));
                graph.addOperand(op, operand);
                graph.addResult(op, value);
                return value;
            };
            const auto aValue = makeNot("a", inA);
            const auto b1Value = makeNot("b1", inB1);
            const auto b2Value = makeNot("b2", inB2);
            const auto cValue = makeNot("c", b1Value);
            const auto dValue = makeNot("d", b2Value);
            const auto eValue = makeNot("e", b2Value);
            graph.bindOutputPort("out_a", aValue);
            graph.bindOutputPort("out_c", cValue);
            graph.bindOutputPort("out_d", dValue);
            graph.bindOutputPort("out_e", eValue);
        };
        const auto runFixture = [&](std::optional<std::size_t> penaltyPpm,
                                    SessionStore &session,
                                    bool expectSuccess)
        {
            wolvrix::lib::grh::Design design;
            buildFixture(design);
            ActivityScheduleOptions options;
            options.path = "dp_segment_penalty_options";
            options.maxOpInComputeSupernode = 3;
            options.maxOpInComputeNode = 1;
            options.enableCoarsen = false;
            options.enableChainMerge = false;
            if (penaltyPpm)
            {
                options.dpSegmentPenaltyPpm = *penaltyPpm;
            }
            PassManager manager;
            manager.options().session = &session;
            manager.addPass(std::make_unique<ActivitySchedulePass>(options));
            PassDiagnostics diags;
            const PassManagerResult result = manager.run(design, diags);
            return expectSuccess
                       ? result.success && !diags.hasError()
                       : !result.success && diags.hasError();
        };

        std::string parseError;
        const std::vector<std::string_view> separatedArgs{
            "-path", "dp_segment_penalty_options",
            "-dp-segment-penalty-ppm", "500000"};
        const std::vector<std::string_view> equalsArgs{
            "-path=dp_segment_penalty_options",
            "-dp-segment-penalty-ppm=2000000"};
        if (makePass("activity-schedule", separatedArgs, parseError) == nullptr ||
            makePass("activity-schedule", equalsArgs, parseError) == nullptr)
        {
            return fail("Expected both DP segment penalty CLI forms to parse");
        }
        const std::vector<std::string_view> malformedSeparatedArgs{
            "-path", "dp_segment_penalty_options",
            "-dp-segment-penalty-ppm", "500000x"};
        const std::vector<std::string_view> malformedEqualsArgs{
            "-path=dp_segment_penalty_options",
            "-dp-segment-penalty-ppm=500000x"};
        const std::vector<std::string_view> overflowArgs{
            "-path=dp_segment_penalty_options",
            "-dp-segment-penalty-ppm=999999999999999999999999999999999999"};
        if (makePass("activity-schedule", malformedSeparatedArgs, parseError) != nullptr ||
            makePass("activity-schedule", malformedEqualsArgs, parseError) != nullptr ||
            makePass("activity-schedule", overflowArgs, parseError) != nullptr)
        {
            return fail("Expected malformed or overflowing DP segment penalty CLI values to fail");
        }

        SessionStore defaultSession;
        SessionStore explicitDefaultSession;
        if (!runFixture(std::nullopt, defaultSession, true) ||
            !runFixture(1000000, explicitDefaultSession, true))
        {
            return fail("Expected default DP segment penalty schedules to succeed");
        }
        if (!schedulesEqual(loadSchedule(defaultSession, "dp_segment_penalty_options"),
                            loadSchedule(explicitDefaultSession,
                                         "dp_segment_penalty_options")))
        {
            return fail("Expected default and explicit DP segment penalty schedules to match");
        }
        SessionStore halfSession;
        if (!runFixture(500000, halfSession, true))
        {
            return fail("Expected half DP segment penalty schedule to succeed");
        }
        const auto defaultSchedule =
            loadSchedule(defaultSession, "dp_segment_penalty_options");
        const auto halfSchedule =
            loadSchedule(halfSession, "dp_segment_penalty_options");
        if (defaultSchedule.summaryStats == nullptr || halfSchedule.summaryStats == nullptr ||
            parseJsonDoubleField(*defaultSchedule.summaryStats,
                                 "compute_supernodes") != 2.0 ||
            parseJsonDoubleField(*defaultSchedule.summaryStats,
                                 "compute_compute_value_pairs") != 2.0 ||
            parseJsonDoubleField(*halfSchedule.summaryStats,
                                 "compute_supernodes") != 3.0 ||
            parseJsonDoubleField(*halfSchedule.summaryStats,
                                 "compute_compute_value_pairs") != 1.0)
        {
            return fail("Expected half DP penalty to trade one supernode for one compute BAE");
        }
        SessionStore zeroSession;
        if (!runFixture(0, zeroSession, true))
        {
            return fail("Expected zero DP segment penalty to be accepted");
        }
        SessionStore invalidSession;
        if (!runFixture(1000000001, invalidSession, false))
        {
            return fail("Expected excessive DP segment penalty to fail");
        }
    }

    {
        currentCase = "Kahn-level option validation";
        const auto runInvalid = [](const std::string &policy,
                                   std::size_t movedOpPpm,
                                   std::size_t regressionPpm)
        {
            wolvrix::lib::grh::Design design;
            auto &graph = design.createGraph("kahn_level_invalid_options");
            design.markAsTop("kahn_level_invalid_options");
            const auto input = makeValue(graph, "input", 8);
            graph.bindInputPort("input", input);
            const auto output = makeValue(graph, "output", 8);
            const auto op = graph.createOperation(wolvrix::lib::grh::OperationKind::kNot,
                                                  graph.internSymbol("not"));
            graph.addOperand(op, input);
            graph.addResult(op, output);
            graph.bindOutputPort("output", output);
            ActivityScheduleOptions options;
            options.path = "kahn_level_invalid_options";
            options.kahnLevelPackPolicy = policy;
            options.kahnLevelPackMaxMovedOpPpm = movedOpPpm;
            options.kahnLevelPackMaxRegressionPpm = regressionPpm;
            SessionStore session;
            PassManager manager;
            manager.options().session = &session;
            manager.addPass(std::make_unique<ActivitySchedulePass>(options));
            PassDiagnostics diags;
            const PassManagerResult result = manager.run(design, diags);
            return !result.success && diags.hasError();
        };
        if (!runInvalid("invalid", 10000, 10000))
        {
            return fail("Expected invalid Kahn-level packing policy to fail");
        }
        if (!runInvalid("off", 1000001, 10000))
        {
            return fail("Expected invalid Kahn-level moved-op ppm to fail");
        }
        if (!runInvalid("off", 10000, 1000001))
        {
            return fail("Expected invalid Kahn-level regression ppm to fail");
        }
    }

    {
        currentCase = "final fanin pullback probe";
        struct FaninFixtureOptions
        {
            std::size_t sourceCount = 4;
            bool sharedTargetInput = false;
            bool duplicateCandidateInput = false;
            bool extraSourceResult = false;
            bool secondCandidateLiveout = false;
            bool anchoredUnusedResult = false;
            bool bindCandidateOutput = false;
            bool declareCandidate = false;
            bool addCommitConsumer = false;
            bool addUnrelatedStateCommit = false;
            bool fillSourceCapacity = false;
            bool candidateUsesNoDef = false;
            bool candidateUsesTargetPredecessor = false;
            bool candidateHasSideEffects = false;
            bool candidateHasCloneForbiddenAttr = false;
            bool wideCandidateResult = false;
        };
        struct FaninFixtureOps
        {
            std::vector<wolvrix::lib::grh::OperationId> sources;
            wolvrix::lib::grh::OperationId candidate;
            wolvrix::lib::grh::OperationId tail;
        };
        const auto buildFixture = [](wolvrix::lib::grh::Design &design,
                                     const std::string &name,
                                     const FaninFixtureOptions &fixture)
        {
            auto &graph = design.createGraph(name);
            design.markAsTop(name);
            std::optional<wolvrix::lib::grh::ValueId> unrelatedStateRead;
            if (fixture.addUnrelatedStateCommit)
            {
                const auto reg = graph.createOperation(
                    wolvrix::lib::grh::OperationKind::kRegister,
                    graph.internSymbol("unrelated_state"));
                graph.setAttr(reg, "width", static_cast<int64_t>(8));
                graph.setAttr(reg, "isSigned", false);
                const auto value = makeValue(graph, "unrelated_state_read_value", 8);
                const auto read = graph.createOperation(
                    wolvrix::lib::grh::OperationKind::kRegisterReadPort,
                    graph.internSymbol("unrelated_state_read"));
                graph.addResult(read, value);
                graph.setAttr(read, "regSymbol", std::string("unrelated_state"));
                graph.bindOutputPort("unrelated_state_read", value);
                unrelatedStateRead = value;
            }
            std::vector<wolvrix::lib::grh::ValueId> sourceValues;
            FaninFixtureOps ops;
            for (std::size_t i = 0; i < fixture.sourceCount; ++i)
            {
                const std::string suffix = std::to_string(i);
                const auto input = makeValue(graph, "source_input_" + suffix, 8);
                graph.bindInputPort("source_input_" + suffix, input);
                const auto value = makeValue(graph, "source_value_" + suffix, 8);
                const auto op = graph.createOperation(
                    wolvrix::lib::grh::OperationKind::kNot,
                    graph.internSymbol("source_" + suffix));
                graph.addOperand(op, input);
                graph.addResult(op, value);
                sourceValues.push_back(value);
                if (i == 0 && fixture.extraSourceResult)
                {
                    const auto extraValue =
                        makeValue(graph, "source_extra_value", 8);
                    graph.addResult(op, extraValue);
                    sourceValues.push_back(extraValue);
                }
                ops.sources.push_back(op);
            }
            if (fixture.fillSourceCapacity)
            {
                const auto input = makeValue(graph, "source_padding_input", 8);
                graph.bindInputPort("source_padding_input", input);
                const auto value = makeValue(graph, "source_padding_value", 8);
                const auto op = graph.createOperation(
                    wolvrix::lib::grh::OperationKind::kNot,
                    graph.internSymbol("source_padding"));
                graph.addOperand(op, input);
                graph.addResult(op, value);
                graph.bindOutputPort("source_padding", value);
            }

            const auto blockerInput = makeValue(graph, "blocker_input", 8);
            graph.bindInputPort("blocker_input", blockerInput);
            const auto blockerValue = makeValue(graph, "blocker_value", 8);
            const auto blocker = graph.createOperation(
                wolvrix::lib::grh::OperationKind::kNot,
                graph.internSymbol("blocker"));
            graph.addOperand(blocker, blockerInput);
            graph.addResult(blocker, blockerValue);

            const auto candidateSymbol = graph.internSymbol("candidate_value");
            if (fixture.declareCandidate)
            {
                graph.addDeclaredSymbol(candidateSymbol);
            }
            const auto candidateValue =
                graph.createValue(candidateSymbol,
                                  fixture.wideCandidateResult ? 128 : 8,
                                  false);
            ops.candidate = graph.createOperation(
                wolvrix::lib::grh::OperationKind::kAnd,
                graph.internSymbol("candidate"));
            for (const auto value : sourceValues)
            {
                graph.addOperand(ops.candidate, value);
            }
            if (fixture.duplicateCandidateInput)
            {
                graph.addOperand(ops.candidate, sourceValues.front());
            }
            if (fixture.candidateUsesNoDef)
            {
                const auto noDef = makeValue(graph, "candidate_no_def", 8);
                graph.bindInputPort("candidate_no_def", noDef);
                graph.addOperand(ops.candidate, noDef);
            }
            if (fixture.candidateUsesTargetPredecessor)
            {
                graph.addOperand(ops.candidate, blockerValue);
            }
            graph.addResult(ops.candidate, candidateValue);
            std::optional<wolvrix::lib::grh::ValueId> secondCandidateValue;
            if (fixture.secondCandidateLiveout)
            {
                secondCandidateValue = makeValue(graph, "candidate_second_value", 8);
                graph.addResult(ops.candidate, *secondCandidateValue);
            }
            if (fixture.anchoredUnusedResult)
            {
                const auto anchored = makeValue(graph, "candidate_anchored_unused", 8);
                graph.addResult(ops.candidate, anchored);
                graph.bindOutputPort("candidate_anchored_unused", anchored);
            }
            if (fixture.candidateHasSideEffects)
            {
                graph.setAttr(ops.candidate, "hasSideEffects", true);
            }
            if (fixture.candidateHasCloneForbiddenAttr)
            {
                graph.setAttr(ops.candidate,
                              "regToMem.intent.testOnly",
                              std::string("forbidden"));
            }
            if (fixture.bindCandidateOutput)
            {
                graph.bindOutputPort("candidate", candidateValue);
            }

            const auto tailValue = makeValue(graph, "tail_value", 8);
            ops.tail = graph.createOperation(
                wolvrix::lib::grh::OperationKind::kXor,
                graph.internSymbol("tail"));
            graph.addOperand(ops.tail, candidateValue);
            if (secondCandidateValue)
            {
                graph.addOperand(ops.tail, *secondCandidateValue);
            }
            graph.addOperand(ops.tail, blockerValue);
            if (fixture.sharedTargetInput)
            {
                graph.addOperand(ops.tail, sourceValues.front());
            }
            graph.addResult(ops.tail, tailValue);

            auto output = tailValue;
            const std::size_t tailChainCount =
                fixture.sourceCount - 2 + (fixture.addUnrelatedStateCommit ? 1 : 0);
            for (std::size_t i = 0; i < tailChainCount; ++i)
            {
                const std::string suffix = std::to_string(i);
                const auto value = makeValue(graph, "tail_chain_value_" + suffix, 8);
                const auto op = graph.createOperation(
                    wolvrix::lib::grh::OperationKind::kNot,
                    graph.internSymbol("tail_chain_" + suffix));
                graph.addOperand(op, output);
                graph.addResult(op, value);
                output = value;
            }
            graph.bindOutputPort("output", output);

            if (unrelatedStateRead)
            {
                const auto enable = makeValue(graph, "unrelated_commit_enable", 1);
                const auto mask = makeValue(graph, "unrelated_commit_mask", 8);
                const auto clock = makeValue(graph, "unrelated_commit_clock", 1);
                graph.bindInputPort("unrelated_commit_enable", enable);
                graph.bindInputPort("unrelated_commit_mask", mask);
                graph.bindInputPort("unrelated_commit_clock", clock);
                const auto write = graph.createOperation(
                    wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                    graph.internSymbol("unrelated_commit_write"));
                graph.addOperand(write, enable);
                graph.addOperand(write, *unrelatedStateRead);
                graph.addOperand(write, mask);
                graph.addOperand(write, clock);
                graph.setAttr(write, "regSymbol", std::string("unrelated_state"));
                graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});
            }

            if (fixture.addCommitConsumer)
            {
                const auto enable = makeValue(graph, "commit_enable", 1);
                const auto mask = makeValue(graph, "commit_mask", 8);
                const auto clock = makeValue(graph, "commit_clock", 1);
                graph.bindInputPort("commit_enable", enable);
                graph.bindInputPort("commit_mask", mask);
                graph.bindInputPort("commit_clock", clock);
                const auto reg = graph.createOperation(
                    wolvrix::lib::grh::OperationKind::kRegister,
                    graph.internSymbol("commit_state"));
                graph.setAttr(reg, "width", static_cast<int64_t>(8));
                graph.setAttr(reg, "isSigned", false);
                const auto write = graph.createOperation(
                    wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                    graph.internSymbol("commit_write"));
                graph.addOperand(write, enable);
                graph.addOperand(write, candidateValue);
                graph.addOperand(write, mask);
                graph.addOperand(write, clock);
                graph.setAttr(write, "regSymbol", std::string("commit_state"));
                graph.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});
            }
            return ops;
        };
        const auto runFixture = [&](wolvrix::lib::grh::Design &design,
                                    const std::string &name,
                                    std::optional<std::string> policy,
                                    SessionStore &session,
                                    std::string *log = nullptr,
                                    std::size_t maxSupernodeOps = 5,
                                    std::size_t maxMoves = 4096,
                                    std::size_t movedOpPpm = 1000000,
                                    std::size_t dpSegmentPenaltyPpm = 1000000)
        {
            ActivityScheduleOptions options;
            options.path = name;
            options.maxOpInComputeSupernode = maxSupernodeOps;
            options.maxOpInComputeNode = 1;
            options.enableCoarsen = false;
            options.enableChainMerge = false;
            if (policy)
            {
                options.finalFaninPullbackPolicy = *policy;
            }
            options.finalFaninPullbackMaxNodeOps = 8;
            options.finalFaninPullbackMaxValueWidth = 64;
            options.finalFaninPullbackMinGain = 3;
            options.finalFaninPullbackMaxMoves = maxMoves;
            options.finalFaninPullbackMaxMovedOpPpm = movedOpPpm;
            options.dpSegmentPenaltyPpm = dpSegmentPenaltyPpm;
            PassManager manager;
            manager.options().session = &session;
            if (log != nullptr)
            {
                manager.options().logLevel = wolvrix::lib::LogLevel::Info;
                manager.options().logSink =
                    [log](wolvrix::lib::LogLevel,
                          std::string_view,
                          std::string_view message)
                    {
                        log->append(message);
                        log->push_back('\n');
                    };
            }
            manager.addPass(std::make_unique<ActivitySchedulePass>(options));
            PassDiagnostics diags;
            const PassManagerResult result = manager.run(design, diags);
            return result.success && !result.changed && !diags.hasError();
        };

        std::string parseError;
        const std::vector<std::string_view> separatedArgs{
            "-path", "final_fanin_pullback_probe",
            "-final-fanin-pullback-policy", "probe",
            "-final-fanin-pullback-max-node-ops", "8",
            "-final-fanin-pullback-max-value-width", "64",
            "-final-fanin-pullback-min-gain", "3",
            "-final-fanin-pullback-max-moves", "4096",
            "-final-fanin-pullback-max-moved-op-ppm", "5000"};
        const std::vector<std::string_view> equalsArgs{
            "-path=final_fanin_pullback_probe",
            "-final-fanin-pullback-policy=off",
            "-final-fanin-pullback-max-node-ops=8",
            "-final-fanin-pullback-max-value-width=64",
            "-final-fanin-pullback-min-gain=3",
            "-final-fanin-pullback-max-moves=4096",
            "-final-fanin-pullback-max-moved-op-ppm=5000"};
        const std::vector<std::string_view> malformedArgs{
            "-path=final_fanin_pullback_probe",
            "-final-fanin-pullback-max-node-ops=8x"};
        const std::vector<std::string_view> overflowArgs{
            "-path=final_fanin_pullback_probe",
            "-final-fanin-pullback-max-moves=999999999999999999999999999999999999"};
        if (makePass("activity-schedule", separatedArgs, parseError) == nullptr ||
            makePass("activity-schedule", equalsArgs, parseError) == nullptr ||
            makePass("activity-schedule", malformedArgs, parseError) != nullptr ||
            makePass("activity-schedule", overflowArgs, parseError) != nullptr)
        {
            return fail("Expected final-fanin pullback CLI policy and size forms to parse strictly");
        }

        constexpr std::string_view kName = "final_fanin_pullback_probe";
        wolvrix::lib::grh::Design defaultDesign;
        buildFixture(defaultDesign, std::string(kName), {});
        SessionStore defaultSession;
        if (!runFixture(defaultDesign, std::string(kName), std::nullopt, defaultSession))
        {
            return fail("Expected default final-fanin pullback schedule to succeed");
        }
        wolvrix::lib::grh::Design offDesign;
        buildFixture(offDesign, std::string(kName), {});
        SessionStore offSession;
        if (!runFixture(offDesign, std::string(kName), "off", offSession))
        {
            return fail("Expected explicit-off final-fanin pullback schedule to succeed");
        }
        wolvrix::lib::grh::Design probeDesign;
        const FaninFixtureOps probeOps =
            buildFixture(probeDesign, std::string(kName), {});
        SessionStore probeSession;
        std::string probeLog;
        if (!runFixture(probeDesign, std::string(kName), "probe", probeSession, &probeLog))
        {
            return fail("Expected final-fanin pullback probe schedule to succeed");
        }
        const auto defaultSchedule = loadSchedule(defaultSession, std::string(kName));
        const auto offSchedule = loadSchedule(offSession, std::string(kName));
        const auto probeSchedule = loadSchedule(probeSession, std::string(kName));
        const auto *probeGraph = probeDesign.findGraph(std::string(kName));
        if (probeGraph == nullptr ||
            validateCommonScheduleShape(*probeGraph, probeSchedule) != 0 ||
            !schedulesEqual(defaultSchedule, offSchedule) ||
            !schedulesEqual(offSchedule, probeSchedule) ||
            defaultSession.size() != offSession.size() ||
            offSession.size() != probeSession.size())
        {
            return fail("Expected default/off/probe final-fanin schedule identity");
        }
        if (probeSchedule.opToSupernode == nullptr ||
            probeSchedule.supernodeToOps == nullptr)
        {
            return fail("Expected final-fanin probe owner maps");
        }
        const auto owner = [&](wolvrix::lib::grh::OperationId op)
        {
            return (*probeSchedule.opToSupernode)[op.index - 1];
        };
        const uint32_t sourceOwner = owner(probeOps.sources.front());
        const uint32_t targetOwner = owner(probeOps.candidate);
        if (sourceOwner == targetOwner || owner(probeOps.tail) != targetOwner ||
            !std::all_of(probeOps.sources.begin(), probeOps.sources.end(),
                         [&](const auto op) { return owner(op) == sourceOwner; }) ||
            sourceOwner >= probeSchedule.supernodeToOps->size() ||
            targetOwner >= probeSchedule.supernodeToOps->size() ||
            (*probeSchedule.supernodeToOps)[sourceOwner].size() != 4 ||
            (*probeSchedule.supernodeToOps)[targetOwner].size() != 5)
        {
            return fail("Expected four-input final-fanin source/target fixture shape");
        }
        if (probeLog.find("activity-schedule final-fanin pullback probe:") ==
                std::string::npos ||
            parseStatField(probeLog, "exact_eligible") != 1 ||
            parseStatField(probeLog, "selected") != 1 ||
            parseStatField(probeLog, "eligible_projected_bae_gain") != 3 ||
            parseStatField(probeLog, "projected_bae_gain") != 3 ||
            parseStatField(probeLog, "moved_ops") != 1)
        {
            return fail("Expected four-input final-fanin projected gain three: " + probeLog);
        }

        const auto commitPartition = [](const ScheduleView &schedule)
        {
            std::vector<std::vector<wolvrix::lib::grh::OperationId>> partition;
            if (schedule.supernodeToOps == nullptr || schedule.supernodeKinds == nullptr)
            {
                return partition;
            }
            for (std::size_t node = 0; node < schedule.supernodeToOps->size(); ++node)
            {
                if ((*schedule.supernodeKinds)[node] ==
                    ActivityScheduleSupernodeKind::Commit)
                {
                    partition.push_back((*schedule.supernodeToOps)[node]);
                }
            }
            return partition;
        };
        const auto scheduleOwner = [](const ScheduleView &schedule,
                                      wolvrix::lib::grh::OperationId op)
        {
            return schedule.opToSupernode == nullptr || op.index == 0 ||
                           op.index - 1 >= schedule.opToSupernode->size()
                       ? kInvalidActivitySupernodeId
                       : (*schedule.opToSupernode)[op.index - 1];
        };

        wolvrix::lib::grh::Design strictDesign;
        const FaninFixtureOps strictOps =
            buildFixture(strictDesign, std::string(kName), {});
        SessionStore strictSession;
        std::string strictLog;
        if (!runFixture(strictDesign, std::string(kName), "strict", strictSession, &strictLog))
        {
            return fail("Expected strict final-fanin pullback schedule to succeed: " + strictLog);
        }
        const auto strictSchedule = loadSchedule(strictSession, std::string(kName));
        const auto *strictGraph = strictDesign.findGraph(std::string(kName));
        if (strictGraph == nullptr ||
            validateCommonScheduleShape(*strictGraph, strictSchedule) != 0 ||
            strictSchedule.supernodeToOps == nullptr || strictSchedule.supernodeKinds == nullptr ||
            strictSchedule.computeNodesBySupernode == nullptr ||
            offSchedule.computeNodesBySupernode == nullptr || offSchedule.summaryStats == nullptr ||
            strictSchedule.summaryStats == nullptr)
        {
            return fail("Expected complete strict final-fanin schedule outputs");
        }
        const uint32_t strictSourceOwner = scheduleOwner(strictSchedule,
                                                         strictOps.sources.front());
        const uint32_t strictCandidateOwner = scheduleOwner(strictSchedule,
                                                            strictOps.candidate);
        const uint32_t strictTailOwner = scheduleOwner(strictSchedule, strictOps.tail);
        if (strictSourceOwner != sourceOwner || strictCandidateOwner != sourceOwner ||
            strictTailOwner != targetOwner ||
            !std::all_of(strictOps.sources.begin(),
                         strictOps.sources.end(),
                         [&](const auto op)
                         {
                             return scheduleOwner(strictSchedule, op) == sourceOwner;
                         }) ||
            (*strictSchedule.supernodeToOps)[sourceOwner].size() != 5 ||
            (*strictSchedule.supernodeToOps)[targetOwner].size() != 4)
        {
            return fail("Expected strict final-fanin candidate owner move from target to source");
        }
        std::vector<uint32_t> addedComputeNodes;
        for (const uint32_t node :
             (*strictSchedule.computeNodesBySupernode)[sourceOwner])
        {
            if (std::find((*offSchedule.computeNodesBySupernode)[sourceOwner].begin(),
                          (*offSchedule.computeNodesBySupernode)[sourceOwner].end(),
                          node) ==
                (*offSchedule.computeNodesBySupernode)[sourceOwner].end())
            {
                addedComputeNodes.push_back(node);
            }
        }
        std::vector<uint32_t> removedComputeNodes;
        for (const uint32_t node : (*offSchedule.computeNodesBySupernode)[targetOwner])
        {
            if (std::find((*strictSchedule.computeNodesBySupernode)[targetOwner].begin(),
                          (*strictSchedule.computeNodesBySupernode)[targetOwner].end(),
                          node) ==
                (*strictSchedule.computeNodesBySupernode)[targetOwner].end())
            {
                removedComputeNodes.push_back(node);
            }
        }
        if (addedComputeNodes.size() != 1 || addedComputeNodes != removedComputeNodes)
        {
            return fail("Expected strict final-fanin to move one complete compute node");
        }
        const double offBae =
            parseJsonDoubleField(*offSchedule.summaryStats, "boundary_activation_edges");
        const double strictBae =
            parseJsonDoubleField(*strictSchedule.summaryStats, "boundary_activation_edges");
        const double offComputeBae =
            parseJsonDoubleField(*offSchedule.summaryStats, "compute_compute_value_pairs");
        const double strictComputeBae =
            parseJsonDoubleField(*strictSchedule.summaryStats,
                                 "compute_compute_value_pairs");
        if (offSchedule.supernodeToOps->size() != strictSchedule.supernodeToOps->size() ||
            *offSchedule.supernodeKinds != *strictSchedule.supernodeKinds ||
            *offSchedule.dag != *strictSchedule.dag ||
            *offSchedule.topoOrder != *strictSchedule.topoOrder ||
            *offSchedule.stateReadSupernodes != *strictSchedule.stateReadSupernodes ||
            commitPartition(offSchedule) != commitPartition(strictSchedule) ||
            offBae != strictBae + 3.0 || offComputeBae != strictComputeBae + 3.0)
        {
            return fail("Expected strict final-fanin to preserve schedule structure and reduce BAE by three");
        }
        if (strictLog.find("activity-schedule final-fanin pullback strict:") ==
                std::string::npos ||
            parseStatField(strictLog, "exact_eligible") != 1 ||
            parseStatField(strictLog, "selected") != 1 ||
            parseStatField(strictLog, "applied") != 1 ||
            parseStatField(strictLog, "projected_bae_gain") != 3 ||
            parseStatField(strictLog, "actual_bae_gain") != 3 ||
            parseStatField(strictLog, "compute_bae_before") !=
            parseStatField(strictLog, "compute_bae_after") + 3 ||
            parseStatField(strictLog, "dag_edges_before") !=
                parseStatField(strictLog, "dag_edges_after") ||
            strictLog.find(
                "activity-schedule final-fanin pullback strict validators: "
                "supernodes=true kinds=true scheduled_ops=true capacity=true commit=true "
                "compute_partition=true dag=true topo=true state_read=true "
                "compute_commit=true bae_gain=true") == std::string::npos)
        {
            return fail("Expected strict final-fanin projected and actual gain accounting: " +
                        strictLog);
        }

        wolvrix::lib::grh::Design repeatStrictDesign;
        buildFixture(repeatStrictDesign, std::string(kName), {});
        SessionStore repeatStrictSession;
        std::string repeatStrictLog;
        if (!runFixture(repeatStrictDesign,
                        std::string(kName),
                        "strict",
                        repeatStrictSession,
                        &repeatStrictLog) ||
            !schedulesEqual(strictSchedule,
                            loadSchedule(repeatStrictSession, std::string(kName))) ||
            parseStatField(repeatStrictLog, "applied") != 1 ||
            parseStatField(repeatStrictLog, "actual_bae_gain") != 3)
        {
            return fail("Expected deterministic strict final-fanin pullback");
        }

        for (const auto &[name, maxMoves, movedOpPpm] :
             std::vector<std::tuple<std::string, std::size_t, std::size_t>>{
                 {"final_fanin_pullback_strict_zero_moves", 0, 1000000},
                 {"final_fanin_pullback_strict_zero_ppm", 4096, 0},
             })
        {
            wolvrix::lib::grh::Design budgetDesign;
            buildFixture(budgetDesign, std::string(kName), {});
            SessionStore budgetSession;
            std::string budgetLog;
            if (!runFixture(budgetDesign,
                            std::string(kName),
                            "strict",
                            budgetSession,
                            &budgetLog,
                            5,
                            maxMoves,
                            movedOpPpm) ||
                !schedulesEqual(offSchedule,
                                loadSchedule(budgetSession, std::string(kName))) ||
                parseStatField(budgetLog, "selected") != 0 ||
                parseStatField(budgetLog, "applied") != 0 ||
                parseStatField(budgetLog, "actual_bae_gain") != 0)
            {
                return fail("Expected strict final-fanin zero-budget identity for " + name +
                            ": " + budgetLog);
            }
        }

        {
            const std::string name = "final_fanin_pullback_state_commit";
            FaninFixtureOptions fixture;
            fixture.addUnrelatedStateCommit = true;
            wolvrix::lib::grh::Design stateOffDesign;
            buildFixture(stateOffDesign, name, fixture);
            SessionStore stateOffSession;
            if (!runFixture(stateOffDesign,
                            name,
                            "off",
                            stateOffSession,
                            nullptr,
                            6,
                            4096,
                            1000000,
                            10000000))
            {
                return fail("Expected state/commit strict baseline schedule to succeed");
            }
            wolvrix::lib::grh::Design stateStrictDesign;
            buildFixture(stateStrictDesign, name, fixture);
            SessionStore stateStrictSession;
            std::string stateStrictLog;
            if (!runFixture(stateStrictDesign,
                            name,
                            "strict",
                            stateStrictSession,
                            &stateStrictLog,
                            6,
                            4096,
                            1000000,
                            10000000))
            {
                return fail("Expected state/commit strict schedule to succeed: " +
                            stateStrictLog);
            }
            const auto stateOffSchedule = loadSchedule(stateOffSession, name);
            const auto stateStrictSchedule = loadSchedule(stateStrictSession, name);
            const auto stateOffCommit = commitPartition(stateOffSchedule);
            if (stateOffSchedule.stateReadSupernodes == nullptr ||
                stateOffSchedule.stateReadSupernodes->empty() || stateOffCommit.empty() ||
                stateOffSchedule.summaryStats == nullptr ||
                stateStrictSchedule.summaryStats == nullptr ||
                stateOffSchedule.supernodeToOps->size() !=
                    stateStrictSchedule.supernodeToOps->size() ||
                *stateOffSchedule.supernodeKinds != *stateStrictSchedule.supernodeKinds ||
                *stateOffSchedule.dag != *stateStrictSchedule.dag ||
                *stateOffSchedule.topoOrder != *stateStrictSchedule.topoOrder ||
                *stateOffSchedule.stateReadSupernodes !=
                    *stateStrictSchedule.stateReadSupernodes ||
                stateOffCommit != commitPartition(stateStrictSchedule) ||
                parseJsonDoubleField(*stateOffSchedule.summaryStats,
                                     "boundary_activation_edges") !=
                    parseJsonDoubleField(*stateStrictSchedule.summaryStats,
                                         "boundary_activation_edges") +
                        3.0 ||
                parseStatField(stateStrictLog, "applied") != 1 ||
                parseStatField(stateStrictLog, "actual_bae_gain") != 3)
            {
                return fail("Expected strict final-fanin nonempty state/commit identity: " +
                            stateStrictLog);
            }
        }

        wolvrix::lib::grh::Design repeatDesign;
        buildFixture(repeatDesign, std::string(kName), {});
        SessionStore repeatSession;
        std::string repeatLog;
        if (!runFixture(repeatDesign, std::string(kName), "probe", repeatSession, &repeatLog) ||
            !schedulesEqual(probeSchedule, loadSchedule(repeatSession, std::string(kName))) ||
            parseStatField(repeatLog, "exact_eligible") !=
                parseStatField(probeLog, "exact_eligible") ||
            parseStatField(repeatLog, "selected") != parseStatField(probeLog, "selected") ||
            parseStatField(repeatLog, "projected_bae_gain") !=
                parseStatField(probeLog, "projected_bae_gain"))
        {
            return fail("Expected deterministic final-fanin probe selection");
        }

        const auto expectSelectionBudgetReject = [&](const std::string &name,
                                                     std::size_t maxMoves,
                                                     std::size_t movedOpPpm,
                                                     const std::string &counter)
        {
            wolvrix::lib::grh::Design design;
            buildFixture(design, name, {});
            SessionStore session;
            std::string log;
            return runFixture(design,
                              name,
                              "probe",
                              session,
                              &log,
                              5,
                              maxMoves,
                              movedOpPpm) &&
                   parseStatField(log, "exact_eligible") == 1 &&
                   parseStatField(log, "selected") == 0 &&
                   parseStatField(log, counter) == 1;
        };
        if (!expectSelectionBudgetReject("final_fanin_pullback_zero_moves",
                                         0,
                                         1000000,
                                         "rejected_selection_move_limit") ||
            !expectSelectionBudgetReject("final_fanin_pullback_zero_ppm",
                                         4096,
                                         0,
                                         "rejected_selection_budget"))
        {
            return fail("Expected final-fanin move-count and moved-op budgets to reject selection");
        }

        FaninFixtureOptions sharedFixture;
        sharedFixture.sharedTargetInput = true;
        sharedFixture.duplicateCandidateInput = true;
        sharedFixture.extraSourceResult = true;
        wolvrix::lib::grh::Design sharedDesign;
        buildFixture(sharedDesign, "final_fanin_pullback_shared_input", sharedFixture);
        SessionStore sharedSession;
        std::string sharedLog;
        if (!runFixture(sharedDesign,
                        "final_fanin_pullback_shared_input",
                        "probe",
                        sharedSession,
                        &sharedLog) ||
            parseStatField(sharedLog, "exact_eligible") != 1 ||
            parseStatField(sharedLog, "selected") != 1 ||
            parseStatField(sharedLog, "eligible_projected_bae_gain") != 3 ||
            parseStatField(sharedLog, "projected_bae_gain") != 3)
        {
            return fail("Expected shared target input to count only four distinct removable inputs: " +
                        sharedLog);
        }

        {
            const std::string name = "final_fanin_pullback_implicit_index";
            const std::string intentGroup = "final_fanin_hidden_index";
            wolvrix::lib::grh::Design design;
            auto &graph = design.createGraph(name);
            design.markAsTop(name);
            std::vector<wolvrix::lib::grh::ValueId> sourceValues;
            for (std::size_t i = 0; i < 4; ++i)
            {
                const std::string suffix = std::to_string(i);
                const auto input = makeValue(graph, "source_input_" + suffix, 8);
                graph.bindInputPort("source_input_" + suffix, input);
                const auto value = makeValue(graph, "source_value_" + suffix, 8);
                const auto op = graph.createOperation(
                    wolvrix::lib::grh::OperationKind::kNot,
                    graph.internSymbol("source_" + suffix));
                graph.addOperand(op, input);
                graph.addResult(op, value);
                sourceValues.push_back(value);
                if (i == 0)
                {
                    const auto extra = makeValue(graph, "source_extra_value", 8);
                    graph.addResult(op, extra);
                    sourceValues.push_back(extra);
                }
            }
            for (std::size_t i = 0; i < 3; ++i)
            {
                const std::string suffix = std::to_string(i);
                const auto input = makeValue(graph, "padding_input_" + suffix, 8);
                graph.bindInputPort("padding_input_" + suffix, input);
                const auto value = makeValue(graph, "padding_value_" + suffix, 8);
                const auto op = graph.createOperation(
                    wolvrix::lib::grh::OperationKind::kNot,
                    graph.internSymbol("padding_" + suffix));
                graph.addOperand(op, input);
                graph.addResult(op, value);
                graph.bindOutputPort("padding_" + suffix, value);
            }

            const auto blockerInput = makeValue(graph, "blocker_input", 8);
            graph.bindInputPort("blocker_input", blockerInput);
            const auto blockerValue = makeValue(graph, "blocker_value", 8);
            const auto blocker = graph.createOperation(
                wolvrix::lib::grh::OperationKind::kNot,
                graph.internSymbol("blocker"));
            graph.addOperand(blocker, blockerInput);
            graph.addResult(blocker, blockerValue);
            const auto candidateValue = makeValue(graph, "candidate_value", 8);
            const auto candidate = graph.createOperation(
                wolvrix::lib::grh::OperationKind::kAnd,
                graph.internSymbol("candidate"));
            for (const auto value : sourceValues)
            {
                graph.addOperand(candidate, value);
            }
            graph.addResult(candidate, candidateValue);
            auto output = makeValue(graph, "tail_value", 8);
            const auto tail = graph.createOperation(
                wolvrix::lib::grh::OperationKind::kXor,
                graph.internSymbol("tail"));
            graph.addOperand(tail, candidateValue);
            graph.addOperand(tail, blockerValue);
            graph.addResult(tail, output);
            for (std::size_t i = 0; i < 2; ++i)
            {
                const std::string suffix = std::to_string(i);
                const auto value = makeValue(graph, "tail_chain_value_" + suffix, 8);
                const auto op = graph.createOperation(
                    wolvrix::lib::grh::OperationKind::kNot,
                    graph.internSymbol("tail_chain_" + suffix));
                graph.addOperand(op, output);
                graph.addResult(op, value);
                output = value;
            }
            graph.bindOutputPort("output", output);

            const auto reg = graph.createOperation(
                wolvrix::lib::grh::OperationKind::kRegister,
                graph.internSymbol("intent_reg"));
            graph.setAttr(reg, "width", int64_t{8});
            graph.setAttr(reg, "isSigned", false);
            setIntentShape(graph, reg, intentGroup, "register", 8, 1);
            graph.setAttr(reg, "regToMem.intent.row", int64_t{0});
            const auto readValue = makeValue(graph, "intent_read_value", 8);
            const auto read = graph.createOperation(
                wolvrix::lib::grh::OperationKind::kRegisterReadPort,
                graph.internSymbol("intent_read"));
            graph.addResult(read, readValue);
            graph.setAttr(read, "regSymbol", std::string("intent_reg"));
            graph.setAttr(read, "regToMem.intent.group", intentGroup);
            graph.setAttr(read, "regToMem.intent.mode", std::string("array-index"));
            graph.setAttr(read, "regToMem.intent.role", std::string("read"));
            graph.setAttr(read, "regToMem.intent.row", int64_t{0});
            const auto packed = makeValue(graph, "intent_packed", 8);
            const auto concat = graph.createOperation(
                wolvrix::lib::grh::OperationKind::kConcat,
                graph.internSymbol("intent_concat"));
            graph.addOperand(concat, readValue);
            graph.addResult(concat, packed);
            setIntentShape(graph, concat, intentGroup, "concat", 8, 1);
            graph.setAttr(concat,
                          "regToMem.intent.regSymbols",
                          std::vector<std::string>{"intent_reg"});
            graph.setAttr(concat,
                          "regToMem.intent.operandRows",
                          std::vector<int64_t>{0});
            const auto elemWidth =
                makeConstant(graph, "intent_width_const", "intent_width", 8, "8'd8");
            const auto start = makeValue(graph, "intent_start", 8);
            const auto mul = graph.createOperation(
                wolvrix::lib::grh::OperationKind::kMul,
                graph.internSymbol("intent_start_mul"));
            graph.addOperand(mul, sourceValues.front());
            graph.addOperand(mul, elemWidth);
            graph.addResult(mul, start);
            const auto selected = makeValue(graph, "intent_selected", 8);
            const auto slice = graph.createOperation(
                wolvrix::lib::grh::OperationKind::kSliceDynamic,
                graph.internSymbol("intent_slice"));
            graph.addOperand(slice, packed);
            graph.addOperand(slice, start);
            graph.addResult(slice, selected);
            graph.setAttr(slice, "sliceWidth", int64_t{8});
            setIntentShape(graph, slice, intentGroup, "slice", 8, 1);
            graph.setAttr(slice,
                          "regToMem.intent.sliceKind",
                          std::string("slice-dynamic"));
            graph.bindOutputPort("intent_selected", selected);

            ActivityScheduleOptions options;
            options.path = name;
            options.maxOpInComputeSupernode = 8;
            options.maxOpInComputeNode = 1;
            options.enableCoarsen = false;
            options.enableChainMerge = false;
            options.finalFaninPullbackPolicy = "probe";
            options.finalFaninPullbackMaxMovedOpPpm = 1000000;
            SessionStore session;
            std::string log;
            PassManager manager;
            manager.options().session = &session;
            manager.options().logLevel = wolvrix::lib::LogLevel::Info;
            manager.options().logSink =
                [&log](wolvrix::lib::LogLevel,
                       std::string_view,
                       std::string_view message)
                {
                    log.append(message);
                    log.push_back('\n');
                };
            manager.addPass(std::make_unique<ActivitySchedulePass>(options));
            PassDiagnostics diags;
            const PassManagerResult result = manager.run(design, diags);
            const auto schedule = loadSchedule(session, name);
            const auto ownerOf = [&](wolvrix::lib::grh::OperationId op)
            {
                return schedule.opToSupernode == nullptr ||
                               op.index - 1 >= schedule.opToSupernode->size()
                           ? kInvalidActivitySupernodeId
                           : (*schedule.opToSupernode)[op.index - 1];
            };
            if (!result.success || diags.hasError() ||
                ownerOf(mul) != kInvalidActivitySupernodeId ||
                parseStatField(log, "exact_eligible") != 1 ||
                parseStatField(log, "selected") != 1 ||
                parseStatField(log, "rejected_restricted") == 0 ||
                parseStatField(log, "eligible_projected_bae_gain") != 3 ||
                log.find(" projected_bae_gain=3") == std::string::npos)
            {
                return fail("Expected hidden reg-to-mem index to make one input non-removable: " +
                            log);
            }

        }

        {
            const std::string name = "final_fanin_pullback_cumulative_capacity";
            wolvrix::lib::grh::Design design;
            auto &graph = design.createGraph(name);
            design.markAsTop(name);
            std::vector<wolvrix::lib::grh::ValueId> sourceValues;
            const auto sourceInput = makeValue(graph, "source_input", 8);
            graph.bindInputPort("source_input", sourceInput);
            const auto source = graph.createOperation(
                wolvrix::lib::grh::OperationKind::kNot,
                graph.internSymbol("source"));
            graph.addOperand(source, sourceInput);
            for (std::size_t i = 0; i < 8; ++i)
            {
                const std::string suffix = std::to_string(i);
                const auto value = makeValue(graph, "source_value_" + suffix, 8);
                graph.addResult(source, value);
                sourceValues.push_back(value);
            }
            auto middle = makeValue(graph, "middle_input", 8);
            graph.bindInputPort("middle_input", middle);
            for (std::size_t i = 0; i < 2; ++i)
            {
                const std::string suffix = std::to_string(i);
                const auto value = makeValue(graph, "middle_value_" + suffix, 8);
                const auto op = graph.createOperation(
                    wolvrix::lib::grh::OperationKind::kNot,
                    graph.internSymbol("middle_" + suffix));
                graph.addOperand(op, middle);
                graph.addResult(op, value);
                middle = value;
            }
            graph.bindOutputPort("middle", middle);
            std::vector<wolvrix::lib::grh::OperationId> candidateOps;
            for (std::size_t group = 0; group < 2; ++group)
            {
                const std::string prefix = "group_" + std::to_string(group) + "_";
                const auto candidateValue = makeValue(graph, prefix + "candidate_value", 8);
                const auto candidate = graph.createOperation(
                    wolvrix::lib::grh::OperationKind::kAnd,
                    graph.internSymbol(prefix + "candidate"));
                for (std::size_t i = group * 4; i < group * 4 + 4; ++i)
                {
                    graph.addOperand(candidate, sourceValues[i]);
                }
                graph.addResult(candidate, candidateValue);
                candidateOps.push_back(candidate);
                auto output = makeValue(graph, prefix + "tail_value", 8);
                const auto tail = graph.createOperation(
                    wolvrix::lib::grh::OperationKind::kNot,
                    graph.internSymbol(prefix + "tail"));
                graph.addOperand(tail, candidateValue);
                graph.addResult(tail, output);
                graph.bindOutputPort(prefix + "output", output);
            }
            ActivityScheduleOptions options;
            options.path = name;
            options.maxOpInComputeSupernode = 2;
            options.maxOpInComputeNode = 1;
            options.enableCoarsen = true;
            options.enableChainMerge = true;
            options.finalFaninPullbackPolicy = "probe";
            options.finalFaninPullbackMaxMovedOpPpm = 1000000;
            SessionStore session;
            std::string log;
            PassManager manager;
            manager.options().session = &session;
            manager.options().logLevel = wolvrix::lib::LogLevel::Info;
            manager.options().logSink =
                [&log](wolvrix::lib::LogLevel,
                       std::string_view,
                       std::string_view message)
                {
                    log.append(message);
                    log.push_back('\n');
                };
            manager.addPass(std::make_unique<ActivitySchedulePass>(options));
            PassDiagnostics diags;
            const PassManagerResult result = manager.run(design, diags);
            if (!result.success || result.changed || diags.hasError() ||
                parseStatField(log, "exact_eligible") != 2 ||
                parseStatField(log, "selected") != 1 ||
                parseStatField(log, "eligible_projected_bae_gain") != 6 ||
                log.find(" projected_bae_gain=3") == std::string::npos ||
                parseStatField(log, "rejected_selection_capacity") != 1)
            {
                return fail("Expected cumulative source capacity to select one candidate: " + log);
            }

            const auto probeCapacitySchedule = loadSchedule(session, name);
            ActivityScheduleOptions strictOptions = options;
            strictOptions.finalFaninPullbackPolicy = "strict";
            SessionStore strictCapacitySession;
            std::string strictCapacityLog;
            PassManager strictManager;
            strictManager.options().session = &strictCapacitySession;
            strictManager.options().logLevel = wolvrix::lib::LogLevel::Info;
            strictManager.options().logSink =
                [&strictCapacityLog](wolvrix::lib::LogLevel,
                                     std::string_view,
                                     std::string_view message)
                {
                    strictCapacityLog.append(message);
                    strictCapacityLog.push_back('\n');
                };
            strictManager.addPass(std::make_unique<ActivitySchedulePass>(strictOptions));
            PassDiagnostics strictDiags;
            const PassManagerResult strictResult = strictManager.run(design, strictDiags);
            const auto strictCapacitySchedule =
                loadSchedule(strictCapacitySession, name);
            const uint32_t capacitySource = scheduleOwner(probeCapacitySchedule, source);
            std::size_t movedCandidates = 0;
            bool unmovedCandidateStable = true;
            for (const auto candidate : candidateOps)
            {
                const uint32_t before = scheduleOwner(probeCapacitySchedule, candidate);
                const uint32_t after = scheduleOwner(strictCapacitySchedule, candidate);
                if (before != capacitySource && after == capacitySource)
                {
                    ++movedCandidates;
                }
                else if (before != after)
                {
                    unmovedCandidateStable = false;
                }
            }
            if (!strictResult.success || strictResult.changed || strictDiags.hasError() ||
                movedCandidates != 1 || !unmovedCandidateStable ||
                parseStatField(strictCapacityLog, "exact_eligible") != 2 ||
                parseStatField(strictCapacityLog, "selected") != 1 ||
                parseStatField(strictCapacityLog, "applied") != 1 ||
                strictCapacityLog.find(" projected_bae_gain=3") == std::string::npos ||
                parseStatField(strictCapacityLog, "actual_bae_gain") != 3 ||
                parseStatField(strictCapacityLog, "rejected_capacity") != 1 ||
                probeCapacitySchedule.supernodeToOps->size() !=
                    strictCapacitySchedule.supernodeToOps->size() ||
                *probeCapacitySchedule.supernodeKinds !=
                    *strictCapacitySchedule.supernodeKinds ||
                *probeCapacitySchedule.dag != *strictCapacitySchedule.dag ||
                *probeCapacitySchedule.topoOrder != *strictCapacitySchedule.topoOrder ||
                *probeCapacitySchedule.stateReadSupernodes !=
                    *strictCapacitySchedule.stateReadSupernodes ||
                commitPartition(probeCapacitySchedule) !=
                    commitPartition(strictCapacitySchedule))
            {
                return fail("Expected strict cumulative capacity to apply only selected move: " +
                            strictCapacityLog);
            }
        }

        std::string rejectedFixtureFailure;
        const auto expectRejected = [&](const std::string &name,
                                        const FaninFixtureOptions &fixture,
                                        const std::string &counter)
        {
            wolvrix::lib::grh::Design design;
            buildFixture(design, name, fixture);
            SessionStore session;
            std::string log;
            const bool accepted = runFixture(design, name, "probe", session, &log) &&
                                  log.find("activity-schedule final-fanin pullback probe:") !=
                                      std::string::npos &&
                                  parseStatField(log, "scanned") != 0 &&
                                  parseStatField(log, "exact_eligible") == 0 &&
                                  parseStatField(log, "selected") == 0 &&
                                  parseStatField(log, counter) != 0;
            if (!accepted)
            {
                rejectedFixtureFailure = name + " expected " + counter + ": " + log;
            }
            return accepted;
        };
        FaninFixtureOptions multiLiveout;
        multiLiveout.secondCandidateLiveout = true;
        FaninFixtureOptions outputLiveout;
        outputLiveout.bindCandidateOutput = true;
        FaninFixtureOptions declaredLiveout;
        declaredLiveout.declareCandidate = true;
        FaninFixtureOptions commitConsumer;
        commitConsumer.addCommitConsumer = true;
        FaninFixtureOptions fullSource;
        fullSource.fillSourceCapacity = true;
        FaninFixtureOptions anchoredUnused;
        anchoredUnused.anchoredUnusedResult = true;
        FaninFixtureOptions noDef;
        noDef.candidateUsesNoDef = true;
        FaninFixtureOptions targetPredecessor;
        targetPredecessor.candidateUsesTargetPredecessor = true;
        FaninFixtureOptions sideEffect;
        sideEffect.candidateHasSideEffects = true;
        FaninFixtureOptions cloneForbidden;
        cloneForbidden.candidateHasCloneForbiddenAttr = true;
        FaninFixtureOptions wide;
        wide.wideCandidateResult = true;
        if (!expectRejected("final_fanin_pullback_multi_liveout",
                            multiLiveout,
                            "rejected_liveout_count") ||
            !expectRejected("final_fanin_pullback_output_liveout",
                            outputLiveout,
                            "rejected_port_or_declared") ||
            !expectRejected("final_fanin_pullback_declared_liveout",
                            declaredLiveout,
                            "rejected_port_or_declared") ||
            !expectRejected("final_fanin_pullback_anchored_unused",
                            anchoredUnused,
                            "rejected_port_or_declared") ||
            !expectRejected("final_fanin_pullback_commit_consumer",
                            commitConsumer,
                            "rejected_external_consumer") ||
            !expectRejected("final_fanin_pullback_full_source",
                            fullSource,
                            "rejected_capacity") ||
            !expectRejected("final_fanin_pullback_no_def", noDef, "rejected_no_def") ||
            !expectRejected("final_fanin_pullback_target_predecessor",
                            targetPredecessor,
                            "rejected_target_predecessor") ||
            !expectRejected("final_fanin_pullback_side_effect",
                            sideEffect,
                            "rejected_side_effect") ||
            !expectRejected("final_fanin_pullback_clone_forbidden",
                            cloneForbidden,
                            "rejected_clone_forbidden") ||
            !expectRejected("final_fanin_pullback_wide", wide, "rejected_width"))
        {
            return fail("Expected final-fanin rejection: " + rejectedFixtureFailure);
        }

        const auto runInvalid = [&](const std::string &policy, std::size_t movedOpPpm)
        {
            wolvrix::lib::grh::Design design;
            buildFixture(design, "final_fanin_pullback_invalid_options", {});
            ActivityScheduleOptions options;
            options.path = "final_fanin_pullback_invalid_options";
            options.finalFaninPullbackPolicy = policy;
            options.finalFaninPullbackMaxMovedOpPpm = movedOpPpm;
            SessionStore session;
            PassManager manager;
            manager.options().session = &session;
            manager.addPass(std::make_unique<ActivitySchedulePass>(options));
            PassDiagnostics diags;
            const PassManagerResult result = manager.run(design, diags);
            return !result.success && diags.hasError();
        };
        const auto runStrictGuardFailure = [&](const std::string &name,
                                               const std::string &finalTopoPolicy,
                                               bool forceActualSplit)
        {
            wolvrix::lib::grh::Design design;
            buildFixture(design, name, {});
            ActivityScheduleOptions options;
            options.path = name;
            options.maxOpInComputeSupernode = 5;
            options.maxOpInComputeNode = forceActualSplit ? 2 : 1;
            options.enableCoarsen = false;
            options.enableChainMerge = false;
            options.finalTopoPolicy = finalTopoPolicy;
            options.finalFaninPullbackPolicy = "strict";
            options.finalFaninPullbackMaxMovedOpPpm = 1000000;
            options.splitOversizeComputeNodes = forceActualSplit;
            options.splitOversizeComputeNodeMaxOps = forceActualSplit ? 1 : 0;
            SessionStore session;
            PassManager manager;
            manager.options().session = &session;
            manager.addPass(std::make_unique<ActivitySchedulePass>(options));
            PassDiagnostics diags;
            const PassManagerResult result = manager.run(design, diags);
            return !result.success && diags.hasError();
        };
        if (!runInvalid("invalid", 5000) || !runInvalid("off", 1000001) ||
            !runStrictGuardFailure("final_fanin_pullback_strict_non_level",
                                   "level-op",
                                   false) ||
            !runStrictGuardFailure("final_fanin_pullback_strict_actual_split",
                                   "level-id",
                                   true))
        {
            return fail("Expected invalid final-fanin options and strict guards to fail");
        }
    }

    {
        currentCase = "final_topo_level_op";
        wolvrix::lib::grh::Design design;
        auto &graph = design.createGraph("final_topo_level_op");
        design.markAsTop("final_topo_level_op");

        const auto a = makeValue(graph, "a", 8);
        const auto b = makeValue(graph, "b", 8);
        graph.bindInputPort("a", a);
        graph.bindInputPort("b", b);
        const auto notAValue = makeValue(graph, "not_a_value", 8);
        const auto notA = graph.createOperation(wolvrix::lib::grh::OperationKind::kNot,
                                                graph.internSymbol("not_a"));
        graph.addOperand(notA, a);
        graph.addResult(notA, notAValue);
        graph.bindOutputPort("not_a", notAValue);

        const auto notBValue = makeValue(graph, "not_b_value", 8);
        const auto notB = graph.createOperation(wolvrix::lib::grh::OperationKind::kNot,
                                                graph.internSymbol("not_b"));
        graph.addOperand(notB, b);
        graph.addResult(notB, notBValue);
        graph.bindOutputPort("not_b", notBValue);

        const auto notCValue = makeValue(graph, "not_c_value", 8);
        const auto notC = graph.createOperation(wolvrix::lib::grh::OperationKind::kNot,
                                                graph.internSymbol("not_c"));
        graph.addOperand(notC, notBValue);
        graph.addResult(notC, notCValue);
        graph.bindOutputPort("not_c", notCValue);

        const auto runSchedule = [&](const std::string &policy, SessionStore &session) -> bool
        {
            PassManager manager;
            manager.options().session = &session;
            manager.addPass(std::make_unique<ActivitySchedulePass>(ActivityScheduleOptions{
                .path = "final_topo_level_op",
                .maxOpInComputeSupernode = 1,
                .maxOpInComputeNode = 1,
                .enableCoarsen = false,
                .finalTopoPolicy = policy,
            }));
            PassDiagnostics diags;
            const PassManagerResult runResult = manager.run(design, diags);
            return runResult.success && !diags.hasError();
        };

        SessionStore levelIdSession;
        if (!runSchedule("level-id", levelIdSession))
        {
            return fail("Expected level-id final topo schedule to succeed");
        }
        SessionStore levelOpSession;
        if (!runSchedule("level-op", levelOpSession))
        {
            return fail("Expected level-op final topo schedule to succeed");
        }
        SessionStore readyOpSession;
        if (!runSchedule("ready-op", readyOpSession))
        {
            return fail("Expected ready-op final topo schedule to succeed");
        }
        const auto levelId = loadSchedule(levelIdSession, "final_topo_level_op");
        const auto levelOp = loadSchedule(levelOpSession, "final_topo_level_op");
        const auto readyOp = loadSchedule(readyOpSession, "final_topo_level_op");
        if (const int rc = validateCommonScheduleShape(graph, levelId); rc != 0)
        {
            return rc;
        }
        if (const int rc = validateCommonScheduleShape(graph, levelOp); rc != 0)
        {
            return rc;
        }
        if (const int rc = validateCommonScheduleShape(graph, readyOp); rc != 0)
        {
            return rc;
        }
        if (*levelId.supernodeToOps != *levelOp.supernodeToOps ||
            *levelId.opToSupernode != *levelOp.opToSupernode ||
            *levelId.dag != *levelOp.dag ||
            *levelId.valueFanout != *levelOp.valueFanout ||
            *levelId.supernodeKinds != *levelOp.supernodeKinds ||
            *levelId.computeNodesBySupernode != *levelOp.computeNodesBySupernode ||
            *levelId.summaryStats != *levelOp.summaryStats)
        {
            return fail("Expected final topo policy to leave schedule structure unchanged");
        }
        if (*levelId.supernodeToOps != *readyOp.supernodeToOps ||
            *levelId.opToSupernode != *readyOp.opToSupernode ||
            *levelId.dag != *readyOp.dag ||
            *levelId.valueFanout != *readyOp.valueFanout ||
            *levelId.supernodeKinds != *readyOp.supernodeKinds ||
            *levelId.computeNodesBySupernode != *readyOp.computeNodesBySupernode ||
            *levelId.summaryStats != *readyOp.summaryStats)
        {
            return fail("Expected ready-op final topo policy to leave schedule structure unchanged");
        }
        if (const int rc = validateLevelOpTopoOrder(levelOp); rc != 0)
        {
            return rc;
        }
        if (const int rc = validateReadyOpTopoOrder(readyOp); rc != 0)
        {
            return rc;
        }
        if (*levelOp.topoOrder == *readyOp.topoOrder)
        {
            return fail("Expected ready-op to cross a complete Kahn layer boundary");
        }
    }

    return 0;
    }
    catch (const std::exception &ex)
    {
        return fail(std::string("Unhandled exception in ") + currentCase + ": " + ex.what());
    }
}
