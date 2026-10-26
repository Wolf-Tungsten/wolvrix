#include "emit/verilator_repcut_package.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "emit/system_verilog.hpp"
#include "slang/numeric/SVInt.h"
namespace wolvrix::lib::emit
{

    namespace
    {
        template <typename T>
        std::optional<T> getAttribute(const wolvrix::lib::grh::Operation &op, std::string_view key)
        {
            const auto attr = op.attr(key);
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

        std::string graphSymbolRequired(const wolvrix::lib::grh::Graph &graph,
                                        wolvrix::lib::grh::SymbolId symbol,
                                        std::string_view context)
        {
            if (!symbol.valid())
            {
                throw std::runtime_error(std::string(context) + " symbol is invalid");
            }
            const std::string text(graph.symbolText(symbol));
            if (text.empty())
            {
                throw std::runtime_error(std::string(context) + " symbol is empty");
            }
            return text;
        }

        std::string opSymbolRequired(const wolvrix::lib::grh::Graph &graph,
                                     wolvrix::lib::grh::OperationId opId)
        {
            return graphSymbolRequired(graph, graph.operationSymbol(opId), "Operation");
        }

        std::string valueSymbolRequired(const wolvrix::lib::grh::Graph &graph,
                                        wolvrix::lib::grh::ValueId valueId)
        {
            return graphSymbolRequired(graph, graph.valueSymbol(valueId), "Value");
        }

        std::vector<const wolvrix::lib::grh::Graph *> graphsInDesignOrder(
            const wolvrix::lib::grh::Design &design,
            const std::unordered_set<std::string> &symbols)
        {
            std::vector<const wolvrix::lib::grh::Graph *> graphs;
            graphs.reserve(symbols.size());
            for (const auto &graphSymbol : design.graphOrder())
            {
                if (symbols.find(graphSymbol) == symbols.end())
                {
                    continue;
                }
                auto it = design.graphs().find(graphSymbol);
                if (it != design.graphs().end() && it->second)
                {
                    graphs.push_back(it->second.get());
                }
            }
            return graphs;
        }

        std::vector<const wolvrix::lib::grh::Graph *> reachableGraphsFromTops(
            const wolvrix::lib::grh::Design &design,
            std::span<const wolvrix::lib::grh::Graph *const> topGraphs)
        {
            std::unordered_set<std::string> reachableSymbols;
            std::vector<const wolvrix::lib::grh::Graph *> worklist;
            worklist.reserve(topGraphs.size());

            for (const auto *graph : topGraphs)
            {
                if (!graph)
                {
                    continue;
                }
                if (reachableSymbols.insert(graph->symbol()).second)
                {
                    worklist.push_back(graph);
                }
            }

            for (std::size_t i = 0; i < worklist.size(); ++i)
            {
                const auto *graph = worklist[i];
                if (!graph)
                {
                    continue;
                }
                for (const auto opId : graph->operations())
                {
                    const auto kind = graph->opKind(opId);
                    if (kind != wolvrix::lib::grh::OperationKind::kInstance &&
                        kind != wolvrix::lib::grh::OperationKind::kBlackbox)
                    {
                        continue;
                    }
                    const auto moduleName = getAttribute<std::string>(graph->getOperation(opId), "moduleName");
                    if (!moduleName || moduleName->empty())
                    {
                        continue;
                    }
                    const auto *targetGraph = design.findGraph(*moduleName);
                    if (!targetGraph)
                    {
                        continue;
                    }
                    if (reachableSymbols.insert(targetGraph->symbol()).second)
                    {
                        worklist.push_back(targetGraph);
                    }
                }
            }

            return graphsInDesignOrder(design, reachableSymbols);
        }

        struct ManifestPort
        {
            std::string name;
            std::string direction;
            int64_t width = 1;
            bool isSigned = false;
        };

        struct ManifestUnit
        {
            std::string instanceName;
            std::string moduleGraphName;
            std::string moduleName;
            std::string sourceSv;
            std::vector<ManifestPort> ports;
        };

        struct UnitShimInfo
        {
            std::string wrapperModuleName;
            std::string wrapperSourceSv;
            std::string wrapperSvText;
            std::vector<ManifestPort> wrapperPorts;
            std::unordered_map<std::string, std::string> inputPortByGraphName;
            std::unordered_map<std::string, std::string> outputPortByGraphName;
            std::unordered_map<std::string, std::string> inoutPortByGraphName;
        };

        struct DriverDesc
        {
            enum class Kind
            {
                Top,
                Unit,
                Const
            };

            Kind kind = Kind::Top;
            std::string instanceName;
            std::string portName;
            std::string constValue;
        };

        struct SinkDesc
        {
            enum class Kind
            {
                Unit,
                Top
            };

            Kind kind = Kind::Unit;
            std::string instanceName;
            std::string portName;
        };

        struct ManifestEdge
        {
            std::string signal;
            int64_t width = 1;
            bool isSigned = false;
            std::string kind;
            DriverDesc driver;
            std::vector<SinkDesc> sinks;
        };

        struct EdgeKey
        {
            std::string kind;
            wolvrix::lib::grh::ValueId valueId;

            bool operator==(const EdgeKey &other) const noexcept
            {
                return kind == other.kind && valueId == other.valueId;
            }
        };

        struct EdgeKeyHash
        {
            std::size_t operator()(const EdgeKey &key) const noexcept
            {
                std::size_t seed = std::hash<std::string>{}(key.kind);
                seed ^= wolvrix::lib::grh::ValueIdHash{}(key.valueId) + 0x9e3779b9u + (seed << 6u) + (seed >> 2u);
                return seed;
            }
        };

        struct PackageManifest
        {
            std::string topModule;
            std::vector<ManifestPort> topInputs;
            std::vector<ManifestPort> topOutputs;
            std::vector<ManifestUnit> units;
            std::vector<ManifestEdge> connections;
            std::vector<std::string> serialEvalOrder;
        };

        struct PendingUnitInputs
        {
            std::string instanceName;
            std::vector<std::string> inputNames;
            std::vector<wolvrix::lib::grh::ValueId> operands;
        };

        struct AliasAssign
        {
            wolvrix::lib::grh::ValueId src;
            wolvrix::lib::grh::ValueId dst;
        };

        struct CppSignalDesc
        {
            std::string typeName;
            bool isWide = false;
            std::size_t wordCount = 0;
        };

        std::string sanitizeIdentifier(std::string_view text)
        {
            std::string out;
            out.reserve(text.size() + 1);
            for (const unsigned char ch : text)
            {
                if (std::isalnum(ch) || ch == '_')
                {
                    out.push_back(static_cast<char>(ch));
                }
                else
                {
                    out.push_back('_');
                }
            }
            if (out.empty())
            {
                out = "unnamed";
            }
            if (std::isdigit(static_cast<unsigned char>(out.front())))
            {
                out.insert(out.begin(), '_');
            }
            return out;
        }

        std::string trimCopy(std::string_view text)
        {
            std::size_t begin = 0;
            while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])))
            {
                ++begin;
            }
            std::size_t end = text.size();
            while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
            {
                --end;
            }
            return std::string(text.substr(begin, end - begin));
        }

        std::string makeUniqueIdentifier(std::string_view base,
                                         std::unordered_set<std::string> &usedIdentifiers)
        {
            std::string candidate = sanitizeIdentifier(base);
            std::string original = candidate;
            std::size_t suffix = 0;
            while (!usedIdentifiers.insert(candidate).second)
            {
                candidate = original + "_" + std::to_string(++suffix);
            }
            return candidate;
        }

        std::string verilatorPublicMemberName(std::string_view svIdentifier)
        {
            std::string out;
            out.reserve(svIdentifier.size());
            for (std::size_t i = 0; i < svIdentifier.size(); ++i)
            {
                const char ch = svIdentifier[i];
                if (ch == '_' && i + 1 < svIdentifier.size() && svIdentifier[i + 1] == '_')
                {
                    out += "___05F";
                    ++i;
                    continue;
                }
                out.push_back(ch);
            }
            return out;
        }

        std::optional<ManifestPort> parseEmittedSvPortLine(std::string_view line)
        {
            static const std::regex kLogicPortRegex(
                R"(^\s*(input|output|inout)\s+(?:(wire|reg)\s+)?(?:(signed)\s+)?(?:\[(\d+):(\d+)\]\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*,?\s*$)");

            std::match_results<std::string_view::const_iterator> match;
            if (!std::regex_match(line.begin(), line.end(), match, kLogicPortRegex))
            {
                return std::nullopt;
            }

            const int64_t msb = match[4].matched ? std::stoll(match[4].str()) : 0;
            const int64_t lsb = match[5].matched ? std::stoll(match[5].str()) : 0;
            return ManifestPort{
                match[6].str(),
                match[1].str(),
                match[4].matched ? (msb - lsb + 1) : 1,
                match[3].matched,
            };
        }

        std::vector<ManifestPort> parseEmittedSvPorts(const std::filesystem::path &svPath,
                                                      std::string_view moduleName)
        {
            std::ifstream stream(svPath);
            if (!stream)
            {
                throw std::runtime_error("Failed to open emitted SV module: " + svPath.string());
            }

            const std::string modulePrefix = "module " + std::string(moduleName);
            bool inPortList = false;
            std::vector<ManifestPort> ports;
            std::string line;
            while (std::getline(stream, line))
            {
                const std::string trimmed = trimCopy(line);
                if (!inPortList)
                {
                    if (trimmed.rfind(modulePrefix, 0) == 0 && trimmed.find('(') != std::string::npos)
                    {
                        inPortList = true;
                    }
                    continue;
                }
                if (trimmed == ");")
                {
                    break;
                }
                if (trimmed.empty() || trimmed.rfind("/*", 0) == 0 || trimmed.rfind("//", 0) == 0)
                {
                    continue;
                }

                const auto port = parseEmittedSvPortLine(trimmed);
                if (!port)
                {
                    throw std::runtime_error("Unsupported emitted SV port declaration in " +
                                             svPath.string() + ": " + trimmed);
                }
                ports.push_back(*port);
            }

            if (!inPortList || ports.empty())
            {
                throw std::runtime_error("Failed to parse emitted SV ports for module " +
                                         std::string(moduleName) + " from " + svPath.string());
            }
            return ports;
        }

        std::string formatShimPortDecl(const ManifestPort &port)
        {
            std::ostringstream out;
            out << port.direction << " wire ";
            if (port.isSigned)
            {
                out << "signed ";
            }
            if (port.width > 1)
            {
                out << "[" << (port.width - 1) << ":0] ";
            }
            out << port.name;
            return out.str();
        }

        UnitShimInfo buildUnitShim(const wolvrix::lib::grh::Graph &unitGraph,
                                   std::string_view emittedModuleName,
                                   const std::filesystem::path &emittedSvPath,
                                   std::string_view wrapperModuleName)
        {
            const std::vector<ManifestPort> emittedPorts = parseEmittedSvPorts(emittedSvPath, emittedModuleName);

            std::vector<ManifestPort> emittedInputs;
            std::vector<ManifestPort> emittedOutputs;
            std::vector<ManifestPort> emittedInouts;
            emittedInputs.reserve(emittedPorts.size());
            emittedOutputs.reserve(emittedPorts.size());
            emittedInouts.reserve(emittedPorts.size());
            for (const auto &port : emittedPorts)
            {
                if (port.direction == "input")
                {
                    emittedInputs.push_back(port);
                }
                else if (port.direction == "output")
                {
                    emittedOutputs.push_back(port);
                }
                else if (port.direction == "inout")
                {
                    emittedInouts.push_back(port);
                }
            }

            if (unitGraph.inputPorts().size() != emittedInputs.size() ||
                unitGraph.outputPorts().size() != emittedOutputs.size() ||
                unitGraph.inoutPorts().size() != emittedInouts.size())
            {
                throw std::runtime_error("Emitted SV port counts do not match graph ports for module " +
                                         unitGraph.symbol());
            }

            std::unordered_map<std::string, const wolvrix::lib::grh::Port *> graphInputsByName;
            std::unordered_map<std::string, const wolvrix::lib::grh::Port *> graphOutputsByName;
            std::unordered_map<std::string, const wolvrix::lib::grh::InoutPort *> graphInoutsByName;
            graphInputsByName.reserve(unitGraph.inputPorts().size());
            graphOutputsByName.reserve(unitGraph.outputPorts().size());
            graphInoutsByName.reserve(unitGraph.inoutPorts().size());
            for (const auto &port : unitGraph.inputPorts())
            {
                graphInputsByName.emplace(port.name, &port);
            }
            for (const auto &port : unitGraph.outputPorts())
            {
                graphOutputsByName.emplace(port.name, &port);
            }
            for (const auto &port : unitGraph.inoutPorts())
            {
                graphInoutsByName.emplace(port.name, &port);
            }

            UnitShimInfo shim;
            shim.wrapperModuleName = std::string(wrapperModuleName);
            shim.wrapperSourceSv =
                (std::filesystem::path("sv") / (shim.wrapperModuleName + ".sv")).generic_string();

            std::ostringstream wrapper;
            wrapper << "module " << shim.wrapperModuleName << " (\n";

            std::vector<std::pair<std::string, std::string>> connectionPairs;
            connectionPairs.reserve(emittedPorts.size());
            bool firstPort = true;
            std::unordered_set<std::string> seenGraphInputs;
            std::unordered_set<std::string> seenGraphOutputs;
            std::unordered_set<std::string> seenGraphInouts;
            seenGraphInputs.reserve(unitGraph.inputPorts().size());
            seenGraphOutputs.reserve(unitGraph.outputPorts().size());
            seenGraphInouts.reserve(unitGraph.inoutPorts().size());
            std::size_t inputOrdinal = 0;
            std::size_t outputOrdinal = 0;
            std::size_t inoutOrdinal = 0;

            for (const auto &emittedPort : emittedPorts)
            {
                ManifestPort wrapperPort;
                wrapperPort.direction = emittedPort.direction;
                wrapperPort.width = emittedPort.width;
                wrapperPort.isSigned = emittedPort.isSigned;

                if (emittedPort.direction == "input")
                {
                    const auto graphIt = graphInputsByName.find(emittedPort.name);
                    if (graphIt == graphInputsByName.end())
                    {
                        throw std::runtime_error("Input port name mismatch while building shim for module " +
                                                 unitGraph.symbol() + ": missing graph port " + emittedPort.name);
                    }
                    const auto &graphPort = *graphIt->second;
                    wrapperPort.name = "in_" + std::to_string(inputOrdinal++);
                    if (!seenGraphInputs.insert(graphPort.name).second)
                    {
                        throw std::runtime_error("Duplicate input port mapping while building shim for module " +
                                                 unitGraph.symbol() + ": " + graphPort.name);
                    }
                    if (unitGraph.valueWidth(graphPort.value) != emittedPort.width ||
                        unitGraph.valueSigned(graphPort.value) != emittedPort.isSigned)
                    {
                        throw std::runtime_error("Input port shape mismatch while building shim for module " +
                                                 unitGraph.symbol() + ": " + graphPort.name);
                    }
                    shim.inputPortByGraphName.emplace(graphPort.name, wrapperPort.name);
                }
                else if (emittedPort.direction == "output")
                {
                    const auto graphIt = graphOutputsByName.find(emittedPort.name);
                    if (graphIt == graphOutputsByName.end())
                    {
                        throw std::runtime_error("Output port name mismatch while building shim for module " +
                                                 unitGraph.symbol() + ": missing graph port " + emittedPort.name);
                    }
                    const auto &graphPort = *graphIt->second;
                    wrapperPort.name = "out_" + std::to_string(outputOrdinal++);
                    if (!seenGraphOutputs.insert(graphPort.name).second)
                    {
                        throw std::runtime_error("Duplicate output port mapping while building shim for module " +
                                                 unitGraph.symbol() + ": " + graphPort.name);
                    }
                    if (unitGraph.valueWidth(graphPort.value) != emittedPort.width ||
                        unitGraph.valueSigned(graphPort.value) != emittedPort.isSigned)
                    {
                        throw std::runtime_error("Output port shape mismatch while building shim for module " +
                                                 unitGraph.symbol() + ": " + graphPort.name);
                    }
                    shim.outputPortByGraphName.emplace(graphPort.name, wrapperPort.name);
                }
                else
                {
                    const auto graphIt = graphInoutsByName.find(emittedPort.name);
                    if (graphIt == graphInoutsByName.end())
                    {
                        throw std::runtime_error("Inout port name mismatch while building shim for module " +
                                                 unitGraph.symbol() + ": missing graph port " + emittedPort.name);
                    }
                    const auto &graphPort = *graphIt->second;
                    wrapperPort.name = "inout_" + std::to_string(inoutOrdinal++);
                    if (!seenGraphInouts.insert(graphPort.name).second)
                    {
                        throw std::runtime_error("Duplicate inout port mapping while building shim for module " +
                                                 unitGraph.symbol() + ": " + graphPort.name);
                    }
                    if (unitGraph.valueWidth(graphPort.out) != emittedPort.width ||
                        unitGraph.valueSigned(graphPort.out) != emittedPort.isSigned)
                    {
                        throw std::runtime_error("Inout port shape mismatch while building shim for module " +
                                                 unitGraph.symbol() + ": " + graphPort.name);
                    }
                    shim.inoutPortByGraphName.emplace(graphPort.name, wrapperPort.name);
                }

                shim.wrapperPorts.push_back(wrapperPort);
                connectionPairs.emplace_back(emittedPort.name, wrapperPort.name);

                if (!firstPort)
                {
                    wrapper << ",\n";
                }
                firstPort = false;
                wrapper << "  " << formatShimPortDecl(wrapperPort);
            }

            if (seenGraphInputs.size() != unitGraph.inputPorts().size() ||
                seenGraphOutputs.size() != unitGraph.outputPorts().size() ||
                seenGraphInouts.size() != unitGraph.inoutPorts().size())
            {
                throw std::runtime_error("Failed to map every graph port by exact SV name for module " +
                                         unitGraph.symbol());
            }

            wrapper << "\n);\n\n";
            wrapper << "  " << emittedModuleName << " inner (\n";
            for (std::size_t i = 0; i < connectionPairs.size(); ++i)
            {
                const auto &[innerPort, wrapperPort] = connectionPairs[i];
                wrapper << "    ." << innerPort << "(" << wrapperPort << ")";
                if (i + 1 != connectionPairs.size())
                {
                    wrapper << ",";
                }
                wrapper << "\n";
            }
            wrapper << "  );\n";
            wrapper << "endmodule\n";
            shim.wrapperSvText = wrapper.str();
            return shim;
        }

        CppSignalDesc cppSignalDesc(int64_t width)
        {
            if (width <= 8)
            {
                return {"CData", false, 0};
            }
            if (width <= 16)
            {
                return {"SData", false, 0};
            }
            if (width <= 32)
            {
                return {"IData", false, 0};
            }
            if (width <= 64)
            {
                return {"QData", false, 0};
            }
            const std::size_t words = static_cast<std::size_t>((width + 31) / 32);
            return {"std::array<WData, " + std::to_string(words) + ">", true, words};
        }

        std::vector<uint32_t> parseConstWords(std::string_view literal, int64_t width)
        {
            if (width <= 0)
            {
                return {};
            }
            slang::SVInt value = slang::SVInt::fromString(std::string(literal));
            value = value.resize(static_cast<slang::bitwidth_t>(width));
            const std::size_t words = static_cast<std::size_t>((width + 31) / 32);
            std::vector<uint32_t> out(words, 0);
            for (int64_t bit = 0; bit < width; ++bit)
            {
                const char bitChar = static_cast<char>(std::tolower(
                    static_cast<unsigned char>(value[static_cast<int32_t>(bit)].toChar())));
                if (bitChar == '1')
                {
                    out[static_cast<std::size_t>(bit / 32)] |= (uint32_t(1) << static_cast<uint32_t>(bit % 32));
                }
            }
            return out;
        }

        std::string cppConstLiteral(std::string_view literal, int64_t width)
        {
            const auto desc = cppSignalDesc(width);
            const std::vector<uint32_t> words = parseConstWords(literal, width);
            std::ostringstream out;
            if (!desc.isWide)
            {
                uint64_t value = 0;
                if (!words.empty())
                {
                    value = words[0];
                    if (words.size() > 1)
                    {
                        value |= (static_cast<uint64_t>(words[1]) << 32u);
                    }
                }
                out << "static_cast<" << desc.typeName << ">(0x" << std::hex << value << "ULL)";
                return out.str();
            }
            out << desc.typeName << "{";
            for (std::size_t i = 0; i < words.size(); ++i)
            {
                if (i != 0)
                {
                    out << ", ";
                }
                out << "static_cast<WData>(0x" << std::hex << words[i] << "U)";
            }
            out << "}";
            return out.str();
        }

        std::optional<std::string> findConstValue(const wolvrix::lib::grh::Graph &graph,
                                                  wolvrix::lib::grh::ValueId valueId)
        {
            const auto def = graph.valueDef(valueId);
            if (!def.valid() || graph.opKind(def) != wolvrix::lib::grh::OperationKind::kConstant)
            {
                return std::nullopt;
            }
            return getAttribute<std::string>(graph.getOperation(def), "constValue");
        }

        struct WrapperCode
        {
            std::string header;
            struct SourceFile
            {
                std::string filename;
                std::string contents;
            };
            std::vector<SourceFile> sources;
        };

        struct BuildGlueCode
        {
            std::string smokeMain;
            std::string unitsMk;
            std::string makefile;
        };

        template <typename Entry, typename SizeFn>
        std::vector<std::vector<std::size_t>> chunkEntriesByEstimatedSize(const std::vector<Entry> &entries,
                                                                          SizeFn sizeFn,
                                                                          std::size_t targetBytes)
        {
            std::vector<std::vector<std::size_t>> chunks;
            std::vector<std::size_t> current;
            std::size_t currentBytes = 0;
            for (std::size_t i = 0; i < entries.size(); ++i)
            {
                const std::size_t entryBytes = std::max<std::size_t>(1, sizeFn(entries[i]));
                if (!current.empty() && currentBytes + entryBytes > targetBytes)
                {
                    chunks.push_back(std::move(current));
                    current.clear();
                    currentBytes = 0;
                }
                current.push_back(i);
                currentBytes += entryBytes;
            }
            if (!current.empty())
            {
                chunks.push_back(std::move(current));
            }
            return chunks;
        }

        WrapperCode generatePartitionedWrapperCode(const PackageManifest &manifest)
        {
            struct UnitInfo
            {
                std::string instanceName;
                std::string memberName;
                std::string modelType;
                std::unordered_map<std::string, CppSignalDesc> portDescByName;
                std::unordered_map<std::string, std::string> portCppMemberByName;
            };
            struct NamedSignal
            {
                std::string originalName;
                std::string memberName;
                CppSignalDesc desc;
            };
            struct PhaseMethodDef
            {
                std::string instanceName;
                std::string methodName;
                std::vector<std::string> modelTypes;
                std::string definition;
            };

            constexpr std::size_t kLoadChunkTargetBytes = 4u * 1024u * 1024u;
            constexpr std::size_t kEvalChunkTargetBytes = 4u * 1024u * 1024u;
            constexpr std::size_t kUpdateChunkTargetBytes = 4u * 1024u * 1024u;

            std::unordered_set<std::string> usedUnitIdentifiers;
            std::unordered_map<std::string, UnitInfo> unitInfoByInstance;
            for (const auto &unit : manifest.units)
            {
                const std::string ident = makeUniqueIdentifier(unit.instanceName, usedUnitIdentifiers);
                unitInfoByInstance.emplace(unit.instanceName,
                                           UnitInfo{
                                               unit.instanceName,
                                               "unit_" + ident + "_",
                                               "V" + unit.moduleName,
                                               {},
                                               {},
                                           });
                auto &unitInfo = unitInfoByInstance.at(unit.instanceName);
                for (const auto &port : unit.ports)
                {
                    unitInfo.portDescByName.emplace(port.name, cppSignalDesc(port.width));
                    unitInfo.portCppMemberByName.emplace(port.name, verilatorPublicMemberName(port.name));
                }
            }

            std::unordered_map<std::string, std::size_t> timingIndexByInstance;
            timingIndexByInstance.reserve(manifest.units.size());
            std::vector<std::string> modelTypesInOrder;
            std::unordered_set<std::string> seenModelTypes;
            for (std::size_t i = 0; i < manifest.units.size(); ++i)
            {
                const auto &unit = manifest.units[i];
                timingIndexByInstance.emplace(unit.instanceName, i);
                const auto &unitInfo = unitInfoByInstance.at(unit.instanceName);
                if (seenModelTypes.insert(unitInfo.modelType).second)
                {
                    modelTypesInOrder.push_back(unitInfo.modelType);
                }
            }

            std::vector<std::string> normalEvalOrder;
            normalEvalOrder.reserve(manifest.serialEvalOrder.size());
            for (const auto &instanceName : manifest.serialEvalOrder)
            {
                const auto unitIt = std::find_if(
                    manifest.units.begin(),
                    manifest.units.end(),
                    [&](const ManifestUnit &unit) { return unit.instanceName == instanceName; });
                if (unitIt == manifest.units.end())
                {
                    continue;
                }
                if (unitInfoByInstance.find(instanceName) == unitInfoByInstance.end())
                {
                    continue;
                }
                normalEvalOrder.push_back(instanceName);
            }

            std::unordered_set<std::string> usedTopInputIdentifiers;
            std::vector<NamedSignal> topInputs;
            std::unordered_map<std::string, NamedSignal> topInputByPort;
            for (const auto &port : manifest.topInputs)
            {
                NamedSignal signal{
                    port.name,
                    "top_in_" + makeUniqueIdentifier(port.name, usedTopInputIdentifiers) + "_",
                    cppSignalDesc(port.width),
                };
                topInputByPort.emplace(port.name, signal);
                topInputs.push_back(std::move(signal));
            }

            std::unordered_set<std::string> usedTopOutputIdentifiers;
            std::vector<NamedSignal> topOutputs;
            std::unordered_map<std::string, NamedSignal> topOutputByPort;
            for (const auto &port : manifest.topOutputs)
            {
                NamedSignal signal{
                    port.name,
                    "top_out_" + makeUniqueIdentifier(port.name, usedTopOutputIdentifiers) + "_",
                    cppSignalDesc(port.width),
                };
                topOutputByPort.emplace(port.name, signal);
                topOutputs.push_back(std::move(signal));
            }

            std::unordered_set<std::string> usedConstIdentifiers;
            std::vector<NamedSignal> constSignals;
            std::unordered_map<std::string, NamedSignal> constSignalByName;
            std::unordered_map<std::string, std::string> constLiteralBySignal;
            for (const auto &edge : manifest.connections)
            {
                if (edge.kind == "const_to_unit" &&
                    constSignalByName.find(edge.signal) == constSignalByName.end())
                {
                    NamedSignal signal{
                        edge.signal,
                        "const_" + makeUniqueIdentifier(edge.signal, usedConstIdentifiers) + "_",
                        cppSignalDesc(edge.width),
                    };
                    constSignalByName.emplace(edge.signal, signal);
                    constSignals.push_back(std::move(signal));
                    constLiteralBySignal.emplace(edge.signal, edge.driver.constValue);
                }
            }

            auto emitAssign = [](std::ostream &out,
                                 std::string_view dstExpr,
                                 bool dstIsWide,
                                 std::size_t dstWordCount,
                                 std::string_view srcExpr,
                                 bool srcIsWide,
                                 std::size_t srcWordCount,
                                 int indentLevel)
            {
                const std::string indent(static_cast<std::size_t>(indentLevel * 2), ' ');
                if (dstIsWide && srcIsWide)
                {
                    out << indent << "copy_wide_words_(" << dstExpr << ", " << srcExpr
                        << ", " << std::min(dstWordCount, srcWordCount) << ");\n";
                }
                else if (dstIsWide)
                {
                    out << indent << "copy_from_verilated_(" << dstExpr << ", " << srcExpr << ");\n";
                }
                else if (srcIsWide)
                {
                    out << indent << "copy_to_verilated_(" << dstExpr << ", " << srcExpr << ");\n";
                }
                else
                {
                    out << indent << dstExpr << " = " << srcExpr << ";\n";
                }
            };

            auto emitTopInputLoadsForUnit = [&](std::ostream &out, const std::string &instanceName, int indentLevel)
            {
                const auto unitIt = unitInfoByInstance.find(instanceName);
                if (unitIt == unitInfoByInstance.end())
                {
                    return false;
                }
                const auto &unitInfo = unitIt->second;
                const std::string indent(static_cast<std::size_t>(indentLevel * 2), ' ');
                bool wroteAny = false;
                for (const auto &edge : manifest.connections)
                {
                    if (edge.driver.kind != DriverDesc::Kind::Top)
                    {
                        continue;
                    }
                    for (const auto &sink : edge.sinks)
                    {
                        if (sink.kind != SinkDesc::Kind::Unit || sink.instanceName != instanceName)
                        {
                            continue;
                        }

                        const auto topInputIt = topInputByPort.find(edge.driver.portName);
                        if (topInputIt == topInputByPort.end())
                        {
                            continue;
                        }
                        if (!wroteAny)
                        {
                            out << indent << "// top inputs for " << instanceName << "\n";
                            wroteAny = true;
                        }
                        emitAssign(out,
                                   unitInfo.memberName + "->" + unitInfo.portCppMemberByName.at(sink.portName),
                                   unitInfo.portDescByName.at(sink.portName).isWide,
                                   unitInfo.portDescByName.at(sink.portName).wordCount,
                                   topInputIt->second.memberName,
                                   topInputIt->second.desc.isWide,
                                   topInputIt->second.desc.wordCount,
                                   indentLevel);
                    }
                }
                return wroteAny;
            };

            auto emitConstLoadsForUnit = [&](std::ostream &out, const std::string &instanceName, int indentLevel)
            {
                const auto unitIt = unitInfoByInstance.find(instanceName);
                if (unitIt == unitInfoByInstance.end())
                {
                    return false;
                }
                const auto &unitInfo = unitIt->second;
                const std::string indent(static_cast<std::size_t>(indentLevel * 2), ' ');
                bool wroteAny = false;
                for (const auto &edge : manifest.connections)
                {
                    if (edge.kind != "const_to_unit")
                    {
                        continue;
                    }
                    const auto constIt = constSignalByName.find(edge.signal);
                    if (constIt == constSignalByName.end())
                    {
                        continue;
                    }
                    for (const auto &sink : edge.sinks)
                    {
                        if (sink.kind != SinkDesc::Kind::Unit || sink.instanceName != instanceName)
                        {
                            continue;
                        }
                        if (!wroteAny)
                        {
                            out << indent << "// const inputs for " << instanceName << "\n";
                            wroteAny = true;
                        }
                        emitAssign(out,
                                   unitInfo.memberName + "->" + unitInfo.portCppMemberByName.at(sink.portName),
                                   unitInfo.portDescByName.at(sink.portName).isWide,
                                   unitInfo.portDescByName.at(sink.portName).wordCount,
                                   constIt->second.memberName,
                                   constIt->second.desc.isWide,
                                   constIt->second.desc.wordCount,
                                   indentLevel);
                    }
                }
                return wroteAny;
            };

            auto emitUnitUpdatesForUnit = [&](std::ostream &out, const std::string &instanceName, int indentLevel)
            {
                const auto unitIt = unitInfoByInstance.find(instanceName);
                if (unitIt == unitInfoByInstance.end())
                {
                    return false;
                }
                const auto &unitInfo = unitIt->second;
                const std::string indent(static_cast<std::size_t>(indentLevel * 2), ' ');
                bool wroteAny = false;
                for (const auto &edge : manifest.connections)
                {
                    if (edge.driver.kind != DriverDesc::Kind::Unit || edge.driver.instanceName != instanceName)
                    {
                        continue;
                    }
                    const std::string srcExpr =
                        unitInfo.memberName + "->" + unitInfo.portCppMemberByName.at(edge.driver.portName);
                    if (edge.kind == "unit_to_unit")
                    {
                        for (const auto &sink : edge.sinks)
                        {
                            if (sink.kind != SinkDesc::Kind::Unit)
                            {
                                continue;
                            }
                            const auto sinkUnitIt = unitInfoByInstance.find(sink.instanceName);
                            if (sinkUnitIt == unitInfoByInstance.end())
                            {
                                continue;
                            }
                            const auto &sinkUnitInfo = sinkUnitIt->second;
                            if (!wroteAny)
                            {
                                out << indent << "// updates from " << instanceName << "\n";
                                wroteAny = true;
                            }
                            emitAssign(out,
                                       sinkUnitInfo.memberName + "->" + sinkUnitInfo.portCppMemberByName.at(sink.portName),
                                       sinkUnitInfo.portDescByName.at(sink.portName).isWide,
                                       sinkUnitInfo.portDescByName.at(sink.portName).wordCount,
                                       srcExpr,
                                       unitInfo.portDescByName.at(edge.driver.portName).isWide,
                                       unitInfo.portDescByName.at(edge.driver.portName).wordCount,
                                       indentLevel);
                        }
                    }
                    else if (edge.kind == "unit_to_top")
                    {
                        for (const auto &sink : edge.sinks)
                        {
                            if (sink.kind != SinkDesc::Kind::Top)
                            {
                                continue;
                            }
                            const auto topOutputIt = topOutputByPort.find(sink.portName);
                            if (topOutputIt == topOutputByPort.end())
                            {
                                continue;
                            }
                            if (!wroteAny)
                            {
                                out << indent << "// updates from " << instanceName << "\n";
                                wroteAny = true;
                            }
                            emitAssign(out,
                                       topOutputIt->second.memberName,
                                       topOutputIt->second.desc.isWide,
                                       topOutputIt->second.desc.wordCount,
                                       srcExpr,
                                       unitInfo.portDescByName.at(edge.driver.portName).isWide,
                                       unitInfo.portDescByName.at(edge.driver.portName).wordCount,
                                       indentLevel);
                        }
                    }
                }
                return wroteAny;
            };

            std::unordered_set<std::string> usedMethodIdentifiers;
            std::vector<PhaseMethodDef> loadMethods;
            std::vector<PhaseMethodDef> evalMethods;
            std::vector<PhaseMethodDef> updateMethods;
            loadMethods.reserve(normalEvalOrder.size());
            evalMethods.reserve(normalEvalOrder.size());
            updateMethods.reserve(normalEvalOrder.size());
            for (const auto &instanceName : normalEvalOrder)
            {
                const auto unitIt = unitInfoByInstance.find(instanceName);
                if (unitIt == unitInfoByInstance.end())
                {
                    continue;
                }
                const auto timingIndexIt = timingIndexByInstance.find(instanceName);
                if (timingIndexIt == timingIndexByInstance.end())
                {
                    throw std::runtime_error("Missing timing index for unit " + instanceName);
                }
                const auto &unitInfo = unitIt->second;

                {
                    std::ostringstream body;
                    if (emitTopInputLoadsForUnit(body, instanceName, 1))
                    {
                        const std::string methodName =
                            makeUniqueIdentifier("run_load_" + instanceName, usedMethodIdentifiers) + "_";
                        std::ostringstream method;
                        method << "void WolviRepCutVerilatorSim::" << methodName << "(std::size_t workerIndex) {\n";
                        method << "  (void)workerIndex;\n";
                        method << "  PartTimingStats* partTimingStats = &part_timing_stats_["
                               << timingIndexIt->second << "];\n";
                        method << "  const auto inputApplyBegin = WolviClock::now();\n";
                        method << body.str();
                        method << "  const auto inputApplyEnd = WolviClock::now();\n";
                        method << "  partTimingStats->input_apply_ns += elapsed_ns_(inputApplyBegin, inputApplyEnd);\n";
                        method << "  partTimingStats->total_ns += elapsed_ns_(inputApplyBegin, inputApplyEnd);\n";
                        method << "}\n";
                        loadMethods.push_back(PhaseMethodDef{
                            instanceName,
                            methodName,
                            {unitInfo.modelType},
                            method.str(),
                        });
                    }
                }

                {
                    const std::string methodName =
                        makeUniqueIdentifier("run_eval_" + instanceName, usedMethodIdentifiers) + "_";
                    std::ostringstream method;
                    method << "void WolviRepCutVerilatorSim::" << methodName << "(std::size_t workerIndex) {\n";
                    method << "  PartTimingStats* partTimingStats = &part_timing_stats_["
                           << timingIndexIt->second << "];\n";
                    method << "  if (phase_parallel_) {\n";
                    method << "    assert(workerIndex < part_timing_worker_stats_.size());\n";
                    method << "    partTimingStats = &part_timing_worker_stats_[workerIndex]["
                           << timingIndexIt->second << "];\n";
                    method << "  }\n";
                    method << "  const auto evalBegin = WolviClock::now();\n";
                    method << "  // eval " << instanceName << "\n";
                    method << "  " << unitInfo.memberName << "->eval();\n";
                    method << "  const auto evalEnd = WolviClock::now();\n";
                    method << "  partTimingStats->eval_ns += elapsed_ns_(evalBegin, evalEnd);\n";
                    method << "  partTimingStats->total_ns += elapsed_ns_(evalBegin, evalEnd);\n";
                    method << "}\n";
                    evalMethods.push_back(PhaseMethodDef{
                        instanceName,
                        methodName,
                        {unitInfo.modelType},
                        method.str(),
                    });
                }

                {
                    std::ostringstream body;
                    if (emitUnitUpdatesForUnit(body, instanceName, 1))
                    {
                        std::unordered_set<std::string> seenMethodModels;
                        std::vector<std::string> methodModels;
                        seenMethodModels.insert(unitInfo.modelType);
                        methodModels.push_back(unitInfo.modelType);
                        for (const auto &edge : manifest.connections)
                        {
                            if (edge.driver.kind != DriverDesc::Kind::Unit || edge.driver.instanceName != instanceName)
                            {
                                continue;
                            }
                            for (const auto &sink : edge.sinks)
                            {
                                if (sink.kind != SinkDesc::Kind::Unit)
                                {
                                    continue;
                                }
                                const auto sinkUnitIt = unitInfoByInstance.find(sink.instanceName);
                                if (sinkUnitIt == unitInfoByInstance.end())
                                {
                                    continue;
                                }
                                const auto &sinkModelType = sinkUnitIt->second.modelType;
                                if (seenMethodModels.insert(sinkModelType).second)
                                {
                                    methodModels.push_back(sinkModelType);
                                }
                            }
                        }

                        const std::string methodName =
                            makeUniqueIdentifier("run_update_" + instanceName, usedMethodIdentifiers) + "_";
                        std::ostringstream method;
                        method << "void WolviRepCutVerilatorSim::" << methodName << "(std::size_t workerIndex) {\n";
                        method << "  PartTimingStats* partTimingStats = &part_timing_stats_["
                               << timingIndexIt->second << "];\n";
                        method << "  if (phase_parallel_) {\n";
                        method << "    assert(workerIndex < part_timing_worker_stats_.size());\n";
                        method << "    partTimingStats = &part_timing_worker_stats_[workerIndex]["
                               << timingIndexIt->second << "];\n";
                        method << "  }\n";
                        method << "  const auto updatePushBegin = WolviClock::now();\n";
                        method << body.str();
                        method << "  const auto updatePushEnd = WolviClock::now();\n";
                        method << "  partTimingStats->update_push_ns += elapsed_ns_(updatePushBegin, updatePushEnd);\n";
                        method << "  partTimingStats->total_ns += elapsed_ns_(updatePushBegin, updatePushEnd);\n";
                        method << "}\n";
                        updateMethods.push_back(PhaseMethodDef{
                            instanceName,
                            methodName,
                            std::move(methodModels),
                            method.str(),
                        });
                    }
                }
            }

            auto loadChunks = chunkEntriesByEstimatedSize(
                loadMethods, [](const PhaseMethodDef &method) { return method.definition.size(); }, kLoadChunkTargetBytes);
            auto evalChunks = chunkEntriesByEstimatedSize(
                evalMethods, [](const PhaseMethodDef &method) { return method.definition.size(); }, kEvalChunkTargetBytes);
            auto updateChunks = chunkEntriesByEstimatedSize(
                updateMethods, [](const PhaseMethodDef &method) { return method.definition.size(); }, kUpdateChunkTargetBytes);

            auto emitSourcePreamble = [](std::ostream &out, const std::vector<std::string> &modelTypes)
            {
                out << "#include \"wolvi_repcut_verilator_sim.h\"\n\n";
                for (const auto &modelType : modelTypes)
                {
                    out << "#include \"" << modelType << ".h\"\n";
                }
                if (!modelTypes.empty())
                {
                    out << "\n";
                }
            };

            std::ostringstream header;
            header << "#ifndef WOLVI_REPCUT_VERILATOR_SIM_H\n";
            header << "#define WOLVI_REPCUT_VERILATOR_SIM_H\n\n";
            header << "#include <array>\n";
            header << "#include <chrono>\n";
            header << "#include <condition_variable>\n";
            header << "#include <cstddef>\n";
            header << "#include <cstdint>\n";
            header << "#include <memory>\n";
            header << "#include <mutex>\n";
            header << "#include <thread>\n";
            header << "#include <vector>\n";
            header << "#include <verilated.h>\n\n";
            for (const auto &modelType : modelTypesInOrder)
            {
                header << "class " << modelType << ";\n";
            }
            if (!modelTypesInOrder.empty())
            {
                header << "\n";
            }
            header << "class WolviRepCutVerilatorSim {\n";
            header << "public:\n";
            header << "  WolviRepCutVerilatorSim();\n";
            header << "  ~WolviRepCutVerilatorSim();\n";
            header << "  void step();\n";
            if (!topInputs.empty())
            {
                header << "\n";
                for (const auto &signal : topInputs)
                {
                    header << "  void set_" << sanitizeIdentifier(signal.originalName) << "("
                           << signal.desc.typeName;
                    if (signal.desc.isWide)
                    {
                        header << " const& value) { " << signal.memberName << " = value; }\n";
                    }
                    else
                    {
                        header << " value) { " << signal.memberName << " = value; }\n";
                    }
                }
            }
            if (!topOutputs.empty())
            {
                header << "\n";
                for (const auto &signal : topOutputs)
                {
                    header << "  ";
                    if (signal.desc.isWide)
                    {
                        header << "const " << signal.desc.typeName << "& ";
                    }
                    else
                    {
                        header << signal.desc.typeName << " ";
                    }
                    header << "get_" << sanitizeIdentifier(signal.originalName) << "() const { return "
                           << signal.memberName << "; }\n";
                }
            }
            header << "\nprivate:\n";
            header << "  using StepFn = void (WolviRepCutVerilatorSim::*)(std::size_t);\n";
            header << "  using WolviClock = std::chrono::steady_clock;\n\n";
            header << "  template <typename WideDst, typename WideSrc>\n";
            header << "  static void copy_wide_words_(WideDst& dst, const WideSrc& src, std::size_t words) {\n";
            header << "    for (std::size_t i = 0; i < words; ++i) {\n";
            header << "      dst[i] = src[i];\n";
            header << "    }\n";
            header << "  }\n\n";
            header << "  template <typename WideDst, std::size_t N>\n";
            header << "  static void copy_to_verilated_(WideDst& dst, const std::array<WData, N>& src) {\n";
            header << "    for (std::size_t i = 0; i < N; ++i) {\n";
            header << "      dst[i] = src[i];\n";
            header << "    }\n";
            header << "  }\n\n";
            header << "  template <std::size_t N, typename WideSrc>\n";
            header << "  static void copy_from_verilated_(std::array<WData, N>& dst, const WideSrc& src) {\n";
            header << "    for (std::size_t i = 0; i < N; ++i) {\n";
            header << "      dst[i] = src[i];\n";
            header << "    }\n";
            header << "  }\n\n";
            header << "  static std::uint64_t elapsed_ns_(WolviClock::time_point begin, WolviClock::time_point end) {\n";
            header << "    return static_cast<std::uint64_t>(\n";
            header << "        std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());\n";
            header << "  }\n\n";
            header << "  struct PhaseWorker {\n";
            header << "    std::thread thread;\n";
            header << "    std::mutex mutex;\n";
            header << "    std::condition_variable cv;\n";
            header << "    bool hasWork{false};\n";
            header << "    bool completed{false};\n";
            header << "    bool stop{false};\n";
            header << "  };\n\n";
            header << "  struct alignas(64) PartTimingStats {\n";
            header << "    std::uint64_t input_apply_ns{};\n";
            header << "    std::uint64_t eval_ns{};\n";
            header << "    std::uint64_t update_push_ns{};\n";
            header << "    std::uint64_t total_ns{};\n";
            header << "  };\n\n";
            header << "  struct StepTimingStats {\n";
            header << "    std::uint64_t steps{};\n";
            header << "    std::uint64_t input_load_ns{};\n";
            header << "    std::uint64_t part_eval_ns{};\n";
            header << "    std::uint64_t global_update_ns{};\n";
            header << "    std::uint64_t total_ns{};\n";
            header << "  };\n\n";
            header << "  void register_step_fns_();\n";
            header << "  void initialize_phase_workers_();\n";
            header << "  void shutdown_phase_workers_();\n";
            header << "  void phase_worker_loop_(std::size_t workerIndex);\n";
            header << "  void run_host_phase_(const std::vector<StepFn>& phaseFns);\n";
            header << "  void run_phase_workers_(const std::vector<StepFn>& phaseFns);\n";
            header << "  void run_phase_batch_(const std::vector<StepFn>& phaseFns,\n";
            header << "                        std::size_t workerIndex,\n";
            header << "                        std::size_t workerCount);\n";
            header << "  PartTimingStats collect_part_timing_stats_(std::size_t partIndex) const;\n";
            header << "  void report_step_timing_() const;\n";
            header << "  void report_part_timing_() const;\n";
            header << "  void dump_timing_jsonl_() const;\n";
            for (const auto &method : loadMethods)
            {
                header << "  void " << method.methodName << "(std::size_t workerIndex);\n";
            }
            for (const auto &method : evalMethods)
            {
                header << "  void " << method.methodName << "(std::size_t workerIndex);\n";
            }
            for (const auto &method : updateMethods)
            {
                header << "  void " << method.methodName << "(std::size_t workerIndex);\n";
            }
            header << "\n";
            for (const auto &signal : constSignals)
            {
                header << "  static const " << signal.desc.typeName << " " << signal.memberName << ";\n";
            }
            if (!constSignals.empty())
            {
                header << "\n";
            }
            for (const auto &unit : manifest.units)
            {
                const auto &unitInfo = unitInfoByInstance.at(unit.instanceName);
                header << "  std::unique_ptr<" << unitInfo.modelType << "> " << unitInfo.memberName << ";\n";
            }
            if (!manifest.units.empty())
            {
                header << "\n";
            }
            header << "  std::vector<StepFn> load_step_fns_;\n";
            header << "  std::vector<StepFn> eval_step_fns_;\n";
            header << "  std::vector<StepFn> update_step_fns_;\n";
            header << "  const std::vector<StepFn>* active_phase_fns_{};\n";
            header << "  std::unique_ptr<PhaseWorker[]> phase_workers_;\n";
            header << "  std::vector<int> phase_cpu_ids_;\n";
            header << "  std::size_t phase_worker_count_{};\n";
            header << "  bool phase_parallel_{};\n";
            header << "  std::uint64_t step_count_{};\n";
            header << "  StepTimingStats step_timing_{};\n";
            header << "  std::array<PartTimingStats, " << manifest.units.size() << "> part_timing_stats_{};\n\n";
            header << "  std::vector<std::array<PartTimingStats, " << manifest.units.size()
                   << ">> part_timing_worker_stats_{};\n\n";
            for (const auto &signal : topInputs)
            {
                header << "  " << signal.desc.typeName << " " << signal.memberName << "{};\n";
            }
            for (const auto &signal : topOutputs)
            {
                header << "  " << signal.desc.typeName << " " << signal.memberName << "{};\n";
            }
            header << "};\n\n";
            header << "#endif // WOLVI_REPCUT_VERILATOR_SIM_H\n";

            std::vector<WrapperCode::SourceFile> sources;

            std::ostringstream commonSource;
            emitSourcePreamble(commonSource, modelTypesInOrder);
            commonSource << "#include <algorithm>\n";
            commonSource << "#include <cassert>\n";
            commonSource << "#include <cstdio>\n";
            commonSource << "#include <cstdlib>\n";
            commonSource << "#include <utility>\n";
            commonSource << "#if defined(__linux__)\n";
            commonSource << "#include <pthread.h>\n";
            commonSource << "#include <sched.h>\n";
            commonSource << "#endif\n\n";
            commonSource << "namespace {\n";
            commonSource << "inline std::vector<int> wolvi_repcut_available_cpus() {\n";
            commonSource << "#if defined(__linux__)\n";
            commonSource << "  cpu_set_t mask;\n";
            commonSource << "  CPU_ZERO(&mask);\n";
            commonSource << "  if (sched_getaffinity(0, sizeof(mask), &mask) != 0) {\n";
            commonSource << "    return {};\n";
            commonSource << "  }\n";
            commonSource << "  std::vector<int> cpus;\n";
            commonSource << "  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {\n";
            commonSource << "    if (CPU_ISSET(cpu, &mask)) {\n";
            commonSource << "      cpus.push_back(cpu);\n";
            commonSource << "    }\n";
            commonSource << "  }\n";
            commonSource << "  return cpus;\n";
            commonSource << "#else\n";
            commonSource << "  return {};\n";
            commonSource << "#endif\n";
            commonSource << "}\n\n";
            commonSource << "inline bool wolvi_repcut_pin_current_thread(int cpuId) {\n";
            commonSource << "#if defined(__linux__)\n";
            commonSource << "  if (cpuId < 0) {\n";
            commonSource << "    return false;\n";
            commonSource << "  }\n";
            commonSource << "  cpu_set_t mask;\n";
            commonSource << "  CPU_ZERO(&mask);\n";
            commonSource << "  CPU_SET(cpuId, &mask);\n";
            commonSource << "  return pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) == 0;\n";
            commonSource << "#else\n";
            commonSource << "  (void)cpuId;\n";
            commonSource << "  return false;\n";
            commonSource << "#endif\n";
            commonSource << "}\n\n";
            commonSource << "inline std::size_t wolvi_repcut_requested_workers() {\n";
            commonSource << "  const char* env = std::getenv(\"XS_EMU_THREADS\");\n";
            commonSource << "  if (env == nullptr || *env == '\\0') {\n";
            commonSource << "    return 0;\n";
            commonSource << "  }\n";
            commonSource << "  char* end = nullptr;\n";
            commonSource << "  const unsigned long long value = std::strtoull(env, &end, 10);\n";
            commonSource << "  if (end == env || (end != nullptr && *end != '\\0')) {\n";
            commonSource << "    assert(false && \"XS_EMU_THREADS must be an unsigned integer\");\n";
            commonSource << "    return 0;\n";
            commonSource << "  }\n";
            commonSource << "  return static_cast<std::size_t>(value);\n";
            commonSource << "}\n";
            commonSource << "} // namespace\n\n";

            for (const auto &signal : constSignals)
            {
                const auto edgeIt = std::find_if(manifest.connections.begin(), manifest.connections.end(),
                                                 [&](const ManifestEdge &edge) { return edge.signal == signal.originalName; });
                const int64_t width = edgeIt != manifest.connections.end() ? edgeIt->width : 1;
                commonSource << "const " << signal.desc.typeName << " WolviRepCutVerilatorSim::" << signal.memberName
                             << " = " << cppConstLiteral(constLiteralBySignal.at(signal.originalName), width) << ";\n";
            }
            if (!constSignals.empty())
            {
                commonSource << "\n";
            }

            commonSource << "WolviRepCutVerilatorSim::WolviRepCutVerilatorSim()\n";
            if (!manifest.units.empty())
            {
                commonSource << "  : ";
                for (std::size_t i = 0; i < manifest.units.size(); ++i)
                {
                    const auto &unit = manifest.units[i];
                    const auto &unitInfo = unitInfoByInstance.at(unit.instanceName);
                    if (i != 0)
                    {
                        commonSource << ", ";
                    }
                    commonSource << unitInfo.memberName << "(std::make_unique<" << unitInfo.modelType << ">())";
                }
            }
            commonSource << " {\n";
            for (const auto &instanceName : normalEvalOrder)
            {
                emitConstLoadsForUnit(commonSource, instanceName, 1);
            }
            commonSource << "  register_step_fns_();\n";
            commonSource << "  initialize_phase_workers_();\n";
            commonSource << "}\n\n";
            commonSource << "WolviRepCutVerilatorSim::~WolviRepCutVerilatorSim() {\n";
            commonSource << "  report_step_timing_();\n";
            commonSource << "  report_part_timing_();\n";
            commonSource << "  dump_timing_jsonl_();\n";
            commonSource << "  shutdown_phase_workers_();\n";
            commonSource << "}\n\n";
            commonSource << "void WolviRepCutVerilatorSim::register_step_fns_() {\n";
            commonSource << "  load_step_fns_.reserve(" << loadMethods.size() << ");\n";
            for (const auto &method : loadMethods)
            {
                commonSource << "  load_step_fns_.push_back(&WolviRepCutVerilatorSim::" << method.methodName
                             << ");\n";
            }
            commonSource << "  eval_step_fns_.reserve(" << evalMethods.size() << ");\n";
            for (const auto &method : evalMethods)
            {
                commonSource << "  eval_step_fns_.push_back(&WolviRepCutVerilatorSim::" << method.methodName
                             << ");\n";
            }
            commonSource << "  update_step_fns_.reserve(" << updateMethods.size() << ");\n";
            for (const auto &method : updateMethods)
            {
                commonSource << "  update_step_fns_.push_back(&WolviRepCutVerilatorSim::" << method.methodName
                             << ");\n";
            }
            commonSource << "}\n\n";
            commonSource << "void WolviRepCutVerilatorSim::initialize_phase_workers_() {\n";
            commonSource << "  const std::size_t requestedWorkers = wolvi_repcut_requested_workers();\n";
            commonSource << "  const std::size_t maxParallelFns = std::max(eval_step_fns_.size(), update_step_fns_.size());\n";
            commonSource << "  assert(requestedWorkers <= maxParallelFns && \"XS_EMU_THREADS must not exceed repcut partition count\");\n";
            commonSource << "  if (maxParallelFns < 2 || requestedWorkers < 2) {\n";
            commonSource << "    return;\n";
            commonSource << "  }\n";
            commonSource << "  phase_cpu_ids_ = wolvi_repcut_available_cpus();\n";
            commonSource << "  if (phase_cpu_ids_.size() < 2) {\n";
            commonSource << "    phase_cpu_ids_.clear();\n";
            commonSource << "    return;\n";
            commonSource << "  }\n";
            commonSource << "  phase_worker_count_ = std::min(maxParallelFns, requestedWorkers);\n";
            commonSource << "  phase_worker_count_ = std::min(phase_worker_count_, phase_cpu_ids_.size());\n";
            commonSource << "  if (phase_worker_count_ < 2) {\n";
            commonSource << "    phase_worker_count_ = 0;\n";
            commonSource << "    phase_cpu_ids_.clear();\n";
            commonSource << "    return;\n";
            commonSource << "  }\n";
            commonSource << "  if (!phase_cpu_ids_.empty()) {\n";
            commonSource << "    phase_cpu_ids_.resize(phase_worker_count_);\n";
            commonSource << "  }\n";
            commonSource << "  part_timing_worker_stats_.assign(phase_worker_count_, {});\n";
            commonSource << "  phase_workers_ = std::make_unique<PhaseWorker[]>(phase_worker_count_);\n";
            commonSource << "  for (std::size_t workerIndex = 0; workerIndex < phase_worker_count_; ++workerIndex) {\n";
            commonSource << "    phase_workers_[workerIndex].thread = std::thread([this, workerIndex]() {\n";
            commonSource << "      if (workerIndex < phase_cpu_ids_.size()) {\n";
            commonSource << "        wolvi_repcut_pin_current_thread(phase_cpu_ids_[workerIndex]);\n";
            commonSource << "      }\n";
            commonSource << "      phase_worker_loop_(workerIndex);\n";
            commonSource << "    });\n";
            commonSource << "  }\n";
            commonSource << "  phase_parallel_ = true;\n";
            commonSource << "}\n\n";
            commonSource << "void WolviRepCutVerilatorSim::shutdown_phase_workers_() {\n";
            commonSource << "  if (!phase_workers_) {\n";
            commonSource << "    return;\n";
            commonSource << "  }\n";
            commonSource << "  for (std::size_t workerIndex = 0; workerIndex < phase_worker_count_; ++workerIndex) {\n";
            commonSource << "    auto &worker = phase_workers_[workerIndex];\n";
            commonSource << "    {\n";
            commonSource << "      std::lock_guard<std::mutex> lock(worker.mutex);\n";
            commonSource << "      worker.stop = true;\n";
            commonSource << "      worker.hasWork = true;\n";
            commonSource << "    }\n";
            commonSource << "    worker.cv.notify_one();\n";
            commonSource << "  }\n";
            commonSource << "  for (std::size_t workerIndex = 0; workerIndex < phase_worker_count_; ++workerIndex) {\n";
            commonSource << "    auto &worker = phase_workers_[workerIndex];\n";
            commonSource << "    if (worker.thread.joinable()) {\n";
            commonSource << "      worker.thread.join();\n";
            commonSource << "    }\n";
            commonSource << "  }\n";
            commonSource << "  phase_workers_.reset();\n";
            commonSource << "  part_timing_worker_stats_.clear();\n";
            commonSource << "  phase_cpu_ids_.clear();\n";
            commonSource << "  phase_worker_count_ = 0;\n";
            commonSource << "  phase_parallel_ = false;\n";
            commonSource << "}\n\n";
            commonSource << "void WolviRepCutVerilatorSim::phase_worker_loop_(std::size_t workerIndex) {\n";
            commonSource << "  auto &worker = phase_workers_[workerIndex];\n";
            commonSource << "  while (true) {\n";
            commonSource << "    std::unique_lock<std::mutex> lock(worker.mutex);\n";
            commonSource << "    worker.cv.wait(lock, [&worker]() { return worker.hasWork; });\n";
            commonSource << "    if (worker.stop) {\n";
            commonSource << "      return;\n";
            commonSource << "    }\n";
            commonSource << "    worker.hasWork = false;\n";
            commonSource << "    const auto* phaseFns = active_phase_fns_;\n";
            commonSource << "    lock.unlock();\n";
            commonSource << "    if (phaseFns != nullptr) {\n";
            commonSource << "      run_phase_batch_(*phaseFns, workerIndex, phase_worker_count_);\n";
            commonSource << "    }\n";
            commonSource << "    lock.lock();\n";
            commonSource << "    worker.completed = true;\n";
            commonSource << "    lock.unlock();\n";
            commonSource << "    worker.cv.notify_one();\n";
            commonSource << "  }\n";
            commonSource << "}\n\n";
            commonSource << "void WolviRepCutVerilatorSim::run_host_phase_(const std::vector<StepFn>& phaseFns) {\n";
            commonSource << "  for (const auto stepFn : phaseFns) {\n";
            commonSource << "    (this->*stepFn)(0);\n";
            commonSource << "  }\n";
            commonSource << "}\n\n";
            commonSource << "void WolviRepCutVerilatorSim::run_phase_batch_(const std::vector<StepFn>& phaseFns,\n";
            commonSource << "                                            std::size_t workerIndex,\n";
            commonSource << "                                            std::size_t workerCount) {\n";
            commonSource << "  if (workerCount == 0 || phaseFns.empty()) {\n";
            commonSource << "    return;\n";
            commonSource << "  }\n";
            commonSource << "  const std::size_t begin = phaseFns.size() * workerIndex / workerCount;\n";
            commonSource << "  const std::size_t end = phaseFns.size() * (workerIndex + 1) / workerCount;\n";
            commonSource << "  for (std::size_t index = begin; index < end; ++index) {\n";
            commonSource << "    (this->*phaseFns[index])(workerIndex);\n";
            commonSource << "  }\n";
            commonSource << "}\n\n";
            commonSource << "void WolviRepCutVerilatorSim::run_phase_workers_(const std::vector<StepFn>& phaseFns) {\n";
            commonSource << "  if (phaseFns.empty()) {\n";
            commonSource << "    return;\n";
            commonSource << "  }\n";
            commonSource << "  if (!phase_parallel_) {\n";
            commonSource << "    run_phase_batch_(phaseFns, 0, 1);\n";
            commonSource << "    return;\n";
            commonSource << "  }\n";
            commonSource << "  active_phase_fns_ = &phaseFns;\n";
            commonSource << "  for (std::size_t workerIndex = 0; workerIndex < phase_worker_count_; ++workerIndex) {\n";
            commonSource << "    auto &worker = phase_workers_[workerIndex];\n";
            commonSource << "    {\n";
            commonSource << "      std::lock_guard<std::mutex> lock(worker.mutex);\n";
            commonSource << "      worker.completed = false;\n";
            commonSource << "      worker.hasWork = true;\n";
            commonSource << "    }\n";
            commonSource << "    worker.cv.notify_one();\n";
            commonSource << "  }\n";
            commonSource << "  for (std::size_t workerIndex = 0; workerIndex < phase_worker_count_; ++workerIndex) {\n";
            commonSource << "    auto &worker = phase_workers_[workerIndex];\n";
            commonSource << "    std::unique_lock<std::mutex> lock(worker.mutex);\n";
            commonSource << "    worker.cv.wait(lock, [&worker]() { return worker.completed; });\n";
            commonSource << "  }\n";
            commonSource << "  active_phase_fns_ = nullptr;\n";
            commonSource << "}\n\n";
            commonSource << "WolviRepCutVerilatorSim::PartTimingStats WolviRepCutVerilatorSim::collect_part_timing_stats_(std::size_t partIndex) const {\n";
            commonSource << "  PartTimingStats total = part_timing_stats_[partIndex];\n";
            commonSource << "  for (const auto& workerStats : part_timing_worker_stats_) {\n";
            commonSource << "    total.input_apply_ns += workerStats[partIndex].input_apply_ns;\n";
            commonSource << "    total.eval_ns += workerStats[partIndex].eval_ns;\n";
            commonSource << "    total.update_push_ns += workerStats[partIndex].update_push_ns;\n";
            commonSource << "    total.total_ns += workerStats[partIndex].total_ns;\n";
            commonSource << "  }\n";
            commonSource << "  return total;\n";
            commonSource << "}\n\n";
            commonSource << "void WolviRepCutVerilatorSim::report_step_timing_() const {\n";
            commonSource << "  if (step_timing_.steps == 0) {\n";
            commonSource << "    return;\n";
            commonSource << "  }\n";
            commonSource << "  const double steps = static_cast<double>(step_timing_.steps);\n";
            commonSource << "  const double totalMs = static_cast<double>(step_timing_.total_ns) / 1.0e6;\n";
            commonSource << "  const auto printPhase = [&](const char* name, std::uint64_t ns) {\n";
            commonSource << "    const double totalPhaseMs = static_cast<double>(ns) / 1.0e6;\n";
            commonSource << "    const double avgPhaseUs = static_cast<double>(ns) / steps / 1.0e3;\n";
            commonSource << "    const double pct = step_timing_.total_ns == 0 ? 0.0 :\n";
            commonSource << "                       (100.0 * static_cast<double>(ns) / static_cast<double>(step_timing_.total_ns));\n";
            commonSource << "    std::fprintf(stderr,\n";
            commonSource << "                 \"[WOLVI][step-timing] %s total=%.3f ms avg=%.3f us pct=%.2f%%\\n\",\n";
            commonSource << "                 name,\n";
            commonSource << "                 totalPhaseMs,\n";
            commonSource << "                 avgPhaseUs,\n";
            commonSource << "                 pct);\n";
            commonSource << "  };\n";
            commonSource << "  std::fprintf(stderr,\n";
            commonSource << "               \"[WOLVI][step-timing] steps=%llu total=%.3f ms avg=%.3f us\\n\",\n";
            commonSource << "               static_cast<unsigned long long>(step_timing_.steps),\n";
            commonSource << "               totalMs,\n";
            commonSource << "               totalMs * 1000.0 / steps);\n";
            commonSource << "  printPhase(\"input_load\", step_timing_.input_load_ns);\n";
            commonSource << "  printPhase(\"part_eval\", step_timing_.part_eval_ns);\n";
            commonSource << "  printPhase(\"global_update\", step_timing_.global_update_ns);\n";
            commonSource << "}\n\n";
            commonSource << "void WolviRepCutVerilatorSim::report_part_timing_() const {\n";
            commonSource << "  if (step_count_ == 0) {\n";
            commonSource << "    return;\n";
            commonSource << "  }\n";
            commonSource << "  static constexpr std::array<const char*, " << manifest.units.size() << "> kPartNames = {";
            for (std::size_t i = 0; i < manifest.units.size(); ++i)
            {
                if (i != 0)
                {
                    commonSource << ", ";
                }
                commonSource << "\"" << manifest.units[i].instanceName << "\"";
            }
            commonSource << "};\n";
            commonSource << "  for (std::size_t partIndex = 0; partIndex < kPartNames.size(); ++partIndex) {\n";
            commonSource << "    const PartTimingStats partStats = collect_part_timing_stats_(partIndex);\n";
            commonSource << "    const double totalMs = static_cast<double>(partStats.total_ns) / 1.0e6;\n";
            commonSource << "    const double avgUs = static_cast<double>(partStats.total_ns) /\n";
            commonSource << "                         static_cast<double>(step_count_) / 1.0e3;\n";
            commonSource << "    const double inputApplyAvgUs = static_cast<double>(partStats.input_apply_ns) /\n";
            commonSource << "                                   static_cast<double>(step_count_) / 1.0e3;\n";
            commonSource << "    const double evalAvgUs = static_cast<double>(partStats.eval_ns) /\n";
            commonSource << "                             static_cast<double>(step_count_) / 1.0e3;\n";
            commonSource << "    const double updatePushAvgUs = static_cast<double>(partStats.update_push_ns) /\n";
            commonSource << "                                   static_cast<double>(step_count_) / 1.0e3;\n";
            commonSource << "    std::fprintf(stderr,\n";
            commonSource << "                 \"[WOLVI][part-timing] part=%s steps=%llu total=%.3f ms avg=%.3f us input_apply=%.3f us eval=%.3f us update_push=%.3f us\\n\",\n";
            commonSource << "                 kPartNames[partIndex],\n";
            commonSource << "                 static_cast<unsigned long long>(step_count_),\n";
            commonSource << "                 totalMs,\n";
            commonSource << "                 avgUs,\n";
            commonSource << "                 inputApplyAvgUs,\n";
            commonSource << "                 evalAvgUs,\n";
            commonSource << "                 updatePushAvgUs);\n";
            commonSource << "  }\n";
            commonSource << "}\n\n";
            commonSource << "void WolviRepCutVerilatorSim::dump_timing_jsonl_() const {\n";
            commonSource << "  const char* path = std::getenv(\"WOLVI_REPCUT_TIMING_JSONL\");\n";
            commonSource << "  if (path == nullptr || *path == '\\0') {\n";
            commonSource << "    return;\n";
            commonSource << "  }\n";
            commonSource << "  FILE* file = std::fopen(path, \"w\");\n";
            commonSource << "  if (file == nullptr) {\n";
            commonSource << "    return;\n";
            commonSource << "  }\n";
            commonSource << "  if (step_timing_.steps != 0) {\n";
            commonSource << "    const double steps = static_cast<double>(step_timing_.steps);\n";
            commonSource << "    const double totalMs = static_cast<double>(step_timing_.total_ns) / 1.0e6;\n";
            commonSource << "    const double avgUs = static_cast<double>(step_timing_.total_ns) / steps / 1.0e3;\n";
            commonSource << "    std::fprintf(file,\n";
            commonSource << "                 \"{\\\"record_type\\\":\\\"step_timing_summary\\\",\\\"schema_version\\\":1,\\\"steps\\\":%llu,\\\"total_ms\\\":%.3f,\\\"avg_us\\\":%.3f}\\n\",\n";
            commonSource << "                 static_cast<unsigned long long>(step_timing_.steps),\n";
            commonSource << "                 totalMs,\n";
            commonSource << "                 avgUs);\n";
            commonSource << "    const auto dumpPhase = [&](const char* name, std::uint64_t ns) {\n";
            commonSource << "      const double phaseTotalMs = static_cast<double>(ns) / 1.0e6;\n";
            commonSource << "      const double phaseAvgUs = static_cast<double>(ns) / steps / 1.0e3;\n";
            commonSource << "      const double pct = step_timing_.total_ns == 0 ? 0.0 :\n";
            commonSource << "                         (100.0 * static_cast<double>(ns) / static_cast<double>(step_timing_.total_ns));\n";
            commonSource << "      std::fprintf(file,\n";
            commonSource << "                   \"{\\\"record_type\\\":\\\"step_timing_phase\\\",\\\"schema_version\\\":1,\\\"phase\\\":\\\"%s\\\",\\\"steps\\\":%llu,\\\"total_ms\\\":%.3f,\\\"avg_us\\\":%.3f,\\\"pct\\\":%.2f}\\n\",\n";
            commonSource << "                   name,\n";
            commonSource << "                   static_cast<unsigned long long>(step_timing_.steps),\n";
            commonSource << "                   phaseTotalMs,\n";
            commonSource << "                   phaseAvgUs,\n";
            commonSource << "                   pct);\n";
            commonSource << "    };\n";
            commonSource << "    dumpPhase(\"input_load\", step_timing_.input_load_ns);\n";
            commonSource << "    dumpPhase(\"part_eval\", step_timing_.part_eval_ns);\n";
            commonSource << "    dumpPhase(\"global_update\", step_timing_.global_update_ns);\n";
            commonSource << "  }\n";
            commonSource << "  if (step_count_ != 0) {\n";
            commonSource << "    static constexpr std::array<const char*, " << manifest.units.size() << "> kPartNames = {";
            for (std::size_t i = 0; i < manifest.units.size(); ++i)
            {
                if (i != 0)
                {
                    commonSource << ", ";
                }
                commonSource << "\"" << manifest.units[i].instanceName << "\"";
            }
            commonSource << "};\n";
            commonSource << "    for (std::size_t partIndex = 0; partIndex < kPartNames.size(); ++partIndex) {\n";
            commonSource << "      const PartTimingStats partStats = collect_part_timing_stats_(partIndex);\n";
            commonSource << "      const double totalMs = static_cast<double>(partStats.total_ns) / 1.0e6;\n";
            commonSource << "      const double avgUs = static_cast<double>(partStats.total_ns) /\n";
            commonSource << "                           static_cast<double>(step_count_) / 1.0e3;\n";
            commonSource << "      const double inputApplyTotalMs = static_cast<double>(partStats.input_apply_ns) / 1.0e6;\n";
            commonSource << "      const double inputApplyAvgUs = static_cast<double>(partStats.input_apply_ns) /\n";
            commonSource << "                                    static_cast<double>(step_count_) / 1.0e3;\n";
            commonSource << "      const double evalTotalMs = static_cast<double>(partStats.eval_ns) / 1.0e6;\n";
            commonSource << "      const double evalAvgUs = static_cast<double>(partStats.eval_ns) /\n";
            commonSource << "                               static_cast<double>(step_count_) / 1.0e3;\n";
            commonSource << "      const double updatePushTotalMs = static_cast<double>(partStats.update_push_ns) / 1.0e6;\n";
            commonSource << "      const double updatePushAvgUs = static_cast<double>(partStats.update_push_ns) /\n";
            commonSource << "                                   static_cast<double>(step_count_) / 1.0e3;\n";
            commonSource << "      std::fprintf(file,\n";
            commonSource << "                   \"{\\\"record_type\\\":\\\"part_timing\\\",\\\"schema_version\\\":1,\\\"part_name\\\":\\\"%s\\\",\\\"steps\\\":%llu,\\\"total_ms\\\":%.3f,\\\"avg_us\\\":%.3f,\\\"input_apply_total_ms\\\":%.3f,\\\"input_apply_avg_us\\\":%.3f,\\\"eval_total_ms\\\":%.3f,\\\"eval_avg_us\\\":%.3f,\\\"update_push_total_ms\\\":%.3f,\\\"update_push_avg_us\\\":%.3f}\\n\",\n";
            commonSource << "                   kPartNames[partIndex],\n";
            commonSource << "                   static_cast<unsigned long long>(step_count_),\n";
            commonSource << "                   totalMs,\n";
            commonSource << "                   avgUs,\n";
            commonSource << "                   inputApplyTotalMs,\n";
            commonSource << "                   inputApplyAvgUs,\n";
            commonSource << "                   evalTotalMs,\n";
            commonSource << "                   evalAvgUs,\n";
            commonSource << "                   updatePushTotalMs,\n";
            commonSource << "                   updatePushAvgUs);\n";
            commonSource << "    }\n";
            commonSource << "  }\n";
            commonSource << "  std::fclose(file);\n";
            commonSource << "}\n\n";
            commonSource << "void WolviRepCutVerilatorSim::step() {\n";
            commonSource << "  ++step_count_;\n";
            commonSource << "  ++step_timing_.steps;\n";
            commonSource << "  const auto stepBegin = WolviClock::now();\n";
            commonSource << "  const auto inputLoadBegin = stepBegin;\n";
            commonSource << "  run_host_phase_(load_step_fns_);\n";
            commonSource << "  const auto inputLoadEnd = WolviClock::now();\n";
            commonSource << "  step_timing_.input_load_ns += elapsed_ns_(inputLoadBegin, inputLoadEnd);\n";
            commonSource << "  const auto partEvalBegin = inputLoadEnd;\n";
            commonSource << "  run_phase_workers_(eval_step_fns_);\n";
            commonSource << "  const auto partEvalEnd = WolviClock::now();\n";
            commonSource << "  step_timing_.part_eval_ns += elapsed_ns_(partEvalBegin, partEvalEnd);\n";
            commonSource << "  const auto globalUpdateBegin = partEvalEnd;\n";
            commonSource << "  run_phase_workers_(update_step_fns_);\n";
            commonSource << "  const auto globalUpdateEnd = WolviClock::now();\n";
            commonSource << "  step_timing_.global_update_ns += elapsed_ns_(globalUpdateBegin, globalUpdateEnd);\n";
            commonSource << "  step_timing_.total_ns += elapsed_ns_(stepBegin, globalUpdateEnd);\n";
            commonSource << "}\n";
            sources.push_back(WrapperCode::SourceFile{
                "wolvi_repcut_verilator_sim_common.cpp",
                commonSource.str(),
            });

            for (std::size_t chunkIndex = 0; chunkIndex < loadChunks.size(); ++chunkIndex)
            {
                std::unordered_set<std::string> seenChunkModels;
                std::vector<std::string> chunkModels;
                std::ostringstream chunkSource;
                for (const std::size_t methodIndex : loadChunks[chunkIndex])
                {
                    const auto &method = loadMethods[methodIndex];
                    for (const auto &modelType : method.modelTypes)
                    {
                        if (seenChunkModels.insert(modelType).second)
                        {
                            chunkModels.push_back(modelType);
                        }
                    }
                }
                emitSourcePreamble(chunkSource, chunkModels);
                for (const std::size_t methodIndex : loadChunks[chunkIndex])
                {
                    chunkSource << loadMethods[methodIndex].definition << "\n";
                }
                sources.push_back(WrapperCode::SourceFile{
                    "wolvi_repcut_verilator_sim_load_" + std::to_string(chunkIndex) + ".cpp",
                    chunkSource.str(),
                });
            }

            for (std::size_t chunkIndex = 0; chunkIndex < evalChunks.size(); ++chunkIndex)
            {
                std::unordered_set<std::string> seenChunkModels;
                std::vector<std::string> chunkModels;
                std::ostringstream chunkSource;
                for (const std::size_t methodIndex : evalChunks[chunkIndex])
                {
                    const auto &method = evalMethods[methodIndex];
                    for (const auto &modelType : method.modelTypes)
                    {
                        if (seenChunkModels.insert(modelType).second)
                        {
                            chunkModels.push_back(modelType);
                        }
                    }
                }
                emitSourcePreamble(chunkSource, chunkModels);
                for (const std::size_t methodIndex : evalChunks[chunkIndex])
                {
                    chunkSource << evalMethods[methodIndex].definition << "\n";
                }
                sources.push_back(WrapperCode::SourceFile{
                    "wolvi_repcut_verilator_sim_eval_" + std::to_string(chunkIndex) + ".cpp",
                    chunkSource.str(),
                });
            }

            for (std::size_t chunkIndex = 0; chunkIndex < updateChunks.size(); ++chunkIndex)
            {
                std::unordered_set<std::string> seenChunkModels;
                std::vector<std::string> chunkModels;
                std::ostringstream chunkSource;
                for (const std::size_t methodIndex : updateChunks[chunkIndex])
                {
                    const auto &method = updateMethods[methodIndex];
                    for (const auto &modelType : method.modelTypes)
                    {
                        if (seenChunkModels.insert(modelType).second)
                        {
                            chunkModels.push_back(modelType);
                        }
                    }
                }
                emitSourcePreamble(chunkSource, chunkModels);
                for (const std::size_t methodIndex : updateChunks[chunkIndex])
                {
                    chunkSource << updateMethods[methodIndex].definition << "\n";
                }
                sources.push_back(WrapperCode::SourceFile{
                    "wolvi_repcut_verilator_sim_update_" + std::to_string(chunkIndex) + ".cpp",
                    chunkSource.str(),
                });
            }

            return WrapperCode{header.str(), std::move(sources)};
        }

        BuildGlueCode generateBuildGlueCode(const PackageManifest &manifest,
                                            const std::vector<std::string> &wrapperSourceNames)
        {
            std::unordered_set<std::string> usedUnitKeys;
            struct UnitBuildInfo
            {
                std::string key;
                std::string instanceName;
                std::string moduleName;
                std::string prefix;
                std::string fileList;
                std::string mdirExpr;
            };
            std::vector<UnitBuildInfo> units;
            units.reserve(manifest.units.size());
            for (const auto &unit : manifest.units)
            {
                const std::string key = makeUniqueIdentifier(unit.instanceName, usedUnitKeys);
                units.push_back(UnitBuildInfo{
                    key,
                    unit.instanceName,
                    unit.moduleName,
                    "V" + unit.moduleName,
                    (std::filesystem::path("verilate") / (unit.instanceName + ".f")).generic_string(),
                    "$(VERILATED_DIR)/" + sanitizeIdentifier(unit.instanceName),
                });
            }

            std::ostringstream smokeMain;
            smokeMain << "#include \"wolvi_repcut_verilator_sim.h\"\n\n";
            smokeMain << "int main() {\n";
            smokeMain << "  WolviRepCutVerilatorSim sim;\n";
            smokeMain << "  sim.step();\n";
            smokeMain << "  return 0;\n";
            smokeMain << "}\n";

            std::ostringstream unitsMk;
            unitsMk << "PARTITIONED_UNITS :=";
            for (const auto &unit : units)
            {
                unitsMk << " " << unit.key;
            }
            unitsMk << "\n\n";
            unitsMk << "PARTITIONED_VERILATOR_FLAGS ?=\n";
            unitsMk << "PARTITIONED_UNIT_MAKE_J ?= $(if $(strip $(VM_BUILD_JOBS)),-j $(VM_BUILD_JOBS),)\n";
            unitsMk << "PARTITIONED_VM_PARALLEL_BUILDS ?= $(VM_PARALLEL_BUILDS)\n\n";
            unitsMk << "UNIT_MKS :=\n";
            unitsMk << "UNIT_ARCHIVES :=\n\n";
            for (const auto &unit : units)
            {
                unitsMk << "UNIT_" << unit.key << "_INSTANCE := " << unit.instanceName << "\n";
                unitsMk << "UNIT_" << unit.key << "_MODULE := " << unit.moduleName << "\n";
                unitsMk << "UNIT_" << unit.key << "_PREFIX := " << unit.prefix << "\n";
                unitsMk << "UNIT_" << unit.key << "_FILELIST := $(PACKAGE_ROOT)/" << unit.fileList << "\n";
                unitsMk << "UNIT_" << unit.key << "_MDIR := " << unit.mdirExpr << "\n";
                unitsMk << "UNIT_" << unit.key << "_MK := $(UNIT_" << unit.key << "_MDIR)/" << unit.prefix << ".mk\n";
                unitsMk << "UNIT_" << unit.key << "_ARCHIVE := $(UNIT_" << unit.key << "_MDIR)/" << unit.prefix << "__ALL.a\n";
                unitsMk << "UNIT_MKS += $(UNIT_" << unit.key << "_MK)\n";
                unitsMk << "UNIT_ARCHIVES += $(UNIT_" << unit.key << "_ARCHIVE)\n\n";

                unitsMk << "$(UNIT_" << unit.key << "_MK): $(UNIT_" << unit.key << "_FILELIST) $(SV_SOURCES)\n";
                unitsMk << "\t@mkdir -p $(@D)\n";
                unitsMk << "\t$(VERILATOR) --cc -f $(UNIT_" << unit.key << "_FILELIST) \\\n";
                unitsMk << "\t  $(PARTITIONED_VERILATOR_FLAGS) \\\n";
                unitsMk << "\t  --top-module $(UNIT_" << unit.key << "_MODULE) \\\n";
                unitsMk << "\t  --prefix $(UNIT_" << unit.key << "_PREFIX) \\\n";
                unitsMk << "\t  --Mdir $(UNIT_" << unit.key << "_MDIR)\n\n";

                unitsMk << "$(UNIT_" << unit.key << "_ARCHIVE): $(UNIT_" << unit.key << "_MK)\n";
                unitsMk << "\t$(MAKE) $(PARTITIONED_UNIT_MAKE_J) -C $(UNIT_" << unit.key
                        << "_MDIR) VM_PARALLEL_BUILDS=$(PARTITIONED_VM_PARALLEL_BUILDS) OBJCACHE= -f "
                        << unit.prefix << ".mk " << unit.prefix << "__ALL.a\n\n";
            }

            std::ostringstream makefile;
            makefile << "PACKAGE_ROOT ?= $(abspath .)\n";
            makefile << "VERILATOR ?= verilator\n";
            makefile << "VERILATOR_ROOT ?= $(shell $(VERILATOR) --getenv VERILATOR_ROOT 2>/dev/null)\n";
            makefile << "PARTITIONED_VERILATOR_FLAGS ?= --no-timing -Wno-STMTDLY -Wno-WIDTH -Wno-WIDTHTRUNC --output-split 30000 --output-split-cfuncs 30000\n";
            makefile << "BUILD_DIR ?= $(PACKAGE_ROOT)/build\n";
            makefile << "VERILATED_DIR ?= $(BUILD_DIR)/verilated\n";
            makefile << "VM_BUILD_JOBS ?=\n";
            makefile << "VM_PARALLEL_BUILDS ?= 1\n";
            makefile << "CXX ?= c++\n";
            makefile << "CXXFLAGS ?= -O2 -std=c++17\n";
            makefile << "CPPFLAGS ?=\n";
            makefile << "LDFLAGS ?=\n";
            makefile << "LDLIBS ?=\n";
            makefile << "SV_SOURCES := $(wildcard $(PACKAGE_ROOT)/sv/*.sv)\n";
            makefile << "WRAPPER_SRC_NAMES :=";
            for (const auto &sourceName : wrapperSourceNames)
            {
                makefile << " " << sourceName;
            }
            makefile << "\n";
            makefile << "WRAPPER_SRCS := $(addprefix $(PACKAGE_ROOT)/,$(WRAPPER_SRC_NAMES))\n";
            makefile << "WRAPPER_OBJS := $(addprefix $(BUILD_DIR)/,$(WRAPPER_SRC_NAMES:.cpp=.o))\n";
            makefile << "SMOKE_MAIN_OBJ := $(BUILD_DIR)/partitioned_smoke_main.o\n";
            makefile << "VERILATED_OBJ := $(BUILD_DIR)/verilated.o\n";
            makefile << "VERILATED_DPI_OBJ := $(BUILD_DIR)/verilated_dpi.o\n";
            makefile << "VERILATED_THREADS_OBJ := $(BUILD_DIR)/verilated_threads.o\n";
            makefile << "TARGET := $(BUILD_DIR)/partitioned-smoke\n";
            makefile << ".DEFAULT_GOAL := all\n";
            makefile << "\ninclude units.mk\n\n";
            makefile << "UNIT_INCLUDE_FLAGS := $(foreach unit,$(PARTITIONED_UNITS),-I$(UNIT_$(unit)_MDIR))\n";
            makefile << "COMMON_CPPFLAGS := -I$(PACKAGE_ROOT) -I$(VERILATOR_ROOT)/include -I$(VERILATOR_ROOT)/include/vltstd $(UNIT_INCLUDE_FLAGS) $(CPPFLAGS)\n";
            makefile << "\nall: $(TARGET)\n\n";
            makefile << "verilate-units: $(UNIT_ARCHIVES)\n\n";
            makefile << "$(BUILD_DIR):\n";
            makefile << "\t@mkdir -p $@\n\n";
            makefile << "$(VERILATED_OBJ): $(VERILATOR_ROOT)/include/verilated.cpp | $(BUILD_DIR)\n";
            makefile << "\t$(CXX) $(COMMON_CPPFLAGS) $(CXXFLAGS) -c -o $@ $<\n\n";
            makefile << "$(VERILATED_DPI_OBJ): $(VERILATOR_ROOT)/include/verilated_dpi.cpp | $(BUILD_DIR)\n";
            makefile << "\t$(CXX) $(COMMON_CPPFLAGS) $(CXXFLAGS) -c -o $@ $<\n\n";
            makefile << "$(VERILATED_THREADS_OBJ): $(VERILATOR_ROOT)/include/verilated_threads.cpp | $(BUILD_DIR)\n";
            makefile << "\t$(CXX) $(COMMON_CPPFLAGS) $(CXXFLAGS) -c -o $@ $<\n\n";
            makefile << "$(BUILD_DIR)/%.o: $(PACKAGE_ROOT)/%.cpp $(PACKAGE_ROOT)/wolvi_repcut_verilator_sim.h $(UNIT_ARCHIVES) | $(BUILD_DIR)\n";
            makefile << "\t$(CXX) $(COMMON_CPPFLAGS) $(CXXFLAGS) -c -o $@ $<\n\n";
            makefile << "$(SMOKE_MAIN_OBJ): $(PACKAGE_ROOT)/partitioned_smoke_main.cpp $(PACKAGE_ROOT)/wolvi_repcut_verilator_sim.h $(UNIT_ARCHIVES) | $(BUILD_DIR)\n";
            makefile << "\t$(CXX) $(COMMON_CPPFLAGS) $(CXXFLAGS) -c -o $@ $<\n\n";
            makefile << "$(TARGET): $(VERILATED_OBJ) $(VERILATED_DPI_OBJ) $(VERILATED_THREADS_OBJ) $(WRAPPER_OBJS) $(SMOKE_MAIN_OBJ) $(UNIT_ARCHIVES)\n";
            makefile << "\t$(CXX) $(LDFLAGS) -o $@ $(VERILATED_OBJ) $(VERILATED_DPI_OBJ) $(VERILATED_THREADS_OBJ) $(WRAPPER_OBJS) $(SMOKE_MAIN_OBJ) $(UNIT_ARCHIVES) $(LDLIBS) -ldl -pthread\n\n";
            makefile << "run: $(TARGET)\n";
            makefile << "\t$(TARGET)\n\n";
            makefile << "clean:\n";
            makefile << "\trm -rf $(BUILD_DIR)\n\n";
            makefile << ".PHONY: all verilate-units run clean\n";

            return BuildGlueCode{smokeMain.str(), unitsMk.str(), makefile.str()};
        }

    } // namespace

    EmitResult EmitVerilatorRepCutPackage::emitImpl(const wolvrix::lib::grh::Design &design,
                                                    std::span<const wolvrix::lib::grh::Graph *const> topGraphs,
                                                    const EmitOptions &options)
    {
        EmitResult result;
        if (topGraphs.size() != 1 || topGraphs.front() == nullptr)
        {
            reportError("emitVerilatorRepCutPackage expects exactly one top graph");
            result.success = false;
            return result;
        }

        const wolvrix::lib::grh::Graph &topGraph = *topGraphs.front();
        const std::filesystem::path packageDir = resolveOutputDir(options);
        const std::filesystem::path svDir = packageDir / "sv";

        EmitSystemVerilog svEmitter(diagnostics());
        EmitOptions svOptions = options;
        svOptions.outputDir = svDir.string();
        svOptions.outputFilename = std::nullopt;
        svOptions.splitModules = true;
        svOptions.topOverrides.clear();
        svOptions.topOverrides.push_back(topGraph.symbol());

        const EmitResult svResult = svEmitter.emit(design, svOptions);
        if (!svResult.success)
        {
            result.success = false;
            return result;
        }
        result.artifacts = svResult.artifacts;

        const std::vector<const wolvrix::lib::grh::Graph *> emittedGraphs =
            reachableGraphsFromTops(design, topGraphs);
        if (emittedGraphs.size() != svResult.artifacts.size())
        {
            reportError("SV split emit artifacts do not match reachable graph count", topGraph.symbol());
            result.success = false;
            return result;
        }

        std::unordered_map<std::string, std::string> emittedModuleNameByGraph;
        emittedModuleNameByGraph.reserve(emittedGraphs.size());
        for (std::size_t i = 0; i < emittedGraphs.size(); ++i)
        {
            const auto *graph = emittedGraphs[i];
            if (!graph)
            {
                continue;
            }
            const std::string moduleName = std::filesystem::path(svResult.artifacts[i]).stem().string();
            emittedModuleNameByGraph.emplace(graph->symbol(), moduleName);
        }

        PackageManifest manifest;
        auto topModuleIt = emittedModuleNameByGraph.find(topGraph.symbol());
        manifest.topModule = topModuleIt != emittedModuleNameByGraph.end() ? topModuleIt->second : topGraph.symbol();

        manifest.topInputs.reserve(topGraph.inputPorts().size());
        for (const auto &port : topGraph.inputPorts())
        {
            manifest.topInputs.push_back(ManifestPort{
                port.name,
                "input",
                topGraph.valueWidth(port.value),
                topGraph.valueSigned(port.value),
            });
        }
        manifest.topOutputs.reserve(topGraph.outputPorts().size());
        for (const auto &port : topGraph.outputPorts())
        {
            manifest.topOutputs.push_back(ManifestPort{
                port.name,
                "output",
                topGraph.valueWidth(port.value),
                topGraph.valueSigned(port.value),
            });
        }

        std::unordered_map<wolvrix::lib::grh::ValueId, DriverDesc, wolvrix::lib::grh::ValueIdHash> drivers;
        std::unordered_map<EdgeKey, std::size_t, EdgeKeyHash> edgeIndex;
        drivers.reserve(topGraph.values().size());

        for (const auto &port : topGraph.inputPorts())
        {
            drivers.emplace(port.value, DriverDesc{DriverDesc::Kind::Top, {}, port.name, {}});
        }
        for (const auto opId : topGraph.operations())
        {
            if (topGraph.opKind(opId) != wolvrix::lib::grh::OperationKind::kConstant)
            {
                continue;
            }
            const auto results = topGraph.opResults(opId);
            if (results.empty())
            {
                continue;
            }
            const auto constValue = getAttribute<std::string>(topGraph.getOperation(opId), "constValue");
            if (!constValue)
            {
                reportError("Constant driver is missing constValue attribute", opSymbolRequired(topGraph, opId));
                result.success = false;
                return result;
            }
            drivers.emplace(results.front(), DriverDesc{DriverDesc::Kind::Const, {}, {}, *constValue});
        }

        auto appendSink = [&](std::string kind,
                              wolvrix::lib::grh::ValueId signalValue,
                              const DriverDesc &driver,
                              SinkDesc sink)
        {
            const EdgeKey key{std::move(kind), signalValue};
            auto it = edgeIndex.find(key);
            if (it == edgeIndex.end())
            {
                ManifestEdge edge;
                edge.signal = valueSymbolRequired(topGraph, signalValue);
                edge.width = topGraph.valueWidth(signalValue);
                edge.isSigned = topGraph.valueSigned(signalValue);
                edge.kind = key.kind;
                edge.driver = driver;
                edge.sinks.push_back(std::move(sink));
                edgeIndex.emplace(key, manifest.connections.size());
                manifest.connections.push_back(std::move(edge));
                return;
            }
            manifest.connections[it->second].sinks.push_back(std::move(sink));
        };

        std::unordered_set<std::string> seenInstances;
        seenInstances.reserve(topGraph.operations().size());
        std::unordered_set<std::string> usedWrapperModuleNames;
        std::unordered_map<std::string, UnitShimInfo> unitShimByInstance;
        unitShimByInstance.reserve(topGraph.operations().size());

        std::vector<PendingUnitInputs> pendingUnitInputs;
        std::vector<AliasAssign> aliasAssigns;
        aliasAssigns.reserve(topGraph.operations().size());

        for (const auto opId : topGraph.operations())
        {
            const auto kind = topGraph.opKind(opId);
            if (kind == wolvrix::lib::grh::OperationKind::kAssign)
            {
                const auto operands = topGraph.opOperands(opId);
                const auto results = topGraph.opResults(opId);
                if (operands.size() == 1 && results.size() == 1)
                {
                    aliasAssigns.push_back(AliasAssign{operands.front(), results.front()});
                }
                continue;
            }
            if (kind != wolvrix::lib::grh::OperationKind::kInstance &&
                kind != wolvrix::lib::grh::OperationKind::kBlackbox)
            {
                continue;
            }

            const auto op = topGraph.getOperation(opId);
            const auto moduleGraphName = getAttribute<std::string>(op, "moduleName");
            const auto inputNames = getAttribute<std::vector<std::string>>(op, "inputPortName");
            const auto outputNames = getAttribute<std::vector<std::string>>(op, "outputPortName");
            const auto inoutNames = getAttribute<std::vector<std::string>>(op, "inoutPortName").value_or(std::vector<std::string>{});
            if (!moduleGraphName || !inputNames || !outputNames)
            {
                reportError("Instance is missing moduleName or port-name attributes", opSymbolRequired(topGraph, opId));
                result.success = false;
                return result;
            }
            if (!inoutNames.empty())
            {
                reportError("emitVerilatorRepCutPackage does not support top-level unit inout ports yet",
                            opSymbolRequired(topGraph, opId));
                result.success = false;
                return result;
            }

            const auto *unitGraph = design.findGraph(*moduleGraphName);
            if (!unitGraph)
            {
                reportError("Instance module graph not found", *moduleGraphName);
                result.success = false;
                return result;
            }

            const std::string instanceName =
                getAttribute<std::string>(op, "instanceName").value_or(opSymbolRequired(topGraph, opId));
            if (!seenInstances.insert(instanceName).second)
            {
                reportError("Duplicate top-level unit instance name", instanceName);
                result.success = false;
                return result;
            }

            auto emittedNameIt = emittedModuleNameByGraph.find(unitGraph->symbol());
            const std::string emittedModuleName =
                emittedNameIt != emittedModuleNameByGraph.end() ? emittedNameIt->second : unitGraph->symbol();
            const std::string wrapperModuleName =
                makeUniqueIdentifier("WolviRepCutUnit_" + instanceName, usedWrapperModuleNames);
            const std::filesystem::path emittedSvPath = svDir / (emittedModuleName + ".sv");

            UnitShimInfo shim;
            try
            {
                shim = buildUnitShim(*unitGraph, emittedModuleName, emittedSvPath, wrapperModuleName);
            }
            catch (const std::exception &ex)
            {
                reportError(ex.what(), instanceName);
                result.success = false;
                return result;
            }

            const std::filesystem::path shimSvPath = packageDir / shim.wrapperSourceSv;
            auto shimSvStream = openOutputFile(shimSvPath);
            if (!shimSvStream)
            {
                result.success = false;
                return result;
            }
            *shimSvStream << shim.wrapperSvText;
            result.artifacts.push_back(shimSvPath.string());
            unitShimByInstance.emplace(instanceName, shim);
            const auto &storedShim = unitShimByInstance.at(instanceName);

            manifest.serialEvalOrder.push_back(instanceName);
            manifest.units.push_back(ManifestUnit{
                instanceName,
                unitGraph->symbol(),
                storedShim.wrapperModuleName,
                storedShim.wrapperSourceSv,
                storedShim.wrapperPorts,
            });

            const auto operands = topGraph.opOperands(opId);
            const auto results = topGraph.opResults(opId);
            if (operands.size() < inputNames->size() || results.size() < outputNames->size())
            {
                reportError("Instance port counts do not match operands/results", instanceName);
                result.success = false;
                return result;
            }

            for (std::size_t i = 0; i < outputNames->size(); ++i)
            {
                const auto mappedOutputIt = storedShim.outputPortByGraphName.find((*outputNames)[i]);
                if (mappedOutputIt == storedShim.outputPortByGraphName.end())
                {
                    reportError("Failed to map instance output port to generated shim port",
                                instanceName + "." + (*outputNames)[i]);
                    result.success = false;
                    return result;
                }
                drivers[results[i]] = DriverDesc{
                    DriverDesc::Kind::Unit,
                    instanceName,
                    mappedOutputIt->second,
                    {},
                };
            }

            std::vector<std::string> mappedInputNames;
            mappedInputNames.reserve(inputNames->size());
            for (const auto &inputName : *inputNames)
            {
                const auto mappedInputIt = storedShim.inputPortByGraphName.find(inputName);
                if (mappedInputIt == storedShim.inputPortByGraphName.end())
                {
                    reportError("Failed to map instance input port to generated shim port",
                                instanceName + "." + inputName);
                    result.success = false;
                    return result;
                }
                mappedInputNames.push_back(mappedInputIt->second);
            }
            pendingUnitInputs.push_back(PendingUnitInputs{
                instanceName,
                std::move(mappedInputNames),
                std::vector<wolvrix::lib::grh::ValueId>(operands.begin(), operands.begin() + inputNames->size()),
            });
        }

        bool changed = true;
        while (changed)
        {
            changed = false;
            for (const auto &alias : aliasAssigns)
            {
                const auto srcIt = drivers.find(alias.src);
                if (srcIt == drivers.end())
                {
                    continue;
                }
                if (drivers.find(alias.dst) != drivers.end())
                {
                    continue;
                }
                drivers.emplace(alias.dst, srcIt->second);
                changed = true;
            }
        }

        for (const auto &pending : pendingUnitInputs)
        {
            for (std::size_t i = 0; i < pending.inputNames.size(); ++i)
            {
                const auto signalValue = pending.operands[i];
                auto driverIt = drivers.find(signalValue);
                if (driverIt == drivers.end())
                {
                    const auto constValue = findConstValue(topGraph, signalValue);
                    if (constValue)
                    {
                        driverIt = drivers.emplace(signalValue,
                                                   DriverDesc{DriverDesc::Kind::Const, {}, {}, *constValue})
                                       .first;
                    }
                }
                if (driverIt == drivers.end())
                {
                    reportError("Unsupported top wrapper value used as unit input driver",
                                valueSymbolRequired(topGraph, signalValue));
                    result.success = false;
                    return result;
                }

                const DriverDesc &driver = driverIt->second;
                std::string edgeKind;
                switch (driver.kind)
                {
                case DriverDesc::Kind::Top:
                    edgeKind = "top_to_unit";
                    break;
                case DriverDesc::Kind::Unit:
                    edgeKind = "unit_to_unit";
                    break;
                case DriverDesc::Kind::Const:
                    edgeKind = "const_to_unit";
                    break;
                }
                appendSink(std::move(edgeKind),
                           signalValue,
                           driver,
                           SinkDesc{SinkDesc::Kind::Unit, pending.instanceName, pending.inputNames[i]});
            }
        }

        for (const auto &port : topGraph.outputPorts())
        {
            auto driverIt = drivers.find(port.value);
            if (driverIt == drivers.end())
            {
                const auto constValue = findConstValue(topGraph, port.value);
                if (constValue)
                {
                    driverIt = drivers.emplace(port.value,
                                               DriverDesc{DriverDesc::Kind::Const, {}, {}, *constValue})
                                   .first;
                }
            }
            if (driverIt == drivers.end())
            {
                reportError("Unsupported top wrapper value used as top output driver",
                            valueSymbolRequired(topGraph, port.value));
                result.success = false;
                return result;
            }

            const DriverDesc &driver = driverIt->second;
            if (driver.kind != DriverDesc::Kind::Unit)
            {
                reportError("Top outputs must be driven by unit outputs in the initial serial-semantics backend",
                            port.name);
                result.success = false;
                return result;
            }
            appendSink("unit_to_top", port.value, driver, SinkDesc{SinkDesc::Kind::Top, {}, port.name});
        }

        for (const auto &unit : manifest.units)
        {
            const auto *unitGraph = design.findGraph(unit.moduleGraphName);
            if (!unitGraph)
            {
                reportError("Unit graph missing while generating Verilator file list", unit.moduleGraphName);
                result.success = false;
                return result;
            }

            const std::array<const wolvrix::lib::grh::Graph *, 1> unitTopGraphs = {unitGraph};
            const auto reachableForUnit = reachableGraphsFromTops(design, unitTopGraphs);
            const std::filesystem::path fileListPath = packageDir / "verilate" / (unit.instanceName + ".f");
            auto fileListStream = openOutputFile(fileListPath);
            if (!fileListStream)
            {
                result.success = false;
                return result;
            }

            *fileListStream << (packageDir / unit.sourceSv).generic_string() << '\n';
            for (const auto *reachableGraph : reachableForUnit)
            {
                if (!reachableGraph)
                {
                    continue;
                }
                auto emittedNameIt = emittedModuleNameByGraph.find(reachableGraph->symbol());
                if (emittedNameIt == emittedModuleNameByGraph.end())
                {
                    continue;
                }
                *fileListStream << (packageDir / "sv" / (emittedNameIt->second + ".sv")).generic_string() << '\n';
            }
            result.artifacts.push_back(fileListPath.string());
        }

        const WrapperCode wrapperCode = generatePartitionedWrapperCode(manifest);
        std::error_code cleanupEc;
        std::filesystem::remove(packageDir / "partitioned_wrapper.h", cleanupEc);
        cleanupEc.clear();
        std::filesystem::remove(packageDir / "partitioned_wrapper.cpp", cleanupEc);
        cleanupEc.clear();
        std::filesystem::remove(packageDir / "wolvi_repcut_verilator_sim.cpp", cleanupEc);
        cleanupEc.clear();
        if (std::filesystem::exists(packageDir, cleanupEc))
        {
            for (const auto &entry : std::filesystem::directory_iterator(packageDir, cleanupEc))
            {
                if (cleanupEc)
                {
                    break;
                }
                if (!entry.is_regular_file())
                {
                    continue;
                }
                const std::string filename = entry.path().filename().string();
                if (filename.rfind("wolvi_repcut_verilator_sim_", 0) != 0 ||
                    entry.path().extension() != ".cpp")
                {
                    continue;
                }
                std::filesystem::remove(entry.path(), cleanupEc);
                cleanupEc.clear();
            }
        }
        const std::filesystem::path wrapperHeaderPath = packageDir / "wolvi_repcut_verilator_sim.h";
        auto wrapperHeaderStream = openOutputFile(wrapperHeaderPath);
        if (!wrapperHeaderStream)
        {
            result.success = false;
            return result;
        }
        *wrapperHeaderStream << wrapperCode.header;
        result.artifacts.push_back(wrapperHeaderPath.string());

        std::vector<std::string> wrapperSourceNames;
        wrapperSourceNames.reserve(wrapperCode.sources.size());
        for (const auto &sourceFile : wrapperCode.sources)
        {
            const std::filesystem::path wrapperSourcePath = packageDir / sourceFile.filename;
            auto wrapperSourceStream = openOutputFile(wrapperSourcePath);
            if (!wrapperSourceStream)
            {
                result.success = false;
                return result;
            }
            *wrapperSourceStream << sourceFile.contents;
            result.artifacts.push_back(wrapperSourcePath.string());
            wrapperSourceNames.push_back(sourceFile.filename);
        }

        const BuildGlueCode buildGlue = generateBuildGlueCode(manifest, wrapperSourceNames);

        const std::filesystem::path smokeMainPath = packageDir / "partitioned_smoke_main.cpp";
        auto smokeMainStream = openOutputFile(smokeMainPath);
        if (!smokeMainStream)
        {
            result.success = false;
            return result;
        }
        *smokeMainStream << buildGlue.smokeMain;
        result.artifacts.push_back(smokeMainPath.string());

        const std::filesystem::path unitsMkPath = packageDir / "units.mk";
        auto unitsMkStream = openOutputFile(unitsMkPath);
        if (!unitsMkStream)
        {
            result.success = false;
            return result;
        }
        *unitsMkStream << buildGlue.unitsMk;
        result.artifacts.push_back(unitsMkPath.string());

        const std::filesystem::path makefilePath = packageDir / "Makefile";
        auto makefileStream = openOutputFile(makefilePath);
        if (!makefileStream)
        {
            result.success = false;
            return result;
        }
        *makefileStream << buildGlue.makefile;
        result.artifacts.push_back(makefilePath.string());

        return result;
    }

} // namespace wolvrix::lib::emit
