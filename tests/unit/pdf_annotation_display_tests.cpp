#define NOMINMAX
#include "pdf_view/text_coverage.h"
#include "pdf_view/marker_composite.h"
#include "pdf_view/path_multiply.h"
#include "pdf_view/text_selection_pdfium.h"
#include "core/page_rotation.h"
#include "fpdf_annot.h"
#include "fpdf_save.h"
#include "fpdf_transformpage.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <cstdio>

namespace {
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Writer : FPDF_FILEWRITE {
    std::vector<uint8_t> bytes;
    Writer() {
        version = 1;
        WriteBlock = [](FPDF_FILEWRITE* writer, const void* buffer, unsigned long size) {
            auto& output = static_cast<Writer*>(writer)->bytes;
            const auto* data = static_cast<const uint8_t*>(buffer);
            try { output.insert(output.end(), data, data + size); return 1; } catch (...) { return 0; }
        };
    }
};

FPDF_PAGEOBJECT Rectangle(FPDF_PAGE page, unsigned r, unsigned g, unsigned b) {
    auto object = FPDFPageObj_CreateNewRect(0, 0, 240, 100);
    Require(object && FPDFPageObj_SetFillColor(object, r, g, b, 255) &&
            FPDFPath_SetDrawMode(object, FPDF_FILLMODE_WINDING, false), "fixture background");
    FPDFPage_InsertObject(page, object);
    return object;
}

// Original encoded glyphs, including Japanese, are exercised after serialization.
FPDF_DOCUMENT Fixture(int mode, bool japanese, int intrinsicRotation = 0, bool crop = false) {
    auto document = FPDF_CreateNewDocument();
    Require(document != nullptr, "fixture document");
    auto page = FPDFPage_New(document, 0, 240, 100);
    Require(page != nullptr, "fixture page");
    if (mode != 0) Rectangle(page, 40, 40, 160);
    FPDF_FONT font = nullptr;
    if (japanese) {
        HDC dc = CreateCompatibleDC(nullptr);
        HFONT gdi = CreateFontW(24, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Noto Sans JP");
        auto old = SelectObject(dc, gdi);
        const DWORD size = GetFontData(dc, 0, 0, nullptr, 0);
        Require(size != GDI_ERROR && size > 0, "required Japanese test font");
        std::vector<uint8_t> bytes(size);
        Require(GetFontData(dc, 0, 0, bytes.data(), size) == size, "font data");
        SelectObject(dc, old); DeleteObject(gdi); DeleteDC(dc);
        font = FPDFText_LoadFont(document, bytes.data(), size, FPDF_FONT_TRUETYPE, true);
    } else font = FPDFText_LoadStandardFont(document, "Helvetica");
    Require(font != nullptr, "fixture font");
    auto text = FPDFPageObj_CreateTextObj(document, font, 28);
    Require(text && FPDFText_SetText(text, reinterpret_cast<FPDF_WIDESTRING>(japanese ? L"文字色 O" : L"Color O")), "fixture text");
    Require(FPDFPageObj_SetFillColor(text, mode == 1 ? 40 : 255, mode == 1 ? 40 : 255,
                                    mode == 1 ? 160 : 255, mode == 3 ? 0 : 255), "fixture color");
    Require(FPDFTextObj_SetTextRenderMode(text, mode == 2 ? FPDF_TEXTRENDERMODE_INVISIBLE : FPDF_TEXTRENDERMODE_FILL), "fixture render mode");
    FPDFPageObj_Transform(text, 1, 0.12, 0.08, 1, 20, 35);
    FPDFPage_InsertObject(page, text);
    if (mode == 4) Rectangle(page, 40, 40, 160); // actual opaque occlusion
    FPDFPage_SetRotation(page, intrinsicRotation);
    if (crop) FPDFPage_SetCropBox(page, 8, 10, 220, 90);
    Require(FPDFPage_GenerateContent(page), "fixture content");
    FPDF_ClosePage(page); FPDFFont_Close(font);
    return document;
}

std::vector<uint8_t> RenderPage(FPDF_DOCUMENT document) {
    auto page = FPDF_LoadPage(document, 0);
    Require(page != nullptr, "source page");
    auto bitmap = FPDFBitmap_Create(480, 200, true);
    Require(bitmap != nullptr, "source bitmap");
    FPDFBitmap_FillRect(bitmap, 0, 0, 480, 200, 0xffffffff);
    FPDF_RenderPageBitmap(bitmap, page, 0, 0, 480, 200, 0, 0);
    auto* data = static_cast<uint8_t*>(FPDFBitmap_GetBuffer(bitmap));
    std::vector<uint8_t> output(data, data + FPDFBitmap_GetStride(bitmap) * 200);
    FPDFBitmap_Destroy(bitmap); FPDF_ClosePage(page);
    return output;
}

void PaintCases(const std::vector<uint8_t>& coverage, int width, int height) {
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    HDC dc = CreateCompatibleDC(nullptr); void* bits = nullptr;
    auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    Require(bitmap && bits, "paint target");
    auto old = SelectObject(dc, bitmap); auto* pixels = static_cast<uint32_t*>(bits);
    const RECT page{0,0,width,height};
    size_t transparent = 0, solid = 0;
    for (const uint32_t background : {0x002828a0u, 0x00c8c8c8u, 0x00ffffffu}) {
        std::fill(pixels, pixels + static_cast<size_t>(width) * height, background);
        Require(pdf_text_coverage::Paint(dc, page, RGB(240,20,30), page, coverage, width, height), "coverage paint");
        GdiFlush();
        for (size_t i = 0; i < coverage.size(); ++i) {
            const uint32_t value = pixels[i] & 0xffffff;
            if (coverage[i] == 0) { ++transparent; Require(value == background, "background pixel changed"); }
            if (coverage[i] == 255) { ++solid; Require(value == 0x00f0141eu, "hidden glyph not revealed"); }
        }
    }
    Require(transparent > 0 && solid > 0, "coverage must distinguish glyphs and background");
    // Clip + offset + scaling uses page coordinates, and never touches the rest.
    std::fill(pixels, pixels + coverage.size(), 0x002828a0u);
    IntersectClipRect(dc, 20, 20, 80, 50);
    const RECT offset{-20,-10,width*2-20,height*2-10};
    Require(pdf_text_coverage::Paint(dc, offset, RGB(10,200,20), offset, coverage, width, height), "clipped scaled paint");
    GdiFlush(); Require(pixels[0] == 0x002828a0u, "paint escaped clip");
    SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc);
}

void LargePaintCase() {
    // Cross the per-allocation budget to exercise bounded strip composition.
    constexpr int width=4096,height=2049;
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    HDC dc=CreateCompatibleDC(nullptr);void* bits=nullptr;
    auto bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    Require(dc && bitmap && bits,"large paint surface");
    auto old=SelectObject(dc,bitmap);
    const RECT page{0,0,width,height};
    Require(pdf_text_coverage::Paint(dc,page,RGB(240,20,30),page,std::vector<uint8_t>{255},1,1),"large tiled paint");
    GdiFlush();const auto* pixels=static_cast<const uint32_t*>(bits);
    for(size_t i=0;i<static_cast<size_t>(width)*height;++i)
        Require((pixels[i]&0xffffff)==0x00f0141e,"tiled paint left a seam");
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
}

std::vector<uint8_t> DirectAlpha(FPDF_PAGE page) {
    std::vector<uint8_t> rgba(480*200*4,0), alpha(480*200);
    auto bitmap=FPDFBitmap_CreateEx(480,200,FPDFBitmap_BGRA,rgba.data(),480*4);
    Require(bitmap!=nullptr,"direct reference bitmap");
    FPDF_RenderPageBitmap(bitmap,page,0,0,480,200,0,0);
    FPDFBitmap_Destroy(bitmap);
    for(size_t i=0;i<alpha.size();++i)alpha[i]=rgba[i*4+3];
    return alpha;
}

void MarkerPaintBoundsCases() {
    constexpr int width = 4096, height = 257; // Cross the marker strip budget.
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    HDC dc = CreateCompatibleDC(nullptr); void* bits = nullptr;
    auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    Require(dc && bitmap && bits, "marker target");
    auto old = SelectObject(dc, bitmap);
    auto* pixels = static_cast<uint32_t*>(bits);
    const size_t count = static_cast<size_t>(width) * height;
    std::fill(pixels, pixels + count, 0x00ffffffu);
    const RECT area{0, 0, width, height};
    Require(pdf_marker::Paint(dc, area, RGB(255,160,0), 128), "strip marker paint");
    GdiFlush();
    for (size_t i = 0; i < count; ++i)
        Require((pixels[i] & 0xffffff) == 0x00ffcf7fu, "marker strip seam or wrong tint");
    std::fill(pixels, pixels + count, 0x00103452u);
    Require(pdf_marker::Paint(dc, area, RGB(255,160,0), 0), "zero opacity marker");
    GdiFlush();
    Require(pixels[0] == 0x00103452u && pixels[count-1] == 0x00103452u, "zero opacity changed backdrop");
    SetViewportOrgEx(dc, -10, -20, nullptr);
    IntersectClipRect(dc, 12, 21, 16, 23);
    Require(pdf_marker::Paint(dc, RECT{10,20,18,24}, RGB(255,160,0), 128), "translated clipped marker");
    GdiFlush();
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const bool inside = x >= 2 && x < 6 && y >= 1 && y < 3;
        Require((pixels[static_cast<size_t>(y)*width+x] & 0xffffff) ==
                (inside ? 0x00102a29u : 0x00103452u), "marker escaped logical clip or changed dark text");
    }
    Require(pdf_marker::Blend(0, RGB(255,160,0), 128) == 0, "marker lightened black");
    Require(!pdf_marker::Paint(nullptr, area, RGB(255,160,0), 128), "missing DC accepted");
    SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc);
}

