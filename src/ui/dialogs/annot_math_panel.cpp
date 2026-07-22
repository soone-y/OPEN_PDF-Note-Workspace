#include "ui/dialogs/annot_math_panel.h"

#include "core/app_core.h"
#include "pdf_view/pdf_view.h"
#include "bridge/view_bridge.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <cwctype>

namespace {

bool s_revealLatestAnnotOnNextRefresh = false;
constexpr UINT kAnnotPanelContextMove = 1;
constexpr UINT kAnnotPanelContextOpenLink = 2;
constexpr UINT kAnnotPanelContextCopy = 3;
constexpr UINT kAnnotPanelContextDuplicate = 4;
constexpr UINT kAnnotPanelContextBringFront = 5;
constexpr UINT kAnnotPanelContextSendBack = 6;
constexpr UINT kAnnotPanelContextDelete = 7;
constexpr UINT kAnnotPanelContextProperties = 8;
constexpr UINT kAnnotPanelContextEdit = 9;

constexpr int kAnnotInspectorView = 101;
constexpr int kAnnotInspectorEdit = 102;
constexpr int kAnnotInspectorContent = 103;
constexpr int kAnnotInspectorColor = 104;
constexpr int kAnnotInspectorWidth = 105;
constexpr int kAnnotInspectorAlpha = 106;
constexpr int kAnnotInspectorShapeKind = 107;
constexpr int kAnnotInspectorShapeDrawMode = 108;
constexpr int kAnnotInspectorArrowHead = 109;
constexpr int kAnnotInspectorDash = 110;
constexpr int kAnnotInspectorClose = 111;
constexpr int kAnnotInspectorDetails = 112;
constexpr int kAnnotInspectorApply = 113;
constexpr int kAnnotInspectorValidation = 114;

struct AnnotInspectorCtx {
    HWND owner = nullptr;
    int index = -1;
    bool editable = false;
    bool initializing = false;
    Annotation::Type type = Annotation::Type::TextBox;
};

std::wstring AnnotTypeLabel(const Annotation& ann) {
    const auto& ui = GetUiText();
    switch (ann.type) {
    case Annotation::Type::MarkerText: return ui.btnModeMarker;
    case Annotation::Type::TextColor: return IsEnglishUi() ? L"Text color" : L"文字色";
    case Annotation::Type::MarkerFree: return ui.btnModeMarkerFree;
    case Annotation::Type::TextBox: return ui.btnModeText;
    case Annotation::Type::Line: return ui.btnModeLine;
    case Annotation::Type::Arrow: return ui.btnModeArrow;
    case Annotation::Type::Wave: return ui.btnModeWave;
    case Annotation::Type::Freehand: return ui.btnModeFreehand;
    case Annotation::Type::Shape: return ui.btnModeShape;
    case Annotation::Type::LinkMarker: return IsEnglishUi() ? L"Link" : L"リンク";
    case Annotation::Type::MathBox: return IsEnglishUi() ? L"Math" : L"数式";
    default:
        return IsEnglishUi() ? L"Annotation" : L"注釈";
    }
}

std::wstring AnnotTextSnippet(const std::wstring& text) {
    std::wstring t = TrimWhitespace(text);
    for (auto& ch : t) {
        if (ch == L'\r' || ch == L'\n') ch = L' ';
    }
    const size_t kMax = 24;
    if (t.size() > kMax) {
        t = t.substr(0, kMax) + L"...";
    }
    return t;
}

std::wstring AnnotListLabel(const Annotation& ann) {
    std::wstring page = (ann.pageIndex >= 0) ? (L"P" + std::to_wstring(ann.pageIndex + 1)) : L"P?";
    std::wstring label = page + L" " + AnnotTypeLabel(ann);
    if (ann.type == Annotation::Type::TextBox || ann.type == Annotation::Type::MathBox) {
        std::wstring snippet = AnnotTextSnippet(ann.text);
        if (!snippet.empty()) {
            label += L": " + snippet;
        }
    } else if (ann.type == Annotation::Type::Shape) {
        std::wstring shapeLabel;
        switch (ann.shapeKind) {
        case ShapeKind::Square: shapeLabel = IsEnglishUi() ? L"Square" : L"正方形"; break;
        case ShapeKind::Rectangle: shapeLabel = IsEnglishUi() ? L"Rectangle" : L"長方形"; break;
        case ShapeKind::Diamond: shapeLabel = IsEnglishUi() ? L"Diamond" : L"菱形"; break;
        case ShapeKind::EquilateralTriangle: shapeLabel = IsEnglishUi() ? L"Equilateral" : L"正三角形"; break;
        case ShapeKind::Triangle: shapeLabel = IsEnglishUi() ? L"Triangle" : L"三角形"; break;
        case ShapeKind::Ellipse: shapeLabel = IsEnglishUi() ? L"Ellipse" : L"円"; break;
        case ShapeKind::Circle: shapeLabel = IsEnglishUi() ? L"Circle" : L"正円"; break;
        case ShapeKind::RotatedEllipse: shapeLabel = IsEnglishUi() ? L"Rotated ellipse" : L"斜め円"; break;
        default: break;
        }
        if (!shapeLabel.empty()) {
            label += L": " + shapeLabel;
            if (ann.shapeDrawMode == ShapeDrawMode::Outline) {
                label += IsEnglishUi() ? L" / Outline" : L" / 枠線";
            }
        }
    } else if (ann.type == Annotation::Type::LinkMarker) {
        if (!ann.linkId.empty()) {
            label += L": " + ann.linkId;
        }
    }
    return label;
}

std::wstring CopyableAnnotText(const Annotation& ann) {
    switch (ann.type) {
    case Annotation::Type::TextBox:
    case Annotation::Type::MathBox:
        return ann.text;
    case Annotation::Type::LinkMarker:
        return ann.linkId;
    default:
        return L"";
    }
}

bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (text.empty() || !OpenClipboard(owner)) return false;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        CloseClipboard();
        return false;
    }
    void* dst = GlobalLock(memory);
    if (!dst) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    std::memcpy(dst, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

void AddPointToAnnotBounds(double x,
                           double y,
                           bool& haveBounds,
                           double& minX,
                           double& maxX,
                           double& minY,
                           double& maxY) {
    if (!haveBounds) {
        minX = maxX = x;
        minY = maxY = y;
        haveBounds = true;
        return;
    }
    minX = std::min(minX, x);
    maxX = std::max(maxX, x);
    minY = std::min(minY, y);
    maxY = std::max(maxY, y);
}

bool GetAnnotJumpPoint(const Annotation& ann, double& outX, double& outY) {
    bool haveBounds = false;
    double minX = 0.0;
    double maxX = 0.0;
    double minY = 0.0;
    double maxY = 0.0;

    auto addRect = [&]() {
        AddPointToAnnotBounds(ann.x1, ann.y1, haveBounds, minX, maxX, minY, maxY);
        AddPointToAnnotBounds(ann.x2, ann.y2, haveBounds, minX, maxX, minY, maxY);
    };

    switch (ann.type) {
    case Annotation::Type::MarkerText:
    case Annotation::Type::TextColor:
        if (!ann.quads.empty()) {
            for (size_t i = 0; i + 1 < ann.quads.size(); i += 2) {
                AddPointToAnnotBounds(ann.quads[i], ann.quads[i + 1], haveBounds, minX, maxX, minY, maxY);
            }
        } else {
            addRect();
        }
        break;
    case Annotation::Type::MarkerFree:
    case Annotation::Type::Freehand:
        for (const auto& pt : ann.path) {
            AddPointToAnnotBounds(pt.x, pt.y, haveBounds, minX, maxX, minY, maxY);
        }
        if (!haveBounds) addRect();
        break;
    case Annotation::Type::Line:
    case Annotation::Type::Arrow:
    case Annotation::Type::Wave:
    case Annotation::Type::TextBox:
    case Annotation::Type::MathBox:
    case Annotation::Type::Shape:
        addRect();
        break;
    case Annotation::Type::LinkMarker:
        AddPointToAnnotBounds(ann.x1, ann.y1, haveBounds, minX, maxX, minY, maxY);
        break;
    default:
        AddPointToAnnotBounds(ann.x1, ann.y1, haveBounds, minX, maxX, minY, maxY);
        break;
    }

    if (!haveBounds) return false;
    outX = (minX + maxX) * 0.5;
    outY = (minY + maxY) * 0.5;
    return true;
}

std::wstring AnnotationInspectorDetails(const Annotation& ann) {
    const std::wstring page = ann.pageIndex >= 0 ? std::to_wstring(ann.pageIndex + 1) : L"?";
    const wchar_t* mode = ann.shapeDrawMode == ShapeDrawMode::Fill ? L"塗りつぶし" : L"枠線";
    const int opacity = static_cast<int>(std::lround(std::clamp(ann.alpha, 0.0, 1.0) * 100.0));
    std::wstring text = L"種類: " + AnnotTypeLabel(ann) +
        L"\r\nページ: " + page +
        L"\r\n座標: (" + std::to_wstring(ann.x1) + L", " + std::to_wstring(ann.y1) +
        L") - (" + std::to_wstring(ann.x2) + L", " + std::to_wstring(ann.y2) + L")" +
        L"\r\n色: #";
    const wchar_t hex[] = L"0123456789ABCDEF";
    for (BYTE value : { GetRValue(ann.color), GetGValue(ann.color), GetBValue(ann.color) }) {
        text.push_back(hex[value >> 4]);
        text.push_back(hex[value & 0x0F]);
    }
    text += L"\r\n幅: " + std::to_wstring(ann.width) + L" pt" +
        L"\r\n透明度: " + std::to_wstring(opacity) + L"%";
    if (ann.type == Annotation::Type::Shape) {
        text += L"\r\n描画: " + std::wstring(mode) + L"\r\n形状: " + ShapeKindToString(ann.shapeKind);
    }
    if (ann.type == Annotation::Type::Arrow) {
        text += L"\r\n矢印の頭: " + std::wstring(ann.arrowHead == ArrowHead::Double ? L"双方向" : L"単方向");
    }
    if (ann.type == Annotation::Type::Line || ann.type == Annotation::Type::Arrow) {
        text += L"\r\n線種: " + std::wstring(ann.dash.empty() ? L"実線" : L"破線");
    }
    if (ann.type == Annotation::Type::LinkMarker) {
        text += L"\r\nリンクID: " + ann.linkId + L"\r\nリンク先: " + ann.linkNotePath;
    }
    return text;
}

bool InspectorHasContent(const Annotation::Type type) {
    return type == Annotation::Type::TextBox || type == Annotation::Type::MathBox ||
        type == Annotation::Type::LinkMarker;
}

std::wstring InspectorColorLabel(COLORREF color) {
    const wchar_t hex[] = L"0123456789ABCDEF";
    std::wstring label = IsEnglishUi() ? L"Change color (#" : L"色を変更 (#";
    for (BYTE value : { GetRValue(color), GetGValue(color), GetBValue(color) }) {
        label.push_back(hex[value >> 4]);
        label.push_back(hex[value & 0x0F]);
    }
    return label + L")";
}

std::wstring InspectorNumberText(double value) {
    std::wstring text = std::to_wstring(value);
    while (!text.empty() && text.back() == L'0') text.pop_back();
    if (!text.empty() && text.back() == L'.') text.pop_back();
    return text.empty() ? L"0" : text;
}

void AddInspectorComboItem(HWND combo, const wchar_t* label, LPARAM value) {
    if (!combo) return;
    const int index = static_cast<int>(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label)));
    if (index >= 0) SendMessageW(combo, CB_SETITEMDATA, index, value);
}

