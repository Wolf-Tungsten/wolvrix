#ifndef WOLVRIX_TRANSFORM_GRHSIM_NODE_CSE_HPP
#define WOLVRIX_TRANSFORM_GRHSIM_NODE_CSE_HPP

#include "core/transform.hpp"

namespace wolvrix::lib::transform::grhsim {

struct NodeCseOptions {
    std::string top;
    std::string mode = "gsim";
    std::filesystem::path report;
};

// Runs only on the converted, unpartitioned graph. Equivalence analysis never
// grants primitive CSE permission across separately retained source nodes.
class NodeCsePass final : public transform::Pass {
public:
    explicit NodeCsePass(NodeCseOptions options = {});
    PassResult run() override;
    bool supportsDialect(std::string_view dialect) const noexcept override;
private:
    NodeCseOptions options_;
};

// Stores a compact checkpoint census; it does not persist an executable AST.
void recordNodeRetention(grh::Graph& graph);

} // namespace wolvrix::lib::transform::grhsim
#endif
