#pragma once
#include <cmath>

namespace shape_rotation {
constexpr double kPi = 3.14159265358979323846;
[[nodiscard]] inline double ArcAngleFromDrag(double startX,double endX) noexcept {
    return endX<startX ? kPi : 0.0;
}
[[nodiscard]] inline double Normalize(double radians) noexcept {
    return std::isfinite(radians) ? std::remainder(radians,2*kPi) : 0.0;
}
struct Point { double x,y; };
[[nodiscard]] inline Point Rotate(double x,double y,double cx,double cy,double radians) noexcept {
    if(radians==0.0)return {x,y};
    const double c=std::cos(radians),s=std::sin(radians),dx=x-cx,dy=y-cy;
    return {cx+dx*c-dy*s,cy+dx*s+dy*c};
}
// PDF coordinates point upward, so inverse hit testing uses the positive angle.
[[nodiscard]] inline bool HitBox(double x,double y,double left,double bottom,double right,double top,double angle) noexcept {
    const auto p=Rotate(x,y,(left+right)*0.5,(bottom+top)*0.5,angle);
    return p.x>=left && p.x<=right && p.y>=bottom && p.y<=top;
}
[[nodiscard]] inline double Drag(double initial,double startX,double startY,double x,double y) noexcept {
    if (std::hypot(x,y)<2 || std::hypot(startX,startY)<2) return initial;
    return Normalize(initial+std::atan2(y,x)-std::atan2(startY,startX));
}
} // namespace shape_rotation
