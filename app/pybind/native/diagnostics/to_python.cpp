#include "native/diagnostics/to_python.hpp"

#include "slang/text/SourceManager.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>

namespace wolvrix::app::pybind
{

    namespace
    {

        const char *diagnosticKindText(wolvrix::lib::diag::DiagnosticKind kind)
        {
            switch (kind)
            {
            case wolvrix::lib::diag::DiagnosticKind::Warning:
                return "warning";
            case wolvrix::lib::diag::DiagnosticKind::Info:
                return "info";
            case wolvrix::lib::diag::DiagnosticKind::Debug:
                return "debug";
            case wolvrix::lib::diag::DiagnosticKind::Error:
            default:
                return "error";
            }
        }

        const char *diagnosticKindName(wolvrix::lib::diag::DiagnosticKind kind)
        {
            switch (kind)
            {
            case wolvrix::lib::diag::DiagnosticKind::Debug:
                return "debug";
            case wolvrix::lib::diag::DiagnosticKind::Info:
                return "info";
            case wolvrix::lib::diag::DiagnosticKind::Warning:
                return "warning";
            case wolvrix::lib::diag::DiagnosticKind::Error:
            default:
                return "error";
            }
        }

        struct DiagnosticLocationInfo
        {
            slang::SourceLocation location;
            std::string filename;
            std::size_t line = 0;
            std::size_t column = 0;
        };

        bool getDiagnosticLocationInfo(const slang::SourceManager *sourceManager,
                                       slang::SourceLocation location,
                                       DiagnosticLocationInfo &info)
        {
            if (!sourceManager || !location.valid())
            {
                return false;
            }
            auto resolved = sourceManager->getFullyOriginalLoc(location);
            if (!resolved.valid())
            {
                resolved = location;
            }
            if (!sourceManager->isFileLoc(resolved))
            {
                auto expanded = sourceManager->getFullyExpandedLoc(location);
                if (expanded.valid() && sourceManager->isFileLoc(expanded))
                {
                    resolved = expanded;
                }
                else if (!sourceManager->isFileLoc(resolved))
                {
                    return false;
                }
            }

            std::string fileName;
            const auto nameView = sourceManager->getFileName(resolved);
            if (!nameView.empty())
            {
                fileName.assign(nameView);
            }
            if (fileName.empty())
            {
                const auto &fullPath = sourceManager->getFullPath(resolved.buffer());
                if (!fullPath.empty())
                {
                    fileName = fullPath.string();
                }
            }
            if (fileName.empty())
            {
                const auto rawName = sourceManager->getRawFileName(resolved.buffer());
                if (!rawName.empty())
                {
                    fileName.assign(rawName);
                }
            }
            if (fileName.empty())
            {
                return false;
            }
            info.location = resolved;
            info.filename = std::move(fileName);
            info.line = sourceManager->getLineNumber(resolved);
            info.column = sourceManager->getColumnNumber(resolved);
            return true;
        }

        std::string formatSourceLineSnippet(const slang::SourceManager *sourceManager,
                                            const DiagnosticLocationInfo &info,
                                            const wolvrix::lib::ingest::SourcePosition *position = nullptr)
        {
            std::string line;
            if (position)
            {
                line = position->sourceLine;
            }
            else
            {
                if (!sourceManager || !info.location.valid())
                {
                    return {};
                }
                const auto sourceText = sourceManager->getSourceText(info.location.buffer());
                const std::size_t offset = info.location.offset();
                if (offset >= sourceText.size())
                {
                    return {};
                }
                const auto previous = sourceText.rfind('\n', offset);
                const auto lineStart = previous == std::string_view::npos ? 0 : previous + 1;
                const auto next = sourceText.find('\n', offset);
                const auto lineEnd = next == std::string_view::npos ? sourceText.size() : next;
                line.assign(sourceText.substr(lineStart, lineEnd - lineStart));
            }
            if (line.empty())
            {
                return {};
            }
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            for (char &ch : line)
            {
                if (ch == '\t')
                {
                    ch = ' ';
                }
            }

            std::size_t caretPos = info.column > 0 ? info.column - 1 : 0;
            if (caretPos > line.size())
            {
                caretPos = line.size();
            }

            const std::size_t maxLen = 200;
            std::size_t prefixLen = 0;
            std::size_t sliceStart = 0;
            std::size_t sliceEnd = line.size();
            if (line.size() > maxLen)
            {
                const std::size_t context = 80;
                if (caretPos > context)
                {
                    sliceStart = caretPos - context;
                }
                sliceEnd = std::min(sliceStart + maxLen, line.size());
                if (sliceStart > 0)
                {
                    prefixLen = 3;
                }
                if (sliceEnd < line.size())
                {
                    line = line.substr(sliceStart, sliceEnd - sliceStart);
                    line.append("...");
                }
                else
                {
                    line = line.substr(sliceStart);
                }
                if (sliceStart > 0)
                {
                    line.insert(0, "...");
                }
                caretPos = prefixLen + (caretPos - sliceStart);
            }

            std::string caretLine;
            caretLine.assign(caretPos, ' ');
            caretLine.push_back('^');

            std::string snippet;
            snippet.append("\n  | ");
            snippet.append(line);
            snippet.append("\n  | ");
            snippet.append(caretLine);
            return snippet;
        }

