#pragma once
#include <cstdint>

namespace pdf_text_edit {

// UI-thread owned. Keeps the edit alive while a viewing gesture temporarily
// owns the viewport. No timer, document mutation or deferred focus change.
class ViewFollow {
public:
    void Reset() noexcept { *this = {}; }
    void Input(std::uint64_t now) noexcept {
        lastInput_ = now;
        active_ = true;
        suspended_ = false;
        pdfZoomLatched_ = false;
    }
    void Suspend() noexcept { suspended_ = true; }
    // Once Ctrl+Plus/Minus has zoomed the PDF, later presses keep zooming it
    // even if the edit box comes fully into view; resumed input releases it.
    void LatchPdfZoom() noexcept { pdfZoomLatched_ = true; }
    [[nodiscard]] bool PdfZoomLatched() const noexcept { return pdfZoomLatched_; }
    [[nodiscard]] bool Following(std::uint64_t now) const noexcept {
        return active_ && !suspended_ && now - lastInput_ < 10000;
    }
private:
    std::uint64_t lastInput_ = 0;
    bool active_ = false;
    bool suspended_ = false;
    bool pdfZoomLatched_ = false;
};

} // namespace pdf_text_edit
