#include "file_output/pdf_annotation_export.h"
#include "fpdf_annot.h"
#include "fpdf_edit.h"
#include <cmath>
#include <limits>
#include <locale>
#include <memory>
#include <sstream>
#include <type_traits>
#include <array>
#include <algorithm>
#include <vector>

namespace pdf_annotation_export {
namespace {

using AnnotHandle = std::unique_ptr<std::remove_pointer_t<FPDF_ANNOTATION>, decltype(&FPDFPage_CloseAnnot)>;
using TextHandle = std::unique_ptr<std::remove_pointer_t<FPDF_TEXTPAGE>, decltype(&FPDFText_ClosePage)>;

bool Finite(double n) {
    return std::isfinite(n) && std::abs(n) < std::numeric_limits<float>::max();
}

[[nodiscard]] bool SetString(FPDF_ANNOTATION annot, const char* key, const std::wstring& value) {
    static_assert(sizeof(wchar_t) == sizeof(FPDF_WCHAR));
    return !!FPDFAnnot_SetStringValue(annot, key,
                                     reinterpret_cast<FPDF_WIDESTRING>(value.c_str()));
}

// Emit only resource-free vector paths. Text appearance uses the embedded
// font's glyph outlines while /Contents retains editable Unicode text. If a
// glyph cannot be represented, keep the original appearance as a Stamp instead.
template<class GetSegment>
[[nodiscard]] bool WritePath(std::ostringstream& out, int count, GetSegment get) {
    if (count < 0) return false;
    for (int i = 0; i < count; ++i) {
        auto segment = get(i);
        float x = 0, y = 0;
        if (!segment || !FPDFPathSegment_GetPoint(segment, &x, &y) ||
            !Finite(x) || !Finite(y)) return false;
        const int type = FPDFPathSegment_GetType(segment);
        if (type == FPDF_SEGMENT_BEZIERTO) {
            if (i + 2 >= count) return false;
            out << x << ' ' << y << ' ';
            for (int j = 0; j < 2; ++j) {
                segment = get(++i);
                if (!segment || FPDFPathSegment_GetType(segment) != FPDF_SEGMENT_BEZIERTO ||
                    !FPDFPathSegment_GetPoint(segment, &x, &y) || !Finite(x) || !Finite(y)) return false;
                out << x << ' ' << y << ' ';
            }
            out << "c\n";
        } else if (type == FPDF_SEGMENT_MOVETO || type == FPDF_SEGMENT_LINETO) {
            out << x << ' ' << y << (type == FPDF_SEGMENT_MOVETO ? " m\n" : " l\n");
        } else return false;
        if (FPDFPathSegment_GetClose(segment)) out << "h\n";
    }
    return true;
}

[[nodiscard]] bool WriteColor(std::ostringstream& out, FPDF_PAGEOBJECT obj, bool stroke) {
    unsigned r = 0, g = 0, b = 0, a = 0;
    const bool ok = stroke ? !!FPDFPageObj_GetStrokeColor(obj, &r, &g, &b, &a)
                           : !!FPDFPageObj_GetFillColor(obj, &r, &g, &b, &a);
    if (!ok) return false;
    out << r / 255.0 << ' ' << g / 255.0 << ' ' << b / 255.0 << (stroke ? " RG\n" : " rg\n");
    return true;
}

[[nodiscard]] bool VectorAppearance(FPDF_PAGE scratch, bool text, std::wstring& result) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out.precision(9);
    TextHandle chars(text ? FPDFText_LoadPage(scratch) : nullptr, FPDFText_ClosePage);
    if (text) {
        if (!chars) return false;
        const int count = FPDFText_CountChars(chars.get());
        for (int i = 0; i < count; ++i) {
            if (FPDFText_IsGenerated(chars.get(), i)) continue;
            const unsigned unicode = FPDFText_GetUnicode(chars.get(), i);
            if (unicode == 0 || unicode > 0xffff) return false;
            if (unicode == '\r' || unicode == '\n' || unicode == '\t' || unicode == ' ' ||
                unicode == 0xa0 || unicode == 0x3000 || (unicode >= 0x2000 && unicode <= 0x200a)) continue;
            FPDF_PAGEOBJECT obj = FPDFText_GetTextObject(chars.get(), i);
            FPDF_FONT font = FPDFTextObj_GetFont(obj);
            const double size = FPDFText_GetFontSize(chars.get(), i);
            double x = 0, y = 0;
            FS_MATRIX matrix{};
            if (!font || !Finite(size) || size <= 0 ||
                !FPDFText_GetCharOrigin(chars.get(), i, &x, &y) ||
                !FPDFText_GetMatrix(chars.get(), i, &matrix) || !Finite(x) || !Finite(y)) return false;
            FPDF_GLYPHPATH glyph = FPDFFont_GetGlyphPath(font, unicode, static_cast<float>(size));
            if (!glyph) return false;
            out << "q\n" << size * matrix.a << ' ' << size * matrix.b << ' '
                << size * matrix.c << ' ' << size * matrix.d << ' ' << x << ' ' << y << " cm\n";
            if (!WriteColor(out, obj, false) ||
                !WritePath(out, FPDFGlyphPath_CountGlyphSegments(glyph),
                           [glyph](int j) { return FPDFGlyphPath_GetGlyphPathSegment(glyph, j); })) return false;
            out << "f\nQ\n";
        }
    } else {
        for (int i = 0; i < FPDFPage_CountObjects(scratch); ++i) {
            FPDF_PAGEOBJECT obj = FPDFPage_GetObject(scratch, i);
            FS_MATRIX m{};
            int fill = 0;
            FPDF_BOOL stroke = 0;
            float width = 0;
            if (FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_PATH || !FPDFPageObj_GetMatrix(obj, &m) ||
                !FPDFPath_GetDrawMode(obj, &fill, &stroke) || !FPDFPageObj_GetStrokeWidth(obj, &width)) return false;
            out << "q\n" << m.a << ' ' << m.b << ' ' << m.c << ' ' << m.d << ' ' << m.e << ' ' << m.f << " cm\n";
            if (!WriteColor(out, obj, false) || !WriteColor(out, obj, true)) return false;
            out << width << " w\n" << FPDFPageObj_GetLineCap(obj) << " J\n"
                << FPDFPageObj_GetLineJoin(obj) << " j\n";
            if (!WritePath(out, FPDFPath_CountSegments(obj),
                           [obj](int j) { return FPDFPath_GetPathSegment(obj, j); })) return false;
            const bool evenOdd = fill == FPDF_FILLMODE_ALTERNATE;
            out << (fill ? (stroke ? (evenOdd ? "B*\n" : "B\n") : (evenOdd ? "f*\n" : "f\n"))
                         : (stroke ? "S\n" : "n\n")) << "Q\n";
        }
    }
    const std::string ascii = out.str();
    result.assign(ascii.begin(), ascii.end());
    return !result.empty();
}

[[nodiscard]] bool InkGeometry(FPDF_ANNOTATION annot, FPDF_PAGE scratch) {
    for (int i = 0; i < FPDFPage_CountObjects(scratch); ++i) {
        const auto obj = FPDFPage_GetObject(scratch, i);
        if (FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_PATH) return false;
        FS_MATRIX m{};
        if (!FPDFPageObj_GetMatrix(obj, &m)) return false;
        std::vector<FS_POINTF> points;
        const auto flush = [&]() {
            // An initial MoveTo followed by another MoveTo has no painted
            // stroke. Keep each actual dashed/arrow segment as a separate ink list.
            if (points.size() < 2) { points.clear(); return true; }
            const bool ok = FPDFAnnot_AddInkStroke(annot, points.data(), points.size()) >= 0;
            points.clear();
            return ok;
        };
        for (int j = 0; j < FPDFPath_CountSegments(obj); ++j) {
            const auto seg = FPDFPath_GetPathSegment(obj, j);
            const int kind = FPDFPathSegment_GetType(seg);
            float x = 0, y = 0;
            if (!FPDFPathSegment_GetPoint(seg, &x, &y) ||
                (kind != FPDF_SEGMENT_MOVETO && kind != FPDF_SEGMENT_LINETO)) return false;
            if (kind == FPDF_SEGMENT_MOVETO && !points.empty() && !flush()) return false;
            points.push_back({m.a * x + m.c * y + m.e, m.b * x + m.d * y + m.f});
        }
        if (!points.empty() && !flush()) return false;
    }
    return FPDFAnnot_GetInkListCount(annot) > 0;
}

[[nodiscard]] bool AppendAppearance(FPDF_ANNOTATION annot, FPDF_PAGE scratch) {
    while (FPDFPage_CountObjects(scratch) > 0) {
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(scratch, 0);
        if (!FPDFPage_RemoveObject(scratch, obj)) return false;
        if (!FPDFAnnot_AppendObject(annot, obj)) {
            FPDFPageObj_Destroy(obj);
            return false;
        }
    }
    return true;
}

} // namespace

