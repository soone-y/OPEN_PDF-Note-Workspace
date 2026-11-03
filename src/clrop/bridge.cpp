#include "bridge.h"
#include "core/annotation_rotation.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <iomanip>
#include <map>
#include <mutex>
#include <cstdio>
#include <sstream>
#include <windows.h>
#include <utility>

#include "hash.h"
#include "json.h"
#include "core/atomic_write.h"

namespace clrop_bridge {
namespace {

struct AnnotationLoadCacheEntry {
    std::wstring key;
    std::uint64_t size = 0;
    std::int64_t mtimeMs = 0;
    clrop::PdfId loadedId;
    std::vector<Annotation> annots;
};

std::mutex g_annotationLoadCacheMutex;
std::vector<AnnotationLoadCacheEntry> g_annotationLoadCache;
constexpr size_t kAnnotationLoadCacheLimit = 8;
constexpr std::uint64_t kAnnotationLoadCacheMaxFileSize = 8ULL * 1024ULL * 1024ULL;
constexpr size_t kAnnotationLoadCacheMaxAnnotations = 4096;

std::wstring NormalizePathKey(const std::wstring& path) {
    if (path.empty()) return {};
    std::filesystem::path p(path);
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(p, ec);
    std::wstring s = (ec || abs.empty()) ? p.wstring() : abs.wstring();
    std::replace(s.begin(), s.end(), L'/', L'\\');
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return s;
}

bool TryGetFileMeta(const std::wstring& path, std::uint64_t* outSize, std::int64_t* outMtimeMs) {
    return TryGetFileMetaWin32(std::filesystem::path(path), outSize, outMtimeMs);
}

bool TryGetCachedAnnotationLoad(const std::wstring& key,
                                std::uint64_t size,
                                std::int64_t mtimeMs,
                                clrop::PdfId* loadedId,
                                std::vector<Annotation>* annots) {
    std::lock_guard<std::mutex> lock(g_annotationLoadCacheMutex);
    for (size_t i = 0; i < g_annotationLoadCache.size(); ++i) {
        const auto& entry = g_annotationLoadCache[i];
        if (entry.key != key || entry.size != size || entry.mtimeMs != mtimeMs) continue;
        if (loadedId) *loadedId = entry.loadedId;
        if (annots) *annots = entry.annots;
        if (i + 1 < g_annotationLoadCache.size()) {
            AnnotationLoadCacheEntry hot = entry;
            g_annotationLoadCache.erase(g_annotationLoadCache.begin() + static_cast<std::ptrdiff_t>(i));
            g_annotationLoadCache.push_back(std::move(hot));
        }
        return true;
    }
    return false;
}

void StoreCachedAnnotationLoad(const std::wstring& key,
                               std::uint64_t size,
                               std::int64_t mtimeMs,
                               const clrop::PdfId& loadedId,
                               const std::vector<Annotation>& annots) {
    if (key.empty()) return;
    if (size > kAnnotationLoadCacheMaxFileSize) return;
    if (annots.size() > kAnnotationLoadCacheMaxAnnotations) return;

    std::lock_guard<std::mutex> lock(g_annotationLoadCacheMutex);
    for (size_t i = 0; i < g_annotationLoadCache.size(); ++i) {
        auto& entry = g_annotationLoadCache[i];
        if (entry.key != key || entry.size != size || entry.mtimeMs != mtimeMs) continue;
        entry.loadedId = loadedId;
        entry.annots = annots;
        if (i + 1 < g_annotationLoadCache.size()) {
            AnnotationLoadCacheEntry hot = std::move(entry);
            g_annotationLoadCache.erase(g_annotationLoadCache.begin() + static_cast<std::ptrdiff_t>(i));
            g_annotationLoadCache.push_back(std::move(hot));
        }
        return;
    }

    AnnotationLoadCacheEntry entry;
    entry.key = key;
    entry.size = size;
    entry.mtimeMs = mtimeMs;
    entry.loadedId = loadedId;
    entry.annots = annots;
    g_annotationLoadCache.push_back(std::move(entry));
    if (g_annotationLoadCache.size() > kAnnotationLoadCacheLimit) {
        g_annotationLoadCache.erase(g_annotationLoadCache.begin());
    }
}

std::wstring IsoNow() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    wchar_t buf[32]{};
    swprintf(buf, 32, L"%04u-%02u-%02uT%02u:%02u:%02uZ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::string ColorToHex(COLORREF c) {
    char buf[8];
    snprintf(buf, sizeof(buf), "#%02X%02X%02X",
             GetRValue(c), GetGValue(c), GetBValue(c));
    return std::string(buf);
}

COLORREF HexToColor(const std::string& s, COLORREF fallback = RGB(255, 230, 180)) {
    if (s.size() == 7 && s[0] == '#') {
        auto hex = [](char ch) -> int {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
            if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
            return -1;
        };
        int r1 = hex(s[1]), r2 = hex(s[2]);
        int g1 = hex(s[3]), g2 = hex(s[4]);
        int b1 = hex(s[5]), b2 = hex(s[6]);
        if (r1 >= 0 && r2 >= 0 && g1 >= 0 && g2 >= 0 && b1 >= 0 && b2 >= 0) {
            int r = r1 * 16 + r2;
            int g = g1 * 16 + g2;
            int b = b1 * 16 + b2;
            return RGB(r, g, b);
        }
    }
    return fallback;
}

std::string TextWritingModeToClrop(TextWritingMode mode) {
    return mode == TextWritingMode::VerticalRl ? "vertical_rl" : "horizontal";
}

TextWritingMode TextWritingModeFromClrop(const std::string& mode) {
    if (mode == "vertical_rl") return TextWritingMode::VerticalRl;
    return TextWritingMode::Horizontal;
}

std::string TextBackgroundAssistModeToClrop(TextBackgroundAssistMode mode) {
    switch (mode) {
    case TextBackgroundAssistMode::Inherit: return "inherit";
    case TextBackgroundAssistMode::Off: return "off";
    case TextBackgroundAssistMode::Auto: return "auto";
    case TextBackgroundAssistMode::Inverted: return "inverted";
    }
    return "inherit";
}

bool TextBackgroundAssistModeFromClrop(const std::string& mode,
                                       TextBackgroundAssistMode* out) {
    if (!out) return false;
    if (mode.empty() || mode == "inherit") *out = TextBackgroundAssistMode::Inherit;
    else if (mode == "off") *out = TextBackgroundAssistMode::Off;
    else if (mode == "auto") *out = TextBackgroundAssistMode::Auto;
    else if (mode == "inverted") *out = TextBackgroundAssistMode::Inverted;
    else return false;
    return true;
}

clrop::Item MakeMarkerFromAnnot(const Annotation& a) {
    clrop::Item item;
    item.kind = clrop::Item::Kind::MarkerText;
    if (a.type == Annotation::Type::MarkerText && a.markerDisplayMode == MarkerDisplayMode::Legacy)
        item.markerDisplayMode = MarkerDisplayModeName(a.markerDisplayMode);
    item.id = a.id;
    item.color = ColorToHex(a.color);
    item.alpha = a.alpha > 0 ? a.alpha : 0.3;
    double minX = std::min(a.x1, a.x2);
    double maxX = std::max(a.x1, a.x2);
    double minY = std::min(a.y1, a.y2);
    double maxY = std::max(a.y1, a.y2);
    item.quads = { minX, maxY, maxX, maxY, maxX, minY, minX, minY };
    item.created = IsoNow();
    item.updated = item.created;
    return item;
}

clrop::Item MakeTextColorFromAnnot(const Annotation& a) {
    clrop::Item item = MakeMarkerFromAnnot(a);
    item.kind = clrop::Item::Kind::TextColor;
    item.alpha = 1.0;
    if (!a.quads.empty()) {
        item.quads = a.quads;
    }
    return item;
}

clrop::Item MakeLineFromAnnot(const Annotation& a) {
    clrop::Item item;
    item.kind = clrop::Item::Kind::Line;
    item.id = a.id;
    item.color = ColorToHex(a.color);
    item.alpha = a.alpha > 0 ? a.alpha : kLineAlphaDefault;
    item.width = a.width > 0 ? a.width : 2.0;
    item.p1 = std::array<double,2>{ a.x1, a.y1 };
    item.p2 = std::array<double,2>{ a.x2, a.y2 };
    item.dash = a.dash;
    item.created = IsoNow();
    item.updated = item.created;
    return item;
}

clrop::Item MakeArrowFromAnnot(const Annotation& a) {
    clrop::Item item = MakeLineFromAnnot(a);
    item.kind = clrop::Item::Kind::Arrow;
    item.arrowHead = WideToUTF8(ArrowHeadToString(a.arrowHead));
    return item;
}

clrop::Item MakeWaveFromAnnot(const Annotation& a) {
    clrop::Item item = MakeLineFromAnnot(a);
    item.kind = clrop::Item::Kind::Wave;
    return item;
}

clrop::Item MakeTextFromAnnot(const Annotation& a) {
    clrop::Item item;
    item.kind = clrop::Item::Kind::Text;
    item.id = a.id;
    item.color = ColorToHex(a.color);
    double minX = std::min(a.x1, a.x2);
    double maxX = std::max(a.x1, a.x2);
    double minY = std::min(a.y1, a.y2);
    double maxY = std::max(a.y1, a.y2);
    item.bbox = std::array<double,4>{ minX, maxY, maxX - minX, maxY - minY };
    item.bboxEndpoints = std::array<double,4>{ a.x1, a.y1, a.x2, a.y2 };
    item.border = "none";
    item.pt = (a.fontPt > 0) ? a.fontPt : 12.0;
    item.font = a.fontName;
    item.writingMode = TextWritingModeToClrop(a.writingMode);
    item.contentRotation = a.contentRotation * 90;
    item.backgroundAssistMode = TextBackgroundAssistModeToClrop(a.backgroundAssistMode);
    item.content = a.text;
    item.lines = a.textLines;
    item.created = IsoNow();
    item.updated = item.created;
    return item;
}

clrop::Item MakeMathFromAnnot(const Annotation& a) {
    clrop::Item item = MakeTextFromAnnot(a);
    item.kind = clrop::Item::Kind::Math;
    item.writingMode.clear();
    item.mathKind = (a.mathKind == MathKind::Markup) ? "markup" : "latex";
    return item;
}

clrop::Item MakeMarkerFreeFromAnnot(const Annotation& a) {
    clrop::Item item;
    if (a.blendMode != AnnotationBlendMode::Normal) item.blendMode = AnnotationBlendModeName(a.blendMode);
    item.kind = clrop::Item::Kind::MarkerFree;
    item.id = a.id;
    item.color = ColorToHex(a.color);
    item.alpha = a.alpha > 0 ? a.alpha : 0.3;
    item.width = a.width > 0 ? a.width : 8.0;
    for (const auto& p : a.path) {
        item.path.push_back(p.x);
        item.path.push_back(p.y);
    }
    item.created = IsoNow();
    item.updated = item.created;
    return item;
}

clrop::Item MakeFreehandFromAnnot(const Annotation& a) {
    clrop::Item item;
    item.kind = clrop::Item::Kind::Freehand;
    item.id = a.id;
    item.color = ColorToHex(a.color);
    item.alpha = a.alpha > 0 ? a.alpha : 1.0;
    item.width = a.width > 0 ? a.width : 2.0;
    for (const auto& p : a.path) {
        item.path.push_back(p.x);
        item.path.push_back(p.y);
    }
    item.created = IsoNow();
    item.updated = item.created;
    return item;
}

clrop::Item MakeLinkMarkerFromAnnot(const Annotation& a) {
    clrop::Item item;
    item.kind = clrop::Item::Kind::LinkMarker;
    item.id = a.id;
    item.color = ColorToHex(a.color);
    item.width = a.width > 0 ? a.width : 6.0;
    item.p1 = std::array<double, 2>{ a.x1, a.y1 };
    item.linkId = a.linkId;
    item.notePath = a.linkNotePath;
    item.created = IsoNow();
    item.updated = item.created;
    return item;
}

clrop::Item MakeShapeFromAnnot(const Annotation& a) {
    clrop::Item item;
    if (a.shapeBoundary != ShapeBoundary::None) item.shapeBoundary = ShapeBoundaryName(a.shapeBoundary);
    if (a.blendMode != AnnotationBlendMode::Normal) item.blendMode = AnnotationBlendModeName(a.blendMode);
    item.kind = clrop::Item::Kind::Shape;
    item.contentRotation = a.contentRotation * 90;
    item.id = a.id;
    item.color = ColorToHex(a.color);
    item.alpha = a.alpha > 0 ? a.alpha : 0.35;
    item.width = a.width;
    double minX = std::min(a.x1, a.x2);
    double maxX = std::max(a.x1, a.x2);
    double minY = std::min(a.y1, a.y2);
    double maxY = std::max(a.y1, a.y2);
    item.bbox = std::array<double, 4>{ minX, maxY, maxX - minX, maxY - minY };
    item.bboxEndpoints = std::array<double, 4>{ a.x1, a.y1, a.x2, a.y2 };
    item.shapeAngle = a.shapeAngle;
    item.shapeKind = WideToUTF8(ShapeKindToString(a.shapeKind));
    if(ShapeKindIsOpen(a.shapeKind))item.dash=a.dash;
    item.shapeDrawMode = WideToUTF8(ShapeDrawModeToString(ShapeDrawModeForKind(a.shapeKind,a.shapeDrawMode)));
    if (a.shapeKind == ShapeKind::RotatedEllipse) item.shapeRotation = std::isfinite(a.shapeRotation) ? a.shapeRotation : kDefaultRotatedEllipseAngleRad;
    item.created = IsoNow();
    item.updated = item.created;
    return item;
}

Annotation MarkerFromItem(const clrop::Item& item, int page) {
    Annotation a;
    a.id = item.id;
    a.pageIndex = page;
    a.type = Annotation::Type::MarkerText;
    (void)ParseMarkerDisplayMode(item.markerDisplayMode, a.markerDisplayMode); // Validated by the load sink.
    a.color = HexToColor(item.color);
    a.alpha = (item.alpha > 0) ? item.alpha : 0.3;
    a.quads = item.quads;
    if (item.quads.size() >= 2) {
        double minX = item.quads[0], maxX = item.quads[0];
        double minY = item.quads[1], maxY = item.quads[1];

        const size_t fullQuadsEnd = (item.quads.size() / 8) * 8;
        for (size_t q = 0; q + 7 < fullQuadsEnd; q += 8) {
            for (size_t k = 0; k < 8; k += 2) {
                const double x = item.quads[q + k];
                const double y = item.quads[q + k + 1];
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
        }

        a.x1 = minX;
        a.x2 = maxX;
        a.y1 = maxY;
        a.y2 = minY;
    }
    return a;
}

Annotation TextColorFromItem(const clrop::Item& item, int page) {
    Annotation a = MarkerFromItem(item, page);
    a.type = Annotation::Type::TextColor;
    a.alpha = 1.0;
    return a;
}

Annotation LineFromItem(const clrop::Item& item, int page) {
    Annotation a;
    a.id = item.id;
    a.pageIndex = page;
    a.type = Annotation::Type::Line;
    a.color = HexToColor(item.color);
    a.alpha = (item.alpha > 0) ? item.alpha : kLineAlphaDefault;
    a.width = item.width;
    if (item.p1) { a.x1 = (*item.p1)[0]; a.y1 = (*item.p1)[1]; }
    if (item.p2) { a.x2 = (*item.p2)[0]; a.y2 = (*item.p2)[1]; }
    a.dash = item.dash;
    return a;
}

Annotation ArrowFromItem(const clrop::Item& item, int page) {
    Annotation a = LineFromItem(item, page);
    a.type = Annotation::Type::Arrow;
    a.arrowHead = ParseArrowHead(UTF8ToWide(item.arrowHead));
    return a;
}

Annotation WaveFromItem(const clrop::Item& item, int page) {
    Annotation a = LineFromItem(item, page);
    a.type = Annotation::Type::Wave;
    return a;
}

void ApplyBboxCoordinates(const clrop::Item& item, Annotation* annotation) {
    if (!annotation) return;
    if (item.bboxEndpoints) {
        annotation->x1 = (*item.bboxEndpoints)[0];
        annotation->y1 = (*item.bboxEndpoints)[1];
        annotation->x2 = (*item.bboxEndpoints)[2];
        annotation->y2 = (*item.bboxEndpoints)[3];
        return;
    }
    if (!item.bbox) return;
    annotation->x1 = (*item.bbox)[0];
    const double top = (*item.bbox)[1];
    const double width = (*item.bbox)[2];
    const double height = (*item.bbox)[3];
    annotation->x2 = annotation->x1 + width;
    annotation->y1 = top;
    annotation->y2 = top - height;
}

Annotation TextFromItem(const clrop::Item& item, int page) {
    Annotation a;
    a.id = item.id;
    a.pageIndex = page;
    a.type = Annotation::Type::TextBox;
    a.color = HexToColor(item.color);
    a.fontPt = item.pt > 0 ? item.pt : 12.0;
    a.fontName = item.font;
    ApplyBboxCoordinates(item, &a);
    a.text = item.content;
    a.textLines = item.lines;
    a.writingMode = TextWritingModeFromClrop(item.writingMode);
    a.contentRotation = item.contentRotation / 90;
    TextBackgroundAssistModeFromClrop(item.backgroundAssistMode, &a.backgroundAssistMode);
    return a;
}

Annotation MathFromItem(const clrop::Item& item, int page) {
    Annotation a = TextFromItem(item, page);
    a.type = Annotation::Type::MathBox;
    a.writingMode = TextWritingMode::Horizontal;
    a.mathKind = (item.mathKind == "markup") ? MathKind::Markup : MathKind::Latex;
    return a;
}

Annotation MarkerFreeFromItem(const clrop::Item& item, int page) {
    Annotation a;
    if (!item.blendMode.empty()) (void)ParseAnnotationBlendMode(item.blendMode,a.blendMode); // validated by sink
    a.id = item.id;
    a.pageIndex = page;
    a.type = Annotation::Type::MarkerFree;
    a.color = HexToColor(item.color);
    a.width = item.width > 0 ? item.width : 8.0;
    a.alpha = item.alpha > 0 ? item.alpha : 0.3;
    for (size_t i = 0; i + 1 < item.path.size(); i += 2) {
        Annotation::Pt p{ item.path[i], item.path[i + 1] };
        a.path.push_back(p);
    }
    return a;
}

Annotation FreehandFromItem(const clrop::Item& item, int page) {
    Annotation a;
    a.id = item.id;
    a.pageIndex = page;
    a.type = Annotation::Type::Freehand;
    a.color = HexToColor(item.color);
    a.width = item.width > 0 ? item.width : 2.0;
    a.alpha = item.alpha > 0 ? item.alpha : 1.0;
    for (size_t i = 0; i + 1 < item.path.size(); i += 2) {
        Annotation::Pt p{ item.path[i], item.path[i + 1] };
        a.path.push_back(p);
    }
    return a;
}

Annotation LinkMarkerFromItem(const clrop::Item& item, int page) {
    Annotation a;
    a.id = item.id;
    a.pageIndex = page;
    a.type = Annotation::Type::LinkMarker;
    a.color = HexToColor(item.color, RGB(40, 40, 40));
    a.width = item.width > 0 ? item.width : 6.0;
    if (item.p1) {
        a.x1 = (*item.p1)[0];
        a.y1 = (*item.p1)[1];
        a.x2 = a.x1;
        a.y2 = a.y1;
    }
    a.linkId = item.linkId;
    a.linkNotePath = item.notePath;
    return a;
}

Annotation ShapeFromItem(const clrop::Item& item, int page) {
    Annotation a;
    if (!item.shapeBoundary.empty()) (void)ParseShapeBoundary(item.shapeBoundary,a.shapeBoundary); // codec validated
    if (!item.blendMode.empty()) (void)ParseAnnotationBlendMode(item.blendMode,a.blendMode); // validated by sink
    a.id = item.id;
    a.pageIndex = page;
    a.type = Annotation::Type::Shape;
    a.contentRotation = item.contentRotation / 90;
    a.color = HexToColor(item.color, RGB(255, 220, 128));
    a.alpha = item.alpha > 0 ? item.alpha : 0.35;
    a.width = item.width;
    a.shapeAngle = item.shapeAngle;
    a.shapeKind = ParseShapeKind(UTF8ToWide(item.shapeKind));
    if(ShapeKindIsOpen(a.shapeKind))a.dash=item.dash;
    a.shapeDrawMode = ParseShapeDrawMode(UTF8ToWide(item.shapeDrawMode));
    NormalizeOpenShapeStyle(a);
    if (item.shapeRotation && std::isfinite(*item.shapeRotation)) {
        a.shapeRotation = *item.shapeRotation;
    } else if (a.shapeKind == ShapeKind::RotatedEllipse) {
        a.shapeRotation = kDefaultRotatedEllipseAngleRad;
    }
    ApplyBboxCoordinates(item, &a);
    return a;
}

static bool IsPathUnderRoot(const std::filesystem::path& child, const std::filesystem::path& root) {
    std::error_code ec;
    auto childCanon = std::filesystem::weakly_canonical(child, ec);
    if (ec || childCanon.empty()) childCanon = std::filesystem::absolute(child, ec);
    ec.clear();
    auto rootCanon = std::filesystem::weakly_canonical(root, ec);
    if (ec || rootCanon.empty()) rootCanon = std::filesystem::absolute(root, ec);
    auto itRoot = rootCanon.begin();
    auto itChild = childCanon.begin();
    for (; itRoot != rootCanon.end(); ++itRoot, ++itChild) {
        if (itChild == childCanon.end() || *itRoot != *itChild) return false;
    }
    return true;
}

void AppendAnnotationFromItem(const clrop::Item& it, int page, std::vector<Annotation>& out) {
    switch (it.kind) {
    case clrop::Item::Kind::MarkerText:
        out.push_back(MarkerFromItem(it, page));
        break;
    case clrop::Item::Kind::TextColor:
        out.push_back(TextColorFromItem(it, page));
        break;
    case clrop::Item::Kind::Line:
        out.push_back(LineFromItem(it, page));
        break;
    case clrop::Item::Kind::Arrow:
        out.push_back(ArrowFromItem(it, page));
        break;
    case clrop::Item::Kind::Wave:
        out.push_back(WaveFromItem(it, page));
        break;
    case clrop::Item::Kind::Math:
        out.push_back(MathFromItem(it, page));
        break;
    case clrop::Item::Kind::Text:
        out.push_back(TextFromItem(it, page));
        break;
    case clrop::Item::Kind::MarkerFree:
        out.push_back(MarkerFreeFromItem(it, page));
        break;
    case clrop::Item::Kind::Freehand:
        out.push_back(FreehandFromItem(it, page));
        break;
    case clrop::Item::Kind::LinkMarker:
        out.push_back(LinkMarkerFromItem(it, page));
        break;
    case clrop::Item::Kind::Shape:
        out.push_back(ShapeFromItem(it, page));
        break;
    default:
        break;
    }
}

class AnnotationLoadSink final : public clrop::ItemLoadSink {
public:
    explicit AnnotationLoadSink(std::vector<Annotation>& target) : out(target) {}

    bool OnClropDocumentVersion(int version, std::wstring&) override {
        for (auto& annotation : out)
            if (annotation.type == Annotation::Type::MarkerText && annotation.markerDisplayMode == MarkerDisplayMode::Inherit)
                annotation.markerDisplayMode = version == 1 ? MarkerDisplayMode::Legacy : MarkerDisplayMode::Multiply;
        return true;
    }

    bool OnClropItem(int page, clrop::Item&& item, std::wstring& err) override {
        AnnotationBlendMode blendMode;
        if (!item.blendMode.empty() && !ParseAnnotationBlendMode(item.blendMode,blendMode)) {
            err = L"Unsupported annotation blend_mode";return false;
        }
        MarkerDisplayMode displayMode;
        if (!ParseMarkerDisplayMode(item.markerDisplayMode, displayMode)) {
            err = L"Unsupported marker_display_mode";
            return false;
        }
        if (item.kind == clrop::Item::Kind::Text || item.kind == clrop::Item::Kind::Math) {
            TextBackgroundAssistMode parsedMode;
            if (!TextBackgroundAssistModeFromClrop(item.backgroundAssistMode, &parsedMode)) {
                err = L"Unsupported background_assist_mode";
                return false;
            }
        }
        AppendAnnotationFromItem(item, page, out);
        return true;
    }

private:
    std::vector<Annotation>& out;
};

void ComputeLoadMismatch(const clrop::PdfId& actualLoadedId,
                         const std::wstring& pdfPath,
                         LoadAnnotationsValidation validation,
                         const clrop::PdfId* expectedPdfId,
                         bool& mismatch) {
    mismatch = false;
    if (validation == LoadAnnotationsValidation::None) return;
    clrop::PdfId expected =
        expectedPdfId
            ? *expectedPdfId
            : ((validation == LoadAnnotationsValidation::Strong)
                ? clrop::ComputePdfId(pdfPath)
                : clrop::ComputePdfFastId(pdfPath));
    mismatch =
        (validation == LoadAnnotationsValidation::Strong)
            ? !clrop::PdfIdStrongMatches(actualLoadedId, expected)
            : !clrop::PdfIdFastMatches(actualLoadedId, expected);
}

} // namespace

std::wstring ClropPathForPdf(const std::wstring& pdfPath) {
    std::filesystem::path p(pdfPath);
    p.replace_extension(L".clrop");
    return p.wstring();
}

bool LoadAnnotations(const std::wstring& clropPath,
                     const std::wstring& pdfPath,
                     std::vector<Annotation>& out,
                     bool& mismatch,
                     clrop::PdfId* loadedId,
                     std::wstring& err,
                     LoadAnnotationsValidation validation,
                     const clrop::PdfId* expectedPdfId) {
    out.clear();
    mismatch = false;
    const std::wstring cacheKey = NormalizePathKey(clropPath);
    std::uint64_t cacheFileSize = 0;
    std::int64_t cacheMtimeMs = 0;
    clrop::PdfId cachedLoadedId;
    if (TryGetFileMeta(clropPath, &cacheFileSize, &cacheMtimeMs) &&
        TryGetCachedAnnotationLoad(cacheKey, cacheFileSize, cacheMtimeMs, &cachedLoadedId, &out)) {
        if (loadedId) *loadedId = cachedLoadedId;
        ComputeLoadMismatch(cachedLoadedId, pdfPath, validation, expectedPdfId, mismatch);
        return true;
    }

    clrop::PdfId filePdfId;
    // Sink callbacks may run before the parser detects a later error. Keep
    // their results private until the complete file has been accepted.
    std::vector<Annotation> loadedAnnots;
    AnnotationLoadSink sink(loadedAnnots);
    clrop::LoadFileFailureKind failureKind = clrop::LoadFileFailureKind::None;
    if (!clrop::LoadClropFileToSink(clropPath, &filePdfId, sink, err, &failureKind)) {
        // Only quarantine parse-corrupt files. A transient read/open failure must not move user data.
        if (failureKind == clrop::LoadFileFailureKind::Parse && !g_workspaceRoot.empty()) {
            std::filesystem::path wsRoot(g_workspaceRoot);
            std::filesystem::path src(clropPath);
            if (!wsRoot.empty() && IsPathUnderRoot(src, wsRoot)) {
                std::filesystem::path escapeDir = wsRoot / L"__pdf_note_workspace__" / L"__escape__";
                std::filesystem::path moved;
                if (atomic_write::QuarantineFileBestEffort(src, escapeDir, &moved) && !moved.empty()) {
                    if (!err.empty()) err += L"\n";
                    err += L"破損ファイルを退避しました: " + moved.wstring();
                }
            }
        }
        return false;
    }
    if (loadedId) *loadedId = filePdfId;
    ComputeLoadMismatch(filePdfId, pdfPath, validation, expectedPdfId, mismatch);
    out = std::move(loadedAnnots);
    if (TryGetFileMeta(clropPath, &cacheFileSize, &cacheMtimeMs)) {
        StoreCachedAnnotationLoad(cacheKey, cacheFileSize, cacheMtimeMs, filePdfId, out);
    }
    return true;
}

bool SaveAnnotations(const std::wstring& clropPath,
                     const std::wstring& pdfPath,
                     const std::vector<Annotation>& annots,
                     std::wstring& err) {
    auto buildDocument = [&](clrop::Document* outDoc) -> bool {
        if (!outDoc) return false;
        clrop::Document doc;
        doc.version = 2;
        doc.pdfId = clrop::ComputePdfId(pdfPath);
        std::map<int, std::vector<clrop::Item>> perPage;
        for (const auto& input : annots) {
            const auto canonical = annotation_rotation::Canonical(input);
            if (!canonical) { err = L"Invalid annotation coordinate basis."; return false; }
            const auto& a = *canonical;
            if (a.pageIndex < 0) continue;
            clrop::Item item;
            switch (a.type) {
            case Annotation::Type::MarkerText:
                if (!a.quads.empty()) {
                    item.kind = clrop::Item::Kind::MarkerText;
                    if (a.markerDisplayMode == MarkerDisplayMode::Legacy) item.markerDisplayMode = "legacy";
                    item.id = a.id;
                    item.color = ColorToHex(a.color);
                    item.alpha = a.alpha > 0 ? a.alpha : 0.3;
                    item.quads = a.quads;
                    item.created = IsoNow();
                    item.updated = item.created;
                } else {
                    item = MakeMarkerFromAnnot(a);
                }
                break;
            case Annotation::Type::TextColor:
                item = MakeTextColorFromAnnot(a);
                break;
            case Annotation::Type::Line:
                item = MakeLineFromAnnot(a);
                break;
            case Annotation::Type::Arrow:
                item = MakeArrowFromAnnot(a);
                break;
            case Annotation::Type::Wave:
                item = MakeWaveFromAnnot(a);
                break;
            case Annotation::Type::TextBox:
                item = MakeTextFromAnnot(a);
                break;
            case Annotation::Type::MathBox:
                item = MakeMathFromAnnot(a);
                break;
            case Annotation::Type::MarkerFree:
                item = MakeMarkerFreeFromAnnot(a);
                break;
            case Annotation::Type::Freehand:
                item = MakeFreehandFromAnnot(a);
                break;
            case Annotation::Type::LinkMarker:
                item = MakeLinkMarkerFromAnnot(a);
                break;
            case Annotation::Type::Shape:
                item = MakeShapeFromAnnot(a);
                break;
            default:
                err.clear();
                err += L"Unsupported annotation type encountered while saving annotations. ";
                err += L"Save was aborted to prevent data loss. ";
                err += L"type=" + std::to_wstring(static_cast<int>(a.type));
                err += L", pageIndex=" + std::to_wstring(a.pageIndex);
                err += L", id=" + a.id;
                return false;
            }
            perPage[a.pageIndex].push_back(std::move(item));
        }
        for (auto& kv : perPage) {
            clrop::Page p;
            p.page = kv.first;
            p.items = std::move(kv.second);
            doc.pages.push_back(std::move(p));
        }
        *outDoc = std::move(doc);
        return true;
    };

    clrop::Document doc;
    if (!buildDocument(&doc)) return false;
    std::filesystem::path preferredTmp;
    std::filesystem::path quarantineDir;
    if (!g_workspaceRoot.empty()) {
        std::filesystem::path resource = std::filesystem::path(g_workspaceRoot) / L"__pdf_note_workspace__";
        preferredTmp = resource / L"__tmp__";
        quarantineDir = resource / L"__escape__";
    }
    if (!clrop::SaveClropFile(clropPath, doc, err, preferredTmp, quarantineDir)) {
        return false;
    }
    std::uint64_t writtenSize = 0;
    std::int64_t writtenMtimeMs = 0;
    if (TryGetFileMeta(clropPath, &writtenSize, &writtenMtimeMs)) {
        StoreCachedAnnotationLoad(NormalizePathKey(clropPath), writtenSize, writtenMtimeMs, doc.pdfId, annots);
    }
    return true;
}

bool SerializeAnnotations(const std::wstring& pdfPath,
                          const std::vector<Annotation>& annots,
                          std::string& outJson,
                          std::wstring& err) {
    clrop::Document doc;
    std::map<int, std::vector<clrop::Item>> perPage;
    doc.version = 2;
    doc.pdfId = clrop::ComputePdfFastId(pdfPath);
    for (const auto& input : annots) {
        const auto canonical = annotation_rotation::Canonical(input);
        if (!canonical) { err = L"Invalid annotation coordinate basis."; return false; }
        const auto& a = *canonical;
        if (a.pageIndex < 0) continue;
        clrop::Item item;
        switch (a.type) {
        case Annotation::Type::MarkerText:
            if (!a.quads.empty()) {
                item.kind = clrop::Item::Kind::MarkerText;
                if (a.markerDisplayMode == MarkerDisplayMode::Legacy) item.markerDisplayMode = "legacy";
                item.id = a.id;
                item.color = ColorToHex(a.color);
                item.alpha = a.alpha > 0 ? a.alpha : 0.3;
                item.quads = a.quads;
                item.created = IsoNow();
                item.updated = item.created;
            } else {
                item = MakeMarkerFromAnnot(a);
            }
            break;
        case Annotation::Type::TextColor:
            item = MakeTextColorFromAnnot(a);
            break;
        case Annotation::Type::Line:
            item = MakeLineFromAnnot(a);
            break;
        case Annotation::Type::Arrow:
            item = MakeArrowFromAnnot(a);
            break;
        case Annotation::Type::Wave:
            item = MakeWaveFromAnnot(a);
            break;
        case Annotation::Type::TextBox:
            item = MakeTextFromAnnot(a);
            break;
        case Annotation::Type::MathBox:
            item = MakeMathFromAnnot(a);
            break;
        case Annotation::Type::MarkerFree:
            item = MakeMarkerFreeFromAnnot(a);
            break;
        case Annotation::Type::Freehand:
            item = MakeFreehandFromAnnot(a);
            break;
        case Annotation::Type::LinkMarker:
            item = MakeLinkMarkerFromAnnot(a);
            break;
        case Annotation::Type::Shape:
            item = MakeShapeFromAnnot(a);
            break;
        default:
            err.clear();
            err += L"Unsupported annotation type encountered while serializing annotations. ";
            err += L"type=" + std::to_wstring(static_cast<int>(a.type));
            err += L", pageIndex=" + std::to_wstring(a.pageIndex);
            err += L", id=" + a.id;
            return false;
        }
        perPage[a.pageIndex].push_back(std::move(item));
    }
    for (auto& kv : perPage) {
        clrop::Page p;
        p.page = kv.first;
        p.items = std::move(kv.second);
        doc.pages.push_back(std::move(p));
    }
    return clrop::SerializeClrop(doc, outJson, err);
}

} // namespace clrop_bridge

