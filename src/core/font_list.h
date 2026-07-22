#pragma once

#include "core/theme_types.h"

#include <windows.h>
#include <string>
#include <algorithm>

enum class FontStyle {
    JapaneseGothic,
    JapaneseMincho,
    Latin,
    Symbol,
    Unknown,
};

inline const wchar_t* GetFontStyleLabel(FontStyle style) {
    switch (style) {
        case FontStyle::JapaneseGothic:
            return L"ゴシック体";
        case FontStyle::JapaneseMincho:
            return L"明朝体";
        case FontStyle::Latin:
            return L"英数字用";
        case FontStyle::Symbol:
            return L"記号用";
        default:
            return nullptr;
    }
}

inline bool FontStyleHasJapaneseGlyphs(FontStyle style) {
    return style == FontStyle::JapaneseGothic || style == FontStyle::JapaneseMincho;
}

inline constexpr const wchar_t* kFontPreviewSample = L"日本語名: 日本語サンプル / 表示例: あア漢 Aa123";
inline constexpr const wchar_t* kFontNoJapanesePreviewLabel = L"日本語なし";

struct FontListEntry {
    const wchar_t* displayName;
    const wchar_t* faceName;
    FontStyle style;
};

inline constexpr FontListEntry kDefaultFontList[] = {
    // Japanese fonts first (preferred for JP text).
    { L"メイリオ UI", L"Meiryo UI", FontStyle::JapaneseGothic },
    { L"メイリオ", L"Meiryo", FontStyle::JapaneseGothic },
    { L"游ゴシック UI", L"Yu Gothic UI", FontStyle::JapaneseGothic },
    { L"游ゴシック", L"Yu Gothic", FontStyle::JapaneseGothic },
    { L"游明朝", L"Yu Mincho", FontStyle::JapaneseMincho },
    { L"ＭＳ UI ゴシック", L"MS UI Gothic", FontStyle::JapaneseGothic },
    { L"ＭＳ ゴシック", L"MS Gothic", FontStyle::JapaneseGothic },
    { L"ＭＳ Ｐゴシック", L"MS PGothic", FontStyle::JapaneseGothic },
    { L"ＭＳ 明朝", L"MS Mincho", FontStyle::JapaneseMincho },
    { L"ＭＳ Ｐ明朝", L"MS PMincho", FontStyle::JapaneseMincho },
    { L"BIZ UDゴシック", L"BIZ UD Gothic", FontStyle::JapaneseGothic },
    { L"BIZ UDPゴシック", L"BIZ UDP Gothic", FontStyle::JapaneseGothic },
    { L"BIZ UD明朝", L"BIZ UD Mincho", FontStyle::JapaneseMincho },
    { L"BIZ UDP明朝", L"BIZ UDP Mincho", FontStyle::JapaneseMincho },
    { L"Noto Sans JP", L"Noto Sans JP", FontStyle::JapaneseGothic },
    { L"Noto Serif JP", L"Noto Serif JP", FontStyle::JapaneseMincho },
    { L"Noto Sans CJK JP", L"Noto Sans CJK JP", FontStyle::JapaneseGothic },
    { L"Noto Serif CJK JP", L"Noto Serif CJK JP", FontStyle::JapaneseMincho },
    { L"Source Han Sans JP", L"Source Han Sans JP", FontStyle::JapaneseGothic },
    { L"Source Han Serif JP", L"Source Han Serif JP", FontStyle::JapaneseMincho },
    { L"Hiragino Sans", L"Hiragino Sans", FontStyle::JapaneseGothic },
    { L"ヒラギノ角ゴ ProN", L"Hiragino Kaku Gothic ProN", FontStyle::JapaneseGothic },

    // A small set of bundled LibreOffice Latin fonts. Loaded with FR_PRIVATE when present.
    { L"Liberation Sans", L"Liberation Sans", FontStyle::Latin },
    { L"Liberation Serif", L"Liberation Serif", FontStyle::Latin },
    { L"Liberation Mono", L"Liberation Mono", FontStyle::Latin },
    { L"Carlito", L"Carlito", FontStyle::Latin },
    { L"Caladea", L"Caladea", FontStyle::Latin },
    { L"OpenSymbol", L"OpenSymbol", FontStyle::Symbol },

    // Non-Japanese fonts later.
    { L"Segoe UI", L"Segoe UI", FontStyle::Latin },
    { L"Segoe UI Symbol", L"Segoe UI Symbol", FontStyle::Symbol },
    { L"Calibri", L"Calibri", FontStyle::Latin },
    { L"Cambria", L"Cambria", FontStyle::Latin },
    { L"Arial", L"Arial", FontStyle::Latin },
    { L"Arial Black", L"Arial Black", FontStyle::Latin },
    { L"Times New Roman", L"Times New Roman", FontStyle::Latin },
    { L"Georgia", L"Georgia", FontStyle::Latin },
    { L"Verdana", L"Verdana", FontStyle::Latin },
    { L"Tahoma", L"Tahoma", FontStyle::Latin },
    { L"Consolas", L"Consolas", FontStyle::Latin },
    { L"Courier New", L"Courier New", FontStyle::Latin },
};

