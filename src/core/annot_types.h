#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>
#include "fpdfview.h"
#include "fpdf_text.h"
#include "core/constants.h"

enum class NodeType { Root, Folder, Pdf, Note };

enum class DocKind { None, Pdf, Image };

enum class NoteSystem { Legacy };

inline NoteSystem ParseNoteSystem(const std::wstring& s) {
    std::wstring t = s;
    std::transform(t.begin(), t.end(), t.begin(), ::towlower);
    return NoteSystem::Legacy;
}

inline std::wstring NoteSystemToString(NoteSystem system) {
    (void)system;
    return L"legacy";
}

enum class PanAxisLock { None, Horizontal, Vertical };

enum class MathKind { Latex, Markup };
enum class TextWritingMode { Horizontal, VerticalRl };
// Inherit is reserved for annotations created by older versions that did not
// record their own background-assist choice.
enum class TextBackgroundAssistMode { Inherit, Off, Auto, Inverted };
enum class ShapeKind {
    Square,
    Rectangle,
    Diamond,
    EquilateralTriangle,
    Triangle,
    Ellipse,
    Circle,
    RotatedEllipse
};

inline std::wstring ShapeKindToString(ShapeKind kind) {
    switch (kind) {
    case ShapeKind::Square: return L"square";
    case ShapeKind::Rectangle: return L"rectangle";
    case ShapeKind::Diamond: return L"diamond";
    case ShapeKind::EquilateralTriangle: return L"equilateral_triangle";
    case ShapeKind::Triangle: return L"triangle";
    case ShapeKind::Ellipse: return L"ellipse";
    case ShapeKind::Circle: return L"circle";
    case ShapeKind::RotatedEllipse: return L"rotated_ellipse";
    default: return L"rectangle";
    }
}

inline ShapeKind ParseShapeKind(const std::wstring& s) {
    std::wstring t = s;
    std::transform(t.begin(), t.end(), t.begin(), ::towlower);
    if (t == L"square") return ShapeKind::Square;
    if (t == L"diamond" || t == L"rhombus") return ShapeKind::Diamond;
    if (t == L"equilateral_triangle" || t == L"equilateraltriangle" || t == L"equilateral")
        return ShapeKind::EquilateralTriangle;
    if (t == L"triangle") return ShapeKind::Triangle;
    if (t == L"ellipse" || t == L"oval") return ShapeKind::Ellipse;
    if (t == L"circle") return ShapeKind::Circle;
    if (t == L"rotated_ellipse" || t == L"rotatedellipse" || t == L"slanted_ellipse" || t == L"tilted_ellipse")
        return ShapeKind::RotatedEllipse;
    return ShapeKind::Rectangle;
}

inline constexpr double kDefaultRotatedEllipseAngleRad = 0.78539816339744830962; // 45 degrees for legacy rotated_ellipse data

enum class ShapeDrawMode {
    Fill,
    Outline
};

enum class ArrowHead {
    Single,
    Double
};

inline std::wstring ShapeDrawModeToString(ShapeDrawMode mode) {
    switch (mode) {
    case ShapeDrawMode::Fill: return L"fill";
    case ShapeDrawMode::Outline: return L"outline";
    default: return L"fill";
    }
}

inline ShapeDrawMode ParseShapeDrawMode(const std::wstring& s) {
    std::wstring t = s;
    std::transform(t.begin(), t.end(), t.begin(), ::towlower);
    if (t == L"outline" || t == L"stroke" || t == L"border") return ShapeDrawMode::Outline;
    return ShapeDrawMode::Fill;
}

inline std::wstring ArrowHeadToString(ArrowHead head) {
    return head == ArrowHead::Double ? L"double" : L"single";
}

inline ArrowHead ParseArrowHead(const std::wstring& s) {
    std::wstring t = s;
    std::transform(t.begin(), t.end(), t.begin(), ::towlower);
    return (t == L"double" || t == L"both" || t == L"bidirectional")
        ? ArrowHead::Double
        : ArrowHead::Single;
}

inline std::wstring NormalizeFreehandCorrection(const std::wstring& s) {
    std::wstring t = s;
    std::transform(t.begin(), t.end(), t.begin(), ::towlower);
    if (t == L"auto" || t == L"on" || t == L"enabled") return L"auto";
    if (t == L"hold" || t == L"hold_to_correct" || t == L"holdtocorrect" ||
        t == L"pause" || t == L"pause_to_correct" || t == L"pausetocorrect" ||
        t == L"still" || t == L"still_to_correct" || t == L"stilltocorrect")
        return L"hold";
    if (t == L"smooth" || t == L"smoothing") return L"smooth";
    return L"off";
}

inline bool FreehandCorrectionEnabled(const std::wstring& s) {
    std::wstring mode = NormalizeFreehandCorrection(s);
    return mode == L"auto" || mode == L"hold";
}

inline bool FreehandSmoothingEnabled(const std::wstring& s) {
    return NormalizeFreehandCorrection(s) == L"smooth";
}

inline constexpr int kAnnotMethodDataCorrectionPen = -1001;