[[nodiscard]] static bool AddImpl(FPDF_DOCUMENT output, FPDF_PAGE destination, FPDF_PAGE scratch, const Annotation& a, bool preferFreeText) {
    if (!output || !destination || !scratch || destination == scratch || a.type == Annotation::Type::TextColor) return false;
    const int count = FPDFPage_CountObjects(scratch);
    if (count <= 0) return false; // Never report a silently omitted annotation as success.
    FS_RECTF bounds{};
    for (int i = 0; i < count; ++i) {
        float l = 0, b = 0, r = 0, t = 0;
        if (!FPDFPageObj_GetBounds(FPDFPage_GetObject(scratch, i), &l, &b, &r, &t) ||
            !Finite(l) || !Finite(b) || !Finite(r) || !Finite(t)) return false;
        if (i == 0) bounds = {l, t, r, b};
        else { bounds.left = std::min(bounds.left, l); bounds.bottom = std::min(bounds.bottom, b);
               bounds.right = std::max(bounds.right, r); bounds.top = std::max(bounds.top, t); }
    }
    // A little padding protects antialiasing and stroke caps from AP clipping.
    if (a.type == Annotation::Type::TextBox) {
        if (!Finite(a.x1) || !Finite(a.y1) || !Finite(a.x2) || !Finite(a.y2)) return false;
        bounds.left = std::min(bounds.left, static_cast<float>(std::min(a.x1,a.x2)));
        bounds.right = std::max(bounds.right, static_cast<float>(std::max(a.x1,a.x2)));
        bounds.bottom = std::min(bounds.bottom, static_cast<float>(std::min(a.y1,a.y2)));
        bounds.top = std::max(bounds.top, static_cast<float>(std::max(a.y1,a.y2)));
    }
    bounds.left -= 1; bounds.bottom -= 1; bounds.right += 1; bounds.top += 1;
    int subtype = FPDF_ANNOT_STAMP;
    std::wstring ap;
    const bool allText = [&]() {
        for (int i = 0; i < count; ++i)
            if (FPDFPageObj_GetType(FPDFPage_GetObject(scratch, i)) != FPDF_PAGEOBJ_TEXT) return false;
        return true;
    }();
    if (a.type == Annotation::Type::MarkerText) subtype = FPDF_ANNOT_HIGHLIGHT;
    else if (a.type == Annotation::Type::Freehand || a.type == Annotation::Type::MarkerFree ||
             a.type == Annotation::Type::Line || a.type == Annotation::Type::Arrow ||
             a.type == Annotation::Type::Wave ||
             (a.type == Annotation::Type::Shape && (ShapeKindIsOpen(a.shapeKind) || (a.shapeDrawMode == ShapeDrawMode::Outline && !a.dash.empty()))))
        // Store actual dash runs as Ink strokes so external editors preserve gaps.
        subtype = FPDF_ANNOT_INK;
    else if (a.type == Annotation::Type::TextBox && preferFreeText && allText && VectorAppearance(scratch, true, ap))
        subtype = FPDF_ANNOT_FREETEXT;
    else if (a.type == Annotation::Type::Shape && a.shapeAngle == 0.0 &&
             !(a.shapeBoundary != ShapeBoundary::None && !a.dash.empty()) &&
             (a.shapeKind == ShapeKind::Rectangle || a.shapeKind == ShapeKind::Square ||
              a.shapeKind == ShapeKind::Ellipse || a.shapeKind == ShapeKind::Circle) && VectorAppearance(scratch, false, ap))
        subtype = (a.shapeKind == ShapeKind::Rectangle || a.shapeKind == ShapeKind::Square) ? FPDF_ANNOT_SQUARE : FPDF_ANNOT_CIRCLE;

    const int index = FPDFPage_GetAnnotCount(destination);
    AnnotHandle annot(FPDFPage_CreateAnnot(destination, subtype), FPDFPage_CloseAnnot);
    if (!annot) return false;
    const auto build = [&]() {
        if (!FPDFAnnot_SetRect(annot.get(), &bounds) || !FPDFAnnot_SetFlags(annot.get(), FPDF_ANNOT_FLAG_PRINT) ||
            !SetString(annot.get(), "T", L"PDF Note Workspace") || !SetString(annot.get(), "NM", a.id) ||
            !SetString(annot.get(), "Contents", a.type == Annotation::Type::LinkMarker ? a.linkId : a.text)) return false;
        const double defaultAlpha = a.type == Annotation::Type::MarkerText || a.type == Annotation::Type::MarkerFree ? kMarkerAlphaDefault
                                  : a.type == Annotation::Type::Shape ? 0.35 : 1.0;
        const double alpha = a.type == Annotation::Type::TextBox || a.type == Annotation::Type::MathBox || a.type == Annotation::Type::LinkMarker
                           ? 1.0 : (a.alpha > 0 ? a.alpha : defaultAlpha);
        if (!Finite(alpha) || alpha < 0 || alpha > 1 || !Finite(a.width) || !Finite(a.fontPt)) return false;
        const bool filledShape=(subtype==FPDF_ANNOT_SQUARE || subtype==FPDF_ANNOT_CIRCLE) && a.shapeDrawMode==ShapeDrawMode::Fill;
        // For FreeText /C is the BACKGROUND, not the text color. Keep it absent
        // for transparent text boxes; text color belongs in /DA and the AP.
        const bool shapeBoundary = a.type == Annotation::Type::Shape && a.shapeBoundary != ShapeBoundary::None;
        const COLORREF strokeColor = shapeBoundary ? ShapeBoundaryColor(a.shapeBoundary) : a.color;
        if (subtype != FPDF_ANNOT_FREETEXT &&
            !FPDFAnnot_SetColor(annot.get(), FPDFANNOT_COLORTYPE_Color, GetRValue(strokeColor), GetGValue(strokeColor), GetBValue(strokeColor),
                               static_cast<unsigned>(std::round(alpha * 255)))) return false;
        if (!FPDFAnnot_SetBorder(annot.get(), 0, 0, subtype == FPDF_ANNOT_FREETEXT || (filledShape && !shapeBoundary) ? 0.0f : static_cast<float>(std::max(0.0, a.width)))) return false;
        if (filledShape && !FPDFAnnot_SetColor(annot.get(), FPDFANNOT_COLORTYPE_InteriorColor,
            GetRValue(a.color),GetGValue(a.color),GetBValue(a.color),static_cast<unsigned>(std::round(alpha*255)))) return false;
        if (subtype == FPDF_ANNOT_HIGHLIGHT) {
            if (a.quads.size() % 8) return false;
            if (a.quads.empty()) {
                FS_QUADPOINTSF q{static_cast<float>(std::min(a.x1, a.x2)), static_cast<float>(std::max(a.y1, a.y2)),
                                static_cast<float>(std::max(a.x1, a.x2)), static_cast<float>(std::max(a.y1, a.y2)),
                                static_cast<float>(std::min(a.x1, a.x2)), static_cast<float>(std::min(a.y1, a.y2)),
                                static_cast<float>(std::max(a.x1, a.x2)), static_cast<float>(std::min(a.y1, a.y2))};
                return FPDFAnnot_AppendAttachmentPoints(annot.get(), &q) != 0;
            }
            for (size_t i = 0; i < a.quads.size(); i += 8) {
                float v[8];
                for (int j = 0; j < 8; ++j) { if (!Finite(a.quads[i+j])) return false; v[j] = static_cast<float>(a.quads[i+j]); }
                // App quads walk clockwise (TL TR BR BL); PDF text markup
                // uses Z order (TL TR BL BR), including in Adobe viewers.
                FS_QUADPOINTSF q{v[0], v[1], v[2], v[3], v[6], v[7], v[4], v[5]};
                if (!FPDFAnnot_AppendAttachmentPoints(annot.get(), &q)) return false;
            }
            return true;
        }
        if (subtype == FPDF_ANNOT_INK) return InkGeometry(annot.get(), scratch) && AppendAppearance(annot.get(), scratch);
        if (subtype == FPDF_ANNOT_STAMP) return AppendAppearance(annot.get(), scratch);
        if (subtype == FPDF_ANNOT_FREETEXT) {
            float appearanceFontPt = 0;
            if (!FPDFTextObj_GetFontSize(FPDFPage_GetObject(scratch, 0), &appearanceFontPt) ||
                !Finite(appearanceFontPt) || appearanceFontPt <= 0) return false;
            std::wostringstream da;
            da.imbue(std::locale::classic());
            da << L"/Helv " << appearanceFontPt << L" Tf " << GetRValue(a.color)/255.0 << L' '
               << GetGValue(a.color)/255.0 << L' ' << GetBValue(a.color)/255.0 << L" rg";
            if (!SetString(annot.get(), "DA", da.str())) return false;
            // Generate the native FreeText appearance using the valid /Helv
            // resource from CreateDocument(), then restore the exact outlines.
            if (!Finalize(destination)) return false;
        }
        if (alpha < 1) ap = L"/GS gs\n" + ap;
        return !!FPDFAnnot_SetAP(annot.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                                 reinterpret_cast<FPDF_WIDESTRING>(ap.c_str()));
    };
    const bool ok = build();
    annot.reset();
    if (!ok) { const bool removed = !!FPDFPage_RemoveAnnot(destination, index); (void)removed; }
    return ok;
}

