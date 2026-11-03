#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace readonly_viewer {
struct PdfTextBounds { double left = 0, top = 0, right = 0, bottom = 0; };
struct PdfRegionGlyph {
    std::uint32_t unicode = 0;
    PdfTextBounds bounds;
    bool drawable = true;
};

// Pure, UI-thread-owned selection values: no PDFium/Win32 handles or I/O.
// Vector indices identify glyphs, never UTF-8 bytes or UTF-16 code units.
class PdfRegionSelection {
public:
    static constexpr std::size_t kMaxGlyphs = 200000;
    [[nodiscard]] bool SetRegion(PdfTextBounds bounds, const std::vector<PdfRegionGlyph>& page_glyphs);
    [[nodiscard]] bool Contains(double x, double y) const noexcept;
    [[nodiscard]] bool Begin(double x, double y) noexcept;
    void Extend(double x, double y) noexcept;
    void Finish() noexcept { active_ = false; }
    void ClearText() noexcept { active_ = false; anchor_ = focus_ = 0; }
    [[nodiscard]] bool Active() const noexcept { return active_; }
    [[nodiscard]] bool HasText() const noexcept { return anchor_ != focus_; }
    [[nodiscard]] std::wstring SelectedText() const;
    [[nodiscard]] std::pair<std::size_t, std::size_t> SelectedRange() const noexcept;
    [[nodiscard]] const std::vector<PdfRegionGlyph>& Glyphs() const noexcept { return glyphs_; }
private:
    [[nodiscard]] std::size_t NearestBoundary(double x, double y, bool require_hit) const noexcept;
    PdfTextBounds bounds_;
    std::vector<PdfRegionGlyph> glyphs_;
    std::size_t anchor_ = 0, focus_ = 0;
    bool active_ = false;
};
}
