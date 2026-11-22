#include "file_output/pdf_annotation_export.h"
#include "file_output/pdf_annotation_detach.h"
#include "core/annot_stage_replay.h"
#include "core/pdf_permissions.h"
#define PDF_NOTE_ANNOT_COMMAND_CODEC_ONLY
#include "pdf_view/annotation_store.cppinc"
#undef PDF_NOTE_ANNOT_COMMAND_CODEC_ONLY
#include "file_output/shape_outline_pdf.h"
#include "ui/dashed_shape_outline.h"
#include "fpdf_annot.h"
#include "fpdf_attachment.h"
#include "fpdf_edit.h"
#include "fpdf_save.h"
#include "fpdf_transformpage.h"
#include <iostream>
#include <stdexcept>
#include <vector>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <iomanip>

std::wstring UTF8ToWide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                     static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                               static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string WideToUTF8(const std::wstring& s) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                     nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                               out.data(), n, nullptr, nullptr);
    return out;
}

void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

struct Writer : FPDF_FILEWRITE {
    std::vector<unsigned char> bytes;
    Writer() { version = 1; WriteBlock = [](FPDF_FILEWRITE* w, const void* p, unsigned long n) {
        auto& b = static_cast<Writer*>(w)->bytes;
        const auto* data = static_cast<const unsigned char*>(p);
        try { b.insert(b.end(), data, data + n); return 1; }
        catch (...) { return 0; }
    }; }
};

std::vector<unsigned char> Render(FPDF_PAGE page, int flags) {
    auto bitmap = FPDFBitmap_Create(400, 400, 1);
    Require(bitmap != nullptr, "bitmap");
    FPDFBitmap_FillRect(bitmap, 0, 0, 400, 400, 0xffffffff);
    FPDF_RenderPageBitmap(bitmap, page, 0, 0, 400, 400, 0, flags);
    const auto* p = static_cast<unsigned char*>(FPDFBitmap_GetBuffer(bitmap));
    std::vector<unsigned char> bytes(p, p + FPDFBitmap_GetStride(bitmap) * 400);
    FPDFBitmap_Destroy(bitmap);
    return bytes;
}

FPDF_PAGEOBJECT Rect(FPDF_PAGE page) {
    auto obj = FPDFPageObj_CreateNewRect(40, 100, 100, 25);
    Require(obj != nullptr && FPDFPageObj_SetFillColor(obj, 0, 100, 240, 128) &&
            FPDFPath_SetDrawMode(obj, FPDF_FILLMODE_ALTERNATE, 0), "rect");
    FPDFPage_InsertObject(page, obj);
    return obj;
}

void Case(Annotation::Type type, int expected, bool freeText = true, bool japanese = false, bool embedEditableData = true) {
    auto doc = pdf_annotation_export::CreateDocument();
    Require(doc!=nullptr,"initialize output resources");
    auto page = FPDFPage_New(doc, 0, 200, 200);
    auto scratch = FPDFPage_New(doc, 1, 200, 200);
    Annotation a;
    a.type = type; a.id = L"round-trip"; a.text = L"Hello PDF";
    a.fontPt = 16; a.color = RGB(0,100,240); a.alpha = 0.5;
    a.x1 = 40; a.y1 = 100; a.x2 = 140; a.y2 = 125;
    if(type == Annotation::Type::MarkerText) {
        a.quads={40,125,140,125,140,100,40,100};
        if (!embedEditableData) {
            a.text.clear();
            a.quads={40,125,140,125,140,100,40,100};
        }
    }
    FPDF_FONT font = nullptr;
    if (type == Annotation::Type::TextBox) {
        if(japanese) {
            a.text=L"授業の記録 ABC";
            auto dc=CreateCompatibleDC(nullptr);
            auto gdi=CreateFontW(24,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,0,0,L"Noto Sans JP");
            auto old=SelectObject(dc,gdi);
            const DWORD size=GetFontData(dc,0,0,nullptr,0);
            Require(size!=GDI_ERROR && size>0,"Japanese test font unavailable");
            std::vector<unsigned char> bytes(size);
            Require(GetFontData(dc,0,0,bytes.data(),size)==size,"Japanese font bytes");
            SelectObject(dc,old);DeleteObject(gdi);DeleteDC(dc);
            font=FPDFText_LoadFont(doc,bytes.data(),size,FPDF_FONT_TRUETYPE,1);
        } else font = FPDFText_LoadStandardFont(doc, "Helvetica");
        Require(font != nullptr,"test font");
        auto text = FPDFPageObj_CreateTextObj(doc, font, 16);
        Require(FPDFText_SetText(text, reinterpret_cast<FPDF_WIDESTRING>(a.text.c_str())) &&
                FPDFPageObj_SetFillColor(text, 0, 100, 240, 255), "text");
        FPDFPageObj_Transform(text, 1, 0, 0, 1, 40, 110);
        FPDFPage_InsertObject(scratch, text);
    } else if (expected == FPDF_ANNOT_INK) {
        auto obj = FPDFPageObj_CreateNewPath(40,100);
        if(type==Annotation::Type::Line) Require(FPDFPath_MoveTo(obj,40,100)!=0,"empty subpath");
        Require(FPDFPath_LineTo(obj,140,125) && FPDFPageObj_SetStrokeColor(obj,0,100,240,128) &&
                FPDFPageObj_SetStrokeWidth(obj,4) && FPDFPath_SetDrawMode(obj,0,1), "ink path");
        FPDFPage_InsertObject(scratch,obj);
    } else Rect(scratch);
    Require(FPDFPage_GenerateContent(scratch) != 0, "scratch content");
    const auto reference = Render(scratch, 0);
    Require(pdf_annotation_export::Add(doc,page,scratch,a,freeText,nullptr,embedEditableData), "annotation creation");
    Require(pdf_annotation_export::Finalize(page), "finalize annotations");
    Require(FPDFPage_CountObjects(page) == 0, "annotation must not become page content");
    auto annot = FPDFPage_GetAnnot(page,0);
    Require(annot && FPDFAnnot_GetSubtype(annot) == expected, "subtype before save");
    Require((FPDFAnnot_GetFlags(annot) & FPDF_ANNOT_FLAG_PRINT) != 0, "print flag");
    if (expected == FPDF_ANNOT_FREETEXT) Require(!FPDFAnnot_HasKey(annot,"C"), "FreeText background must be transparent");
    if (expected == FPDF_ANNOT_INK) Require(FPDFAnnot_GetInkListCount(annot) == 1, "InkList");
    if (expected == FPDF_ANNOT_HIGHLIGHT) {
        FS_QUADPOINTSF q{};
        Require(FPDFAnnot_CountAttachmentPoints(annot) == 1 &&
                FPDFAnnot_GetAttachmentPoints(annot,0,&q) && q.x3==40 && q.x4==140,"QuadPoints Z order");
    }
    FPDFPage_CloseAnnot(annot);
    if (expected != FPDF_ANNOT_HIGHLIGHT) {
        auto actual = Render(page, FPDF_ANNOT);
        size_t differences = 0, visible = 0;
        for(size_t i=0;i<actual.size();i+=4) {
            if(reference[i]!=255 || reference[i+1]!=255 || reference[i+2]!=255) ++visible;
            if(std::abs(int(actual[i])-int(reference[i])) > 40 ||
               std::abs(int(actual[i+1])-int(reference[i+1])) > 40 ||
               std::abs(int(actual[i+2])-int(reference[i+2])) > 40) ++differences;
        }
        std::cout << "Appearance " << expected << ": visible=" << visible << " different=" << differences << '\n';
        Require(visible > 200 && differences < visible / 3, "annotation appearance differs");
    }
    FPDF_ClosePage(scratch); FPDFPage_Delete(doc,1); FPDF_ClosePage(page);
    Writer writer;
    Require(FPDF_SaveAsCopy(doc,&writer,0) != 0, "save");
    if(font) FPDFFont_Close(font);
    FPDF_CloseDocument(doc);
    auto loaded = FPDF_LoadMemDocument64(writer.bytes.data(),writer.bytes.size(),nullptr);
    Require(loaded != nullptr && FPDF_GetPageCount(loaded)==1,"reload/page count");
    page = FPDF_LoadPage(loaded,0);
    Require(FPDFPage_GetAnnotCount(page)==1 && FPDFPage_CountObjects(page)==0,"reload structure");
    annot = FPDFPage_GetAnnot(page,0);
    Require(FPDFAnnot_GetSubtype(annot)==expected,"reload subtype");
    wchar_t contents[64]{};
    Require(FPDFAnnot_GetStringValue(annot,"Contents",reinterpret_cast<FPDF_WCHAR*>(contents),sizeof(contents))>=2 &&
            std::wstring(contents)==a.text,"Unicode annotation contents");
    auto with = Render(page,FPDF_ANNOT), without = Render(page,0);
    Require(with != without,"annotation must render separately from page");
    FPDFPage_CloseAnnot(annot);
    pdf_annotation_detach::Result detached;
    std::wstring detachError;
    Require(pdf_annotation_detach::Detach(loaded,detached,detachError), "detach saved native annotation");
    if (!embedEditableData) {
        auto savedAnnot = FPDFPage_GetAnnot(page,0);
        Require(savedAnnot && !FPDFAnnot_HasKey(savedAnnot,"PNWAnnotationV1") &&
                !FPDFAnnot_HasKey(savedAnnot,"PNWNativeV1"),"saved annotation omits both recovery keys");
        FPDFPage_CloseAnnot(savedAnnot);
        auto retainedPage = FPDF_LoadPage(detached.pdf.get(),0);
        if (expected==FPDF_ANNOT_HIGHLIGHT) {
            Require(detached.converted==1 && detached.annotations.size()==1 && detached.unsupported==0 && detached.retainedForOrder==0 &&
                    detached.annotations[0].type==Annotation::Type::MarkerText &&
                    detached.annotations[0].quads==a.quads && retainedPage &&
                    FPDFPage_GetAnnotCount(retainedPage)==0,
                    "export without recovery data uses standard highlight conversion");
        } else {
            Require(detached.annotations.empty() && detached.unsupported==1 && detached.retainedForOrder==0 && retainedPage &&
                    Render(retainedPage,FPDF_ANNOT)==with,"detach retains unsupported visible annotation");
        }
        FPDF_ClosePage(retainedPage);
        FPDF_ClosePage(page); FPDF_CloseDocument(loaded);
        return;
    }
    Require(detached.annotations.size()==1 && detached.unsupported==0 && detached.retainedForOrder==0, "detach count");
    const auto& restored=detached.annotations[0];
    Require(restored.type==a.type && restored.id==a.id && restored.text==a.text &&
            restored.x1==a.x1 && restored.y1==a.y1 && restored.quads==a.quads &&
            restored.fontPt==a.fontPt, "editable data survives saved PDF");
    auto clean=FPDF_LoadPage(detached.pdf.get(),0);
    Require(FPDFPage_GetAnnotCount(clean)==0 && Render(clean,FPDF_ANNOT)==without, "detached PDF background intact");
    FPDF_ClosePage(clean);
    Require(FPDFPage_GetAnnotCount(page)==1,"source untouched");
    pdf_annotation_detach::Result twice;
    Require(pdf_annotation_detach::Detach(detached.pdf.get(),twice,detachError) && twice.annotations.empty(),"detach twice");
    annot=FPDFPage_GetAnnot(page,0);
    Require(FPDFAnnot_SetStringValue(annot,"Contents",reinterpret_cast<FPDF_WIDESTRING>(L"external edit")),"external edit fixture");
    FPDFPage_CloseAnnot(annot);
    auto previous=detached.pdf.get();
    Require(!pdf_annotation_detach::Detach(loaded,detached,detachError) && detached.pdf.get()==previous &&
            detached.annotations.size()==1,"stale payload rejected without publishing partial result");
    detached.pdf.reset(); twice.pdf.reset();
    FPDF_ClosePage(page); FPDF_CloseDocument(loaded);
}