// Independent reference: PDFium's standard Highlight annotation renderer,
// using the same color/opacity metadata as the production PDF exporter.
void MarkerPdfReferenceCases() {
    size_t cases = 0;
    for (int turn = 0; turn < 4; ++turn) for (BYTE alpha : {64,128,255}) {
        const int width = turn % 2 ? 200 : 480, height = turn % 2 ? 480 : 200;
        auto document = FPDF_CreateNewDocument();
        auto page = FPDFPage_New(document, 0, 240, 100);
        Require(document && page, "marker reference page");
        int x = 0;
        for (COLORREF color : {RGB(0,0,0), RGB(16,52,82), RGB(200,200,200), RGB(255,255,255)}) {
            auto rectangle = FPDFPageObj_CreateNewRect(static_cast<float>(x), 0, 60, 100);
            Require(rectangle && FPDFPageObj_SetFillColor(rectangle, GetRValue(color), GetGValue(color), GetBValue(color), 255) &&
                    FPDFPath_SetDrawMode(rectangle, FPDF_FILLMODE_WINDING, false), "marker reference swatches");
            FPDFPage_InsertObject(page, rectangle); x += 60;
        }
        auto font = FPDFText_LoadStandardFont(document, "Helvetica-Bold");
        auto text = FPDFPageObj_CreateTextObj(document, font, 48);
        Require(font && text && FPDFText_SetText(text, reinterpret_cast<FPDF_WIDESTRING>(L"Guide")) &&
                FPDFPageObj_SetFillColor(text, 16,52,82,255), "marker reference text");
        FPDFPageObj_Transform(text, 1,0,0,1,100,35);
        FPDFPage_InsertObject(page, text);
        Require(FPDFPage_GenerateContent(page), "marker reference content");
        const auto render = [&](int flags) {
            std::vector<uint32_t> output(static_cast<size_t>(width)*height, 0xffffffffu);
            auto image = FPDFBitmap_CreateEx(width,height,FPDFBitmap_BGRA,output.data(),width*4);
            Require(image != nullptr, "marker reference image");
            FPDF_RenderPageBitmap(image,page,0,0,width,height,turn,flags);
            FPDFBitmap_Destroy(image);
            return output;
        };
        const auto base = render(0);
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        HDC dc = CreateCompatibleDC(nullptr); void* bits = nullptr;
        auto image = CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
        Require(dc && image && bits, "marker composition image");
        auto old = SelectObject(dc,image);
        std::memcpy(bits,base.data(),base.size()*4);
        for (int layers = 1; layers <= 2; ++layers) {
            auto annotation = FPDFPage_CreateAnnot(page, FPDF_ANNOT_HIGHLIGHT);
            const FS_RECTF bounds{0,100,240,0};
            const FS_QUADPOINTSF quad{0,100,240,100,0,0,240,0};
            Require(annotation && FPDFAnnot_SetRect(annotation,&bounds) &&
                    FPDFAnnot_SetColor(annotation,FPDFANNOT_COLORTYPE_Color,255,160,0,alpha) &&
                    FPDFAnnot_AppendAttachmentPoints(annotation,&quad), "native highlight annotation");
            FPDFPage_CloseAnnot(annotation);
            const auto reference = render(FPDF_ANNOT);
            Require(pdf_marker::Paint(dc,RECT{0,0,width,height},RGB(255,160,0),alpha), "multiply marker composition");
            GdiFlush();
            const auto* actual = static_cast<const uint32_t*>(bits);
            // Native highlights curve at their outer edges. Compare the common
            // interior, including antialiased colored glyphs and dark backgrounds.
            for (int y = height/4; y < height*3/4; ++y) for (int x = width/4; x < width*3/4; ++x) {
                const size_t i = static_cast<size_t>(y)*width+x;
                for (unsigned shift : {0u,8u,16u})
                    Require(std::abs(static_cast<int>((actual[i]>>shift)&255) -
                                     static_cast<int>((reference[i]>>shift)&255)) <= 2,
                            "marker differs from standard PDF highlight");
            }
            Require(render(0) == base, "marker modified PDF content"); ++cases;
        }
        SelectObject(dc,old); DeleteObject(image); DeleteDC(dc);
        FPDFFont_Close(font); FPDF_ClosePage(page); FPDF_CloseDocument(document);
    }
    std::cout << "pdf_marker_display:ok native_pdf_cases=" << cases << " rotations=4 opacities=3 layers=2 clip=1 strips=1\n";
}