void SelectInspectorComboValue(HWND combo, LPARAM wanted) {
    if (!combo) return;
    const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    for (int i = 0; i < count; ++i) {
        if (SendMessageW(combo, CB_GETITEMDATA, i, 0) == wanted) {
            SendMessageW(combo, CB_SETCURSEL, i, 0);
            return;
        }
    }
}

LPARAM InspectorComboValue(HWND hWnd, int id, LPARAM fallback) {
    HWND combo = GetDlgItem(hWnd, id);
    const int selected = combo ? static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0)) : -1;
    const LRESULT value = selected >= 0 ? SendMessageW(combo, CB_GETITEMDATA, selected, 0) : CB_ERR;
    return value == CB_ERR ? fallback : static_cast<LPARAM>(value);
}

bool TryReadInspectorNumber(HWND hWnd, int id, double minimum, double maximum, double& outValue) {
    HWND control = GetDlgItem(hWnd, id);
    const int length = control ? GetWindowTextLengthW(control) : 0;
    if (!control || length <= 0) return false;
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    const wchar_t* start = text.c_str();
    while (iswspace(*start)) ++start;
    wchar_t* end = nullptr;
    errno = 0;
    const double value = std::wcstod(start, &end);
    while (end && iswspace(*end)) ++end;
    if (start == end || !end || *end != L'\0' || errno == ERANGE || !std::isfinite(value) ||
        value < minimum || value > maximum) {
        return false;
    }
    outValue = value;
    return true;
}

