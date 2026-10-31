#include "core/diagnostics.hpp"

#include <utility>

namespace wolvrix::lib::diag
{

    thread_local Diagnostics::ThreadLocalBuffer Diagnostics::threadLocal_{};

    namespace
    {
        bool isErrorKind(DiagnosticKind kind)
        {
            return kind == DiagnosticKind::Error;
        }
    } // namespace

    void Diagnostics::error(std::string message, std::string context)
    {
        add(DiagnosticKind::Error, std::move(message), std::move(context));
    }

    void Diagnostics::warning(std::string message, std::string context)
    {
        add(DiagnosticKind::Warning, std::move(message), std::move(context));
    }

    void Diagnostics::info(std::string message, std::string context)
    {
        add(DiagnosticKind::Info, std::move(message), std::move(context));
    }

    void Diagnostics::debug(std::string message, std::string context)
    {
        add(DiagnosticKind::Debug, std::move(message), std::move(context));
    }

    void Diagnostics::flushThreadLocal()
    {
        if (!threadLocalEnabled_)
        {
            return;
        }
        flushThreadLocalLocked(threadLocal_);
    }

    void Diagnostics::clear()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            messages_.clear();
        }
        hasError_.store(false, std::memory_order_relaxed);
        if (threadLocalEnabled_)
        {
            threadLocal_.messages.clear();
            threadLocal_.hasError = false;
        }
    }

    void Diagnostics::add(DiagnosticKind kind,
                          std::string message,
                          std::string context,
                          std::string passName,
                          std::string originSymbol,
                          std::optional<slang::SourceLocation> location,
                          std::optional<SourcePosition> sourcePosition)
    {
        const bool isError = isErrorKind(kind);
        Diagnostic diag{
            .kind = kind,
            .message = std::move(message),
            .context = std::move(context),
            .passName = std::move(passName),
            .originSymbol = std::move(originSymbol),
            .location = location,
            .sourcePosition = std::move(sourcePosition),
        };

        if (threadLocalEnabled_)
        {
            ThreadLocalBuffer &buffer = threadLocal_;
            buffer.messages.push_back(std::move(diag));
            if (isError)
            {
                buffer.hasError = true;
            }
        }
        else
        {
            std::lock_guard<std::mutex> lock(mutex_);
            messages_.push_back(std::move(diag));
        }

        if (isError)
        {
            hasError_.store(true, std::memory_order_relaxed);
            if (onError_)
            {
                onError_();
            }
        }
    }

    void Diagnostics::flushThreadLocalLocked(ThreadLocalBuffer &buffer)
    {
        if (buffer.messages.empty() && !buffer.hasError)
        {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!buffer.messages.empty())
        {
            messages_.insert(messages_.end(),
                             std::make_move_iterator(buffer.messages.begin()),
                             std::make_move_iterator(buffer.messages.end()));
            buffer.messages.clear();
        }
        if (buffer.hasError)
        {
            hasError_.store(true, std::memory_order_relaxed);
            buffer.hasError = false;
        }
    }

} // namespace wolvrix::lib::diag
