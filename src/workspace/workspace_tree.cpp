// file: main/workspace_tree.cpp
// Owns lecture/session/file enumeration and selection reload flows.
// Converted from main/workspace_tree.cppinc.
#include "workspace/workspace_tree.h"
#include "core/app_core.h"
#include "core/preview_trace.h"
#include "ui/core/main_window_api.h"
#include "workspace/workspace_config_io.h"
#include "workspace/workspace_actions.h"
#include "schedule/schedule.h"
#include "workspace/file_ops.h"
#include <windows.h>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <climits>

// Inline the original implementation (no changes to function bodies)
#include "workspace/workspace_tree.cppinc"

