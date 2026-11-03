#pragma once

#include <windows.h>
#include <cstdint>
#include <string>

namespace text_input_accessibility {

struct Snapshot {
    std::wstring name;
    std::wstring value;
    RECT screenBounds{};
    bool focused = false;
    bool readOnly = true;
    bool enabled = false;
    bool offscreen = true;
};

enum class Action { Focus, ReplaceValue };
using Read = HRESULT (*)(HWND, std::uint64_t, Snapshot*) noexcept;
using Modify = HRESULT (*)(HWND, std::uint64_t, Action, const std::wstring*) noexcept;

// Called on the window's STA thread for WM_GETOBJECT/OBJID_CLIENT. COM owns
// retained references; callbacks must reject an ended/replaced edit generation.
[[nodiscard]] LRESULT Expose(HWND window, WPARAM request, std::uint64_t generation,
                             Read read, Modify modify) noexcept;

} // namespace text_input_accessibility
