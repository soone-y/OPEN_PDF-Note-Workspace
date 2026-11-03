#pragma once

#include <windows.h>

#include "readonly_viewer/mermaid_subset_model.h"

namespace textviewer::mermaid {

constexpr UINT kOpenDetachedDiagramMessage = WM_APP + 3;

// UI-thread-owned, transient state; never serialized with document preferences.
struct DiagramViewState {
    int scroll_x = 0;
    int scroll_y = 0;
    float zoom = 1.0F;
    bool captured = false;
};

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
    void SetDocumentScrollTarget(HWND target) noexcept { document_scroll_target_ = target; }
    void ZoomIn();
    void ZoomOut();
    void ResetView();
    void FitToWindow();
    [[nodiscard]] DiagramViewState GetViewState() const noexcept;
    void RestoreViewState(const DiagramViewState& state);

private:
    [[nodiscard]] static ATOM RegisterWindowClass(HINSTANCE instance);
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);
    void Paint(HDC hdc);
    void ScrollBy(int horizontal_delta, int vertical_delta);
    void ChangeZoom(float delta);
    void HandleScroll(int bar, UINT command);
    void UpdateScrollbars(int content_width, int content_height, const RECT& client);

    HWND hwnd_ = nullptr;
    DiagramModel model_;
    HWND detached_window_target_ = nullptr;
    HWND document_scroll_target_ = nullptr;
    int scroll_x_ = 0;
    int scroll_y_ = 0;
    float zoom_ = 1.0F;
    bool fit_pending_ = false;
};

} // namespace textviewer::mermaid
