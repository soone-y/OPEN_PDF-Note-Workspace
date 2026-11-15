#pragma once

#include "core/theme_types.h"
#include <windows.h>
#include <string>
#include <vector>

namespace empty_view_guide {
// Process-local startup guidance ends when work begins, even if all documents
// are later closed. Changing the content mode does not restart guidance.
class StartupVisibility {
public:
    void Dismiss() noexcept { dismissed_ = true; }
    [[nodiscard]] bool ShouldShow(int displayMode) const noexcept {
        return !dismissed_ && displayMode != 2;
    }
private:
    bool dismissed_ = false;
};

struct Content {
    std::wstring title;
    std::wstring usage;
    std::wstring generalTitle, annotationTitle;
    std::wstring scrollHint;
    std::wstring displayHint;
    std::vector<std::wstring> general, annotation;
};
// The PDF HWND owns the scroll position. Rendering owns only temporary GDI
// objects; content and preferences remain with the calling UI thread.
void Paint(HWND owner, HDC dc, const RECT& client, const Content& content,
           HFONT font, const ThemeColors& theme);
void Clear(HWND owner, bool resetScroll = true);
[[nodiscard]] bool Scroll(HWND owner, UINT message, WPARAM wParam);
[[nodiscard]] std::wstring PlainText(const Content& content);
}