        std::string formatDiagnostic(const wolvrix::lib::diag::Diagnostic &message,
                                     const slang::SourceManager *sourceManager,
                                     const wolvrix::lib::ingest::SourcePosition *position = nullptr)
        {
            std::string out;
            out.append(diagnosticKindText(message.kind));
            DiagnosticLocationInfo info;
            bool hasLocation = false;
            if (position && !position->file.empty())
            {
                info.filename = position->file.string();
                info.line = position->line;
                info.column = position->column;
                hasLocation = true;
            }
            else
            {
                hasLocation = message.location && message.location->valid() &&
                              getDiagnosticLocationInfo(sourceManager, *message.location, info);
            }
            if (hasLocation)
            {
                out.append(" ");
                out.append(info.filename);
                out.append(":");
                out.append(std::to_string(info.line));
                out.append(":");
                out.append(std::to_string(info.column));
            }
            if (!message.passName.empty())
            {
                out.append(" [");
                out.append(message.passName);
                out.append("]");
            }
            out.append(" ");
            out.append(message.message);
            if (!message.context.empty())
            {
                out.append(" (");
                out.append(message.context);
                out.append(")");
            }
            if (!message.originSymbol.empty())
            {
                out.append(" <");
                out.append(message.originSymbol);
                out.append(">");
            }
            if (hasLocation)
            {
                out.append(formatSourceLineSnippet(sourceManager, info, position));
            }
            return out;
        }

        PyObject *diagnosticsToPyList(const std::vector<wolvrix::lib::diag::Diagnostic> &messages,
                                      const slang::SourceManager *sourceManager,
                                      const std::vector<wolvrix::lib::ingest::IngestDiagnostic> *ingest = nullptr)
        {
            PyObject *list = PyList_New(static_cast<Py_ssize_t>(messages.size()));
            if (!list)
            {
                return nullptr;
            }
            for (Py_ssize_t i = 0; i < static_cast<Py_ssize_t>(messages.size()); ++i)
            {
                const auto &message = messages[static_cast<std::size_t>(i)];
                const auto *position = ingest && (*ingest)[i].location
                                           ? &*(*ingest)[i].location : nullptr;
                PyObject *dict = PyDict_New();
                if (!dict)
                {
                    Py_DECREF(list);
                    return nullptr;
                }
                const char *kindText = diagnosticKindName(message.kind);
                auto *kind = PyUnicode_FromStringAndSize(kindText, static_cast<Py_ssize_t>(std::strlen(kindText)));
                auto *pass = PyUnicode_FromStringAndSize(message.passName.data(),
                                                         static_cast<Py_ssize_t>(message.passName.size()));
                auto *msg = PyUnicode_FromStringAndSize(message.message.data(),
                                                        static_cast<Py_ssize_t>(message.message.size()));
                auto *ctx = PyUnicode_FromStringAndSize(message.context.data(),
                                                        static_cast<Py_ssize_t>(message.context.size()));
                auto *origin = PyUnicode_FromStringAndSize(message.originSymbol.data(),
                                                           static_cast<Py_ssize_t>(message.originSymbol.size()));
                if (!kind || !pass || !msg || !ctx || !origin)
                {
                    Py_XDECREF(kind);
                    Py_XDECREF(pass);
                    Py_XDECREF(msg);
                    Py_XDECREF(ctx);
                    Py_XDECREF(origin);
                    Py_DECREF(dict);
                    Py_DECREF(list);
                    return nullptr;
                }
                PyDict_SetItemString(dict, "kind", kind);
                PyDict_SetItemString(dict, "pass", pass);
                PyDict_SetItemString(dict, "message", msg);
                PyDict_SetItemString(dict, "context", ctx);
                PyDict_SetItemString(dict, "origin", origin);
                Py_DECREF(kind);
                Py_DECREF(pass);
                Py_DECREF(msg);
                Py_DECREF(ctx);
                Py_DECREF(origin);

                if (position || (message.location && sourceManager))
                {
                    DiagnosticLocationInfo info;
                    bool hasLocation = false;
                    if (position && !position->file.empty())
                    {
                        info.filename = position->file.string();
                        info.line = position->line;
                        info.column = position->column;
                        hasLocation = true;
                    }
                    else if (message.location)
                    {
                        hasLocation = getDiagnosticLocationInfo(sourceManager, *message.location, info);
                    }
                    if (hasLocation)
                    {
                        PyObject *file = PyUnicode_FromString(info.filename.c_str());
                        PyObject *line = PyLong_FromUnsignedLongLong(info.line);
                        PyObject *column = PyLong_FromUnsignedLongLong(info.column);
                        if (file && line && column)
                        {
                            PyDict_SetItemString(dict, "file", file);
                            PyDict_SetItemString(dict, "line", line);
                            PyDict_SetItemString(dict, "column", column);
                        }
                        Py_XDECREF(file);
                        Py_XDECREF(line);
                        Py_XDECREF(column);
                    }
                }

                const std::string text = formatDiagnostic(message, sourceManager, position);
                PyObject *textObj = PyUnicode_FromStringAndSize(text.data(),
                                                                static_cast<Py_ssize_t>(text.size()));
                if (!textObj)
                {
                    Py_DECREF(dict);
                    Py_DECREF(list);
                    return nullptr;
                }
                PyDict_SetItemString(dict, "text", textObj);
                Py_DECREF(textObj);

                PyList_SET_ITEM(list, i, dict);
            }
            return list;
        }

    } // namespace

