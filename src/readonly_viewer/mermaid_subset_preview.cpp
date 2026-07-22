#include "readonly_viewer/mermaid_subset_preview.h"

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

namespace textviewer::mermaid {
namespace {

constexpr wchar_t kPreviewWindowClass[] = L"PdfNoteMermaidSubsetPreview";
constexpr int kMargin = 28;
constexpr int kMinimumNodeWidth = 150;
constexpr int kMinimumNodeHeight = 58;
constexpr int kMaximumNodeTextWidth = 300;
constexpr int kLayerGap = 92;
constexpr int kNodeGap = 44;

struct PositionedNode {
    RECT rect{};
};

SIZE MeasureNode(HDC hdc, const DiagramNode& node, float zoom) {
    const int horizontal_padding = std::max(8, static_cast<int>(16 * zoom));
    const int vertical_padding = std::max(6, static_cast<int>(12 * zoom));
    RECT single_line{};
    DrawTextW(hdc, node.label.c_str(), static_cast<int>(node.label.size()), &single_line,
              DT_CALCRECT | DT_SINGLELINE);
    const int text_width = std::max(90, std::min(static_cast<int>(kMaximumNodeTextWidth * zoom), static_cast<int>(single_line.right)));
    RECT wrapped{0, 0, text_width, 0};
    DrawTextW(hdc, node.label.c_str(), static_cast<int>(node.label.size()), &wrapped,
              DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL);
    return {
        std::max(static_cast<int>(kMinimumNodeWidth * zoom), text_width + horizontal_padding * 2),
        std::max(static_cast<int>(kMinimumNodeHeight * zoom), static_cast<int>(wrapped.bottom) + vertical_padding * 2),
    };
}

void DrawArrowHead(HDC hdc, POINT tip, POINT tail, COLORREF color, int width) {
    const int dx = tip.x - tail.x;
    const int dy = tip.y - tail.y;
    if (dx == 0 && dy == 0) return;
    const bool horizontal = abs(dx) >= abs(dy);
    POINT points[3]{};
    points[0] = tip;
    if (horizontal) {
        const int sign = dx >= 0 ? 1 : -1;
        points[1] = {tip.x - sign * 10, tip.y - 5};
        points[2] = {tip.x - sign * 10, tip.y + 5};
    } else {
        const int sign = dy >= 0 ? 1 : -1;
        points[1] = {tip.x - 5, tip.y - sign * 10};
        points[2] = {tip.x + 5, tip.y - sign * 10};
    }
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, std::max(1, width), color);
    HGDIOBJ old_brush = SelectObject(hdc, brush);
    HGDIOBJ old_pen = SelectObject(hdc, pen);
    Polygon(hdc, points, 3);
    SelectObject(hdc, old_pen);
    SelectObject(hdc, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
}

POINT AnchorFor(const RECT& rect, const RECT& other) {
    const int center_x = (rect.left + rect.right) / 2;
    const int center_y = (rect.top + rect.bottom) / 2;
    const int other_x = (other.left + other.right) / 2;
    const int other_y = (other.top + other.bottom) / 2;
    if (abs(other_x - center_x) >= abs(other_y - center_y)) {
        return {other_x >= center_x ? rect.right : rect.left, center_y};
    }
    return {center_x, other_y >= center_y ? rect.bottom : rect.top};
}

void DrawNode(HDC hdc, const DiagramNode& node, const RECT& rect) {
    const COLORREF border = RGB(70, 96, 135);
    const COLORREF fill = RGB(242, 247, 255);
    HPEN pen = CreatePen(PS_SOLID, 2, border);
    HBRUSH brush = CreateSolidBrush(fill);
    HGDIOBJ old_pen = SelectObject(hdc, pen);
    HGDIOBJ old_brush = SelectObject(hdc, brush);
    if (node.shape == NodeShape::RoundedRectangle) {
        RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, 16, 16);
    } else if (node.shape == NodeShape::Diamond) {
        POINT points[4] = {
            {(rect.left + rect.right) / 2, rect.top}, {rect.right, (rect.top + rect.bottom) / 2},
            {(rect.left + rect.right) / 2, rect.bottom}, {rect.left, (rect.top + rect.bottom) / 2},
        };
        Polygon(hdc, points, 4);
    } else {
        Rectangle(hdc, rect.left, rect.top, rect.right, rect.bottom);
    }
    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);

    RECT text_rect = rect;
    InflateRect(&text_rect, -8, -6);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(24, 35, 54));
    DrawTextW(hdc, node.label.c_str(), static_cast<int>(node.label.size()), &text_rect,
              DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_EDITCONTROL);
}

