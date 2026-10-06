#include "note/note_render_final_gdi_measurement_provider.h"

#include "note/note_render_final_display_run.h"
#include "note/note_render_final_style.h"
#include "note/note_presentation.h"
#include "math/math_render.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cwctype>
#include <exception>
#include <iterator>
#include <limits>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

namespace note {
namespace {

class ScopedRunFont final {
public:
    ScopedRunFont(HDC hdc, const NoteRenderSourceLinePlan& line,
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
        lineHeightPermille_ = style.line_height_permille;
        const bool needsFont = style.bold || style.italic || !style.font_family.empty() ||
            style.font_height_px != std::max<LONG>(1, baseMetrics.tmHeight);
        if (!needsFont) {
            metrics_ = baseMetrics;
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
        if (!owned_) return;
        if (SelectObject(hdc_, owned_) == HGDI_ERROR) return;
        selected_ = true;
        valid_ = GetTextMetricsW(hdc_, &metrics_) != FALSE;
    }

    ~ScopedRunFont() {
        if (selected_ && hdc_ && old_) (void)SelectObject(hdc_, old_);
        if (owned_) (void)DeleteObject(owned_);
    }

    ScopedRunFont(const ScopedRunFont&) = delete;
    ScopedRunFont& operator=(const ScopedRunFont&) = delete;

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const TEXTMETRICW& metrics() const noexcept { return metrics_; }
    [[nodiscard]] uint32_t line_height_permille() const noexcept {
        return lineHeightPermille_;
    }

private:
    HDC hdc_ = nullptr;
    HGDIOBJ old_ = nullptr;
    HFONT owned_ = nullptr;
    TEXTMETRICW metrics_{};
    uint32_t lineHeightPermille_ = 1000;
    bool selected_ = false;
    bool valid_ = false;
};

[[nodiscard]] bool ScaleLineHeight(LONG fontHeight,
                                   uint32_t lineHeightPermille,
                                   uint32_t* out) noexcept {
    if (!out || fontHeight <= 0 || lineHeightPermille < 1000) return false;
    const uint64_t scaled = static_cast<uint64_t>(fontHeight) * lineHeightPermille;
    const uint64_t rounded = (scaled + 999) / 1000;
    if (rounded == 0 || rounded > std::numeric_limits<uint32_t>::max()) return false;
    *out = static_cast<uint32_t>(rounded);
    return true;
}

// The legacy markup contract treats `<d=N>` as a line property even though
// the parser attaches a style span to runs.  Resolve it from the first
// visible run, rather than from whichever run happens to be measured last.
// This also makes a leading zero-width syntax token incapable of moving text.
[[nodiscard]] bool ResolveLineIndentColumns(const NoteRenderSourcePlan& sourcePlan,
                                            const NoteRenderSourceLinePlan& sourceLine,
                                            int baseFontHeightPx,
                                            int* outColumns) noexcept {
    if (!outColumns || !sourcePlan.syntax() || baseFontHeightPx <= 0) return false;
    *outColumns = 0;
    try {
        const auto resolve = [&](const NoteRenderSourceRun& run,
                                 bool requireNonWhitespace,
                                 bool* outMatched) {
            if (!outMatched) return false;
            *outMatched = false;
            NoteRenderFinalDisplayRun display;
            if (BuildNoteRenderFinalDisplayRun(*sourcePlan.syntax(), run, &display) !=
                NoteRenderFinalDisplayRunBuildResult::Built) {
                return false;
            }
            if (display.display_text.empty()) return true;
            if (requireNonWhitespace) {
                const bool hasVisibleGlyph = std::any_of(
                    display.display_text.begin(), display.display_text.end(),
                    [](wchar_t value) { return std::iswspace(value) == 0; });
                if (!hasVisibleGlyph) return true;
            }
            NoteRenderFinalRunFontStyle style;
            if (!ResolveNoteRenderFinalRunFontStyle(sourceLine, run, baseFontHeightPx, &style)) {
                return false;
            }
            *outColumns = style.has_indent ? style.indent_columns : 0;
            *outMatched = true;
            return true;
        };
        for (const NoteRenderSourceRun& run : sourceLine.runs) {
            bool matched = false;
            if (!resolve(run, true, &matched)) return false;
            if (matched) return true;
        }
        for (const NoteRenderSourceRun& run : sourceLine.runs) {
            bool matched = false;
            if (!resolve(run, false, &matched)) return false;
            if (matched) return true;
        }
        return true;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool AddIndentColumnsToX(int baseX,
                                       int indentColumns,
                                       int spaceWidthPx,
                                       int* outX) noexcept {
    if (!outX || spaceWidthPx <= 0) return false;
    const int64_t adjusted = static_cast<int64_t>(baseX) +
        static_cast<int64_t>(indentColumns) * spaceWidthPx;
    if (adjusted < std::numeric_limits<int>::min() ||
        adjusted > std::numeric_limits<int>::max()) return false;
    *outX = static_cast<int>(adjusted);
    return true;
}

[[nodiscard]] bool SameAnchorStyle(const NoteRenderFinalRunFontStyle& lhs,
                                   const NoteRenderFinalRunFontStyle& rhs) noexcept {
    return lhs.anchor == rhs.anchor &&
        lhs.anchor_offset_columns == rhs.anchor_offset_columns;
}

[[nodiscard]] bool ResolveAnchoredGroupStartX(NoteRenderFinalHorizontalAnchor anchor,
                                              int offsetColumns,
                                              int spaceWidthPx,
                                              uint32_t clientWidthPx,
                                              int groupWidthPx,
                                              int* outX) noexcept {
    if (!outX || spaceWidthPx <= 0 || groupWidthPx < 0) return false;
    const int64_t offset = static_cast<int64_t>(offsetColumns) * spaceWidthPx;
    int64_t x = offset;
    switch (anchor) {
    case NoteRenderFinalHorizontalAnchor::Left:
        break;
    case NoteRenderFinalHorizontalAnchor::Center:
        x += (static_cast<int64_t>(clientWidthPx) - groupWidthPx) / 2;
        break;
    case NoteRenderFinalHorizontalAnchor::Right:
        x += static_cast<int64_t>(clientWidthPx) - groupWidthPx;
        break;
    case NoteRenderFinalHorizontalAnchor::None:
    default:
        return false;
    }
    if (x < std::numeric_limits<int>::min() || x > std::numeric_limits<int>::max()) return false;
    *outX = static_cast<int>(x);
    return true;
}

[[nodiscard]] bool MeasurePrefixWithTabs(HDC hdc,
                                         std::wstring_view text,
                                         int initialX,
                                         int tabOriginX,
                                         int tabWidthPx,
                                         std::vector<int>* out) noexcept {
    if (!hdc || !out || tabWidthPx <= 0 || text.size() > static_cast<size_t>(INT_MAX)) return false;
    try {
        std::vector<int> prefix(text.size() + 1, 0);
        int64_t absoluteX = initialX;
        size_t cursor = 0;
        while (cursor < text.size()) {
            size_t chunkEnd = cursor;
            while (chunkEnd < text.size() && text[chunkEnd] != L'\t') ++chunkEnd;
            const size_t chunkLength = chunkEnd - cursor;
            if (chunkLength > 0) {
                std::vector<int> widths(chunkLength);
                SIZE measured{};
                int fitted = 0;
                if (!GetTextExtentExPointW(hdc, text.data() + cursor,
                                            static_cast<int>(chunkLength), INT_MAX, &fitted,
                                            widths.data(), &measured) ||
                    fitted != static_cast<int>(chunkLength)) {
                    return false;
                }
                for (size_t offset = 0; offset < chunkLength; ++offset) {
                    const int64_t relative = absoluteX + widths[offset] - initialX;
                    if (relative < 0 || relative > std::numeric_limits<int>::max()) return false;
                    prefix[cursor + offset + 1] = static_cast<int>(relative);
                }
                absoluteX += measured.cx;
                if (absoluteX < std::numeric_limits<int>::min() ||
                    absoluteX > std::numeric_limits<int>::max()) return false;
            }
            if (chunkEnd == text.size()) break;
            const int64_t relativeToOrigin = absoluteX - tabOriginX;
            const int64_t modulo = ((relativeToOrigin % tabWidthPx) + tabWidthPx) % tabWidthPx;
            const int64_t advance = tabWidthPx - modulo;
            if (advance <= 0 || absoluteX > std::numeric_limits<int>::max() - advance) return false;
            absoluteX += advance;
            const int64_t relative = absoluteX - initialX;
            if (relative < 0 || relative > std::numeric_limits<int>::max()) return false;
            prefix[chunkEnd + 1] = static_cast<int>(relative);
            cursor = chunkEnd + 1;
        }
        *out = std::move(prefix);
        return true;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool IsUtf16SplitSafe(std::wstring_view text, size_t count) noexcept {
    if (count == 0 || count >= text.size()) return true;
    const wchar_t before = text[count - 1];
    const wchar_t after = text[count];
    return !(before >= 0xD800 && before <= 0xDBFF && after >= 0xDC00 && after <= 0xDFFF);
}

[[nodiscard]] size_t FirstScalarLength(std::wstring_view text) noexcept {
    if (text.empty()) return 0;
    return text.size() >= 2 && text[0] >= 0xD800 && text[0] <= 0xDBFF &&
        text[1] >= 0xDC00 && text[1] <= 0xDFFF ? 2 : 1;
}

[[nodiscard]] size_t PreferredWrapCount(std::wstring_view text, size_t maximum) noexcept {
    maximum = std::min(maximum, text.size());
    while (maximum > 0 && !IsUtf16SplitSafe(text, maximum)) --maximum;
    if (maximum == 0) return 0;
    for (size_t candidate = maximum; candidate > 0; --candidate) {
        const wchar_t previous = text[candidate - 1];
        if (previous == L' ' || previous == L'\t' || previous == L'-' || previous == L'/') {
            return candidate;
        }
    }
    return maximum;
}

[[nodiscard]] bool LastSourceOffsetAtDisplayBoundary(
    const NoteRenderFinalDisplayRun& display,
    size_t displayOffset,
    Utf16CodeUnitOffset* out) noexcept {
    if (!out) return false;
    bool found = false;
    Utf16CodeUnitOffset result{};
    for (const NoteRenderFinalDisplayBoundary& boundary : display.boundaries) {
        if (boundary.display_offset > displayOffset) break;
        if (boundary.display_offset == displayOffset) {
            result = boundary.source_offset;
            found = true;
        }
    }
    if (!found) return false;
    *out = result;
    return true;
}

[[nodiscard]] bool AppendFragmentBoundaries(
    const NoteRenderFinalDisplayRun& display,
    size_t displayStart,
    size_t displayEnd,
    Utf16CodeUnitOffset sourceStart,
    Utf16CodeUnitOffset sourceEnd,
    int fragmentX,
    const std::vector<int>& prefix,
    uint32_t fragmentIndex,
    std::vector<NoteRenderPlacementBoundary>* out) noexcept {
    if (!out || displayEnd < displayStart || prefix.size() != displayEnd - displayStart + 1) {
        return false;
    }
    try {
        const auto append = [&](Utf16CodeUnitOffset source, size_t displayOffset) {
            if (displayOffset < displayStart || displayOffset > displayEnd) return false;
            const int advance = prefix[displayOffset - displayStart];
            if (fragmentX > std::numeric_limits<int>::max() - advance) return false;
            const NoteRenderPlacementBoundary boundary{
                source, fragmentX + advance, fragmentIndex};
            if (!out->empty()) {
                const NoteRenderPlacementBoundary& previous = out->back();
                if (boundary.source_offset < previous.source_offset ||
                    (boundary.source_offset == previous.source_offset &&
                     boundary.fragment_index < previous.fragment_index)) {
                    return false;
                }
            }
            out->push_back(boundary);
            return true;
        };

        bool haveStart = false;
        bool haveEnd = false;
        for (const NoteRenderFinalDisplayBoundary& boundary : display.boundaries) {
            if (boundary.source_offset < sourceStart || boundary.source_offset > sourceEnd ||
                boundary.display_offset < displayStart || boundary.display_offset > displayEnd) {
                continue;
            }
            if (!append(boundary.source_offset, boundary.display_offset)) return false;
            haveStart = haveStart || boundary.source_offset == sourceStart;
            haveEnd = haveEnd || boundary.source_offset == sourceEnd;
        }
        if (!haveStart && !append(sourceStart, displayStart)) return false;
        if (!haveEnd && !append(sourceEnd, displayEnd)) return false;
        return true;
    } catch (...) {
        return false;
    }
}

struct FlowCursor {
    int content_left_px = 0;
    int content_right_px = 0;
    int tab_origin_px = 0;
    int x_px = 0;
    uint32_t visual_row = 0;
};

[[nodiscard]] bool AdvanceToNextVisualRow(FlowCursor* cursor) noexcept {
    if (!cursor || cursor->visual_row == std::numeric_limits<uint32_t>::max()) return false;
    ++cursor->visual_row;
    cursor->x_px = cursor->content_left_px;
    return true;
}

[[nodiscard]] bool BuildRunPlacement(HDC hdc,
                                     const NoteRenderFinalDisplayRun& display,
                                     const NoteRenderSourceRun& sourceRun,
                                     bool wordWrap,
                                     int tabWidthPx,
                                     uint32_t rowHeightPx,
                                     FlowCursor* cursor,
                                     NoteRenderRunPlacement* out,
                                     uint32_t contentHeightPx = 0,
                                     NoteRenderInlineMathVerticalAlignment alignment =
                                         NoteRenderInlineMathVerticalAlignment::Top) noexcept {
    if (!hdc || !cursor || !out || rowHeightPx == 0 || cursor->content_right_px <= cursor->content_left_px ||
        tabWidthPx <= 0) return false;
    // A tall inline formula defines the row's height.  Align its shorter text
    // neighbours too; otherwise the formula itself has no spare row height in
    // which to move and all alignment choices look identical.
    const uint32_t fragmentHeight = contentHeightPx == 0
        ? rowHeightPx : std::min(contentHeightPx, rowHeightPx);
    const uint32_t rowTopOffset = contentHeightPx == 0
        ? 0 : ResolveNoteRenderInlineMathTopOffset(rowHeightPx, fragmentHeight, alignment);
    try {
        NoteRenderRunPlacement candidate;
        candidate.source_span = sourceRun.source_span;
        size_t displayStart = 0;
        Utf16CodeUnitOffset sourceStart = sourceRun.source_span.start;
        while (displayStart < display.display_text.size() ||
               (display.display_text.empty() && candidate.fragments.empty())) {
            if (cursor->visual_row >
                (std::numeric_limits<uint32_t>::max() - rowTopOffset) / rowHeightPx) {
                return false;
            }
            const uint32_t topPx = cursor->visual_row * rowHeightPx + rowTopOffset;
            if (display.display_text.empty()) {
                std::vector<int> emptyPrefix{0};
                candidate.fragments.push_back(
                    {sourceRun.source_span, cursor->x_px, 0, topPx, fragmentHeight});
                if (!AppendFragmentBoundaries(display, 0, 0, sourceRun.source_span.start,
                                              sourceRun.source_span.end, cursor->x_px,
                                              emptyPrefix, 0, &candidate.boundaries)) {
                    return false;
                }
                break;
            }

            const std::wstring_view remaining(display.display_text.data() + displayStart,
                                               display.display_text.size() - displayStart);
            std::vector<int> remainingPrefix;
            if (!MeasurePrefixWithTabs(hdc, remaining, cursor->x_px, cursor->tab_origin_px,
                                       tabWidthPx, &remainingPrefix)) {
                return false;
            }
            size_t take = remaining.size();
            if (wordWrap) {
                const int available = cursor->content_right_px - cursor->x_px;
                size_t fitting = 0;
                for (size_t index = 1; index < remainingPrefix.size(); ++index) {
                    if (remainingPrefix[index] > available) break;
                    fitting = index;
                }
                if (fitting == 0 && cursor->x_px != cursor->content_left_px) {
                    if (!AdvanceToNextVisualRow(cursor)) return false;
                    continue;
                }
                if (fitting == 0) fitting = FirstScalarLength(remaining);
                if (fitting < remaining.size()) take = PreferredWrapCount(remaining, fitting);
                if (take == 0) take = FirstScalarLength(remaining);
            }
            while (take > 0 && !IsUtf16SplitSafe(remaining, take)) --take;
            if (take == 0) return false;

            const std::wstring_view fragmentText = remaining.substr(0, take);
            std::vector<int> fragmentPrefix;
            if (!MeasurePrefixWithTabs(hdc, fragmentText, cursor->x_px, cursor->tab_origin_px,
                                       tabWidthPx, &fragmentPrefix) ||
                fragmentPrefix.empty()) {
                return false;
            }
            const int widthPx = fragmentPrefix.back();
            if (widthPx < 0 || cursor->x_px > std::numeric_limits<int>::max() - widthPx) return false;
            Utf16CodeUnitOffset sourceEnd;
            if (!LastSourceOffsetAtDisplayBoundary(display, displayStart + take, &sourceEnd) ||
                sourceEnd <= sourceStart) {
                return false;
            }
            const uint32_t fragmentIndex = static_cast<uint32_t>(candidate.fragments.size());
            candidate.fragments.push_back(
                {{sourceStart, sourceEnd}, cursor->x_px, widthPx, topPx, fragmentHeight});
            if (!AppendFragmentBoundaries(display, displayStart, displayStart + take,
                                          sourceStart, sourceEnd, cursor->x_px,
                                          fragmentPrefix, fragmentIndex,
                                          &candidate.boundaries)) {
                return false;
            }
            cursor->x_px += widthPx;
            displayStart += take;
            sourceStart = sourceEnd;
        }
        if (candidate.fragments.empty() || candidate.boundaries.size() < 2) return false;
        int left = std::numeric_limits<int>::max();
        int right = std::numeric_limits<int>::min();
        for (const NoteRenderRunPlacementFragment& fragment : candidate.fragments) {
            if (fragment.x_px > std::numeric_limits<int>::max() - fragment.width_px) return false;
            left = std::min(left, fragment.x_px);
            right = std::max(right, fragment.x_px + fragment.width_px);
        }
        const int64_t width = static_cast<int64_t>(right) - left;
        if (left == std::numeric_limits<int>::max() || width < 0 ||
            width > std::numeric_limits<int>::max()) return false;
        candidate.x_px = left;
        candidate.width_px = static_cast<int>(width);
        *out = std::move(candidate);
        return true;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool MeasureMathGraphic(HDC hdc,
                                      const NoteRenderFinalDisplayRun& display,
                                      int fontHeightPx,
                                      mathrender::RenderStyle style,
                                      mathrender::Layout* out) noexcept {
    if (!hdc || !out || display.display_text.empty() || fontHeightPx <= 0) return false;
    try {
        std::unique_ptr<mathrender::Node> node = mathrender::Parse(display.display_text);
        if (!node) return false;
        const mathrender::Layout layout = mathrender::Measure(
            *node, hdc, fontHeightPx, style);
        if (layout.width <= 0 || layout.height <= 0) return false;
        *out = layout;
        return true;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool MeasureInlineMathGraphic(HDC hdc,
                                            const NoteRenderFinalDisplayRun& display,
                                            int fontHeightPx,
                                            mathrender::Layout* out) noexcept {
    return MeasureMathGraphic(hdc, display, fontHeightPx, mathrender::RenderStyle::Inline, out);
}

[[nodiscard]] bool BuildInlineMathPlacement(HDC hdc,
                                            const NoteRenderFinalDisplayRun& display,
                                            const NoteRenderSourceRun& sourceRun,
                                            int fontHeightPx,
                                            uint32_t rowHeightPx,
                                            NoteRenderInlineMathVerticalAlignment alignment,
                                            FlowCursor* cursor,
                                            NoteRenderRunPlacement* out) noexcept {
    if (!out || !cursor || rowHeightPx == 0) return false;
    mathrender::Layout mathLayout;
    if (!MeasureInlineMathGraphic(hdc, display, fontHeightPx, &mathLayout) ||
        mathLayout.width < 0 || mathLayout.height <= 0 ||
        static_cast<uint32_t>(mathLayout.height) > rowHeightPx) {
        return false;
    }
    if (cursor->x_px > cursor->content_right_px - mathLayout.width &&
        cursor->x_px != cursor->content_left_px) {
        if (!AdvanceToNextVisualRow(cursor)) return false;
    }
    const uint32_t rowTopOffset = ResolveNoteRenderInlineMathTopOffset(
        rowHeightPx, static_cast<uint32_t>(mathLayout.height), alignment);
    if (cursor->visual_row >
            (std::numeric_limits<uint32_t>::max() - rowTopOffset) / rowHeightPx ||
        cursor->x_px > std::numeric_limits<int>::max() - mathLayout.width) {
        return false;
    }
    const uint32_t top = cursor->visual_row * rowHeightPx + rowTopOffset;
    NoteRenderRunPlacement candidate;
    candidate.source_span = sourceRun.source_span;
    candidate.x_px = cursor->x_px;
    candidate.width_px = mathLayout.width;
    candidate.fragments.push_back({sourceRun.source_span, cursor->x_px,
                                   mathLayout.width, top,
                                   static_cast<uint32_t>(mathLayout.height),
                                   mathLayout.baseline});
    candidate.boundaries.push_back({sourceRun.source_span.start, cursor->x_px, 0});
    candidate.boundaries.push_back(
        {sourceRun.source_span.end, cursor->x_px + mathLayout.width, 0});
    cursor->x_px += mathLayout.width;
    *out = std::move(candidate);
    return true;
}

[[nodiscard]] bool UnsupportedExactGroup(const NoteRenderSourceLinePlan& line) noexcept {
    if (line.decoration.has(NoteRenderSourceLineDecorationTable) ||
        line.decoration.has(NoteRenderSourceLineDecorationTableDivider)) {
        return true;
    }
    return std::any_of(line.runs.begin(), line.runs.end(), [](const NoteRenderSourceRun& run) {
        return run.kind == NoteRenderSourceRunKind::BlockMath ||
               run.table_block != NoteRenderSourceRun::kNoTableBlock;
    });
}

[[nodiscard]] bool AddColumns(int* value, uint32_t columns, int columnWidthPx) noexcept {
    if (!value || columnWidthPx <= 0 ||
        columns > static_cast<uint32_t>(std::numeric_limits<int>::max() / columnWidthPx)) {
        return false;
    }
    const int amount = static_cast<int>(columns) * columnWidthPx;
    if (*value > std::numeric_limits<int>::max() - amount) return false;
    *value += amount;
    return true;
}

[[nodiscard]] Span DecorationSourceSpan(const NoteRenderSourceLinePlan& line) noexcept {
    for (const NoteRenderSourceRun& run : line.runs) {
        if (run.kind == NoteRenderSourceRunKind::HiddenSyntax) return run.source_span;
    }
    return line.content_span;
}

struct TaskCheckboxMetrics {
    int side_px = 0;
    int gap_px = 0;
    uint32_t top_px = 0;
};

[[nodiscard]] bool ResolveTaskCheckboxMetrics(uint32_t fontHeightPx,
                                              int spaceWidthPx,
                                              TaskCheckboxMetrics* out) noexcept {
    if (!out || fontHeightPx == 0 || spaceWidthPx <= 0) return false;
    // Match the first text row, not its line spacing or an adjacent math graphic.
    const uint64_t side = std::max<uint64_t>(1, (uint64_t{fontHeightPx} * 4 + 2) / 5);
    const uint64_t gap = std::max<uint64_t>(spaceWidthPx, std::max<uint64_t>(1, side / 3));
    if (side + gap > static_cast<uint64_t>(std::numeric_limits<int>::max())) return false;
    *out = {static_cast<int>(side), static_cast<int>(gap),
            static_cast<uint32_t>((fontHeightPx - side) / 2)};
    return true;
}

[[nodiscard]] bool AddVisualDecorations(const NoteRenderSourceLinePlan& sourceLine,
                                        int baseLeftPx,
                                        int contentLeftPx,
                                        int contentRightPx,
                                        int spaceWidthPx,
                                        const TaskCheckboxMetrics& taskMetrics,
                                        uint32_t rowHeightPx,
                                        uint32_t totalHeightPx,
                                        const std::vector<NoteRenderRunPlacement>& runs,
                                        std::vector<NoteRenderVisualDecoration>* out) noexcept {
    if (!out || baseLeftPx < 0 || contentLeftPx < baseLeftPx || contentRightPx <= contentLeftPx ||
        spaceWidthPx <= 0 || rowHeightPx == 0 || totalHeightPx == 0) {
        return false;
    }
    try {
        std::vector<NoteRenderVisualDecoration> candidate;
        const Span sourceSpan = DecorationSourceSpan(sourceLine);
        if (sourceLine.decoration.has(NoteRenderSourceLineDecorationListItem) &&
            !sourceLine.decoration.has(NoteRenderSourceLineDecorationTaskItem)) {
            const int markerRight = contentLeftPx - spaceWidthPx;
            if (markerRight <= baseLeftPx) return false;
            std::wstring marker = sourceLine.decoration.has(
                NoteRenderSourceLineDecorationOrderedListItem)
                ? std::to_wstring(sourceLine.decoration.ordered_list_number) + L"."
                : L"•";
            if (marker.empty()) return false;
            candidate.push_back({NoteRenderVisualDecorationKind::ListMarker, sourceSpan,
                                 baseLeftPx, 0, markerRight, rowHeightPx,
                                 std::move(marker), false});
        }
        if (sourceLine.decoration.has(NoteRenderSourceLineDecorationTaskItem)) {
            const int64_t boxRight = int64_t{contentLeftPx} - taskMetrics.gap_px;
            const int64_t boxLeft = boxRight - taskMetrics.side_px;
            if (taskMetrics.side_px <= 0 || boxLeft < baseLeftPx || boxRight <= boxLeft ||
                uint64_t{taskMetrics.top_px} + taskMetrics.side_px > totalHeightPx) return false;
            candidate.push_back({NoteRenderVisualDecorationKind::TaskCheckbox, sourceSpan,
                                 static_cast<int>(boxLeft), taskMetrics.top_px,
                                 static_cast<int>(boxRight),
                                 taskMetrics.top_px + static_cast<uint32_t>(taskMetrics.side_px), {},
                                 sourceLine.decoration.task_checked});
        }
        if (sourceLine.decoration.has(NoteRenderSourceLineDecorationQuote)) {
            const int barRight = contentLeftPx - std::max(1, spaceWidthPx);
            const int barLeft = barRight - std::max(1, spaceWidthPx / 2);
            if (barLeft < baseLeftPx || barRight <= barLeft) return false;
            candidate.push_back({NoteRenderVisualDecorationKind::QuoteBar, sourceSpan,
                                 barLeft, 0, barRight, totalHeightPx, {}, false});
        }
        if (sourceLine.decoration.has(NoteRenderSourceLineDecorationCodeBlock)) {
            candidate.push_back({NoteRenderVisualDecorationKind::CodeBlockSurface, sourceSpan,
                                 baseLeftPx, 0, contentRightPx, totalHeightPx, {}, false});
        }
        if (sourceLine.decoration.has(NoteRenderSourceLineDecorationHorizontalRule)) {
            const uint32_t top = (rowHeightPx - 1) / 2;
            candidate.push_back({NoteRenderVisualDecorationKind::HorizontalRule, sourceSpan,
                                 baseLeftPx, top, contentRightPx, top + 1, {}, false});
        }
        for (const NoteRenderRunPlacement& run : runs) {
            if (run.source_span.end <= run.source_span.start) return false;
            for (const NoteRenderRunPlacementFragment& fragment : run.fragments) {
                if (fragment.x_px > std::numeric_limits<int>::max() - fragment.width_px ||
                    fragment.top_offset_px > totalHeightPx ||
                    fragment.height_px > totalHeightPx - fragment.top_offset_px) {
                    return false;
                }
            }
        }
        for (size_t index = 0; index < runs.size(); ++index) {
            // The source-plan row order is identical to the measured run
            // order. Inline-code surfaces therefore use the exact wrapped
            // rectangles already chosen for text/caret/selection.
            if (index >= sourceLine.runs.size() ||
                sourceLine.runs[index].kind != NoteRenderSourceRunKind::InlineCode) {
                continue;
            }
            for (const NoteRenderRunPlacementFragment& fragment : runs[index].fragments) {
                const int pad = std::max(1, spaceWidthPx / 3);
                if (fragment.x_px < std::numeric_limits<int>::min() + pad ||
                    fragment.x_px > std::numeric_limits<int>::max() - fragment.width_px - pad ||
                    fragment.top_offset_px > totalHeightPx ||
                    fragment.height_px > totalHeightPx - fragment.top_offset_px) {
                    return false;
                }
                candidate.push_back({NoteRenderVisualDecorationKind::InlineCodeSurface,
                                     fragment.source_span, fragment.x_px - pad,
                                     fragment.top_offset_px, fragment.x_px + fragment.width_px + pad,
                                     fragment.top_offset_px + fragment.height_px, {}, false});
            }
        }
        for (size_t index = 0; index < runs.size(); ++index) {
            if (index >= sourceLine.runs.size() ||
                sourceLine.runs[index].kind != NoteRenderSourceRunKind::InlineMath) {
                continue;
            }
            for (const NoteRenderRunPlacementFragment& fragment : runs[index].fragments) {
                if (fragment.x_px > std::numeric_limits<int>::max() - fragment.width_px ||
                    fragment.top_offset_px > totalHeightPx ||
                    fragment.height_px > totalHeightPx - fragment.top_offset_px) {
                    return false;
                }
                candidate.push_back({NoteRenderVisualDecorationKind::InlineMathGraphic,
                                     fragment.source_span, fragment.x_px,
                                     fragment.top_offset_px, fragment.x_px + fragment.width_px,
                                     fragment.top_offset_px + fragment.height_px, {}, false});
            }
        }
        out->insert(out->end(), std::make_move_iterator(candidate.begin()),
                    std::make_move_iterator(candidate.end()));
        return true;
    } catch (...) {
        return false;
    }
}

// The same per-row surface geometry used by code blocks, with recursively
// inset container edges. All coordinates precede glyph/selection/hit mapping.
[[nodiscard]] bool AddContainerDecorations(
    const NoteRenderSourceLinePlan& sourceLine, const NoteRenderLayoutKey& key,
    const NoteRenderFinalGdiMeasurementOptions& options, int spaceWidth,
    uint32_t height, NoteRenderLinePlacement* placement) {
    if (sourceLine.decoration.container_depth == 0 || height == 0) return true;
    const int64_t left = std::max(key.horizontal_padding_px, options.minimum_left_padding_px);
    const int64_t right = int64_t{key.client_width_px} -
        std::max(key.horizontal_padding_px, options.minimum_right_padding_px);
    const int64_t inset = int64_t{options.code_block_inset_columns} * spaceWidth;
    if (inset <= 0 || sourceLine.decoration.container_depth >
        static_cast<uint64_t>(std::numeric_limits<int>::max() / inset)) return false;
    for (uint32_t depth = 0; depth < sourceLine.decoration.container_depth; ++depth) {
        const int64_t x0 = left + depth * inset;
        const int64_t x1 = right - depth * inset;
        if (x0 < 0 || x1 <= x0 + 1 || x1 > std::numeric_limits<int>::max()) return false;
        const Span span = DecorationSourceSpan(sourceLine);
        const auto append = [&](NoteRenderVisualDecorationKind kind,
                                int a, uint32_t top, int b, uint32_t bottom) {
            placement->decorations.push_back({kind, span, a, top, b, bottom, {}, false});
        };
        append(NoteRenderVisualDecorationKind::ContainerSurface, static_cast<int>(x0), 0,
               static_cast<int>(x1), height);
        append(NoteRenderVisualDecorationKind::ContainerBorder, static_cast<int>(x0), 0,
               static_cast<int>(x0 + 1), height);
        append(NoteRenderVisualDecorationKind::ContainerBorder, static_cast<int>(x1 - 1), 0,
               static_cast<int>(x1), height);
        if (depth + 1 == sourceLine.decoration.container_depth) {
            if (sourceLine.decoration.has(NoteRenderSourceLineDecorationContainerOpening)) {
                append(NoteRenderVisualDecorationKind::ContainerBorder, static_cast<int>(x0), 0,
                       static_cast<int>(x1), 1);
            }
            if (sourceLine.decoration.has(NoteRenderSourceLineDecorationContainerClosing)) {
                append(NoteRenderVisualDecorationKind::ContainerBorder, static_cast<int>(x0), height - 1,
                       static_cast<int>(x1), height);
            }
        }
    }
    return true;
}

[[nodiscard]] bool ContainerInsetPx(const NoteRenderSourceLinePlan& line,
                                     const NoteRenderFinalGdiMeasurementOptions& options,
                                     int spaceWidth, int* out) noexcept {
    const uint64_t columns = uint64_t{line.decoration.container_depth} * options.code_block_inset_columns;
    if (!out || spaceWidth <= 0 || columns >
        static_cast<uint64_t>(std::numeric_limits<int>::max() / spaceWidth)) return false;
    *out = static_cast<int>(columns) * spaceWidth;
    return true;
}

[[nodiscard]] bool MeasureLine(const NoteRenderSourcePlan& sourcePlan,
                               const NoteRenderLayoutKey& layoutKey,
                               const NoteRenderFinalGdiMeasurementOptions& options,
                               HDC hdc,
                               TEXTMETRICW baseMetrics,
                               int spaceWidthPx,
                               LineIndex lineIndex,
                               NoteRenderLineLayout* outLayout,
                               NoteRenderLinePlacement* outPlacement) noexcept {
    if (!outLayout || !outPlacement || !hdc || spaceWidthPx <= 0) return false;
    try {
        NoteRenderSourceLinePlan sourceLine;
        if (!sourcePlan.ResolveLine(lineIndex, &sourceLine) || UnsupportedExactGroup(sourceLine)) {
            return false;
        }
        const uint32_t baseHeight = std::max<LONG>(1, baseMetrics.tmHeight);
        uint32_t rowHeight = baseHeight;
        uint32_t markerFontHeight = baseHeight;
        bool hasInlineMath = false;
        for (const NoteRenderSourceRun& run : sourceLine.runs) {
            ScopedRunFont font(hdc, sourceLine, run);
            if (!font.valid()) return false;
            if (run.kind != NoteRenderSourceRunKind::HiddenSyntax) {
                markerFontHeight = std::max<uint32_t>(markerFontHeight,
                                                     std::max<LONG>(1, font.metrics().tmHeight));
            }
            uint32_t styledRowHeight = 0;
            if (!ScaleLineHeight(std::max<LONG>(1, font.metrics().tmHeight),
                                 font.line_height_permille(), &styledRowHeight)) {
                return false;
            }
            rowHeight = std::max(rowHeight, styledRowHeight);
            if (run.kind == NoteRenderSourceRunKind::InlineMath) {
                hasInlineMath = true;
                NoteRenderFinalDisplayRun display;
                mathrender::Layout mathLayout;
                if (BuildNoteRenderFinalDisplayRun(*sourcePlan.syntax(), run, &display) !=
                        NoteRenderFinalDisplayRunBuildResult::Built ||
                    !MeasureInlineMathGraphic(hdc, display,
                                             std::max<LONG>(1, font.metrics().tmHeight),
                                             &mathLayout) ||
                    mathLayout.height <= 0) {
                    return false;
                }
                rowHeight = std::max<uint32_t>(rowHeight,
                                               static_cast<uint32_t>(mathLayout.height));
            }
        }
        TaskCheckboxMetrics taskMetrics;
        if (sourceLine.decoration.has(NoteRenderSourceLineDecorationTaskItem) &&
            !ResolveTaskCheckboxMetrics(markerFontHeight, spaceWidthPx, &taskMetrics)) return false;
        const uint64_t basePadding = std::max<uint32_t>(layoutKey.horizontal_padding_px,
                                                        options.minimum_left_padding_px);
        const uint64_t rightPadding = std::max<uint32_t>(layoutKey.horizontal_padding_px,
                                                         options.minimum_right_padding_px);
        if (basePadding >= layoutKey.client_width_px ||
            rightPadding >= layoutKey.client_width_px ||
            basePadding + rightPadding >= layoutKey.client_width_px ||
            basePadding > std::numeric_limits<int>::max()) {
            return false;
        }
        int contentLeft = static_cast<int>(basePadding);
        int containerInset = 0;
        if (!ContainerInsetPx(sourceLine, options, spaceWidthPx, &containerInset) ||
            contentLeft > std::numeric_limits<int>::max() - containerInset) return false;
        contentLeft += containerInset;
        int explicitIndentColumns = 0;
        if (!ResolveLineIndentColumns(sourcePlan, sourceLine,
                                      std::max<LONG>(1, baseMetrics.tmHeight),
                                      &explicitIndentColumns) ||
            !AddIndentColumnsToX(contentLeft, explicitIndentColumns, spaceWidthPx,
                                 &contentLeft)) {
            return false;
        }
        if (sourceLine.decoration.has(NoteRenderSourceLineDecorationListItem)) {
            const uint32_t depth = std::max<uint32_t>(1, sourceLine.decoration.list_depth);
            if (!AddColumns(&contentLeft, (depth - 1) * layoutKey.tab_columns, spaceWidthPx)) {
                return false;
            }
            const int markerLeft = contentLeft;
            if (!AddColumns(&contentLeft, options.list_continuation_columns, spaceWidthPx) ||
                (sourceLine.decoration.has(NoteRenderSourceLineDecorationTaskItem) &&
                 !AddColumns(&contentLeft, layoutKey.tab_columns, spaceWidthPx))) {
                return false;
            }
            if (sourceLine.decoration.has(NoteRenderSourceLineDecorationTaskItem)) {
                const int64_t minimumLeft = int64_t{markerLeft} + taskMetrics.side_px + taskMetrics.gap_px;
                if (minimumLeft > std::numeric_limits<int>::max()) return false;
                contentLeft = std::max(contentLeft, static_cast<int>(minimumLeft));
            }
        }
        if (sourceLine.decoration.has(NoteRenderSourceLineDecorationQuote) &&
            !AddColumns(&contentLeft, options.quote_gutter_columns, spaceWidthPx)) {
            return false;
        }
        if (sourceLine.decoration.has(NoteRenderSourceLineDecorationCodeBlock) &&
            !AddColumns(&contentLeft, options.code_block_inset_columns, spaceWidthPx)) {
            return false;
        }
        const int contentRight = static_cast<int>(layoutKey.client_width_px - rightPadding) - containerInset;
        if (contentLeft >= contentRight || layoutKey.tab_columns == 0 ||
            layoutKey.tab_columns > static_cast<uint32_t>(std::numeric_limits<int>::max() / spaceWidthPx)) {
            return false;
        }
        const int tabWidth = static_cast<int>(layoutKey.tab_columns) * spaceWidthPx;
        FlowCursor cursor{contentLeft, contentRight, contentLeft, contentLeft, 0};
        NoteRenderLinePlacement placement;
        int inlineExtent = 0;
        const auto buildRunAtCursor = [&](const NoteRenderSourceRun& run,
                                          bool wordWrap,
                                          FlowCursor* runCursor,
                                          NoteRenderRunPlacement* outRun) {
            if (!runCursor || !outRun) return false;
            ScopedRunFont font(hdc, sourceLine, run);
            if (!font.valid()) return false;
            NoteRenderFinalDisplayRun display;
            if (BuildNoteRenderFinalDisplayRun(*sourcePlan.syntax(), run, &display) !=
                NoteRenderFinalDisplayRunBuildResult::Built) {
                return false;
            }
            return run.kind == NoteRenderSourceRunKind::InlineMath
                ? BuildInlineMathPlacement(hdc, display, run,
                                           std::max<LONG>(1, font.metrics().tmHeight),
                                           rowHeight, options.inline_math_vertical_alignment,
                                           runCursor, outRun)
                : BuildRunPlacement(hdc, display, run, wordWrap, tabWidth,
                                    rowHeight, runCursor, outRun,
                                    std::max<LONG>(1, font.metrics().tmHeight),
                                    hasInlineMath ? options.inline_math_vertical_alignment
                                                  : NoteRenderInlineMathVerticalAlignment::Top);
        };
        for (size_t runIndex = 0; runIndex < sourceLine.runs.size();) {
            const NoteRenderSourceRun& run = sourceLine.runs[runIndex];
            NoteRenderFinalRunFontStyle runStyle;
            if (!ResolveNoteRenderFinalRunFontStyle(
                    sourceLine, run, std::max<LONG>(1, baseMetrics.tmHeight), &runStyle)) {
                return false;
            }
            if (runStyle.anchor == NoteRenderFinalHorizontalAnchor::None) {
                NoteRenderRunPlacement runPlacement;
                if (!buildRunAtCursor(run, layoutKey.word_wrap, &cursor, &runPlacement) ||
                    runPlacement.x_px > std::numeric_limits<int>::max() -
                        runPlacement.width_px) {
                    return false;
                }
                inlineExtent = std::max(inlineExtent,
                                        runPlacement.x_px + runPlacement.width_px);
                placement.runs.push_back(std::move(runPlacement));
                ++runIndex;
                continue;
            }

            // A contiguous anchored sequence is one non-flow group.  Its
            // total width is measured first, then every member is placed from
            // that one origin.  Normal-flow text before/after the group keeps
            // its own cursor, exactly as the legacy `<d=center>` contract.
            size_t groupEnd = runIndex + 1;
            while (groupEnd < sourceLine.runs.size()) {
                NoteRenderFinalRunFontStyle nextStyle;
                if (!ResolveNoteRenderFinalRunFontStyle(
                        sourceLine, sourceLine.runs[groupEnd],
                        std::max<LONG>(1, baseMetrics.tmHeight), &nextStyle)) {
                    return false;
                }
                // Source-plan tag boundaries retain source coordinates for
                // editing but have no display width.  Their representation
                // may be HiddenSyntax or an empty text run; neither may
                // terminate `<d=center>a</><d=center>b</>`.
                NoteRenderFinalDisplayRun interveningDisplay;
                const bool zeroWidthSyntax =
                    BuildNoteRenderFinalDisplayRun(
                        *sourcePlan.syntax(), sourceLine.runs[groupEnd],
                        &interveningDisplay) == NoteRenderFinalDisplayRunBuildResult::Built &&
                    interveningDisplay.display_text.empty();
                if (sourceLine.runs[groupEnd].kind == NoteRenderSourceRunKind::HiddenSyntax ||
                    zeroWidthSyntax) {
                    ++groupEnd;
                    continue;
                }
                if (nextStyle.anchor == NoteRenderFinalHorizontalAnchor::None ||
                    !SameAnchorStyle(runStyle, nextStyle)) {
                    break;
                }
                ++groupEnd;
            }
            FlowCursor widthCursor{0, std::numeric_limits<int>::max(), 0, 0, 0};
            for (size_t index = runIndex; index < groupEnd; ++index) {
                NoteRenderRunPlacement ignored;
                if (!buildRunAtCursor(sourceLine.runs[index], false, &widthCursor, &ignored)) {
                    return false;
                }
            }
            if (widthCursor.visual_row != 0 || widthCursor.x_px < 0) return false;
            int anchorX = 0;
            if (!ResolveAnchoredGroupStartX(runStyle.anchor,
                                            runStyle.anchor_offset_columns,
                                            spaceWidthPx, layoutKey.client_width_px,
                                            widthCursor.x_px, &anchorX)) {
                return false;
            }
            FlowCursor anchoredCursor{anchorX, std::numeric_limits<int>::max(),
                                      anchorX, anchorX, 0};
            for (size_t index = runIndex; index < groupEnd; ++index) {
                NoteRenderRunPlacement runPlacement;
                if (!buildRunAtCursor(sourceLine.runs[index], false,
                                      &anchoredCursor, &runPlacement) ||
                    runPlacement.x_px > std::numeric_limits<int>::max() -
                        runPlacement.width_px) {
                    return false;
                }
                inlineExtent = std::max(inlineExtent,
                                        runPlacement.x_px + runPlacement.width_px);
                placement.runs.push_back(std::move(runPlacement));
            }
            if (anchoredCursor.visual_row != 0) return false;
            runIndex = groupEnd;
        }
        if (cursor.visual_row >= std::numeric_limits<uint32_t>::max() / rowHeight) return false;
        const uint64_t totalHeight = static_cast<uint64_t>(cursor.visual_row + 1) * rowHeight;
        if (totalHeight == 0 || totalHeight > std::numeric_limits<uint32_t>::max() ||
            inlineExtent < 0) {
            return false;
        }
        const uint32_t totalHeightPx = static_cast<uint32_t>(totalHeight);
        // Negative explicit indents are valid legacy input.  Their ink is
        // clipped by the client at paint time, but geometry remains complete
        // and selection/hit testing must not silently drop to another model.
        const int decorationBaseLeft = std::min(static_cast<int>(basePadding), contentLeft);
        if (!AddContainerDecorations(sourceLine, layoutKey, options, spaceWidthPx,
                                     totalHeightPx, &placement) ||
            !AddVisualDecorations(sourceLine, decorationBaseLeft + containerInset, contentLeft,
                                  contentRight, spaceWidthPx, taskMetrics, rowHeight, totalHeightPx,
                                  placement.runs, &placement.decorations)) {
            return false;
        }
        *outLayout = {totalHeightPx, static_cast<uint32_t>(inlineExtent)};
        *outPlacement = std::move(placement);
        return true;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool MeasureBlockMathGroup(
    const NoteRenderSourcePlan& sourcePlan,
    const NoteRenderLayoutKey& layoutKey,
    const NoteRenderFinalGdiMeasurementOptions& options,
    HDC hdc,
    TEXTMETRICW baseMetrics,
    const NoteRenderAtomicGroup& group,
    std::vector<NoteRenderLineLayout>* lineLayouts,
    std::vector<NoteRenderLinePlacement>* linePlacements,
    NoteRenderAtomicGroupPlacement* outGroupPlacement) noexcept {
    if (!lineLayouts || !linePlacements || !outGroupPlacement ||
        group.kind != NoteRenderAtomicGroupKind::BlockMath ||
        group.first_line > group.last_line ||
        group.last_line.value >= lineLayouts->size() ||
        group.last_line.value >= linePlacements->size()) {
        return false;
    }
    try {
        NoteRenderSourceLinePlan anchorLine;
        if (!sourcePlan.ResolveLine(group.first_line, &anchorLine)) return false;
        const auto runIt = std::find_if(anchorLine.runs.begin(), anchorLine.runs.end(),
                                        [&group](const NoteRenderSourceRun& run) {
                                            return run.kind == NoteRenderSourceRunKind::BlockMath &&
                                                   run.source_span.start == group.source_span.start &&
                                                   run.source_span.end == group.source_span.end;
                                        });
        if (runIt == anchorLine.runs.end()) return false;
        ScopedRunFont font(hdc, anchorLine, *runIt);
        if (!font.valid()) return false;
        NoteRenderFinalDisplayRun display;
        if (BuildNoteRenderFinalDisplayRun(*sourcePlan.syntax(), *runIt, &display) !=
            NoteRenderFinalDisplayRunBuildResult::Built) {
            return false;
        }
        mathrender::Layout mathLayout;
        if (!MeasureMathGraphic(hdc, display,
                                std::max<LONG>(1, font.metrics().tmHeight),
                                mathrender::RenderStyle::Display, &mathLayout)) {
            return false;
        }
        const uint64_t leftPadding = std::max<uint32_t>(layoutKey.horizontal_padding_px,
                                                         options.minimum_left_padding_px);
        const uint64_t rightPadding = std::max<uint32_t>(layoutKey.horizontal_padding_px,
                                                          options.minimum_right_padding_px);
        if (leftPadding >= layoutKey.client_width_px ||
            rightPadding >= layoutKey.client_width_px ||
            leftPadding + rightPadding >= layoutKey.client_width_px ||
            leftPadding > static_cast<uint64_t>(std::numeric_limits<int>::max()) ||
            mathLayout.width <= 0 || mathLayout.height <= 0) {
            return false;
        }
        SIZE space{};
        int containerInset = 0;
        if (!GetTextExtentPoint32W(hdc, L" ", 1, &space) ||
            !ContainerInsetPx(anchorLine, options, std::max<LONG>(1, space.cx), &containerInset) ||
            leftPadding + static_cast<uint64_t>(containerInset) >
                static_cast<uint64_t>(std::numeric_limits<int>::max())) return false;
        const int contentLeft = static_cast<int>(leftPadding) + containerInset;
        const int contentRight = static_cast<int>(layoutKey.client_width_px - rightPadding) - containerInset;
        const int contentWidth = contentRight - contentLeft;
        if (contentWidth <= 0) return false;
        const int mathX = mathLayout.width >= contentWidth
            ? contentLeft
            : contentLeft + (contentWidth - mathLayout.width) / 2;
        const uint32_t verticalPadding = std::max<uint32_t>(1,
            static_cast<uint32_t>(std::max<LONG>(1, baseMetrics.tmHeight)) / 2);
        if (static_cast<uint32_t>(mathLayout.height) >
            std::numeric_limits<uint32_t>::max() - verticalPadding * 2u ||
            mathX > std::numeric_limits<int>::max() - mathLayout.width) {
            return false;
        }
        const uint32_t groupHeight = static_cast<uint32_t>(mathLayout.height) + verticalPadding * 2u;
        NoteRenderRunPlacement runPlacement;
        runPlacement.source_span = runIt->source_span;
        runPlacement.x_px = mathX;
        runPlacement.width_px = mathLayout.width;
        runPlacement.fragments.push_back({runIt->source_span, mathX, mathLayout.width,
                                          verticalPadding,
                                          static_cast<uint32_t>(mathLayout.height),
                                          mathLayout.baseline});
        runPlacement.boundaries.push_back({runIt->source_span.start, mathX, 0});
        runPlacement.boundaries.push_back(
            {runIt->source_span.end, mathX + mathLayout.width, 0});
        NoteRenderLinePlacement anchorPlacement;
        anchorPlacement.runs.push_back(std::move(runPlacement));
        if (!AddContainerDecorations(anchorLine, layoutKey, options, std::max<LONG>(1, space.cx),
                                     groupHeight, &anchorPlacement)) return false;
        (*lineLayouts)[group.first_line.value] = {
            groupHeight, static_cast<uint32_t>(mathX + mathLayout.width), false};
        (*linePlacements)[group.first_line.value] = std::move(anchorPlacement);
        for (size_t line = group.first_line.value + 1; line <= group.last_line.value; ++line) {
            NoteRenderSourceLinePlan sourceLine;
            if (!sourcePlan.ResolveLine({line}, &sourceLine) || !sourceLine.runs.empty()) return false;
            (*lineLayouts)[line] = {0, 0, true};
            (*linePlacements)[line] = {};
            if (line == std::numeric_limits<size_t>::max()) return false;
        }
        *outGroupPlacement = {group.kind, group.source_span, group.first_line, group.last_line,
                              mathX, mathLayout.width, groupHeight};
        outGroupPlacement->math_baseline_offset_px =
            static_cast<int>(verticalPadding) + mathLayout.baseline;
        return true;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] NoteTableCellAlignment ToTableAlignment(TableCellAlign alignment) noexcept {
    switch (alignment) {
    case TableCellAlign::Center:
        return NoteTableCellAlignment::Center;
    case TableCellAlign::Right:
        return NoteTableCellAlignment::Right;
    case TableCellAlign::Left:
    case TableCellAlign::Default:
    default:
        return NoteTableCellAlignment::Left;
    }
}

struct MeasuredTableRow {
    NoteRenderSourceLinePlan source_line;
    std::vector<int> cell_widths;
    uint32_t height_px = 0;
    bool has_inline_math = false;
    bool header = false;
    bool divider = false;
};

[[nodiscard]] bool MeasureTableRunWidth(HDC hdc,
                                        const NoteSyntaxSnapshot& syntax,
                                        const NoteRenderSourceLinePlan& sourceLine,
                                        const NoteRenderSourceRun& run,
                                        int tabWidthPx,
                                        int currentWidthPx,
                                        int* outWidthPx,
                                        uint32_t* inOutHeightPx) noexcept {
    if (!hdc || !outWidthPx || !inOutHeightPx || tabWidthPx <= 0 || currentWidthPx < 0) {
        return false;
    }
    ScopedRunFont font(hdc, sourceLine, run);
    if (!font.valid()) return false;
    NoteRenderFinalDisplayRun display;
    if (BuildNoteRenderFinalDisplayRun(syntax, run, &display) !=
        NoteRenderFinalDisplayRunBuildResult::Built) {
        return false;
    }
    if (run.kind == NoteRenderSourceRunKind::InlineMath) {
        mathrender::Layout mathLayout;
        if (!MeasureInlineMathGraphic(hdc, display, std::max<LONG>(1, font.metrics().tmHeight),
                                      &mathLayout) || mathLayout.width < 0 ||
            mathLayout.height <= 0) {
            return false;
        }
        if (mathLayout.width > std::numeric_limits<int>::max() - currentWidthPx ||
            static_cast<uint32_t>(mathLayout.height) > *inOutHeightPx) {
            return false;
        }
        *outWidthPx = mathLayout.width;
        *inOutHeightPx = static_cast<uint32_t>(mathLayout.height);
        return true;
    }
    std::vector<int> prefix;
    if (!MeasurePrefixWithTabs(hdc, display.display_text, currentWidthPx, 0, tabWidthPx, &prefix) ||
        prefix.empty()) {
        return false;
    }
    const int width = prefix.back();
    if (width < 0 || width > std::numeric_limits<int>::max() - currentWidthPx) return false;
    *outWidthPx = width;
    uint32_t styledRowHeight = 0;
    if (!ScaleLineHeight(std::max<LONG>(1, font.metrics().tmHeight),
                         font.line_height_permille(), &styledRowHeight)) {
        return false;
    }
    *inOutHeightPx = std::max(*inOutHeightPx, styledRowHeight);
    return true;
}

[[nodiscard]] bool MeasureTableGroup(
    const NoteRenderSourcePlan& sourcePlan,
    const NoteRenderLayoutKey& layoutKey,
    const NoteRenderFinalGdiMeasurementOptions& options,
    HDC hdc,
    TEXTMETRICW baseMetrics,
    int spaceWidthPx,
    const NoteRenderAtomicGroup& group,
    std::vector<NoteRenderLineLayout>* lineLayouts,
    std::vector<NoteRenderLinePlacement>* linePlacements,
    NoteRenderAtomicGroupPlacement* outGroupPlacement) noexcept {
    if (!lineLayouts || !linePlacements || !outGroupPlacement || !sourcePlan.syntax() ||
        group.kind != NoteRenderAtomicGroupKind::Table || group.first_line > group.last_line ||
        group.last_line.value >= lineLayouts->size() || group.last_line.value >= linePlacements->size() ||
        layoutKey.tab_columns == 0 ||
        layoutKey.tab_columns > static_cast<uint32_t>(std::numeric_limits<int>::max() / spaceWidthPx)) {
        return false;
    }
    const uint64_t leftPadding = std::max<uint32_t>(layoutKey.horizontal_padding_px,
                                                     options.minimum_left_padding_px);
    if (leftPadding >= layoutKey.client_width_px ||
        leftPadding > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    int tableX = static_cast<int>(leftPadding);
    const int tabWidth = static_cast<int>(layoutKey.tab_columns) * spaceWidthPx;
    try {
        std::vector<MeasuredTableRow> rows;
        rows.reserve(group.last_line.value - group.first_line.value + 1);
        size_t columnCount = 0;
        size_t tableBlock = NoteRenderSourceRun::kNoTableBlock;
        for (size_t line = group.first_line.value; line <= group.last_line.value; ++line) {
            MeasuredTableRow row;
            if (!sourcePlan.ResolveLine({line}, &row.source_line)) return false;
            row.header = row.source_line.decoration.has(NoteRenderSourceLineDecorationTableHeader);
            row.divider = row.source_line.decoration.has(NoteRenderSourceLineDecorationTableDivider);
            row.height_px = std::max<LONG>(1, baseMetrics.tmHeight);
            for (const NoteRenderSourceRun& run : row.source_line.runs) {
                if (run.table_column == NoteRenderSourceRun::kNoTableColumn) {
                    if (run.kind != NoteRenderSourceRunKind::HiddenSyntax) return false;
                    continue;
                }
                if (run.table_column_count == 0 || run.table_column >= run.table_column_count ||
                    (tableBlock != NoteRenderSourceRun::kNoTableBlock &&
                     run.table_block != tableBlock)) {
                    return false;
                }
                tableBlock = run.table_block;
                row.has_inline_math = row.has_inline_math ||
                    run.kind == NoteRenderSourceRunKind::InlineMath;
                columnCount = std::max(columnCount, run.table_column_count);
                if (row.cell_widths.size() < run.table_column_count) {
                    row.cell_widths.resize(run.table_column_count, 0);
                }
                int runWidth = 0;
                uint32_t runHeight = row.height_px;
                if (!MeasureTableRunWidth(hdc, *sourcePlan.syntax(), row.source_line, run,
                                          tabWidth, row.cell_widths[run.table_column], &runWidth,
                                          &runHeight) ||
                    runWidth > std::numeric_limits<int>::max() - row.cell_widths[run.table_column]) {
                    return false;
                }
                row.cell_widths[run.table_column] += runWidth;
                row.height_px = std::max(row.height_px, runHeight);
            }
            if (row.divider) {
                const int dividerHeight = ResolveNoteTableDividerHeightPx(1);
                if (dividerHeight <= 0) return false;
                row.height_px = static_cast<uint32_t>(dividerHeight);
            } else if (row.height_px > std::numeric_limits<uint32_t>::max() - 2u) {
                return false;
            } else {
                row.height_px += 2u;
            }
            rows.push_back(std::move(row));
            if (line == std::numeric_limits<size_t>::max()) return false;
        }
        if (columnCount == 0 || tableBlock == NoteRenderSourceRun::kNoTableBlock) return false;
        // A table is one atomic grid, so its origin follows the first visible
        // content row's line-level `<d=N>` value.  Per-cell anchors are not a
        // table feature in the legacy renderer and deliberately remain
        // column-layout controlled here.
        for (const MeasuredTableRow& row : rows) {
            if (row.divider) continue;
            int containerInset = 0;
            if (!ContainerInsetPx(row.source_line, options, spaceWidthPx, &containerInset) ||
                tableX > std::numeric_limits<int>::max() - containerInset) return false;
            tableX += containerInset;
            int indentColumns = 0;
            if (!ResolveLineIndentColumns(sourcePlan, row.source_line,
                                          std::max<LONG>(1, baseMetrics.tmHeight),
                                          &indentColumns) ||
                !AddIndentColumnsToX(tableX, indentColumns, spaceWidthPx, &tableX) ||
                tableX < 0) {
                return false;
            }
            break;
        }
        NoteTableLayoutInput tableInput;
        tableInput.column_count = columnCount;
        tableInput.minimum_content_width_px = std::max(1, spaceWidthPx * 2);
        tableInput.cell_horizontal_padding_px = std::max(1, spaceWidthPx / 2);
        tableInput.border_width_px = 1;
        for (const MeasuredTableRow& row : rows) {
            for (size_t column = 0; column < row.cell_widths.size(); ++column) {
                tableInput.cell_measures.push_back({column, row.cell_widths[column]});
            }
        }
        const NoteTableLayout tableLayout = ResolveNoteTableLayout(tableInput);
        if (!tableLayout.valid || tableLayout.columns.size() != columnCount ||
            tableLayout.total_width_px <= 0 || tableX >
                std::numeric_limits<int>::max() - tableLayout.total_width_px) {
            return false;
        }
        NoteRenderAtomicGroupPlacement groupPlacement{
            group.kind, group.source_span, group.first_line, group.last_line,
            tableX, tableLayout.total_width_px, 0};
        for (const NoteTableColumnLayout& column : tableLayout.columns) {
            groupPlacement.table_columns.push_back(
                {column.left_border_x_px, column.content_x_px, column.content_width_px,
                 column.right_border_x_px});
        }
        uint32_t tableTop = 0;
        for (size_t rowOffset = 0; rowOffset < rows.size(); ++rowOffset) {
            const size_t line = group.first_line.value + rowOffset;
            const MeasuredTableRow& row = rows[rowOffset];
            if (row.height_px == 0 || tableTop > std::numeric_limits<uint32_t>::max() - row.height_px) {
                return false;
            }
            (*lineLayouts)[line] = {row.height_px,
                                    static_cast<uint32_t>(tableX + tableLayout.total_width_px), false};
            groupPlacement.table_rows.push_back(
                {{line}, tableTop, row.height_px, row.header, row.divider});
            tableTop += row.height_px;
        }
        if (tableTop == 0) return false;
        groupPlacement.height_px = tableTop;
        for (size_t rowOffset = 0; rowOffset < rows.size(); ++rowOffset) {
            const size_t line = group.first_line.value + rowOffset;
            const MeasuredTableRow& row = rows[rowOffset];
            NoteRenderLinePlacement placement;
            std::vector<FlowCursor> cursors(columnCount);
            std::vector<bool> initialized(columnCount, false);
            size_t currentColumn = NoteRenderSourceRun::kNoTableColumn;
            for (const NoteRenderSourceRun& run : row.source_line.runs) {
                if (run.table_column == NoteRenderSourceRun::kNoTableColumn) {
                    if (run.kind != NoteRenderSourceRunKind::HiddenSyntax) return false;
                    const int hiddenX = currentColumn == NoteRenderSourceRun::kNoTableColumn
                        ? tableX + tableLayout.columns.front().left_border_x_px
                        : tableX + tableLayout.columns[currentColumn].right_border_x_px;
                    NoteRenderFinalDisplayRun display;
                    if (BuildNoteRenderFinalDisplayRun(*sourcePlan.syntax(), run, &display) !=
                            NoteRenderFinalDisplayRunBuildResult::Built ||
                        !display.display_text.empty()) {
                        return false;
                    }
                    FlowCursor hiddenCursor{hiddenX, hiddenX + 1, tableX, hiddenX, 0};
                    NoteRenderRunPlacement runPlacement;
                    if (!BuildRunPlacement(hdc, display, run, false, tabWidth, row.height_px,
                                           &hiddenCursor, &runPlacement)) {
                        return false;
                    }
                    placement.runs.push_back(std::move(runPlacement));
                    continue;
                }
                if (run.table_block != tableBlock || run.table_column >= columnCount) return false;
                const size_t column = run.table_column;
                if (!initialized[column]) {
                    const NoteTableColumnLayout& columnLayout = tableLayout.columns[column];
                    const int contentWidth = column < row.cell_widths.size()
                        ? row.cell_widths[column] : 0;
                    const int alignmentOffset = ResolveNoteTableCellContentOffset(
                        columnLayout, contentWidth, ToTableAlignment(run.table_cell_align));
                    const int cellLeft = tableX + columnLayout.content_x_px;
                    const int cellRight = cellLeft + columnLayout.content_width_px;
                    if (alignmentOffset < 0 || cellLeft > std::numeric_limits<int>::max() - alignmentOffset ||
                        cellLeft + alignmentOffset > cellRight) {
                        return false;
                    }
                    cursors[column] = {cellLeft, cellRight, cellLeft,
                                       cellLeft + alignmentOffset, 0};
                    initialized[column] = true;
                }
                ScopedRunFont font(hdc, row.source_line, run);
                if (!font.valid()) return false;
                NoteRenderFinalDisplayRun display;
                if (BuildNoteRenderFinalDisplayRun(*sourcePlan.syntax(), run, &display) !=
                    NoteRenderFinalDisplayRunBuildResult::Built) {
                    return false;
                }
                NoteRenderRunPlacement runPlacement;
                const bool built = run.kind == NoteRenderSourceRunKind::InlineMath
                    ? BuildInlineMathPlacement(hdc, display, run,
                                               std::max<LONG>(1, font.metrics().tmHeight),
                                               row.height_px, options.inline_math_vertical_alignment,
                                               &cursors[column], &runPlacement)
                    : BuildRunPlacement(hdc, display, run, false, tabWidth, row.height_px,
                                        &cursors[column], &runPlacement,
                                        std::max<LONG>(1, font.metrics().tmHeight),
                                        row.has_inline_math
                                            ? options.inline_math_vertical_alignment
                                            : NoteRenderInlineMathVerticalAlignment::Top);
                if (!built) return false;
                currentColumn = column;
                placement.runs.push_back(std::move(runPlacement));
            }
            if (!AddContainerDecorations(row.source_line, layoutKey, options, spaceWidthPx,
                                         row.height_px, &placement)) return false;
            (*linePlacements)[line] = std::move(placement);
        }
        *outGroupPlacement = std::move(groupPlacement);
        return true;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool MeasureCompleteImpl(
    const NoteRenderSourcePlan& sourcePlan,
    const NoteRenderLayoutKey& layoutKey,
    const NoteRenderFinalGdiMeasurementOptions& options,
    HDC hdc,
    NoteRenderFinalMeasuredFrame* out) noexcept {
    if (!out || !hdc || !options.valid() || !sourcePlan.valid() || !layoutKey.valid()) return false;
    TEXTMETRICW baseMetrics{};
    SIZE space{};
    if (!GetTextMetricsW(hdc, &baseMetrics) || !GetTextExtentPoint32W(hdc, L" ", 1, &space)) {
        return false;
    }
    const int spaceWidth = std::max<LONG>(1, space.cx > 0 ? space.cx : baseMetrics.tmAveCharWidth);
    try {
        std::vector<NoteRenderAtomicGroup> sourceGroups;
        if (!sourcePlan.CopyAtomicGroups(&sourceGroups)) return false;
        std::vector<NoteRenderAtomicGroup> measuredGroups;
        for (const NoteRenderAtomicGroup& group : sourceGroups) {
            if (group.kind == NoteRenderAtomicGroupKind::Table ||
                group.kind == NoteRenderAtomicGroupKind::BlockMath) {
                measuredGroups.push_back(group);
            }
        }
        NoteRenderFinalMeasuredFrame candidate;
        candidate.line_layouts.resize(sourcePlan.line_count());
        candidate.line_placements.resize(sourcePlan.line_count());
        candidate.atomic_group_placements.reserve(measuredGroups.size());
        size_t nextGroup = 0;
        for (size_t line = 0; line < sourcePlan.line_count();) {
            if (nextGroup < measuredGroups.size() &&
                measuredGroups[nextGroup].first_line.value == line) {
                NoteRenderAtomicGroupPlacement groupPlacement;
                const NoteRenderAtomicGroup& group = measuredGroups[nextGroup];
                const bool measured = group.kind == NoteRenderAtomicGroupKind::BlockMath
                    ? MeasureBlockMathGroup(sourcePlan, layoutKey, options, hdc, baseMetrics, group,
                                            &candidate.line_layouts, &candidate.line_placements,
                                            &groupPlacement)
                    : MeasureTableGroup(sourcePlan, layoutKey, options, hdc, baseMetrics, spaceWidth,
                                        group, &candidate.line_layouts, &candidate.line_placements,
                                        &groupPlacement);
                if (!measured) {
                    return false;
                }
                candidate.atomic_group_placements.push_back(std::move(groupPlacement));
                if (group.last_line.value == std::numeric_limits<size_t>::max()) {
                    return false;
                }
                line = group.last_line.value + 1;
                ++nextGroup;
                continue;
            }
            if (nextGroup < measuredGroups.size() &&
                measuredGroups[nextGroup].first_line.value < line) {
                return false;
            }
            if (!MeasureLine(sourcePlan, layoutKey, options, hdc, baseMetrics, spaceWidth, {line},
                             &candidate.line_layouts[line], &candidate.line_placements[line])) {
                return false;
            }
            ++line;
        }
        if (nextGroup != measuredGroups.size()) return false;
        *out = std::move(candidate);
        return true;
    } catch (...) {
        return false;
    }
}

template <typename Frame>
[[nodiscard]] bool MeasureRangeImpl(const NoteRenderSourcePlan& sourcePlan,
                                    const NoteRenderLayoutKey& layoutKey,
                                    const NoteRenderFinalGdiMeasurementOptions& options,
                                    HDC hdc,
                                    LineIndex firstLine,
                                    LineIndex lastLineExclusive,
                                    Frame* out) noexcept {
    if (!out || !hdc || !options.valid() || !sourcePlan.valid() || !layoutKey.valid() ||
        firstLine >= lastLineExclusive || lastLineExclusive.value > sourcePlan.line_count()) {
        return false;
    }
    TEXTMETRICW baseMetrics{};
    if (!GetTextMetricsW(hdc, &baseMetrics)) return false;
    SIZE space{};
    if (!GetTextExtentPoint32W(hdc, L" ", 1, &space)) return false;
    const int spaceWidth = std::max<LONG>(1, space.cx > 0 ? space.cx : baseMetrics.tmAveCharWidth);
    try {
        Frame candidate;
        const size_t count = lastLineExclusive.value - firstLine.value;
        candidate.line_layouts.reserve(count);
        candidate.line_placements.reserve(count);
        for (size_t index = firstLine.value; index < lastLineExclusive.value; ++index) {
            NoteRenderLineLayout lineLayout;
            NoteRenderLinePlacement linePlacement;
            if (!MeasureLine(sourcePlan, layoutKey, options, hdc, baseMetrics, spaceWidth,
                             {index}, &lineLayout, &linePlacement)) {
                return false;
            }
            candidate.line_layouts.push_back(lineLayout);
            candidate.line_placements.push_back(std::move(linePlacement));
        }
        *out = std::move(candidate);
        return true;
    } catch (...) {
        return false;
    }
}

struct RawLineMeasurementContext {
    HDC hdc = nullptr;
    bool word_wrap = false;
    int content_left_px = 0;
    int content_right_px = 0;
    int tab_width_px = 0;
    uint32_t row_height_px = 0;
};

[[nodiscard]] bool BuildRawLineMeasurementContext(
    const NoteRenderLayoutKey& layoutKey,
    const NoteRenderFinalGdiMeasurementOptions& options,
    HDC hdc,
    RawLineMeasurementContext* out) noexcept {
    if (!out || !hdc || !options.valid() || !layoutKey.valid()) return false;
    TEXTMETRICW baseMetrics{};
    SIZE space{};
    if (!GetTextMetricsW(hdc, &baseMetrics) ||
        !GetTextExtentPoint32W(hdc, L" ", 1, &space)) {
        return false;
    }
    const int spaceWidth = std::max<LONG>(
        1, space.cx > 0 ? space.cx : baseMetrics.tmAveCharWidth);
    const uint64_t leftPadding = std::max<uint32_t>(
        layoutKey.horizontal_padding_px, options.minimum_left_padding_px);
    const uint64_t rightPadding = std::max<uint32_t>(
        layoutKey.horizontal_padding_px, options.minimum_right_padding_px);
    if (leftPadding >= layoutKey.client_width_px || rightPadding >= layoutKey.client_width_px ||
        leftPadding + rightPadding >= layoutKey.client_width_px ||
        leftPadding > static_cast<uint64_t>(std::numeric_limits<int>::max()) ||
        layoutKey.tab_columns == 0 ||
        layoutKey.tab_columns >
            static_cast<uint32_t>(std::numeric_limits<int>::max() / spaceWidth)) {
        return false;
    }
    const int contentLeft = static_cast<int>(leftPadding);
    const int contentRight = static_cast<int>(layoutKey.client_width_px - rightPadding);
    const int tabWidth = static_cast<int>(layoutKey.tab_columns) * spaceWidth;
    const uint32_t rowHeight = static_cast<uint32_t>(std::max<LONG>(1, baseMetrics.tmHeight));
    *out = {hdc, layoutKey.word_wrap, contentLeft, contentRight, tabWidth, rowHeight};
    return true;
}

[[nodiscard]] bool MeasureRawTextLine(
    const RawLineMeasurementContext& context,
    const NoteRenderFinalRawTextLine& request,
    NoteRenderFinalRawLineSurface* out) noexcept {
    if (!out || !context.hdc || request.text_span.end < request.text_span.start ||
        request.text_span.end.value - request.text_span.start.value != request.text.size()) {
        return false;
    }
    try {
        NoteRenderFinalRawLineSurface rawLine;
        rawLine.line_index = request.line_index;
        rawLine.source_span = request.text_span;
        rawLine.display.display_text = request.text;
        rawLine.display.boundaries.reserve(request.text.size() + 1);
        for (size_t offset = 0; offset <= request.text.size(); ++offset) {
            if (request.text_span.start.value > std::numeric_limits<size_t>::max() - offset) {
                return false;
            }
            rawLine.display.boundaries.push_back(
                {{request.text_span.start.value + offset}, offset});
            if (offset == std::numeric_limits<size_t>::max()) return false;
        }

        NoteRenderRunPlacement runPlacement;
        if (request.text.empty()) {
            runPlacement.source_span = rawLine.source_span;
            runPlacement.x_px = context.content_left_px;
            runPlacement.width_px = 0;
            runPlacement.fragments.push_back(
                {rawLine.source_span, context.content_left_px, 0, 0, context.row_height_px, 0});
            runPlacement.boundaries.push_back(
                {request.text_span.start, context.content_left_px, 0});
            rawLine.layout = {context.row_height_px,
                              static_cast<uint32_t>(context.content_left_px), false};
        } else {
            NoteRenderSourceRun sourceRun;
            sourceRun.source_span = rawLine.source_span;
            sourceRun.display_source_span = rawLine.source_span;
            sourceRun.line_index = request.line_index;
            sourceRun.kind = NoteRenderSourceRunKind::Text;
            FlowCursor cursor{context.content_left_px, context.content_right_px,
                              context.content_left_px, context.content_left_px, 0};
            if (!BuildRunPlacement(context.hdc, rawLine.display, sourceRun, context.word_wrap,
                                   context.tab_width_px, context.row_height_px, &cursor,
                                   &runPlacement) ||
                cursor.visual_row >= std::numeric_limits<uint32_t>::max() /
                    context.row_height_px ||
                runPlacement.x_px > std::numeric_limits<int>::max() - runPlacement.width_px) {
                return false;
            }
            const uint64_t height = static_cast<uint64_t>(cursor.visual_row + 1) *
                context.row_height_px;
            const int extent = runPlacement.x_px + runPlacement.width_px;
            if (height == 0 || height > std::numeric_limits<uint32_t>::max() || extent < 0) {
                return false;
            }
            rawLine.layout = {static_cast<uint32_t>(height),
                              static_cast<uint32_t>(extent), false};
        }
        rawLine.placement.runs.push_back(std::move(runPlacement));
        *out = std::move(rawLine);
        return true;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool MeasureRawLinesImpl(
    const NoteTextCore& textCore,
    const NoteRenderLayoutKey& layoutKey,
    const NoteRenderFinalGdiMeasurementOptions& options,
    HDC hdc,
    LineIndex firstLine,
    LineIndex lastLineExclusive,
    std::vector<NoteRenderFinalRawLineSurface>* out) noexcept {
    if (!out || !textCore.valid() || firstLine >= lastLineExclusive ||
        lastLineExclusive.value > textCore.logical_line_count()) {
        return false;
    }
    RawLineMeasurementContext context;
    if (!BuildRawLineMeasurementContext(layoutKey, options, hdc, &context)) return false;
    try {
        std::vector<NoteRenderFinalRawLineSurface> candidate;
        candidate.reserve(lastLineExclusive.value - firstLine.value);
        for (size_t line = firstLine.value; line < lastLineExclusive.value; ++line) {
            const auto sourceLine = NoteSourceLineMap::LineAt(textCore.source_line_map(), {line});
            if (!sourceLine.has_value()) return false;
            NoteRenderFinalRawTextLine request;
            request.line_index = {line};
            request.text_span = {sourceLine->start, sourceLine->content_end};
            request.text = textCore.CopyRawRange(
                sourceLine->start, sourceLine->line.content_length);
            if (request.text.size() != sourceLine->line.content_length) return false;
            NoteRenderFinalRawLineSurface measured;
            if (!MeasureRawTextLine(context, request, &measured)) return false;
            candidate.push_back(std::move(measured));
        }
        *out = std::move(candidate);
        return true;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool MeasureRawTextLinesImpl(
    const NoteRenderLayoutKey& layoutKey,
    const NoteRenderFinalGdiMeasurementOptions& options,
    HDC hdc,
    const std::vector<NoteRenderFinalRawTextLine>& lines,
    std::vector<NoteRenderFinalRawLineSurface>* out) noexcept {
    if (!out || lines.empty()) return false;
    RawLineMeasurementContext context;
    if (!BuildRawLineMeasurementContext(layoutKey, options, hdc, &context)) return false;
    try {
        std::vector<NoteRenderFinalRawLineSurface> candidate;
        candidate.reserve(lines.size());
        std::optional<LineIndex> previous;
        for (const NoteRenderFinalRawTextLine& request : lines) {
            if (previous.has_value() && request.line_index <= *previous) return false;
            NoteRenderFinalRawLineSurface measured;
            if (!MeasureRawTextLine(context, request, &measured)) return false;
            candidate.push_back(std::move(measured));
            previous = request.line_index;
        }
        *out = std::move(candidate);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

uint32_t ResolveNoteRenderInlineMathTopOffset(
    uint32_t rowHeightPx, uint32_t mathHeightPx,
    NoteRenderInlineMathVerticalAlignment alignment) noexcept {
    if (mathHeightPx > rowHeightPx) return 0;
    switch (alignment) {
    case NoteRenderInlineMathVerticalAlignment::Top:
        return 0;
    case NoteRenderInlineMathVerticalAlignment::Center:
        return (rowHeightPx - mathHeightPx) / 2;
    case NoteRenderInlineMathVerticalAlignment::Bottom:
        return rowHeightPx - mathHeightPx;
    }
    return rowHeightPx - mathHeightPx;
}

bool NoteRenderFinalGdiMeasurementOptions::valid() const noexcept {
    return minimum_left_padding_px <= static_cast<uint32_t>(std::numeric_limits<int>::max()) &&
           minimum_right_padding_px <= static_cast<uint32_t>(std::numeric_limits<int>::max()) &&
           list_continuation_columns > 0 && quote_gutter_columns > 0 &&
           code_block_inset_columns > 0 &&
           inline_math_vertical_alignment >= NoteRenderInlineMathVerticalAlignment::Top &&
           inline_math_vertical_alignment <= NoteRenderInlineMathVerticalAlignment::Bottom;
}

NoteRenderFinalGdiMeasurementProvider::NoteRenderFinalGdiMeasurementProvider(
    HDC hdc, NoteRenderFinalGdiMeasurementOptions options) noexcept
    : hdc_(hdc), options_(options) {}

bool NoteRenderFinalGdiMeasurementProvider::Measure(
    const NoteRenderSourcePlan& sourcePlan,
    const NoteRenderLayoutKey& layoutKey,
    NoteRenderFinalMeasuredFrame* out) const noexcept {
    return MeasureCompleteImpl(sourcePlan, layoutKey, options_, hdc_, out);
}

bool NoteRenderFinalGdiMeasurementProvider::MeasureReplacement(
    const NoteRenderSourcePlan& sourcePlan,
    const NoteRenderLayoutKey& layoutKey,
    LineIndex firstLine,
    LineIndex lastLineExclusive,
    NoteRenderFinalMeasuredLineRange* out) const noexcept {
    return MeasureRangeImpl(sourcePlan, layoutKey, options_, hdc_, firstLine,
                            lastLineExclusive, out);
}

bool NoteRenderFinalGdiMeasurementProvider::MeasureRawLines(
    const NoteTextCore& textCore,
    const NoteRenderLayoutKey& layoutKey,
    LineIndex firstLine,
    LineIndex lastLineExclusive,
    std::vector<NoteRenderFinalRawLineSurface>* out) const noexcept {
    return MeasureRawLinesImpl(textCore, layoutKey, options_, hdc_, firstLine,
                               lastLineExclusive, out);
}

bool NoteRenderFinalGdiMeasurementProvider::MeasureRawTextLines(
    const NoteRenderLayoutKey& layoutKey,
    const std::vector<NoteRenderFinalRawTextLine>& lines,
    std::vector<NoteRenderFinalRawLineSurface>* out) const noexcept {
    return MeasureRawTextLinesImpl(layoutKey, options_, hdc_, lines, out);
}

} // namespace note