inline constexpr FontListEntry kSelectableFontList[] = {
    { L"メイリオ", L"Meiryo", FontStyle::JapaneseGothic },
    { L"游ゴシック", L"Yu Gothic", FontStyle::JapaneseGothic },
    { L"游明朝", L"Yu Mincho", FontStyle::JapaneseMincho },
    { L"Segoe UI", L"Segoe UI", FontStyle::Latin },
    { L"Arial", L"Arial", FontStyle::Latin },
    { L"Times New Roman", L"Times New Roman", FontStyle::Latin },
    { L"Georgia", L"Georgia", FontStyle::Latin },
    { L"Consolas", L"Consolas", FontStyle::Latin },
    { L"Courier New", L"Courier New", FontStyle::Latin },
};

inline void AppendDefaultFontListToCombo(HWND combo) {
    if (!combo) return;
    for (const auto& entry : kSelectableFontList) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry.displayName));
    }
}

inline void ResetAndFillDefaultFontListCombo(HWND combo) {
    if (!combo) return;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    AppendDefaultFontListToCombo(combo);
}

inline std::wstring GetComboItemText(HWND combo, int index) {
    if (!combo || index < 0) return L"";
    wchar_t buf[256]{};
    int len = static_cast<int>(SendMessageW(combo, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(buf)));
    if (len < 0) return L"";
    buf[std::min<int>(len, 255)] = L'\0';
    return std::wstring(buf);
}

inline int MeasureFontComboItemHeight(HWND combo, int pt) {
    HDC hdc = GetDC(combo);
    if (!hdc) return 20;
    int heightPx = -MulDiv(pt, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    ReleaseDC(combo, hdc);
    return std::max(20, (-heightPx) + 6);
}

inline std::wstring ResolveFontFaceName(const std::wstring& displayName);
inline std::wstring ResolveFontDisplayName(const std::wstring& faceName);

inline const FontListEntry* FindFontListEntry(const std::wstring& displayName) {
    for (const auto& entry : kDefaultFontList) {
        if (displayName == entry.displayName) return &entry;
    }
    return nullptr;
}

inline void DrawFontComboItem(const DRAWITEMSTRUCT* dis, int pt, const wchar_t* sample,
                              const ThemeColors& theme) {
    if (!dis || dis->itemID == static_cast<UINT>(-1)) return;
    HWND combo = dis->hwndItem;
    std::wstring name = GetComboItemText(combo, static_cast<int>(dis->itemID));
    std::wstring face = ResolveFontFaceName(name);
    if (face.empty()) face = name;

    LOGFONTW lf{};
    lf.lfWeight = FW_NORMAL;
    wcsncpy_s(lf.lfFaceName, face.c_str(), _TRUNCATE);
    lf.lfHeight = -MulDiv(pt, GetDeviceCaps(dis->hDC, LOGPIXELSY), 72);
    HFONT font = CreateFontIndirectW(&lf);

    const bool selected = (dis->itemState & ODS_SELECTED) != 0;
    COLORREF bg = selected ? theme.selectionBg : theme.panelBg;
    COLORREF fg = selected ? theme.selectionText : theme.panelText;
    HBRUSH br = CreateSolidBrush(bg);
    FillRect(dis->hDC, &dis->rcItem, br);
    DeleteObject(br);

    int oldBk = SetBkMode(dis->hDC, TRANSPARENT);
    COLORREF oldFg = SetTextColor(dis->hDC, fg);
    HFONT oldFont = font ? static_cast<HFONT>(SelectObject(dis->hDC, font)) : nullptr;

    std::wstring text = name;
    const auto* entry = FindFontListEntry(name);
    if (entry && entry->faceName && std::wstring(entry->faceName) != name) {
        text += L" (";
        text += entry->faceName;
        text += L")";
    }
    if (entry) {
        if (const wchar_t* label = GetFontStyleLabel(entry->style); label && *label) {
            text += L" [";
            text += label;
            text += L"]";
        }
    }
    if (sample && *sample) {
        std::wstring preview = sample;
        const bool hasJapanese = entry && FontStyleHasJapaneseGlyphs(entry->style);
        std::wstring jpName = hasJapanese ? ResolveFontDisplayName(face) : kFontNoJapanesePreviewLabel;
        if (!jpName.empty()) {
            const std::wstring target = L"日本語サンプル";
            size_t pos = preview.find(target);
            if (pos != std::wstring::npos) {
                preview.replace(pos, target.size(), jpName);
            }
        }
        text += L"  ";
        text += preview;
    }
    RECT rc = dis->rcItem;
    rc.left += 6;
    DrawTextW(dis->hDC, text.c_str(), static_cast<int>(text.size()), &rc,
              DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

    if (oldFont) SelectObject(dis->hDC, oldFont);
    SetTextColor(dis->hDC, oldFg);
    SetBkMode(dis->hDC, oldBk);
    if (font) DeleteObject(font);

    if (dis->itemState & ODS_FOCUS) {
        DrawFocusRect(dis->hDC, &dis->rcItem);
    }
}

inline const wchar_t* FindFontFaceName(const std::wstring& displayName) {
    for (const auto& entry : kDefaultFontList) {
        if (displayName == entry.displayName) return entry.faceName;
    }
    return nullptr;
}

inline const wchar_t* FindFontDisplayName(const std::wstring& faceName) {
    for (const auto& entry : kDefaultFontList) {
        if (faceName == entry.faceName) return entry.displayName;
    }
    return nullptr;
}

inline std::wstring ResolveFontFaceName(const std::wstring& displayName) {
    if (const auto* face = FindFontFaceName(displayName)) return face;
    return displayName;
}

inline std::wstring ResolveFontDisplayName(const std::wstring& faceName) {
    if (const auto* display = FindFontDisplayName(faceName)) return display;
    return faceName;
}
