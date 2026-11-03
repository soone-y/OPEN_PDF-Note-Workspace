#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <vector>

namespace ui {
// Caller supplies client-space vertices. Paint a premultiplied mask once, so
// translucent polygon joins do not accumulate opacity.
inline void FillPolygonAlpha(HDC target, const std::vector<POINT>& points, COLORREF color, BYTE alpha) {
    if (!target || points.size() < 3 || alpha == 0) return;
    RECT bounds{points[0].x,points[0].y,points[0].x,points[0].y};
    for (const auto& p:points) {
        bounds.left=std::min(bounds.left,p.x);bounds.right=std::max(bounds.right,p.x);
        bounds.top=std::min(bounds.top,p.y);bounds.bottom=std::max(bounds.bottom,p.y);
    }
    const int64_t w=static_cast<int64_t>(bounds.right)-bounds.left,h=static_cast<int64_t>(bounds.bottom)-bounds.top;
    if (w<=0 || h<=0 || w>16384 || h>16384 || w*h>64'000'000) return;
    std::vector<POINT> local=points;for(auto& p:local) {p.x-=bounds.left;p.y-=bounds.top;}
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=static_cast<LONG>(w);info.bmiHeader.biHeight=-static_cast<LONG>(h);
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    void* bits=nullptr;HBITMAP bitmap=CreateDIBSection(target,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    HDC memory=CreateCompatibleDC(target);
    if (!bitmap || !bits || !memory) {if(bitmap) DeleteObject(bitmap);if(memory) DeleteDC(memory);return;}
    const auto oldBitmap=SelectObject(memory,bitmap);
    const auto oldBrush=SelectObject(memory,GetStockObject(WHITE_BRUSH));
    const auto oldPen=SelectObject(memory,GetStockObject(NULL_PEN));
    auto* pixels=static_cast<DWORD*>(bits);std::fill_n(pixels,static_cast<size_t>(w*h),0);
    Polygon(memory,local.data(),static_cast<int>(local.size()));GdiFlush();
    const DWORD pixel=(static_cast<DWORD>(alpha)<<24)|(GetRValue(color)*alpha/255<<16)|
        (GetGValue(color)*alpha/255<<8)|(GetBValue(color)*alpha/255);
    for(size_t i=0;i<static_cast<size_t>(w*h);++i) pixels[i]=pixels[i]?pixel:0;
    BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
    AlphaBlend(target,bounds.left,bounds.top,static_cast<int>(w),static_cast<int>(h),memory,0,0,
               static_cast<int>(w),static_cast<int>(h),blend);
    SelectObject(memory,oldPen);SelectObject(memory,oldBrush);SelectObject(memory,oldBitmap);
    DeleteDC(memory);DeleteObject(bitmap);
}
} // namespace ui
