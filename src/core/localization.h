#pragma once

#include "core/theme_types.h"
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace localization {

std::wstring Text(std::wstring_view id);
std::wstring Format(std::wstring_view id,
                    const std::vector<std::pair<std::wstring_view, std::wstring>>& values);
const UiText& UserInterfaceText();
bool IsEnglishBuild();

}  // namespace localization