void DrawGroup(HDC hdc, const DiagramGroup& group, const RECT& rect) {
    HPEN pen = CreatePen(PS_DOT, 1, RGB(120, 140, 170));
    HBRUSH brush = CreateSolidBrush(RGB(250, 252, 255));
    HGDIOBJ old_pen = SelectObject(hdc, pen);
    HGDIOBJ old_brush = SelectObject(hdc, brush);
    Rectangle(hdc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);
    RECT label_rect{rect.left + 8, rect.top + 4, rect.right - 8, rect.top + 24};
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(66, 83, 110));
    DrawTextW(hdc, group.label.c_str(), static_cast<int>(group.label.size()), &label_rect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

bool IsGroupOrAncestor(size_t group_index, size_t expected_group, const std::vector<DiagramGroup>& groups) {
    for (size_t depth = 0; group_index != kNoDiagramGroup && depth < groups.size(); ++depth) {
        if (group_index == expected_group) return true;
        if (group_index >= groups.size()) return false;
        group_index = groups[group_index].parent_group;
    }
    return false;
}

} // namespace

MermaidSubsetPreview::~MermaidSubsetPreview() {
    if (hwnd_) DestroyWindow(hwnd_);
}

ATOM MermaidSubsetPreview::RegisterWindowClass(HINSTANCE instance) {
    static ATOM atom = 0;
    if (atom) return atom;
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kPreviewWindowClass;
    atom = RegisterClassW(&window_class);
    return atom;
}

bool MermaidSubsetPreview::Create(HWND parent, HINSTANCE instance, int control_id) {
    if (hwnd_) return true;
    if (!RegisterWindowClass(instance)) return false;
    hwnd_ = CreateWindowExW(WS_EX_CLIENTEDGE, kPreviewWindowClass, L"",
                            WS_CHILD | WS_VSCROLL | WS_HSCROLL,
                            0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)),
                            instance, this);
    return hwnd_ != nullptr;
}

void MermaidSubsetPreview::SetModel(DiagramModel model) {
    model_ = std::move(model);
    scroll_x_ = 0;
    scroll_y_ = 0;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE);
}

void MermaidSubsetPreview::Clear() {
    model_ = {};
    if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE);
}

void MermaidSubsetPreview::SetVisible(bool visible) {
    if (hwnd_) ShowWindow(hwnd_, visible ? SW_SHOW : SW_HIDE);
}

void MermaidSubsetPreview::SetBounds(int x, int y, int width, int height) {
    if (hwnd_) MoveWindow(hwnd_, x, y, std::max(0, width), std::max(0, height), TRUE);
}

void MermaidSubsetPreview::SetDetachedWindowTarget(HWND target) {
    detached_window_target_ = target;
}

void MermaidSubsetPreview::ZoomIn() {
    ChangeZoom(0.1F);
}

void MermaidSubsetPreview::ZoomOut() {
    ChangeZoom(-0.1F);
}

void MermaidSubsetPreview::ResetView() {
    zoom_ = 1.0F;
    scroll_x_ = 0;
    scroll_y_ = 0;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE);
}

void MermaidSubsetPreview::ScrollBy(int horizontal_delta, int vertical_delta) {
    scroll_x_ = std::max(0, scroll_x_ + horizontal_delta);
    scroll_y_ = std::max(0, scroll_y_ + vertical_delta);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE);
}

void MermaidSubsetPreview::ChangeZoom(float delta) {
    zoom_ = std::max(0.5F, std::min(2.0F, zoom_ + delta));
    if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE);
}

LRESULT CALLBACK MermaidSubsetPreview::WindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
    MermaidSubsetPreview* preview = reinterpret_cast<MermaidSubsetPreview*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
        preview = static_cast<MermaidSubsetPreview*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(preview));
        if (preview) preview->hwnd_ = hwnd;
    }
    if (preview && message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC hdc = BeginPaint(hwnd, &paint);
        preview->Paint(hdc);
        EndPaint(hwnd, &paint);
        return 0;
    }
    if (preview && message == WM_MOUSEWHEEL) {
        const int delta = GET_WHEEL_DELTA_WPARAM(w_param);
        if ((GET_KEYSTATE_WPARAM(w_param) & MK_CONTROL) != 0) {
            preview->ChangeZoom(delta > 0 ? 0.1F : -0.1F);
        } else {
            preview->ScrollBy(0, delta > 0 ? -48 : 48);
        }
        return 0;
    }
    if (preview && message == WM_RBUTTONUP && preview->detached_window_target_) {
        PostMessageW(preview->detached_window_target_, kOpenDetachedDiagramMessage, 0, 0);
        return 0;
    }
    if (preview && message == WM_VSCROLL) {
        const int code = LOWORD(w_param);
        preview->ScrollBy(0, code == SB_LINEUP ? -48 : (code == SB_LINEDOWN ? 48 : 0));
        return 0;
    }
    if (preview && message == WM_HSCROLL) {
        const int code = LOWORD(w_param);
        preview->ScrollBy(code == SB_LINELEFT ? -48 : (code == SB_LINERIGHT ? 48 : 0), 0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, w_param, l_param);
}

