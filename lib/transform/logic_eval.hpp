#ifndef WOLVRIX_TRANSFORM_LOGIC_EVAL_HPP
#define WOLVRIX_TRANSFORM_LOGIC_EVAL_HPP

#include "core/grh.hpp"
#include "slang/numeric/SVInt.h"

namespace wolvrix::lib::transform::detail {

// Shared pure evaluation only; each transform owns its traversal and rewrites.
std::optional<slang::SVInt> evaluateLogicOperation(const grh::Graph& graph, grh::OperationId operation,
                                                 std::span<const slang::SVInt> operands,
                                                 grh::OperationKind kind);

} // namespace wolvrix::lib::transform::detail
#endif
