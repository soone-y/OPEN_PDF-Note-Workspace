#pragma once
#include <windows.h>
#include "fpdf_progressive.h"
#include <mutex>

namespace pdf_worker_render {
// The page/bitmap belong exclusively to this worker. All PDFium calls stay
// serialized, but the caller's engine lock is released between continuations.
// Pause is cooperative: an individual PDFium call has no hard time bound.
template<class Canceled, class Yield>
[[nodiscard]] bool Render(std::unique_lock<std::recursive_mutex>& engine,
    FPDF_BITMAP bitmap, FPDF_PAGE page, int width, int height, int rotation, int flags,
    Canceled canceled, Yield yield, unsigned sliceMilliseconds = 8) {
    struct Pause {
        IFSDK_PAUSE api{};
        ULONGLONG deadline = 0;
        Canceled& canceled;
        explicit Pause(Canceled& callback) : canceled(callback) {
            api.version = 1; api.user = this;
            api.NeedToPauseNow = [](IFSDK_PAUSE* self) -> FPDF_BOOL {
                const auto& state = *static_cast<Pause*>(self->user);
                return state.canceled() || GetTickCount64() >= state.deadline;
            };
        }
    } pause(canceled);
    if (canceled()) return false;
    struct Close {
        FPDF_PAGE page;
        ~Close() { FPDF_RenderPage_Close(page); }
    } close{page}; // caller keeps/reacquires engine lock before unwinding
    pause.deadline = GetTickCount64() + sliceMilliseconds;
    int status = FPDF_RenderPageBitmap_Start(bitmap, page, 0, 0, width, height, rotation, flags, &pause.api);
    while (status == FPDF_RENDER_TOBECONTINUED) {
        engine.unlock();
        try { yield(); } catch (...) { engine.lock(); throw; }
        engine.lock();
        if (canceled()) return false;
        pause.deadline = GetTickCount64() + sliceMilliseconds;
        status = FPDF_RenderPage_Continue(page, &pause.api);
    }
    return status == FPDF_RENDER_DONE && !canceled();
}
} // namespace pdf_worker_render
