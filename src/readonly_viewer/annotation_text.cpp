#include "readonly_viewer/annotation_text.h"
#include "ui/annotation_text_raster.h"
#include <memory>
#include <type_traits>

namespace readonly_viewer {
namespace {
struct BitmapDelete { void operator()(HBITMAP value) const { if (value) DeleteObject(value); } };
using Bitmap = std::unique_ptr<std::remove_pointer_t<HBITMAP>, BitmapDelete>;
struct Key {
    std::wstring text, face;
    int width=0,height=0,font=0,turns=0;
    bool vertical=false, saved=false;
    bool operator==(const Key& other) const {
        return text==other.text && face==other.face && width==other.width && height==other.height &&
            font==other.font && turns==other.turns && vertical==other.vertical && saved==other.saved;
    }
};
struct Entry { Key key; Bitmap bitmap; int width=0,height=0; size_t bytes=0; };
std::vector<Entry> cache;
constexpr size_t kBudget=64ull*1024*1024;

bool MakeKey(const clrop::Item& item, Key& key, COLORREF& color) {
    if (!item.bbox) return false;
    key.turns=page_rotation::Normalize(item.contentRotation/90);
    const auto& box=*item.bbox;
    double width=std::abs(box[2]),height=std::abs(box[3]);
    if (item.bboxEndpoints) {
        width=std::abs((*item.bboxEndpoints)[2]-(*item.bboxEndpoints)[0]);
        height=std::abs((*item.bboxEndpoints)[3]-(*item.bboxEndpoints)[1]);
    }
    if (key.turns%2) std::swap(width,height);
    if (!std::isfinite(width) || !std::isfinite(height) || width<=0 || height<=0 || width>2048 || height>2048) return false;
    key.width=static_cast<int>(std::lround(width*1000));
    key.height=static_cast<int>(std::lround(height*1000));
    key.font=annotation_text::FontHeight(item.pt>0 ? item.pt : 12.0);
    key.face=item.font.empty() ? L"Segoe UI" : item.font;
    key.vertical=item.writingMode=="vertical_rl";
    key.saved=!item.lines.empty() && !key.vertical;
    if (key.saved) {
        for (size_t i=0;i<item.lines.size();++i) { if (i) key.text+=L'\n'; key.text+=item.lines[i]; }
    } else key.text=item.content.empty() ? std::wstring(item.mathKind.begin(),item.mathKind.end()) : item.content;
    unsigned int rgb=0;
    if (item.color.size()==7 && item.color.front()=='#') {
        try { rgb=static_cast<unsigned int>(std::stoul(item.color.substr(1),nullptr,16)); } catch (...) { return false; }
    }
    color=RGB((rgb>>16)&255,(rgb>>8)&255,rgb&255);
    return true;
}
}

void ClearAnnotationTextCache() { cache.clear(); }

bool DrawAnnotationText(HDC dc, const RECT& bounds, const clrop::Item& item) try {
    if (!dc || bounds.right<=bounds.left || bounds.bottom<=bounds.top) return false;
    Key key;
    COLORREF color=0;
    if (!MakeKey(item,key,color)) return false;
    auto found=std::find_if(cache.begin(),cache.end(),[&](const Entry& value){return value.key==key;});
    Entry large;
    Entry* raster=nullptr;
    if (found==cache.end()) {
        Entry value; value.key=std::move(key);
        value.bitmap.reset(annotation_text::BuildMask(dc,value.key.width/1000.0,value.key.height/1000.0,
            value.key.font,value.key.face,value.key.text,value.key.turns,value.key.vertical,
            value.key.saved,&value.width,&value.height));
        if (!value.bitmap) return false;
        value.bytes=static_cast<size_t>(value.width)*value.height*4;
        if (value.bytes>kBudget) { large=std::move(value); raster=&large; }
        else {
            size_t bytes=0; for (const auto& entry:cache) bytes+=entry.bytes;
            while (!cache.empty() && (cache.size()>=16 || bytes+value.bytes>kBudget)) {
                bytes-=cache.front().bytes; cache.erase(cache.begin());
            }
            cache.push_back(std::move(value)); raster=&cache.back();
        }
    } else raster=&*found;
    HDC memory=CreateCompatibleDC(dc);
    if (!memory) return false;
    const auto oldBitmap=SelectObject(memory,raster->bitmap.get());
    if (!oldBitmap) { DeleteDC(memory); return false; }
    const double paperWidth=(*item.bbox)[2];
    const double fontPixels=(item.pt>0 ? item.pt : 12.0)*(bounds.right-bounds.left)/std::max(0.001,paperWidth);
    const BYTE boost=fontPixels<5.5 ? 190 : fontPixels<7 ? 145 : fontPixels<8.5 ? 105 :
        fontPixels<11.5 ? 75 : fontPixels<14 ? 50 : fontPixels<18 ? 30 : 0;
    const bool painted=annotation_text::BlendMask(dc,bounds,memory,raster->width,raster->height,color,boost);
    SelectObject(memory,oldBitmap); DeleteDC(memory);
    return painted;
} catch (...) {
    return false;
}
}