inline bool FreehandCorrectionPenActive(const std::wstring& s) {
    return NormalizeFreehandCorrection(s) != L"off";
}

inline std::wstring NormalizeCorrectionPenMode(const std::wstring& s) {
    std::wstring mode = NormalizeFreehandCorrection(s);
    return mode == L"off" ? L"auto" : mode;
}

inline std::wstring NormalizeFreehandCorrectionStyle(const std::wstring& s) {
    std::wstring t = s;
    std::transform(t.begin(), t.end(), t.begin(), ::towlower);
    if (t == L"pen" || t == L"freehand") return L"pen";
    if (t == L"shape") return L"shape";
    return L"auto";
}

inline std::wstring NormalizeFreehandCorrectionFill(const std::wstring& s) {
    std::wstring t = s;
    std::transform(t.begin(), t.end(), t.begin(), ::towlower);
    if (t == L"use_shape_setting" || t == L"shape" || t == L"shape_setting") return L"use_shape_setting";
    if (t == L"always_fill" || t == L"fill") return L"always_fill";
    return L"off";
}

struct NodeData {
    NodeType type = NodeType::Root;
    std::wstring path;
};

struct MathEntry {
    size_t start = 0;
    size_t end = 0;
    size_t rawStart = 0;
    size_t rawEnd = 0;
    int line = 1;
    std::wstring text;
    MathKind kind = MathKind::Latex;
    bool isBlock = false;
    bool isMixed = false;
    int eqNumber = 0;
};

struct HighlightRange {
    enum class Kind { Mark, Heading };
    size_t start = 0;
    size_t end = 0;
    Kind kind = Kind::Mark;
};
inline COLORREF ColorForHighlight(HighlightRange::Kind k, COLORREF markCol = RGB(255,245,200), COLORREF headingCol = RGB(220,240,255)) {
    return (k == HighlightRange::Kind::Heading) ? headingCol : markCol;
}


struct PageCache {
    int index = 0;
    double widthPt = 0.0;
    double heightPt = 0.0;
    int w = 0;
    int h = 0;
    int bmpW = 0;
    int bmpH = 0;
    int stride = 0;
    uint64_t pixelsGeneration = 0;
    bool bitmapCoarse = false;
    uint64_t bitmapFailedGeneration = 0;
    int bitmapFailedW = 0;
    int bitmapFailedH = 0;
    bool bitmapFailedCoarse = false;
    std::vector<uint8_t> pixels;
    struct CharBox {
        float left = 0.0f;
        float right = 0.0f;
        float top = 0.0f;
        float bottom = 0.0f;
    };
    std::vector<CharBox> charBoxes;
    struct ReadableGlyph {
        std::wstring text;
        unsigned int codePoint = 0;
        float left = 0.0f;
        float right = 0.0f;
        float top = 0.0f;
        float bottom = 0.0f;
        float originX = 0.0f;
        float originY = 0.0f;
        float fontPt = 0.0f;
        COLORREF fillColor = RGB(0, 0, 0);
        bool haveFillColor = false;
        bool haveOrigin = false;
    };
    std::vector<ReadableGlyph> readableGlyphs;
    struct ReadablePath {
        struct Segment {
            float x1 = 0.0f;
            float y1 = 0.0f;
            float x2 = 0.0f;
            float y2 = 0.0f;
        };
        std::vector<Segment> segments;
        float left = 0.0f;
        float right = 0.0f;
        float top = 0.0f;
        float bottom = 0.0f;
        float widthPt = 1.0f;
        COLORREF strokeColor = RGB(0, 0, 0);
        bool haveStrokeColor = false;
    };
    std::vector<ReadablePath> readablePaths;
};

struct TextLineLayout {
    size_t start = 0;
    size_t len = 0;
    std::vector<int> widths;
};

struct TextLayoutResult {
    TEXTMETRICW tm{};
    std::vector<TextLineLayout> lines;
};

struct Annotation {
    enum class Type { MarkerText, TextColor, MarkerFree, TextBox, Line, Arrow, Wave, Freehand, LinkMarker, MathBox, Shape } type = Type::MarkerText;
    std::wstring id;
    int pageIndex = -1;
    double x1 = 0.0; // pt
    double y1 = 0.0; // pt
    double x2 = 0.0; // pt
    double y2 = 0.0; // pt
    double width = 2.0; // pt
    double alpha = 0.5;
    double fontPt = 12.0; // for text box
    COLORREF color = RGB(255, 220, 128);
    std::wstring text; // for TextBox / MathBox
    std::vector<std::wstring> textLines;
    std::wstring fontName;
    std::wstring linkId;
    std::wstring linkNotePath;
    MathKind mathKind = MathKind::Latex;
    TextWritingMode writingMode = TextWritingMode::Horizontal; // TextBox only for now
    ShapeKind shapeKind = ShapeKind::Rectangle;
    ShapeDrawMode shapeDrawMode = ShapeDrawMode::Fill;
    ArrowHead arrowHead = ArrowHead::Single;
    double shapeRotation = kDefaultRotatedEllipseAngleRad; // radians; used by RotatedEllipse
    struct Pt { double x = 0.0; double y = 0.0; };
    std::vector<Pt> path; // for marker-free / freehand
    std::vector<double> dash; // line dash pattern in PDF points (on/off lengths)
    std::vector<double> quads; // for marker-text / text-color (flattened 8N)
    TextBackgroundAssistMode backgroundAssistMode = TextBackgroundAssistMode::Inherit;
};

