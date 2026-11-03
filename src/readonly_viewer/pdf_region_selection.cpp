#include "pdf_region_selection.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace readonly_viewer {
namespace {
bool Valid(const PdfTextBounds& r) {
    return std::isfinite(r.left) && std::isfinite(r.right) && std::isfinite(r.top) &&
           std::isfinite(r.bottom) && r.left < r.right && r.bottom < r.top;
}
void AppendUnicode(std::wstring& text, std::uint32_t unicode) {
    if (unicode == '\n') { text += L"\r\n"; return; }
    if (unicode > 0x10ffff || (unicode >= 0xd800 && unicode <= 0xdfff)) unicode = 0xfffd;
    if (unicode <= 0xffff) text += static_cast<wchar_t>(unicode);
    else {
        unicode -= 0x10000;
        text += static_cast<wchar_t>(0xd800 + (unicode >> 10));
        text += static_cast<wchar_t>(0xdc00 + (unicode & 0x3ff));
    }
}
}

bool PdfRegionSelection::SetRegion(PdfTextBounds bounds, const std::vector<PdfRegionGlyph>& page_glyphs) {
    if (!Valid(bounds) || page_glyphs.size() > kMaxGlyphs) return false;
    std::vector<PdfRegionGlyph> accepted;
    bool line_break = false, space = false;
    for (const auto& glyph : page_glyphs) {
        if (glyph.unicode == '\r' || glyph.unicode == '\n') { line_break = !accepted.empty(); space = false; continue; }
        if (glyph.unicode == ' ' && !Valid(glyph.bounds)) { space = !accepted.empty(); continue; }
        const auto& r = glyph.bounds;
        // A partly clipped glyph is outside the second-stage selection domain.
        if (!Valid(r) || glyph.unicode < 32 || r.left < bounds.left || r.right > bounds.right ||
            r.top > bounds.top || r.bottom < bounds.bottom) { space = false; continue; }
        if (line_break) accepted.push_back({'\n', {}, false});
        else if (space) accepted.push_back({' ', {}, false});
        line_break = space = false;
        accepted.push_back(glyph);
    }
    bounds_ = bounds;
    glyphs_ = std::move(accepted);
    ClearText();
    return true;
}

bool PdfRegionSelection::Contains(double x, double y) const noexcept {
    return std::isfinite(x) && std::isfinite(y) && x >= bounds_.left && x <= bounds_.right &&
           y >= bounds_.bottom && y <= bounds_.top;
}

std::size_t PdfRegionSelection::NearestBoundary(double x, double y, bool require_hit) const noexcept {
    std::size_t nearest = glyphs_.size();
    double distance = std::numeric_limits<double>::max();
    for (std::size_t i = 0; i < glyphs_.size(); ++i) {
        const auto& glyph = glyphs_[i];
        if (!glyph.drawable) continue;
        const auto& r = glyph.bounds;
        const double dx = x - std::clamp(x, r.left, r.right);
        const double dy = y - std::clamp(y, r.bottom, r.top);
        if (dx * dx + dy * dy < distance) { distance = dx * dx + dy * dy; nearest = i; }
    }
    if (nearest == glyphs_.size()) return std::numeric_limits<std::size_t>::max();
    const auto& r = glyphs_[nearest].bounds;
    if (require_hit && distance > std::max(4.0, (r.top - r.bottom) * (r.top - r.bottom) * 0.36))
        return std::numeric_limits<std::size_t>::max();
    const double cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    // Infer the local reading direction from neighboring glyphs. A new-line
    // separator ends the run; it must not make the last letter look right-to-left.
    for (int step : {1, -1}) {
        for (auto j = static_cast<long long>(nearest) + step;
             j >= 0 && j < static_cast<long long>(glyphs_.size()); j += step) {
            const auto& other = glyphs_[static_cast<std::size_t>(j)];
            if (other.unicode == '\n') break;
            if (!other.drawable) continue;
            const double dx = ((other.bounds.left + other.bounds.right) / 2 - cx) * step;
            const double dy = ((other.bounds.top + other.bounds.bottom) / 2 - cy) * step;
            if (std::abs(dy) <= (r.top-r.bottom)*0.6 && std::abs(dx) > 0.01)
                return nearest + ((dx > 0 ? x >= cx : x <= cx) ? 1 : 0);
            if (std::abs(dx) <= (r.right-r.left)*0.6 && std::abs(dy) > 0.01)
                return nearest + ((dy < 0 ? y <= cy : y >= cy) ? 1 : 0);
            break;
        }
    }
    return nearest + (x >= cx ? 1 : 0);
}

bool PdfRegionSelection::Begin(double x, double y) noexcept {
    if (!Contains(x, y)) return false;
    const auto boundary = NearestBoundary(x, y, true);
    // The insertion position after the final glyph is valid, distinct from no hit.
    if (boundary == std::numeric_limits<std::size_t>::max()) return false;
    anchor_ = focus_ = boundary;
    active_ = true;
    return true;
}

void PdfRegionSelection::Extend(double x, double y) noexcept {
    if (!active_ || !std::isfinite(x) || !std::isfinite(y)) return;
    focus_ = NearestBoundary(std::clamp(x, bounds_.left, bounds_.right),
                             std::clamp(y, bounds_.bottom, bounds_.top), false);
}

std::pair<std::size_t, std::size_t> PdfRegionSelection::SelectedRange() const noexcept {
    return {std::min(anchor_, focus_), std::max(anchor_, focus_)};
}

std::wstring PdfRegionSelection::SelectedText() const {
    std::wstring text;
    const auto [begin, end] = SelectedRange();
    for (auto i = begin; i < end; ++i) AppendUnicode(text, glyphs_[i].unicode);
    return text;
}
}
