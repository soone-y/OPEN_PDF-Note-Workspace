#pragma once

#include "ui/quarter_turn_bitmap.h"
#include "ui/vertical_text_gdi.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

// The editor and reader supply immutable text and paper dimensions. This
// boundary creates one owned grayscale coverage DIB; it never reads application
// settings or changes annotation geometry. The caller owns the returned bitmap.
namespace annotation_text {
inline constexpr double kRasterDpi = 576.0;
inline constexpr int kPad = 24; // Main editor's 4 px at 96 DPI.

[[nodiscard]] inline int FontHeight(double pt) {
    if (!std::isfinite(pt) || pt<=0 || pt*kRasterDpi/72.0>100000) return 0;
    return std::max(1, static_cast<int>(std::lround(pt * kRasterDpi / 72.0)));
}

inline int IdeographicWidth(HDC dc) {
    SIZE size{}; const wchar_t sample = 0x4e00;
    if (GetTextExtentPoint32W(dc, &sample, 1, &size) && size.cx > 0) return size.cx;
    TEXTMETRICW metrics{};
    if (!GetTextMetricsW(dc, &metrics)) return 0;
    return metrics.tmMaxCharWidth > 0 ? metrics.tmMaxCharWidth : metrics.tmAveCharWidth;
}

inline void AdjustSpaces(std::wstring_view text, std::vector<int>& widths, int ideographic) {
    int extra = 0, previous = 0;
    for (size_t i=0; i<widths.size(); ++i) {
        const int original = widths[i];
        if (text[i] == 0x3000 && original-previous < ideographic) extra += ideographic-(original-previous);
        widths[i] = original+extra; previous = original;
    }
}

inline void DrawLine(HDC dc, int x, int y, std::wstring_view line, int ideographic) {
    if (line.empty()) return;
    std::vector<int> advances(line.size()); SIZE size{};
    if (GetTextExtentExPointW(dc, line.data(), static_cast<int>(line.size()),
                             std::numeric_limits<int>::max(), nullptr, advances.data(), &size)) {
        AdjustSpaces(line, advances, ideographic);
        int previous = 0;
        for (auto& advance : advances) { const int current = advance; advance -= previous; previous = current; }
        ExtTextOutW(dc, x, y, 0, nullptr, line.data(), static_cast<UINT>(line.size()), advances.data());
    } else TextOutW(dc, x, y, line.data(), static_cast<int>(line.size()));
}

// Legacy sidecars can omit saved lines. Match the editor's measured wrapping,
// including its one-pixel tolerance and full-width space advances.
[[nodiscard]] inline std::wstring Wrap(HDC dc, const std::wstring& text, int width) {
    std::wstring result;
    const int ideographic = IdeographicWidth(dc);
    size_t paragraph = 0;
    bool first = true;
    const auto append = [&](std::wstring_view line) {
        if (!first) result += L'\n';
        result.append(line); first = false;
    };
    while (paragraph <= text.size()) {
        const size_t newline = text.find(L'\n', paragraph);
        const size_t end = newline == std::wstring::npos ? text.size() : newline;
        const auto line = std::wstring_view(text).substr(paragraph, end-paragraph);
        if (line.empty()) append(line);
        else {
            std::vector<int> widths(line.size()); SIZE size{};
            if (!GetTextExtentExPointW(dc,line.data(),static_cast<int>(line.size()),
                    std::numeric_limits<int>::max(),nullptr,widths.data(),&size)) return text;
            AdjustSpaces(line,widths,ideographic);
            size_t start=0;
            while (start<line.size()) {
                size_t best=start, space=start;
                const int base = start ? widths[start-1] : 0;
                for (size_t i=start; i<line.size(); ++i) {
                    if (widths[i]-base <= width+1) {
                        best=i+1;
                        if (line[i]==L' ' || line[i]==L'\t') space=i+1;
                    } else { if (space>start) best=space; else if (best==start) best=i+1; break; }
                }
                if (best<=start) best=start+1;
                append(line.substr(start,best-start)); start=best;
                while (start<line.size() && (line[start]==L' ' || line[start]==L'\t')) ++start;
            }
        }
        if (newline==std::wstring::npos) break;
        paragraph=newline+1;
    }
    return result;
}

[[nodiscard]] inline HBITMAP BuildMask(HDC reference, double widthPt, double heightPt,
    int fontPx, const std::wstring& face, const std::wstring& text,
    int turns, bool vertical, bool savedLines, int* outputWidth, int* outputHeight) {
    if (!reference || !outputWidth || !outputHeight || fontPx<=0 || fontPx>100000 ||
        !std::isfinite(widthPt) || !std::isfinite(heightPt) || widthPt<=0 || heightPt<=0) return nullptr;
    const double scaledW=widthPt*kRasterDpi/72.0, scaledH=heightPt*kRasterDpi/72.0;
    if (scaledW>16384 || scaledH>16384 || scaledW*scaledH>64'000'000) return nullptr;
    int width=std::max(1,static_cast<int>(std::lround(scaledW)));
    int height=std::max(1,static_cast<int>(std::lround(scaledH)));
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=width; info.bmiHeader.biHeight=-height;
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
    void* bits=nullptr;
    HBITMAP bitmap=CreateDIBSection(reference,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    if (!bitmap || !bits) { if (bitmap) DeleteObject(bitmap); return nullptr; }
    HDC dc=CreateCompatibleDC(reference);
    if (!dc) { DeleteObject(bitmap); return nullptr; }
    const auto oldBitmap=SelectObject(dc,bitmap);
    std::fill_n(static_cast<DWORD*>(bits),static_cast<size_t>(width)*height,0);
    LOGFONTW description{}; description.lfHeight=-fontPx;
    description.lfQuality=ANTIALIASED_QUALITY; description.lfCharSet=DEFAULT_CHARSET;
    wcsncpy_s(description.lfFaceName,face.c_str(),LF_FACESIZE-1);
    HFONT font=CreateFontIndirectW(&description);
    if (!font) { SelectObject(dc,oldBitmap); DeleteDC(dc); DeleteObject(bitmap); return nullptr; }
    const auto oldFont=SelectObject(dc,font);
    SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(255,255,255));
    TEXTMETRICW metrics{}; GetTextMetricsW(dc,&metrics);
    const int lineHeight=std::max(1,static_cast<int>(metrics.tmHeight));
    try {
    const auto renderText=vertical || savedLines ? text : Wrap(dc,text,std::max(1,width-kPad*2));
    if (vertical) {
        vertical_text::Draw(dc,RECT{kPad,kPad,width-kPad,height-kPad},renderText,lineHeight);
    } else {
        const int ideographic=IdeographicWidth(dc);
        size_t start=0; int y=kPad;
        while (start<=renderText.size() && y<height-kPad) {
            const size_t newline=renderText.find(L'\n',start);
            const size_t end=newline==std::wstring::npos ? renderText.size() : newline;
            DrawLine(dc,kPad,y,std::wstring_view(renderText).substr(start,end-start),ideographic);
            y+=lineHeight;
            if (newline==std::wstring::npos) break;
            start=newline+1;
        }
    }
    } catch (...) {
        SelectObject(dc,oldFont); DeleteObject(font); SelectObject(dc,oldBitmap); DeleteDC(dc); DeleteObject(bitmap);
        return nullptr;
    }
    SelectObject(dc,oldFont); DeleteObject(font); SelectObject(dc,oldBitmap); DeleteDC(dc);
    GdiFlush();
    auto* pixels=static_cast<DWORD*>(bits);
    for (size_t i=0;i<static_cast<size_t>(width)*height;++i) {
        const DWORD value=pixels[i];
        const DWORD coverage=std::max(static_cast<BYTE>(value),std::max(static_cast<BYTE>(value>>8),static_cast<BYTE>(value>>16)));
        pixels[i]=coverage*0x010101u;
    }
    if (page_rotation::Normalize(turns)) {
        HBITMAP rotated=ui::RotateTopDownDibQuarterTurns(reference,bitmap,turns,&width,&height);
        DeleteObject(bitmap); bitmap=rotated;
        if (!bitmap) return nullptr;
    }
    *outputWidth=width; *outputHeight=height;
    return bitmap;
}

[[nodiscard]] inline bool BlendMask(HDC target, const RECT& bounds, HDC source,
    int sourceWidth, int sourceHeight, COLORREF color, BYTE boost) noexcept {
    RECT clip{}, visible{};
    const int clipType = GetClipBox(target, &clip);
    if (clipType == ERROR) return false;
    if (clipType == NULLREGION || !IntersectRect(&visible, &clip, &bounds)) return true;
    const int width = visible.right - visible.left;
    const int height = visible.bottom - visible.top;
    const size_t pixelCount = static_cast<size_t>(width) * height;
    if (pixelCount > 64'000'000) return false;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(target, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) { if (bitmap) DeleteObject(bitmap); return false; }
    HDC memory = CreateCompatibleDC(target);
    if (!memory) { DeleteObject(bitmap); return false; }
    const auto previous = SelectObject(memory, bitmap);
    if (!previous) { DeleteDC(memory); DeleteObject(bitmap); return false; }

    // AlphaBlend forces COLORONCOLOR when resizing. Filter grayscale coverage
    // first, then tint it and blend at 1:1 so narrow strokes keep their coverage.
    SetStretchBltMode(memory, HALFTONE);
    SetBrushOrgEx(memory, 0, 0, nullptr);
    bool painted = StretchBlt(memory, bounds.left - visible.left, bounds.top - visible.top,
        bounds.right - bounds.left, bounds.bottom - bounds.top,
        source, 0, 0, sourceWidth, sourceHeight, SRCCOPY) != 0;
    if (painted) {
        GdiFlush();
        auto* pixels = static_cast<DWORD*>(bits);
        for (size_t i = 0; i < pixelCount; ++i) {
            const DWORD alpha = pixels[i] & 0xFFu;
            pixels[i] = (alpha << 24) |
                (((GetRValue(color) * alpha + 127) / 255) << 16) |
                (((GetGValue(color) * alpha + 127) / 255) << 8) |
                ((GetBValue(color) * alpha + 127) / 255);
        }
        painted = AlphaBlend(target, visible.left, visible.top, width, height,
            memory, 0, 0, width, height, {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA}) != 0;
        if (painted && boost) {
            AlphaBlend(target, visible.left, visible.top, width, height,
                memory, 0, 0, width, height, {AC_SRC_OVER, 0, boost, AC_SRC_ALPHA});
        }
    }
    SelectObject(memory, previous);
    DeleteDC(memory);
    DeleteObject(bitmap);
    return painted;
}
} // namespace annotation_text
