#include "grhsim/dialect/core.hpp"

#include "grhsim/dialect/registry.hpp"

#include <array>
#include <string>
#include <string_view>

namespace wolvrix::lib::grhsim
{

    bool registerCoreDialect(DialectRegistry &registry)
    {
        std::string error;
        if (!registry.registerDialect(
                DialectDefinition{"core", "1", "wolvrix.grhsim.core.v1"}, error))
        {
            return false;
        }

        constexpr std::array<std::string_view, 4> types = {
            "core.logic", "core.real", "core.string", "core.array"};
        for (std::string_view name : types)
        {
            if (!registry.registerType("core", name, error)) return false;
        }

        constexpr std::array<std::string_view, 49> ops = {
            "core.input.read", "core.output.write",
            "core.state.read", "core.state.regWrite", "core.state.latchWrite",
            "core.state.memRead", "core.state.memWrite", "core.state.memFill",
            "core.state.memAssign", "core.state.memWriteSeq",
            "core.system.function", "core.system.task", "core.dpi.call",
            "core.compute.constant", "core.compute.add", "core.compute.sub",
            "core.compute.mul", "core.compute.div", "core.compute.mod",
            "core.compute.eq", "core.compute.ne", "core.compute.caseEq",
            "core.compute.caseNe", "core.compute.wildcardEq", "core.compute.wildcardNe",
            "core.compute.lt", "core.compute.le", "core.compute.gt", "core.compute.ge",
            "core.compute.and", "core.compute.or", "core.compute.xor", "core.compute.xnor",
            "core.compute.not", "core.compute.logicAnd", "core.compute.logicOr",
            "core.compute.logicNot", "core.compute.reduceAnd", "core.compute.reduceOr",
            "core.compute.reduceXor", "core.compute.reduceNor", "core.compute.reduceNand",
            "core.compute.reduceXnor", "core.compute.shl", "core.compute.lshr",
            "core.compute.ashr", "core.compute.mux", "core.compute.assign",
            "core.compute.concat"};
        for (std::string_view name : ops)
        {
            if (!registry.registerOp("core", name, error)) return false;
        }
        constexpr std::array<std::string_view, 6> remainingOps = {
            "core.compute.replicate", "core.compute.sliceStatic",
            "core.compute.sliceDynamic", "core.compute.sliceArray", "core.compute.bitSelect",
            "core.compute.prioritySelect"};
        for (std::string_view name : remainingOps)
        {
            if (!registry.registerOp("core", name, error)) return false;
        }

        if (!registry.registerFunctionDecl("core", "core.dpi", error)) return false;
        constexpr std::array<std::string_view, 4> initSteps = {
            "core.init.const", "core.init.random", "core.init.readmem", "core.init.fill"};
        for (std::string_view name : initSteps)
        {
            if (!registry.registerInitStep("core", name, error)) return false;
        }
        return true;
    }

} // namespace wolvrix::lib::grhsim
