#pragma once

#include <string>

namespace setup_json_policy {

enum class AutoUpdateDecision {
    Allow,
    BlockReadFailure,
    BlockInvalidJson,
    BlockMissingWorkspaceRoot,
    BlockUnknownTopLevelField,
};

inline bool IsKnownTopLevelField(const std::string& key) {
    return key == "workspaceRoot" ||
           key == "workspaceRootMode" ||
           key == "tempExternalLectureDirs" ||
           key == "annotToolModeOrder" ||
           key == "annotToolModeState" ||
           // Legacy setup keys are known so old files can be migrated intentionally.
           key == "annotToolUiStructure" ||
           key == "annotToolOrder" ||
           key == "annotToolState";
}

inline AutoUpdateDecision ResolveAutoUpdateDecision(bool read_ok,
                                                   bool syntactically_valid_json,
                                                   bool has_workspace_root,
                                                   bool has_unknown_top_level_fields) {
    if (!read_ok) return AutoUpdateDecision::BlockReadFailure;
    if (!syntactically_valid_json) return AutoUpdateDecision::BlockInvalidJson;
    if (!has_workspace_root) return AutoUpdateDecision::BlockMissingWorkspaceRoot;
    if (has_unknown_top_level_fields) return AutoUpdateDecision::BlockUnknownTopLevelField;
    return AutoUpdateDecision::Allow;
}

inline bool AllowsAutoUpdate(AutoUpdateDecision decision) {
    return decision == AutoUpdateDecision::Allow;
}

} // namespace setup_json_policy