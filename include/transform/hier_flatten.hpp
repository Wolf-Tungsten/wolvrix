#ifndef WOLVRIX_TRANSFORM_HIER_FLATTEN_HPP
#define WOLVRIX_TRANSFORM_HIER_FLATTEN_HPP

#include "core/transform.hpp"

namespace wolvrix::lib::transform
{

    struct HierFlattenOptions
    {
        bool preserveFlattenedModules = false;
        // Kept for API/CLI compatibility. Declared value/op symbols from
        // inlined child graphs are now ALWAYS preserved under hierarchical
        // names (`inst$...$name`) and re-registered via addDeclaredSymbol in
        // every mode; undeclared child entities always take internal
        // `_val_N`/`_op_N` names. The mode only controls whether an
        // undeclared parent-side value mapped to a child port is renamed to
        // the child's hierarchical port name (All/Hierarchy) or left as-is
        // (Stateful/None).
        enum class SymProtectMode
        {
            All,
            Hierarchy,
            Stateful,
            None
        };
        SymProtectMode symProtect = SymProtectMode::All;
    };

    class HierFlattenPass : public Pass
    {
    public:
        HierFlattenPass();
        explicit HierFlattenPass(HierFlattenOptions options);

        PassResult run() override;

    private:
        HierFlattenOptions options_;
    };

} // namespace wolvrix::lib::transform

#endif // WOLVRIX_TRANSFORM_HIER_FLATTEN_HPP
