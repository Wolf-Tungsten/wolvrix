#include "function-writer.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace wolvrix::lib::emit::grhsim_detail {

Code Code::line(std::string text, bool terminal) {
    if (text.find('\n') != std::string::npos) throw std::invalid_argument("C++ statement contains a newline");
    return {std::move(text), {}, false, terminal, {}, {}};
}
Code Code::block(std::string header, std::vector<Code> body) {
    if (header.find('\n') != std::string::npos) throw std::invalid_argument("C++ scope contains a newline");
    return {std::move(header), std::move(body), true, false, {}, {}};
}
Code Code::commented(Code code, std::string comment) {
    code.comment = std::move(comment);
    return code;
}
std::size_t Code::lines() const {
    std::size_t result = scope ? 2 : 1;
    for (const auto& child : body) result += child.lines();
    return result;
}

namespace {
using LocalIndex = std::map<std::string, std::size_t, std::less<>>;
using Replacements = std::map<std::string, std::string, std::less<>>;

// Comments may contain original HDL identifiers and source locations. Keep
// them on one physical line and outside placeholder substitution/preprocessing.
std::string sanitizeComment(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto ch = static_cast<unsigned char>(text[i]);
        if (ch < 32 || ch == 127) result += ' ';
        else if (ch == '@') result += "(at)";
        else if (ch == '\\') result += "\\x5c";
        else if (i + 1 < text.size() &&
                 ((ch == '*' && text[i + 1] == '/') || (ch == '/' && text[i + 1] == '*'))) {
            result += static_cast<char>(ch);
            result += ' ';
        } else result += static_cast<char>(ch);
    }
    const auto first = result.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    const auto last = result.find_last_not_of(' ');
    return result.substr(first, last - first + 1);
}

std::string inlineComment(std::string_view text) {
    const auto comment = sanitizeComment(text);
    return comment.empty() ? std::string{} : " /* " + comment + " */";
}

template<typename Match>
void placeholders(std::string_view text, Match&& match) {
    std::size_t begin = 0;
    while ((begin = text.find('@', begin)) != std::string_view::npos) {
        const auto end = text.find('@', begin + 1);
        if (end == std::string_view::npos) break;
        // Unknown delimiters can overlap the next real placeholder.
        if (match(text.substr(begin + 1, end - begin - 1), begin, end + 1)) begin = end + 1;
        else ++begin;
    }
}

std::string replace(std::string_view text, const Replacements& names) {
    std::string result;
    result.reserve(text.size());
    std::size_t offset = 0;
    placeholders(text, [&](std::string_view name, std::size_t begin, std::size_t end) {
        const auto found = names.find(name);
        if (found == names.end()) return false;
        result.append(text.substr(offset, begin - offset));
        result += found->second;
        offset = end;
        return true;
    });
    result.append(text.substr(offset));
    return result;
}
void render(const std::vector<Code>& body, std::string& output, std::size_t indent,
            const Replacements& names, bool helper, bool returnsValue,
            bool returnControl) {
    for (const auto& statement : body) {
        const auto padding = std::string(indent, ' ');
        if (statement.text == "@__chunk_return_check@") {
            output += padding + "if (chunkFrame.chunk_returned) return" +
                std::string(!helper && returnsValue ? " chunkFrame.chunk_result" : "") +
                ";" + inlineComment(statement.comment) + "\n";
            continue;
        }
        if (helper && returnControl && statement.terminal) {
            if (!statement.text.starts_with("return") || !statement.text.ends_with(';'))
                throw std::invalid_argument("Only structured return statements can cross a chunk boundary");
            if (returnsValue) output += padding + "chunkFrame.chunk_result = " +
                replace(statement.text.substr(6), names) + inlineComment(statement.comment) + "\n";
            output += padding + "chunkFrame.chunk_returned = true;" +
                (returnsValue ? std::string{} : inlineComment(statement.comment)) + "\n" + padding + "return;\n";
            continue;
        }
        output += padding + replace(statement.text, names) + (statement.scope ? " {" : "") +
            inlineComment(statement.comment) + "\n";
        if (statement.scope) {
            render(statement.body, output, indent + 4, names, helper, returnsValue, returnControl);
            output += std::string(indent, ' ') + "}\n";
        }
    }
}
void references(const Code& statement, const LocalIndex& locals,
                std::set<std::string>& result) {
    placeholders(statement.text, [&](std::string_view name, std::size_t, std::size_t) {
        const auto found = locals.find(name);
        if (found == locals.end()) return false;
        result.insert(found->first);
        return true;
    });
    for (const auto& child : statement.body) references(child, locals, result);
}
void references(const std::vector<Code>& body, const LocalIndex& locals,
                std::set<std::string>& result) {
    for (const auto& statement : body) references(statement, locals, result);
}
bool hasTerminal(const Code& code) {
    return code.terminal || std::any_of(code.body.begin(), code.body.end(), hasTerminal);
}
void operationReferences(const std::vector<Code>& body, std::set<std::string>& result) {
    for (const auto& statement : body) {
        if (!statement.sourceOperation.empty()) result.insert(statement.sourceOperation);
        operationReferences(statement.body, result);
    }
}
void controlReferences(const std::vector<Code>& body, bool helper, bool returnsValue,
                       bool returnControl, std::set<std::string>& result) {
    for (const auto& statement : body) {
        // These references are emitted directly by render rather than through
        // the ordinary local-placeholder path. Match only its actual branches.
        if (statement.text == "@__chunk_return_check@") {
            result.insert("chunk_returned");
            if (!helper && returnsValue) result.insert("chunk_result");
        } else if (helper && returnControl && statement.terminal) {
            result.insert("chunk_returned");
            if (returnsValue) result.insert("chunk_result");
        } else if (statement.scope) {
            controlReferences(statement.body, helper, returnsValue, returnControl, result);
        }
    }
}
}

