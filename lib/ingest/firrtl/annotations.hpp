#ifndef WOLVRIX_FIRRTL_ANNOTATIONS_HPP
#define WOLVRIX_FIRRTL_ANNOTATIONS_HPP

#include "internal.hpp"

namespace wolvrix::lib::ingest::firrtl::detail {
struct Annotation {
    std::string kind;
    std::string target;
    std::string module;
    std::map<std::string, std::string> fields;
    std::string raw;
    Range range;
};
struct AnnotationIndex {
    std::map<std::string, uint64_t> counts;
    std::vector<Annotation> records;
    // Record indices grouped by module, retaining source order within each
    // group. This avoids rescanning the circuit annotation payload per graph.
    std::map<std::string, std::vector<std::size_t>> byModule;
};
AnnotationIndex readAnnotations(const Source& source);
AnnotationIndex parseAnnotations(std::string_view text);
void validateAnnotations(const Source& source, const AnnotationIndex& annotations);
void applyAnnotations(const Source& source, const Module& module, const AnnotationIndex& annotations, grh::Graph& graph);
} // namespace wolvrix::lib::ingest::firrtl::detail
#endif
