#ifndef WOLVRIX_FIRRTL_ANNOTATIONS_HPP
#define WOLVRIX_FIRRTL_ANNOTATIONS_HPP

#include "internal.hpp"

namespace wolvrix::lib::ingest::firrtl::detail {
struct Annotation {
    std::string kind;
    std::string target;
    std::string module;
    std::map<std::string, std::string> fields;
    std::string_view raw;
    Range range;
};
struct AnnotationIndex {
    std::map<std::string, uint64_t> counts;
    std::vector<Annotation> records;
};
AnnotationIndex readAnnotations(const Source& source);
void applyAnnotations(const Source& source, const Module& module, const AnnotationIndex& annotations, grh::Graph& graph);
} // namespace wolvrix::lib::ingest::firrtl::detail
#endif
