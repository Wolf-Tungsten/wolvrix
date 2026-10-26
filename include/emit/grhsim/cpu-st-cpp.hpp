#ifndef WOLVRIX_EMIT_GRHSIM_CPU_ST_CPP_HPP
#define WOLVRIX_EMIT_GRHSIM_CPU_ST_CPP_HPP

#include "core/emit.hpp"

namespace wolvrix::lib::emit {

// Consumes the ordinary GRH design and the current session's SinkSchedule.
class GrhSimCpuStCpp final : public Emit {
public:
    using Emit::Emit;
    static constexpr std::string_view id = "grhsim-cpu-st-cpp";
private:
    EmitResult emitImpl(const grh::Design& design,
                        std::span<const grh::Graph* const> tops,
                        const EmitOptions& options) override;
};

}
#endif
