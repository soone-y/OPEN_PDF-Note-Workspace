#include "ui/shortcut_help.h"
#include "core/localization.h"

namespace shortcut_help {
bool IsOverridden(const application_shortcuts::Binding& binding,
                  const std::vector<AnnotToolShortcutBinding>& bindings) {
    for (const auto& custom : bindings)
        if (custom.ctrl && custom.vk == binding.key && custom.alt == binding.alt && custom.shift == binding.shift) return true;
    return false;
}

std::wstring MainMenuLabel(const std::wstring& label, application_shortcuts::Action action) {
    const auto* binding = application_shortcuts::Find(action, application_shortcuts::Surface::Main);
    if (!binding || IsOverridden(*binding, AnnotToolShortcutBindings())) return label;
    return application_shortcuts::MenuHint(label, action, application_shortcuts::Surface::Main);
}

std::vector<std::wstring> ApplicationRows(const std::vector<AnnotToolShortcutBinding>& bindings) {
    std::vector<std::wstring> rows;
    for (const auto& binding : application_shortcuts::bindings) {
        if (!application_shortcuts::Supports(binding, application_shortcuts::Surface::Main)) continue;
        auto row = std::wstring(binding.chord) + L"  —  " + localization::Text(binding.mainLabel);
        if (IsOverridden(binding, bindings)) row += localization::Text(L"shortcut.overridden");
        rows.push_back(std::move(row));
    }
    return rows;
}

std::wstring CommonOverview(const std::vector<AnnotToolShortcutBinding>& bindings) {
    std::wstring text;
    for (const auto& row : ApplicationRows(bindings)) text += row + L"\r\n";
    return text + L"\r\n" + localization::Text(L"shortcut.context") + L"\r\n\r\n" +
        localization::Text(L"settings.shortcut.fixed_overview");
}

std::wstring TargetLabel(AnnotToolShortcutTargetKind kind,
                         AnnotToolFamily family, ToolMode mode, ShapeDrawMode shapeDrawMode) {
    if (kind == AnnotToolShortcutTargetKind::Category) {
        switch (family) {
        case AnnotToolFamily::Select: return localization::Text(L"settings.shortcut.target.category_select");
        case AnnotToolFamily::Pan: return localization::Text(L"settings.shortcut.target.category_pan");
        case AnnotToolFamily::Magnifier: return localization::Text(L"settings.shortcut.target.category_magnifier");
        case AnnotToolFamily::Text: return localization::Text(L"settings.shortcut.target.category_text");
        case AnnotToolFamily::Marker: return localization::Text(L"settings.shortcut.target.category_marker");
        case AnnotToolFamily::Pen: return localization::Text(L"settings.shortcut.target.category_pen");
        case AnnotToolFamily::Shape: return localization::Text(L"settings.shortcut.target.category_shape");
        case AnnotToolFamily::Eraser: return localization::Text(L"settings.shortcut.target.category_eraser");
        default: return localization::Text(L"settings.shortcut.target.category");
        }
    }
    if (family == AnnotToolFamily::Shape) {
        if (AnnotToolUsesGeometry(mode, AnnotToolGeometry::Shape)) {
            return localization::Text(L"settings.shortcut.target.detail_shape");
        }
        const bool emphasis = IsEmphasisAnnotToolMode(mode, shapeDrawMode);
        if (IsArrowAnnotToolMode(mode)) return localization::Text(emphasis ? L"settings.shortcut.target.emphasis_arrow" : L"settings.shortcut.target.stroke_arrow");
        if (IsWaveAnnotToolMode(mode)) return localization::Text(emphasis ? L"settings.shortcut.target.emphasis_wave" : L"settings.shortcut.target.stroke_wave");
        return localization::Text(emphasis ? L"settings.shortcut.target.emphasis_line" : L"settings.shortcut.target.stroke_line");
    }
    switch (mode) {
    case ToolMode::MarkerText: return localization::Text(L"settings.shortcut.target.detail_marker");
    case ToolMode::MarkerTextUnderline: return localization::Text(L"settings.shortcut.target.detail_marker_underline");
    case ToolMode::MarkerTextColor: return localization::Text(L"settings.shortcut.target.detail_marker_text_color");
    case ToolMode::MarkerFree: return localization::Text(L"settings.shortcut.target.detail_free_marker");
    case ToolMode::TextBox: return localization::Text(L"settings.shortcut.target.detail_text");
    case ToolMode::Freehand: return localization::Text(L"settings.shortcut.target.detail_pen");
    case ToolMode::Select: return localization::Text(L"settings.shortcut.target.detail_select");
    case ToolMode::Pan: return localization::Text(L"settings.shortcut.target.detail_pan");
    case ToolMode::Magnifier: return localization::Text(L"settings.shortcut.target.detail_magnifier");
    case ToolMode::Eraser: return localization::Text(L"settings.shortcut.target.detail_eraser");
    default: return localization::Text(L"settings.shortcut.target.detail");
    }
}

empty_view_guide::Content Build(int displayMode, const std::vector<AnnotToolShortcutBinding>& bindings,
                               ShapeDrawMode shapeDrawMode) {
    empty_view_guide::Content content;
    if (displayMode == 2) return content;
    content.title = localization::Text(L"guide.title");
    if (displayMode == 0) content.usage = localization::Text(L"guide.usage");
    content.generalTitle = localization::Text(L"guide.general_title");
    content.annotationTitle = localization::Text(L"guide.annotation_title");
    content.scrollHint = localization::Text(L"guide.scroll_hint");
    content.displayHint = localization::Text(L"guide.display_settings");
    for (auto& row : ApplicationRows(bindings)) content.general.push_back(std::move(row));
    content.general.push_back(localization::Text(L"shortcut.context"));
    for (const wchar_t* id : {L"guide.save", L"guide.search", L"guide.focus", L"guide.zoom",
                             L"guide.zoom_reset", L"guide.undo", L"guide.categories",
                             L"guide.details", L"guide.colors", L"guide.annotation_context"})
        content.general.push_back(localization::Text(id));
    for (const auto& binding : bindings) {
        content.annotation.push_back(binding.key + L"  —  " +
            TargetLabel(binding.targetKind, binding.family, binding.mode, shapeDrawMode));
    }
    if (content.annotation.empty()) content.annotation.push_back(localization::Text(L"guide.no_bindings"));
    content.annotation.push_back(localization::Text(L"guide.customize"));
    return content;
}

}
