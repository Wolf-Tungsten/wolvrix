#ifndef WOLVRIX_GRHSIM_DIALECT_MASKED_OR_HPP
#define WOLVRIX_GRHSIM_DIALECT_MASKED_OR_HPP

#include "grhsim/ir/model.hpp"

namespace wolvrix::lib::grhsim
{
    // Four integers per term: selector operand, selector bit, data operand,
    // data bit offset. Absent views means the dense (mask, array) encoding.
    inline const std::vector<int64_t> *maskedOrViews(const GrhSimModel &model, const SimOp &op)
    {
        for (const auto &parameter : model.parameters(op))
            if (model.text(parameter.name) == "views")
                return std::get_if<std::vector<int64_t>>(&parameter.value);
        return nullptr;
    }

    inline uint64_t maskedOrTermCount(const GrhSimModel &model, const SimOp &op)
    {
        if (const auto *views = maskedOrViews(model, op)) return views->size() / 4;
        const auto mask = model.operands(op).front();
        return model.types()[model.values()[mask.index - 1].type.index - 1].width;
    }
}

#endif
