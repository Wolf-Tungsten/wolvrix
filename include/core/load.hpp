#ifndef WOLVRIX_LOAD_HPP
#define WOLVRIX_LOAD_HPP

#include "core/grh.hpp"

#include <filesystem>
#include <string_view>

namespace wolvrix::lib::load
{

    class Load
    {
    public:
        virtual ~Load() = default;

        virtual wolvrix::lib::grh::Design load(std::string_view data) = 0;
    };

    class LoadJson final : public Load
    {
    public:
        wolvrix::lib::grh::Design load(std::string_view data) override;
        wolvrix::lib::grh::Design loadFile(const std::filesystem::path &path);
    };

} // namespace wolvrix::lib::load

#endif // WOLVRIX_LOAD_HPP
