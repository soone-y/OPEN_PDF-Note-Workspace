#include "file_output/pdf_annotation_detach.h"
#include "core/annot_commands.h"
#include "fpdf_annot.h"
#include "fpdf_save.h"
#include "fpdf_edit.h"
#include "fpdf_ppo.h"
#include "fpdf_flatten.h"
#include "fpdf_transformpage.h"
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <unordered_set>

namespace pdf_annotation_detach {
namespace {
constexpr size_t kLimit = 8 * 1024 * 1024;
constexpr const char* kPayload = "PNWAnnotationV1";
constexpr const char* kNative = "PNWNativeV1";
using Page = std::unique_ptr<std::remove_pointer_t<FPDF_PAGE>, decltype(&FPDF_ClosePage)>;
using Annot = std::unique_ptr<std::remove_pointer_t<FPDF_ANNOTATION>, decltype(&FPDFPage_CloseAnnot)>;
struct MemoryWriter : FPDF_FILEWRITE {
    std::shared_ptr<std::vector<unsigned char>> bytes=std::make_shared<std::vector<unsigned char>>();
    MemoryWriter() {
        version=1;
        WriteBlock=[](FPDF_FILEWRITE* self,const void* data,unsigned long count) {
            try {
                auto& out=*static_cast<MemoryWriter*>(self)->bytes;
                if (count>1024ull*1024*1024-out.size()) return 0;
                const auto* begin=static_cast<const unsigned char*>(data);
                out.insert(out.end(),begin,begin+count); return 1;
            } catch (...) { return 0; }
        };
    }
};

[[nodiscard]] bool String(FPDF_ANNOTATION a, const char* key, std::wstring& out, bool appearance = false) {
    const auto get = [&](FPDF_WCHAR* p, unsigned long n) {
        return appearance ? FPDFAnnot_GetAP(a, FPDF_ANNOT_APPEARANCEMODE_NORMAL, p, n)
                          : FPDFAnnot_GetStringValue(a, key, p, n);
    };
    const auto bytes = get(nullptr, 0);
    if (bytes < 2 || bytes % 2 || bytes > kLimit) return false;
    std::vector<FPDF_WCHAR> buffer(bytes / 2);
    if (get(buffer.data(), bytes) != bytes || buffer.back() != 0) return false;
    out.assign(reinterpret_cast<const wchar_t*>(buffer.data()), buffer.size() - 1);
    return out.find(L'\0') == std::wstring::npos;
}

// Snapshot of the PDF-facing geometry/style/appearance. A later external edit
// must not be overwritten by stale private data. This is not a security hash.
[[nodiscard]] bool Native(FPDF_ANNOTATION a, std::wstring& out) {
    std::wostringstream s; s.imbue(std::locale::classic()); s << std::setprecision(17);
    FS_RECTF r{};
    if (!FPDFAnnot_GetRect(a, &r)) return false;
    s << FPDFAnnot_GetSubtype(a) << ' ' << FPDFAnnot_GetFlags(a) << ' '
      << r.left << ' ' << r.top << ' ' << r.right << ' ' << r.bottom;
    for (auto type : {FPDFANNOT_COLORTYPE_Color, FPDFANNOT_COLORTYPE_InteriorColor}) {
        unsigned red=0,g=0,b=0,alpha=0;
        const bool ok=!!FPDFAnnot_GetColor(a,type,&red,&g,&b,&alpha);
        s << ' ' << ok << ' ' << red << ' ' << g << ' ' << b << ' ' << alpha;
    }
    float h=0,v=0,w=0;
    const bool border=!!FPDFAnnot_GetBorder(a,&h,&v,&w);
    s << ' ' << border << ' ' << h << ' ' << v << ' ' << w;
    for (const char* key : {"NM", "Contents", "DA", "AP"}) {
        std::wstring value;
        if (!String(a,key,value,std::string(key)=="AP")) return false;
        s << ' ' << value.size() << ':' << value;
    }
    const auto count=FPDFAnnot_CountAttachmentPoints(a);
    if (count>100000) return false;
    s << ' ' << count;
    for (size_t i=0;i<count;++i) {
        FS_QUADPOINTSF q{};
        if (!FPDFAnnot_GetAttachmentPoints(a,i,&q)) return false;
        s << ' ' << q.x1 << ' ' << q.y1 << ' ' << q.x2 << ' ' << q.y2
          << ' ' << q.x3 << ' ' << q.y3 << ' ' << q.x4 << ' ' << q.y4;
    }
    const auto strokes=FPDFAnnot_GetInkListCount(a);
    if (strokes>100000) return false;
    s << ' ' << strokes;
    for (size_t i=0;i<strokes;++i) {
        const auto n=FPDFAnnot_GetInkListPath(a,i,nullptr,0);
        if (n>1000000) return false;
        std::vector<FS_POINTF> points(n);
        if (FPDFAnnot_GetInkListPath(a,i,points.data(),n)!=n) return false;
        s << ' ' << n;
        for (const auto& p:points) s << ' ' << p.x << ' ' << p.y;
        if (s.tellp()>static_cast<std::streamoff>(kLimit)) return false;
    }
    out=s.str(); return out.size()<kLimit/2;
}

[[nodiscard]] bool ReadPayload(FPDF_ANNOTATION a, int page, Annotation& out) {
    std::wstring text, saved, actual;
    if (!String(a,kPayload,text) || text.size()>kLimit/2 || text.size()%2 ||
        !String(a,kNative,saved) || !Native(a,actual)) return false;
    if (saved!=actual) return false;
    const auto digit=[](wchar_t c) { return c>=L'0' && c<=L'9' ? c-L'0' : c>=L'a' && c<=L'f' ? c-L'a'+10 : -1; };
    std::string json; json.reserve(text.size()/2);
    for (size_t i=0;i<text.size();i+=2) {
        const int h=digit(text[i]),l=digit(text[i+1]);
        if (h<0 || l<0) return false;
        json.push_back(static_cast<char>(h*16+l));
    }
    std::vector<AnnotCommand> commands;
    if (!DeserializeAnnotCommandsJson(json,&commands) || commands.size()!=1 ||
        commands[0].kind!=AnnotCommandKind::Add ||
        commands[0].numericPrecision!=AnnotNumericPrecision::RoundTrip17) return false;
    out=std::move(commands[0].after); out.pageIndex=page;
    return true;
}
// PDFium reads the dictionary colors only when there is no appearance stream.
// Strip the appearance from a separate scratch page, leaving retained annotations intact.
class NativeStyleReader {
public:
    NativeStyleReader(FPDF_DOCUMENT source, int page) : source_(source), page_(page) {}