// Intentionally save WITHOUT rendering first: rendering constructs PDFium's
// annotation list and can conceal a missing pre-save finalization step.
void InteropFixture(const std::filesystem::path& directory, Annotation::Type type, const char* name) {
    auto doc=pdf_annotation_export::CreateDocument();
    Require(doc!=nullptr,"initialize interop resources");
    auto page=FPDFPage_New(doc,0,200,200),scratch=FPDFPage_New(doc,1,200,200);
    Annotation a;
    a.type=type; a.id=L"interop"; a.text=L"Blue PDF text";
    a.x1=40; a.y1=100; a.x2=170; a.y2=150; a.color=RGB(0,100,240); a.fontPt=16; a.width=4;
    FPDF_FONT font=nullptr;
    if(type==Annotation::Type::TextBox) {
        font=FPDFText_LoadStandardFont(doc,"Helvetica");
        auto obj=FPDFPageObj_CreateTextObj(doc,font,16);
        Require(obj && FPDFText_SetText(obj,reinterpret_cast<FPDF_WIDESTRING>(a.text.c_str())) &&
                FPDFPageObj_SetFillColor(obj,0,100,240,255),"interop text");
        FPDFPageObj_Transform(obj,1,0,0,1,40,125); FPDFPage_InsertObject(scratch,obj);
    } else if(type==Annotation::Type::Freehand) {
        auto obj=FPDFPageObj_CreateNewPath(40,100);
        Require(obj && FPDFPath_LineTo(obj,140,125) && FPDFPageObj_SetStrokeColor(obj,0,100,240,255) &&
                FPDFPageObj_SetStrokeWidth(obj,4) && FPDFPath_SetDrawMode(obj,0,1),"interop ink");
        FPDFPage_InsertObject(scratch,obj);
    } else { a.quads={40,125,140,125,140,100,40,100}; Rect(scratch); }
    Require(pdf_annotation_export::CanScaleExisting(page,2),"unannotated input may scale");
    Require(pdf_annotation_export::Add(doc,page,scratch,a,true),"interop add");
    auto annot=FPDFPage_GetAnnot(page,0); FS_RECTF before{},after{};
    Require(annot && FPDFAnnot_GetRect(annot,&before),"preflight rect");
    Require(pdf_annotation_export::CanScaleExisting(page,1) &&
            !pdf_annotation_export::CanScaleExisting(page,2) &&
            !pdf_annotation_export::CanScaleExisting(page,0.5) &&
            !pdf_annotation_export::CanScaleExisting(page,std::numeric_limits<double>::infinity()) &&
            !pdf_annotation_export::CanScaleExisting(page,0),"native annotations must reject non-unit scaling");
    Require(FPDFAnnot_GetRect(annot,&after) && std::memcmp(&before,&after,sizeof(before))==0,
            "scale rejection must not mutate the annotation");
    FPDFPage_CloseAnnot(annot);
    Require(pdf_annotation_export::Finalize(page),"interop finalize");
    FPDF_ClosePage(scratch); FPDFPage_Delete(doc,1); FPDF_ClosePage(page);
    Writer writer; Require(FPDF_SaveAsCopy(doc,&writer,0)!=0,"interop save");
    std::ofstream file(directory/name,std::ios::binary);
    file.write(reinterpret_cast<const char*>(writer.bytes.data()),writer.bytes.size());
    file.close(); Require(!file.fail(),"fixture write");
    if(font) FPDFFont_Close(font);
    FPDF_CloseDocument(doc);
}

