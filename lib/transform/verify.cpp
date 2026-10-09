#include "transform/verify.hpp"

#include "core/grh.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wolvrix::lib::transform
{

    namespace
    {
        using wolvrix::lib::grh::AttributeValue;
        using wolvrix::lib::grh::Graph;
        using wolvrix::lib::grh::InoutPort;
        using wolvrix::lib::grh::Operation;
        using wolvrix::lib::grh::OperationId;
        using wolvrix::lib::grh::OperationKind;
        using wolvrix::lib::grh::Port;
        using wolvrix::lib::grh::Value;
        using wolvrix::lib::grh::ValueId;
        using wolvrix::lib::grh::ValueType;

        std::string shapeText(std::span<const int64_t> shape)
        {
            std::string out = "[";
            bool first = true;
            for (const int64_t dim : shape)
            {
                if (!first)
                {
                    out.push_back(',');
                }
                out += std::to_string(dim);
                first = false;
            }
            out.push_back(']');
            return out;
        }

        bool shapeDimsValid(std::span<const int64_t> shape)
        {
            for (const int64_t dim : shape)
            {
                if (dim <= 0)
                {
                    return false;
                }
            }
            return true;
        }

        bool shapeElementCount(std::span<const int64_t> shape, int64_t &count)
        {
            count = 1;
            for (const int64_t dim : shape)
            {
                if (dim <= 0 || count > std::numeric_limits<int64_t>::max() / dim)
                {
                    return false;
                }
                count *= dim;
            }
            return true;
        }

        std::optional<std::string> shapeProblem(int32_t width, ValueType type,
                                                std::span<const int64_t> shape)
        {
            if (shape.empty())
            {
                return std::nullopt;
            }
            if (type != ValueType::Logic)
            {
                return std::string("shape requires a Logic value, got type ") +
                       std::string(wolvrix::lib::grh::toString(type));
            }
            if (!shapeDimsValid(shape))
            {
                return "shape dimensions must be positive, got " + shapeText(shape);
            }
            int64_t elementCount = 0;
            if (!shapeElementCount(shape, elementCount))
            {
                return "shape dimension product overflows int64: " + shapeText(shape);
            }
            if (width <= 0 || elementCount > width || width % elementCount != 0)
            {
                return "shape " + shapeText(shape) + " product " + std::to_string(elementCount) +
                       " must divide total width " + std::to_string(width);
            }
            return std::nullopt;
        }

        template <typename T>
        std::optional<T> attrAs(const Operation &op, std::string_view key)
        {
            std::optional<AttributeValue> attr = op.attr(key);
            if (!attr)
            {
                return std::nullopt;
            }
            if (const T *value = std::get_if<T>(&*attr))
            {
                return *value;
            }
            return std::nullopt;
        }

        bool shapesEqual(std::span<const int64_t> lhs, std::span<const int64_t> rhs)
        {
            if (lhs.size() != rhs.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < lhs.size(); ++i)
            {
                if (lhs[i] != rhs[i])
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace

    GrhVerifyPass::GrhVerifyPass()
        : Pass("grh.verify", "grh.verify", "Structural verifier for GRH graphs")
    {
    }

    PassResult GrhVerifyPass::run()
    {
        PassResult result;
        for (const std::string &graphName : design().graphOrder())
        {
            Graph *graph = design().findGraph(graphName);
            if (!graph)
            {
                continue;
            }

            for (const std::string &entry : graph->validateDeclaredSymbols())
            {
                error(*graph, "grh.verify: " + entry);
            }

            for (const ValueId valueId : graph->values())
            {
                const Value value = graph->getValue(valueId);
                const std::string name(value.symbolText());
                if (value.type() == ValueType::Logic && value.width() <= 0)
                {
                    error(*graph, value, "grh.verify: Logic value width must be positive");
                }
                if (auto problem = shapeProblem(value.width(), value.type(), value.shape()))
                {
                    error(*graph, value, "grh.verify: " + *problem);
                }
                (void)name;
            }

            for (const Port &port : graph->inputPorts())
            {
                if (!graph->valueIsInput(port.value))
                {
                    error(*graph, "grh.verify: input port " + port.name +
                                      " binds a value without the isInput flag");
                }
            }
            for (const Port &port : graph->outputPorts())
            {
                if (!graph->valueIsOutput(port.value))
                {
                    error(*graph, "grh.verify: output port " + port.name +
                                      " binds a value without the isOutput flag");
                }
            }
            for (const InoutPort &port : graph->inoutPorts())
            {
                for (const ValueId member : {port.in, port.out, port.oe})
                {
                    if (!graph->valueIsInout(member))
                    {
                        error(*graph, "grh.verify: inout port " + port.name +
                                          " binds a value without the isInout flag");
                    }
                }
            }

            for (const OperationId opId : graph->operations())
            {
                const Operation op = graph->getOperation(opId);
                switch (op.kind())
                {
                case OperationKind::kRegister:
                case OperationKind::kLatch:
                case OperationKind::kMemory:
                {
                    if (!op.operands().empty() || !op.results().empty())
                    {
                        error(*graph, op, "grh.verify: declaration op must not have operands/results");
                    }
                    std::optional<int64_t> width = attrAs<int64_t>(op, "width");
                    if (!width || *width <= 0)
                    {
                        error(*graph, op, "grh.verify: declaration op requires a positive int width attr");
                    }
                    if (!attrAs<bool>(op, "isSigned"))
                    {
                        error(*graph, op, "grh.verify: declaration op requires a bool isSigned attr");
                    }
                    if (op.kind() == OperationKind::kMemory)
                    {
                        std::optional<int64_t> row = attrAs<int64_t>(op, "row");
                        if (!row || *row <= 0)
                        {
                            error(*graph, op, "grh.verify: kMemory requires a positive int row attr");
                        }
                        break;
                    }
                    // `shape` is part of the declaration type contract (like
                    // `width`/`isSigned`), not a free-form attribute.
                    std::optional<AttributeValue> shapeAttr = op.attr("shape");
                    if (op.kind() == OperationKind::kLatch)
                    {
                        if (shapeAttr)
                        {
                            error(*graph, op, "grh.verify: kLatch must not carry a shape attr");
                        }
                        break;
                    }
                    if (!shapeAttr)
                    {
                        break;
                    }
                    const auto *dims = std::get_if<std::vector<int64_t>>(&*shapeAttr);
                    if (!dims)
                    {
                        error(*graph, op, "grh.verify: kRegister shape attr must be int[]");
                        break;
                    }
                    if (width)
                    {
                        if (auto problem =
                                shapeProblem(static_cast<int32_t>(*width), ValueType::Logic, *dims))
                        {
                            error(*graph, op, "grh.verify: kRegister " + *problem);
                        }
                    }
                    if (dims->empty())
                    {
                        error(*graph, op, "grh.verify: kRegister shape attr must be non-empty when present");
                    }
                    break;
                }
                case OperationKind::kInstance:
                {
                    std::optional<std::string> moduleName = attrAs<std::string>(op, "moduleName");
                    if (!moduleName || moduleName->empty())
                    {
                        error(*graph, op, "grh.verify: kInstance requires a non-empty moduleName attr");
                        break;
                    }
                    Graph *target = design().findGraph(*moduleName);
                    if (!target)
                    {
                        error(*graph, op, "grh.verify: kInstance target graph not found: " + *moduleName);
                        break;
                    }
                    std::optional<std::vector<std::string>> inputNames = attrAs<std::vector<std::string>>(op, "inputPortName");
                    std::optional<std::vector<std::string>> outputNames = attrAs<std::vector<std::string>>(op, "outputPortName");
                    std::optional<std::vector<std::string>> inoutNames = attrAs<std::vector<std::string>>(op, "inoutPortName");
                    const std::size_t inputCount = inputNames ? inputNames->size() : 0;
                    const std::size_t outputCount = outputNames ? outputNames->size() : 0;
                    const std::size_t inoutCount = inoutNames ? inoutNames->size() : 0;
                    if (op.operands().size() != inputCount + inoutCount)
                    {
                        error(*graph, op, "grh.verify: kInstance operand count " +
                                              std::to_string(op.operands().size()) +
                                              " does not match input+inout port count " +
                                              std::to_string(inputCount + inoutCount));
                        break;
                    }
                    if (op.results().size() != outputCount + 2 * inoutCount)
                    {
                        error(*graph, op, "grh.verify: kInstance result count " +
                                              std::to_string(op.results().size()) +
                                              " does not match output+inout port count " +
                                              std::to_string(outputCount + 2 * inoutCount));
                        break;
                    }

                    auto checkPort = [&](ValueId actual, ValueId expected, std::string_view portName,
                                         std::string_view direction) {
                        if (!actual.valid() || !expected.valid())
                        {
                            return;
                        }
                        const int32_t actualWidth = graph->valueWidth(actual);
                        const int32_t expectedWidth = target->valueWidth(expected);
                        if (actualWidth != expectedWidth)
                        {
                            error(*graph, op, "grh.verify: kInstance " + std::string(direction) + " port " +
                                                  std::string(portName) + " width mismatch: connection is " +
                                                  std::to_string(actualWidth) + ", target port is " +
                                                  std::to_string(expectedWidth));
                            return;
                        }
                        if (!shapesEqual(graph->valueShape(actual), target->valueShape(expected)))
                        {
                            warning(*graph, op,
                                    "grh.verify: kInstance " + std::string(direction) + " port " +
                                        std::string(portName) + " shape mismatch: connection is " +
                                        shapeText(graph->valueShape(actual)) + ", target port is " +
                                        shapeText(target->valueShape(expected)));
                        }
                    };

                    std::size_t operandIndex = 0;
                    if (inputNames)
                    {
                        for (const std::string &portName : *inputNames)
                        {
                            checkPort(op.operands()[operandIndex++], target->inputPortValue(portName),
                                      portName, "input");
                        }
                    }
                    if (inoutNames)
                    {
                        for (const std::string &portName : *inoutNames)
                        {
                            ValueId expected;
                            for (const InoutPort &port : target->inoutPorts())
                            {
                                if (port.name == portName)
                                {
                                    expected = port.in;
                                    break;
                                }
                            }
                            checkPort(op.operands()[operandIndex++], expected, portName, "inout-in");
                        }
                    }
                    std::size_t resultIndex = 0;
                    if (outputNames)
                    {
                        for (const std::string &portName : *outputNames)
                        {
                            checkPort(op.results()[resultIndex++], target->outputPortValue(portName),
                                      portName, "output");
                        }
                    }
                    if (inoutNames)
                    {
                        for (std::size_t field = 0; field < 2; ++field)
                        {
                            for (const std::string &portName : *inoutNames)
                            {
                                ValueId expected;
                                for (const InoutPort &port : target->inoutPorts())
                                {
                                    if (port.name == portName)
                                    {
                                        expected = field == 0 ? port.out : port.oe;
                                        break;
                                    }
                                }
                                checkPort(op.results()[resultIndex++], expected, portName,
                                          field == 0 ? "inout-out" : "inout-oe");
                            }
                        }
                    }
                    break;
                }
                case OperationKind::kSliceArray:
                {
                    // stage-02 semantics: operands [array, i0..ik-1], no attrs;
                    // base must carry a non-empty shape and 1 <= k <= rank.
                    const auto operands = op.operands();
                    const auto results = op.results();
                    if (operands.size() < 2 || results.size() != 1)
                    {
                        error(*graph, op, "grh.verify: kSliceArray expects [array, i0..ik-1] "
                                          "operands and one result");
                        break;
                    }
                    const ValueId baseId = operands.front();
                    const std::span<const int64_t> baseShape = graph->valueShape(baseId);
                    const int32_t baseWidth = graph->valueWidth(baseId);
                    if (baseShape.empty())
                    {
                        error(*graph, op, "grh.verify: kSliceArray base must be an array value "
                                          "(non-empty shape)");
                        break;
                    }
                    const std::size_t indexCount = operands.size() - 1;
                    if (indexCount > baseShape.size())
                    {
                        error(*graph, op, "grh.verify: kSliceArray index count " +
                                              std::to_string(indexCount) + " exceeds array rank " +
                                              std::to_string(baseShape.size()));
                        break;
                    }
                    for (std::size_t i = 1; i < operands.size(); ++i)
                    {
                        if (!graph->valueShape(operands[i]).empty())
                        {
                            error(*graph, op, "grh.verify: kSliceArray index operands must be scalar");
                            break;
                        }
                    }
                    int64_t keptProduct = 1;
                    for (std::size_t i = 0; i < indexCount; ++i)
                    {
                        keptProduct *= baseShape[i];
                    }
                    if (keptProduct <= 0 || baseWidth % keptProduct != 0)
                    {
                        error(*graph, op, "grh.verify: kSliceArray base shape does not divide its width");
                        break;
                    }
                    const Value result = graph->getValue(results.front());
                    const int64_t expectedWidth = baseWidth / keptProduct;
                    if (result.width() != expectedWidth)
                    {
                        error(*graph, op, "grh.verify: kSliceArray result width " +
                                              std::to_string(result.width()) + " does not match expected " +
                                              std::to_string(expectedWidth));
                    }
                    const std::span<const int64_t> expectedShape = baseShape.subspan(indexCount);
                    if (!shapesEqual(result.shape(), expectedShape))
                    {
                        error(*graph, op, "grh.verify: kSliceArray result shape " +
                                              shapeText(result.shape()) + " does not match expected " +
                                              shapeText(expectedShape));
                    }
                    break;
                }
                default:
                    break;
                }
            }
        }

        result.failed = diags().hasError();
        return result;
    }

} // namespace wolvrix::lib::transform
