#ifndef WOLVRIX_STORE_HPP
#define WOLVRIX_STORE_HPP

#include "core/diagnostics.hpp"
#include "core/grh.hpp"
#include "core/transform.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wolvrix::lib::store
{

    enum class JsonPrintMode
    {
        Compact,
        PrettyCompact,
        Pretty
    };

    using StoreDiagnosticKind = wolvrix::lib::diag::DiagnosticKind;
    using StoreDiagnostic = wolvrix::lib::diag::Diagnostic;

    class StoreDiagnostics : public wolvrix::lib::diag::Diagnostics
    {
    public:
        using wolvrix::lib::diag::Diagnostics::Diagnostics;
        using wolvrix::lib::diag::Diagnostics::error;
        using wolvrix::lib::diag::Diagnostics::warning;
        using wolvrix::lib::diag::Diagnostics::info;
        using wolvrix::lib::diag::Diagnostics::debug;
    };

    struct StoreOptions
    {
        std::optional<std::string> outputDir;
        std::optional<std::string> outputFilename;
        JsonPrintMode jsonMode = JsonPrintMode::PrettyCompact;
        std::vector<std::string> topOverrides;
    };

    struct StoreResult
    {
        bool success = true;
        std::vector<std::string> artifacts;
    };

    class Store
    {
    public:
        explicit Store(StoreDiagnostics *diagnostics = nullptr);
        virtual ~Store() = default;

        StoreResult store(const wolvrix::lib::grh::Design &design, const StoreOptions &options = StoreOptions());

    protected:
        StoreDiagnostics *diagnostics() const noexcept { return diagnostics_; }

        std::vector<const wolvrix::lib::grh::Graph *> resolveTopGraphs(const wolvrix::lib::grh::Design &design,
                                                                       const StoreOptions &options) const;
        std::filesystem::path resolveOutputDir(const StoreOptions &options) const;
        bool ensureParentDirectory(const std::filesystem::path &path) const;
        std::unique_ptr<std::ofstream> openOutputFile(const std::filesystem::path &path) const;

        void reportError(std::string message, std::string context = {}) const;
        void reportWarning(std::string message, std::string context = {}) const;

    protected:
        virtual StoreResult storeImpl(const wolvrix::lib::grh::Design &design,
                                      std::span<const wolvrix::lib::grh::Graph *const> topGraphs,
                                      const StoreOptions &options) = 0;

        bool validateTopGraphs(const std::vector<const wolvrix::lib::grh::Graph *> &topGraphs) const;

    private:
        StoreDiagnostics *diagnostics_ = nullptr;
    };

    class StoreJson : public Store
    {
    public:
        using Store::Store;

        std::optional<std::string> storeToString(const wolvrix::lib::grh::Design &design,
                                                 const StoreOptions &options = StoreOptions());

    private:
        StoreResult storeImpl(const wolvrix::lib::grh::Design &design,
                              std::span<const wolvrix::lib::grh::Graph *const> topGraphs,
                              const StoreOptions &options) override;
    };

    // Human-readable inspection format; JSON remains the round-trip format.
    // A session supplies the validated grhsim writer schedule when available.
    class StoreText : public Store
    {
    public:
        explicit StoreText(StoreDiagnostics *diagnostics = nullptr,
                           const wolvrix::lib::transform::SessionStore *session = nullptr);

        std::optional<std::string> storeToString(const wolvrix::lib::grh::Design &design,
                                                const StoreOptions &options = StoreOptions());

    private:
        StoreResult storeImpl(const wolvrix::lib::grh::Design &design,
                              std::span<const wolvrix::lib::grh::Graph *const> topGraphs,
                              const StoreOptions &options) override;

        const wolvrix::lib::transform::SessionStore *session_ = nullptr;
    };

} // namespace wolvrix::lib::store

#endif // WOLVRIX_STORE_HPP