void ShapeOutlineCases(const std::filesystem::path& directory) {
    const auto corner = shape_outline::Dash({{0,0},{5,0},{5,10}}, {8,4});
    Require(corner.size()==2 && corner[0].size()==3 && corner[0].back().x==5 &&
            corner[0].back().y==3 && corner[1].front().y==7, "dash phase continues across corners");
    const auto invalid = std::vector<double>{0,-1,std::numeric_limits<double>::infinity()};
    Require(shape_outline::Dash({{0,0},{10,0}},invalid).size()==1, "invalid display pattern falls back to solid");
    Require(shape_outline::Dash({{0,0},{std::numeric_limits<double>::infinity(),0}},{4,2}).empty(),
            "nonfinite geometry rejected");
    const auto odd = shape_outline::Dash({{0,0},{30,0}}, {2,3,4});
    Require(odd.size()==6, "odd PDF dash array repeats with alternating on/off phase");
    for (int index=0; index<8; ++index) {
        const auto kind=static_cast<ShapeKind>(index);
        const auto outline=shape_outline::Build(kind,20,100,160,20,0.4);
        Require(outline.size()>=4 && outline.front().x==outline.back().x &&
                outline.front().y==outline.back().y, "legacy closed shape outlines close");
        // Compare real GDI output against the same solid contour at four zooms.
        for (double scale : {0.5,1.0,2.0,4.0}) {
            auto dc=CreateCompatibleDC(nullptr); BITMAPINFO bmi{}; void* bits=nullptr;
            bmi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); bmi.bmiHeader.biWidth=640;
            bmi.bmiHeader.biHeight=-480; bmi.bmiHeader.biPlanes=1; bmi.bmiHeader.biBitCount=32;
            auto bitmap=CreateDIBSection(dc,&bmi,DIB_RGB_COLORS,&bits,nullptr,0);
            Require(dc && bitmap && bits, "shape drawing bitmap");
            auto old=SelectObject(dc,bitmap);
            size_t counts[2]{};
            for (int dashed=0; dashed<2; ++dashed) {
                std::fill_n(static_cast<DWORD*>(bits),640*480,0xffffffff);
                RECT bounds{20,20,20+static_cast<LONG>(120*scale),20+static_cast<LONG>(80*scale)};
                ui::DrawDashedShapeOutline(dc,kind,bounds,0.4,std::max(1,static_cast<int>(2*scale)),
                    RGB(0,100,240),128,dashed ? std::vector<double>{8,4} : std::vector<double>{},scale);
                GdiFlush();
                for(size_t i=0;i<640*480;++i) {
                    const DWORD p=static_cast<DWORD*>(bits)[i];
                    if((p&0xffffff)!=0xffffff) {
                        ++counts[dashed];
                        Require(((p>>16)&255)>=126, "one alpha blend at dash joins");
                    }
                }
            }
            SelectObject(dc,old); DeleteObject(bitmap); DeleteDC(dc);
            Require(counts[0]>100 && counts[1]>counts[0]/3 && counts[1]<counts[0]*9/10,
                    "every shape has visible dash gaps at every zoom");
        }
        auto doc=pdf_annotation_export::CreateDocument();
        auto page=FPDFPage_New(doc,0,200,200), scratch=FPDFPage_New(doc,1,200,200);
        Annotation a; a.type=Annotation::Type::Shape; a.shapeKind=kind;
        a.shapeDrawMode=ShapeDrawMode::Outline; a.shapeRotation=0.4;
        a.x1=20; a.y1=100; a.x2=160; a.y2=20; a.width=2; a.alpha=1;
        a.dash={8,4}; a.color=RGB(0,100,240); a.id=L"dashed-shape";
        auto path=shape_outline::CreateDashedPdfPath(kind,20,100,160,20,0.4,a.dash);
        Require(path && FPDFPageObj_SetStrokeColor(path,0,100,240,255) &&
                FPDFPageObj_SetStrokeWidth(path,2) && FPDFPath_SetDrawMode(path,0,1), "dashed shape path");
        FPDFPage_InsertObject(scratch,path);
        Require(pdf_annotation_export::Add(doc,page,scratch,a,true) && pdf_annotation_export::Finalize(page),
                "dashed shape annotation");
        auto annot=FPDFPage_GetAnnot(page,0);
        Require(annot && FPDFAnnot_GetSubtype(annot)==FPDF_ANNOT_INK &&
                FPDFAnnot_GetInkListCount(annot)>10, "separate editable dash strokes");
        FPDFPage_CloseAnnot(annot);
        FPDF_ClosePage(scratch); FPDFPage_Delete(doc,1); FPDF_ClosePage(page);
        Writer writer; Require(FPDF_SaveAsCopy(doc,&writer,0)!=0,"save dashed shape without rendering");
        std::ofstream file(directory/("shape_outline_"+std::to_string(index)+".pdf"),std::ios::binary);
        file.write(reinterpret_cast<const char*>(writer.bytes.data()),writer.bytes.size()); file.close();
        Require(!file.fail(),"write dashed shape fixture");
        FPDF_CloseDocument(doc);
    }
    std::cout << "Shape outlines: 8 kinds, 4 zooms, continuous dash phase and alpha, editable PDF.\n";
}