bool Add(FPDF_DOCUMENT output, FPDF_PAGE destination, FPDF_PAGE scratch, const Annotation& a, bool preferFreeText) {
    try { return AddImpl(output, destination, scratch, a, preferFreeText); }
    catch (...) { return false; } // Caller discards the output copy, with no write.
}

namespace {
using ColorQuad = std::array<double, 8>;

bool Contains(const ColorQuad& q, double x, double y) {
    bool positive = false, negative = false;
    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) % 4;
        const double cross = (q[2*j] - q[2*i]) * (y - q[2*i+1]) -
                             (q[2*j+1] - q[2*i+1]) * (x - q[2*i]);
        positive |= cross > 1e-6; negative |= cross < -1e-6;
    }
    return !(positive && negative);
}

[[nodiscard]] bool SampleBackground(FPDF_PAGE page, const ColorQuad& q, COLORREF& color) {
    double left=q[0], right=q[0], bottom=q[1], top=q[1];
    for (int i=1; i<4; ++i) {
        left=std::min(left,q[2*i]); right=std::max(right,q[2*i]);
        bottom=std::min(bottom,q[2*i+1]); top=std::max(top,q[2*i+1]);
    }
    left-=2; right+=2; bottom-=2; top+=2;
    const double scale=std::min(2.0,512.0/std::max(right-left,top-bottom));
    const int width=std::max(1,static_cast<int>(std::ceil((right-left)*scale)));
    const int height=std::max(1,static_cast<int>(std::ceil((top-bottom)*scale)));
    struct Bin { unsigned count=0,r=0,g=0,b=0; };
    std::vector<Bin> bins(32768);
    FPDF_BITMAP bitmap=FPDFBitmap_Create(width,height,0);
    if (!bitmap) return false;
    FPDFBitmap_FillRect(bitmap,0,0,width,height,0xffffffff);
    FS_MATRIX matrix{static_cast<float>(scale),0,0,static_cast<float>(-scale),
                     static_cast<float>(-left*scale),static_cast<float>(top*scale)};
    FS_RECTF clip{0,0,static_cast<float>(width),static_cast<float>(height)};
    // Page content only: previously exported app annotations must not affect
    // the automatic color. Sampling is bounded to 512 x 512 pixels per quad.
    FPDF_RenderPageBitmapWithMatrix(bitmap,page,&matrix,&clip,0);
    const auto* bytes=static_cast<const unsigned char*>(FPDFBitmap_GetBuffer(bitmap));
    const int stride=FPDFBitmap_GetStride(bitmap);
    if (!bytes || stride<width*4) { FPDFBitmap_Destroy(bitmap); return false; }
    unsigned best=0;
    for (int y=0;y<height;++y) for (int x=0;x<width;++x) {
        if (x>=2 && y>=2 && x<width-2 && y<height-2) continue;
        const auto* pixel=bytes+y*stride+x*4;
        const unsigned key=((pixel[2]>>3)<<10)|((pixel[1]>>3)<<5)|(pixel[0]>>3);
        auto& bin=bins[key]; ++bin.count; bin.r+=pixel[2]; bin.g+=pixel[1]; bin.b+=pixel[0];
        if (bin.count>bins[best].count) best=key;
    }
    FPDFBitmap_Destroy(bitmap);
    const auto& bin=bins[best];
    if (!bin.count) return false;
    color=RGB(bin.r/bin.count,bin.g/bin.count,bin.b/bin.count);
    return true;
}
} // namespace

