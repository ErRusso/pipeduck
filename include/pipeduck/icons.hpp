#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace pipeduck::icons {

inline constexpr const char* kAppIconName = "pipeduck";

std::optional<std::filesystem::path> icon_theme_path();

} // namespace pipeduck::icons