void TextAppearanceCases() {
    // Independent references use the original page renderer directly. They
    // must fail if the coverage implementation silently fills a stroke glyph.
    const FPDF_TEXT_RENDERMODE visible[]={FPDF_TEXTRENDERMODE_FILL,FPDF_TEXTRENDERMODE_STROKE,
        FPDF_TEXTRENDERMODE_FILL_STROKE,FPDF_TEXTRENDERMODE_FILL,FPDF_TEXTRENDERMODE_FILL,
        FPDF_TEXTRENDERMODE_STROKE,FPDF_TEXTRENDERMODE_FILL_STROKE,FPDF_TEXTRENDERMODE_FILL};
    for(int mode=0;mode<8;++mode){
        auto doc=Fixture(0,false);auto page=FPDF_LoadPage(doc,0);
        Require(page!=nullptr,"appearance page");auto text=FPDFPage_GetObject(page,0);
        Require(FPDFPageObj_SetFillColor(text,255,255,255,255) &&
                FPDFPageObj_SetStrokeColor(text,255,255,255,255) &&
                FPDFTextObj_SetTextRenderMode(text,visible[mode]),"reference appearance");
        const auto expected=DirectAlpha(page);
        Require(FPDFTextObj_SetTextRenderMode(text,static_cast<FPDF_TEXT_RENDERMODE>(mode)) &&
                FPDFPageObj_SetFillColor(text,255,255,255,0) &&
                FPDFPageObj_SetStrokeColor(text,255,255,255,0) && FPDFPage_GenerateContent(page),"transparent source");
        const auto before=DirectAlpha(page);std::vector<uint8_t> actual;
        Require(pdf_text_coverage::Render(doc,0,480,200,0,actual) && actual==expected,"reveal changed fill/stroke geometry");
        Require(DirectAlpha(page)==before,"coverage edited source appearance");
        FPDF_ClosePage(page);FPDF_CloseDocument(doc);
    }

}
void PathMultiplyCases() {
    constexpr int size=64;HDC dc=CreateCompatibleDC(nullptr);void* bits=nullptr;
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=size;info.bmiHeader.biHeight=-size;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    auto bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);Require(bitmap && bits,"path test DIB");
    auto old=SelectObject(dc,bitmap);auto* pixels=static_cast<uint32_t*>(bits);
    const RECT area{0,0,size,size};const uint32_t base=0x00a0c8f0u;
    const std::vector<std::vector<POINT>> square{{{10,10},{50,10},{50,50},{10,50},{10,10}}};
    auto expected=[&](unsigned b,unsigned c){return static_cast<unsigned>(std::lround(b*(1.0-128.0/255.0+(128.0/255.0)*c/255.0)));};
    for(bool fill:{false,true}){
        std::fill(pixels,pixels+size*size,base);
        Require(pdf_marker::PaintPaths(dc,area,square,fill,3,RGB(240,80,20),128),"shape multiply");GdiFlush();
        const auto inside=pixels[(fill ? 30 : 10)*size+30]&0xffffff;
        for(int c=0;c<3;++c){const unsigned color[]={20,80,240};const unsigned bg[]={240,200,160};
            Require(std::abs(int((inside>>(c*8))&255)-int(expected(bg[c],color[c])))<=1,"multiply differs from independent equation");}
        Require((pixels[3*size+3]&0xffffff)==base,"path paint changed exterior");
        if(!fill)Require((pixels[30*size+30]&0xffffff)==base,"outline painted interior");
    }
    std::fill(pixels,pixels+size*size,base);
    const std::vector<std::vector<POINT>> dashes{{{10,10},{20,10}},{{30,10},{40,10}}};
    Require(pdf_marker::PaintPaths(dc,area,dashes,false,3,RGB(240,80,20),128),"dashed multiply");GdiFlush();
    Require((pixels[10*size+15]&0xffffff)!=base && (pixels[10*size+25]&0xffffff)==base,"dash gap lost");
    SetViewportOrgEx(dc,5,7,nullptr);IntersectClipRect(dc,10,10,40,40);
    std::fill(pixels,pixels+size*size,0);Require(pdf_marker::PaintPaths(dc,area,square,true,3,RGB(240,80,20),255),"translated clipped multiply");GdiFlush();
    Require((pixels[25*size+25]&0xffffff)==0 && pixels[0]==0,"black backdrop must remain black");
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
}
void ScaledTextSelectionCases() {
    using namespace text_selection;
    auto doc=FPDF_CreateNewDocument();Require(doc!=nullptr,"scaled text document");
    auto page=FPDFPage_New(doc,0,500,400);Require(page!=nullptr,"scaled text page");
    const auto add=[&](float size,const FS_MATRIX& matrix,const char16_t* value) {
        auto obj=FPDFPageObj_NewTextObj(doc,"Helvetica",size);
        Require(obj && FPDFText_SetText(obj,reinterpret_cast<const unsigned short*>(value)) &&
            FPDFPageObj_SetMatrix(obj,&matrix),"scaled text object");
        FPDFPage_InsertObject(page,obj);
    };
    // Same visible font size, two equivalent PDF representations. The reference
    // document uses size 1 with a ~25x matrix, including separate colored objects.
    add(1,{25,0,0,25,30,340},u"Step1");
    add(1,{25,0,0,25,110,340},u"marker connection");
    add(25,{1,0,0,1,30,290},u"Step1");
    add(25,{1,0,0,1,110,290},u"marker connection");
    // A large gutter must stay split, even with the corrected size.
    add(1,{25,0,0,25,30,240},u"Left");
    add(1,{25,0,0,25,350,240},u"Right");
    // Matrix rotation changes the reading axis, not the em height.
    add(1,{0,25,-25,0,470,40},u"Vertical");
    Require(FPDFPage_GenerateContent(page),"scaled text content");
    Writer saved;Require(FPDF_SaveAsCopy(doc,&saved,FPDF_NO_INCREMENTAL),"scaled text serialize");
    FPDF_ClosePage(page);FPDF_CloseDocument(doc);
    doc=FPDF_LoadMemDocument64(saved.bytes.data(),saved.bytes.size(),nullptr);Require(doc!=nullptr,"scaled text reload");
    page=FPDF_LoadPage(doc,0);auto text=FPDFText_LoadPage(page);Require(text!=nullptr,"scaled text extract");
    const auto boxes=ReadPdf(text);
    const auto bands=Build(boxes,[](size_t,const CharBox&){return true;},0,false,false,500,400);
    Require(bands.size()==5,"scaled text connects within rows but not across rows/gutter");
    std::vector<Span> topRows;size_t vertical=0;
    for(const auto& band:bands) {
        if(band.axis==Axis::Vertical){++vertical;continue;}
        Require(band.axis==Axis::Horizontal,"scaled horizontal axis is known");
        if(band.bottom>270)topRows.push_back(band);
    }
    Require(vertical==1 && topRows.size()==2,"scaled vertical and horizontal writing remain distinct");
    Require(std::abs(topRows[0].left-topRows[1].left)<0.01 && std::abs(topRows[0].right-topRows[1].right)<0.01 &&
        std::abs(std::abs(topRows[0].top-topRows[1].top)-50)<0.01 &&
        std::abs((topRows[0].top-topRows[0].bottom)-(topRows[1].top-topRows[1].bottom))<0.01,
        "equivalent font and matrix scales produce equivalent continuous bands");
    FPDFText_ClosePage(text);FPDF_ClosePage(page);FPDF_CloseDocument(doc);
    std::cout<<"pdf_marker_scaled_text:ok equivalent_size=1 separate_objects=1 row_gutter_separation=1 rotated=1\n";
}