WrittenFunctions writeFunction(Function source, std::string_view className, std::size_t maxLines) {
    if (maxLines < 8) throw std::invalid_argument("max-function-lines must be at least 8 (function/frame/call scaffolding)");
    const bool returnsValue = source.result != "void" && !source.result.empty();
    bool returnControl = false;
    struct Part {
        std::string name;
        std::vector<Code> body;
        bool repeated = false;
        std::set<std::string> sourceOperations;
    };
    std::vector<Part> parts;
    parts.push_back({source.name, std::move(source.body)});
    const std::string frameType = "chunkFrame_" + source.name;
    std::vector<std::set<std::string>> refs;
    std::set<std::string> shared, forcedFrame;
    LocalIndex localIndex;
    std::vector<std::size_t> sharedParameters;
    const auto analyze = [&] {
        refs.assign(parts.size(), {}); shared = forcedFrame;
        localIndex.clear(); sharedParameters.clear();
        for (std::size_t i = 0; i < source.locals.size(); ++i) localIndex.emplace(source.locals[i].name, i);
        std::map<std::string, std::size_t> counts;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            references(parts[i].body, localIndex, refs[i]);
            for (const auto& name : refs[i]) ++counts[name];
        }
        for (const auto& local : source.locals)
            if (counts[local.name] > 1 || (local.parameter && !refs[0].contains(local.name) && counts[local.name]))
                shared.insert(local.name);
        // A helper extracted from a loop can execute repeatedly. A local
        // initialized by the root must retain its value between these calls,
        // even when all textual references landed in just that helper.
        for (std::size_t i = 1; i < parts.size(); ++i) if (parts[i].repeated)
            shared.insert(refs[i].begin(), refs[i].end());
        if (returnControl) {
            shared.insert("chunk_returned");
            if (returnsValue) shared.insert("chunk_result");
        }
        for (std::size_t i = 0; i < source.locals.size(); ++i)
            if (source.locals[i].parameter && shared.contains(source.locals[i].name)) sharedParameters.push_back(i);
    };
    const auto statementLines = [&](const auto& self, const Code& statement, bool helper) -> std::size_t {
        std::size_t lines = statement.scope ? 2 : 1;
        if (helper && returnControl && statement.terminal && statement.text != "@__chunk_return_check@")
            lines += returnsValue ? 2 : 1;
        for (const auto& child : statement.body) lines += self(self, child, helper);
        return lines;
    };
    const auto cost = [&](std::size_t index) {
        std::size_t lines = 3;
        if (index == 0 && !shared.empty()) ++lines;
        if (index == 0) lines += sharedParameters.size();
        for (const auto& name : refs[index])
            if (!shared.contains(name) && !source.locals[localIndex.at(name)].parameter) ++lines;
        for (const auto& statement : parts[index].body) lines += statementLines(statementLines, statement, index != 0);
        return lines;
    };
    for (;;) {
        analyze();
        std::vector<std::size_t> oversized;
        for (std::size_t index = 0; index < parts.size(); ++index)
            if (cost(index) > maxLines) oversized.push_back(index);
        if (oversized.empty()) break;
        // Splitting only adds part boundaries. Existing shared locals stay
        // shared, so this pass can conservatively split all oversized parts
        // before recomputing references for the whole function.
        for (const auto index : oversized) {
            // Moving an indivisible statement into another helper does not reduce
            // its private declarations. Spill these locals once when declarations
            // alone prevent the body from fitting, rather than wrapping the same
            // scope in an unbounded chain of helpers.
            std::set<std::string> privateLocals;
            for (const auto& name : refs[index])
                if (!shared.contains(name) && !source.locals[localIndex.at(name)].parameter)
                    privateLocals.insert(name);
            const auto frameLine = index == 0 && shared.empty() ? 1u : 0u;
            if (!privateLocals.empty() && cost(index) - privateLocals.size() + frameLine <= maxLines) {
                forcedFrame.insert(privateLocals.begin(), privateLocals.end());
                continue;
            }
            auto& body = parts[index].body;
            // Extract a scope only when it cannot fit in one function. Splitting
            // every small guard first creates a helper per dispatch branch even
            // though ordinary statement batching would satisfy the same budget.
            std::vector<Part> extracted;
            for (auto& scope : body) {
                if (!scope.scope) continue;
                std::set<std::string> used;
                references(scope, localIndex, used);
                std::size_t lines = 3 + statementLines(statementLines, scope, index != 0);
                for (const auto& name : used)
                    if (!shared.contains(name) && !source.locals[localIndex.at(name)].parameter) ++lines;
                if (lines <= maxLines) continue;
                const std::string name = source.name + "_chunk" + std::to_string(parts.size() + extracted.size());
                const bool terminal = hasTerminal(scope);
                if (terminal && !returnControl) {
                    if (std::any_of(source.locals.begin(), source.locals.end(), [](const Local& local) {
                        return local.name == "chunk_returned" || local.name == "chunk_result";
                    })) throw std::invalid_argument("Reserved chunk control local in " + source.name);
                    source.locals.push_back({"bool", "chunk_returned"});
                    if (returnsValue) source.locals.push_back({source.result, "chunk_result"});
                    returnControl = true;
                }
                const bool repeated = parts[index].repeated || scope.text.starts_with("for (") || scope.text.starts_with("while (");
                std::vector<Code> inner = std::move(scope.body);
                auto sourceOperations = parts[index].sourceOperations;
                if (!scope.sourceOperation.empty()) {
                    sourceOperations.insert(scope.sourceOperation);
                    // Preserve a tagged scope in both the controlling function
                    // and its extracted implementation without replacing a
                    // distinct tag already carried by the first inner statement.
                    if (!inner.empty() && inner.front().sourceOperation.empty())
                        inner.front().sourceOperation = scope.sourceOperation;
                }
                scope.body = {Code::line(name + "(chunkFrame);")};
                if (terminal) scope.body.push_back(Code::line("@__chunk_return_check@", true));
                extracted.push_back({name, std::move(inner), repeated, std::move(sourceOperations)});
            }
            if (!extracted.empty()) {
                for (auto& part : extracted) parts.push_back(std::move(part));
                continue;
            }
            // Reserve scaffolding and declarations; small batches also split the
            // dispatcher tree on subsequent iterations, including the public root.
            const std::size_t target = std::max<std::size_t>(1, (maxLines - 4) / 2);
            std::vector<Code> replacement;
            std::vector<Part> additions;
            for (std::size_t begin = 0; begin < body.size();) {
                if (hasTerminal(body[begin])) { replacement.push_back(std::move(body[begin++])); continue; }
                std::size_t end = begin, size = 0;
                while (end < body.size() && !hasTerminal(body[end]) &&
                       (end == begin || size + body[end].lines() <= target)) size += body[end++].lines();
                if (end - begin == 1 && body[begin].lines() + 3 > maxLines) {
                    replacement.push_back(std::move(body[begin++])); continue;
                }
                const std::string name = source.name + "_chunk" + std::to_string(parts.size() + additions.size());
                std::vector<Code> inner;
                for (; begin < end; ++begin) inner.push_back(std::move(body[begin]));
                additions.push_back({name, std::move(inner), parts[index].repeated, parts[index].sourceOperations});
                replacement.push_back(Code::line(name + "(chunkFrame);"));
            }
            std::size_t previousLines = 0, replacementLines = 0;
            for (const auto& part : additions) for (const auto& statement : part.body) previousLines += statement.lines();
            // Unmoved terminal/oversized statements have equal cost on both sides.
            for (const auto& statement : replacement) {
                replacementLines += statement.lines();
                if (hasTerminal(statement) || statement.scope) previousLines += statement.lines();
            }
            if (additions.empty() || replacementLines >= previousLines) {
                throw std::invalid_argument("max-function-lines=" + std::to_string(maxLines) +
                    " cannot preserve the control flow/locals of " + source.name + "; increase the budget");
            }
            body = std::move(replacement);
            for (auto& part : additions) parts.push_back(std::move(part));
        }
    }
    WrittenFunctions result;
    // A source function may need helpers solely because its dispatcher/body
    // exceeds the line budget.  Those helpers do not need an argument when
    // no local actually crosses the split.  Creating an empty frame in that
    // case is both misleading and needlessly turns a pure call split into a
    // state object.  A frame is present only when liveness (or chunk return
    // control) found state that must survive a helper call.
    const bool hasFrame = !shared.empty();
    ValueStructLayout layout;
    if (hasFrame) {
        std::vector<Local> fields;
        for (const auto& local : source.locals) if (shared.contains(local.name))
            fields.push_back(local);
        layout = source.readableFrame ? writeDirectValueLayout(frameType, fields) : writeValueLayout(frameType, fields);
        result.frameDeclaration = std::move(layout.declaration);
        // Only physical references are needed after the declaration is written.
        std::vector<Local>().swap(layout.physicalFields);
    }
    if (!hasFrame) {
        // Split points are discovered before the final liveness fixpoint, so
        // helper calls were conservatively emitted with a frame argument.
        // Once analysis proves that no value crosses a boundary, remove that
        // argument and keep the helpers as ordinary no-argument functions.
        std::set<std::string> helperCalls;
        for (std::size_t i = 1; i < parts.size(); ++i)
            helperCalls.insert(parts[i].name + "(chunkFrame);");
        const auto stripFrameArgument = [&](auto&& self, std::vector<Code>& body) -> void {
            for (auto& statement : body) {
                if (helperCalls.contains(statement.text)) {
                    statement.text.erase(statement.text.size() - std::string_view("(chunkFrame);").size(),
                                         std::string_view("(chunkFrame);").size());
                    statement.text += "();";
                }
                self(self, statement.body);
            }
        };
        for (auto& part : parts) stripFrameArgument(stripFrameArgument, part.body);
    }
    for (std::size_t i = 0; i < parts.size(); ++i) {
        auto& part = parts[i];
        const std::string parameters = i == 0 ? source.parameters : (hasFrame ? frameType + " &chunkFrame" : "");
        const std::string signature = (i == 0 ? source.result : "void") + " " + part.name + "(" + parameters + ")" + (source.isConst ? " const" : "");
        WrittenFunction function;
        function.name = part.name;
        auto operations = std::move(part.sourceOperations);
        operationReferences(part.body, operations);
        function.operations.assign(operations.begin(), operations.end());
        function.declaration = "    " + signature + ";\n";
        function.definition = (i == 0 ? source.result : "void") + " " + std::string(className) + "::" +
            part.name + "(" + parameters + ")" + (source.isConst ? " const" : "") + "\n{\n";
        if (i == 0 && hasFrame) function.definition += "    " + frameType + " chunkFrame" +
            (source.zeroInitializeLocals || returnControl ? "{}" : "") + ";\n";
        Replacements names;
        for (const auto index : sharedParameters) if (i == 0) {
            const auto& local = source.locals[index];
            const auto reference = "chunkFrame." + layout.references.at(local.name);
            // A helper-only parameter still occurs in this root's frame setup.
            // Include that emitted reference even without a root body use.
            names[local.name] = reference;
            function.definition += "    " + reference + " = " + local.name + ";\n";
        }
        std::vector<std::size_t> usedLocals;
        for (const auto& name : refs[i]) usedLocals.push_back(localIndex.at(name));
        std::sort(usedLocals.begin(), usedLocals.end());
        for (const auto index : usedLocals) {
            const auto& local = source.locals[index];
            names[local.name] = shared.contains(local.name) ? "chunkFrame." + layout.references.at(local.name) : local.name;
            if (!shared.contains(local.name) && !local.parameter)
                function.definition += "    " + local.type + " " + local.name +
                    (!source.zeroInitializeLocals && local.initializer == "{}" ? "" : local.initializer) +
                    ";" + inlineComment(local.comment) + "\n";
        }
        std::set<std::string> controls;
        controlReferences(part.body, i != 0, returnsValue, returnControl, controls);
        for (const auto& name : controls) names[name] = "chunkFrame." + layout.references.at(name);
        for (const auto& [name, reference] : names)
            function.localTypes.emplace(name, source.locals[localIndex.at(name)].type);
        render(part.body, function.definition, 4, names, i != 0, returnsValue, returnControl);
        function.localReferences = std::move(names);
        // Rendering has consumed this part's structured text. Release it before
        // retaining the rendered definition and proceeding to the next chunk.
        std::vector<Code>().swap(part.body);
        part.sourceOperations.clear();
        refs[i].clear();
        function.definition += "}\n";
        function.lines = std::count(function.definition.begin(), function.definition.end(), '\n');
        if (function.lines > maxLines) throw std::logic_error("Generated function line accounting failed: " + part.name);
        if (!source.comment.empty()) {
            const auto comment = sanitizeComment((i == 0 ? std::string{} : "Extracted chunk of " + source.name + ". ") +
                                                 source.comment);
            if (!comment.empty()) {
                function.declaration.insert(0, "    // " + comment + "\n");
                function.definition.insert(0, "// " + comment + "\n");
            }
        }
        result.functions.push_back(std::move(function));
    }
    return result;
}

