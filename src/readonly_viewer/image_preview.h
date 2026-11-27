#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace readonly_viewer {

// True for raster formats decoded through the local Windows Imaging Component.
// Formats whose codec is not installed fail to decode and are reported quietly.
[[nodiscard]] bool IsImagePath(std::wstring_view path);

// A decoded, display-only copy. The source file is read once into memory and
// closed before decoding; nothing is written back or kept open.
class DecodedImage final {
public:
    struct Level {
        HBITMAP bitmap = nullptr;
        int width = 0;
        int height = 0;
    };
    ~DecodedImage();
    DecodedImage() = default;
    DecodedImage(const DecodedImage&) = delete;
    DecodedImage& operator=(const DecodedImage&) = delete;

    int width = 0;            // Displayed size after EXIF orientation and any memory cap.
    int height = 0;
    int sourceWidth = 0;      // Encoded frame size before orientation and reduction.
    int sourceHeight = 0;
    unsigned int frameCount = 1;
    bool reduced = false;
    std::vector<Level> levels; // levels[0] is full display size; later levels halve.
};

// On failure returns nullptr and a localization ID describing the reason.
[[nodiscard]] std::shared_ptr<const DecodedImage> DecodeImageFile(const std::wstring& path, std::wstring* errorTextId);

struct ImageViewState {
    double zoom = 1.0;
    bool fit = true;
    int scrollX = 0;
    int scrollY = 0;
    bool captured = false;
};

class ImagePreview final {
public:
    ImagePreview() = default;
    ~ImagePreview();
    ImagePreview(const ImagePreview&) = delete;
    ImagePreview& operator=(const ImagePreview&) = delete;

    [[nodiscard]] bool Create(HWND parent, HINSTANCE instance, int controlId);
    void SetImage(std::shared_ptr<const DecodedImage> image, std::wstring message);
    void Clear();
    void SetVisible(bool visible);
    void SetBounds(int x, int y, int width, int height);
    [[nodiscard]] HWND GetWindow() const noexcept { return hwnd_; }
    void ZoomIn();
    void ZoomOut();
    void FitToWindow();
    [[nodiscard]] double Zoom() const noexcept;
    [[nodiscard]] const DecodedImage* Image() const noexcept { return image_.get(); }
    [[nodiscard]] ImageViewState GetViewState() const noexcept;
    void RestoreViewState(const ImageViewState& state);

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);
    void Paint(HDC dc);
    void SetZoomAt(double zoom, POINT anchor);
    void ScrollTo(int x, int y);
    void UpdateScrollbars();
    [[nodiscard]] double FitZoom() const;
    [[nodiscard]] SIZE ContentSize() const;
    [[nodiscard]] RECT ImageRect() const;

    HWND hwnd_ = nullptr;
    std::shared_ptr<const DecodedImage> image_;
    std::wstring message_;
    double zoom_ = 1.0;
    bool fit_ = true;
    int scrollX_ = 0;
    int scrollY_ = 0;
    bool dragging_ = false;
    POINT dragOrigin_{};
    int dragScrollX_ = 0;
    int dragScrollY_ = 0;
};

} // namespace readonly_viewer