void ShowInspectorValidation(HWND hWnd, int controlId, const wchar_t* message) {
    SetWindowTextW(GetDlgItem(hWnd, kAnnotInspectorValidation), message);
    HWND control = GetDlgItem(hWnd, controlId);
    if (control) {
        SetFocus(control);
        SendMessageW(control, CB_SETEDITSEL, 0, MAKELPARAM(0, -1));
    }
}

void ClearInspectorValidation(HWND hWnd) {
    SetWindowTextW(GetDlgItem(hWnd, kAnnotInspectorValidation), L"");
}

void UpdateInspectorDetails(HWND hWnd, const AnnotInspectorCtx* ctx) {
    if (!ctx || ctx->index < 0 || ctx->index >= static_cast<int>(g_annots.size())) return;
    SetWindowTextW(GetDlgItem(hWnd, kAnnotInspectorDetails),
                   AnnotationInspectorDetails(g_annots[static_cast<size_t>(ctx->index)]).c_str());
}

std::wstring InspectorEditableText(const Annotation& ann) {
    if (ann.type == Annotation::Type::TextBox || ann.type == Annotation::Type::MathBox) return ann.text;
    if (ann.type == Annotation::Type::LinkMarker) return ann.linkNotePath;
    return L"";
}

void SetAnnotInspectorEditable(HWND hWnd, AnnotInspectorCtx* ctx, bool editable) {
    if (!ctx) return;
    ctx->editable = editable && !IsPdfPreviewReadOnlyActive();
    CheckRadioButton(hWnd, kAnnotInspectorView, kAnnotInspectorEdit,
                     ctx->editable ? kAnnotInspectorEdit : kAnnotInspectorView);
    HWND content = GetDlgItem(hWnd, kAnnotInspectorContent);
    if (content) EnableWindow(content, ctx->editable && InspectorHasContent(ctx->type));
    for (const int id : { kAnnotInspectorColor, kAnnotInspectorWidth, kAnnotInspectorAlpha,
                          kAnnotInspectorShapeKind, kAnnotInspectorShapeDrawMode,
                          kAnnotInspectorArrowHead, kAnnotInspectorDash }) {
        if (HWND control = GetDlgItem(hWnd, id)) EnableWindow(control, ctx->editable);
    }
}

