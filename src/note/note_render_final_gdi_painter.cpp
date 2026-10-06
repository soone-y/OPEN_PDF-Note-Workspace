#include "note/note_render_final_gdi_painter.h"

#include "note/note_render_final_presentation_interaction.h"

#include "note/note_render_final_display_run.h"
#include "note/note_render_final_style.h"
#include "math/math_render.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace note {
namespace {

[[nodiscard]] bool HasStyle(const NoteRenderSourceRun& run, StyleKind kind) noexcept {
    return std::any_of(run.styles.begin(), run.styles.end(),
                       [kind](const NoteRenderSourceStyleAttribute& style) {
                           return style.kind == kind;
                       });
}

enum class FinalStyleColorMode : uint8_t {
    Inherit,
    Explicit,
    HighContrast,
};

struct FinalStyleColor {
    FinalStyleColorMode mode = FinalStyleColorMode::Inherit;
    COLORREF value = RGB(0, 0, 0);
};

struct FinalPaintRunStyle {
    COLORREF text = RGB(0, 0, 0);
    COLORREF background = RGB(255, 255, 255);
    bool has_background = false;
    bool underline = false;
    bool strike = false;
};

[[nodiscard]] bool AsciiEqualsIgnoreCase(std::wstring_view lhs,
                                          std::wstring_view rhs) noexcept {
    if (lhs.size() != rhs.size()) return false;
    for (size_t index = 0; index < lhs.size(); ++index) {
        wchar_t a = lhs[index];
        wchar_t b = rhs[index];
        if (a >= L'A' && a <= L'Z') a = static_cast<wchar_t>(a - L'A' + L'a');
        if (b >= L'A' && b <= L'Z') b = static_cast<wchar_t>(b - L'A' + L'a');
        if (a != b) return false;
    }
    return true;
}

[[nodiscard]] std::wstring_view TrimAsciiWhitespace(std::wstring_view value) noexcept {
    while (!value.empty() && (value.front() == L' ' || value.front() == L'\t' ||
                              value.front() == L'\r' || value.front() == L'\n')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == L' ' || value.back() == L'\t' ||
                              value.back() == L'\r' || value.back() == L'\n')) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] int HexDigit(wchar_t ch) noexcept {
    if (ch >= L'0' && ch <= L'9') return ch - L'0';
    if (ch >= L'a' && ch <= L'f') return 10 + ch - L'a';
    if (ch >= L'A' && ch <= L'F') return 10 + ch - L'A';
    return -1;
}

[[nodiscard]] FinalStyleColor ParseStyleColor(std::wstring_view value) noexcept {
    value = TrimAsciiWhitespace(value);
    if (AsciiEqualsIgnoreCase(value, L"hicont") ||
        AsciiEqualsIgnoreCase(value, L"hi-contrast") ||
        AsciiEqualsIgnoreCase(value, L"highcontrast") ||
        AsciiEqualsIgnoreCase(value, L"high-contrast")) {
        return {FinalStyleColorMode::HighContrast, RGB(0, 0, 0)};
    }
    if (value.size() >= 2 && value[0] == L'0' && (value[1] == L'x' || value[1] == L'X')) {
        value.remove_prefix(2);
    }
    if (!value.empty() && value.front() == L'#') value.remove_prefix(1);
    if (value.size() != 6) return {};
    const int r1 = HexDigit(value[0]);
    const int r2 = HexDigit(value[1]);
    const int g1 = HexDigit(value[2]);
    const int g2 = HexDigit(value[3]);
    const int b1 = HexDigit(value[4]);
    const int b2 = HexDigit(value[5]);
    if (r1 < 0 || r2 < 0 || g1 < 0 || g2 < 0 || b1 < 0 || b2 < 0) return {};
    return {FinalStyleColorMode::Explicit,
            RGB(r1 * 16 + r2, g1 * 16 + g2, b1 * 16 + b2)};
}

[[nodiscard]] int Luma(COLORREF colour) noexcept {
    return (GetRValue(colour) * 299 + GetGValue(colour) * 587 + GetBValue(colour) * 114) /
        1000;
}

[[nodiscard]] COLORREF HighContrastText(COLORREF background) noexcept {
    return Luma(background) >= 128 ? RGB(0, 0, 0) : RGB(255, 255, 255);
}

[[nodiscard]] COLORREF HighContrastBackground(COLORREF foreground) noexcept {
    return Luma(foreground) >= 128 ? RGB(0, 0, 0) : RGB(255, 255, 255);
}

[[nodiscard]] FinalPaintRunStyle ResolvePaintRunStyle(
    const NoteRenderSourceRun& run,
    const NoteRenderFinalGdiPaintOptions& options) noexcept {
    FinalStyleColor text;
    FinalStyleColor background;
    for (const NoteRenderSourceStyleAttribute& attribute : run.styles) {
        if (attribute.kind == StyleKind::TextColor) {
            text = ParseStyleColor(attribute.value);
        } else if (attribute.kind == StyleKind::BackgroundColor) {
            background = ParseStyleColor(attribute.value);
        }
    }
    FinalPaintRunStyle resolved;
    resolved.text = options.theme.text;
    resolved.background = options.theme.background;
    resolved.has_background = background.mode != FinalStyleColorMode::Inherit;
    if (text.mode == FinalStyleColorMode::Explicit) resolved.text = text.value;
    if (background.mode == FinalStyleColorMode::Explicit) resolved.background = background.value;
    if (text.mode == FinalStyleColorMode::HighContrast &&
        background.mode == FinalStyleColorMode::HighContrast) {
        resolved.background = GetSysColor(COLOR_HIGHLIGHT);
        resolved.text = GetSysColor(COLOR_HIGHLIGHTTEXT);
        resolved.has_background = true;
    } else if (background.mode == FinalStyleColorMode::HighContrast) {
        resolved.background = HighContrastBackground(
            text.mode == FinalStyleColorMode::Explicit ? text.value : options.theme.background);
        resolved.text = text.mode == FinalStyleColorMode::Explicit
            ? text.value : HighContrastText(resolved.background);
        resolved.has_background = true;
    } else if (text.mode == FinalStyleColorMode::HighContrast) {
        resolved.text = HighContrastText(resolved.background);
    }
    if (run.kind == NoteRenderSourceRunKind::LinkText ||
        HasStyle(run, StyleKind::LinkAccent)) {
        resolved.text = options.theme.link;
    }
    resolved.underline = HasStyle(run, StyleKind::Underline) ||
        HasStyle(run, StyleKind::LinkUnderline);
    resolved.strike = HasStyle(run, StyleKind::Strike);
    return resolved;
}

