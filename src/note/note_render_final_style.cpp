#include "note/note_render_final_style.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <limits>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace note {
namespace {

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

[[nodiscard]] std::wstring_view TrimMatchingQuotes(std::wstring_view value) noexcept {
    value = TrimAsciiWhitespace(value);
    if (value.size() >= 2 && ((value.front() == L'\'' && value.back() == L'\'') ||
                              (value.front() == L'"' && value.back() == L'"'))) {
        value.remove_prefix(1);
        value.remove_suffix(1);
        value = TrimAsciiWhitespace(value);
    }
    return value;
}

[[nodiscard]] bool EndsWithAsciiIgnoreCase(std::wstring_view value,
                                            std::wstring_view suffix) noexcept {
    return value.size() >= suffix.size() &&
        AsciiEqualsIgnoreCase(value.substr(value.size() - suffix.size()), suffix);
}

[[nodiscard]] bool TryParseFontHeight(std::wstring_view value,
                                      int current_height_px,
                                      int* out_height_px) noexcept {
    if (!out_height_px || current_height_px <= 0) return false;
    value = TrimAsciiWhitespace(value);
    if (EndsWithAsciiIgnoreCase(value, L"px")) value.remove_suffix(2);
    value = TrimAsciiWhitespace(value);
    if (value.empty() || value.size() >= 64) return false;
    wchar_t buffer[64]{};
    std::copy(value.begin(), value.end(), buffer);
    wchar_t* end = nullptr;
    const double parsed = std::wcstod(buffer, &end);
    if (end == buffer || *end != L'\0' || !std::isfinite(parsed)) return false;
    const bool relative = value.front() == L'+' || value.front() == L'-';
    const double requested = relative ? static_cast<double>(current_height_px) + parsed : parsed;
    if (!(requested > 0.0) || requested > static_cast<double>(std::numeric_limits<int>::max())) {
        return false;
    }
    constexpr int kMinimumFontHeightPx = 6;
    constexpr int kMaximumFontHeightPx = 256;
    *out_height_px = std::clamp(static_cast<int>(std::lround(requested)),
                                kMinimumFontHeightPx, kMaximumFontHeightPx);
    return true;
}

[[nodiscard]] bool TryParseLineHeightPermille(std::wstring_view value,
                                               uint32_t current_permille,
                                               uint32_t* out_permille) noexcept {
    if (!out_permille || current_permille < 1000) return false;
    value = TrimAsciiWhitespace(value);
    if (value.empty() || value.size() >= 64) return false;
    wchar_t buffer[64]{};
    std::copy(value.begin(), value.end(), buffer);
    wchar_t* end = nullptr;
    const double parsed = std::wcstod(buffer, &end);
    if (end == buffer || *end != L'\0' || !std::isfinite(parsed)) return false;
    const bool relative = value.front() == L'+' || value.front() == L'-';
    constexpr double kPermillePerUnit = 100.0;
    constexpr double kMaximumUnits = 60.0;
    double currentUnits = static_cast<double>(current_permille - 1000) / kPermillePerUnit;
    double units = relative ? currentUnits + parsed : parsed;
    if (!std::isfinite(units)) return false;
    units = std::clamp(units, 0.0, kMaximumUnits);
    const double requested = 1000.0 + std::round(units * kPermillePerUnit);
    if (requested < 1000.0 || requested > static_cast<double>(std::numeric_limits<uint32_t>::max())) {
        return false;
    }
    *out_permille = static_cast<uint32_t>(requested);
    return true;
}

[[nodiscard]] int HeadingScalePercent(int level) noexcept {
    switch (level) {
    case 1: return 160;
    case 2: return 145;
    case 3: return 130;
    case 4: return 120;
    case 5: return 110;
    default: return 100;
    }
}

[[nodiscard]] bool TryParseWholeInt(std::wstring_view value, int* out) noexcept {
    if (!out) return false;
    value = TrimAsciiWhitespace(value);
    if (value.empty() || value.size() >= 64) return false;
    wchar_t buffer[64]{};
    std::copy(value.begin(), value.end(), buffer);
    wchar_t* end = nullptr;
    const long parsed = std::wcstol(buffer, &end, 10);
    if (end == buffer || *end != L'\0' ||
        parsed < static_cast<long>(std::numeric_limits<int>::min()) ||
        parsed > static_cast<long>(std::numeric_limits<int>::max())) {
        return false;
    }
    *out = static_cast<int>(parsed);
    return true;
}

[[nodiscard]] bool TryParseAnchor(std::wstring_view value,
                                  NoteRenderFinalHorizontalAnchor* outAnchor,
                                  int* outOffsetColumns) noexcept {
    if (!outAnchor || !outOffsetColumns) return false;
    value = TrimAsciiWhitespace(value);
    if (value.empty()) return false;
    NoteRenderFinalHorizontalAnchor anchor = NoteRenderFinalHorizontalAnchor::None;
    size_t prefixLength = 0;
    if (value.size() >= 4 && AsciiEqualsIgnoreCase(value.substr(0, 4), L"left")) {
        anchor = NoteRenderFinalHorizontalAnchor::Left;
        prefixLength = 4;
    } else if (value.size() >= 6 && AsciiEqualsIgnoreCase(value.substr(0, 6), L"center")) {
        anchor = NoteRenderFinalHorizontalAnchor::Center;
        prefixLength = 6;
    } else if (value.size() >= 5 && AsciiEqualsIgnoreCase(value.substr(0, 5), L"right")) {
        anchor = NoteRenderFinalHorizontalAnchor::Right;
        prefixLength = 5;
    } else {
        return false;
    }
    int offset = 0;
    const std::wstring_view suffix = TrimAsciiWhitespace(value.substr(prefixLength));
    if (!suffix.empty() && !TryParseWholeInt(suffix, &offset)) return false;
    *outAnchor = anchor;
    *outOffsetColumns = offset;
    return true;
}

} // namespace