bool ApplyAnnotInspectorContent(HWND hWnd, AnnotInspectorCtx* ctx) {
    if (!ctx || ctx->initializing || !ctx->editable || ctx->index < 0 ||
        ctx->index >= static_cast<int>(g_annots.size())) return false;
    Annotation after = g_annots[static_cast<size_t>(ctx->index)];
    HWND content = GetDlgItem(hWnd, kAnnotInspectorContent);
    const int length = content ? GetWindowTextLengthW(content) : 0;
    std::wstring value(static_cast<size_t>(std::max(0, length)) + 1, L'\0');
    if (content && length > 0) GetWindowTextW(content, value.data(), length + 1);
    value.resize(static_cast<size_t>(std::max(0, length)));
    if (after.type == Annotation::Type::TextBox || after.type == Annotation::Type::MathBox) {
        after.text = value;
        if (after.type == Annotation::Type::TextBox) after.textLines.clear();
    } else if (after.type == Annotation::Type::LinkMarker) {
        after.linkNotePath = value;
    } else {
        return false;
    }
    return UpdateAnnotationAtIndex(ctx->owner, ctx->index, after);
}

bool ApplyAnnotInspectorStyle(HWND hWnd, AnnotInspectorCtx* ctx, bool showValidation) {
    if (!ctx || ctx->initializing || !ctx->editable || ctx->index < 0 ||
        ctx->index >= static_cast<int>(g_annots.size())) return true;
    double width = 0.0;
    if (!TryReadInspectorNumber(hWnd, kAnnotInspectorWidth, 0.5, 24.0, width)) {
        if (showValidation) ShowInspectorValidation(hWnd, kAnnotInspectorWidth, L"幅は 0.5〜24 pt の数値で入力してください。");
        return false;
    }
    double opacity = 0.0;
    if (!TryReadInspectorNumber(hWnd, kAnnotInspectorAlpha, 0.0, 100.0, opacity)) {
        if (showValidation) ShowInspectorValidation(hWnd, kAnnotInspectorAlpha, L"透明度は 0〜100 の数値で入力してください。");
        return false;
    }
    Annotation after = g_annots[static_cast<size_t>(ctx->index)];
    after.width = width;
    after.alpha = opacity / 100.0;
    if (after.type == Annotation::Type::Shape) {
        after.shapeKind = static_cast<ShapeKind>(InspectorComboValue(hWnd, kAnnotInspectorShapeKind,
            static_cast<LPARAM>(after.shapeKind)));
        after.shapeDrawMode = static_cast<ShapeDrawMode>(InspectorComboValue(hWnd, kAnnotInspectorShapeDrawMode,
            static_cast<LPARAM>(after.shapeDrawMode)));
    }
    if (after.type == Annotation::Type::Arrow) {
        after.arrowHead = static_cast<ArrowHead>(InspectorComboValue(hWnd, kAnnotInspectorArrowHead,
            static_cast<LPARAM>(after.arrowHead)));
    }
    if (after.type == Annotation::Type::Line || after.type == Annotation::Type::Arrow) {
        const bool dashed = InspectorComboValue(hWnd, kAnnotInspectorDash, after.dash.empty() ? 0 : 1) != 0;
        after.dash = dashed ? std::vector<double>{ std::max(1.0, after.width * 4.0),
                                                    std::max(1.0, after.width * 2.0) }
                            : std::vector<double>{};
    }
    const bool changed = UpdateAnnotationAtIndex(ctx->owner, ctx->index, after);
    if (changed) UpdateInspectorDetails(hWnd, ctx);
    ClearInspectorValidation(hWnd);
    return true;
}