void WritingDirectionCases() {
    using namespace text_selection;
    std::vector<CharBox> boxes;std::vector<Origin> origins;
    auto add=[&](double x,double y,double em,uintptr_t object) {
        boxes.push_back({float(x),float(x+em*0.8),float(y+em*0.8),float(y)});
        origins.push_back({x,y,em,object,true});
    };
    // Two vertical columns, two horizontal rows, and smaller ruby on one page.
    for(double x:{100,130}) for(double y:{180,160,140}) add(x,y,20,x==100?1:2);
    for(double y:{100,80}) for(double x:{100,120,140}) add(x,y,20,y==100?3:4);
    for(double y:{180,170,160}) add(122,y,8,5);
    Classify(boxes,origins);
    auto all=[](size_t,const CharBox&){return true;};
    const auto spans=Build(boxes,all,2,false,true,300,300);
    Require(spans.size()==5,"mixed directions must remain separate columns/rows/ruby");
    for(size_t i=0;i<6;++i) Require(boxes[i].axis==Axis::Vertical,"vertical direction");
    for(size_t i=6;i<12;++i) Require(boxes[i].axis==Axis::Horizontal,"horizontal rows must not become vertical");
    Require(spans[0].left==98 && spans[0].right==118 && spans[0].top==198 && spans[0].bottom==138,"vertical padding axes");
    Require(spans[2].left==98 && spans[2].right==158 && spans[2].top==118 && spans[2].bottom==98,"horizontal padding axes");
    const auto partial=Build(boxes,[](size_t i,const CharBox&){return i>=1 && i<=2;},0,false,false,300,300);
    Require(partial.size()==1 && partial[0].top==176 && partial[0].bottom==140,"partial column selection");
    const auto rect=Build(boxes,[](size_t,const CharBox& b){return Intersects(b,99,117,180,139,0);},0,false,false,300,300);
    Require(rect.size()==1 && rect[0].run==boxes[0].run,"rectangle excludes neighbouring column/ruby");
    const auto lines=Build(boxes,all,2,true,true,300,300);
    Require(lines[0].right==117.5 && lines[0].left==111.5 && lines[2].bottom==98.5,"vertical right-side line and horizontal underline");
    Require(Hit(boxes,108,158,0)>=0 && Hit(boxes,108,158,0)<3,"vertical inter-glyph gap hit");
    Require(Hit(boxes,124,145,0)==-1,"column gutter is not selectable text");
    for(int turn=0;turn<4;++turn) {
        auto view=boxes;
        for(auto& b:view) {
            auto a=page_rotation::FromCanonical({b.left,b.bottom},300,300,turn);
            auto c=page_rotation::FromCanonical({b.right,b.top},300,300,turn);
            a=page_rotation::ToCanonical(a,300,300,turn);c=page_rotation::ToCanonical(c,300,300,turn);
            b.left=float(std::min(a.x,c.x));b.right=float(std::max(a.x,c.x));
            b.bottom=float(std::min(a.y,c.y));b.top=float(std::max(a.y,c.y));
        }
        const auto restored=Build(view,all,2,false,true,300,300);
        Require(restored.size()==spans.size() && restored[0].left==spans[0].left && restored[2].top==spans[2].top,
            "page rotation preserves writing axes");
    }
    boxes.clear();origins.clear();
    for(int i=0;i<3;++i)add(100,180-i*20,20,uintptr_t(10+i));
    Classify(boxes,origins);
    Require(Build(boxes,all,0,false,false,300,300).size()==1 && boxes[0].axis==Axis::Vertical,
        "one text object per glyph needs consistent advance evidence");
    boxes.resize(2);origins.resize(2);Classify(boxes,origins);
    Require(boxes[0].axis==Axis::Unknown && Build(boxes,all,0,false,false,300,300).size()==2,
        "ambiguous independent glyphs must not be guessed into a column");
    boxes.clear();origins.clear();
    // A short horizontal insertion (tate-chu-yoko) splits the surrounding column.
    add(100,180,20,20);add(100,160,20,20);add(100,140,10,21);add(110,140,10,21);add(100,120,20,20);
    Classify(boxes,origins);
    const auto mixed=Build(boxes,all,0,false,false,300,300);
    Require(mixed.size()==3 && boxes[2].axis==Axis::Horizontal && boxes[4].axis==Axis::Vertical,
        "horizontal insertion cannot change its surrounding vertical text");

    // Real PDF: upright Japanese vertical fonts have angle 0, just like horizontal fonts.
    const std::string content="BT /V 20 Tf 160 230 Td <65E5672C8A9E> Tj ET\nBT /V 20 Tf 120 230 Td <65875B57> Tj ET\nBT /H 20 Tf 20 50 Td <65E5672C8A9E> Tj ET\n";
    const std::vector<std::string> objects={
        "<< /Type /Catalog /Pages 2 0 R >>","<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Resources << /Font << /V 4 0 R /H 6 0 R >> >> /Contents 7 0 R >>",
        "<< /Type /Font /Subtype /Type0 /BaseFont /HeiseiMin-W3 /Encoding /UniJIS-UTF16-V /DescendantFonts [5 0 R] >>",
        "<< /Type /Font /Subtype /CIDFontType0 /BaseFont /HeiseiMin-W3 /CIDSystemInfo << /Registry (Adobe) /Ordering (Japan1) /Supplement 6 >> /DW 1000 >>",
        "<< /Type /Font /Subtype /Type0 /BaseFont /HeiseiMin-W3 /Encoding /UniJIS-UTF16-H /DescendantFonts [5 0 R] >>",
        "<< /Length "+std::to_string(content.size())+" >>\nstream\n"+content+"endstream"};
    std::string pdf="%PDF-1.7\n";std::vector<size_t> offsets{0};
    for(size_t i=0;i<objects.size();++i){offsets.push_back(pdf.size());pdf+=std::to_string(i+1)+" 0 obj\n"+objects[i]+"\nendobj\n";}
    const auto xref=pdf.size();pdf+="xref\n0 8\n0000000000 65535 f \n";
    for(size_t i=1;i<offsets.size();++i){char line[40];std::snprintf(line,sizeof line,"%010llu 00000 n \n",static_cast<unsigned long long>(offsets[i]));pdf+=line;}
    pdf+="trailer << /Size 8 /Root 1 0 R >>\nstartxref\n"+std::to_string(xref)+"\n%%EOF\n";
    auto doc=FPDF_LoadMemDocument64(pdf.data(),pdf.size(),nullptr);Require(doc!=nullptr,"vertical PDF fixture");
    auto page=FPDF_LoadPage(doc,0);auto text=FPDFText_LoadPage(page);Require(text!=nullptr,"vertical PDF text page");
    const auto actual=ReadPdf(text);const auto actualSpans=Build(actual,all,2,false,true,300,300);
    Require(actual.size()==size_t(FPDFText_CountChars(text)) && actualSpans.size()==3,"real PDF indices and separated runs");
    size_t vertical=0,horizontal=0;
    for(size_t i=0;i<actual.size();++i) {
        const auto ch=FPDFText_GetUnicode(text,int(i));
        if(ch==13 || ch==10) {Require(!Valid(actual[i]),"generated newline must not create a marker");continue;}
        Require(FPDFText_GetCharAngle(text,int(i))==0,"fixture must distinguish upright vertical from upright horizontal");
        if(actual[i].axis==Axis::Vertical)++vertical;
        if(actual[i].axis==Axis::Horizontal)++horizontal;
    }
    Require(vertical==5 && horizontal==3,"real vertical/horizontal Japanese distinction");
    FPDFText_ClosePage(text);
    auto annotation=FPDFPage_CreateAnnot(page,FPDF_ANNOT_HIGHLIGHT);
    const FS_RECTF bounds{0,300,300,0};
    Require(annotation && FPDFAnnot_SetRect(annotation,&bounds) &&
        FPDFAnnot_SetColor(annotation,FPDFANNOT_COLORTYPE_Color,255,255,0,128),"vertical marker output");
    for(const auto& s:actualSpans) {
        const FS_QUADPOINTSF q{float(s.left),float(s.top),float(s.right),float(s.top),
            float(s.left),float(s.bottom),float(s.right),float(s.bottom)};
        Require(FPDFAnnot_AppendAttachmentPoints(annotation,&q),"vertical marker quad");
    }
    FPDFPage_CloseAnnot(annotation);FPDF_ClosePage(page);
    Writer saved;Require(FPDF_SaveAsCopy(doc,&saved,FPDF_NO_INCREMENTAL),"vertical marker save");
    auto reopened=FPDF_LoadMemDocument64(saved.bytes.data(),saved.bytes.size(),nullptr);
    Require(reopened!=nullptr,"vertical marker reload");page=FPDF_LoadPage(reopened,0);
    annotation=FPDFPage_GetAnnot(page,0);
    Require(annotation && FPDFAnnot_CountAttachmentPoints(annotation)==3,"vertical/horizontal bands survive reload");
    FPDFPage_CloseAnnot(annotation);
    auto bitmap=FPDFBitmap_Create(300,300,true);Require(bitmap!=nullptr,"vertical marker bitmap");
    FPDFBitmap_FillRect(bitmap,0,0,300,300,0xffffffff);
    FPDF_RenderPageBitmap(bitmap,page,0,0,300,300,0,FPDF_ANNOT);
    const auto* pixels=static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bitmap));const int stride=FPDFBitmap_GetStride(bitmap);
    const auto blue=[&](int x,int y){return pixels[(300-y-1)*stride+x*4];};
    Require(blue(121,210)<240 && blue(140,210)==255,"vertical band is continuous through glyph gap and leaves the gutter clear");
    FPDFBitmap_Destroy(bitmap);FPDF_ClosePage(page);FPDF_CloseDocument(reopened);FPDF_CloseDocument(doc);
    std::cout<<"pdf_marker_writing_direction:ok mixed=1 ruby=1 insertion=1 fragmented=1 ambiguous=1 real_pdf=1 save_render=1\n";
}
} // namespace