std::string writeValueStruct(std::string_view name, const std::vector<Local>& fields) {
    std::string result = "    struct " + std::string(name) + " {\n";
    // Keep {} as value-initialization instead of expanding one aggregate
    // initializer per scalar field in every containing class/frame.
    result += "        " + std::string(name) + "() = default;\n";
    for (const auto& field : fields)
        result += "        " + field.type + " " + field.name +
            (field.initializer == "{}" ? "" : field.initializer) +
            ";" + inlineComment(field.comment) + "\n";
    return result + "    };\n";
}

ValueStructLayout writeValueLayout(std::string_view name, const std::vector<Local>& fields) {
    ValueStructLayout result;
    if (fields.size() <= 1024) {
        return writeDirectValueLayout(name, fields);
    }
    struct Group { std::string type, name; std::size_t count = 0; };
    std::map<std::string, std::size_t> indices;
    std::vector<Group> groups;
    std::set<std::string> names;
    for (const auto& field : fields) names.insert(field.name);
    std::vector<Local> declarations;
    for (const auto& field : fields) {
        if (field.initializer != "{}" || field.name == "chunk_returned" || field.name == "chunk_result") {
            declarations.push_back(field); result.references.emplace(field.name, field.name); continue;
        }
        const auto [it, fresh] = indices.emplace(field.type, groups.size());
        if (fresh) {
            auto member = "cells_" + std::to_string(groups.size());
            while (names.contains(member)) member += '_';
            names.insert(member); groups.push_back({field.type, std::move(member)});
        }
        auto& group = groups[it->second];
        result.references.emplace(field.name, group.name + '[' + std::to_string(group.count++) + ']');
    }
    for (const auto& group : groups)
        declarations.push_back({"std::array<" + group.type + "," + std::to_string(group.count) + ">", group.name});
    result.declaration = writeValueStruct(name, declarations);
    result.physicalFields = std::move(declarations);
    return result;
}

ValueStructLayout writeDirectValueLayout(std::string_view name, const std::vector<Local>& fields) {
    ValueStructLayout result;
    result.declaration = writeValueStruct(name, fields);
    result.physicalFields = fields;
    for (const auto& field : fields) result.references.emplace(field.name, field.name);
    return result;
}

}