    [[nodiscard]] Annot Open(FPDF_ANNOTATION original, int index) {
        if (!FPDFAnnot_HasKey(original, "AP")) return Annot(nullptr, FPDFPage_CloseAnnot);
        if (!scratch_) {
            scratch_.reset(FPDF_CreateNewDocument());
            if (scratch_ && FPDF_ImportPagesByIndex(scratch_.get(), source_, &page_, 1, 0))
                scratchPage_.reset(FPDF_LoadPage(scratch_.get(), 0));
        }
        if (!scratchPage_) return Annot(nullptr, FPDFPage_CloseAnnot);
        Annot copy(FPDFPage_GetAnnot(scratchPage_.get(), index), FPDFPage_CloseAnnot);
        if (copy && !FPDFAnnot_SetAP(copy.get(), FPDF_ANNOT_APPEARANCEMODE_NORMAL, nullptr))
            copy.reset();
        return copy;
    }

private:
    FPDF_DOCUMENT source_;
    int page_;
    Document scratch_;
    Page scratchPage_{nullptr, FPDF_ClosePage};
};

[[nodiscard]] bool PdfColor(FPDF_ANNOTATION a, FPDFANNOT_COLORTYPE type,
                            COLORREF& color, double& alpha) {
    unsigned r=0,g=0,b=0,opacity=0;
    if (!FPDFAnnot_GetColor(a,type,&r,&g,&b,&opacity) ||
        r>255 || g>255 || b>255 || opacity>255) return false;
    color=RGB(r,g,b);
    alpha=opacity/255.0;
    return true;
}

[[nodiscard]] bool ReadTextAppearance(FPDF_ANNOTATION a, Annotation& out) {
    std::wstring da;
    if (!String(a,"DA",da) || da.size()>4096) return false;
    std::wistringstream input(da);
    input.imbue(std::locale::classic());
    std::vector<double> operands;
    bool haveSize=false;
    out.color=RGB(0,0,0);
    for (std::wstring token; input >> token;) {
        if (token.front()==L'/') {
            if (!operands.empty()) return false;
            continue; // The font resource name is local to this PDF.
        }
        if (token==L"Tf") {
            if (operands.size()!=1 || operands[0]<=0 || operands[0]>512) return false;
            out.fontPt=operands[0];
            haveSize=true;
        } else if (token==L"g" || token==L"rg") {
            const bool gray=token==L"g";
            if (operands.size()!=(gray?1u:3u) ||
                std::any_of(operands.begin(),operands.end(),[](double v){return v<0 || v>1;}))
                return false;
            const auto channel=[&](size_t i){return std::lround(operands[gray?0:i]*255);};
            out.color=RGB(channel(0),channel(1),channel(2));
        } else {
            std::wistringstream number(token);
            number.imbue(std::locale::classic());
            double value=0;
            if (!(number >> value) || !number.eof() || !std::isfinite(value) || operands.size()>=3)
                return false;
            operands.push_back(value);
            continue;
        }
        operands.clear();
    }
    return haveSize && operands.empty();
}

[[nodiscard]] bool ValidPoint(double x, double y) {
    constexpr double maxCoordinate = 1e7;
    return std::isfinite(x) && std::isfinite(y) &&
           std::abs(x)<=maxCoordinate && std::abs(y)<=maxCoordinate;
}

// PDFium resolves the appearance's BBox and Matrix against the annotation
// rectangle during flattening. Use an isolated page containing only this
// annotation so the resulting form carries that placement in PDF coordinates.
struct StampTransform {
    double a=1,b=0,c=0,d=1,e=0,f=0;