void TextColorOverlayCases(const std::filesystem::path& directory) {
    using namespace pdf_annotation_export;
    for (int mode=1; mode<=3; ++mode) for (bool japanese : {false,true}) for (int rotation=0;rotation<4;++rotation)
    for (bool scaled : {false,true}) {
        auto doc=CreateDocument(); Require(doc!=nullptr,"color document");
        auto page=FPDFPage_New(doc,0,200,200);
        auto background=FPDFPageObj_CreateNewRect(0,0,200,200);
        Require(background && FPDFPageObj_SetFillColor(background,220,240,230,255) &&
                FPDFPath_SetDrawMode(background,FPDF_FILLMODE_WINDING,0),"color background");
        FPDFPage_InsertObject(page,background);
        auto font=FPDFText_LoadStandardFont(doc,"Helvetica");
        if (japanese) {
            FPDFFont_Close(font);
            auto dc=CreateCompatibleDC(nullptr);
            auto gdi=CreateFontW(24,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,0,0,L"Noto Sans JP");
            auto old=SelectObject(dc,gdi);
            const DWORD size=GetFontData(dc,0,0,nullptr,0);
            Require(size!=GDI_ERROR && size>0,"color Japanese test font unavailable");
            std::vector<unsigned char> bytes(size);
            Require(GetFontData(dc,0,0,bytes.data(),size)==size,"color Japanese font bytes");
            SelectObject(dc,old); DeleteObject(gdi); DeleteDC(dc);
            font=FPDFText_LoadFont(doc,bytes.data(),size,FPDF_FONT_TRUETYPE,1);
        }
        Require(font!=nullptr,"color fixture font");
        auto obj=FPDFPageObj_CreateTextObj(doc,font,18);
        Require(obj && FPDFText_SetText(obj,reinterpret_cast<FPDF_WIDESTRING>(japanese?L"文字色":L"Color")) &&
                FPDFPageObj_SetFillColor(obj,0,0,0,255),"color source text");
        FPDFPageObj_Transform(obj,1,0,0,1,40,100); FPDFPage_InsertObject(page,obj);
        Require(FPDFPage_GenerateContent(page)!=0,"color source content");
        if (scaled) {
            FS_MATRIX matrix{0.5f,0,0,0.5f,0,0};
            Require(FPDFPage_TransFormWithClip(page,&matrix,nullptr)!=0,"color page scaling");
            FPDFPage_SetMediaBox(page,0,0,100,100); FPDFPage_SetCropBox(page,0,0,100,100);
            FPDF_ClosePage(page); page=FPDF_LoadPage(doc,0);
            Require(page!=nullptr,"color scaled page reparse");
        }
        FPDFPage_SetRotation(page,rotation);
        auto text=FPDFText_LoadPage(page); Require(text!=nullptr,"color text page");
        double l=200,r=0,b=200,t=0;
        const int charCount=japanese?3:5;
        Require(FPDFText_CountChars(text)==charCount,"color source character count");
        for (int i=0;i<charCount;++i) {
            double cl=0,cr=0,cb=0,ct=0;
            Require(FPDFText_GetCharBox(text,i,&cl,&cr,&cb,&ct)!=0,"color character box");
            l=std::min(l,cl); r=std::max(r,cr); b=std::min(b,cb); t=std::max(t,ct);
        }
        FPDFText_ClosePage(text);
        Annotation a; a.type=Annotation::Type::TextColor; a.id=L"color-overlay"; a.color=RGB(240,20,30);
        a.quads={l-1,t+1,r+1,t+1,r+1,b-1,l-1,b-1};
        TextColorOptions options{static_cast<TextColorMode>(mode),RGB(80,140,200)};
        const auto before=Render(page,0);
        Require(AddTextColorOverlay(page,a,options) && Finalize(page),"color overlay creation");
        Require(FPDFPage_CountObjects(page)==2,"color overlay must preserve page objects");
        const auto after=Render(page,FPDF_ANNOT);
        unsigned black=0,recolored=0;
        for (size_t i=0;i<before.size();i+=4) if (before[i]<20 && before[i+1]<20 && before[i+2]<20) {
            ++black;
            if (after[i+2]>180 && after[i+1]<80 && after[i]<80) ++recolored;
        }
        // Thin embedded fonts have fewer fully black pixels than Helvetica.
        // Require a nonempty reference mask and the same recoloring coverage.
        if (!(black>0 && recolored*10>black*8)) {
            std::cerr<<"Color case: mode="<<mode<<" japanese="<<japanese<<" rotation="<<rotation
                     <<" black="<<black<<" recolored="<<recolored<<'\n';
            Require(false,"color overlay must follow the original glyphs");
        }
        text=FPDFText_LoadPage(page);
        Require(text && FPDFText_CountChars(text)==charCount,"color overlay original searchable text");
        FPDFText_ClosePage(text);
        const auto invisible=Render(page,0); Require(invisible==before,"hiding annotations restores source rendering");
        Writer writer; Require(FPDF_SaveAsCopy(doc,&writer,0)!=0,"color overlay save");
        const auto suffix=std::string(japanese?"_jp":"")+(rotation?"_r"+std::to_string(rotation):"")+(scaled?"_half":"");
        std::ofstream file(directory/("text_color_overlay_"+std::to_string(mode)+suffix+".pdf"),std::ios::binary);
        file.write(reinterpret_cast<const char*>(writer.bytes.data()),writer.bytes.size());
        Require(file.good(),"color fixture write"); file.close();
        if (mode==1 && !japanese && !rotation && !scaled) {
            Require(FPDFPage_RemoveAnnot(page,0) && AddTextColorOverlay(page,a,options,false) &&
                    Finalize(page),"color overlay without recovery data");
            Writer withoutRecovery;
            Require(FPDF_SaveAsCopy(doc,&withoutRecovery,0),"save color overlay without recovery data");
            auto reopened = FPDF_LoadMemDocument64(withoutRecovery.bytes.data(),withoutRecovery.bytes.size(),nullptr);
            Require(reopened != nullptr,"reopen color overlay without recovery data");
            auto reopenedPage = FPDF_LoadPage(reopened,0);
            Require(reopenedPage != nullptr,"reopen color overlay page");
            auto savedAnnot = FPDFPage_GetAnnot(reopenedPage,0);
            Require(savedAnnot && !FPDFAnnot_HasKey(savedAnnot,"PNWAnnotationV1") &&
                    !FPDFAnnot_HasKey(savedAnnot,"PNWNativeV1") &&
                    Render(reopenedPage,FPDF_ANNOT)==after,"color overlay preserves pixels and omits recovery keys");
            FPDFPage_CloseAnnot(savedAnnot);
            FPDF_ClosePage(reopenedPage);
            FPDF_CloseDocument(reopened);
        }
        options.mode=static_cast<TextColorMode>(99);
        Require(!AddTextColorOverlay(page,a,options) && FPDFPage_GetAnnotCount(page)==1,"invalid color mode rollback");
        options.mode=TextColorMode::Transparent; a.quads={1,2,3};
        Require(!AddTextColorOverlay(page,a,options) && FPDFPage_GetAnnotCount(page)==1,"malformed color quad rollback");
        FPDF_ClosePage(page); FPDFFont_Close(font); FPDF_CloseDocument(doc);
    }
}

void ArcCases() {
    for(const double angle : {0.0,shape_rotation::kPi/2,shape_rotation::kPi}) for(bool dashed:{false,true}) {
        auto doc=pdf_annotation_export::CreateDocument();Require(doc!=nullptr,"arc document");
        auto page=FPDFPage_New(doc,0,200,200),scratch=FPDFPage_New(doc,1,200,200);
        Annotation arc;arc.type=Annotation::Type::Shape;arc.shapeKind=ShapeKind::Arc;arc.id=L"arc";
        arc.x1=40;arc.x2=160;arc.y1=160;arc.y2=40;arc.alpha=1;arc.width=4;arc.shapeAngle=angle;arc.color=RGB(180,40,0);
        if(dashed)arc.dash={12,8};
        NormalizeOpenShapeStyle(arc);
        auto path=shape_outline::CreateDashedPdfPath(ShapeKind::Arc,40,160,160,40,0,arc.dash,angle);
        Require(path && FPDFPageObj_SetStrokeColor(path,180,40,0,255) && FPDFPageObj_SetStrokeWidth(path,4) &&
            FPDFPath_SetDrawMode(path,FPDF_FILLMODE_NONE,1),"open arc appearance");
        FPDFPage_InsertObject(scratch,path);
        Require(pdf_annotation_export::Add(doc,page,scratch,arc,true) && pdf_annotation_export::Finalize(page),"arc native export");
        auto annot=FPDFPage_GetAnnot(page,0);
        Require(annot && FPDFAnnot_GetSubtype(annot)==FPDF_ANNOT_INK &&
            (dashed ? FPDFAnnot_GetInkListCount(annot)>1 : FPDFAnnot_GetInkListCount(annot)==1 &&
            FPDFAnnot_GetInkListPath(annot,0,nullptr,0)==65),"arc exports editable solid/dashed open Ink strokes");
        FPDFPage_CloseAnnot(annot);
        const auto before=Render(page,FPDF_ANNOT);
        const size_t ink=angle==0 ? (200*400+80)*4 : angle==shape_rotation::kPi ? (200*400+320)*4 : (80*400+200)*4;
        const size_t gap=angle==0 ? (200*400+320)*4 : angle==shape_rotation::kPi ? (200*400+80)*4 : (320*400+200)*4;
        if(!dashed)Require(before[ink]==0 && before[ink+1]==40 && before[ink+2]==180,"arc curve remains visible after rotation/reversal");
        Require(before[gap]==255 && before[gap+1]==255 && before[gap+2]==255 && before[(200*400+200)*4]==255,
            "arc export has no closing segment or fill");
        FPDF_ClosePage(scratch);FPDFPage_Delete(doc,1);FPDF_ClosePage(page);
        Writer writer;Require(FPDF_SaveAsCopy(doc,&writer,0),"arc save");FPDF_CloseDocument(doc);
        auto loaded=FPDF_LoadMemDocument64(writer.bytes.data(),writer.bytes.size(),nullptr);Require(loaded!=nullptr,"arc reopen");
        page=FPDF_LoadPage(loaded,0);Require(page && Render(page,FPDF_ANNOT)==before,"open arc pixels survive save/reload");
        FPDF_ClosePage(page);FPDF_CloseDocument(loaded);
    }
    std::cout << "Open arc: editable stroke, rotation, no closure/fill, PDF save/reload passed.\n";
}

