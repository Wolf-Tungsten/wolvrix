#include "transform/memory_init_check.hpp"

#include "core/grh.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace wolvrix::lib::transform
{

    namespace
    {
        template <typename T>
        std::optional<T> getAttr(const wolvrix::lib::grh::Operation &op, std::string_view key)
        {
            auto attr = op.attr(key);
            if (!attr)
            {
                return std::nullopt;
            }
            if (const auto *value = std::get_if<T>(&*attr))
            {
                return *value;
            }
            return std::nullopt;
        }

        struct InitInfo
        {
            std::vector<std::string> kinds;
            std::vector<std::string> files;
            std::vector<int64_t> starts;
            std::vector<int64_t> lens;
        };

        bool hasAnyInitAttrs(const wolvrix::lib::grh::Operation &op)
        {
            return op.attr("initKind").has_value() ||
                   op.attr("initFile").has_value() ||
                   op.attr("initStart").has_value() ||
                   op.attr("initLen").has_value();
        }

        bool initInfoEquals(const InitInfo &lhs, const InitInfo &rhs)
        {
            return lhs.kinds == rhs.kinds &&
                   lhs.files == rhs.files &&
                   lhs.starts == rhs.starts &&
                   lhs.lens == rhs.lens;
        }
    } // namespace

    MemoryInitCheckPass::MemoryInitCheckPass()
        : Pass("memory-init-check", "memory-init-check",
               "Validate array-register (kRegister with shape) init attribute consistency")
    {
    }

    PassResult MemoryInitCheckPass::run()
    {
        PassResult result;
        const std::size_t graphCount = design().graphs().size();
        logDebug("begin graphs=" + std::to_string(graphCount));

        for (const auto &entry : design().graphs())
        {
            wolvrix::lib::grh::Graph &graph = *entry.second;
            std::unordered_map<std::string, InitInfo> initBySymbol;

            for (const auto opId : graph.operations())
            {
                const wolvrix::lib::grh::Operation op = graph.getOperation(opId);
                if (op.kind() != wolvrix::lib::grh::OperationKind::kRegister ||
                    !op.attr("shape").has_value())
                {
                    continue;
                }

                if (!hasAnyInitAttrs(op))
                {
                    continue;
                }

                auto kinds = getAttr<std::vector<std::string>>(op, "initKind").value_or(std::vector<std::string>{});
                auto files = getAttr<std::vector<std::string>>(op, "initFile").value_or(std::vector<std::string>{});
                const std::size_t count = std::max(kinds.size(), files.size());
                if (count == 0)
                {
                    warning(graph, op, "array register init attributes are incomplete (missing initKind/initFile)");
                    continue;
                }
                if (kinds.size() != files.size())
                {
                    error(graph, op, "array register initKind/initFile size mismatch");
                    result.failed = true;
                    continue;
                }

                auto starts = getAttr<std::vector<int64_t>>(op, "initStart");
                auto lens = getAttr<std::vector<int64_t>>(op, "initLen");
                if (!starts || !lens)
                {
                    error(graph, op, "array register initStart/initLen missing");
                    result.failed = true;
                    continue;
                }
                if (starts->size() != count || lens->size() != count)
                {
                    error(graph, op, "array register initStart/initLen size mismatch");
                    result.failed = true;
                    continue;
                }

                InitInfo info{
                    .kinds = std::move(kinds),
                    .files = std::move(files),
                    .starts = std::move(*starts),
                    .lens = std::move(*lens),
                };

                const std::string symbol = std::string(op.symbolText());
                auto it = initBySymbol.find(symbol);
                if (it == initBySymbol.end())
                {
                    initBySymbol.emplace(symbol, std::move(info));
                    continue;
                }

                if (!initInfoEquals(it->second, info))
                {
                    error(graph, op, "array register init attributes differ for merged register '" + symbol + "'");
                    result.failed = true;
                }
            }
        }

        return result;
    }

} // namespace wolvrix::lib::transform
