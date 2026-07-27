// file: main/file_ops.cpp
#include "workspace/file_ops.h"
#include "ui/core/main_window_api.h"
#include "workspace/workspace_tree.h"
#include "workspace/workspace_actions.h"
#include "workspace/workspace_config_io.h"
#include "ui/dialogs/dialogs.h"
#include "core/app_core.h"
#include "core/atomic_write.h"
#include "clrop/bridge.h"
#include "file_output/file_output.h"
#include "note/note_parser.h"
#include "note/note_identity_store.h"
#include "pdf_view/pdf_view.h"
#include "ui/noop_nav_guard.h"

#include <windows.h>
#include <string>
#include <vector>
#include <filesystem>
#include <unordered_map>

// Forward decls for internal functions shared between the two cppinc files
static bool EnsureOperationReadyForPath(HWND owner,
                                        const std::filesystem::path& src,
                                        bool isPdf,
                                        const std::wstring& title);

#include "workspace/file_ops_move_rename.cppinc"
#include "workspace/file_ops_stage_manager.cppinc"