void ShapeAngleCase() {
    auto doc=pdf_annotation_export::CreateDocument();Require(doc!=nullptr,"rotated shape document");
    auto page=FPDFPage_New(doc,0,200,200),scratch=FPDFPage_New(doc,1,200,200),reference=FPDFPage_New(doc,2,200,200);
    Annotation a;a.type=Annotation::Type::Shape;a.shapeKind=ShapeKind::Rectangle;a.shapeAngle=0.6;a.alpha=1;a.color=RGB(200,40,20);
    a.x1=30;a.x2=170;a.y1=120;a.y2=80;a.id=L"rotated-shape";
    auto path=shape_outline::CreateDashedPdfPath(a.shapeKind,30,120,170,80,0,{},a.shapeAngle);
    Require(path && FPDFPageObj_SetFillColor(path,200,40,20,255) && FPDFPath_SetDrawMode(path,FPDF_FILLMODE_ALTERNATE,0),"rotated shape path");
    FPDFPage_InsertObject(scratch,path);
    auto rect=FPDFPageObj_CreateNewRect(30,80,140,40);Require(rect!=nullptr,"independent rotation reference");
    Require(FPDFPageObj_SetFillColor(rect,200,40,20,255) && FPDFPath_SetDrawMode(rect,FPDF_FILLMODE_ALTERNATE,0),"reference fill");
    const double c=std::cos(a.shapeAngle),s=std::sin(a.shapeAngle);
    FPDFPageObj_Transform(rect,c,-s,s,c,100-c*100-s*100,100+s*100-c*100);
    FPDFPage_InsertObject(reference,rect);Require(FPDFPage_GenerateContent(reference),"reference rotation content");
    Require(pdf_annotation_export::Add(doc,page,scratch,a,true) && pdf_annotation_export::Finalize(page),"rotated shape annotation");
    auto annot=FPDFPage_GetAnnot(page,0);Require(annot && FPDFAnnot_GetSubtype(annot)==FPDF_ANNOT_STAMP,"rotated rectangle keeps its exact appearance");
    FPDFPage_CloseAnnot(annot);
    const auto actual=Render(page,FPDF_ANNOT),expected=Render(reference,0);size_t differences=0;
    for(size_t i=0;i<actual.size();++i)if(std::abs(int(actual[i])-int(expected[i]))>2)++differences;
    Require(differences<100,"arbitrary rotation differs from independent PDF matrix reference");
    FPDF_ClosePage(reference);FPDFPage_Delete(doc,2);FPDF_ClosePage(scratch);FPDFPage_Delete(doc,1);FPDF_ClosePage(page);
    Writer writer;Require(FPDF_SaveAsCopy(doc,&writer,0),"rotated shape save");FPDF_CloseDocument(doc);
    auto loaded=FPDF_LoadMemDocument64(writer.bytes.data(),writer.bytes.size(),nullptr);Require(loaded!=nullptr,"rotated shape reload");
    page=FPDF_LoadPage(loaded,0);Require(page && Render(page,FPDF_ANNOT)==actual,"rotated shape pixels survive save/reload");
    FPDF_ClosePage(page);FPDF_CloseDocument(loaded);
    std::cout << "Arbitrary shape rotation: independent matrix and PDF save/reload passed.\n";
}

void ShapeBoundaryCases() {
    struct Case {ShapeBoundary color;bool fill,dashed;};
    for(const auto entry : {Case{ShapeBoundary::Black,false,false},Case{ShapeBoundary::Black,true,false},
        Case{ShapeBoundary::White,false,false},Case{ShapeBoundary::White,true,false},Case{ShapeBoundary::White,true,true}}) {
        const auto color=entry.color;const bool fill=entry.fill;
        auto doc=pdf_annotation_export::CreateDocument();Require(doc!=nullptr,"boundary document");
        auto page=FPDFPage_New(doc,0,200,200),scratch=FPDFPage_New(doc,1,200,200);
        auto background=FPDFPageObj_CreateNewRect(0,0,200,200);
        Require(background && FPDFPageObj_SetFillColor(background,128,128,128,255) && FPDFPath_SetDrawMode(background,2,0),"boundary backdrop");
        FPDFPage_InsertObject(page,background);Require(FPDFPage_GenerateContent(page),"boundary backdrop content");
        Annotation a;a.type=Annotation::Type::Shape;a.shapeKind=ShapeKind::Rectangle;a.shapeBoundary=color;
        a.shapeDrawMode=fill ? ShapeDrawMode::Fill : ShapeDrawMode::Outline;a.color=RGB(220,60,40);a.alpha=1;a.width=4;a.id=L"boundary";
        if(entry.dashed)a.dash={12,8};
        auto rect=FPDFPageObj_CreateNewRect(40,40,120,120);const auto stroke=ShapeBoundaryColor(color);
        Require(rect && FPDFPageObj_SetFillColor(rect,220,60,40,255) &&
            FPDFPageObj_SetStrokeColor(rect,GetRValue(stroke),GetGValue(stroke),GetBValue(stroke),255) &&
            FPDFPageObj_SetStrokeWidth(rect,4) && FPDFPath_SetDrawMode(rect,fill ? FPDF_FILLMODE_ALTERNATE : FPDF_FILLMODE_NONE,entry.dashed ? 0 : 1),"boundary reference appearance");
        FPDFPage_InsertObject(scratch,rect);
        if(entry.dashed){
            auto border=shape_outline::CreateDashedPdfPath(ShapeKind::Rectangle,40,160,160,40,0,a.dash);
            Require(border && FPDFPageObj_SetStrokeColor(border,255,255,255,255) &&
                FPDFPageObj_SetStrokeWidth(border,4) && FPDFPath_SetDrawMode(border,FPDF_FILLMODE_NONE,1),"dashed boundary reference");
            FPDFPage_InsertObject(scratch,border);
        }
        Require(pdf_annotation_export::Add(doc,page,scratch,a,true) && pdf_annotation_export::Finalize(page),"boundary standard export");
        auto annot=FPDFPage_GetAnnot(page,0);unsigned r=0,g=0,b=0,alpha=0;float hr=0,vr=0,width=0;
        Require(annot && FPDFAnnot_GetSubtype(annot)==(entry.dashed ? FPDF_ANNOT_STAMP : FPDF_ANNOT_SQUARE),"shape boundary annotation kind");
        const auto appearance = FPDFAnnot_GetObject(annot,entry.dashed ? 1 : 0);
        Require(appearance && FPDFPageObj_GetStrokeColor(appearance,&r,&g,&b,&alpha) &&
            r==GetRValue(stroke) && g==r && b==r && alpha==255 && FPDFAnnot_GetBorder(annot,&hr,&vr,&width) && width==4,"boundary appearance color/width");
        if(fill)Require(FPDFPageObj_GetFillColor(FPDFAnnot_GetObject(annot,0),&r,&g,&b,&alpha) && r==220 && g==60 && b==40,"fill color independent of boundary");
        FPDFPage_CloseAnnot(annot);
        const auto before=Render(page,FPDF_ANNOT);
        const auto edge=static_cast<size_t>(200*400+80)*4,center=static_cast<size_t>(200*400+200)*4;
        if(!entry.dashed)Require(before[edge]==GetBValue(stroke) && before[edge+1]==GetGValue(stroke) && before[edge+2]==GetRValue(stroke),"boundary visible on gray background");
        else {
            bool ink=false,gap=false;
            for(int y=90;y<310;++y){const auto i=static_cast<size_t>(y*400+80)*4;
                ink=ink || (before[i]==255 && before[i+1]==255 && before[i+2]==255);
                gap=gap || before[i]<250;}
            Require(ink && gap,"filled shape keeps white dashed boundary and gaps");
        }
        Require(before[center]==(fill ? 40 : 128) && before[center+2]==(fill ? 220 : 128),"boundary keeps fill/interior");
        FPDF_ClosePage(scratch);FPDFPage_Delete(doc,1);FPDF_ClosePage(page);
        Writer writer;Require(FPDF_SaveAsCopy(doc,&writer,0),"boundary save");FPDF_CloseDocument(doc);
        auto reopened=FPDF_LoadMemDocument64(writer.bytes.data(),writer.bytes.size(),nullptr);Require(reopened!=nullptr,"boundary reopen");
        page=FPDF_LoadPage(reopened,0);Require(page && Render(page,FPDF_ANNOT)==before,"boundary pixels survive save/reopen");
        FPDF_ClosePage(page);FPDF_CloseDocument(reopened);
    }
    std::cout << "Shape boundaries: black/white, fill/outline, filled dashed boundary and save/reopen passed.\n";
}