struct LinkPending {
    bool active = false;
    bool haveNote = false;
    bool havePdf = false;
    int notePoints = 0;
    int pdfPoints = 0;
    std::wstring id;
    std::wstring notePath;
    std::vector<std::wstring> pendingPdfPaths;
    size_t notePos = 0;
    int pdfPage = -1;
    int pdfAnnotIndex = -1;
    double pdfX = 0.0;
    double pdfY = 0.0;
};

struct PdfViewState {
    DocKind kind = DocKind::None;
    FPDF_DOCUMENT doc = nullptr;
    HANDLE fileHandle = INVALID_HANDLE_VALUE; // for custom document loading (kept open while doc is alive)
    std::wstring path;
    std::string openPasswordUtf8; // memory-only; never persisted
    int pageCount = 0;
    double scale = 1.0;
    double renderScale = 1.0;
    double scrollY = 0.0;
    double scrollX = 0.0;
    int singlePageIndex = 0;
    bool panning = false;
    POINT panAnchor{};
    double panStartY = 0.0;
    double panStartX = 0.0;
    PanAxisLock panAxisLock = PanAxisLock::None;

    std::vector<PageCache> pages;
    struct RectD {
        double left = 0.0;
        double top = 0.0;
        double right = 0.0;
        double bottom = 0.0;
    };
    std::vector<RectD> pageRects;
    double contentWidth = 0.0;
    double contentHeight = 0.0;
    uint64_t renderGeneration = 1;

    bool drawingAnnot = false;
    int drawPageIndex = -1;
    POINT drawStart{};
    POINT drawEnd{};
    ULONGLONG drawStartTick = 0;
    ULONGLONG drawLastMoveTick = 0;
    std::vector<POINT> drawPath;

    bool selectingText = false;
    bool hasSelection = false;
    int selectionStartPage = -1;
    int selectionEndPage = -1;
    POINT selStart{};
    POINT selEnd{};
    bool selectionByText = false;
    int selCharAnchor = -1;
    int selCharFocus = -1;

    // inline text editing (TextBox)
    bool editingText = false;
    int editPage = -1;
    double editX1 = 0.0, editY1 = 0.0, editX2 = 0.0, editY2 = 0.0; // pt
    std::wstring editText;
    size_t editCaret = 0;
    size_t editSelStart = 0;
    size_t editSelEnd = 0;
    bool editSelectingText = false;
    size_t editSelAnchor = 0;
    POINT editSelectDown{};
    bool editSelectMoved = false;
    bool editLayoutDirty = true;
    int editInnerLayoutW = 0;
    TextLayoutResult editLayout{};
    std::wstring editFontName;
    double editFontPt = 14.0;
    std::wstring imeComp; // composition string
    bool imeComposing = false;
    bool suppressNextImeResultReturnChar = false;
    DWORD imeResultMessageTime = 0;
    bool movingEdit = false;
    double editMoveStartX1 = 0.0, editMoveStartY1 = 0.0, editMoveStartX2 = 0.0, editMoveStartY2 = 0.0;
    double editMoveStartPdfX = 0.0, editMoveStartPdfY = 0.0;

    // annotation move (Ctrl+drag)
    bool movingAnnot = false;
    int movingIndex = -1;
    Annotation movingOriginal;
    double moveStartPdfX = 0.0;
    double moveStartPdfY = 0.0;
    int movePage = -1;
    bool pendingTextBoxHit = false;
    int pendingTextBoxIndex = -1;
    POINT pendingTextBoxDown{};
    ULONGLONG textBoxRapidClickTick = 0;
    int textBoxRapidClickCount = 0;
    int textBoxRapidClickPage = -1;
    double textBoxRapidClickX1 = 0.0;
    double textBoxRapidClickY1 = 0.0;
    double textBoxRapidClickX2 = 0.0;
    double textBoxRapidClickY2 = 0.0;
    bool textBoxHoverValid = false;
    POINT textBoxHoverPos{};

    // image backing (single-page)
    std::vector<uint8_t> imagePixels;
    int imageWidth = 0;
    int imageHeight = 0;
    int imageStride = 0;
    double imageDpiX = kDpi;
    double imageDpiY = kDpi;

    bool magnifierActive = false;
    POINT magnifierPos{};

    // eraser (debug + drag erase)
    bool erasing = false;
    bool eraserCursorValid = false;
    POINT eraserCursor{};
    int eraserHoverIndex = -1;
    int eraserHoverPageIndex = -1;
};

struct FileEntry {
    std::wstring path;
    bool isPdf;
};

struct SessionEntry {
    std::wstring displayName;
    std::wstring path;
};
