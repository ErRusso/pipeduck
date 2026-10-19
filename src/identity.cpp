#include "pipeduck/identity.hpp"

#include <algorithm>
#include <optional>
#include <vector>

namespace pipeduck {
namespace {

std::optional<std::string> get(const StringMap& props, const std::string& key) {
    const auto it = props.find(key);
    if (it == props.end() || it->second.empty()) {
        return std::nullopt;
    }
    return it->second;
}

bool field_matches(const std::optional<std::string>& configured,
                   const std::optional<std::string>& actual) {
    if (!configured.has_value() || configured->empty()) {
        return true;
    }
    return actual.has_value() && *actual == *configured;
}

bool same_present_value(const std::optional<std::string>& left,
                        const std::optional<std::string>& right) {
    return left.has_value() && right.has_value() && !left->empty() && *left == *right;
}

bool matches_voice_variant(const AudioIdentity& identity, const ConfiguredSource& source) {
    if (!identity.is_playback_stream()) {
        return false;
    }
    if (!field_matches(source.media_class, identity.media_class)) {
        return false;
    }
    if (!same_present_value(source.application_process_binary,
                            identity.application_process_binary)) {
        return false;
    }
    const bool identity_hint =
        looks_like_voice_text({identity.application_name, identity.application_process_binary,
                               identity.media_name, identity.media_role, identity.node_name});
    const bool source_hint =
        looks_like_voice_text({source.application_name, source.application_process_binary,
                               source.media_name, source.media_role, source.node_name});
    return identity_hint || source_hint;
}

} // namespace

AudioIdentity AudioIdentity::from_props(const StringMap& props) {
    AudioIdentity identity;
    identity.application_name = get(props, "application.name");
    identity.application_process_binary = get(props, "application.process.binary");
    identity.media_name = get(props, "media.name");
    identity.media_role = get(props, "media.role");
    identity.media_class = get(props, "media.class");
    identity.node_name = get(props, "node.name");
    return identity;
}

bool AudioIdentity::matches_configured_source(const ConfiguredSource& source) const {
    const bool exact =
        field_matches(source.application_name, application_name) &&
        field_matches(source.application_process_binary, application_process_binary) &&
        field_matches(source.media_name, media_name) &&
        field_matches(source.media_role, media_role) &&
        field_matches(source.media_class, media_class) &&
        field_matches(source.node_name, node_name);
    return exact || matches_voice_variant(*this, source);
}

bool AudioIdentity::is_playback_stream() const {
    return media_class.has_value() && *media_class == "Stream/Output/Audio";
}

bool AudioIdentity::looks_like_voice_source() const {
    return looks_like_voice_text(
        {application_name, application_process_binary, media_name, media_role, node_name});
}

std::string AudioIdentity::label() const {
    return source_label(*this);
}

ConfiguredSource configured_source_from_identity(const std::string& label,
                                                 const AudioIdentity& identity) {
    ConfiguredSource source;
    source.label = label;
    source.application_name = identity.application_name;
    source.application_process_binary = identity.application_process_binary;
    source.media_name = identity.media_name;
    source.media_role = identity.media_role;
    source.media_class = identity.media_class;
    source.node_name = identity.node_name;
    return source;
}

std::string source_label(const AudioIdentity& identity) {
    const auto value_or = [](const std::optional<std::string>& value, const char* fallback) {
        return value.has_value() && !value->empty() ? *value : std::string(fallback);
    };
    return value_or(identity.application_name, "unknown-app") + " / " +
           value_or(identity.application_process_binary, "unknown-bin") + " / " +
           value_or(identity.media_name, "unknown-media");
}

bool looks_like_voice_text(const std::vector<std::optional<std::string>>& parts) {
    std::string text;
    for (const auto& part : parts) {
        if (part.has_value() && !part->empty()) {
            if (!text.empty()) {
                text.push_back(' ');
            }
            text += *part;
        }
    }
    text = to_lower(text);
    return text.find("voice") != std::string::npos || text.find("webrtc") != std::string::npos ||
           text.find("discord") != std::string::npos ||
           text.find("communication") != std::string::npos ||
           text.find("playstream") != std::string::npos;
}

} // namespace pipeduck