class ScopedPaintFont final {
public:
    ScopedPaintFont(HDC hdc, const NoteRenderSourceLinePlan& line,
                    const NoteRenderSourceRun& run) noexcept
        : hdc_(hdc) {
        if (!hdc_) return;
        old_ = GetCurrentObject(hdc_, OBJ_FONT);
        if (!old_) return;
        LOGFONTW font{};
        if (GetObjectW(old_, sizeof(font), &font) != sizeof(font)) return;
        TEXTMETRICW baseMetrics{};
        if (!GetTextMetricsW(hdc_, &baseMetrics)) return;
        NoteRenderFinalRunFontStyle style;
        if (!ResolveNoteRenderFinalRunFontStyle(
                line, run, std::max<LONG>(1, baseMetrics.tmHeight), &style)) {
            return;
        }
        const bool needsFont = style.bold || style.italic || !style.font_family.empty() ||
            style.font_height_px != std::max<LONG>(1, baseMetrics.tmHeight);
        if (!needsFont) {
            valid_ = true;
            return;
        }
        if (style.bold) font.lfWeight = FW_BOLD;
        if (style.italic) font.lfItalic = TRUE;
        font.lfHeight = -static_cast<LONG>(style.font_height_px);
        if (!style.font_family.empty()) {
            std::copy(style.font_family.begin(), style.font_family.end(), font.lfFaceName);
            font.lfFaceName[style.font_family.size()] = L'\0';
        }
        owned_ = CreateFontIndirectW(&font);
        if (!owned_ || SelectObject(hdc_, owned_) == HGDI_ERROR) return;
        selected_ = true;
        valid_ = true;
    }

    ~ScopedPaintFont() {
        if (selected_ && hdc_ && old_) (void)SelectObject(hdc_, old_);
        if (owned_) (void)DeleteObject(owned_);
    }

    ScopedPaintFont(const ScopedPaintFont&) = delete;
    ScopedPaintFont& operator=(const ScopedPaintFont&) = delete;

    [[nodiscard]] bool valid() const noexcept { return valid_; }

private:
    HDC hdc_ = nullptr;
    HGDIOBJ old_ = nullptr;
    HFONT owned_ = nullptr;
    bool selected_ = false;
    bool valid_ = false;
};

[[nodiscard]] bool IsCommittedOwner(const NoteRenderFinalPublication& publication,
                                    LineIndex line) noexcept {
    const auto& ranges = publication.owner_plan()->owner_ranges();
    for (const NotePresentationOwnerRange& range : ranges) {
        if (line < range.lines.first || line >= range.lines.last_exclusive) continue;
        return range.owner == NotePresentationLineOwner::CommittedPlacement;
    }
    return false;
}

[[nodiscard]] bool ContentYToClient(uint64_t contentY,
                                    uint64_t scrollY,
                                    int* outY) noexcept {
    if (!outY) return false;
    if (contentY >= scrollY) {
        const uint64_t relative = contentY - scrollY;
        if (relative > static_cast<uint64_t>(std::numeric_limits<int>::max())) return false;
        *outY = static_cast<int>(relative);
        return true;
    }
    const uint64_t preceding = scrollY - contentY;
    if (preceding > static_cast<uint64_t>(std::numeric_limits<int>::max())) return false;
    *outY = -static_cast<int>(preceding);
    return true;
}

[[nodiscard]] bool ContentXToClient(int contentX,
                                    int scrollX,
                                    int* outX) noexcept {
    if (!outX) return false;
    const int64_t translated = static_cast<int64_t>(contentX) -
        static_cast<int64_t>(scrollX);
    if (translated < std::numeric_limits<int>::min() ||
        translated > std::numeric_limits<int>::max()) {
        return false;
    }
    *outX = static_cast<int>(translated);
    return true;
}

[[nodiscard]] bool AddContentX(int left, int width, int* outRight) noexcept {
    if (!outRight) return false;
    const int64_t right = static_cast<int64_t>(left) + static_cast<int64_t>(width);
    if (right < std::numeric_limits<int>::min() ||
        right > std::numeric_limits<int>::max()) {
        return false;
    }
    *outRight = static_cast<int>(right);
    return true;
}

[[nodiscard]] bool TranslateRect(int left, uint64_t top, int right, uint64_t bottom,
                                 const NoteRenderFinalGdiPaintOptions& options,
                                 RECT* out) noexcept {
    if (!out || right <= left || bottom <= top) {
        return false;
    }
    int translatedLeft = 0;
    int translatedRight = 0;
    int translatedTop = 0;
    int translatedBottom = 0;
    if (!ContentXToClient(left, options.horizontal_scroll_px, &translatedLeft) ||
        !ContentXToClient(right, options.horizontal_scroll_px, &translatedRight) ||
        translatedRight <= translatedLeft ||
        !ContentYToClient(top, options.vertical_scroll_px, &translatedTop) ||
        !ContentYToClient(bottom, options.vertical_scroll_px, &translatedBottom) ||
        translatedBottom <= translatedTop) {
        return false;
    }
    *out = {translatedLeft, translatedTop, translatedRight, translatedBottom};
    return true;
}

[[nodiscard]] bool FillContentRect(HDC hdc, int left, uint64_t top, int right, uint64_t bottom,
                                   COLORREF colour,
                                   const NoteRenderFinalGdiPaintOptions& options) noexcept {
    RECT rect{};
    if (!TranslateRect(left, top, right, bottom, options, &rect)) return true;
    HBRUSH brush = CreateSolidBrush(colour);
    if (!brush) return false;
    const int result = FillRect(hdc, &rect, brush);
    (void)DeleteObject(brush);
    return result != 0;
}

