#ifndef WOLVRIX_TRANSFORM_NODE_EVOLUTION_HPP
#define WOLVRIX_TRANSFORM_NODE_EVOLUTION_HPP

#include "core/grh.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wolvrix::lib::transform
{

    // Optional native evidence. The recorder does not infer rewrite reasons.
    class GraphEvolutionTrace
    {
    public:
        struct Ref
        {
            bool operation = false;
            uint32_t index = 0;
            uint32_t generation = 0;
            bool graph = false;
            friend bool operator==(const Ref &, const Ref &) = default;
        };

        struct Geometry
        {
            std::string kind;
            Ref base;
            std::vector<int64_t> physicalShape, coordinates;
            int64_t offset = 0, width = 0, element = -1;
        };

        GraphEvolutionTrace(grh::Graph &graph, const std::string &path, const std::string &pass,
                            const std::string &inputSha256 = {}, bool sourceEndpointsOnly = false);
        ~GraphEvolutionTrace();
        GraphEvolutionTrace(const GraphEvolutionTrace &) = delete;
        GraphEvolutionTrace &operator=(const GraphEvolutionTrace &) = delete;

        static Ref ref(grh::OperationId operation) noexcept;
        static Ref ref(grh::ValueId value) noexcept;
        static Ref graphRef(const grh::Graph &graph) noexcept;
        bool enabled() const noexcept { return static_cast<bool>(impl_); }
        void step(std::string action, std::string reason, std::vector<Ref> before,
                  std::vector<Ref> after);
        void step(std::string action, std::string reason, std::vector<Ref> before,
                  std::vector<Ref> after, Geometry geometry);
        void retain(grh::OperationId operation, std::string reason);
        void finish();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace wolvrix::lib::transform

#endif // WOLVRIX_TRANSFORM_NODE_EVOLUTION_HPP
