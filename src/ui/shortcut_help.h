#pragma once

#include "core/app_core.h"
#include "ui/empty_view_guide.h"
#include "ui/application_shortcuts.h"

namespace shortcut_help {
[[nodiscard]] bool IsOverridden(const application_shortcuts::Binding& binding,
    const std::vector<AnnotToolShortcutBinding>& bindings);
[[nodiscard]] std::wstring MainMenuLabel(const std::wstring& label, application_shortcuts::Action action);
[[nodiscard]] std::vector<std::wstring> ApplicationRows(const std::vector<AnnotToolShortcutBinding>& bindings);
[[nodiscard]] std::wstring CommonOverview(const std::vector<AnnotToolShortcutBinding>& bindings);
[[nodiscard]] std::wstring TargetLabel(AnnotToolShortcutTargetKind kind,
    AnnotToolFamily family, ToolMode mode, ShapeDrawMode shapeDrawMode);
// Snapshot actual accepted bindings, never a parallel list of default keys.
[[nodiscard]] empty_view_guide::Content Build(int displayMode,
    const std::vector<AnnotToolShortcutBinding>& bindings, ShapeDrawMode shapeDrawMode);
}