[[nodiscard]] bool PaintRunBackground(
    HDC hdc,
    const NoteRenderRunPlacement& placement,
    uint64_t lineTop,
    const FinalPaintRunStyle& style,
    const NoteRenderFinalGdiPaintOptions& options) noexcept {
    if (!style.has_background) return true;
    for (const NoteRenderRunPlacementFragment& fragment : placement.fragments) {
        if (fragment.width_px <= 0 || fragment.height_px == 0 ||
            fragment.x_px > std::numeric_limits<int>::max() - fragment.width_px ||
            lineTop > std::numeric_limits<uint64_t>::max() - fragment.top_offset_px ||
            lineTop + fragment.top_offset_px > std::numeric_limits<uint64_t>::max() -
                fragment.height_px) {
            return false;
        }
        if (!FillContentRect(hdc, fragment.x_px, lineTop + fragment.top_offset_px,
                             fragment.x_px + fragment.width_px,
                             lineTop + fragment.top_offset_px + fragment.height_px,
                             style.background, options)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool PaintRunTextDecorations(
    HDC hdc,
    const NoteRenderRunPlacement& placement,
    uint64_t lineTop,
    const FinalPaintRunStyle& style,
    const NoteRenderFinalGdiPaintOptions& options) noexcept {
    if (!style.underline && !style.strike) return true;
    for (const NoteRenderRunPlacementFragment& fragment : placement.fragments) {
        if (fragment.width_px <= 0 || fragment.height_px == 0 ||
            fragment.x_px > std::numeric_limits<int>::max() - fragment.width_px ||
            lineTop > std::numeric_limits<uint64_t>::max() - fragment.top_offset_px) {
            return false;
        }
        const uint64_t top = lineTop + fragment.top_offset_px;
        if (top > std::numeric_limits<uint64_t>::max() - fragment.height_px) return false;
        const int right = fragment.x_px + fragment.width_px;
        if (style.underline &&
            !FillContentRect(hdc, fragment.x_px, top + fragment.height_px - 1,
                             right, top + fragment.height_px, style.text, options)) {
            return false;
        }
        if (style.strike) {
            const uint64_t strikeTop = top + (fragment.height_px - 1) / 2;
            if (!FillContentRect(hdc, fragment.x_px, strikeTop, right, strikeTop + 1,
                                 style.text, options)) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool DisplayOffsetsForFragment(
    const NoteRenderFinalDisplayRun& display,
    const NoteRenderRunPlacement& placement,
    size_t fragmentIndex,
    size_t* outStart,
    size_t* outEnd,
    std::vector<int>* outX) noexcept {
    if (!outStart || !outEnd || !outX || fragmentIndex >= placement.fragments.size()) return false;
    const NoteRenderRunPlacementFragment& fragment = placement.fragments[fragmentIndex];
    try {
        std::vector<int> xByDisplay(display.display_text.size() + 1,
                                    std::numeric_limits<int>::min());
        std::optional<size_t> start;
        std::optional<size_t> end;
        for (const NoteRenderFinalDisplayBoundary& displayBoundary : display.boundaries) {
            if (displayBoundary.source_offset < fragment.source_span.start ||
                displayBoundary.source_offset > fragment.source_span.end ||
                displayBoundary.display_offset >= xByDisplay.size()) {
                continue;
            }
            for (const NoteRenderPlacementBoundary& placementBoundary : placement.boundaries) {
                if (placementBoundary.fragment_index != fragmentIndex ||
                    placementBoundary.source_offset != displayBoundary.source_offset) {
                    continue;
                }
                int& value = xByDisplay[displayBoundary.display_offset];
                if (value != std::numeric_limits<int>::min() && value != placementBoundary.x_px) {
                    return false;
                }
                value = placementBoundary.x_px;
                if (displayBoundary.source_offset == fragment.source_span.start) {
                    start = start.has_value() ? std::min(*start, displayBoundary.display_offset)
                                              : displayBoundary.display_offset;
                }
                if (displayBoundary.source_offset == fragment.source_span.end) {
                    end = end.has_value() ? std::max(*end, displayBoundary.display_offset)
                                          : displayBoundary.display_offset;
                }
            }
        }
        if (!start.has_value() || !end.has_value() || *end < *start) return false;
        for (size_t offset = *start; offset <= *end; ++offset) {
            if (xByDisplay[offset] == std::numeric_limits<int>::min()) return false;
            if (offset == std::numeric_limits<size_t>::max()) return false;
        }
        *outStart = *start;
        *outEnd = *end;
        *outX = std::move(xByDisplay);
        return true;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool DrawTextFragment(HDC hdc,
                                    const NoteRenderFinalDisplayRun& display,
                                    const NoteRenderRunPlacement& placement,
                                    size_t fragmentIndex,
                                    uint64_t contentTop,
                                    COLORREF colour,
                                    const NoteRenderFinalGdiPaintOptions& options) noexcept {
    if (!hdc || fragmentIndex >= placement.fragments.size()) return false;
    const NoteRenderRunPlacementFragment& fragment = placement.fragments[fragmentIndex];
    if (display.display_text.empty()) return true;
    size_t start = 0;
    size_t end = 0;
    std::vector<int> xByDisplay;
    if (!DisplayOffsetsForFragment(display, placement, fragmentIndex, &start, &end, &xByDisplay) ||
        end > display.display_text.size()) {
        return false;
    }
    if (contentTop > std::numeric_limits<uint64_t>::max() - fragment.top_offset_px) return false;
    int y = 0;
    if (!ContentYToClient(contentTop + fragment.top_offset_px, options.vertical_scroll_px, &y)) {
        return true;
    }
    const UINT oldAlign = SetTextAlign(hdc, TA_LEFT | TA_TOP | TA_NOUPDATECP);
    const int oldBk = SetBkMode(hdc, TRANSPARENT);
    const COLORREF oldColour = SetTextColor(hdc, colour);
    bool ok = true;
    size_t cursor = start;
    while (cursor < end) {
        if (display.display_text[cursor] == L'\t') {
            ++cursor;
            continue;
        }
        size_t segmentEnd = cursor + 1;
        while (segmentEnd < end && display.display_text[segmentEnd] != L'\t') ++segmentEnd;
        int clientX = 0;
        if (segmentEnd - cursor > static_cast<size_t>(std::numeric_limits<UINT>::max()) ||
            !ContentXToClient(xByDisplay[cursor], options.horizontal_scroll_px, &clientX)) {
            ok = false;
            break;
        }
        std::vector<int> advances;
        advances.reserve(segmentEnd - cursor);
        for (size_t offset = cursor; offset < segmentEnd; ++offset) {
            const int64_t advance = static_cast<int64_t>(xByDisplay[offset + 1]) - xByDisplay[offset];
            if (advance < std::numeric_limits<int>::min() ||
                advance > std::numeric_limits<int>::max()) {
                ok = false;
                break;
            }
            advances.push_back(static_cast<int>(advance));
        }
        if (!ok || !ExtTextOutW(hdc, clientX, y, 0,
                                 nullptr, display.display_text.data() + cursor,
                                 static_cast<UINT>(segmentEnd - cursor), advances.data())) {
            ok = false;
            break;
        }
        cursor = segmentEnd;
    }
    (void)SetTextColor(hdc, oldColour);
    (void)SetBkMode(hdc, oldBk);
    (void)SetTextAlign(hdc, oldAlign);
    return ok;
}

[[nodiscard]] bool DrawMathFragment(HDC hdc,
                                    const NoteRenderFinalDisplayRun& display,
                                    const NoteRenderRunPlacementFragment& fragment,
                                    uint64_t contentTop,
                                    int baseline,
                                    mathrender::RenderStyle style,
                                    COLORREF colour,
                                    const NoteRenderFinalGdiPaintOptions& options) noexcept {
    int clientX = 0;
    if (!hdc || display.display_text.empty() || fragment.width_px <= 0 || fragment.height_px == 0 ||
        baseline < 0 || static_cast<uint32_t>(baseline) > fragment.height_px ||
        contentTop > std::numeric_limits<uint64_t>::max() - fragment.top_offset_px ||
        !ContentXToClient(fragment.x_px, options.horizontal_scroll_px, &clientX)) {
        return false;
    }
    int y = 0;
    if (!ContentYToClient(contentTop + fragment.top_offset_px, options.vertical_scroll_px, &y)) {
        return true;
    }
    try {
        std::unique_ptr<mathrender::Node> node = mathrender::Parse(display.display_text);
        if (!node) return false;
        TEXTMETRICW metrics{};
        if (!GetTextMetricsW(hdc, &metrics)) return false;
        const mathrender::Layout layout{fragment.width_px, static_cast<int>(fragment.height_px), baseline};
        mathrender::Draw(*node, layout, hdc, clientX, y,
                         std::max<LONG>(1, metrics.tmHeight), colour, style);
        return true;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool PaintDecorationBackground(HDC hdc,
                                              const NoteRenderVisualDecoration& decoration,
                                              uint64_t lineTop,
                                              const NoteRenderFinalGdiPaintOptions& options) noexcept {
    if (lineTop > std::numeric_limits<uint64_t>::max() - decoration.top_offset_px ||
        lineTop + decoration.top_offset_px > std::numeric_limits<uint64_t>::max() -
            (decoration.bottom_offset_px - decoration.top_offset_px)) {
        return false;
    }
    const uint64_t top = lineTop + decoration.top_offset_px;
    const uint64_t bottom = lineTop + decoration.bottom_offset_px;
    switch (decoration.kind) {
    case NoteRenderVisualDecorationKind::CodeBlockSurface:
        return FillContentRect(hdc, decoration.left_px, top, decoration.right_px, bottom,
                               options.theme.code_surface, options);
    case NoteRenderVisualDecorationKind::ContainerSurface:
        return FillContentRect(hdc, decoration.left_px, top, decoration.right_px, bottom,
                               options.theme.container_surface, options);
    case NoteRenderVisualDecorationKind::ContainerBorder:
        return FillContentRect(hdc, decoration.left_px, top, decoration.right_px, bottom,
                               options.theme.container_border, options);
    case NoteRenderVisualDecorationKind::InlineCodeSurface:
        return FillContentRect(hdc, decoration.left_px, top, decoration.right_px, bottom,
                               options.theme.inline_code_surface, options);
    case NoteRenderVisualDecorationKind::QuoteBar:
        return FillContentRect(hdc, decoration.left_px, top, decoration.right_px, bottom,
                               options.theme.quote_bar, options);
    case NoteRenderVisualDecorationKind::HorizontalRule:
        return FillContentRect(hdc, decoration.left_px, top, decoration.right_px, bottom,
                               options.theme.rule, options);
    default:
        return true;
    }
}

[[nodiscard]] bool PaintDecorationForeground(HDC hdc,
                                              const NoteRenderVisualDecoration& decoration,
                                              uint64_t lineTop,
                                              const NoteRenderFinalGdiPaintOptions& options) noexcept {
    if (decoration.kind != NoteRenderVisualDecorationKind::ListMarker &&
        decoration.kind != NoteRenderVisualDecorationKind::TaskCheckbox) {
        return true;
    }
    if (lineTop > std::numeric_limits<uint64_t>::max() - decoration.top_offset_px) return false;
    int top = 0;
    int bottom = 0;
    if (!ContentYToClient(lineTop + decoration.top_offset_px, options.vertical_scroll_px, &top) ||
        !ContentYToClient(lineTop + decoration.bottom_offset_px, options.vertical_scroll_px, &bottom)) {
        return true;
    }
    int left = 0;
    int right = 0;
    if (!ContentXToClient(decoration.left_px, options.horizontal_scroll_px, &left) ||
        !ContentXToClient(decoration.right_px, options.horizontal_scroll_px, &right) ||
        right <= left || bottom <= top) return false;
    if (decoration.kind == NoteRenderVisualDecorationKind::ListMarker) {
        RECT rect{left, top, right, bottom};
        const int oldBk = SetBkMode(hdc, TRANSPARENT);
        const COLORREF oldColour = SetTextColor(hdc, options.theme.text);
        const int result = DrawTextW(hdc, decoration.text.c_str(), static_cast<int>(decoration.text.size()),
                                     &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        (void)SetTextColor(hdc, oldColour);
        (void)SetBkMode(hdc, oldBk);
        return result != 0;
    }
    HPEN pen = CreatePen(PS_SOLID, 1, options.theme.text);
    HBRUSH brush = static_cast<HBRUSH>(GetStockObject(HOLLOW_BRUSH));
    if (!pen || !brush) {
        if (pen) (void)DeleteObject(pen);
        return false;
    }
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    bool painted = Rectangle(hdc, left, top, right, bottom) != FALSE;
    const int64_t side = std::min(int64_t{right} - left, int64_t{bottom} - top);
    if (painted && decoration.checked && side >= 7) {
        // Keep the tick clear of the border; all points stay inside the measured box.
        const POINT tick[] = {
            {left + static_cast<int>(side / 4), top + static_cast<int>(side / 2)},
            {left + static_cast<int>(side * 2 / 5), top + static_cast<int>(side * 3 / 4)},
            {left + static_cast<int>(side * 3 / 4), top + static_cast<int>(side / 4)}};
        painted = Polyline(hdc, tick, 3) != FALSE;
    } else if (painted && decoration.checked) {
        // Very small fonts still distinguish checked from unchecked without crossing the border.
        painted = SetPixelV(hdc, left + static_cast<int>(side / 2),
                            top + static_cast<int>(side / 2), options.theme.text) != FALSE;
    }
    if (oldBrush && oldBrush != HGDI_ERROR) (void)SelectObject(hdc, oldBrush);
    if (oldPen && oldPen != HGDI_ERROR) (void)SelectObject(hdc, oldPen);
    (void)DeleteObject(pen);
    return painted;
}

[[nodiscard]] bool PaintTableGrid(HDC hdc,
                                  const NoteRenderAtomicGroupPlacement& group,
                                  uint64_t groupTop,
                                  const NoteRenderFinalGdiPaintOptions& options) noexcept {
    if (group.kind != NoteRenderAtomicGroupKind::Table || group.table_columns.empty() ||
        group.table_rows.empty() || group.width_px <= 0 || group.height_px == 0) {
        return false;
    }
    int right = 0;
    if (!AddContentX(group.x_px, group.width_px, &right)) return false;
    if (groupTop > std::numeric_limits<uint64_t>::max() - group.height_px) return false;
    for (const NoteRenderTableRowPlacement& row : group.table_rows) {
        if (groupTop > std::numeric_limits<uint64_t>::max() - row.top_offset_px ||
            groupTop + row.top_offset_px > std::numeric_limits<uint64_t>::max() - row.height_px) {
            return false;
        }
        if (!row.header || row.divider) {
            continue;
        }
        if (!FillContentRect(hdc, group.x_px, groupTop + row.top_offset_px,
                             right,
                             groupTop + row.top_offset_px + row.height_px,
                             options.theme.table_header_surface, options)) {
            return false;
        }
    }
    const int left = group.x_px;
    if (!FillContentRect(hdc, left, groupTop, right, groupTop + 1,
                         options.theme.table_border, options) ||
        !FillContentRect(hdc, left, groupTop + group.height_px - 1, right,
                         groupTop + group.height_px, options.theme.table_border, options)) {
        return false;
    }
    for (const NoteRenderTableRowPlacement& row : group.table_rows) {
        const uint64_t y = groupTop + row.top_offset_px;
        if (!FillContentRect(hdc, left, y, right, y + 1, options.theme.table_border, options) ||
            !FillContentRect(hdc, left, y + row.height_px - 1, right, y + row.height_px,
                             options.theme.table_border, options)) {
            return false;
        }
    }
    for (const NoteRenderTableColumnPlacement& column : group.table_columns) {
        int leftBorder = 0;
        int leftBorderRight = 0;
        int rightBorder = 0;
        int rightBorderRight = 0;
        if (!AddContentX(group.x_px, column.left_border_x_px, &leftBorder) ||
            !AddContentX(leftBorder, 1, &leftBorderRight) ||
            !AddContentX(group.x_px, column.right_border_x_px, &rightBorder) ||
            !AddContentX(rightBorder, 1, &rightBorderRight) ||
            !FillContentRect(hdc, leftBorder, groupTop, leftBorderRight,
                             groupTop + group.height_px, options.theme.table_border, options) ||
            !FillContentRect(hdc, rightBorder, groupTop, rightBorderRight,
                             groupTop + group.height_px, options.theme.table_border, options)) {
            return false;
        }
    }
    return true;
}

// A table grid spans several rows. Paint its enclosing surfaces before the
// shared grid, never afterward row by row (which would erase its borders).
[[nodiscard]] bool PaintTableContainerBackgrounds(
    HDC hdc,
    const NoteRenderPlacementSnapshot& placement,
    const NoteRenderLineLayoutMap::Snapshot& layouts,
    const NoteRenderAtomicGroupPlacement& group,
    const NoteRenderFinalGdiPaintOptions& options) noexcept {
    NoteRenderLinePlacement anchor;
    if (!placement.ResolveLine(group.first_line, &anchor)) return false;
    if (std::none_of(anchor.decorations.begin(), anchor.decorations.end(), [](const auto& item) {
            return item.kind == NoteRenderVisualDecorationKind::ContainerSurface;
        })) return true;
    RECT clip{};
    const int clipKind = GetClipBox(hdc, &clip);
    if (clipKind == NULLREGION) return true;
    if (clipKind == ERROR || clip.top < 0 || clip.bottom <= clip.top ||
        options.vertical_scroll_px > std::numeric_limits<uint64_t>::max() -
            static_cast<uint64_t>(clip.bottom)) return false;
    const auto first = NoteRenderLineLayoutMap::LineContainingY(
        layouts, options.vertical_scroll_px + static_cast<uint64_t>(clip.top));
    if (!first) return true;
    const uint64_t lastY = options.vertical_scroll_px + static_cast<uint64_t>(clip.bottom);
    for (size_t line = std::max(group.first_line.value, first->line_index.value);
         line <= group.last_line.value; ++line) {
        NoteRenderLinePlacement row;
        const auto layout = NoteRenderLineLayoutMap::LineAt(layouts, {line});
        if (!layout || !placement.ResolveLine({line}, &row)) return false;
        if (layout->top_px >= lastY) break;
        for (const NoteRenderVisualDecoration& decoration : row.decorations) {
            if (decoration.kind != NoteRenderVisualDecorationKind::ContainerSurface &&
                decoration.kind != NoteRenderVisualDecorationKind::ContainerBorder) continue;
            if (!PaintDecorationBackground(hdc, decoration, layout->top_px, options)) return false;
        }
        if (line == std::numeric_limits<size_t>::max()) return false;
    }
    return true;
}

[[nodiscard]] bool PaintSelectionForLine(
    HDC hdc,
    const std::vector<NoteRenderFinalRect>& selectionRects,
    const NoteRenderLineLayoutLocation& layout,
    const NoteRenderFinalGdiPaintOptions& options) noexcept {
    for (const NoteRenderFinalRect& rect : selectionRects) {
        // ResolveNoteRenderFinalSelection emits fragment rectangles, but keep
        // the line-band check at this Win32 edge so a malformed adapter input
        // cannot paint into an unrelated published row.
        if (rect.right_px <= rect.left_px || rect.bottom_px <= rect.top_px) {
            return false;
        }
        if (rect.bottom_px <= layout.top_px || rect.top_px >= layout.bottom_px) continue;
        if (!FillContentRect(hdc, rect.left_px, rect.top_px, rect.right_px, rect.bottom_px,
                             options.theme.selection_surface, options)) return false;
    }
    return true;
}

[[nodiscard]] bool PaintStructuredResolvedLine(
    HDC hdc,
    const NoteSyntaxSnapshot& syntax,
    const NoteRenderSourceLinePlan& sourceLine,
    const NoteRenderLinePlacement& placementLine,
    const NoteRenderLineLayoutLocation& layout,
    const std::vector<NoteRenderFinalRect>& selectionRects,
    const NoteRenderFinalGdiPaintOptions& options) noexcept {
    if (sourceLine.runs.size() != placementLine.runs.size()) return false;
    for (const NoteRenderVisualDecoration& decoration : placementLine.decorations) {
        if (sourceLine.decoration.has(NoteRenderSourceLineDecorationTable) &&
            (decoration.kind == NoteRenderVisualDecorationKind::ContainerSurface ||
             decoration.kind == NoteRenderVisualDecorationKind::ContainerBorder)) continue;
        if (!PaintDecorationBackground(hdc, decoration, layout.top_px, options)) return false;
    }
    // A source style background is part of the same run geometry as its
    // glyphs. Paint it after container/chip surfaces but before selection so
    // selected text keeps the normal selection surface rather than becoming
    // an opaque second background.
    for (size_t runIndex = 0; runIndex < sourceLine.runs.size(); ++runIndex) {
        const FinalPaintRunStyle style = ResolvePaintRunStyle(
            sourceLine.runs[runIndex], options);
        if (!PaintRunBackground(hdc, placementLine.runs[runIndex], layout.top_px,
                                style, options)) {
            return false;
        }
    }
    if (!PaintSelectionForLine(hdc, selectionRects, layout, options)) return false;
    for (size_t runIndex = 0; runIndex < sourceLine.runs.size(); ++runIndex) {
        const NoteRenderSourceRun& sourceRun = sourceLine.runs[runIndex];
        const NoteRenderRunPlacement& placement = placementLine.runs[runIndex];
        const FinalPaintRunStyle style = ResolvePaintRunStyle(sourceRun, options);
        NoteRenderFinalDisplayRun display;
        if (BuildNoteRenderFinalDisplayRun(syntax, sourceRun, &display) !=
            NoteRenderFinalDisplayRunBuildResult::Built) {
            return false;
        }
        ScopedPaintFont font(hdc, sourceLine, sourceRun);
        if (!font.valid()) return false;
        for (size_t fragmentIndex = 0; fragmentIndex < placement.fragments.size(); ++fragmentIndex) {
            const NoteRenderRunPlacementFragment& fragment = placement.fragments[fragmentIndex];
            const bool math = sourceRun.kind == NoteRenderSourceRunKind::InlineMath ||
                sourceRun.kind == NoteRenderSourceRunKind::BlockMath;
            const bool drawn = math
                ? DrawMathFragment(hdc, display, fragment, layout.top_px,
                                   fragment.baseline_offset_px,
                                   sourceRun.kind == NoteRenderSourceRunKind::BlockMath
                                       ? mathrender::RenderStyle::Display
                                       : mathrender::RenderStyle::Inline,
                                   style.text, options)
                : DrawTextFragment(hdc, display, placement, fragmentIndex, layout.top_px,
                                   style.text, options);
            if (!drawn) return false;
        }
        if (!PaintRunTextDecorations(hdc, placement, layout.top_px, style, options)) {
            return false;
        }
    }
    for (const NoteRenderVisualDecoration& decoration : placementLine.decorations) {
        if (!PaintDecorationForeground(hdc, decoration, layout.top_px, options)) return false;
    }
    return true;
}

[[nodiscard]] bool PaintLine(HDC hdc,
                              const NoteRenderFinalPublication& publication,
                              LineIndex lineIndex,
                              const NoteRenderLineLayoutLocation& layout,
                              const std::vector<NoteRenderFinalRect>& selectionRects,
                              const NoteRenderFinalGdiPaintOptions& options) noexcept {
    NoteRenderSourceLinePlan sourceLine;
    NoteRenderLinePlacement placementLine;
    if (!publication.source_plan()->ResolveLine(lineIndex, &sourceLine) ||
        !publication.placement()->ResolveLine(lineIndex, &placementLine) ||
        !publication.syntax()) {
        return false;
    }
    return PaintStructuredResolvedLine(hdc, *publication.syntax(), sourceLine, placementLine,
                                       layout, selectionRects, options);
}

[[nodiscard]] bool PaintRawLine(HDC hdc,
                                 const NoteRenderFinalRawLineSurface& rawLine,
                                 const NoteRenderLineLayoutLocation& layout,
                                 const std::vector<NoteRenderFinalRect>& selectionRects,
                                 const NoteRenderFinalGdiPaintOptions& options) noexcept {
    if (rawLine.placement.runs.size() != 1 ||
        rawLine.placement.runs.front().source_span.start != rawLine.source_span.start ||
        rawLine.placement.runs.front().source_span.end != rawLine.source_span.end) {
        return false;
    }
    const NoteRenderRunPlacement& placement = rawLine.placement.runs.front();
    if (!PaintSelectionForLine(hdc, selectionRects, layout, options)) return false;
    for (size_t fragment = 0; fragment < placement.fragments.size(); ++fragment) {
        if (!DrawTextFragment(hdc, rawLine.display, placement, fragment, layout.top_px,
                              options.theme.text, options)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool PaintImeDecorations(
    HDC hdc, const NoteRenderFinalGdiPaintOptions& options) noexcept {
    if (!options.ime_decorations || options.ime_decorations->empty()) return true;
    RECT clip{};
    const int clipKind = GetClipBox(hdc, &clip);
    if (clipKind == NULLREGION) return true;
    if (clipKind == ERROR || clip.top < 0 || clip.bottom < clip.top ||
        options.vertical_scroll_px > std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(clip.bottom)) return false;
    const HBRUSH brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
    if (!brush) return false;
    const COLORREF previous = SetDCBrushColor(hdc, options.theme.text);
    if (previous == CLR_INVALID) return false;
    struct RestoreBrushColour {
        HDC dc;
        COLORREF colour;
        ~RestoreBrushColour() { (void)SetDCBrushColor(dc, colour); }
    } restore{hdc, previous};
    const uint64_t firstY = options.vertical_scroll_px + static_cast<uint64_t>(clip.top);
    const uint64_t lastY = options.vertical_scroll_px + static_cast<uint64_t>(clip.bottom);
    const auto first = std::upper_bound(options.ime_decorations->begin(), options.ime_decorations->end(), firstY,
        [](uint64_t y, const NoteImePreeditDecoration& decoration) { return y < decoration.rect.bottom_px; });
    for (auto it = first; it != options.ime_decorations->end(); ++it) {
        const auto& decoration = *it;
        const auto& source = decoration.rect;
        if (source.top_px >= lastY) break;
        if (source.bottom_px <= source.top_px || source.right_px <= source.left_px) return false;
        const bool target = decoration.attribute == NoteImeCharacterAttribute::TargetConverted ||
                            decoration.attribute == NoteImeCharacterAttribute::TargetNotConverted;
        const bool error = decoration.attribute == NoteImeCharacterAttribute::InputError;
        const bool dotted = decoration.attribute == NoteImeCharacterAttribute::Input;
        const uint64_t thickness = std::min<uint64_t>(source.bottom_px - source.top_px,
                                                     target || error ? 2 : 1);
        int left = source.left_px;
        int right = source.right_px;
        // A small gap keeps adjacent clauses distinguishable without changing
        // glyph positions or moving the surrounding committed text.
        if (static_cast<int64_t>(right) - left > 2) { ++left; --right; }
        RECT full{};
        if (!TranslateRect(left, source.bottom_px - thickness, right, source.bottom_px, options, &full)) continue;
        RECT visible{};
        if (!IntersectRect(&visible, &full, &clip)) continue;
        if (!dotted && !error) {
            if (!FillRect(hdc, &visible, brush)) return false;
            continue;
        }
        // Pattern work is bounded by the visible width, not the full preedit
        // width or horizontal scroll offset. Keep its phase stable on scroll.
        const int64_t period = dotted ? 4 : 1;
        int64_t x = visible.left;
        const int64_t phase = (x - static_cast<int64_t>(full.left)) % period;
        x -= phase;
        for (; x < visible.right; x += period) {
            RECT stripe = visible;
            stripe.left = static_cast<LONG>(std::max<int64_t>(x, visible.left));
            stripe.right = static_cast<LONG>(std::min<int64_t>(x + (dotted ? 2 : 1), visible.right));
            if (error) {
                const int64_t column = x - static_cast<int64_t>(full.left);
                stripe.top = full.top + static_cast<LONG>(column % thickness);
                stripe.bottom = stripe.top + 1;
                if (stripe.top < visible.top || stripe.bottom > visible.bottom) continue;
            }
            if (stripe.right > stripe.left && !FillRect(hdc, &stripe, brush)) return false;
        }
    }
    return true;
}

[[nodiscard]] bool ResolveStructuredSelection(
    const NoteRenderFinalPublication& publication,
    const NoteRenderFinalGdiPaintOptions& options,
    std::vector<NoteRenderFinalRect>* out) noexcept {
    if (!out) return false;
    try {
        std::vector<NoteRenderFinalRect> resolved;
        if (!options.selection.has_value()) {
            *out = std::move(resolved);
            return true;
        }
        const NoteRenderFinalInteractionResult result = ResolveNoteRenderFinalSelection(
            publication, *options.selection, &resolved);
        if (result == NoteRenderFinalInteractionResult::Resolved ||
            result == NoteRenderFinalInteractionResult::NativeEditorOwner) {
            // A NativeEditor result deliberately emits no rectangles.  The
            // owner plan has assigned that source range to raw input, so this
            // painter must not recreate a second selection there.
            *out = std::move(resolved);
            return true;
        }
        return false;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool PaintStructuredCaret(
    HDC hdc,
    const NoteRenderFinalPublication& publication,
    const NoteRenderFinalGdiPaintOptions& options) noexcept {
    if (!options.caret.has_value()) return true;
    NoteRenderFinalCaretGeometry caret;
    const NoteRenderFinalInteractionResult result = ResolveNoteRenderFinalCaret(
        publication, *options.caret, options.caret_affinity, &caret);
    if (result == NoteRenderFinalInteractionResult::NativeEditorOwner) return true;
    if (result != NoteRenderFinalInteractionResult::Resolved || caret.bottom_px <= caret.top_px) {
        return false;
    }
    int right = 0;
    return AddContentX(caret.x_px, 1, &right) &&
        FillContentRect(hdc, caret.x_px, caret.top_px, right, caret.bottom_px,
                        options.theme.caret, options);
}

[[nodiscard]] bool ResolveHybridSelection(
    const NoteRenderFinalPresentationSnapshot& presentation,
    const NoteRenderFinalGdiPaintOptions& options,
    std::vector<NoteRenderFinalRect>* out) noexcept {
    if (!out) return false;
    try {
        std::vector<NoteRenderFinalRect> resolved;
        if (!options.selection.has_value()) {
            *out = std::move(resolved);
            return true;
        }
        const NoteRenderFinalPresentationInteractionResult result =
            ResolveNoteRenderFinalPresentationEditorSelection(
                presentation, *options.selection, &resolved);
        if (result != NoteRenderFinalPresentationInteractionResult::Resolved) return false;
        *out = std::move(resolved);
        return true;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool PaintHybridCaret(
    HDC hdc,
    const NoteRenderFinalPresentationSnapshot& presentation,
    const NoteRenderFinalGdiPaintOptions& options) noexcept {
    if (!options.caret.has_value()) return true;
    NoteRenderFinalCaretGeometry caret;
    const NoteRenderFinalPresentationInteractionResult result =
        ResolveNoteRenderFinalPresentationEditorCaret(
            presentation, *options.caret, options.caret_affinity, &caret);
    if (result != NoteRenderFinalPresentationInteractionResult::Resolved ||
        caret.bottom_px <= caret.top_px) {
        return false;
    }
    int right = 0;
    return AddContentX(caret.x_px, 1, &right) &&
        FillContentRect(hdc, caret.x_px, caret.top_px, right, caret.bottom_px,
                        options.theme.caret, options);
}

} // namespace

bool NoteRenderFinalGdiPainter::Paint(
    HDC hdc,
    const NoteRenderFinalPublication& publication,
    const NoteRenderFinalGdiPaintOptions& options) noexcept {
    if (!hdc || !publication.valid() || !publication.syntax() || !publication.source_plan() ||
        !publication.layout() || !publication.placement() || !publication.owner_plan() ||
        options.horizontal_scroll_px < 0 ||
        publication.owner_plan()->frame().frame_kind != NotePresentationFrameKind::DrawCommitted) {
        return false;
    }
    RECT clip{};
    const int clipKind = GetClipBox(hdc, &clip);
    if (clipKind == NULLREGION) return true;
    if (clipKind == ERROR || clip.bottom <= clip.top || clip.top < 0 || clip.bottom < 0 ||
        options.vertical_scroll_px > std::numeric_limits<uint64_t>::max() -
            static_cast<uint64_t>(clip.bottom)) {
        return false;
    }
    const uint64_t firstY = options.vertical_scroll_px + static_cast<uint64_t>(clip.top);
    const uint64_t lastY = options.vertical_scroll_px + static_cast<uint64_t>(clip.bottom);
    const auto firstLayout = NoteRenderLineLayoutMap::LineContainingY(
        publication.layout()->line_layouts(), firstY);
    if (!firstLayout.has_value()) return true;
    try {
        std::vector<NoteRenderFinalRect> selectionRects;
        if (!ResolveStructuredSelection(publication, options, &selectionRects)) return false;
        LineIndex lastTableGroup{};
        bool hasLastTableGroup = false;
        for (size_t line = firstLayout->line_index.value;
             line < publication.layout()->line_layouts().line_count(); ++line) {
            const auto layout = NoteRenderLineLayoutMap::LineAt(
                publication.layout()->line_layouts(), {line});
            if (!layout.has_value()) return false;
            if (layout->top_px >= lastY && line != firstLayout->line_index.value) break;
            if (layout->layout.collapsed_into_atomic_group || !IsCommittedOwner(publication, {line})) {
                continue;
            }
            NoteRenderAtomicGroupPlacement group;
            if (publication.placement()->ResolveAtomicGroupContaining({line}, &group) &&
                group.kind == NoteRenderAtomicGroupKind::Table &&
                (!hasLastTableGroup || group.first_line != lastTableGroup)) {
                bool allCommitted = true;
                for (size_t groupLine = group.first_line.value;
                     groupLine <= group.last_line.value; ++groupLine) {
                    allCommitted = allCommitted && IsCommittedOwner(publication, {groupLine});
                    if (groupLine == std::numeric_limits<size_t>::max()) return false;
                }
                const auto groupLayout = NoteRenderLineLayoutMap::LineAt(
                    publication.layout()->line_layouts(), group.first_line);
                if (!groupLayout.has_value() || !allCommitted ||
                    !PaintTableContainerBackgrounds(hdc,
                        *publication.placement(), publication.layout()->line_layouts(), group, options) ||
                    !PaintTableGrid(hdc, group, groupLayout->top_px, options)) {
                    return false;
                }
                lastTableGroup = group.first_line;
                hasLastTableGroup = true;
            }
            if (!PaintLine(hdc, publication, {line}, *layout, selectionRects, options)) return false;
        }
        return PaintStructuredCaret(hdc, publication, options);
    } catch (...) {
        return false;
    }
}

bool NoteRenderFinalGdiPainter::Paint(
    HDC hdc,
    const NoteRenderFinalPresentationSnapshot& presentation,
    const NoteRenderFinalGdiPaintOptions& options) noexcept {
    const std::shared_ptr<const NoteRenderFinalPublication>& structural =
        presentation.structural_publication();
    if (!hdc || !presentation.valid() || !structural || !structural->valid() ||
        !structural->syntax() || !structural->source_plan() || !structural->placement() ||
        options.horizontal_scroll_px < 0) {
        return false;
    }
    RECT clip{};
    const int clipKind = GetClipBox(hdc, &clip);
    if (clipKind == NULLREGION) return true;
    if (clipKind == ERROR || clip.bottom <= clip.top || clip.top < 0 || clip.bottom < 0 ||
        options.vertical_scroll_px > std::numeric_limits<uint64_t>::max() -
            static_cast<uint64_t>(clip.bottom)) {
        return false;
    }
    const uint64_t firstY = options.vertical_scroll_px + static_cast<uint64_t>(clip.top);
    const uint64_t lastY = options.vertical_scroll_px + static_cast<uint64_t>(clip.bottom);
    const auto firstLayout = NoteRenderLineLayoutMap::LineContainingY(
        presentation.line_layouts(), firstY);
    if (!firstLayout.has_value()) return true;
    try {
        std::vector<NoteRenderFinalRect> selectionRects;
        if (!ResolveHybridSelection(presentation, options, &selectionRects)) return false;
        LineIndex lastTableGroup{};
        bool hasLastTableGroup = false;
        for (size_t line = firstLayout->line_index.value;
             line < presentation.line_layouts().line_count(); ++line) {
            const auto layout = NoteRenderLineLayoutMap::LineAt(presentation.line_layouts(), {line});
            if (!layout.has_value()) return false;
            if (layout->top_px >= lastY && line != firstLayout->line_index.value) break;
            const auto surface = presentation.SurfaceAt({line});
            if (!surface.has_value()) return false;
            if (*surface == NoteRenderFinalSurfaceKind::RawPaint) {
                NoteRenderFinalRawLineSurface rawLine;
                if (!presentation.ResolveRawLine({line}, &rawLine) ||
                    !PaintRawLine(hdc, rawLine, *layout, selectionRects, options)) {
                    return false;
                }
                continue;
            }
            if (layout->layout.collapsed_into_atomic_group) continue;

            NoteRenderAtomicGroupPlacement group;
            if (structural->placement()->ResolveAtomicGroupContaining({line}, &group) &&
                group.kind == NoteRenderAtomicGroupKind::Table &&
                (!hasLastTableGroup || group.first_line != lastTableGroup)) {
                bool allStructured = true;
                for (size_t groupLine = group.first_line.value;
                     groupLine <= group.last_line.value; ++groupLine) {
                    const auto groupSurface = presentation.SurfaceAt({groupLine});
                    allStructured = allStructured && groupSurface.has_value() &&
                        *groupSurface == NoteRenderFinalSurfaceKind::StructuredPaint;
                    if (groupLine == std::numeric_limits<size_t>::max()) return false;
                }
                const auto groupLayout = NoteRenderLineLayoutMap::LineAt(
                    presentation.line_layouts(), group.first_line);
                if (!groupLayout.has_value() || !allStructured ||
                    !PaintTableContainerBackgrounds(hdc,
                        *structural->placement(), presentation.line_layouts(), group, options) ||
                    !PaintTableGrid(hdc, group, groupLayout->top_px, options)) {
                    return false;
                }
                lastTableGroup = group.first_line;
                hasLastTableGroup = true;
            }
            NoteRenderSourceLinePlan sourceLine;
            NoteRenderLinePlacement placementLine;
            if (!structural->source_plan()->ResolveLine({line}, &sourceLine) ||
                !structural->placement()->ResolveLine({line}, &placementLine) ||
                !PaintStructuredResolvedLine(hdc, *structural->syntax(), sourceLine,
                                             placementLine, *layout, selectionRects, options)) {
                return false;
            }
        }
        return PaintImeDecorations(hdc, options) && PaintHybridCaret(hdc, presentation, options);
    } catch (...) {
        return false;
    }
}

} // namespace note
