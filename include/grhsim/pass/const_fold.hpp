#ifndef WOLVRIX_GRHSIM_PASS_CONST_FOLD_HPP
#define WOLVRIX_GRHSIM_PASS_CONST_FOLD_HPP

#include "grhsim/pass/pass.hpp"

namespace wolvrix::lib::grhsim
{
    void registerConstFoldPass(PassRegistry &registry);
}

#endif // WOLVRIX_GRHSIM_PASS_CONST_FOLD_HPP
