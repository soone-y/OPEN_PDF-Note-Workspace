#pragma once

#include <windows.h>
#include <string>
#include <vector>

// Selection/capture use the same virtual-key catalog and canonical file tokens.
// No UI, keyboard hooks, runtime bindings or persistence are owned here.
namespace shortcut_key_model {
struct Key {
    UINT vk;
    std::wstring token;
    const wchar_t* group;
};
[[nodiscard]] inline const std::vector<Key>& Keys() {
    static const auto keys = [] {
        std::vector<Key> result;
        for (UINT key = '0'; key <= '9'; ++key)
            result.push_back({key, std::wstring(1, static_cast<wchar_t>(key)), L"settings.shortcut.group.digits"});
        for (UINT key = 'A'; key <= 'Z'; ++key)
            result.push_back({key, std::wstring(1, static_cast<wchar_t>(key)), L"settings.shortcut.group.letters"});
        for (UINT key = VK_F1; key <= VK_F24; ++key)
            result.push_back({key, L"F" + std::to_wstring(key - VK_F1 + 1), L"settings.shortcut.group.function"});
        for (UINT key = VK_NUMPAD0; key <= VK_NUMPAD9; ++key)
            result.push_back({key, L"Numpad" + std::to_wstring(key - VK_NUMPAD0), L"settings.shortcut.group.numpad"});
        for (const auto& key : std::vector<Key>{
                 {VK_LEFT, L"Left", L"settings.shortcut.group.navigation"},
                 {VK_RIGHT, L"Right", L"settings.shortcut.group.navigation"},
                 {VK_UP, L"Up", L"settings.shortcut.group.navigation"},
                 {VK_DOWN, L"Down", L"settings.shortcut.group.navigation"},
                 {VK_SPACE, L"Space", L"settings.shortcut.group.symbols"},
                 {VK_OEM_MINUS, L"Minus", L"settings.shortcut.group.symbols"},
                 {VK_OEM_PLUS, L"Equal", L"settings.shortcut.group.symbols"},
                 {VK_OEM_COMMA, L"Comma", L"settings.shortcut.group.symbols"},
                 {VK_OEM_PERIOD, L"Period", L"settings.shortcut.group.symbols"},
                 {VK_OEM_2, L"Slash", L"settings.shortcut.group.symbols"},
                 {VK_OEM_5, L"Backslash", L"settings.shortcut.group.symbols"},
                 {VK_OEM_1, L"Semicolon", L"settings.shortcut.group.symbols"},
                 {VK_OEM_7, L"Quote", L"settings.shortcut.group.symbols"},
                 {VK_OEM_3, L"Backquote", L"settings.shortcut.group.symbols"},
                 {VK_OEM_4, L"LBracket", L"settings.shortcut.group.symbols"},
                 {VK_OEM_6, L"RBracket", L"settings.shortcut.group.symbols"}})
            result.push_back(key);
        return result;
    }();
    return keys;
}
[[nodiscard]] inline const Key* Find(UINT vk) {
    for (const auto& key : Keys()) if (key.vk == vk) return &key;
    return nullptr;
}
[[nodiscard]] inline std::wstring Format(UINT vk, bool ctrl, bool alt, bool shift) {
    const auto* key = Find(vk);
    if (!key) return {};
    std::wstring result;
    if (ctrl) result += L"Ctrl+";
    if (alt) result += L"Alt+";
    if (shift) result += L"Shift+";
    return result + key->token;
}
[[nodiscard]] inline bool IsModifier(UINT vk) {
    return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
           vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
           vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU;
}
} // namespace shortcut_key_model
