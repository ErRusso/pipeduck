#pragma once

#include "pipeduck/duck.hpp"

namespace pipeduck {

struct TrayOptions {
    DuckingSettings settings;
};

int run_tray(const TrayOptions& options);

} // namespace pipeduck
