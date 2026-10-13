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

        struct RegisterDeclInfo
        {
            OperationId id;
            int64_t width = 0;
            std::vector<int64_t> shape;
        };

        // Resolves the kRegister declaration referenced by a port op's
        // regSymbol attr. On failure `problem` describes why.
        std::optional<RegisterDeclInfo> registerDeclForPort(Graph &graph, const Operation &op,
                                                            std::string &problem)
        {
            std::optional<std::string> regSymbol = attrAs<std::string>(op, "regSymbol");
            if (!regSymbol || regSymbol->empty())
            {
                problem = "requires a non-empty regSymbol attr";
                return std::nullopt;
            }
            OperationId declId = graph.findOperation(*regSymbol);
            if (!declId.valid())
            {
                problem = "references unknown register '" + *regSymbol + "'";
                return std::nullopt;
            }
            const Operation decl = graph.getOperation(declId);
            if (decl.kind() != OperationKind::kRegister)
            {
                problem = "regSymbol '" + *regSymbol + "' does not name a kRegister";
                return std::nullopt;
            }
            RegisterDeclInfo info;
            info.id = declId;
            if (std::optional<int64_t> width = attrAs<int64_t>(decl, "width"))
            {
                info.width = *width;
            }
            if (std::optional<AttributeValue> shapeAttr = decl.attr("shape"))
            {
                if (const auto *dims = std::get_if<std::vector<int64_t>>(&*shapeAttr))
                {
                    info.shape = *dims;
                }
            }
            return info;
        }

        // Element count of shape prefix [0, keptDims); false on overflow.
        bool shapePrefixProduct(std::span<const int64_t> shape, std::size_t keptDims,
                                int64_t &product)
        {
            product = 1;
            for (std::size_t i = 0; i < keptDims; ++i)
            {
                if (shape[i] <= 0 || product > std::numeric_limits<int64_t>::max() / shape[i])
                {
                    return false;
                }
                product *= shape[i];
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
                case OperationKind::kRegisterReadPort:
                case OperationKind::kRegisterReadArrayPort:
                {
                    std::string problem;
                    std::optional<RegisterDeclInfo> decl = registerDeclForPort(*graph, op, problem);
                    if (!decl)
                    {
                        error(*graph, op, "grh.verify: " + std::string(toString(op.kind())) + " " + problem);
                        break;
                    }
                    if (op.results().size() != 1)
                    {
                        error(*graph, op, "grh.verify: " + std::string(toString(op.kind())) +
                                              " must produce exactly one result");
                        break;
                    }
                    const Value result = graph->getValue(op.results().front());
                    if (op.kind() == OperationKind::kRegisterReadPort)
                    {
                        if (!op.operands().empty())
                        {
                            error(*graph, op, "grh.verify: kRegisterReadPort takes no operands");
                            break;
                        }
                        if (decl->width > 0 && result.width() != decl->width)
                        {
                            error(*graph, op, "grh.verify: kRegisterReadPort result width " +
                                                  std::to_string(result.width()) +
                                                  " does not match register width " +
                                                  std::to_string(decl->width));
                        }
                        if (!shapesEqual(result.shape(), decl->shape))
                        {
                            // stage-07: the whole-read result is a structural
                            // view of the register's bits (packed-first
                            // doctrine); it may be flat over a shaped register
                            // or carry a tiling view (e.g. inherited from a
                            // connected port's packed type) over a flat
                            // register. Width equality is checked above; here
                            // only require a non-empty result shape to tile
                            // the register width exactly.
                            bool tiledView = result.shape().empty();
                            if (!tiledView && decl->width > 0)
                            {
                                int64_t product = 1;
                                for (const int64_t dim : result.shape())
                                {
                                    if (dim <= 0)
                                    {
                                        product = -1;
                                        break;
                                    }
                                    product *= dim;
                                }
                                tiledView = product > 0 && decl->width % product == 0;
                            }
                            if (!tiledView)
                            {
                                error(*graph, op, "grh.verify: kRegisterReadPort result shape " +
                                                      shapeText(result.shape()) +
                                                      " does not tile register width " +
                                                      std::to_string(decl->width) +
                                                      " (register shape " + shapeText(decl->shape) + ")");
                            }
                        }
                        break;
                    }
                    // kRegisterReadArrayPort: [i0..ik-1], 1 <= k <= rank; a
                    // partial index reads the trailing sub-array.
                    const std::size_t indexCount = op.operands().size();
                    if (indexCount < 1)
                    {
                        error(*graph, op, "grh.verify: kRegisterReadArrayPort expects at least one index operand");
                        break;
                    }
                    if (decl->shape.empty())
                    {
                        error(*graph, op, "grh.verify: kRegisterReadArrayPort requires a shaped (array) register");
                        break;
                    }
                    if (indexCount > decl->shape.size())
                    {
                        error(*graph, op, "grh.verify: kRegisterReadArrayPort index count " +
                                              std::to_string(indexCount) + " exceeds register rank " +
                                              std::to_string(decl->shape.size()));
                        break;
                    }
                    for (const ValueId index : op.operands())
                    {
                        if (!graph->valueShape(index).empty())
                        {
                            warning(*graph, op, "grh.verify: kRegisterReadArrayPort index operand carries a shape (read as flat integer)");
                            break;
                        }
                    }
                    int64_t keptProduct = 1;
                    if (!shapePrefixProduct(decl->shape, indexCount, keptProduct) || decl->width <= 0 ||
                        decl->width % keptProduct != 0)
                    {
                        error(*graph, op, "grh.verify: register shape does not divide its width");
                        break;
                    }
                    const int64_t expectedWidth = decl->width / keptProduct;
                    if (result.width() != expectedWidth)
                    {
                        error(*graph, op, "grh.verify: kRegisterReadArrayPort result width " +
                                              std::to_string(result.width()) + " does not match expected " +
                                              std::to_string(expectedWidth));
                    }
                    if (!shapesEqual(result.shape(), std::span<const int64_t>(decl->shape).subspan(indexCount)))
                    {
                        // stage-07: a flat result is always a valid view of the
                        // sub-array bits (width equality is checked above);
                        // shape is a structural view and passes legitimately
                        // produce flat forms (e.g. alias/CSE representatives).
                        // A non-empty result shape must still equal the
                        // expected sub-array geometry exactly.
                        if (!result.shape().empty())
                        {
                            error(*graph, op, "grh.verify: kRegisterReadArrayPort result shape " +
                                                  shapeText(result.shape()) + " does not match expected sub-array shape");
                        }
                    }
                    break;
                }
                case OperationKind::kRegisterWritePort:
                case OperationKind::kRegisterWriteMaskPort:
                case OperationKind::kRegisterWriteArrayPort:
                case OperationKind::kRegisterWriteArrayMaskPort:
                case OperationKind::kRegisterFillPort:
                {
                    std::string problem;
                    std::optional<RegisterDeclInfo> decl = registerDeclForPort(*graph, op, problem);
                    if (!decl)
                    {
                        error(*graph, op, "grh.verify: " + std::string(toString(op.kind())) + " " + problem);
                        break;
                    }
                    if (!op.results().empty())
                    {
                        error(*graph, op, "grh.verify: " + std::string(toString(op.kind())) +
                                              " must not produce results");
                        break;
                    }
                    const auto operands = op.operands();
                    if (operands.size() < 2)
                    {
                        error(*graph, op, "grh.verify: " + std::string(toString(op.kind())) +
                                              " expects at least updateCond and a data operand");
                        break;
                    }
                    if (graph->valueWidth(operands[0]) != 1)
                    {
                        error(*graph, op, "grh.verify: " + std::string(toString(op.kind())) +
                                              " updateCond must be 1 bit");
                    }
                    const std::optional<std::size_t> eventStart =
                        wolvrix::lib::grh::firstEventOperandIndex(op);
                    if (!eventStart)
                    {
                        error(*graph, op, "grh.verify: " + std::string(toString(op.kind())) +
                                              " cannot locate event operands (missing eventEdge attr or too few operands)");
                        break;
                    }
                    std::optional<std::vector<std::string>> edges =
                        attrAs<std::vector<std::string>>(op, "eventEdge");
                    if (!edges || edges->empty())
                    {
                        error(*graph, op, "grh.verify: " + std::string(toString(op.kind())) +
                                              " requires a non-empty eventEdge attr");
                    }
                    else if (*eventStart + edges->size() != operands.size())
                    {
                        error(*graph, op, "grh.verify: " + std::string(toString(op.kind())) +
                                              " operand count does not match layout (eventEdge count " +
                                              std::to_string(edges->size()) + ")");
                    }
                    if (decl->width <= 0)
                    {
                        break;
                    }
                    const bool isArrayKind =
                        op.kind() == OperationKind::kRegisterWriteArrayPort ||
                        op.kind() == OperationKind::kRegisterWriteArrayMaskPort;
                    if (isArrayKind && decl->shape.empty())
                    {
                        error(*graph, op, "grh.verify: " + std::string(toString(op.kind())) +
                                              " requires a shaped (array) register");
                        break;
                    }
                    const std::optional<std::size_t> indexCount =
                        wolvrix::lib::grh::arrayPortIndexCount(op);
                    if (isArrayKind)
                    {
                        if (!indexCount || *indexCount < 1 || *indexCount > decl->shape.size())
                        {
                            error(*graph, op, "grh.verify: " + std::string(toString(op.kind())) +
                                                  " index count out of range for register rank " +
                                                  std::to_string(decl->shape.size()));
                            break;
                        }
                        for (std::size_t i = 0; i < *indexCount; ++i)
                        {
                            if (!graph->valueShape(operands[1 + i]).empty())
                            {
                                warning(*graph, op, "grh.verify: " + std::string(toString(op.kind())) +
                                                      " index operand carries a shape (read as flat integer)");
                                break;
                            }
                        }
                    }
                    int64_t keptProduct = 1;
                    if (!shapePrefixProduct(decl->shape, isArrayKind ? *indexCount : 0, keptProduct))
                    {
                        break;
                    }
                    const int64_t subWidth = decl->width / keptProduct;
                    const int64_t elementCount = [&]() {
                        int64_t count = 1;
                        for (int64_t dim : decl->shape)
                        {
                            count *= dim;
                        }
                        return count;
                    }();
                    const int64_t elementWidth = decl->width / elementCount;
                    switch (op.kind())
                    {
                    case OperationKind::kRegisterWritePort:
                        if (graph->valueWidth(operands[1]) != decl->width)
                        {
                            error(*graph, op, "grh.verify: kRegisterWritePort nextValue width does not match register width");
                        }
                        break;
                    case OperationKind::kRegisterWriteMaskPort:
                        if (graph->valueWidth(operands[1]) != decl->width ||
                            graph->valueWidth(operands[2]) != decl->width)
                        {
                            error(*graph, op, "grh.verify: kRegisterWriteMaskPort nextValue/mask widths must match register width");
                        }
                        break;
                    case OperationKind::kRegisterWriteArrayPort:
                        if (graph->valueWidth(operands[*eventStart - 1]) != subWidth)
                        {
                            error(*graph, op, "grh.verify: kRegisterWriteArrayPort data width does not match the addressed sub-array width");
                        }
                        break;
                    case OperationKind::kRegisterWriteArrayMaskPort:
                        if (graph->valueWidth(operands[*eventStart - 2]) != subWidth ||
                            graph->valueWidth(operands[*eventStart - 1]) != subWidth)
                        {
                            error(*graph, op, "grh.verify: kRegisterWriteArrayMaskPort data/mask widths must match the addressed sub-array width");
                        }
                        break;
                    case OperationKind::kRegisterFillPort:
                        if (graph->valueWidth(operands[1]) != elementWidth)
                        {
                            error(*graph, op, "grh.verify: kRegisterFillPort data width must match the element width");
                        }
                        break;
                    default:
                        break;
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
                            warning(*graph, op, "grh.verify: kSliceArray index operand carries a shape (read as flat integer)");
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
                case OperationKind::kConcat:
                {
                    // stage-07: a concat's result width must equal the sum of
                    // its operand widths. (A mis-widthed concat sailed past
                    // every pass and broke comb-loop-elim's range analysis on
                    // the preserve-aggregate XiangShan design before anything
                    // reported it.)
                    const auto operands = op.operands();
                    const auto results = op.results();
                    if (results.size() != 1)
                    {
                        error(*graph, op, "grh.verify: kConcat expects exactly one result");
                        break;
                    }
                    int64_t totalWidth = 0;
                    bool widthOk = true;
                    for (const ValueId operand : operands)
                    {
                        if (!operand.valid())
                        {
                            widthOk = false;
                            break;
                        }
                        const int32_t width = graph->valueWidth(operand);
                        if (width <= 0)
                        {
                            widthOk = false;
                            break;
                        }
                        totalWidth += width;
                    }
                    if (widthOk && totalWidth > 0
                        && graph->valueWidth(results.front()) != totalWidth)
                    {
                        std::string locText;
                        if (auto loc = op.srcLoc(); loc && !loc->file.empty())
                        {
                            locText = " at " + std::string(loc->file) + ":" + std::to_string(loc->line);
                        }
                        if (auto loc = op.srcLoc(); loc && !loc->pass.empty())
                        {
                            locText += " pass=" + loc->pass;
                        }
                        std::string operandWidths;
                        for (std::size_t oi = 0; oi < operands.size() && oi < 6; ++oi)
                        {
                            operandWidths += (oi == 0 ? " operands w=[" : ",");
                            operandWidths += std::to_string(graph->valueWidth(operands[oi]));
                        }
                        if (!operandWidths.empty())
                        {
                            operandWidths += operands.size() > 6 ? ",...]" : "]";
                        }
                        error(*graph, op, "grh.verify: kConcat result width " +
                                              std::to_string(graph->valueWidth(results.front())) +
                                              " does not match operand width sum " +
                                              std::to_string(totalWidth) + locText + operandWidths);
                    }
                    break;
                }
                case OperationKind::kReplicate:
                {
                    const auto operands = op.operands();
                    const auto results = op.results();
                    const std::optional<int64_t> rep = attrAs<int64_t>(op, "rep");
                    if (operands.size() != 1 || results.size() != 1 || !rep || *rep <= 0 ||
                        !operands.front().valid() || !results.front().valid())
                    {
                        error(*graph, op, "grh.verify: kReplicate requires one operand, one result, and a positive int rep attr");
                        break;
                    }
                    const int32_t operandWidth = graph->valueWidth(operands.front());
                    if (operandWidth <= 0 || *rep > std::numeric_limits<int32_t>::max() / operandWidth ||
                        graph->valueWidth(results.front()) != *rep * operandWidth)
                    {
                        error(*graph, op, "grh.verify: kReplicate result width " +
                                              std::to_string(graph->valueWidth(results.front())) +
                                              " does not match rep " + std::to_string(*rep) +
                                              " x operand width " + std::to_string(operandWidth));
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
