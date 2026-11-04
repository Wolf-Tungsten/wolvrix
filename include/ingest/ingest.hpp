#ifndef WOLVRIX_INGEST_INGEST_HPP
#define WOLVRIX_INGEST_INGEST_HPP

#include "core/grh.hpp"
#include "core/logging.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace wolvrix::lib::ingest {

enum class SourceLanguage { SystemVerilog, Firrtl };

struct IngestSource {
    SourceLanguage language = SourceLanguage::SystemVerilog;
    std::filesystem::path path;
    std::string unit;
};

struct SourcePosition {
    std::string unit;
    std::filesystem::path file;
    uint64_t line = 0;
    uint64_t column = 0;
    std::string sourceLine;
    std::string sourceInfo;
};

enum class IngestSeverity { Error, Warning, Info, Debug };

struct IngestDiagnostic {
    IngestSeverity kind = IngestSeverity::Error;
    std::string message;
    std::string context;
    std::string passName;
    std::string originSymbol;
    std::optional<SourcePosition> location;
};

// Integer bits are canonical, MSB first; width and signedness are part of the type.
struct IntegerParameter {
    uint32_t width = 0;
    bool isSigned = false;
    std::string bits;
    bool operator==(const IntegerParameter&) const = default;
};

struct RawStringParameter {
    std::string text;
    bool operator==(const RawStringParameter&) const = default;
};

using ParameterValue = std::variant<IntegerParameter, std::string, double, RawStringParameter>;

struct ModuleParameter {
    std::string name;
    ParameterValue value;
    bool operator==(const ModuleParameter&) const = default;
};

enum class LeafDirection { Input, Output, Inout };

struct PortLeafSignature {
    std::string name;
    std::vector<std::string> fieldPath;
    LeafDirection direction = LeafDirection::Input;
    grh::ValueType valueType = grh::ValueType::Logic;
    int32_t elementWidth = 0;
    bool isSigned = false;
    std::vector<int64_t> shape;
    // Field-path depth of each Vector dimension: r[0].data has depth 1,
    // while r.data[0] has depth 2. Arrays remain shaped GRH values.
    std::vector<std::size_t> shapeFieldDepths;
    // Scanning retains source semantics before width/reset/ABI resolution.
    std::string sourceType;
    bool requiresTypeResolution = false;
    bool isConst = false;
};

struct ModuleSignature {
    SourceLanguage language = SourceLanguage::SystemVerilog;
    std::string sourceUnit;
    std::string name;
    std::string implementationName;
    bool external = false;
    std::vector<ModuleParameter> parameters;
    std::vector<PortLeafSignature> ports;
    std::optional<SourcePosition> location;
};

struct ModuleSpecialization {
    SourceLanguage language = SourceLanguage::SystemVerilog;
    std::string sourceUnit;
    std::string implementationName;
    std::vector<ModuleParameter> parameters;
};

struct PortBinding {
    std::string sourcePort;
    std::string implementationPort;
};

struct ExternalBinding {
    std::string sourceUnit;
    std::string moduleName;
    ModuleSpecialization implementation;
    std::vector<PortBinding> ports;
};

enum class FirrtlInvalidPolicy { Unknown, Zero };
enum class FirrtlInitPolicy { Unknown, Zero };

struct FirrtlOptions {
    std::optional<std::string> expectedVersion;
    bool nodeAnnotations = true;
    // Number of independent FIRRTL module lowerers. One preserves the legacy
    // serial path; larger values lower into final-owner unpublished graphs
    // before the transaction publishes them in dependency order.
    std::size_t lowerThreads = 1;
    FirrtlInvalidPolicy invalidPolicy = FirrtlInvalidPolicy::Unknown;
    FirrtlInitPolicy initPolicy = FirrtlInitPolicy::Unknown;
    // Full layer paths. Selecting a child also enables its ancestors.
    std::vector<std::string> enabledLayers;
    // Sidecar JSON and explicit filesystem roots for blackbox resources.
    std::vector<std::string> annotationFiles;
    std::vector<std::string> resourceRoots;
};

enum class IngestPipeline { Ingest, GrhPreFlatten };

struct IngestRequest {
    std::vector<IngestSource> sources;
    // Empty uses the caller's working directory. Relative source paths use this root.
    std::filesystem::path inputRoot;
    std::vector<std::string> tops;
    std::vector<std::string> slangArgs;
    FirrtlOptions firrtl;
    std::vector<ExternalBinding> externalBindings;
    LogLevel logLevel = LogLevel::Info;
    Logger::Sink logSink;
    // Explicit endpoint selection. The default returns the historical ingest
    // graph; GrhPreFlatten additionally runs the production prefix, with
    // cross-graph barriers followed by fused graph-local worker chains.
    IngestPipeline pipeline = IngestPipeline::Ingest;
};

class SourceOwner {
public:
    virtual ~SourceOwner() = default;
};

using SourceOwners = std::vector<std::shared_ptr<const SourceOwner>>;

struct InterfaceScanResult {
    SourceOwners sourceOwners;
    std::vector<ModuleSignature> modules;
    std::vector<IngestDiagnostic> diagnostics;
    bool success = false;
};

struct IngestPrefixStage {
    std::string pass;
    std::vector<IngestDiagnostic> diagnostics;
    double wallSeconds = 0;
    double workerSeconds = 0;
    bool overlapped = false;
};

struct IngestResult {
    // Declared first so the design and diagnostics are destroyed before their sources.
    SourceOwners sourceOwners;
    std::optional<grh::Design> design;
    std::vector<ModuleSignature> modules;
    std::vector<IngestDiagnostic> diagnostics;
    // Canonical source identities, preprocessing inputs and specialization keys.
    std::vector<std::string> dependencies;
    std::vector<IngestPrefixStage> prefixStages;
    std::size_t ingestDiagnosticCount = 0;
    double prefixWallSeconds = 0;

    bool success() const noexcept;
};

IngestResult readSources(const IngestRequest& request);
InterfaceScanResult scanInterfaces(const IngestRequest& request);
IngestResult specializeModule(const IngestRequest& request,
                             const ModuleSpecialization& specialization);
IngestResult linkModules(const IngestRequest& request, std::vector<IngestResult> modules);

} // namespace wolvrix::lib::ingest

#endif // WOLVRIX_INGEST_INGEST_HPP