LRESULT CALLBACK AnnotInspectorProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<AnnotInspectorCtx*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    case WM_CREATE: {
        ctx = reinterpret_cast<AnnotInspectorCtx*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
        if (!ctx || ctx->index < 0 || ctx->index >= static_cast<int>(g_annots.size())) return -1;
        const Annotation& ann = g_annots[static_cast<size_t>(ctx->index)];
        ctx->type = ann.type;
        ctx->initializing = true;
        const bool hasContent = InspectorHasContent(ann.type);
        const int styleTop = hasContent ? 382 : 196;
        CreateWindowExW(0, L"BUTTON", L"プロパティ", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                        16, 8, 95, 24, hWnd, reinterpret_cast<HMENU>(kAnnotInspectorView), g_hInst, nullptr);
        CreateWindowExW(0, L"BUTTON", L"編集", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                        116, 8, 70, 24, hWnd, reinterpret_cast<HMENU>(kAnnotInspectorEdit), g_hInst, nullptr);
        CreateWindowExW(0, L"BUTTON", L"注釈情報", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                        8, 36, 496, 140, hWnd, nullptr, g_hInst, nullptr);
        HWND details = CreateWindowExW(0, L"STATIC", AnnotationInspectorDetails(ann).c_str(),
                                       WS_CHILD | WS_VISIBLE,
                                       20, 56, 468, 112, hWnd,
                                       reinterpret_cast<HMENU>(kAnnotInspectorDetails), g_hInst, nullptr);
        const wchar_t* label = (ann.type == Annotation::Type::LinkMarker) ? L"リンク先" :
            ((ann.type == Annotation::Type::TextBox || ann.type == Annotation::Type::MathBox) ? L"本文" : L"内容");
        if (hasContent) {
            CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                            8, 184, 496, 182, hWnd, nullptr, g_hInst, nullptr);
        }
        HWND contentLabel = CreateWindowExW(0, L"STATIC", label, WS_CHILD | (hasContent ? WS_VISIBLE : 0),
                        20, 204, 90, 20, hWnd, nullptr, g_hInst, nullptr);
        HWND content = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", InspectorEditableText(ann).c_str(),
                                       WS_CHILD | WS_VISIBLE | ES_MULTILINE | WS_VSCROLL,
                                       20, 226, 468, 126, hWnd,
                                       reinterpret_cast<HMENU>(kAnnotInspectorContent), g_hInst, nullptr);
        if (!hasContent) ShowWindow(content, SW_HIDE);
        CreateWindowExW(0, L"BUTTON", L"スタイル", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                        8, styleTop - 12, 496, 106, hWnd, nullptr, g_hInst, nullptr);
        CreateWindowExW(0, L"STATIC", L"表示", WS_CHILD | WS_VISIBLE,
                        20, styleTop, 55, 22, hWnd, nullptr, g_hInst, nullptr);
        HWND color = CreateWindowExW(0, L"BUTTON", InspectorColorLabel(ann.color).c_str(), WS_CHILD | WS_VISIBLE,
                                     70, styleTop - 3, 145, 26, hWnd,
                                     reinterpret_cast<HMENU>(kAnnotInspectorColor), g_hInst, nullptr);
        CreateWindowExW(0, L"STATIC", L"幅", WS_CHILD | WS_VISIBLE,
                        228, styleTop, 28, 22, hWnd, nullptr, g_hInst, nullptr);
        HWND width = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWN,
                                     254, styleTop - 3, 82, 180, hWnd,
                                     reinterpret_cast<HMENU>(kAnnotInspectorWidth), g_hInst, nullptr);
        for (const auto& option : { std::pair<const wchar_t*, int>{L"1.0", 10}, {L"1.5", 15},
                                    {L"2.0", 20}, {L"2.5", 25}, {L"4.0", 40},
                                    {L"6.0", 60}, {L"8.0", 80} }) {
            AddInspectorComboItem(width, option.first, option.second);
        }
        SelectInspectorComboValue(width, static_cast<LPARAM>(std::lround(ann.width * 10.0)));
        SetWindowTextW(width, InspectorNumberText(ann.width).c_str());
        CreateWindowExW(0, L"STATIC", L"pt", WS_CHILD | WS_VISIBLE,
                        338, styleTop, 20, 22, hWnd, nullptr, g_hInst, nullptr);
        CreateWindowExW(0, L"STATIC", L"透明度", WS_CHILD | WS_VISIBLE,
                        362, styleTop, 48, 22, hWnd, nullptr, g_hInst, nullptr);
        HWND alpha = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWN,
                                     412, styleTop - 3, 56, 180, hWnd,
                                     reinterpret_cast<HMENU>(kAnnotInspectorAlpha), g_hInst, nullptr);
        for (const auto& option : { std::pair<const wchar_t*, int>{L"20", 200}, {L"40", 400},
                                    {L"50", 500}, {L"60", 600}, {L"80", 800}, {L"100", 1000} }) {
            AddInspectorComboItem(alpha, option.first, option.second);
        }
        SelectInspectorComboValue(alpha, static_cast<LPARAM>(std::lround(ann.alpha * 1000.0)));
        SetWindowTextW(alpha, std::to_wstring(static_cast<int>(std::lround(ann.alpha * 100.0))).c_str());
        CreateWindowExW(0, L"STATIC", L"%", WS_CHILD | WS_VISIBLE,
                        470, styleTop, 16, 22, hWnd, nullptr, g_hInst, nullptr);

        if (ann.type == Annotation::Type::Shape) {
            CreateWindowExW(0, L"STATIC", L"形状", WS_CHILD | WS_VISIBLE,
                            20, styleTop + 34, 40, 22, hWnd, nullptr, g_hInst, nullptr);
            HWND kind = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                                        60, styleTop + 31, 136, 190, hWnd,
                                        reinterpret_cast<HMENU>(kAnnotInspectorShapeKind), g_hInst, nullptr);
            const std::pair<const wchar_t*, ShapeKind> kinds[] = {
                {L"正円", ShapeKind::Circle}, {L"円", ShapeKind::Ellipse}, {L"斜め円", ShapeKind::RotatedEllipse},
                {L"正方形", ShapeKind::Square}, {L"長方形", ShapeKind::Rectangle}, {L"菱形", ShapeKind::Diamond},
                {L"三角形", ShapeKind::Triangle}, {L"正三角形", ShapeKind::EquilateralTriangle}
            };
            for (const auto& option : kinds) AddInspectorComboItem(kind, option.first, static_cast<LPARAM>(option.second));
            SelectInspectorComboValue(kind, static_cast<LPARAM>(ann.shapeKind));
            CreateWindowExW(0, L"STATIC", L"描画", WS_CHILD | WS_VISIBLE,
                            214, styleTop + 34, 40, 22, hWnd, nullptr, g_hInst, nullptr);
            HWND drawMode = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                                            254, styleTop + 31, 120, 120, hWnd,
                                            reinterpret_cast<HMENU>(kAnnotInspectorShapeDrawMode), g_hInst, nullptr);
            AddInspectorComboItem(drawMode, L"塗りつぶし", static_cast<LPARAM>(ShapeDrawMode::Fill));
            AddInspectorComboItem(drawMode, L"枠線", static_cast<LPARAM>(ShapeDrawMode::Outline));
            SelectInspectorComboValue(drawMode, static_cast<LPARAM>(ann.shapeDrawMode));
        } else if (ann.type == Annotation::Type::Arrow) {
            CreateWindowExW(0, L"STATIC", L"矢印の頭", WS_CHILD | WS_VISIBLE,
                            20, styleTop + 34, 60, 22, hWnd, nullptr, g_hInst, nullptr);
            HWND arrowHead = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                                             84, styleTop + 31, 110, 120, hWnd,
                                             reinterpret_cast<HMENU>(kAnnotInspectorArrowHead), g_hInst, nullptr);
            AddInspectorComboItem(arrowHead, L"単方向", static_cast<LPARAM>(ArrowHead::Single));
            AddInspectorComboItem(arrowHead, L"双方向", static_cast<LPARAM>(ArrowHead::Double));
            SelectInspectorComboValue(arrowHead, static_cast<LPARAM>(ann.arrowHead));
        }
        if (ann.type == Annotation::Type::Line || ann.type == Annotation::Type::Arrow) {
            CreateWindowExW(0, L"STATIC", L"線種", WS_CHILD | WS_VISIBLE,
                            214, styleTop + 34, 40, 22, hWnd, nullptr, g_hInst, nullptr);
            HWND dash = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                                        254, styleTop + 31, 120, 120, hWnd,
                                        reinterpret_cast<HMENU>(kAnnotInspectorDash), g_hInst, nullptr);
            AddInspectorComboItem(dash, L"実線", 0);
            AddInspectorComboItem(dash, L"破線", 1);
            SelectInspectorComboValue(dash, ann.dash.empty() ? 0 : 1);
        }
        CreateWindowExW(0, L"BUTTON", L"適用", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                        302, styleTop + 66, 90, 26, hWnd,
                        reinterpret_cast<HMENU>(kAnnotInspectorApply), g_hInst, nullptr);
        CreateWindowExW(0, L"BUTTON", L"閉じる", WS_CHILD | WS_VISIBLE,
                        398, styleTop + 66, 90, 26, hWnd,
                        reinterpret_cast<HMENU>(kAnnotInspectorClose), g_hInst, nullptr);
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                        20, styleTop + 70, 276, 20, hWnd,
                        reinterpret_cast<HMENU>(kAnnotInspectorValidation), g_hInst, nullptr);
        SetUIFont(details); SetUIFont(content); SetUIFont(contentLabel); SetUIFont(color); SetUIFont(width); SetUIFont(alpha);
        SetAnnotInspectorEditable(hWnd, ctx, ctx->editable);
        ctx->initializing = false;
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);
        if (!ctx) break;
        if (id == kAnnotInspectorView && code == BN_CLICKED) {
            SetAnnotInspectorEditable(hWnd, ctx, false);
            return 0;
        }
        if (id == kAnnotInspectorEdit && code == BN_CLICKED) {
            SetAnnotInspectorEditable(hWnd, ctx, true);
            return 0;
        }
        if (id == kAnnotInspectorContent && code == EN_CHANGE) {
            ApplyAnnotInspectorContent(hWnd, ctx);
            return 0;
        }
        if (id == kAnnotInspectorColor && code == BN_CLICKED && ctx->editable &&
            ctx->index >= 0 && ctx->index < static_cast<int>(g_annots.size())) {
            COLORREF color{};
            if (PickColorDialog(hWnd, g_annots[static_cast<size_t>(ctx->index)].color, &color)) {
                Annotation after = g_annots[static_cast<size_t>(ctx->index)];
                after.color = color;
                if (UpdateAnnotationAtIndex(ctx->owner, ctx->index, after)) {
                    SetWindowTextW(GetDlgItem(hWnd, kAnnotInspectorColor), InspectorColorLabel(color).c_str());
                    UpdateInspectorDetails(hWnd, ctx);
                }
            }
            return 0;
        }
        if ((id == kAnnotInspectorWidth || id == kAnnotInspectorAlpha ||
             id == kAnnotInspectorShapeKind || id == kAnnotInspectorShapeDrawMode ||
             id == kAnnotInspectorArrowHead || id == kAnnotInspectorDash) && code == CBN_SELCHANGE) {
            ApplyAnnotInspectorStyle(hWnd, ctx, false);
            return 0;
        }
        if (id == kAnnotInspectorApply && code == BN_CLICKED) {
            ApplyAnnotInspectorStyle(hWnd, ctx, true);
            return 0;
        }
        if (id == kAnnotInspectorClose && code == BN_CLICKED) {
            if (ApplyAnnotInspectorStyle(hWnd, ctx, true)) DestroyWindow(hWnd);
            return 0;
        }
        break;
    }
    case WM_ACTIVATE:
        // Keep this dialog alive while its owned color picker is open.  A click in
        // the owner or elsewhere still closes it, as required for a transient inspector.
        if (LOWORD(wParam) == WA_INACTIVE && GetWindow(reinterpret_cast<HWND>(lParam), GW_OWNER) != hWnd) {
            if (ApplyAnnotInspectorStyle(hWnd, ctx, true)) DestroyWindow(hWnd);
        }
        return 0;
    case WM_CLOSE:
        if (ApplyAnnotInspectorStyle(hWnd, ctx, true)) DestroyWindow(hWnd);
        return 0;
    case WM_NCDESTROY:
        delete ctx;
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void EnsureAnnotInspectorClass() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSW wc{};
    wc.lpfnWndProc = AnnotInspectorProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = L"PdfNoteAnnotInspector";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    registered = true;
}

} // namespace

