#pragma once

#include "math/math_render.h"
#undef TRANSPARENT
#include "microtex.h"
#include "core/formula.h"

#include <mutex>
#include <vector>

namespace mathrender::detail {

struct GdiSession {
    uintptr_t token = 0;
    GdiSession();
    ~GdiSession();
    GdiSession(const GdiSession&) = delete;
    GdiSession& operator=(const GdiSession&) = delete;
};

void RegisterWindowsBackend();
void ReleaseWindowsBackend() noexcept;
[[nodiscard]] bool DrawPrepared(microtex::Render& render, HDC dc, int x, int y,
                                COLORREF colour) noexcept;

struct Runtime {
    std::recursive_mutex mutex;
    GdiSession graphics;
    std::vector<microtex::u8> fontData;
    std::string mathFont;
    Runtime();
    ~Runtime();
};

} // namespace mathrender::detail
