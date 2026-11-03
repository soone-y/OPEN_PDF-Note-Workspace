#pragma once

#include <windows.h>
#include <array>
#include <cstddef>
#include <string>

// Shared key identities only. Each application owns command dispatch, document
// availability, editing boundaries and failure handling through its usual menus.
namespace application_shortcuts {
enum class Surface : unsigned { Main = 1, Reader = 2 };
enum class Action {
    None, JumpPage, FitPage, OpenFile, CloseFile, CloseAll, ToggleAnnotations,
    RotateLeft, RotateRight, ResetRotation, Memo, Export, QuickPdf, QuickNote
};
struct Binding {
    Action action;
    UINT key;
    bool alt;
    bool shift;
    unsigned surfaces;
    bool pdfOnly;
    bool allowTextInput;
    const wchar_t* chord;
    const wchar_t* mainLabel;
    const wchar_t* readerLabel;
};
inline constexpr std::array<Binding, 13> bindings{{
    {Action::JumpPage, 'G', false, false, 3, true, false, L"Ctrl+G", L"shortcut.action.jump", L"shortcut.action.jump"},
    {Action::FitPage, 'E', false, false, 3, true, false, L"Ctrl+E", L"shortcut.action.fit_page", L"shortcut.action.fit_page"},
    {Action::OpenFile, 'O', false, false, 3, false, true, L"Ctrl+O", L"shortcut.action.import", L"shortcut.action.open"},
    // Q is near the tool-selection digits. Closing always requires Shift;
    // closing every display/tab additionally requires Alt. No Ctrl+Q alias.
    {Action::CloseFile, 'Q', false, true, 3, false, true, L"Ctrl+Shift+Q", L"shortcut.action.close", L"shortcut.action.close_tab"},
    {Action::CloseAll, 'Q', true, true, 3, false, true, L"Ctrl+Alt+Shift+Q", L"shortcut.action.close_pair", L"shortcut.action.close_tabs"},
    {Action::ToggleAnnotations, 'A', false, true, 3, true, false, L"Ctrl+Shift+A", L"shortcut.action.annotations", L"shortcut.action.annotations"},
    {Action::RotateLeft, VK_LEFT, false, true, 3, true, false, L"Ctrl+Shift+\u2190", L"shortcut.action.rotate_left", L"shortcut.action.rotate_left"},
    {Action::RotateRight, VK_RIGHT, false, true, 3, true, false, L"Ctrl+Shift+\u2192", L"shortcut.action.rotate_right", L"shortcut.action.rotate_right"},
    {Action::ResetRotation, '0', false, true, 3, true, false, L"Ctrl+Shift+0", L"shortcut.action.rotate_reset", L"shortcut.action.rotate_reset"},
    {Action::Memo, 'M', true, false, 1, false, true, L"Ctrl+Alt+M", L"shortcut.action.memo", nullptr},
    {Action::Export, 'E', true, false, 3, false, true, L"Ctrl+Alt+E", L"shortcut.action.export", L"shortcut.action.export_pdf"},
    {Action::QuickPdf, 'P', true, false, 1, false, true, L"Ctrl+Alt+P", L"shortcut.action.quick_pdf", nullptr},
    {Action::QuickNote, 'T', true, false, 1, false, true, L"Ctrl+Alt+T", L"shortcut.action.quick_note", nullptr},
}};

[[nodiscard]] constexpr bool Supports(const Binding& binding, Surface surface) {
    return (binding.surfaces & static_cast<unsigned>(surface)) != 0;
}
[[nodiscard]] constexpr const Binding* Find(Action action, Surface surface) {
    for (const auto& binding : bindings)
        if (binding.action == action && Supports(binding, surface)) return &binding;
    return nullptr;
}
[[nodiscard]] constexpr const Binding* Resolve(UINT key, bool ctrl, bool alt, bool shift, Surface surface) {
    if (!ctrl) return nullptr;
    for (const auto& binding : bindings)
        if (binding.key == key && binding.alt == alt && binding.shift == shift && Supports(binding, surface)) return &binding;
    return nullptr;
}
[[nodiscard]] constexpr bool MayRun(const Binding& binding, bool textInput, bool ime, bool repeat) {
    return !ime && !repeat && (!textInput || binding.allowTextInput);
}
[[nodiscard]] constexpr bool RegistryValid() {
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        const auto& binding = bindings[i];
        if (!binding.mainLabel || !binding.chord || binding.action == Action::None ||
            (Supports(binding, Surface::Reader) && !binding.readerLabel)) return false;
        for (std::size_t j = i + 1; j < bindings.size(); ++j) {
            const auto& other = bindings[j];
            if ((binding.surfaces & other.surfaces) && binding.key == other.key &&
                binding.alt == other.alt && binding.shift == other.shift) return false;
        }
    }
    return true;
}
static_assert(RegistryValid(), "Application shortcut registry must be complete and unique.");
[[nodiscard]] inline std::wstring MenuHint(std::wstring label, Action action, Surface surface) {
    const auto* binding = Find(action, surface);
    if (!binding) return label;
    const auto tab = label.find(L'\t');
    if (tab != std::wstring::npos) label.resize(tab);
    return label + L"\t" + binding->chord;
}
} // namespace application_shortcuts