bool AddTextColorOverlay(FPDF_PAGE destination, const Annotation& a, const TextColorOptions& options) {
    if (!destination || a.type!=Annotation::Type::TextColor || options.mode==TextColorMode::Marker ||
        static_cast<unsigned>(options.mode)>static_cast<unsigned>(TextColorMode::SpecifiedBackground)) return false;
    const int index=FPDFPage_GetAnnotCount(destination);
    bool created=false;
    try {
        std::vector<ColorQuad> quads;
        if (a.quads.empty()) quads.push_back({std::min(a.x1,a.x2),std::max(a.y1,a.y2),
            std::max(a.x1,a.x2),std::max(a.y1,a.y2),std::max(a.x1,a.x2),std::min(a.y1,a.y2),
            std::min(a.x1,a.x2),std::min(a.y1,a.y2)});
        else {
            if (a.quads.size()%8 || a.quads.size()>80000) return false;
            for (size_t i=0;i<a.quads.size();i+=8) {
                ColorQuad q{}; std::copy_n(a.quads.data()+i,8,q.data()); quads.push_back(q);
            }
        }
        FS_RECTF bounds{std::numeric_limits<float>::max(),-std::numeric_limits<float>::max(),
                        -std::numeric_limits<float>::max(),std::numeric_limits<float>::max()};
        for (const auto& q:quads) {
            double area=0;
            for (int i=0;i<4;++i) {
                if (!Finite(q[2*i]) || !Finite(q[2*i+1])) return false;
                const int j=(i+1)%4; area+=q[2*i]*q[2*j+1]-q[2*j]*q[2*i+1];
                bounds.left=std::min(bounds.left,static_cast<float>(q[2*i]));
                bounds.right=std::max(bounds.right,static_cast<float>(q[2*i]));
                bounds.bottom=std::min(bounds.bottom,static_cast<float>(q[2*i+1]));
                bounds.top=std::max(bounds.top,static_cast<float>(q[2*i+1]));
            }
            if (std::abs(area)<1e-6) return false;
        }
        std::ostringstream out; out.imbue(std::locale::classic()); out.precision(9);
        if (options.mode!=TextColorMode::Transparent) for (const auto& q:quads) {
            COLORREF background=options.background;
            if (options.mode==TextColorMode::AutomaticBackground && !SampleBackground(destination,q,background)) return false;
            out<<"q\n"<<GetRValue(background)/255.0<<' '<<GetGValue(background)/255.0<<' '<<GetBValue(background)/255.0<<" rg\n";
            for (int i=0;i<4;++i) out<<q[2*i]<<' '<<q[2*i+1]<<(i?" l\n":" m\n");
            out<<"h f\nQ\n";
        }
        TextHandle text(FPDFText_LoadPage(destination),FPDFText_ClosePage);
        if (!text) return false;
        const int count=FPDFText_CountChars(text.get());
        if (count<0 || count>1000000) return false;
        int glyphs=0;
        for (int i=0;i<count;++i) {
            const unsigned unicode=FPDFText_GetUnicode(text.get(),i);
            const int generated=FPDFText_IsGenerated(text.get(),i);
            if (generated<0) return false;
            if (generated==1) continue;
            double l=0,r=0,b=0,t=0;
            if (!FPDFText_GetCharBox(text.get(),i,&l,&r,&b,&t)) return false;
            const double cx=(l+r)/2,cy=(b+t)/2;
            if (cx<bounds.left || cx>bounds.right || cy<bounds.bottom || cy>bounds.top ||
                !std::any_of(quads.begin(),quads.end(),[&](const auto& q){return Contains(q,cx,cy);})) continue;
            if (!unicode) return false; // Never silently omit an unmapped selected glyph.
            if (unicode<=32 || unicode==0xa0 || unicode==0x3000 || (unicode>=0x2000 && unicode<=0x200a)) continue;
            const auto obj=FPDFText_GetTextObject(text.get(),i);
            const auto font=FPDFTextObj_GetFont(obj);
            const double size=FPDFText_GetFontSize(text.get(),i);
            double x=0,y=0; FS_MATRIX m{};
            if (!font || !Finite(size) || size<=0 || !FPDFText_GetCharOrigin(text.get(),i,&x,&y) ||
                !FPDFText_GetMatrix(text.get(),i,&m) || !Finite(x) || !Finite(y) ||
                !Finite(m.a) || !Finite(m.b) || !Finite(m.c) || !Finite(m.d)) return false;
            const auto glyph=FPDFFont_GetGlyphPath(font,unicode,static_cast<float>(size));
            if (!glyph) return false;
            out<<"q\n"<<size*m.a<<' '<<size*m.b<<' '<<size*m.c<<' '<<size*m.d<<' '<<x<<' '<<y<<" cm\n"
               <<GetRValue(a.color)/255.0<<' '<<GetGValue(a.color)/255.0<<' '<<GetBValue(a.color)/255.0<<" rg\n";
            if (!WritePath(out,FPDFGlyphPath_CountGlyphSegments(glyph),
                           [glyph](int j){return FPDFGlyphPath_GetGlyphPathSegment(glyph,j);})) return false;
            out<<"f\nQ\n"; ++glyphs;
            if (out.tellp()>16*1024*1024) return false;
            bounds.left=std::min(bounds.left,static_cast<float>(l)); bounds.right=std::max(bounds.right,static_cast<float>(r));
            bounds.bottom=std::min(bounds.bottom,static_cast<float>(b)); bounds.top=std::max(bounds.top,static_cast<float>(t));
        }
        if (!glyphs) return false;
        const auto ascii=out.str(); const std::wstring appearance(ascii.begin(),ascii.end());
        AnnotHandle annot(FPDFPage_CreateAnnot(destination,FPDF_ANNOT_STAMP),FPDFPage_CloseAnnot);
        if (!annot) return false;
        created=true;
        const bool ok=FPDFAnnot_SetRect(annot.get(),&bounds) && FPDFAnnot_SetFlags(annot.get(),FPDF_ANNOT_FLAG_PRINT) &&
            SetString(annot.get(),"NM",a.id) && SetString(annot.get(),"T",L"PDF Note Workspace") &&
            FPDFAnnot_SetAP(annot.get(),FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                           reinterpret_cast<FPDF_WIDESTRING>(appearance.c_str()));
        annot.reset();
        if (!ok) { const bool removed=!!FPDFPage_RemoveAnnot(destination,index); (void)removed; }
        return ok;
    } catch (...) {
        if (created) { const bool removed=!!FPDFPage_RemoveAnnot(destination,index); (void)removed; }
        return false;
    }
}

