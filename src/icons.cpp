#include "pipeduck/icons.hpp"

#include <unistd.h>

#include <cstdlib>

#include "pipeduck/common.hpp"

namespace pipeduck::icons {

std::optional<std::filesystem::path> icon_theme_path() {
    if (const auto configured = getenv_string("PIPEDUCK_ICON_THEME_PATH")) {
        const std::filesystem::path path(*configured);
        if (std::filesystem::exists(path)) {
            return path;
        }
    }

    // Installed layout: PREFIX/bin/pipeduck -> PREFIX/share/icons
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) {
        const auto prefix = executable.parent_path().parent_path();
        const auto installed = prefix / "share" / "icons";
        if (std::filesystem::exists(installed)) {
            return installed;
        }
    }

#ifdef PIPEDUCK_ASSETS_DIR
    const std::filesystem::path source(PIPEDUCK_ASSETS_DIR);
    if (std::filesystem::exists(source)) {
        return source;
    }
#endif

    const std::filesystem::path system("/usr/share/icons");
    if (std::filesystem::exists(system)) {
        return system;
    }
    return std::nullopt;
}

} // namespace pipeduck::icons