void MermaidSubsetPreview::Paint(HDC hdc) {
    RECT client{};
    GetClientRect(hwnd_, &client);
    FillRect(hdc, &client, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
    if (model_.nodes.empty()) return;

    int font_height = static_cast<int>(-14 * zoom_);
    HFONT hFont = CreateFontW(font_height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, 
                              DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, 
                              CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    if (!hFont) hFont = CreateFontW(font_height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, 
                              DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, 
                              CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Meiryo");
    HGDIOBJ old_font = SelectObject(hdc, hFont);


    const int layer_gap = static_cast<int>(kLayerGap * zoom_);
    const int node_gap = static_cast<int>(kNodeGap * zoom_);
    std::vector<SIZE> node_sizes;
    node_sizes.reserve(model_.nodes.size());
    for (const DiagramNode& node : model_.nodes) node_sizes.push_back(MeasureNode(hdc, node, zoom_));

    std::vector<size_t> layer(model_.nodes.size(), 0);
    for (size_t pass = 0; pass < model_.nodes.size(); ++pass) {
        bool changed = false;
        for (const DiagramEdge& edge : model_.edges) {
            if (edge.from_node >= layer.size() || edge.to_node >= layer.size() || edge.from_node == edge.to_node) continue;
            const size_t candidate = std::min(layer.size() - 1, layer[edge.from_node] + 1);
            if (candidate > layer[edge.to_node]) {
                layer[edge.to_node] = candidate;
                changed = true;
            }
        }
        if (!changed) break;
    }

    std::map<size_t, std::vector<size_t>> by_layer;
    for (size_t index = 0; index < layer.size(); ++index) by_layer[layer[index]].push_back(index);

    std::vector<PositionedNode> positioned(model_.nodes.size());
    const bool horizontal_flow = model_.direction == FlowDirection::LeftRight || model_.direction == FlowDirection::RightLeft;
    
    // First, find the maximum cross-axis extent among all layers to center them
    int max_cross_axis = 0;
    for (const auto& entry : by_layer) {
        int cross_axis = kMargin;
        for (const size_t node_index : entry.second) {
            const SIZE size = node_sizes[node_index];
            cross_axis += (horizontal_flow ? size.cy : size.cx) + node_gap;
        }
        cross_axis -= node_gap; // remove last gap
        if (cross_axis > max_cross_axis) max_cross_axis = cross_axis;
    }

    int layer_start = kMargin;
    for (const auto& entry : by_layer) {
        // Calculate total cross axis for this specific layer
        int layer_cross_axis = 0;
        for (const size_t node_index : entry.second) {
            const SIZE size = node_sizes[node_index];
            layer_cross_axis += (horizontal_flow ? size.cy : size.cx) + node_gap;
        }
        layer_cross_axis -= node_gap;
        
        // Center the layer's nodes relative to the maximum cross axis
        int node_start = kMargin + (max_cross_axis - layer_cross_axis) / 2;
        int layer_extent = 0;
        for (const size_t node_index : entry.second) {
            const SIZE size = node_sizes[node_index];
            if (horizontal_flow) {
                positioned[node_index].rect = {layer_start, node_start, layer_start + size.cx, node_start + size.cy};
                node_start += size.cy + node_gap;
                layer_extent = std::max(layer_extent, static_cast<int>(size.cx));
            } else {
                positioned[node_index].rect = {node_start, layer_start, node_start + size.cx, layer_start + size.cy};
                node_start += size.cx + node_gap;
                layer_extent = std::max(layer_extent, static_cast<int>(size.cy));
            }
        }
        layer_start += layer_extent + layer_gap;
    }


    int content_width = 0;
    int content_height = 0;
    for (const PositionedNode& node : positioned) {
        content_width = std::max(content_width, static_cast<int>(node.rect.right) + kMargin);
        content_height = std::max(content_height, static_cast<int>(node.rect.bottom) + kMargin);
    }
    if (model_.direction == FlowDirection::BottomTop || model_.direction == FlowDirection::RightLeft) {
        for (PositionedNode& node : positioned) {
            if (model_.direction == FlowDirection::BottomTop) {
                const int height = node.rect.bottom - node.rect.top;
                node.rect.top = content_height - node.rect.bottom;
                node.rect.bottom = node.rect.top + height;
            }
            if (model_.direction == FlowDirection::RightLeft) {
                const int width = node.rect.right - node.rect.left;
                node.rect.left = content_width - node.rect.right;
                node.rect.right = node.rect.left + width;
            }
        }
    }
    scroll_x_ = std::min(scroll_x_, std::max(0, content_width - static_cast<int>(client.right)));
    scroll_y_ = std::min(scroll_y_, std::max(0, content_height - static_cast<int>(client.bottom)));
    for (PositionedNode& node : positioned) OffsetRect(&node.rect, -scroll_x_, -scroll_y_);

    for (size_t group_index = 0; group_index < model_.groups.size(); ++group_index) {
        RECT bounds{};
        bool has_member = false;
        for (size_t node_index = 0; node_index < model_.nodes.size(); ++node_index) {
            if (!IsGroupOrAncestor(model_.nodes[node_index].group_index, group_index, model_.groups)) continue;
            if (!has_member) {
                bounds = positioned[node_index].rect;
                has_member = true;
            } else {
                UnionRect(&bounds, &bounds, &positioned[node_index].rect);
            }
        }
        if (has_member) {
            InflateRect(&bounds, 18, 28);
            DrawGroup(hdc, model_.groups[group_index], bounds);
        }
    }

    SetBkMode(hdc, TRANSPARENT);
    for (const DiagramEdge& edge : model_.edges) {
        if (edge.from_node >= positioned.size() || edge.to_node >= positioned.size()) continue;
        const RECT from = positioned[edge.from_node].rect;
        const RECT to = positioned[edge.to_node].rect;
        if (edge.from_node == edge.to_node) {
            const int loop_x = from.right + 22;
            const int loop_y = (from.top + from.bottom) / 2;
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(78, 87, 105));
            HGDIOBJ old_pen = SelectObject(hdc, pen);
            Arc(hdc, from.right - 4, from.top - 18, from.right + 38, from.bottom + 18,
                loop_x, loop_y - 10, loop_x, loop_y + 10);
            SelectObject(hdc, old_pen);
            DeleteObject(pen);
            DrawArrowHead(hdc, {from.right, loop_y + 10}, {from.right + 12, loop_y + 10}, RGB(78, 87, 105), 2);
            continue;
        }
        const POINT start = AnchorFor(from, to);
        const POINT end = AnchorFor(to, from);
        int pen_style = PS_SOLID;
        int pen_width = edge.style == EdgeStyle::ThickArrow ? 3 : 2;
        if (edge.style == EdgeStyle::DottedArrow) pen_style = PS_DOT;
        HPEN pen = CreatePen(pen_style, pen_width, RGB(78, 87, 105));
        HGDIOBJ old_pen = SelectObject(hdc, pen);
        
        MoveToEx(hdc, start.x, start.y, nullptr);
        if (horizontal_flow) {
            int mid_x = (start.x + end.x) / 2;
            LineTo(hdc, mid_x, start.y);
            LineTo(hdc, mid_x, end.y);
            LineTo(hdc, end.x, end.y);
        } else {
            int mid_y = (start.y + end.y) / 2;
            LineTo(hdc, start.x, mid_y);
            LineTo(hdc, end.x, mid_y);
            LineTo(hdc, end.x, end.y);
        }

        SelectObject(hdc, old_pen);
        DeleteObject(pen);
        if (edge.style != EdgeStyle::SolidLine) DrawArrowHead(hdc, end, start, RGB(78, 87, 105), pen_width);
        if (!edge.label.empty()) {
            RECT label_rect{(start.x + end.x) / 2 - 60, (start.y + end.y) / 2 - 12,
                            (start.x + end.x) / 2 + 60, (start.y + end.y) / 2 + 12};
            SetTextColor(hdc, RGB(50, 58, 74));
            DrawTextW(hdc, edge.label.c_str(), static_cast<int>(edge.label.size()), &label_rect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    for (size_t index = 0; index < model_.nodes.size(); ++index) {
        DrawNode(hdc, model_.nodes[index], positioned[index].rect);
    }

    SelectObject(hdc, old_font);
    DeleteObject(hFont);

}

} // namespace textviewer::mermaid