bool ResolveNoteRenderFinalRunFontStyle(
    const NoteRenderSourceLinePlan& line,
    const NoteRenderSourceRun& run,
    int baseFontHeightPx,
    NoteRenderFinalRunFontStyle* out) noexcept {
    if (!out || baseFontHeightPx <= 0) return false;
    try {
        NoteRenderFinalRunFontStyle resolved;
        resolved.font_height_px = baseFontHeightPx;
        bool explicitFontHeight = false;
        for (const NoteRenderSourceStyleAttribute& attribute : run.styles) {
            switch (attribute.kind) {
            case StyleKind::Bold:
                resolved.bold = true;
                break;
            case StyleKind::Italic:
                resolved.italic = true;
                break;
            case StyleKind::FontFamily: {
                const std::wstring_view face = TrimMatchingQuotes(attribute.value);
                if (!face.empty() && face.size() < LF_FACESIZE) {
                    resolved.font_family = face;
                }
                break;
            }
            case StyleKind::FontSize: {
                int height = 0;
                if (TryParseFontHeight(attribute.value, resolved.font_height_px, &height)) {
                    resolved.font_height_px = height;
                    explicitFontHeight = true;
                }
                break;
            }
            case StyleKind::LineHeight: {
                uint32_t height = 0;
                if (TryParseLineHeightPermille(
                        attribute.value, resolved.line_height_permille, &height)) {
                    resolved.line_height_permille = height;
                }
                break;
            }
            case StyleKind::Indent: {
                int columns = 0;
                if (TryParseWholeInt(attribute.value, &columns)) {
                    resolved.has_indent = true;
                    resolved.indent_columns = columns;
                }
                break;
            }
            case StyleKind::Anchor: {
                NoteRenderFinalHorizontalAnchor anchor =
                    NoteRenderFinalHorizontalAnchor::None;
                int offsetColumns = 0;
                if (TryParseAnchor(attribute.value, &anchor, &offsetColumns)) {
                    resolved.anchor = anchor;
                    resolved.anchor_offset_columns = offsetColumns;
                }
                break;
            }
            default:
                break;
            }
        }
        if (!explicitFontHeight) {
            const int headingLevel = std::max(line.decoration.heading_level, run.heading_level);
            const int64_t scaled = static_cast<int64_t>(baseFontHeightPx) *
                HeadingScalePercent(headingLevel) / 100;
            if (scaled <= 0 || scaled > std::numeric_limits<int>::max()) return false;
            resolved.font_height_px = static_cast<int>(scaled);
        }
        // Literal code uses its documented monospaced face unless a future
        // source-plan decoration introduces an explicit code-face attribute.
        if (line.decoration.has(NoteRenderSourceLineDecorationCodeBlock) ||
            run.kind == NoteRenderSourceRunKind::InlineCode) {
            resolved.font_family = L"Consolas";
        }
        *out = resolved;
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace note