bool SyncTextBoxToolFontFromAnnotationIndex(int index) {
    if (index < 0 || index >= static_cast<int>(g_annots.size())) return false;
    const Annotation& ann = g_annots[static_cast<size_t>(index)];
    if (ann.type != Annotation::Type::TextBox) return false;

    bool changed = false;
    if (!ann.fontName.empty() && g_textFontName != ann.fontName) {
        g_textFontName = ann.fontName;
        changed = true;
    }
    // TextBox font-size changes are applied to the selected/editing TextBox itself.
    // Selecting a TextBox must not overwrite the annotation toolbar A/B size slots.
    if (changed) {
        SyncToolbarFontSizeCombo();
        PersistConfig();
        if (g_hPdfView) PostMessageW(g_hPdfView, kMsgTextBoxFontUpdate, 0, 0);
    }
    return true;
}
void UpdateAnnotPanelSummary() {
    if (!g_hAnnotSummary) return;
    SetWindowTextW(g_hAnnotSummary, L"");
    ShowWindow(g_hAnnotSummary, SW_HIDE);
}

void RequestAnnotPanelRevealLatest() {
    s_revealLatestAnnotOnNextRefresh = true;
}

void RefreshAnnotPanel() {
    if (g_hAnnotShow) {
        SendMessageW(g_hAnnotShow, BM_SETCHECK, g_showAnnots ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (!g_hAnnotList) {
        UpdateAnnotPanelSummary();
        return;
    }
    int sel = static_cast<int>(SendMessageW(g_hAnnotList, LB_GETCURSEL, 0, 0));
    SendMessageW(g_hAnnotList, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_hAnnotList, LB_RESETCONTENT, 0, 0);
    for (const auto& ann : g_annots) {
        std::wstring label = AnnotListLabel(ann);
        SendMessageW(g_hAnnotList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    SendMessageW(g_hAnnotList, WM_SETREDRAW, TRUE, 0);
    int count = static_cast<int>(g_annots.size());
    int targetSel = -1;
    if (s_revealLatestAnnotOnNextRefresh && count > 0) {
        targetSel = count - 1;
    } else if (sel >= 0 && sel < count) {
        targetSel = sel;
    }
    s_revealLatestAnnotOnNextRefresh = false;
    if (targetSel >= 0) {
        SendMessageW(g_hAnnotList, LB_SETCURSEL, static_cast<WPARAM>(targetSel), 0);
        SendMessageW(g_hAnnotList, LB_SETTOPINDEX, static_cast<WPARAM>(targetSel), 0);
    } else {
        SendMessageW(g_hAnnotList, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    }
    UpdateAnnotPanelSummary();
    // A shorter list exposes unused rows that LISTBOX does not reliably clear
    // when only item painting is invalidated.
    InvalidateRect(g_hAnnotList, nullptr, TRUE);
}

void JumpToSelectedAnnot() {
    if (!g_hAnnotList || !g_hPdfView) return;
    int sel = static_cast<int>(SendMessageW(g_hAnnotList, LB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= static_cast<int>(g_annots.size())) return;
    const auto& ann = g_annots[static_cast<size_t>(sel)];
    if (ann.pageIndex < 0 || ann.pageIndex >= g_pdf.pageCount) return;
    double x = 0.0;
    double y = 0.0;
    if (GetAnnotJumpPoint(ann, x, y)) {
        JumpToPdfPoint(g_hPdfView, ann.pageIndex, x, y);
    } else {
        JumpToPage(g_hPdfView, ann.pageIndex);
    }
}

void ShowAnnotationInspector(HWND owner, int annotationIndex, bool editMode) {
    if (annotationIndex < 0 || annotationIndex >= static_cast<int>(g_annots.size())) return;
    EnsureAnnotInspectorClass();
    auto* ctx = new AnnotInspectorCtx;
    ctx->owner = owner ? owner : g_hMainWnd;
    ctx->index = annotationIndex;
    ctx->editable = editMode && !IsPdfPreviewReadOnlyActive();
    constexpr int kDialogWidth = 520;
    constexpr int kDialogHeight = 530;
    RECT ownerRect{};
    GetWindowRect(ctx->owner, &ownerRect);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    GetMonitorInfoW(MonitorFromWindow(ctx->owner, MONITOR_DEFAULTTONEAREST), &monitorInfo);
    const RECT& work = monitorInfo.rcWork;
    const int ownerWidth = ownerRect.right - ownerRect.left;
    const int rightOffset = std::clamp(ownerWidth / 10, 32, 120);
    int x = ownerRect.left + (ownerWidth - kDialogWidth) / 2 + rightOffset;
    int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - kDialogHeight) / 2;
    const int workLeft = static_cast<int>(work.left);
    const int workTop = static_cast<int>(work.top);
    const int workRight = static_cast<int>(work.right);
    const int workBottom = static_cast<int>(work.bottom);
    x = std::clamp(x, workLeft, std::max(workLeft, workRight - kDialogWidth));
    y = std::clamp(y, workTop, std::max(workTop, workBottom - kDialogHeight));
    HWND dialog = CreateWindowExW(WS_EX_TOOLWINDOW, L"PdfNoteAnnotInspector",
                                  IsEnglishUi() ? L"Annotation properties" : L"注釈プロパティ",
                                  WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                                  x, y, kDialogWidth, kDialogHeight, ctx->owner, nullptr, g_hInst, ctx);
    if (!dialog) delete ctx;
}

bool ShowAnnotPanelContextMenu(HWND owner, POINT screenPt) {
    if (!g_hAnnotList || !IsWindowEnabled(g_hAnnotList)) return false;

    if (screenPt.x != -1 || screenPt.y != -1) {
        POINT clientPt = screenPt;
        if (!ScreenToClient(g_hAnnotList, &clientPt)) return false;
        const LRESULT hit = SendMessageW(g_hAnnotList, LB_ITEMFROMPOINT, 0,
                                        MAKELPARAM(clientPt.x, clientPt.y));
        const int index = static_cast<int>(LOWORD(hit));
        const bool outside = HIWORD(hit) != 0;
        if (outside || index < 0 || index >= static_cast<int>(g_annots.size())) return false;
        SendMessageW(g_hAnnotList, LB_SETCURSEL, static_cast<WPARAM>(index), 0);
        (void)SyncTextBoxToolFontFromAnnotationIndex(index);
        UpdateAnnotPanelSummary();
        SetFocus(g_hAnnotList);
    } else {
        GetCursorPos(&screenPt);
    }

    const int selected = static_cast<int>(SendMessageW(g_hAnnotList, LB_GETCURSEL, 0, 0));
    if (selected < 0 || selected >= static_cast<int>(g_annots.size())) return false;
    const Annotation& ann = g_annots[static_cast<size_t>(selected)];
    const bool canCopy = !CopyableAnnotText(ann).empty();
    const bool writable = !IsPdfPreviewReadOnlyActive();
    const bool canOpenLink = ann.type == Annotation::Type::LinkMarker && !ann.linkId.empty();
    bool canBringFront = false;
    bool canSendBack = false;
    for (int i = 0; i < static_cast<int>(g_annots.size()); ++i) {
        if (i == selected || g_annots[static_cast<size_t>(i)].pageIndex != ann.pageIndex) continue;
        if (i > selected) canBringFront = true;
        if (i < selected) canSendBack = true;
    }

    HMENU menu = CreatePopupMenu();
    if (!menu) return false;
    AppendMenuW(menu, MF_STRING, kAnnotPanelContextProperties,
                IsEnglishUi() ? L"Properties" : L"プロパティ");
    AppendMenuW(menu, MF_STRING | (writable ? 0 : MF_GRAYED), kAnnotPanelContextEdit,
                IsEnglishUi() ? L"Edit" : L"編集");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kAnnotPanelContextMove,
                IsEnglishUi() ? L"Go to annotation" : L"この注釈へ移動");
    AppendMenuW(menu, MF_STRING | (canOpenLink ? 0 : MF_GRAYED), kAnnotPanelContextOpenLink,
                IsEnglishUi() ? L"Open linked note" : L"リンクを開く");
    AppendMenuW(menu, MF_STRING | (canCopy ? 0 : MF_GRAYED), kAnnotPanelContextCopy,
                IsEnglishUi() ? L"Copy text" : L"コピー");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (writable ? 0 : MF_GRAYED), kAnnotPanelContextDuplicate,
                IsEnglishUi() ? L"Duplicate" : L"複製");
    AppendMenuW(menu, MF_STRING | (writable && canBringFront ? 0 : MF_GRAYED),
                kAnnotPanelContextBringFront, IsEnglishUi() ? L"Bring to front" : L"最前面へ");
    AppendMenuW(menu, MF_STRING | (writable && canSendBack ? 0 : MF_GRAYED),
                kAnnotPanelContextSendBack, IsEnglishUi() ? L"Send to back" : L"最背面へ");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (writable ? 0 : MF_GRAYED), kAnnotPanelContextDelete,
                IsEnglishUi() ? L"Delete" : L"削除");
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                        screenPt.x, screenPt.y, 0, owner, nullptr);
    DestroyMenu(menu);

    switch (command) {
    case kAnnotPanelContextProperties:
        ShowAnnotationInspector(owner, selected, false);
        return true;
    case kAnnotPanelContextEdit:
        ShowAnnotationInspector(owner, selected, true);
        return true;
    case kAnnotPanelContextMove:
        JumpToSelectedAnnot();
        return true;
    case kAnnotPanelContextOpenLink:
        return JumpToNoteLinkId(ann.linkId, ann.linkNotePath, true);
    case kAnnotPanelContextCopy:
        return CopyTextToClipboard(owner, CopyableAnnotText(ann));
    case kAnnotPanelContextDuplicate:
        return DuplicateAnnotationAtIndex(owner, selected);
    case kAnnotPanelContextBringFront:
        return ReorderAnnotationAtIndex(owner, selected, true);
    case kAnnotPanelContextSendBack:
        return ReorderAnnotationAtIndex(owner, selected, false);
    case kAnnotPanelContextDelete:
        return DeleteAnnotationAtIndex(owner, selected);
    default:
        return false;
    }
}


