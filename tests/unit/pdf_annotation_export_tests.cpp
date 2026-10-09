#include "file_output/pdf_annotation_export.h"
#include "fpdf_annot.h"
#include "fpdf_edit.h"
#include "fpdf_save.h"
#include <iostream>
#include <stdexcept>
#include <vector>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

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

void Case(Annotation::Type type, int expected, bool freeText = true, bool japanese = false) {
    auto doc = pdf_annotation_export::CreateDocument();
    Require(doc!=nullptr,"initialize output resources");
    auto page = FPDFPage_New(doc, 0, 200, 200);
    auto scratch = FPDFPage_New(doc, 1, 200, 200);
    Annotation a;
    a.type = type; a.id = L"round-trip"; a.text = L"Hello PDF";
    a.fontPt = 16; a.color = RGB(0,100,240); a.alpha = 0.5;
    a.x1 = 40; a.y1 = 100; a.x2 = 140; a.y2 = 125;
    if(type == Annotation::Type::MarkerText) a.quads={40,125,140,125,140,100,40,100};
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
    Require(pdf_annotation_export::Add(doc,page,scratch,a,freeText), "annotation creation");
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
    Require(FPDFAnnot_GetStringValue(annot,"Contents",reinterpret_cast<FPDF_WCHAR*>(contents),sizeof(contents))>2 &&
            std::wstring(contents)==a.text,"Unicode annotation contents");
    auto with = Render(page,FPDF_ANNOT), without = Render(page,0);
    Require(with != without,"annotation must render separately from page");
    FPDFPage_CloseAnnot(annot); FPDF_ClosePage(page); FPDF_CloseDocument(loaded);
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
        Require(!pdf_annotation_export::Finalize(nullptr) &&
                !pdf_annotation_export::CanScaleExisting(nullptr,1),"null output page must fail");
        Require(argc==2,"an explicit local fixture output directory is required");
        const std::filesystem::path directory(argv[1]);
        std::error_code ec;
        std::filesystem::create_directories(directory,ec);
        Require(!ec,"fixture directory creation");
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
