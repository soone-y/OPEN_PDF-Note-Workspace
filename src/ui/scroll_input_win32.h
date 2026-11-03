#pragma once
#include "ui/scroll_input_policy.h"
#include <windows.h>

namespace scroll_input {
// Some drivers/OS versions expose touchpad origin; others expose mouse or
// unavailable. The delta/gesture fallback is still necessary in those cases.
[[nodiscard]] inline bool CurrentMessageIsTouchpad() noexcept {
    INPUT_MESSAGE_SOURCE source{};
    return GetCurrentInputMessageSource(&source) && source.deviceType == IMDT_TOUCHPAD;
}
[[nodiscard]] inline uint32_t MessageTime() noexcept {
    return static_cast<uint32_t>(GetMessageTime());
}
} // namespace scroll_input
