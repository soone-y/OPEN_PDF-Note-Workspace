#pragma once

#include "core/annot_types.h"
#include <cmath>
#include <utility>

// Pure geometry shared by the editor, bitmap/PDF output and readonly viewer.
// Coordinates belong to the caller; top may exceed bottom in PDF coordinates.
namespace shape_outline {
struct Point { double x, y; };
using Run = std::vector<Point>;

[[nodiscard]] inline Run Build(ShapeKind kind, double left, double top, double right,
                               double bottom, double rotation = 0.0, ShapeDrawMode drawMode = ShapeDrawMode::Outline) {
    if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) ||
        !std::isfinite(bottom) || right <= left || top == bottom) return {};
    const double cx = (left + right) * 0.5, cy = (top + bottom) * 0.5;
    if (kind == ShapeKind::Star) {
        // A regular five-point star fitted to its actual bounding box.
        // The inner radius is the intersection radius of a regular pentagram.
        const bool lines = drawMode == ShapeDrawMode::Line;
        const double pi = shape_rotation::kPi;
        const double radius = std::min((right-left)/(2*std::sin(2*pi/5)),
            std::abs(bottom-top)/(1+std::cos(pi/5)));
        const double sign = bottom > top ? 1.0 : -1.0;
        const double centerY = cy + sign*radius*(1-std::cos(pi/5))*0.5;
        Run points;
        const int count = lines ? 5 : 10;
        for (int i=0; i<count; ++i) {
            const double angle = -pi/2 + (lines ? 4*pi*i/5 : pi*i/5);
            const double r = !lines && i%2 ? radius*std::cos(2*pi/5)/std::cos(pi/5) : radius;
            points.push_back({cx+r*std::cos(angle),centerY+sign*r*std::sin(angle)});
        }
        points.push_back(points.front());
        return points;
    }
    if (kind == ShapeKind::Arc) {
        // Open half ellipse: drag width is bow depth, drag height is the end-to-end span.
        Run points;constexpr int segments=64;points.reserve(segments+1);
        for(int i=0;i<=segments;++i) {
            const double t=shape_rotation::kPi*i/segments;
            points.push_back({right-(right-left)*std::sin(t),cy-(bottom-top)*0.5*std::cos(t)});
        }
        points.front()={right,top};points.back()={right,bottom};
        return points;
    }
    if (kind == ShapeKind::Rectangle || kind == ShapeKind::Square)
        return {{left, top}, {right, top}, {right, bottom}, {left, bottom}, {left, top}};
    if (kind == ShapeKind::Diamond)
        return {{cx, top}, {right, cy}, {cx, bottom}, {left, cy}, {cx, top}};
    if (kind == ShapeKind::Triangle || kind == ShapeKind::EquilateralTriangle)
        return {{cx, top}, {right, bottom}, {left, bottom}, {cx, top}};
    const double rx = (right - left) * 0.5, ry = std::abs(bottom - top) * 0.5;
    const double angle = kind == ShapeKind::RotatedEllipse
        ? (std::isfinite(rotation) ? rotation : kDefaultRotatedEllipseAngleRad) : 0.0;
    const double c = std::cos(angle), s = std::sin(angle);
    const double fit = kind == ShapeKind::RotatedEllipse
        ? std::min(rx / std::max(0.5, std::hypot(rx * c, ry * s)),
                   ry / std::max(0.5, std::hypot(rx * s, ry * c))) : 1.0;
    Run points;
    constexpr int segments = 128;
    points.reserve(segments + 1);
    for (int i = 0; i < segments; ++i) {
        const double t = 6.28318530717958647692 * i / segments;
        const double x = rx * fit * std::cos(t), y = ry * fit * std::sin(t);
        points.push_back({cx + x * c - y * s, cy + x * s + y * c});
    }
    points.push_back(points.front());
    return points;
}

