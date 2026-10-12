#ifndef WOLVRIX_TRANSFORM_MEM_TO_REG_HPP
#define WOLVRIX_TRANSFORM_MEM_TO_REG_HPP

#include "core/transform.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace wolvrix::lib::transform
{

    struct MemToRegOptions
    {
        int64_t rowLimit = 32;
        bool strictInit = false;
        // Empty means all eligible memories; otherwise only exact symbols are lowered.
        std::vector<std::string> memorySymbols;
    };

    class MemToRegPass : public Pass
    {
    public:
        MemToRegPass();
        explicit MemToRegPass(MemToRegOptions options);

        PassResult run() override;

    private:
        MemToRegOptions options_;
    };

} // namespace wolvrix::lib::transform

#endif // WOLVRIX_TRANSFORM_MEM_TO_REG_HPP
