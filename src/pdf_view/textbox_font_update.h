#pragma once
#include <windows.h>

// Font application uses synchronous kMsgTextBoxFontUpdate requests.
// Zero retains the legacy request to apply both font attributes.
inline constexpr WPARAM kTextBoxFontFamilyOnly = 1;
inline constexpr WPARAM kTextBoxFontSizeOnly = 2;
// Deferred dropdown refreshes may only read the current state, never apply it.
inline constexpr WPARAM kTextBoxFontDisplayOnly = 3;
