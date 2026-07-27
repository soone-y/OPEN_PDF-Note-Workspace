#pragma once

#include "core/app_core.h"

#include <string>
#include <vector>

namespace theme {

inline constexpr const wchar_t* kDefaultBuiltInThemeId = L"00AA7B";

std::vector<ThemeColors> MakeBuiltInThemeCatalog();
const ThemeColors* FindBuiltInThemeById(const std::vector<ThemeColors>& catalog,
                                        const std::wstring& id);
ThemeColors BuiltInThemeOrDefault(const std::wstring& id);

} // namespace theme

