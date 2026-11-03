#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

// Paper coordinates (before page/view rotation). No handles, global state or I/O.
namespace text_selection {
enum class Axis : uint8_t { Unknown, Horizontal, Vertical, Skip };
struct CharBox {
    float left=0, right=0, top=0, bottom=0;
    uint32_t run=0;
    Axis axis=Axis::Unknown;
};
struct Origin {
    double x=0, y=0, em=0;
    uintptr_t object=0;
    bool drawable=false;
};
struct Span {
    double top=-1e9, bottom=1e9, left=1e9, right=-1e9;
    Axis axis=Axis::Unknown;
    uint32_t run=0;
    size_t first=0, last=0;
};
[[nodiscard]] inline bool Valid(const CharBox& b) noexcept {
    return std::isfinite(b.left) && std::isfinite(b.right) && std::isfinite(b.top) &&
        std::isfinite(b.bottom) && b.right>b.left && b.top>b.bottom;
}
struct Link { Axis axis=Axis::Unknown; bool positive=false; };
[[nodiscard]] inline Link Advance(const Origin& a,const Origin& b) noexcept {
    const double em=std::max(a.em,b.em), small=std::min(a.em,b.em);
    if(!a.drawable || !b.drawable || !std::isfinite(em) || small<em*0.7 || small<=0) return {};
    const double dx=b.x-a.x,dy=b.y-a.y,ax=std::abs(dx),ay=std::abs(dy);
    // Reject column/paragraph jumps, ruby-size transitions, and diagonal layouts.
    if(ax>=em*0.15 && ax<=em*2.25 && ay<=em*0.2 && ay<=ax*0.25)
        return {Axis::Horizontal,dx>0};
    if(ay>=em*0.15 && ay<=em*2.25 && ax<=em*0.2 && ax<=ay*0.25)
        return {Axis::Vertical,dy>0};
    return {};
}

// Run once with text extraction, not on pointer moves. Retain source indices,
// including generated CR/LF. Object-local advances take precedence over nearby
// glyphs in another writing direction. Fragmented text needs two matching links.
inline void Classify(std::vector<CharBox>& boxes,const std::vector<Origin>& origins) {
    if(boxes.size()!=origins.size()) return;
    std::vector<size_t> indices; indices.reserve(boxes.size());
    for(size_t i=0;i<boxes.size();++i) {
        boxes[i].axis=Axis::Unknown;boxes[i].run=0;
        if(Valid(boxes[i]) && origins[i].drawable) indices.push_back(i);
    }
    std::vector<Link> links(indices.size());
    struct Evidence { Axis axis=Axis::Unknown; bool conflict=false; };
    std::unordered_map<uintptr_t,Evidence> objects;
    for(size_t n=1;n<indices.size();++n) {
        const auto& a=origins[indices[n-1]];const auto& b=origins[indices[n]];
        links[n]=Advance(a,b);
        if(a.object && a.object==b.object && links[n].axis!=Axis::Unknown) {
            auto& evidence=objects[a.object];
            if(evidence.axis!=Axis::Unknown && evidence.axis!=links[n].axis) evidence.conflict=true;
            evidence.axis=links[n].axis;
        }
    }
    for(size_t i:indices) {
        const auto it=objects.find(origins[i].object);
        if(it!=objects.end() && !it->second.conflict) boxes[i].axis=it->second.axis;
    }
    for(size_t n=2;n<indices.size();++n) {
        const auto axis=links[n].axis;
        if(axis==Axis::Unknown || links[n-1].axis!=axis || links[n-1].positive!=links[n].positive) continue;
        bool compatible=true;
        for(size_t k=n-2;k<=n;++k)
            if(boxes[indices[k]].axis!=Axis::Unknown && boxes[indices[k]].axis!=axis) compatible=false;
        if(compatible) for(size_t k=n-2;k<=n;++k) boxes[indices[k]].axis=axis;
    }
    uint32_t run=0;bool positive=false;
    for(size_t n=0;n<indices.size();++n) {
        auto& box=boxes[indices[n]];
        const bool join=n>0 && box.axis!=Axis::Unknown && box.axis==boxes[indices[n-1]].axis &&
            links[n].axis==box.axis && (n<2 || boxes[indices[n-1]].run!=boxes[indices[n-2]].run || links[n].positive==positive);
        if(!join) ++run;
        box.run=run;positive=links[n].positive;
    }
}

template<class Selected>
[[nodiscard]] inline std::vector<Span> Build(const std::vector<CharBox>& boxes,Selected selected,
    double pad,bool underline,bool endPad,double pageWidth,double pageHeight) {
    std::vector<Span> spans;
    for(size_t i=0;i<boxes.size();++i) {
        const auto& b=boxes[i];
        if(!Valid(b) || b.axis==Axis::Skip || !selected(i,b)) continue;
        if(spans.empty() || !b.run || b.axis==Axis::Unknown || spans.back().run!=b.run || spans.back().axis!=b.axis) {
            Span s;s.left=b.left;s.right=b.right;s.top=b.top;s.bottom=b.bottom;
            s.axis=b.axis;s.run=b.run;s.first=s.last=i;spans.push_back(s);
        } else {
            auto& s=spans.back();s.left=std::min(s.left,double(b.left));s.right=std::max(s.right,double(b.right));
            s.top=std::max(s.top,double(b.top));s.bottom=std::min(s.bottom,double(b.bottom));s.last=i;
        }
    }
    for(auto& s:spans) {
        const bool vertical=s.axis==Axis::Vertical;
        if(underline) {
            const double thickness=std::clamp(pad*3.0,2.0,40.0),drop=std::clamp(thickness*0.25,0.0,2.0);
            // Vertical emphasis line is on the right of the column.
            if(vertical) {s.right+=drop;s.left=s.right-thickness;}
            else {s.bottom-=drop;s.top=s.bottom+thickness;}
        } else if(vertical) {s.left-=pad;s.right+=pad;}
        else {s.top+=pad;s.bottom-=pad;}
        if(endPad) {
            if(vertical) {s.top=std::min(pageHeight,s.top+2.0);s.bottom=std::max(0.0,s.bottom-2.0);}
            else {s.left=std::max(0.0,s.left-2.0);s.right=std::min(pageWidth,s.right+2.0);}
        }
    }
    return spans;
}
[[nodiscard]] inline bool Intersects(const CharBox& b,double left,double right,double top,double bottom,double pad) noexcept {
    const bool vertical=b.axis==Axis::Vertical;
    return b.right+(vertical?pad:0)>=left && b.left-(vertical?pad:0)<=right &&
        b.top+(vertical?0:pad)>=bottom && b.bottom-(vertical?0:pad)<=top;
}
[[nodiscard]] inline double Distance(const CharBox& b,double x,double y) noexcept {
    const double dx=std::max({double(b.left)-x,0.0,x-double(b.right)});
    const double dy=std::max({double(b.bottom)-y,0.0,y-double(b.top)});
    return dx*dx*(b.axis==Axis::Vertical?4:1)+dy*dy*(b.axis==Axis::Horizontal?4:1);
}
[[nodiscard]] inline int Hit(const std::vector<CharBox>& boxes,double x,double y,double pad) {
    for(size_t i=0;i<boxes.size();++i) {
        const auto& b=boxes[i];
        if(Valid(b) && b.axis!=Axis::Skip && x>=b.left-pad && x<=b.right+pad && y>=b.bottom-pad && y<=b.top+pad) return int(i);
    }
    const auto spans=Build(boxes,[](size_t,const CharBox&){return true;},pad,false,false,0,0);
    double best=1e30;int index=-1;
    for(const auto& s:spans) {
        if(x<s.left || x>s.right || y<s.bottom || y>s.top) continue;
        for(size_t i=s.first;i<=s.last;++i) {
            if(!Valid(boxes[i]) || boxes[i].axis==Axis::Skip)continue;
            const double score=Distance(boxes[i],x,y);
            if(score<best){best=score;index=int(i);}
        }
    }
    return index;
}
} // namespace text_selection
