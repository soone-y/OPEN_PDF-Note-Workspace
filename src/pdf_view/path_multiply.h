#pragma once
#include "pdf_view/marker_composite.h"
#include <vector>
#include <climits>

namespace pdf_marker {
// The caller provides client-coordinate geometry. Mask and backdrop are owned
// here, limited to two 4 MiB strips. Uncovered pixels and the source PDF stay
// unchanged. GDI is flushed before CPU access or reusing a strip.
[[nodiscard]] inline bool PaintPaths(HDC dc, const RECT& bounds, const std::vector<std::vector<POINT>>& paths,
                                      bool fill, int penWidth, COLORREF color, BYTE alpha) noexcept {
    if (!dc || penWidth <= 0) return false;
    if (alpha == 0 || paths.empty()) return true;
    RECT clip{}, visible{};
    if (GetClipBox(dc, &clip) == ERROR) return false;
    if (!IntersectRect(&visible, &bounds, &clip)) return true;
    constexpr int maxPixels = 1024*1024;
    const int width = visible.right-visible.left;
    if (width <= 0 || width > maxPixels) return false;
    const int rows = std::min<int>(visible.bottom-visible.top,maxPixels/width);
    detail::Surface target,mask;
    void* targetBits=nullptr;void* maskBits=nullptr;
    const auto create=[&](detail::Surface& surface,void*& bits) {
        surface.dc=CreateCompatibleDC(dc);
        if(!surface.dc)return false;
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-rows;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
        surface.bitmap=CreateDIBSection(surface.dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
        if(!surface.bitmap || !bits)return false;
        surface.previous=SelectObject(surface.dc,surface.bitmap);
        return surface.previous && surface.previous!=HGDI_ERROR;
    };
    if(!create(target,targetBits) || !create(mask,maskBits))return false;
    HPEN pen=fill ? nullptr : CreatePen(PS_SOLID,penWidth,RGB(255,255,255));
    if(!fill && !pen)return false;
    const auto oldPen=SelectObject(mask.dc,fill ? GetStockObject(NULL_PEN) : pen);
    const auto oldBrush=SelectObject(mask.dc,GetStockObject(fill ? WHITE_BRUSH : NULL_BRUSH));
    bool ok=true;
    for(LONG top=visible.top;top<visible.bottom && ok;top+=rows){
        const int height=std::min<LONG>(rows,visible.bottom-top);
        if(!GdiFlush()){ok=false;break;}
        std::fill_n(static_cast<uint32_t*>(maskBits),static_cast<size_t>(width)*rows,0);
        SetViewportOrgEx(mask.dc,-visible.left,-top,nullptr);
        for(const auto& path:paths){
            if(path.size()<2)continue;
            if(path.size()>static_cast<size_t>(INT_MAX)){ok=false;break;}
            ok=(fill ? Polygon(mask.dc,path.data(),static_cast<int>(path.size()))
                     : Polyline(mask.dc,path.data(),static_cast<int>(path.size())))!=0;
            if(!ok)break;
        }
        if(!ok || !BitBlt(target.dc,0,0,width,height,dc,visible.left,top,SRCCOPY) || !GdiFlush()){ok=false;break;}
        auto* pixels=static_cast<uint32_t*>(targetBits);
        const auto* coverage=static_cast<const uint32_t*>(maskBits);
        for(size_t i=0;i<static_cast<size_t>(width)*height;++i)
            if(coverage[i]&0xffffff)pixels[i]=Blend(pixels[i],color,alpha);
        ok=BitBlt(dc,visible.left,top,width,height,target.dc,0,0,SRCCOPY)!=0;
    }
    SelectObject(mask.dc,oldBrush);SelectObject(mask.dc,oldPen);
    if(pen)DeleteObject(pen);
    return ok;
}
} // namespace pdf_marker