FPDF_DOCUMENT CreateDocument() {
    // PDFium has no public API for adding document-level font resources. Its
    // implicit AcroForm initialization can bind a cached stock font to 0 0 R
    // and omits the required Fields array. Load this fixed empty graph instead.
    // Static storage satisfies LoadMemDocument64's lifetime contract. No source
    // data is rewritten, no general PDF byte patcher or new dependency is used.
    static constexpr char empty[]=
        "%PDF-1.7\n"
        "1 0 obj\n<</Type/Catalog/Pages 2 0 R/AcroForm 3 0 R>>\nendobj\n"
        "2 0 obj\n<</Type/Pages/Count 0/Kids[]>>\nendobj\n"
        "3 0 obj\n<</Fields[]/DR<</Font<</Helv 4 0 R>>>>/DA(/Helv 12 Tf 0 g)>>\nendobj\n"
        "4 0 obj\n<</Type/Font/Subtype/Type1/BaseFont/Helvetica/Encoding/WinAnsiEncoding>>\nendobj\n"
        "xref\n0 5\n0000000000 65535 f \n0000000009 00000 n \n0000000069 00000 n \n"
        "0000000115 00000 n \n0000000191 00000 n \n"
        "trailer\n<</Size 5/Root 1 0 R>>\nstartxref\n279\n%%EOF\n";
    auto doc=FPDF_LoadMemDocument64(empty,sizeof(empty)-1,nullptr);
    if (doc && FPDF_GetPageCount(doc)!=0) { FPDF_CloseDocument(doc); return nullptr; }
    return doc;
}

bool CanScaleExisting(FPDF_PAGE destination, double scale) {
    if (!destination || !Finite(scale) || scale <= 0) return false;
    const int count = FPDFPage_GetAnnotCount(destination);
    return count >= 0 && (scale == 1.0 || count == 0);
}

bool Finalize(FPDF_PAGE destination) {
    if (!destination) return false;
    const int count = FPDFPage_GetAnnotCount(destination);
    if (count < 0) return false;
    FPDFPage_TransformAnnots(destination, 1, 0, 0, 1, 0, 0);
    if (FPDFPage_GetAnnotCount(destination) != count) return false;
    for (int i = 0; i < count; ++i) {
        AnnotHandle annot(FPDFPage_GetAnnot(destination, i), FPDFPage_CloseAnnot);
        FS_RECTF rect{};
        if (!annot || !FPDFAnnot_GetRect(annot.get(), &rect) ||
            !Finite(rect.left) || !Finite(rect.right) || !Finite(rect.top) || !Finite(rect.bottom)) return false;
    }
    return true;
}

} // namespace pdf_annotation_export
