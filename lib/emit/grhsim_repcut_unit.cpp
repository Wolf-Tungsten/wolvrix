#include "grhsim_repcut_unit.hpp"

#include "emit/grhsim_cpp.hpp"
#include "transform/activity_schedule.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wolvrix::lib::emit::detail
{
    namespace
    {
        using wolvrix::lib::grh::ValueId;
        using wolvrix::lib::grh::ValueIdHash;

        bool isIdentifier(std::string_view text)
        {
            if (text.empty() || (!std::isalpha(static_cast<unsigned char>(text.front())) &&
                                 text.front() != '_'))
            {
                return false;
            }
            for (unsigned char ch : text)
            {
                if (!std::isalnum(ch) && ch != '_')
                {
                    return false;
                }
            }
            return true;
        }

        std::string portType(std::int64_t width)
        {
            if (width <= 1) return "bool";
            if (width <= 8) return "std::uint8_t";
            if (width <= 16) return "std::uint16_t";
            if (width <= 32) return "std::uint32_t";
            if (width <= 64) return "std::uint64_t";
            return "std::array<std::uint64_t, " + std::to_string((width + 63) / 64) + ">";
        }

        struct UnitCloneGuard
        {
            wolvrix::lib::grh::Design &design;
            std::string name;
            ~UnitCloneGuard() { design.deleteGraph(name); }
        };

        void forwardPassDiagnostics(const wolvrix::lib::transform::PassDiagnostics &source,
                                    EmitDiagnostics *destination)
        {
            if (!destination) return;
            for (const auto &diagnostic : source.messages())
            {
                const std::string text = "GrhSIM RepCut activity-schedule: " + diagnostic.message;
                switch (diagnostic.kind)
                {
                case EmitDiagnosticKind::Error: destination->error(text, diagnostic.context); break;
                case EmitDiagnosticKind::Warning: destination->warning(text, diagnostic.context); break;
                case EmitDiagnosticKind::Info: destination->info(text, diagnostic.context); break;
                case EmitDiagnosticKind::Debug: destination->debug(text, diagnostic.context); break;
                }
            }
        }
    }

    EmitResult emitGrhSimRepCutUnit(wolvrix::lib::grh::Design &clonedDesign,
                                    std::string_view graphName,
                                    std::string_view adapterName,
                                    std::span<const GrhSimRepCutPort> ports,
                                    const std::filesystem::path &packageDir,
                                    const EmitOptions &options,
                                    EmitDiagnostics *diagnostics)
    {
        EmitResult result;
        try
        {
            if (!isIdentifier(adapterName))
            {
                throw std::runtime_error("invalid GrhSIM RepCut adapter identifier");
            }
            const auto *source = clonedDesign.findGraph(graphName);
            if (!source) throw std::runtime_error("GrhSIM RepCut unit graph not found");
            if (!source->inoutPorts().empty())
            {
                throw std::runtime_error("GrhSIM RepCut units do not support inout ports");
            }
            if (ports.size() != source->inputPorts().size() + source->outputPorts().size())
            {
                throw std::runtime_error("GrhSIM RepCut adapter must map every graph port");
            }

            const std::string modelName = std::string(adapterName) + "_model";
            const auto dirtyTrackingIt = options.attributes.find("input_dirty_tracking");
            const bool inputDirtyTracking = dirtyTrackingIt != options.attributes.end() &&
                                            (dirtyTrackingIt->second == "1" ||
                                             dirtyTrackingIt->second == "true" ||
                                             dirtyTrackingIt->second == "on");
            std::vector<std::size_t> trackedPorts;
            if (inputDirtyTracking)
            {
                const auto names = options.attributes.find("tracked_input_ports");
                if (names == options.attributes.end()) throw std::runtime_error("missing tracked_input_ports");
                std::istringstream list(names->second);
                std::string member;
                while (std::getline(list, member, ','))
                {
                    const auto port = std::find_if(ports.begin(), ports.end(), [&](const auto &entry) {
                        return entry.input && entry.memberName == member;
                    });
                    if (port == ports.end()) throw std::runtime_error("invalid tracked input: " + member);
                    trackedPorts.push_back(static_cast<std::size_t>(port - ports.begin()));
                }
            }
            auto &graph = clonedDesign.cloneGraph(graphName, modelName);
            UnitCloneGuard guard{clonedDesign, modelName};
            std::vector<ValueId> values;
            std::vector<std::string> modelMembers;
            std::unordered_set<std::string> usedAdapterMembers;
            std::unordered_set<std::string> usedGraphPorts;
            values.reserve(ports.size());
            modelMembers.reserve(ports.size());
            for (const auto &port : ports)
            {
                const std::string direction = port.input ? "input:" : "output:";
                const std::string memberPrefix = port.input ? "in_" : "out_";
                if (!isIdentifier(port.memberName) || !port.memberName.starts_with(memberPrefix) ||
                    !usedAdapterMembers.insert(port.memberName).second ||
                    !usedGraphPorts.insert(direction + port.graphName).second)
                {
                    throw std::runtime_error("invalid or duplicate GrhSIM RepCut port mapping: " + port.graphName);
                }
                const ValueId value = port.input ? graph.inputPortValue(port.graphName)
                                                 : graph.outputPortValue(port.graphName);
                if (!value.valid() || graph.valueType(value) != wolvrix::lib::grh::ValueType::Logic ||
                    port.width <= 0 || graph.valueWidth(value) != port.width)
                {
                    throw std::runtime_error("GrhSIM RepCut port is missing or has incompatible shape: " + port.graphName);
                }
                values.push_back(value);
            }
            // Canonical names on the private clone avoid C++ keyword/sanitizer
            // collisions in arbitrary RTL port names. The source graph is intact.
            for (const auto &port : ports)
            {
                if (port.input) graph.removeInputPort(port.graphName);
                else graph.removeOutputPort(port.graphName);
            }
            std::unordered_map<ValueId, std::string, ValueIdHash> inputMemberByValue;
            for (std::size_t i = 0; i < ports.size(); ++i)
            {
                const auto &port = ports[i];
                std::string member = "repcut_port_" + std::to_string(i);
                if (port.input)
                {
                    // GrhSIM exposes one public input per ValueId. Preserve
                    // aliases in the adapter without duplicate model fields.
                    auto [entry, inserted] = inputMemberByValue.emplace(values[i], member);
                    member = entry->second;
                    if (inserted) graph.bindInputPort(member, values[i]);
                }
                else graph.bindOutputPort(member, values[i]);
                modelMembers.push_back(std::move(member));
            }

            wolvrix::lib::transform::SessionStore session;
            wolvrix::lib::transform::PassManager manager;
            manager.options().session = &session;
            wolvrix::lib::transform::ActivityScheduleOptions scheduleOptions;
            scheduleOptions.path = modelName;
            manager.addPass(std::make_unique<wolvrix::lib::transform::ActivitySchedulePass>(scheduleOptions));
            wolvrix::lib::transform::PassDiagnostics scheduleDiagnostics;
            const auto scheduled = manager.run(clonedDesign, scheduleDiagnostics);
            forwardPassDiagnostics(scheduleDiagnostics, diagnostics);
            if (!scheduled.success || scheduleDiagnostics.hasError())
            {
                result.success = false;
                return result;
            }

            const auto modelDir = packageDir / "grhsim" / adapterName;
            EmitOptions modelOptions = options;
            modelOptions.outputDir = modelDir.string();
            modelOptions.outputFilename.reset();
            modelOptions.topOverrides = {modelName};
            modelOptions.session = &session;
            modelOptions.sessionPathPrefix = modelName;
            modelOptions.attributes["waveform"] = "off";
            // Package emission keeps per-model counters disabled by default,
            // but a private diagnostic run may opt into the same eval trace
            // used by the direct GrhSIM emitter.  The environment escape hatch
            // is deliberately narrow so normal package behavior is unchanged.
            const char *diagnosticPerf = std::getenv("WOLVRIX_GRHSIM_REPCUT_PERF");
            modelOptions.attributes["perf"] =
                diagnosticPerf != nullptr && std::string_view(diagnosticPerf) == "eval" ? "eval" : "off";
            if (inputDirtyTracking)
            {
                std::string canonical;
                for (std::size_t index : trackedPorts)
                {
                    if (!canonical.empty()) canonical += ',';
                    canonical += modelMembers.at(index);
                }
                modelOptions.attributes["tracked_input_ports"] = canonical;
            }
            EmitGrhSimCpp emitter(diagnostics);
            result = emitter.emit(clonedDesign, modelOptions);
            if (!result.success) return result;

            // grhsim.mk tracks sources at package root. Names include the
            // unique adapter/model stem, so independent units cannot collide.
            // Copy only artifacts from this emit, never stale directory entries.
            for (auto &artifact : result.artifacts)
            {
                const std::filesystem::path path(artifact);
                if (path.extension() != ".cpp" && path.extension() != ".hpp") continue;
                const auto flatPath = packageDir / path.filename();
                std::filesystem::copy_file(path, flatPath,
                                           std::filesystem::copy_options::overwrite_existing);
                artifact = flatPath.string();
            }

            const auto headerPath = packageDir / (std::string(adapterName) + ".h");
            const auto sourcePath = packageDir / (std::string(adapterName) + ".cpp");
            const std::string modelClass = "GrhSIM_" + modelName;
            std::ofstream header(headerPath);
            std::ofstream implementation(sourcePath);
            if (!header || !implementation)
            {
                throw std::runtime_error("cannot open GrhSIM RepCut adapter outputs");
            }
            header << "#pragma once\n\n#include <array>\n#include <cstdint>\n#include <memory>\n\n";
            header << "class " << modelClass << ";\n\n";
            header << "class " << adapterName << " final {\n";
            header << "  std::unique_ptr<" << modelClass << "> model_;\npublic:\n";
            header << "  " << adapterName << "();\n  ~" << adapterName << "();\n";
            header << "  " << adapterName << "(const " << adapterName << " &) = delete;\n";
            header << "  " << adapterName << " &operator=(const " << adapterName << " &) = delete;\n";
            header << "  void init();\n  void eval();\n";
            if (inputDirtyTracking)
            {
                header << "  void eval_tracked();\n";
            }
            header << "  void set_runtime_profile_enabled(bool enabled);\n";
            header << "  void dump_runtime_profile() const;\n";
            for (const auto &port : ports)
            {
                header << "  " << portType(port.width) << " &" << port.memberName << ";\n";
            }
            if (inputDirtyTracking)
            {
                header << "  alignas(64) std::array<std::int8_t, " << trackedPorts.size() * 64u
                       << "> input_changes_{};\n";
            }
            header << "};\n";
            // Keep each model's runtime helper definitions in a separate TU.
            implementation << "#include \"" << adapterName << ".h\"\n";
            implementation << "#include \"grhsim_" << modelName << ".hpp\"\n\n";
            implementation << adapterName << "::" << adapterName << "()\n"
                           << "    : model_(std::make_unique<" << modelClass << ">())";
            for (std::size_t i = 0; i < ports.size(); ++i)
            {
                implementation << ",\n      " << ports[i].memberName << "(model_->" << modelMembers[i] << ")";
            }
            implementation << " {\n  model_->init();\n";
            implementation << "}\n\n";
            implementation << adapterName << "::~" << adapterName << "() = default;\n\n";
            implementation << "void " << adapterName << "::init() { model_->init();";
            if (inputDirtyTracking)
            {
                implementation << " input_changes_.fill(0);";
            }
            implementation << " }\n";
            implementation << "void " << adapterName << "::eval() { model_->eval(); }\n";
            if (inputDirtyTracking)
            {
                implementation << "void " << adapterName << "::eval_tracked() {\n"
                               << "  model_->eval_with_input_changes(input_changes_.data());\n"
                               << "  for (std::size_t i = 0; i < input_changes_.size(); i += 64u)\n"
                               << "    if (input_changes_[i]) input_changes_[i] = 0;\n"
                               << "}\n";
            }
            implementation << "void " << adapterName
                           << "::set_runtime_profile_enabled(bool enabled) { model_->set_runtime_profile_enabled(enabled); }\n";
            implementation << "void " << adapterName
                           << "::dump_runtime_profile() const { model_->dump_runtime_profile(); }\n";
            header.close();
            implementation.close();
            if (!header || !implementation)
            {
                throw std::runtime_error("failed writing GrhSIM RepCut adapter outputs");
            }
            result.artifacts.push_back(headerPath.string());
            result.artifacts.push_back(sourcePath.string());
        }
        catch (const std::exception &error)
        {
            result.success = false;
            if (diagnostics) diagnostics->error(error.what(), std::string(graphName));
        }
        return result;
    }
}