    PyObject *makeIngestActionResult(const wolvrix::lib::ingest::IngestResult &result)
    {
        std::vector<wolvrix::lib::diag::Diagnostic> messages;
        messages.reserve(result.diagnostics.size());
        for (const auto &diagnostic : result.diagnostics)
        {
            wolvrix::lib::diag::Diagnostic message;
            switch (diagnostic.kind)
            {
            case wolvrix::lib::ingest::IngestSeverity::Error:
                message.kind = wolvrix::lib::diag::DiagnosticKind::Error; break;
            case wolvrix::lib::ingest::IngestSeverity::Warning:
                message.kind = wolvrix::lib::diag::DiagnosticKind::Warning; break;
            case wolvrix::lib::ingest::IngestSeverity::Info:
                message.kind = wolvrix::lib::diag::DiagnosticKind::Info; break;
            case wolvrix::lib::ingest::IngestSeverity::Debug:
                message.kind = wolvrix::lib::diag::DiagnosticKind::Debug; break;
            }
            message.message = diagnostic.message;
            message.context = diagnostic.context;
            message.passName = diagnostic.passName;
            message.originSymbol = diagnostic.originSymbol;
            messages.push_back(std::move(message));
        }
        PyObject *diagnostics = diagnosticsToPyList(messages, nullptr, &result.diagnostics);
        if (!diagnostics)
        {
            return nullptr;
        }
        PyObject *action = PyTuple_New(2);
        if (!action)
        {
            Py_DECREF(diagnostics);
            return nullptr;
        }
        PyTuple_SET_ITEM(action, 0, PyBool_FromLong(result.success()));
        PyTuple_SET_ITEM(action, 1, diagnostics);
        return action;
    }

    PyObject *makeActionResult(bool success,
                               const std::vector<wolvrix::lib::diag::Diagnostic> &messages,
                               const slang::SourceManager *sourceManager)
    {
        PyObject *diagList = diagnosticsToPyList(messages, sourceManager);
        if (!diagList)
        {
            return nullptr;
        }
        PyObject *result = PyTuple_New(2);
        if (!result)
        {
            Py_DECREF(diagList);
            return nullptr;
        }
        PyTuple_SET_ITEM(result, 0, PyBool_FromLong(success ? 1 : 0));
        PyTuple_SET_ITEM(result, 1, diagList);
        return result;
    }

    PyObject *makePassActionResult(bool success,
                                   bool changed,
                                   const std::vector<wolvrix::lib::diag::Diagnostic> &messages,
                                   const slang::SourceManager *sourceManager)
    {
        PyObject *diagList = diagnosticsToPyList(messages, sourceManager);
        if (!diagList)
        {
            return nullptr;
        }
        PyObject *result = PyTuple_New(3);
        if (!result)
        {
            Py_DECREF(diagList);
            return nullptr;
        }
        PyTuple_SET_ITEM(result, 0, PyBool_FromLong(success ? 1 : 0));
        PyTuple_SET_ITEM(result, 1, PyBool_FromLong(changed ? 1 : 0));
        PyTuple_SET_ITEM(result, 2, diagList);
        return result;
    }

} // namespace wolvrix::app::pybind
