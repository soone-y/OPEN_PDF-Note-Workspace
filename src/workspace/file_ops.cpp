// file: main/file_ops.cpp
#include "workspace/file_ops.h"
#include "recovery/stage_recovery.h"
#include "ui/core/main_window_api.h"
#include "workspace/workspace_tree.h"
#include "workspace/workspace_actions.h"
#include "workspace/workspace_config_io.h"
#include "workspace/workspace_write_lock.h"
#include "workspace/workspace_resource_identity.h"
#include "ui/dialogs/dialogs.h"
#include "core/app_core.h"
#include "core/localization.h"
#include "core/atomic_write.h"
#include "core/fault_injection.h"
#include "core/preview_trace.h"
#include "clrop/bridge.h"
#include "bridge/view_bridge.h"
#include "file_output/file_output.h"
#include "note/note_parser.h"
#include "note/note_identity_store.h"
#include "pdf_view/pdf_view.h"
#include "ui/noop_nav_guard.h"

#include <windows.h>
#include <string>
#include <vector>
#include <filesystem>
#include <cwctype>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

// Forward decls for internal functions shared between the two cppinc files
static bool EnsureOperationReadyForPath(HWND owner,
                                        const std::filesystem::path& src,
                                        bool isPdf,
                                        const std::wstring& title);

#include "workspace/file_ops_move_rename.cppinc"
#include "workspace/file_ops_stage_manager.cppinc"