int main() {
    FPDF_InitLibrary();
    try {
        ScaledTextSelectionCases();
        WritingDirectionCases();
        MarkerPaintBoundsCases();
        MarkerPdfReferenceCases();
        PathMultiplyCases();
        LargePaintCase();
        TextAppearanceCases();
        size_t cases = 0;
        for (bool japanese : {false, true}) for (int intrinsic : {0,1}) for (bool crop : {false,true}) {
            auto reference = Fixture(0, japanese, intrinsic, crop);
            for (int rotation = 0; rotation < 4; ++rotation) {
                const int width = rotation % 2 ? 200 : 480, height = rotation % 2 ? 480 : 200;
                std::vector<uint8_t> expected;
                Require(pdf_text_coverage::Render(reference, 0, width, height, rotation, expected), "reference coverage");
                Require(std::count_if(expected.begin(), expected.end(), [](auto a){return a != 0;}) > 100, "empty reference");
                for (int mode = 1; mode <= 4; ++mode) {
                    auto original = Fixture(mode, japanese, intrinsic, crop);
                    Writer serialized; Require(FPDF_SaveAsCopy(original, &serialized, FPDF_NO_INCREMENTAL), "serialize fixture");
                    auto source = FPDF_LoadMemDocument64(serialized.bytes.data(), serialized.bytes.size(), nullptr);
                    Require(source != nullptr, "reload fixture");
                    const auto before = RenderPage(source);
                    std::vector<uint8_t> actual;
                    Require(pdf_text_coverage::Render(source, 0, width, height, rotation, actual), "hidden text coverage");
                    Require(actual == expected, "same-color/invisible/transparent/occluded glyph coverage differs");
                    Require(RenderPage(source) == before, "source page changed");
                    if (!japanese && intrinsic == 0 && !crop && rotation == 0) PaintCases(actual, width, height);
                    FPDF_CloseDocument(source); FPDF_CloseDocument(original); ++cases;
                }
            }
            FPDF_CloseDocument(reference);
        }
        auto inner = Fixture(4, false);
        auto nested = FPDF_CreateNewDocument(); auto page = FPDFPage_New(nested,0,240,100);
        auto xobject = FPDF_NewXObjectFromPage(nested,inner,0);
        Require(xobject != nullptr, "form source");
        auto form = FPDF_NewFormObjectFromXObject(xobject); Require(form != nullptr, "nested form");
        FPDFPage_InsertObject(page,form); Require(FPDFPage_GenerateContent(page), "form content");
        std::vector<uint8_t> plain, grouped;
        Require(pdf_text_coverage::Render(inner,0,480,200,0,plain) &&
                pdf_text_coverage::Render(nested,0,480,200,0,grouped) && plain == grouped,"form glyph coverage");
        FPDF_CloseXObject(xobject); FPDF_ClosePage(page); FPDF_CloseDocument(nested);
        auto start = std::chrono::steady_clock::now();
        Require(pdf_text_coverage::Render(inner,0,1200,500,0,plain), "timed coverage");
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
        const auto retained = plain;
        Require(!pdf_text_coverage::Render(inner,0,100000,100000,0,plain) && plain == retained, "size failure must preserve output");
        Require(!pdf_text_coverage::Render(inner,99,480,200,0,plain) && plain == retained, "page failure must preserve output");
        int width=100000,height=100000; pdf_text_coverage::LimitSize(width,height);
        Require(static_cast<uint64_t>(width)*height <= pdf_text_coverage::kMaxPixels, "coverage limit");
        FPDF_CloseDocument(inner);
        std::cout << "pdf_annotation_display:ok hidden_cases=" << cases << " forms=1 paint_backgrounds=3 rotations=4 crops=2 languages=2\n"
                  << "coverage_sample_ms=" << elapsed << " renders=1 pixels=600000 cache_bytes=600000 appearance_modes=8\n";
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n'; FPDF_DestroyLibrary(); return 1;
    }
    FPDF_DestroyLibrary(); return 0;
}
