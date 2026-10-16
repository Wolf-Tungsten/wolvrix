#include "grhsim/dialect/cpu.hpp"

#include "grhsim/dialect/registry.hpp"

namespace wolvrix::lib::grhsim
{
    bool registerCpuDialect(DialectRegistry &registry)
    {
        std::string error;
        if (!registry.registerDialect({"cpu", "1", "wolvrix.grhsim.cpu.v2"}, error)) return false;
        for (auto name : {"cpu.bool", "cpu.uint8", "cpu.sint8", "cpu.uint16", "cpu.sint16",
                          "cpu.uint32", "cpu.sint32", "cpu.uint64", "cpu.sint64", "cpu.uint",
                          "cpu.sint", "cpu.f32", "cpu.f64", "cpu.str", "cpu.array"})
            if (!registry.registerType("cpu", name, error)) return false;
        if (!registry.registerOp("cpu", "cpu.compute.maskedOr", error)) return false;
        return true;
    }
}