    [[nodiscard]] StampTransform Then(const FS_MATRIX& local) const {
        return {a*local.a+c*local.b, b*local.a+d*local.b,
                a*local.c+c*local.d, b*local.c+d*local.d,
                a*local.e+c*local.f+e, b*local.e+d*local.f+f};
    }
};

[[nodiscard]] bool ReadStampPaths(FPDF_PAGEOBJECT object, const StampTransform& parent,
                                  int depth, size_t& budget, const FS_RECTF& rect,
                                  const Annotation& base, std::vector<Annotation>& out) {
    if (!object || depth>8 || budget==0) return false;
    --budget;
    FS_MATRIX local{};
    if (!FPDFPageObj_GetMatrix(object,&local)) return false;
    const auto matrix=parent.Then(local);
    if (FPDFPageObj_GetType(object)==FPDF_PAGEOBJ_FORM) {
        const int count=FPDFFormObj_CountObjects(object);
        if (count<=0 || static_cast<size_t>(count)>budget) return false;
        for (int i=0;i<count;++i)
            if (!ReadStampPaths(FPDFFormObj_GetObject(object,i),matrix,depth+1,budget,rect,base,out))
                return false;
        return true;
    }
    if (FPDFPageObj_GetType(object)!=FPDF_PAGEOBJ_PATH) return false;
    const auto clip=FPDFPageObj_GetClipPath(object);
    if (clip && FPDFClipPath_CountPaths(clip)>0) return false;
    int fill=0;
    FPDF_BOOL stroke=0;
    float width=0;
    unsigned red=0,green=0,blue=0,alpha=0;
    if (!FPDFPath_GetDrawMode(object,&fill,&stroke) || fill!=FPDF_FILLMODE_NONE || !stroke ||
        FPDFPageObj_GetDashCount(object)!=0 ||
        !FPDFPageObj_GetStrokeWidth(object,&width) ||
        !FPDFPageObj_GetStrokeColor(object,&red,&green,&blue,&alpha) || !alpha ||
        red>255 || green>255 || blue>255 || alpha>255) return false;

    const double sx=std::hypot(matrix.a,matrix.b), sy=std::hypot(matrix.c,matrix.d);
    if (!(sx>0) || !(sy>0) || std::abs(sx-sy)>0.001*std::max(sx,sy) ||
        std::abs(matrix.a*matrix.c+matrix.b*matrix.d)>0.001*sx*sy) return false;
    Annotation pen=base;
    pen.type=Annotation::Type::Freehand;
    pen.width=width*sx;
    if (!(pen.width>0 && pen.width<=1000)) return false;
    pen.color=RGB(red,green,blue);
    pen.alpha=alpha/255.0;

    const int count=FPDFPath_CountSegments(object);
    if (count<2 || static_cast<size_t>(count)>budget) return false;
    budget-=count;
    const auto flush=[&]() {
        if (pen.path.size()>=2) out.push_back(pen);
        pen.path.clear();
    };
    for (int i=0;i<count;++i) {
        const auto segment=FPDFPath_GetPathSegment(object,i);
        const auto kind=FPDFPathSegment_GetType(segment);
        float x=0,y=0;
        if (!FPDFPathSegment_GetPoint(segment,&x,&y)) return false;
        const Annotation::Pt pt{matrix.a*x+matrix.c*y+matrix.e,matrix.b*x+matrix.d*y+matrix.f};
        if (!ValidPoint(pt.x,pt.y) || pt.x<rect.left || pt.x>rect.right ||
            pt.y<rect.bottom || pt.y>rect.top) return false;
        if (kind==FPDF_SEGMENT_MOVETO) flush();
        else if (kind!=FPDF_SEGMENT_LINETO || pen.path.empty()) return false;
        pen.path.push_back(pt);
        if (FPDFPathSegment_GetClose(segment)) {
            pen.path.push_back(pen.path.front());
            flush();
        }
    }
    flush();
    return true;
}

[[nodiscard]] std::vector<Annotation> ReadStrokeStamp(FPDF_DOCUMENT document, int pageIndex,
                                                       int index, const FS_RECTF& rect,
                                                       const Annotation& base) {
    Document scratch(FPDF_CreateNewDocument());
    if (!scratch || !FPDF_ImportPagesByIndex(scratch.get(),document,&pageIndex,1,0)) return {};
    Page page(FPDF_LoadPage(scratch.get(),0),FPDF_ClosePage);
    if (!page) return {};
    const int count=FPDFPage_GetAnnotCount(page.get());
    if (index<0 || index>=count) return {};
    for (int i=count-1;i>=0;--i)
        if (i!=index && !FPDFPage_RemoveAnnot(page.get(),i)) return {};
    const int before=FPDFPage_CountObjects(page.get());
    if (before<0 || FPDFPage_Flatten(page.get(),FLAT_NORMALDISPLAY)!=FLATTEN_SUCCESS) return {};
    page.reset();
    page.reset(FPDF_LoadPage(scratch.get(),0));
    if (!page || FPDFPage_CountObjects(page.get())!=before+1 ||
        FPDFPage_GetAnnotCount(page.get())!=0) return {};
    const auto appearance=FPDFPage_GetObject(page.get(),before);
    if (!appearance || FPDFPageObj_GetType(appearance)!=FPDF_PAGEOBJ_FORM) return {};
    std::vector<Annotation> result;
    size_t budget=100000;
    if (!ReadStampPaths(appearance,{},0,budget,rect,base,result)) return {};
    return result;
}

// An empty batch means this annotation stays in the PDF. All strokes of an
// Ink annotation must be convertible before the original is removed.
[[nodiscard]] std::vector<Annotation> ReadStandard(FPDF_DOCUMENT document, FPDF_ANNOTATION a, int page, int index,
                                                   NativeStyleReader& styles) {
    const auto subtype=FPDFAnnot_GetSubtype(a);
    switch (subtype) {
    case FPDF_ANNOT_HIGHLIGHT: case FPDF_ANNOT_UNDERLINE: case FPDF_ANNOT_STRIKEOUT:
    case FPDF_ANNOT_SQUIGGLY: case FPDF_ANNOT_POLYLINE:
    case FPDF_ANNOT_LINE: case FPDF_ANNOT_INK: case FPDF_ANNOT_SQUARE:
    case FPDF_ANNOT_CIRCLE: case FPDF_ANNOT_FREETEXT: case FPDF_ANNOT_STAMP: break;
    default: return {};
    }
    const int flags=FPDFAnnot_GetFlags(a);
    if (flags & ~FPDF_ANNOT_FLAG_PRINT)
        return {};
    for (const char* key : {"Popup","IRT","RC","A","AA","OC","Measure","ExData","BE"})
        if (FPDFAnnot_HasKey(a,key)) return {};

    Annotation ann;
    ann.pageIndex=page;
    FS_RECTF rect{};
    if (!FPDFAnnot_GetRect(a,&rect) || !ValidPoint(rect.left,rect.bottom) ||
        !ValidPoint(rect.right,rect.top) || rect.left>=rect.right || rect.bottom>=rect.top)
        return {};
    ann.x1=rect.left; ann.y1=rect.top; ann.x2=rect.right; ann.y2=rect.bottom;
    if (FPDFAnnot_HasKey(a,"Contents")) {
        if (!String(a,"Contents",ann.text) || ann.text.size()>65536) return {};
        if (subtype!=FPDF_ANNOT_FREETEXT && !ann.text.empty()) return {};
    }
    if (subtype==FPDF_ANNOT_STAMP) {
        if (!FPDFAnnot_HasKey(a,"AP") || FPDFAnnot_GetObjectCount(a)<=0) return {};
        return ReadStrokeStamp(document,page,index,rect,ann);
    }
    if (subtype==FPDF_ANNOT_FREETEXT) {
        if (ann.text.empty() || FPDFAnnot_HasKey(a,"CL") || !ReadTextAppearance(a,ann))
            return {};
        ann.type=Annotation::Type::TextBox;
        ann.alpha=1;
        ann.backgroundAssistMode=TextBackgroundAssistMode::Off;
        return {std::move(ann)};
    }

    auto styleCopy=styles.Open(a,index);
    auto style=styleCopy ? styleCopy.get() : a;
    if (!PdfColor(style,FPDFANNOT_COLORTYPE_Color,ann.color,ann.alpha) || ann.alpha<=0)
        return {};
    float opacity=0;
    if (FPDFAnnot_HasKey(style,"CA")) {
        if (!FPDFAnnot_GetNumberValue(style,"CA",&opacity) || !std::isfinite(opacity) ||
            opacity<=0 || opacity>1) return {};
        ann.alpha=opacity;
    }

    if (subtype==FPDF_ANNOT_HIGHLIGHT || subtype==FPDF_ANNOT_UNDERLINE ||
        subtype==FPDF_ANNOT_STRIKEOUT || subtype==FPDF_ANNOT_SQUIGGLY) {
        const auto count=FPDFAnnot_CountAttachmentPoints(a);
        if (!count || count>10000) return {};
        std::vector<Annotation> lines;
        ann.type=Annotation::Type::MarkerText;
        for (size_t i=0;i<count;++i) {
            FS_QUADPOINTSF q{};
            if (!FPDFAnnot_GetAttachmentPoints(a,i,&q) || !ValidPoint(q.x1,q.y1) ||
                !ValidPoint(q.x2,q.y2) || !ValidPoint(q.x3,q.y3) || !ValidPoint(q.x4,q.y4))
                return {};
            const auto [left,right]=std::minmax({q.x1,q.x2,q.x3,q.x4});
            const auto [bottom,top]=std::minmax({q.y1,q.y2,q.y3,q.y4});
            if (left>=right || bottom>=top) return {};
            // The clrop marker renderer uses axis-aligned bounds for each quad.
            // Retain skewed/rotated text markup rather than covering extra text.
            unsigned corners=0;
            for (const auto& point : {FS_POINTF{q.x1,q.y1},{q.x2,q.y2},{q.x3,q.y3},{q.x4,q.y4}}) {
                if ((point.x!=left && point.x!=right) || (point.y!=top && point.y!=bottom))
                    return {};
                corners |= 1u << ((point.x==right ? 1 : 0)+(point.y==top ? 2 : 0));
            }
            if (corners!=15) return {};
            if (subtype==FPDF_ANNOT_HIGHLIGHT) {
                ann.quads.insert(ann.quads.end(),{left,top,right,top,right,bottom,left,bottom});
            } else {
                Annotation line=ann;
                line.type=subtype==FPDF_ANNOT_SQUIGGLY ? Annotation::Type::Wave : Annotation::Type::Line;
                line.x1=left; line.x2=right;
                line.y1=line.y2=subtype==FPDF_ANNOT_STRIKEOUT ? (top+bottom)*0.5 : bottom;
                line.width=std::clamp((top-bottom)*0.08,0.5,3.0);
                lines.push_back(std::move(line));
            }
        }
        if (subtype==FPDF_ANNOT_HIGHLIGHT) return {std::move(ann)};
        return lines;
    }

    // The public getter reads /Border. For Ink, a single stroked path can
    // also supply the width used by an appearance generated from /BS.
    float h=0,v=0,width=1;
    if (FPDFAnnot_HasKey(style,"Border")) {
        if (!FPDFAnnot_GetBorder(style,&h,&v,&width)) return {};
    } else if (subtype==FPDF_ANNOT_INK && FPDFAnnot_GetObjectCount(a)==1) {
        auto path=FPDFAnnot_GetObject(a,0);
        int fill=0;
        FPDF_BOOL stroke=0;
        if (path && FPDFPageObj_GetType(path)==FPDF_PAGEOBJ_PATH &&
            FPDFPath_GetDrawMode(path,&fill,&stroke) && stroke &&
            !FPDFPageObj_GetStrokeWidth(path,&width)) return {};
    }
    if (!std::isfinite(width) || width<0 || width>1000 || h!=0 || v!=0)
        return {};
    ann.width=width;
    if (subtype==FPDF_ANNOT_LINE) {
        FS_POINTF start{},end{};
        if (width<=0 || FPDFAnnot_HasKey(a,"LE") || FPDFAnnot_HasKey(a,"LL") ||
            !FPDFAnnot_GetLine(a,&start,&end) || !ValidPoint(start.x,start.y) ||
            !ValidPoint(end.x,end.y) || (start.x==end.x && start.y==end.y)) return {};
        ann.type=Annotation::Type::Line;
        ann.x1=start.x; ann.y1=start.y; ann.x2=end.x; ann.y2=end.y;
        return {std::move(ann)};
    }
    if (subtype==FPDF_ANNOT_POLYLINE) {
        const auto count=FPDFAnnot_GetVertices(a,nullptr,0);
        if (width<=0 || count<2 || count>100000 || FPDFAnnot_HasKey(a,"LE")) return {};
        std::vector<FS_POINTF> vertices(count);
        if (FPDFAnnot_GetVertices(a,vertices.data(),count)!=count) return {};
        ann.type=Annotation::Type::Freehand;
        ann.path.reserve(count);
        for (const auto& point : vertices) {
            if (!ValidPoint(point.x,point.y)) return {};
            ann.path.push_back({point.x,point.y});
        }
        return {std::move(ann)};
    }
    if (subtype==FPDF_ANNOT_INK) {
        const auto count=FPDFAnnot_GetInkListCount(a);
        if (width<=0 || !count || count>10000) return {};
        size_t remaining=100000;
        std::vector<Annotation> strokes;
        ann.type=Annotation::Type::Freehand;
        for (size_t i=0;i<count;++i) {
            const auto n=FPDFAnnot_GetInkListPath(a,i,nullptr,0);
            if (n<2 || n>remaining) return {};
            remaining-=n;
            std::vector<FS_POINTF> points(n);
            if (FPDFAnnot_GetInkListPath(a,i,points.data(),n)!=n) return {};
            Annotation stroke=ann;
            stroke.path.reserve(n);
            for (const auto& point : points) {
                if (!ValidPoint(point.x,point.y)) return {};
                stroke.path.push_back({point.x,point.y});
            }
            strokes.push_back(std::move(stroke));
        }
        return strokes;
    }

    ann.type=Annotation::Type::Shape;
    ann.shapeKind=subtype==FPDF_ANNOT_SQUARE ? ShapeKind::Rectangle : ShapeKind::Ellipse;
    ann.shapeDrawMode=ShapeDrawMode::Outline;
    if (FPDFAnnot_HasKey(style,"IC")) {
        COLORREF fill=0;
        double fillAlpha=0;
        if (!PdfColor(style,FPDFANNOT_COLORTYPE_InteriorColor,fill,fillAlpha) ||
            fillAlpha<=0) return {};
        if (width>0 && ann.color!=fill) {
            if (ann.color==RGB(0,0,0)) ann.shapeBoundary=ShapeBoundary::Black;
            else if (ann.color==RGB(255,255,255)) ann.shapeBoundary=ShapeBoundary::White;
            else return {};
        }
        ann.color=fill;
        ann.shapeDrawMode=ShapeDrawMode::Fill;
    } else if (width<=0) {
        return {};
    }
    return {std::move(ann)};
}

} // namespace

bool Attach(FPDF_ANNOTATION destination, const Annotation& annotation) {
    try {
        if (!destination) return false;
        AnnotCommand command; command.after=annotation;
        std::string json;
        if (!SerializeAnnotCommandsJson({command},&json) || json.size()>kLimit/4) return false;
        std::wstring text; text.reserve(json.size()*2);
        constexpr wchar_t hex[]=L"0123456789abcdef";
        for (unsigned char c:json) { text.push_back(hex[c>>4]); text.push_back(hex[c&15]); }
        return FPDFAnnot_SetStringValue(destination,kNative,reinterpret_cast<FPDF_WIDESTRING>(L"pending")) &&
            FPDFAnnot_SetStringValue(destination,kPayload,reinterpret_cast<FPDF_WIDESTRING>(text.c_str()));
    } catch (...) { return false; }
}

bool Seal(FPDF_ANNOTATION destination) {
    try {
        if (!destination) return false;
        if (!FPDFAnnot_HasKey(destination,kPayload)) return true;
        std::wstring saved,native;
        if (!String(destination,kNative,saved)) return false;
        if (saved!=L"pending") return true;
        return Native(destination,native) && FPDFAnnot_SetStringValue(destination,kNative,
            reinterpret_cast<FPDF_WIDESTRING>(native.c_str()));
    } catch (...) { return false; }
}

bool Detach(FPDF_DOCUMENT source, Result& result, std::wstring& error) {
    try {
        error=L"PDF annotation detachment failed; no result was committed.";
        if (!source || FPDF_GetPageCount(source)<=0) return false;
        Result next;
        // Clone the WHOLE document, retaining outlines, attachments and other
        // document-level data that page import would silently discard.
        MemoryWriter writer;
        if (!FPDF_SaveAsCopy(source,&writer,0)) return false;
        next.pdf=Document(FPDF_LoadMemDocument64(writer.bytes->data(),writer.bytes->size(),nullptr),
                          DocumentCloser{writer.bytes});
        if (!next.pdf) return false;
        std::vector<size_t> converted;
        for (int p=0;p<FPDF_GetPageCount(next.pdf.get());++p) {
            Page page(FPDF_LoadPage(next.pdf.get(),p),FPDF_ClosePage);
            if (!page) return false;
            std::vector<int> remove;
            const size_t pageStart=next.annotations.size();
            const size_t convertedStart=converted.size();
            NativeStyleReader styles(next.pdf.get(),p);
            const int count=FPDFPage_GetAnnotCount(page.get());
            if (count<0 || count>100000) return false;
            for (int i=0;i<count;++i) {
                Annot a(FPDFPage_GetAnnot(page.get(),i),FPDFPage_CloseAnnot);
                if (!a) return false;
                if (FPDFAnnot_HasKey(a.get(),kPayload)) {
                    Annotation annotation;
                    if (!ReadPayload(a.get(),p,annotation)) {
                        error=L"Invalid or externally modified embedded annotation at page " + std::to_wstring(p+1);
                        return false;
                    }
                    next.annotations.push_back(std::move(annotation));
                } else {
                    auto batch=ReadStandard(next.pdf.get(),a.get(),p,i,styles);
                    if (batch.empty()) {
                        // clrop is painted above retained native annotations. Only
                        // detach a suffix, preserving the original stacking order.
                        ++next.unsupported;
                        next.retainedForOrder+=remove.size();
                        next.annotations.resize(pageStart);
                        converted.resize(convertedStart);
                        remove.clear();
                        continue;
                    }
                    for (auto& annotation : batch) {
                        converted.push_back(next.annotations.size());
                        next.annotations.push_back(std::move(annotation));
                    }
                }
                remove.push_back(i);
            }
            for (auto i=remove.rbegin();i!=remove.rend();++i)
                if (!FPDFPage_RemoveAnnot(page.get(),*i)) return false;
        }
        // External /NM values need not be unique. Generate IDs that also
        // cannot collide with IDs restored from embedded application data.
        std::unordered_set<std::wstring> used;
        for (const auto& annotation : next.annotations)
            if (!annotation.id.empty()) used.insert(annotation.id);
        size_t serial=0;
        for (const auto index : converted) {
            auto& id=next.annotations[index].id;
            do { id=L"pdf-detached-"+std::to_wstring(++serial); } while (!used.insert(id).second);
        }
        next.converted=converted.size();
        result=std::move(next); error.clear(); return true;
    } catch (...) { error=L"PDF annotation detachment failed; no result was committed."; return false; }
}
} // namespace pdf_annotation_detach