void StandardDetachCases() {
    // Independent PDF dictionaries, including an appearance stream that would
    // make the PDFium annotation color getter fail on the original handle.
    const std::vector<std::string> objects{
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R 14 0 R] /Count 2 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Annots [4 0 R 5 0 R 6 0 R 7 0 R 8 0 R 9 0 R 10 0 R 11 0 R 12 0 R] >>",
        "<< /Subtype /Highlight /Rect [20 100 80 112] /QuadPoints [20 112 80 112 20 100 80 100] >>",
        "<< /Subtype /Stamp /Rect [10 10 30 30] >>",
        "<< /Subtype /Highlight /F 2 /Rect [20 100 80 112] /QuadPoints [20 112 80 112 20 100 80 100] >>",
        "<< /Subtype /Highlight /Rect [20 100 80 112] /QuadPoints [20 112 80 112 20 100 80 100] /C [0 1 0] /CA 0.5 /AP << /N 13 0 R >> >>",
        "<< /Subtype /Ink /Rect [20 20 80 80] /InkList [[20 20 30 30] [40 40 50 50]] /BS << /W 3 >> /C [1 0 0] >>",
        "<< /Subtype /Line /Rect [20 20 81 81] /L [20 20 80 80] /Border [0 0 2] >>",
        "<< /Subtype /Square /Rect [20 20 80 80] /IC [0 0 1] /C [0 0 0] /Border [0 0 2] >>",
        "<< /Subtype /FreeText /Rect [90 20 180 80] /Contents (PDF text) /DA (/Helv 14 Tf 0.5 g) >>",
        "<< /Subtype /Underline /Rect [20 100 80 112] /QuadPoints [20 112 80 112 20 100 80 100] >>",
        "<< /Type /XObject /Subtype /Form /BBox [0 0 60 12] /Length 23 >>\nstream\n0 1 0 rg 0 0 60 12 re f\nendstream",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Annots [15 0 R 16 0 R 17 0 R] >>",
        "<< /Subtype /Stamp /Rect [20 100 80 112] /AP << /N 13 0 R >> >>",
        "<< /Subtype /Highlight /Rect [20 100 80 112] /QuadPoints [20 112 80 112 20 100 80 100] >>",
        "<< /Subtype /Stamp /Rect [30 50 130 150] /AP << /N 18 0 R >> >>",
        "<< /Type /XObject /Subtype /Form /BBox [0 0 10 10] /Length 39 >>\nstream\n0.5 w 1 0 0 RG 2 2 m 8 8 l 8 2 m 2 8 l S\nendstream"
    };
    std::ostringstream pdf;
    pdf << "%PDF-1.7\n";
    std::vector<std::streamoff> offsets;
    for (size_t i=0;i<objects.size();++i) {
        offsets.push_back(pdf.tellp());
        pdf << i+1 << " 0 obj\n" << objects[i] << "\nendobj\n";
    }
    const auto xref=pdf.tellp();
    pdf << "xref\n0 " << offsets.size()+1 << "\n0000000000 65535 f \n";
    for (auto offset : offsets)
        pdf << std::setw(10) << std::setfill('0') << offset << " 00000 n \n";
    pdf << "trailer\n<< /Root 1 0 R /Size " << offsets.size()+1
        << " >>\nstartxref\n" << xref << "\n%%EOF\n";
    const auto bytes=pdf.str();
    pdf_annotation_detach::Document source(FPDF_LoadMemDocument64(bytes.data(),bytes.size(),nullptr));
    Require(bool(source),"standard annotation fixture opens");
    auto original=FPDF_LoadPage(source.get(),0);
    const auto originalPixels=Render(original,FPDF_ANNOT);
    pdf_annotation_detach::Result result;
    std::wstring error;
    Require(pdf_annotation_detach::Detach(source.get(),result,error),"detach standard annotations");
    const auto& a=result.annotations;
    Require(a.size()==10 && result.converted==10 && result.unsupported==3 && result.retainedForOrder==1,
            "standard conversion and per-reason retained counts");
    Require(a[0].type==Annotation::Type::MarkerText && a[0].color==RGB(0,255,0) &&
            a[0].alpha==0.5 && a[0].quads.size()==8,"highlight reads dictionary color despite AP");
    Require(a[1].type==Annotation::Type::Freehand && a[2].type==Annotation::Type::Freehand &&
            a[1].path.size()==2 && a[1].width==1 && a[2].path.front().x==40 && a[1].id!=a[2].id,
            "Ink subpaths stay separate");
    Require(a[3].type==Annotation::Type::Line && a[3].x1==20 && a[3].y2==80 &&
            a[3].width==2,"line endpoints and width");
    Require(a[4].type==Annotation::Type::Shape && a[4].shapeDrawMode==ShapeDrawMode::Fill &&
            a[4].color==RGB(0,0,255) && a[4].shapeBoundary==ShapeBoundary::Black,"filled rectangle and border");
    Require(a[5].type==Annotation::Type::TextBox && a[5].text==L"PDF text" &&
            a[5].fontPt==14,"FreeText source and font size");
    Require(a[6].type==Annotation::Type::Line && a[6].y1==100,"underline baseline");
    Require(a[7].type==Annotation::Type::MarkerText && a[7].pageIndex==1,
            "stroke stamp permits detaching the highlight below it");
    Require(a[8].type==Annotation::Type::Freehand && a[9].type==Annotation::Type::Freehand &&
            a[8].pageIndex==1 && a[8].color==RGB(255,0,0) && a[8].width==5 &&
            a[8].path.size()==2 && a[8].path[0].x==50 && a[8].path[0].y==70 &&
            a[8].path[1].x==110 && a[8].path[1].y==130 &&
            a[9].path.size()==2 && a[9].path[0].x==110 && a[9].path[0].y==70,
            "stamp placement and stroke subpaths survive as editable ink");
    Require(FPDFPage_GetAnnotCount(original)==9 && Render(original,FPDF_ANNOT)==originalPixels,
            "original PDF annotations and pixels are untouched");
    FPDF_ClosePage(original);
    Writer writer;
    Require(FPDF_SaveAsCopy(result.pdf.get(),&writer,0),"save detached standard PDF");
    pdf_annotation_detach::Document copy(FPDF_LoadMemDocument64(writer.bytes.data(),writer.bytes.size(),nullptr));
    Require(bool(copy),"reopen detached standard PDF");
    auto page=FPDF_LoadPage(copy.get(),0);
    Require(page && FPDFPage_GetAnnotCount(page)==3,"reopened copy retains unsupported and lower annotations");
    FPDF_ClosePage(page);
    page=FPDF_LoadPage(copy.get(),1);
    Require(page && FPDFPage_GetAnnotCount(page)==1,"filled stamp stays in the copied PDF");
    auto stamp=FPDFPage_GetAnnot(page,0);
    Require(stamp && FPDFAnnot_GetSubtype(stamp)==FPDF_ANNOT_STAMP &&
            FPDFAnnot_HasKey(stamp,"AP"),"retained stamp keeps its appearance");
    FPDFPage_CloseAnnot(stamp);
    FPDF_ClosePage(page);
    std::cout << "PASS: standard annotation conversion, stacking order, and source protection\n";
}

