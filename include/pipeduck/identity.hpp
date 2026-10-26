#pragma once

#include <string>

#include "pipeduck/common.hpp"
#include "pipeduck/config.hpp"

namespace pipeduck {

struct AudioIdentity {
    std::optional<std::string> application_name;
    std::optional<std::string> application_process_binary;
    std::optional<std::string> media_name;
    std::optional<std::string> media_role;
    std::optional<std::string> media_class;
    std::optional<std::string> node_name;

    static AudioIdentity from_props(const StringMap& props);
    bool matches_configured_source(const ConfiguredSource& source) const;
    // Compares only the fields an application keeps across restarts (binary or
    // application name, plus the media class). Used as a fallback when the
    // configured stream was re-created with new volatile fields.
    bool matches_stable_source(const ConfiguredSource& source) const;
    bool is_playback_stream() const;
    bool looks_like_voice_source() const;
    std::string label() const;
};

ConfiguredSource configured_source_from_identity(const std::string& label,
                                                 const AudioIdentity& identity);

std::string source_label(const AudioIdentity& identity);
bool looks_like_voice_text(const std::vector<std::optional<std::string>>& parts);

} // namespace pipeduck
