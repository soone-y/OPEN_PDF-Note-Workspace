#pragma once

#include <windows.h>

#include "readonly_viewer/mermaid_subset_model.h"

namespace textviewer::mermaid {

constexpr UINT kOpenDetachedDiagramMessage = WM_APP + 3;

class MermaidSubsetPreview {
public:
    MermaidSubsetPreview() = default;
    ~MermaidSubsetPreview();

    MermaidSubsetPreview(const MermaidSubsetPreview&) = delete;
    MermaidSubsetPreview& operator=(const MermaidSubsetPreview&) = delete;

    [[nodiscard]] bool Create(HWND parent, HINSTANCE instance, int control_id);
    void SetModel(DiagramModel model);
    void Clear();
    void SetVisible(bool visible);
    void SetBounds(int x, int y, int width, int height);
    [[nodiscard]] HWND GetWindow() const noexcept { return hwnd_; }
    void SetDetachedWindowTarget(HWND target);
    void ZoomIn();
    void ZoomOut();
    void ResetView();

private:
    [[nodiscard]] static ATOM RegisterWindowClass(HINSTANCE instance);
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);
    void Paint(HDC hdc);
    void ScrollBy(int horizontal_delta, int vertical_delta);
    void ChangeZoom(float delta);

    HWND hwnd_ = nullptr;
    DiagramModel model_;
    HWND detached_window_target_ = nullptr;
    int scroll_x_ = 0;
    int scroll_y_ = 0;
    float zoom_ = 1.0F;
};

} // namespace textviewer::mermaid