void DetachCycles() {
    int cases=0;
    for (auto type : {Annotation::Type::MarkerText,Annotation::Type::TextColor,Annotation::Type::MarkerFree,
        Annotation::Type::TextBox,Annotation::Type::Line,Annotation::Type::Arrow,Annotation::Type::Wave,
        Annotation::Type::Freehand,Annotation::Type::LinkMarker,Annotation::Type::MathBox,Annotation::Type::Shape}) {
        Annotation original; original.type=type; original.pageIndex=0; original.id=L"cycle";
        original.text=L"精度と種類"; original.textLines={original.text,L"second line"};
        original.x1=40.123456789012345; original.y1=100.98765432101234;
        original.x2=140.23456789012345; original.y2=125.4567890123456;
        original.fontName=L"Noto Sans JP"; original.backgroundAssistMode=TextBackgroundAssistMode::Inverted;
        original.quads={40,125,140,125,140,100,40,100};
        original.path={{40,100},{80.1234567890123,110},{140,125}};
        original.dash={3.12345678901234,2}; original.linkId=L"link"; original.linkNotePath=L"notes/ノート.clro";
        original.arrowHead=ArrowHead::Double; original.color=RGB(12,34,56); original.alpha=0.45;
        Annotation current=original;
        std::vector<unsigned char> previousImage;
        pdf_annotation_detach::Document document(pdf_annotation_export::CreateDocument());
        auto initial=FPDFPage_New(document.get(),0,200,200); FPDF_ClosePage(initial);
        for(int cycle=0;cycle<3;++cycle) {
            auto page=FPDF_LoadPage(document.get(),0);
            auto scratch=FPDFPage_New(document.get(),1,200,200); Rect(scratch);
            Annotation appearance=current;
            if(type==Annotation::Type::TextColor) appearance.type=Annotation::Type::MarkerText;
            Require(pdf_annotation_export::Add(document.get(),page,scratch,appearance,false),"cycle export");
            if(type==Annotation::Type::TextColor) {
                auto a=FPDFPage_GetAnnot(page,0);
                Require(pdf_annotation_detach::Attach(a,current),"preserve TextColor type"); FPDFPage_CloseAnnot(a);
            }
            Require(pdf_annotation_export::Finalize(page),"cycle finalize");
            FPDF_ClosePage(scratch); FPDFPage_Delete(document.get(),1); FPDF_ClosePage(page);
            Writer writer; Require(FPDF_SaveAsCopy(document.get(),&writer,0),"cycle save");
            pdf_annotation_detach::Document saved(FPDF_LoadMemDocument64(writer.bytes.data(),writer.bytes.size(),nullptr));
            Require(bool(saved),"cycle reload");
            page=FPDF_LoadPage(saved.get(),0);
            const auto image=Render(page,FPDF_ANNOT);
            if(cycle) Require(image==previousImage,"cycle visual equality");
            previousImage=image; FPDF_ClosePage(page);
            pdf_annotation_detach::Result detached; std::wstring error;
            Require(pdf_annotation_detach::Detach(saved.get(),detached,error) && detached.annotations.size()==1,"cycle detach");
            current=detached.annotations[0];
            AnnotCommand expected,actual; expected.after=original; actual.after=current;
            std::string a,b;
            Require(SerializeAnnotCommandsJson({expected},&a) && SerializeAnnotCommandsJson({actual},&b) && a==b,
                "cycle full editable data equality");
            document=std::move(detached.pdf); ++cases;
        }
    }
    pdf_annotation_detach::Result retained; std::wstring error;
    pdf_annotation_detach::Document source(pdf_annotation_export::CreateDocument());
    auto attachment=FPDFDoc_AddAttachment(source.get(),reinterpret_cast<FPDF_WIDESTRING>(L"retain.txt"));
    Require(attachment && FPDFAttachment_SetFile(attachment,source.get(),"original attachment",19),"attachment fixture");
    auto page=FPDFPage_New(source.get(),0,200,200);
    auto unknown=FPDFPage_CreateAnnot(page,FPDF_ANNOT_TEXT);
    FS_RECTF rect{20,40,40,20}; Require(FPDFAnnot_SetRect(unknown,&rect),"unsupported fixture");
    FPDFPage_CloseAnnot(unknown); FPDF_ClosePage(page);
    Require(pdf_annotation_detach::Detach(source.get(),retained,error) && retained.unsupported==1 && retained.retainedForOrder==0 && retained.annotations.empty(),
        "unsupported annotation retained");
    Require(FPDFDoc_GetAttachmentCount(retained.pdf.get())==1,"document attachment retained");
    char bytes[19]{}; unsigned long count=0;
    Require(FPDFAttachment_GetFile(FPDFDoc_GetAttachment(retained.pdf.get(),0),bytes,sizeof(bytes),&count) &&
            count==19 && std::string(bytes,19)=="original attachment","attachment content retained");
    page=FPDF_LoadPage(source.get(),0);
    for(int i=0;i<3;++i) {
        auto scratch=FPDFPage_New(source.get(),1,200,200); Rect(scratch);
        Annotation a; a.pageIndex=0; a.type=Annotation::Type::MathBox; a.id=std::to_wstring(i);
        Require(pdf_annotation_export::Add(source.get(),page,scratch,a,false),"stacking fixture");
        FPDF_ClosePage(scratch); FPDFPage_Delete(source.get(),1);
        if(i==1) {
            auto foreign=FPDFPage_CreateAnnot(page,FPDF_ANNOT_TEXT);
            Require(foreign && FPDFAnnot_SetRect(foreign,&rect),"stacking retained fixture");
            FPDFPage_CloseAnnot(foreign);
        }
    }
    Require(pdf_annotation_export::Finalize(page),"stacking finalize");
    FPDF_ClosePage(page);
    Require(pdf_annotation_detach::Detach(source.get(),retained,error) && retained.unsupported==2 && retained.retainedForOrder==2 &&
            retained.annotations.size()==1 && retained.annotations[0].id==L"2","mixed stacking order preserved");
    page=FPDF_LoadPage(source.get(),0);
    auto top=FPDFPage_CreateAnnot(page,FPDF_ANNOT_TEXT);
    Require(top && FPDFAnnot_SetRect(top,&rect),"unsupported top annotation");
    FPDFPage_CloseAnnot(top);
    FPDF_ClosePage(page);
    pdf_annotation_detach::Result none;
    Require(pdf_annotation_detach::Detach(source.get(),none,error) && none.annotations.empty() &&
            none.unsupported==3 && none.retainedForOrder==3,"zero detached reports both reasons");
    page=FPDF_LoadPage(source.get(),0);
    auto corrupt=FPDFPage_GetAnnot(page,4);
    Require(corrupt && FPDFAnnot_SetStringValue(corrupt,"PNWAnnotationV1",reinterpret_cast<FPDF_WIDESTRING>(L"xx")),"invalid payload fixture");
    FPDFPage_CloseAnnot(corrupt); FPDF_ClosePage(page);
    auto prior=retained.pdf.get();
    Require(!pdf_annotation_detach::Detach(source.get(),retained,error) && retained.pdf.get()==prior &&
            retained.annotations.size()==1,"invalid payload cannot publish partial result");
    auto old=retained.pdf.get();
    Require(!pdf_annotation_detach::Detach(nullptr,retained,error) && retained.pdf.get()==old,"failure retains previous result");
    std::cout << "PASS: detach cycles=" << cases << " types=11; exact data/appearance; retained/invalid/source protection\n";
}

