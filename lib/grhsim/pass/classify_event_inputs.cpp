#include "grhsim/pass/classify_event_inputs.hpp"
#include "grhsim/ir/model.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace wolvrix::lib::grhsim
{

    namespace
    {
        // grhsim.classify-event-inputs: marks core.input.read ops whose result
        // is used exclusively as event operands (the trailing
        // len(event_edges) operand slots of event-sensitive ops) with a bool
        // parameter event_only=true. Inputs with any data-path use (e.g. rst
        // feeding a mux select) stay unmarked; unused inputs stay unmarked.
        // Pure-event inputs skip the P_input data-activity path in the
        // six-phase model and are detected in P_event instead.
        class ClassifyEventInputsPass final : public Pass
        {
        public:
            ClassifyEventInputsPass()
                : Pass("grhsim.classify-event-inputs", PassKind::SemanticTransform) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                std::vector<uint8_t> eventUse(model.values().size() + 1, 0);
                std::vector<uint8_t> dataUse(model.values().size() + 1, 0);
                for (const auto &op : model.operations())
                {
                    const auto operands = model.operands(op);
                    std::size_t eventTail = operands.size();
                    for (const auto &parameter : model.parameters(op))
                    {
                        if (model.text(parameter.name) != "event_edges") continue;
                        const auto *edges = std::get_if<std::vector<std::string>>(&parameter.value);
                        if (edges && edges->size() <= operands.size())
                            eventTail = operands.size() - edges->size();
                    }
                    for (std::size_t i = 0; i < operands.size(); ++i)
                    {
                        if (i >= eventTail)
                            eventUse[operands[i].index] = 1;
                        else
                            dataUse[operands[i].index] = 1;
                    }
                }

                uint32_t marked = 0;
                for (const auto &op : model.operations())
                {
                    if (model.text(op.opType) != "core.input.read") continue;
                    const auto results = model.results(op);
                    if (results.size() != 1) continue;
                    const auto result = results.front();
                    if (!eventUse[result.index] || dataUse[result.index]) continue;
                    const auto parameters = model.parameters(op);
                    bool already = false;
                    for (const auto &parameter : parameters)
                    {
                        if (model.text(parameter.name) != "event_only") continue;
                        const auto *flag = std::get_if<bool>(&parameter.value);
                        already = flag && *flag;
                    }
                    if (already) continue;
                    std::vector<Parameter> replacement(parameters.begin(), parameters.end());
                    replacement.erase(std::remove_if(replacement.begin(), replacement.end(),
                                                     [&](const Parameter &parameter) {
                                                         return model.text(parameter.name) == "event_only";
                                                     }),
                                      replacement.end());
                    replacement.push_back(Parameter{model.intern("event_only"), true});
                    const std::vector<ValueId> operands(model.operands(op).begin(),
                                                        model.operands(op).end());
                    const std::vector<ValueId> resultsCopy(results.begin(), results.end());
                    const std::vector<ObjectRef> refs(model.objectRefs(op).begin(),
                                                      model.objectRefs(op).end());
                    const auto opType = std::string(model.text(op.opType));
                    model.replaceOperation(op.id, opType, operands, resultsCopy, refs, replacement);
                    ++marked;
                }
                if (marked != 0)
                {
                    // Repack the orphaned ranges left by replaceOperation.
                    model.compact(std::vector<uint8_t>(model.operations().size() + 1, 0),
                                  std::vector<uint8_t>(model.states().size() + 1, 0));
                }
                diagnostics.info("event_only_inputs=" + std::to_string(marked), name());
                return {true, marked != 0, {}};
            }
        };
    } // namespace

    void registerClassifyEventInputsPass(PassRegistry &registry)
    {
        std::string error;
        registry.registerPass(
            "grhsim.classify-event-inputs", PassKind::SemanticTransform,
            [](std::span<const std::string_view> args, std::string &factoryError) {
                if (!args.empty())
                {
                    factoryError = "grhsim.classify-event-inputs does not accept arguments";
                    return std::unique_ptr<Pass>{};
                }
                return std::unique_ptr<Pass>(std::make_unique<ClassifyEventInputsPass>());
            },
            error);
    }

} // namespace wolvrix::lib::grhsim