// Clockwise content orientation, in either screen (top < bottom) or PDF
// (top > bottom) coordinates. The visual ellipse angle stays local to the shape.
[[nodiscard]] inline Run BuildOriented(ShapeKind kind, double left, double top, double right,
                                       double bottom, double angle, int turns, double wholeAngle = 0.0, ShapeDrawMode drawMode = ShapeDrawMode::Outline) {
    turns = ((turns % 4) + 4) % 4;
    const double width = right - left, height = std::abs(bottom - top);
    const double localW = turns % 2 ? height : width;
    const double localH = turns % 2 ? width : height;
    auto points = Build(kind, 0, 0, localW, localH, angle,drawMode);
    const double sign = bottom > top ? 1.0 : -1.0;
    const double wholeC=wholeAngle==0.0 ? 1.0 : std::cos(wholeAngle),wholeS=wholeAngle==0.0 ? 0.0 : std::sin(sign*wholeAngle);
    const double cx=(left+right)*0.5,cy=(top+bottom)*0.5;
    for (auto& p : points) {
        const double x = p.x, y = p.y;
        if (turns == 1) { p.x = localH - y; p.y = x; }
        else if (turns == 2) { p.x = localW - x; p.y = localH - y; }
        else if (turns == 3) { p.x = y; p.y = localW - x; }
        p.x += left; p.y = top + sign * p.y;
        if (wholeAngle != 0.0) {
            const double dx=p.x-cx,dy=p.y-cy;
            p.x=cx+dx*wholeC-dy*wholeS;p.y=cy+dx*wholeS+dy*wholeC;
        }
    }
    return points;
}

// Open curves are hit along their stroke, including after whole-shape/page rotation.
[[nodiscard]] inline bool HitOpenStroke(const Annotation& ann,double x,double y,double tolerance) {
    if(!ShapeKindIsOpen(ann.shapeKind) || !std::isfinite(tolerance) || tolerance<0)return false;
    const double left=std::min(ann.x1,ann.x2),right=std::max(ann.x1,ann.x2);
    const double bottom=std::min(ann.y1,ann.y2),top=std::max(ann.y1,ann.y2);
    if(!shape_rotation::HitBox(x,y,left-tolerance,bottom-tolerance,right+tolerance,top+tolerance,ann.shapeAngle))return false;
    const auto points=BuildOriented(ann.shapeKind,left,top,right,bottom,ann.shapeRotation,ann.contentRotation,ann.shapeAngle);
    for(size_t i=1;i<points.size();++i) {
        const auto a=points[i-1],b=points[i];const double dx=b.x-a.x,dy=b.y-a.y,len=dx*dx+dy*dy;
        const double t=len>0 ? std::clamp(((x-a.x)*dx+(y-a.y)*dy)/len,0.0,1.0) : 0.0;
        const double px=x-a.x-t*dx,py=y-a.y-t*dy;
        if(px*px+py*py<=tolerance*tolerance)return true;
    }
    return false;
}

// Saved dash data is immutable. Normalize only this local rendering pattern,
// and carry its phase across vertices so curved outlines retain visible gaps.
[[nodiscard]] inline std::vector<Run> Dash(const Run& points, const std::vector<double>& dash,
                                          double scale = 1.0, double minimum = 1.0) {
    if (points.size() < 2 || !std::isfinite(scale) || scale <= 0 ||
        !std::isfinite(minimum) || minimum <= 0) return {};
    for (const auto& p : points)
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) return {};
    std::vector<double> pattern;
    for (double part : dash)
        if (std::isfinite(part) && part > 0 && std::isfinite(part * scale))
            pattern.push_back(std::max(minimum, part * scale));
    if (pattern.empty()) return {points};
    if (pattern.size() % 2) {
        const auto copy = pattern;
        pattern.insert(pattern.end(), copy.begin(), copy.end());
    }
    std::vector<Run> runs;
    size_t index = 0;
    size_t steps = 0;
    double remaining = pattern.front();
    Run run;
    for (size_t i = 1; i < points.size(); ++i) {
        const auto a = points[i - 1], b = points[i];
        const double length = std::hypot(b.x - a.x, b.y - a.y);
        if (!std::isfinite(length)) return {};
        double position = 0;
        while (position < length) {
            if (++steps > 1000000) return {};
            const double step = std::min(remaining, length - position);
            if (index % 2 == 0) {
                if (run.empty()) run.push_back({a.x + (b.x - a.x) * position / length,
                                               a.y + (b.y - a.y) * position / length});
                run.push_back({a.x + (b.x - a.x) * (position + step) / length,
                               a.y + (b.y - a.y) * (position + step) / length});
            }
            position += step;
            remaining -= step;
            if (remaining <= 1e-9) {
                if (run.size() >= 2) { runs.push_back(std::move(run)); run.clear(); }
                index = (index + 1) % pattern.size();
                remaining = pattern[index];
            }
        }
    }
    if (run.size() >= 2) runs.push_back(std::move(run));
    return runs;
}
} // namespace shape_outline