static void PermissionCases(const std::filesystem::path& directory) {
    using pdf_permissions::BlockMessage;
    using pdf_permissions::Operation;
    struct Case {
        const char* file;
        const char* password;
        bool canExtract;
        std::wstring_view pdfRestriction;
    };
    const Case cases[] = {
        {"permissions_copy_and_print.pdf", "reader", true, L"pdf.permissions.encrypted_pdf"},
        {"permissions_print_only.pdf", nullptr, false, L"pdf.permissions.pdf_denied"},
        {"permissions_no_copy.pdf", "reader", false, L"pdf.permissions.pdf_denied"},
        {"permissions_no_copy.pdf", "owner", true, L"pdf.permissions.encrypted_pdf"},
        {"permissions_signature.pdf", nullptr, true, L"pdf.permissions.signed_pdf"},
        {"permissions_unsigned_field.pdf", nullptr, true, L""},
    };
    for (const auto& test : cases) {
        const auto path = directory / test.file;
        pdf_annotation_detach::Document doc(FPDF_LoadDocument(path.string().c_str(), test.password));
        Require(!!doc, test.file);
        const auto* copy = BlockMessage(doc.get(), Operation::CopyText);
        const auto* image = BlockMessage(doc.get(), Operation::ExportImage);
        const auto* pdf = BlockMessage(doc.get(), Operation::ExportPdf);
        Require(std::wstring_view(copy ? copy : L"") ==
                    (test.canExtract ? L"" : L"pdf.permissions.copy_denied"), test.file);
        Require(std::wstring_view(image ? image : L"") ==
                    (test.canExtract ? L"" : L"pdf.permissions.image_denied"), test.file);
        Require(std::wstring_view(pdf ? pdf : L"") == test.pdfRestriction, test.file);
    }
    const auto encrypted = (directory / "permissions_no_copy.pdf").string();
    pdf_annotation_detach::Document wrong(FPDF_LoadDocument(encrypted.c_str(), "incorrect"));
    Require(!wrong && FPDF_GetLastError() == FPDF_ERR_PASSWORD, "wrong PDF password");
    Require(BlockMessage(nullptr, Operation::CopyText) && BlockMessage(nullptr, Operation::ExportImage) &&
            BlockMessage(nullptr, Operation::ExportPdf), "permission check must reject a missing document");
    std::cout << "PASS: PDF permissions: 6 documents by 3 operations; wrong password and missing document rejected\n";
}

int main(int argc, char** argv) {
    FPDF_InitLibrary();
    try {
        Case(Annotation::Type::MarkerText,FPDF_ANNOT_HIGHLIGHT);
        Case(Annotation::Type::Freehand,FPDF_ANNOT_INK);
        Case(Annotation::Type::Line,FPDF_ANNOT_INK);
        Case(Annotation::Type::TextBox,FPDF_ANNOT_FREETEXT);
        Case(Annotation::Type::TextBox,FPDF_ANNOT_FREETEXT,true,true);
        Case(Annotation::Type::TextBox,FPDF_ANNOT_STAMP,false);
        Case(Annotation::Type::Shape,FPDF_ANNOT_SQUARE);
        Case(Annotation::Type::MathBox,FPDF_ANNOT_STAMP);
        Case(Annotation::Type::MathBox,FPDF_ANNOT_STAMP,false,false,false);
        Case(Annotation::Type::MarkerText,FPDF_ANNOT_HIGHLIGHT,false,false,false);
        Require(!pdf_annotation_export::Finalize(nullptr) &&
                !pdf_annotation_export::CanScaleExisting(nullptr,1),"null output page must fail");
        Require(argc==2,"an explicit local fixture output directory is required");
        const std::filesystem::path directory(argv[1]);
        std::error_code ec;
        std::filesystem::create_directories(directory,ec);
        Require(!ec,"fixture directory creation");
        PermissionCases(directory);
        DetachCycles();
        StandardDetachCases();
        ArcCases();
        ShapeAngleCase();
        ShapeBoundaryCases();
        ShapeOutlineCases(directory);
        TextColorOverlayCases(directory);
        InteropFixture(directory,Annotation::Type::MarkerText,"highlight.pdf");
        InteropFixture(directory,Annotation::Type::Freehand,"ink.pdf");
        InteropFixture(directory,Annotation::Type::TextBox,"freetext.pdf");
        auto doc=pdf_annotation_export::CreateDocument();
        Require(doc!=nullptr,"initialize rejection fixture resources");
        auto page=FPDFPage_New(doc,0,200,200),scratch=FPDFPage_New(doc,1,200,200);
        Annotation a;
        Require(!pdf_annotation_export::Add(doc,page,scratch,a,true),"empty appearance must fail");
        Rect(scratch); a.type=Annotation::Type::TextColor;
        Require(!pdf_annotation_export::Add(doc,page,scratch,a,true),"text recoloring must fail");
        a.type=Annotation::Type::MarkerText; a.quads={1,2,3};
        Require(!pdf_annotation_export::Add(doc,page,scratch,a,true) && FPDFPage_GetAnnotCount(page)==0,"rollback malformed quad");
        Require(!pdf_annotation_export::Add(doc,page,page,a,true),"reject source/scratch alias");
        FPDF_ClosePage(scratch);FPDF_ClosePage(page);FPDF_CloseDocument(doc);
        FPDF_DestroyLibrary();
        std::cout << "PDF annotation export tests passed.\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr << e.what() << '\n'; FPDF_DestroyLibrary(); return 1;
    }
}
