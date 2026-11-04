#include "core/annotation_rotation.h"
#include "pdf_view/view_rotation_record.h"
#include "clrop/rotation.h"
#include "core/shape_outline.h"
#include "file_output/shape_outline_pdf.h"
#include "ui/quarter_turn_bitmap.h"
#include "ui/alpha_polygon.h"
#include "ui/dashed_shape_outline.h"
#include "fpdf_edit.h"
#include "fpdf_transformpage.h"
#include <cstdlib>
#include <iostream>

namespace {
int checks = 0;
void Expect(bool value, const char* message) {
    ++checks;
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
bool Near(double x, double y) { return std::abs(x - y) < 1e-10; }
}

int main() {
    FPDF_InitLibrary();
    const double quarter=shape_rotation::kPi/2;
    const double pi=shape_rotation::kPi;
    const double starH=100*(1+std::cos(pi/5))/(2*std::sin(2*pi/5));
    const auto star=shape_outline::Build(ShapeKind::Star,0,0,100,starH);
    const auto pentagram=shape_outline::Build(ShapeKind::Star,0,0,100,starH,0,ShapeDrawMode::Line);
    Expect(star.size()==11 && pentagram.size()==6,"star outline has ten edges; line mode has five segments");
    Expect(Near(pentagram.front().x,pentagram.back().x) && Near(pentagram.front().y,pentagram.back().y),"pentagram closes after five segments");
    double minX=100,maxX=0,minY=starH,maxY=0;
    const double edge=std::hypot(pentagram[1].x-pentagram[0].x,pentagram[1].y-pentagram[0].y);
    for(size_t i=0;i<5;++i) {
        Expect(Near(std::hypot(pentagram[i+1].x-pentagram[i].x,pentagram[i+1].y-pentagram[i].y),edge),"regular pentagram has five equal straight edges");
        minX=std::min(minX,pentagram[i].x);maxX=std::max(maxX,pentagram[i].x);
        minY=std::min(minY,pentagram[i].y);maxY=std::max(maxY,pentagram[i].y);
        const auto& outer=star[(4*i)%10];
        Expect(Near(outer.x,pentagram[i].x) && Near(outer.y,pentagram[i].y),"line mode joins every second outer vertex");
    }
    Expect(Near(minX,0) && Near(maxX,100) && Near(minY,0) && Near(maxY,starH),"regular star exactly fits fixed aspect bounds");
    for(auto mode:{ShapeDrawMode::Fill,ShapeDrawMode::Outline,ShapeDrawMode::Line}) {
        Expect(ParseShapeKind(ShapeKindToString(ShapeKind::Star))==ShapeKind::Star && ParseShapeDrawMode(ShapeDrawModeToString(mode))==mode,"star metadata round trips");
        for(int turn=0;turn<4;++turn) {
            const auto kind=ShapeKind::Star;
            const auto paper=shape_outline::BuildOriented(kind,0,0,turn%2?starH:100,turn%2?100:starH,0,turn,0.37,mode);
            const auto pdf=shape_outline::BuildOriented(kind,0,starH,100,0,0,0,0.37,mode);
            Expect(paper.size()==(mode==ShapeDrawMode::Line?6u:11u) && pdf.size()==paper.size(),"all star modes survive page and whole-shape rotation");
        }
        const auto pdf=shape_outline::CreateDashedPdfPath(ShapeKind::Star,0,starH,100,0,0,{},0.37,mode);
        Expect(pdf && FPDFPath_CountSegments(pdf)==(mode==ShapeDrawMode::Line?6:11),"PDF output retains star topology after rotation");
        FPDFPageObj_Destroy(pdf);
    }

    const auto rotated=shape_outline::BuildOriented(ShapeKind::Rectangle,0,0,120,40,0,0,quarter);
    Expect(rotated.size()==5 && Near(rotated[0].x,80) && Near(rotated[0].y,-40),"whole-shape rotation preserves the pivot and dimensions");
    Expect(shape_rotation::HitBox(60,70,0,0,120,40,quarter) && !shape_rotation::HitBox(110,20,0,0,120,40,quarter),"rotated move/erase hit box follows the shape");
    Expect(Near(shape_rotation::Drag(0,0,-100,100,0),quarter),"handle drag clockwise quarter turn");
    Expect(Near(shape_rotation::Drag(0.7,0,-100,0,0),0.7),"dragging over pivot retains the starting angle");
    Expect(std::abs(shape_rotation::Drag(0,-100,-1,-100,1))<0.03,"handle drag crosses the angle seam without a full turn");
    const auto oldEllipse=shape_outline::BuildOriented(ShapeKind::RotatedEllipse,0,0,120,40,0.4,0);
    const auto retainedEllipse=shape_outline::BuildOriented(ShapeKind::RotatedEllipse,0,0,120,40,0.4,0,0);
    bool same=oldEllipse.size()==retainedEllipse.size();
    for(size_t i=0;i<oldEllipse.size() && same;++i)same=oldEllipse[i].x==retainedEllipse[i].x && oldEllipse[i].y==retainedEllipse[i].y;
    Expect(same,"old rotated ellipse retains its fitted outline with no whole-shape angle");
    const auto oddView=shape_outline::BuildOriented(ShapeKind::Rectangle,0,0,40,120,0,1,quarter);
    Expect(Near(oddView[0].x,80) && Near(oddView[0].y,80),"whole-shape angle follows an odd page orientation");
    {
        HDC dc=CreateCompatibleDC(nullptr);BITMAPINFO info{};void* bits=nullptr;
        info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=200;info.bmiHeader.biHeight=-200;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
        auto bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
        Expect(dc && bitmap && bits,"rotated outline DIB");auto old=SelectObject(dc,bitmap);
        auto* pixels=static_cast<uint32_t*>(bits);std::fill_n(pixels,40000,0x00808080u);
        ui::DrawDashedShapeOutline(dc,ShapeKind::Rectangle,RECT{60,80,140,120},0,3,RGB(255,0,0),255,{},1,0,quarter);
        GdiFlush();
        Expect((pixels[60*200+100]&0xffffff)==0x00ff0000u && (pixels[100*200+100]&0xffffff)==0x00808080u,
            "rotated outline paints beyond its old box without filling its interior");
        SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
    }
    for(const auto size : {std::pair<double,double>{20,100},{80,100},{80,30}}) {
        const auto arc=shape_outline::Build(ShapeKind::Arc,0,0,size.first,size.second);
        Expect(arc.size()==65 && Near(arc.front().x,size.first) && Near(arc.back().x,size.first) &&
            Near(arc.front().y,0) && Near(arc.back().y,size.second) && Near(arc[32].x,0) && Near(arc[32].y,size.second/2),
            "arc drag width sets bow depth and height sets the endpoint span");
    }
    {
        Annotation arc;arc.type=Annotation::Type::Shape;arc.shapeKind=ShapeKind::Arc;
        arc.x1=0;arc.x2=80;arc.y1=100;arc.y2=0;arc.dash={8,4};
        NormalizeOpenShapeStyle(arc);
        Expect(arc.shapeDrawMode==ShapeDrawMode::Outline && arc.dash==std::vector<double>({8,4}),"arc keeps a line-style dash without filling");
        const auto leftAngle=shape_rotation::ArcAngleFromDrag(10,90),rightAngle=shape_rotation::ArcAngleFromDrag(90,10);
        const auto left=shape_outline::BuildOriented(ShapeKind::Arc,0,0,80,100,0,0,leftAngle);
        const auto right=shape_outline::BuildOriented(ShapeKind::Arc,0,0,80,100,0,0,rightAngle);
        Expect(Near(left[32].x,0) && Near(right[32].x,80) && Near(right.front().x,0),"drag direction selects opposite parentheses");
        arc.shapeAngle=rightAngle;
        Expect(shape_outline::HitOpenStroke(arc,80,50,2) && !shape_outline::HitOpenStroke(arc,0,50,2),"reversed arc hit follows its curve");
        arc.shapeAngle=0;
        Expect(shape_outline::HitOpenStroke(arc,0,50,2) && !shape_outline::HitOpenStroke(arc,80,50,2),"arc hit follows its stroke instead of the empty opening");
        arc.shapeAngle=quarter;
        Expect(shape_outline::HitOpenStroke(arc,40,90,2) && !shape_outline::HitOpenStroke(arc,40,10,2),"arc hit follows whole-shape rotation");
        HDC dc=CreateCompatibleDC(nullptr);BITMAPINFO info{};void* bits=nullptr;
        info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=200;info.bmiHeader.biHeight=-200;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
        auto bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
        Expect(dc && bitmap && bits,"arc DIB");auto old=SelectObject(dc,bitmap);
        auto* pixels=static_cast<uint32_t*>(bits);std::fill_n(pixels,40000,0x00808080u);
        ui::DrawDashedShapeOutline(dc,ShapeKind::Arc,RECT{30,40,150,160},0,3,RGB(255,0,0),255,{},1);
        GdiFlush();
        Expect((pixels[100*200+30]&0xffffff)==0x00ff0000u && (pixels[100*200+150]&0xffffff)==0x00808080u &&
            (pixels[100*200+100]&0xffffff)==0x00808080u,"arc drawing leaves its interior and opening unpainted");
        const auto inkCount=[&](){return std::count_if(pixels,pixels+40000,[](auto p){return (p&0xffffff)==0x00ff0000u;});};
        const auto solidInk=inkCount();std::fill_n(pixels,40000,0x00808080u);
        ui::DrawDashedShapeOutline(dc,ShapeKind::Arc,RECT{30,40,150,160},0,3,RGB(255,0,0),255,{8,4},1);
        GdiFlush();Expect(inkCount()>0 && inkCount()<solidInk,"dashed arc leaves real gaps");
        std::fill_n(pixels,40000,0x00808080u);
        ui::DrawDashedShapeOutline(dc,ShapeKind::Arc,RECT{30,40,150,160},0,3,RGB(255,0,0),255,{},1,0,rightAngle);
        GdiFlush();Expect((pixels[100*200+150]&0xffffff)==0x00ff0000u &&
            (pixels[100*200+30]&0xffffff)==0x00808080u,"reversed arc paints the other side with an open gap");
        SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
    }
    const std::vector<int> turns{0, 1, 2, 3, 0};
    const auto encoded = view_rotation_record::Encode(turns);
    std::vector<int> decoded{3};
    Expect(encoded && *encoded == "R1:01230", "rotation state suffix uses quarter turns");
    Expect(view_rotation_record::Decode(*encoded, decoded) && decoded == turns, "page rotations round trip");
    for (const auto invalid : {"R2:0123", "R1:4", "R1:-1", "R1:0junk", "0123"}) {
        const auto before = decoded;
        Expect(!view_rotation_record::Decode(invalid, decoded) && decoded == before, "invalid suffix preserves output");
    }
    Expect(!view_rotation_record::Encode(std::vector<int>{-1}) && !view_rotation_record::Encode(std::vector<int>{4}),
        "only normalized rotation states can be written");
    Expect(!view_rotation_record::Decode(std::string("R1:") + std::string(view_rotation_record::kMaxPages + 1, '0'), decoded),
        "rotation input is bounded");
    constexpr double width = 595.276, height = 841.89;
    const page_rotation::Point p{23.125, 701.75};
    for (int turn = 0; turn < 4; ++turn) {
        const auto projected = page_rotation::FromCanonical(p, width, height, turn);
        const auto restored = page_rotation::ToCanonical(projected, width, height, turn);
        Expect(Near(p.x, restored.x) && Near(p.y, restored.y), "non-square point inverse");
        const auto extent = page_rotation::Extent(width, height, turn);
        Expect(Near(extent.width, turn % 2 ? height : width), "odd turns exchange extent");
        Annotation text;
        text.type = Annotation::Type::TextBox;
        text.x1 = p.x; text.y1 = p.y; text.x2 = p.x + 123; text.y2 = p.y - 45;
        text.text = L"日本語 test";
        const auto view = annotation_rotation::InBasis(text, turn, width, height);
        Expect(view.has_value() && view->contentRotation == turn, "text stays attached to paper");
        const auto logical = annotation_rotation::ContentExtent(*view);
        Expect(Near(logical.width, 123) && Near(logical.height, 45), "local text dimensions preserved");
        const auto canonical = annotation_rotation::Canonical(*view);
        Expect(canonical && canonical->contentRotation == 0 &&
            Near(std::min(canonical->x1, canonical->x2), text.x1) &&
            Near(std::max(canonical->y1, canonical->y2), text.y1), "view text returns to paper coordinates");
        Annotation input = *view;
        input.contentRotation = 0;
        const auto saved = annotation_rotation::Canonical(input);
        const auto reopened = annotation_rotation::InBasis(*saved, turn, width, height);
        Expect(reopened && reopened->contentRotation == 0, "text created in rotated view reopens upright there");
        for (const int nextTurn : {0, 1, 2, 3, 0}) {
            const auto copy = annotation_rotation::InBasis(text, nextTurn, width, height);
            Expect(copy && text.x1 == p.x && text.y1 == p.y, "repeated view changes never mutate original geometry");
        }
    }
    Annotation path;
    path.type = Annotation::Type::Freehand;
    path.path = {{1.25, 2.5}, {34.5, 67.25}};
    path.quads = {1, 2, 3, 4, 5, 6, 7, 8};
    for (int turn = 0; turn < 4; ++turn) {
        const auto view = annotation_rotation::InBasis(path, turn, width, height);
        const auto restored = annotation_rotation::Canonical(*view);
        Expect(restored && Near(restored->path[1].x, 34.5) && Near(restored->quads[7], 8), "path and quad share exact basis");
    }
    for (int native = 0; native < 4; ++native) for (int view = 0; view < 4; ++view) {
        Annotation text;
        text.type=Annotation::Type::TextBox;text.x1=125;text.y1=630;text.x2=305;text.y2=585;
        const auto p = annotation_rotation::InBasis(text,native+view,500,700,20,30);
        const auto restored = annotation_rotation::Canonical(*p);
        Expect(restored && Near(std::min(restored->x1,restored->x2),125) &&
            Near(std::max(restored->y1,restored->y2),630), "native rotation and crop offset round trip");
        clrop::Item item;item.kind=clrop::Item::Kind::Text;item.bbox=std::array<double,4>{125,630,180,45};
        const auto projected=clrop::ProjectItem(item,500,700,native+view,20,30);
        Expect(projected.bbox && Near((*projected.bbox)[0],std::min(p->x1,p->x2)) &&
               projected.contentRotation==p->contentRotation*90,"readonly and editable projection agree");
    }
    for (int turn=0;turn<4;++turn) {
        const auto triangle=shape_outline::BuildOriented(ShapeKind::Triangle,10,20,turn%2?50:110,turn%2?120:60,0,turn);
        Expect(triangle.size()==4,"triangle remains a closed polygon");
        const auto expected=page_rotation::FromCanonical({50,40},100,40,turn);
        Expect(Near(triangle[0].x,10+expected.x) && Near(triangle[0].y,(turn%2?120:60)-expected.y),
               "asymmetric triangle apex rotates with paper");
    }
    HDC dc=CreateCompatibleDC(nullptr);
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=3;info.bmiHeader.biHeight=-2;info.bmiHeader.biPlanes=1;
    info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    void* bits=nullptr;HBITMAP source=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    Expect(dc && source && bits,"pixel test DIB allocation");
    const uint32_t values[]={0xff010203,0xff111213,0xff212223,0xff313233,0xff414243,0xff515253};
    std::copy(std::begin(values),std::end(values),static_cast<uint32_t*>(bits));
    for (int turn=0;turn<4;++turn) {
        int w=0,h=0;HBITMAP rotated=ui::RotateTopDownDibQuarterTurns(dc,source,turn,&w,&h);
        Expect(rotated && w==(turn%2?2:3) && h==(turn%2?3:2),"pixel extent rotates exactly");
        int rw=0,rh=0;HBITMAP restored=ui::RotateTopDownDibQuarterTurns(dc,rotated,-turn,&rw,&rh);
        DIBSECTION section{};GetObjectW(restored,sizeof(section),&section);
        Expect(restored && rw==3 && rh==2 && std::equal(std::begin(values),std::end(values),
            static_cast<uint32_t*>(section.dsBm.bmBits)),"quarter-turn raster is exactly reversible");
        DeleteObject(restored);DeleteObject(rotated);
    }
    const auto old=SelectObject(dc,source);
    Expect(ui::DrawQuarterTurnBox(dc,RECT{0,0,3,2},1,[](HDC local,const RECT& rect){
        Expect(rect.right==2 && rect.bottom==3,"rotated drawing receives logical extent");
        SetPixelV(local,0,0,RGB(254,100,20));
    }),"rotated GDI composition succeeds");
    Expect(GetPixel(dc,2,0)==RGB(254,100,20),"content top-left maps to rotated corner");
    PatBlt(dc,0,0,3,2,WHITENESS);
    ui::FillPolygonAlpha(dc,{{0,0},{3,0},{3,2},{0,2}},RGB(255,0,0),128);
    const COLORREF blended=GetPixel(dc,1,1);
    Expect(GetRValue(blended)==255 && std::abs(int(GetGValue(blended))-127)<=1 &&
           std::abs(int(GetBValue(blended))-127)<=1,"readonly shape fill retains the stored translucency");
    SelectObject(dc,old);DeleteObject(source);DeleteDC(dc);

    FPDF_DOCUMENT document=FPDF_CreateNewDocument();
    FPDF_PAGE page=FPDFPage_New(document,0,600,800);
    Expect(document && page,"native rotation fixture allocation");
    FPDFPage_SetCropBox(page,20,30,520,730);
    FPDF_PAGEOBJECT rectangle=FPDFPageObj_CreateNewRect(80,650,20,30);
    Expect(rectangle && FPDFPageObj_SetFillColor(rectangle,255,0,0,255) &&
           FPDFPath_SetDrawMode(rectangle,FPDF_FILLMODE_ALTERNATE,0),"native rotation coloured rectangle");
    FPDFPage_InsertObject(page,rectangle);Expect(FPDFPage_GenerateContent(page),"native rotation fixture content");
    FS_RECTF crop{};
    Expect(FPDF_GetPageBoundingBox(page,&crop) && crop.left==20 && crop.bottom==30 && crop.right==520 && crop.top==730,
           "PDF engine exposes original crop coordinates");
    for (int native=0;native<4;++native) for (int view=0;view<4;++view) {
        FPDFPage_SetRotation(page,native);
        const int turn=page_rotation::Normalize(native+view);
        const int w=turn%2?700:500,h=turn%2?500:700;
        FPDF_BITMAP bitmap=FPDFBitmap_Create(w,h,1);Expect(bitmap!=nullptr,"native raster allocation");
        FPDFBitmap_FillRect(bitmap,0,0,w,h,0xffffffff);
        FPDF_RenderPageBitmap(bitmap,page,0,0,w,h,view,0);
        const auto* buffer=static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(bitmap));
        const int stride=FPDFBitmap_GetStride(bitmap);
        double sumX=0,sumY=0;size_t count=0;
        for (int y=0;y<h;++y) for (int x=0;x<w;++x) {
            const auto* pixel=buffer+static_cast<size_t>(y)*stride+x*4;
            if (pixel[2]>200 && pixel[0]<30 && pixel[1]<30) {sumX+=x+0.5;sumY+=y+0.5;++count;}
        }
        const auto point=page_rotation::FromCanonical({90-20,665-30},500,700,turn);
        Expect(count>500 && std::abs(sumX/count-point.x)<1 && std::abs(sumY/count-(h-point.y))<1,
               "PDF engine native rotation and view rotation match projected crop coordinates");
        FPDFBitmap_Destroy(bitmap);
    }
    FPDF_ClosePage(page);FPDF_CloseDocument(document);FPDF_DestroyLibrary();
    path.coordinateRotation = 1;
    path.projectedCoordinates = true;
    Expect(!annotation_rotation::Canonical(path), "missing input basis fails instead of saving wrong coordinates");
    std::cout << "Page rotation geometry: " << checks << " checks passed\n";
}
