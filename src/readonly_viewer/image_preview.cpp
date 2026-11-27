#include "readonly_viewer/image_preview.h"

#include <wincodec.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <limits>

#include "readonly_viewer/reader_chrome.h"

namespace readonly_viewer {
namespace {

constexpr wchar_t kImagePreviewClass[] = L"PdfReadonlyViewerImagePreview";
constexpr unsigned long long kMaximumImageFileBytes = 512ull * 1024ull * 1024ull;
// Display copy budget: about 128 MiB at 32 bpp, plus a third for halved levels.
constexpr unsigned long long kMaximumDisplayPixels = 32ull * 1024ull * 1024ull;
constexpr int kMinimumLevelEdge = 256;
constexpr double kMinimumZoom = 0.02;
constexpr double kMaximumZoom = 16.0;
constexpr double kZoomStep = 1.25;
constexpr COLORREF kImageBackground = RGB(255, 255, 255);

template <typename T>
class ComPtr final {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T** Put() { Reset(); return &value_; }
    T* Get() const { return value_; }
    T* operator->() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }
    void Reset() { if (value_) { value_->Release(); value_ = nullptr; } }
private:
    T* value_ = nullptr;
};

[[nodiscard]] std::wstring LowerExtension(std::wstring_view path) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring_view::npos || (slash != std::wstring_view::npos && dot < slash)) return {};
    std::wstring extension(path.substr(dot));
    for (wchar_t& c : extension) c = static_cast<wchar_t>(towlower(c));
    return extension;
}

[[nodiscard]] bool ReadWholeFile(const std::wstring& path, std::vector<unsigned char>* bytes, std::wstring* error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) { *error = L"readonly.image.error.open"; return false; }
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0;
    if (ok && static_cast<unsigned long long>(size.QuadPart) > kMaximumImageFileBytes) {
        *error = L"readonly.image.error.too_large";
        ok = false;
    } else if (!ok) {
        *error = L"readonly.image.error.open";
    }
    if (ok) {
        try {
            bytes->resize(static_cast<size_t>(size.QuadPart));
        } catch (...) {
            *error = L"readonly.image.error.memory";
            ok = false;
        }
    }
    size_t offset = 0;
    while (ok && offset < bytes->size()) {
        const DWORD request = static_cast<DWORD>(std::min<size_t>(bytes->size() - offset, 16u * 1024u * 1024u));
        DWORD read = 0;
        if (!ReadFile(file, bytes->data() + offset, request, &read, nullptr) || read == 0) {
            *error = L"readonly.image.error.open";
            ok = false;
            break;
        }
        offset += read;
    }
    CloseHandle(file);
    return ok;
}

[[nodiscard]] WICBitmapTransformOptions OrientationTransform(IWICBitmapFrameDecode* frame) {
    ComPtr<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(reader.Put())) || !reader) return WICBitmapTransformRotate0;
    unsigned int orientation = 1;
    for (const wchar_t* query : {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"}) {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(reader->GetMetadataByName(query, &value))) {
            if (value.vt == VT_UI2) orientation = value.uiVal;
            PropVariantClear(&value);
            break;
        }
        PropVariantClear(&value);
    }
    switch (orientation) {
        case 2: return WICBitmapTransformFlipHorizontal;
        case 3: return WICBitmapTransformRotate180;
        case 4: return WICBitmapTransformFlipVertical;
        case 5: return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
        case 6: return WICBitmapTransformRotate90;
        case 7: return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
        case 8: return WICBitmapTransformRotate270;
        default: return WICBitmapTransformRotate0;
    }
}

[[nodiscard]] HBITMAP CreateLevelBitmap(int width, int height, uint32_t** bits) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* memory = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &memory, nullptr, 0);
    *bits = static_cast<uint32_t*>(memory);
    return bitmap;
}

// Each level halves the previous one with a box filter, so drawing never
// shrinks a source by more than 2x and stays fast and smooth.
[[nodiscard]] bool AddHalfLevel(DecodedImage& image) {
    const DecodedImage::Level& source = image.levels.back();
    const int width = std::max(1, source.width / 2);
    const int height = std::max(1, source.height / 2);
    DIBSECTION section{};
    if (!GetObjectW(source.bitmap, sizeof(section), &section) || !section.dsBm.bmBits) return false;
    const auto* input = static_cast<const uint32_t*>(section.dsBm.bmBits);
    uint32_t* output = nullptr;
    HBITMAP bitmap = CreateLevelBitmap(width, height, &output);
    if (!bitmap) return false;
    for (int y = 0; y < height; ++y) {
        const int y0 = std::min(source.height - 1, y * 2);
        const int y1 = std::min(source.height - 1, y * 2 + 1);
        for (int x = 0; x < width; ++x) {
            const int x0 = std::min(source.width - 1, x * 2);
            const int x1 = std::min(source.width - 1, x * 2 + 1);
            const uint32_t p[4] = {input[static_cast<size_t>(y0) * source.width + x0], input[static_cast<size_t>(y0) * source.width + x1],
                                   input[static_cast<size_t>(y1) * source.width + x0], input[static_cast<size_t>(y1) * source.width + x1]};
            uint32_t result = 0xff000000u;
            for (int shift = 0; shift < 24; shift += 8) {
                unsigned int sum = 2;
                for (uint32_t value : p) sum += (value >> shift) & 0xffu;
                result |= (sum / 4) << shift;
            }
            output[static_cast<size_t>(y) * width + x] = result;
        }
    }
    image.levels.push_back({bitmap, width, height});
    return true;
}

} // namespace

bool IsImagePath(std::wstring_view path) {
    static const wchar_t* const kExtensions[] = {
        L".png", L".jpg", L".jpeg", L".jpe", L".jfif", L".bmp", L".dib", L".gif", L".tif", L".tiff",
        L".ico", L".webp", L".jxr", L".wdp", L".hdp", L".heic", L".heif", L".avif",
    };
    const std::wstring extension = LowerExtension(path);
    return std::any_of(std::begin(kExtensions), std::end(kExtensions),
                       [&](const wchar_t* candidate) { return extension == candidate; });
}

DecodedImage::~DecodedImage() {
    for (Level& level : levels) if (level.bitmap) DeleteObject(level.bitmap);
}

std::shared_ptr<const DecodedImage> DecodeImageFile(const std::wstring& path, std::wstring* errorTextId) {
    std::wstring ignored;
    std::wstring& error = errorTextId ? *errorTextId : ignored;
    error.clear();
    std::vector<unsigned char> bytes;
    if (!ReadWholeFile(path, &bytes, &error)) return nullptr;
    if (bytes.size() > std::numeric_limits<DWORD>::max()) { error = L"readonly.image.error.too_large"; return nullptr; }

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IWICImagingFactory, reinterpret_cast<void**>(factory.Put()))) || !factory) {
        error = L"readonly.image.error.codec";
        return nullptr;
    }
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(factory->CreateStream(stream.Put())) ||
        FAILED(stream->InitializeFromMemory(bytes.data(), static_cast<DWORD>(bytes.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.Put())) ||
        FAILED(decoder->GetFrame(0, frame.Put()))) {
        error = L"readonly.image.error.decode";
        return nullptr;
    }
    auto image = std::make_shared<DecodedImage>();
    UINT frames = 1;
    if (SUCCEEDED(decoder->GetFrameCount(&frames)) && frames > 0) image->frameCount = frames;
    UINT width = 0, height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0 ||
        width > 0x7fffffffu || height > 0x7fffffffu) {
        error = L"readonly.image.error.decode";
        return nullptr;
    }
    image->sourceWidth = static_cast<int>(width);
    image->sourceHeight = static_cast<int>(height);

    IWICBitmapSource* current = frame.Get();
    ComPtr<IWICBitmapScaler> scaler;
    const unsigned long long pixels = static_cast<unsigned long long>(width) * height;
    if (pixels > kMaximumDisplayPixels) {
        const double scale = std::sqrt(static_cast<double>(kMaximumDisplayPixels) / static_cast<double>(pixels));
        const UINT scaledWidth = std::max(1u, static_cast<UINT>(width * scale));
        const UINT scaledHeight = std::max(1u, static_cast<UINT>(height * scale));
        if (FAILED(factory->CreateBitmapScaler(scaler.Put())) ||
            FAILED(scaler->Initialize(current, scaledWidth, scaledHeight, WICBitmapInterpolationModeFant))) {
            error = L"readonly.image.error.decode";
            return nullptr;
        }
        current = scaler.Get();
        image->reduced = true;
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.Put())) ||
        FAILED(converter->Initialize(current, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                     nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        error = L"readonly.image.error.decode";
        return nullptr;
    }
    current = converter.Get();
    ComPtr<IWICBitmapFlipRotator> rotator;
    const WICBitmapTransformOptions transform = OrientationTransform(frame.Get());
    if (transform != WICBitmapTransformRotate0 && SUCCEEDED(factory->CreateBitmapFlipRotator(rotator.Put())) &&
        SUCCEEDED(rotator->Initialize(current, transform))) {
        current = rotator.Get();
    }
    if (FAILED(current->GetSize(&width, &height)) || width == 0 || height == 0 ||
        static_cast<unsigned long long>(width) * height > kMaximumDisplayPixels + width + height) {
        error = L"readonly.image.error.decode";
        return nullptr;
    }
    // Release the encoded copy as soon as pixels are available.
    uint32_t* pixelsOut = nullptr;
    HBITMAP bitmap = CreateLevelBitmap(static_cast<int>(width), static_cast<int>(height), &pixelsOut);
    if (!bitmap || !pixelsOut) {
        if (bitmap) DeleteObject(bitmap);
        error = L"readonly.image.error.memory";
        return nullptr;
    }
    image->levels.push_back({bitmap, static_cast<int>(width), static_cast<int>(height)});
    const UINT stride = width * 4;
    const WICRect all{0, 0, static_cast<INT>(width), static_cast<INT>(height)};
    if (FAILED(current->CopyPixels(&all, stride, stride * height, reinterpret_cast<BYTE*>(pixelsOut)))) {
        error = L"readonly.image.error.decode";
        return nullptr;
    }
    GdiFlush();
    // Composite premultiplied alpha onto the fixed viewer background once.
    const size_t count = static_cast<size_t>(width) * height;
    for (size_t i = 0; i < count; ++i) {
        const uint32_t value = pixelsOut[i];
        const unsigned int alpha = value >> 24;
        if (alpha == 255) continue;
        const unsigned int inverse = 255 - alpha;
        uint32_t result = 0xff000000u;
        for (int shift = 0; shift < 24; shift += 8) {
            const unsigned int background = shift == 0 ? GetBValue(kImageBackground) :
                (shift == 8 ? GetGValue(kImageBackground) : GetRValue(kImageBackground));
            const unsigned int channel = ((value >> shift) & 0xffu) + (background * inverse + 127) / 255;
            result |= std::min(255u, channel) << shift;
        }
        pixelsOut[i] = result;
    }
    image->width = static_cast<int>(width);
    image->height = static_cast<int>(height);
    while (std::max(image->levels.back().width, image->levels.back().height) > kMinimumLevelEdge) {
        if (!AddHalfLevel(*image)) break;
    }
    return image;
}

ImagePreview::~ImagePreview() {
    if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_);
}

bool ImagePreview::Create(HWND parent, HINSTANCE instance, int controlId) {
    WNDCLASSW windowClass{};
    if (!GetClassInfoW(instance, kImagePreviewClass, &windowClass)) {
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = nullptr;
        windowClass.lpszClassName = kImagePreviewClass;
        windowClass.style = CS_DBLCLKS;
        if (!RegisterClassW(&windowClass)) return false;
    }
    hwnd_ = CreateWindowExW(0, kImagePreviewClass, L"", WS_CHILD | WS_HSCROLL | WS_VSCROLL | WS_TABSTOP,
                            0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)), instance, this);
    return hwnd_ != nullptr;
}

void ImagePreview::SetImage(std::shared_ptr<const DecodedImage> image, std::wstring message) {
    image_ = std::move(image);
    message_ = std::move(message);
    fit_ = true;
    zoom_ = FitZoom();
    scrollX_ = scrollY_ = 0;
    UpdateScrollbars();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void ImagePreview::Clear() { SetImage(nullptr, L""); }

void ImagePreview::SetVisible(bool visible) {
    if (hwnd_) ShowWindow(hwnd_, visible ? SW_SHOW : SW_HIDE);
}

void ImagePreview::SetBounds(int x, int y, int width, int height) {
    if (!hwnd_) return;
    MoveWindow(hwnd_, x, y, std::max(0, width), std::max(0, height), TRUE);
}

double ImagePreview::FitZoom() const {
    if (!image_ || !hwnd_) return 1.0;
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int margin = reader_next::Px(hwnd_, 16);
    const double width = std::max(1L, client.right - client.left - margin * 2);
    const double height = std::max(1L, client.bottom - client.top - margin * 2);
    return std::clamp(std::min({1.0, width / image_->width, height / image_->height}), kMinimumZoom, 1.0);
}

double ImagePreview::Zoom() const noexcept { return zoom_; }

SIZE ImagePreview::ContentSize() const {
    if (!image_) return {0, 0};
    return {static_cast<LONG>(std::lround(image_->width * zoom_)), static_cast<LONG>(std::lround(image_->height * zoom_))};
}

RECT ImagePreview::ImageRect() const {
    RECT client{};
    if (hwnd_) GetClientRect(hwnd_, &client);
    const SIZE content = ContentSize();
    const LONG left = content.cx < client.right ? (client.right - content.cx) / 2 : -scrollX_;
    const LONG top = content.cy < client.bottom ? (client.bottom - content.cy) / 2 : -scrollY_;
    return {left, top, left + content.cx, top + content.cy};
}

void ImagePreview::UpdateScrollbars() {
    if (!hwnd_) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    const SIZE content = ContentSize();
    scrollX_ = std::clamp(scrollX_, 0, std::max(0, static_cast<int>(content.cx - client.right)));
    scrollY_ = std::clamp(scrollY_, 0, std::max(0, static_cast<int>(content.cy - client.bottom)));
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL};
    info.nMax = std::max(0, static_cast<int>(content.cx) - 1);
    info.nPage = static_cast<UINT>(std::max(0L, client.right));
    info.nPos = scrollX_;
    SetScrollInfo(hwnd_, SB_HORZ, &info, TRUE);
    info.nMax = std::max(0, static_cast<int>(content.cy) - 1);
    info.nPage = static_cast<UINT>(std::max(0L, client.bottom));
    info.nPos = scrollY_;
    SetScrollInfo(hwnd_, SB_VERT, &info, TRUE);
}

void ImagePreview::ScrollTo(int x, int y) {
    scrollX_ = x;
    scrollY_ = y;
    UpdateScrollbars();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ImagePreview::SetZoomAt(double zoom, POINT anchor) {
    if (!image_) return;
    zoom = std::clamp(zoom, kMinimumZoom, kMaximumZoom);
    const RECT before = ImageRect();
    const double imageX = (anchor.x - before.left) / zoom_;
    const double imageY = (anchor.y - before.top) / zoom_;
    zoom_ = zoom;
    fit_ = false;
    scrollX_ = static_cast<int>(std::lround(imageX * zoom_ - anchor.x));
    scrollY_ = static_cast<int>(std::lround(imageY * zoom_ - anchor.y));
    UpdateScrollbars();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ImagePreview::ZoomIn() {
    RECT client{}; GetClientRect(hwnd_, &client);
    SetZoomAt(zoom_ * kZoomStep, {client.right / 2, client.bottom / 2});
}

void ImagePreview::ZoomOut() {
    RECT client{}; GetClientRect(hwnd_, &client);
    SetZoomAt(zoom_ / kZoomStep, {client.right / 2, client.bottom / 2});
}

void ImagePreview::FitToWindow() {
    fit_ = true;
    zoom_ = FitZoom();
    scrollX_ = scrollY_ = 0;
    UpdateScrollbars();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

ImageViewState ImagePreview::GetViewState() const noexcept {
    return {zoom_, fit_, scrollX_, scrollY_, image_ != nullptr};
}

void ImagePreview::RestoreViewState(const ImageViewState& state) {
    if (!state.captured || state.fit) { FitToWindow(); return; }
    zoom_ = std::clamp(state.zoom, kMinimumZoom, kMaximumZoom);
    fit_ = false;
    ScrollTo(state.scrollX, state.scrollY);
}

void ImagePreview::Paint(HDC target) {
    RECT client{};
    GetClientRect(hwnd_, &client);
    if (client.right <= 0 || client.bottom <= 0) return;
    HDC dc = CreateCompatibleDC(target);
    HBITMAP buffer = dc ? CreateCompatibleBitmap(target, client.right, client.bottom) : nullptr;
    HDC paint = buffer ? dc : target;
    HGDIOBJ oldBuffer = buffer ? SelectObject(dc, buffer) : nullptr;
    HBRUSH background = CreateSolidBrush(reader_next::kPaper);
    FillRect(paint, &client, background);
    DeleteObject(background);
    if (image_ && !image_->levels.empty()) {
        const RECT rect = ImageRect();
        // Choose the smallest prepared level that is still at least the displayed size.
        size_t index = 0;
        while (index + 1 < image_->levels.size() &&
               image_->levels[index + 1].width >= rect.right - rect.left &&
               image_->levels[index + 1].height >= rect.bottom - rect.top) ++index;
        const auto& level = image_->levels[index];
        RECT visible{};
        if (IntersectRect(&visible, &rect, &client)) {
            const double scaleX = static_cast<double>(level.width) / std::max(1L, rect.right - rect.left);
            const double scaleY = static_cast<double>(level.height) / std::max(1L, rect.bottom - rect.top);
            const int sourceLeft = std::clamp(static_cast<int>(std::floor((visible.left - rect.left) * scaleX)), 0, level.width - 1);
            const int sourceTop = std::clamp(static_cast<int>(std::floor((visible.top - rect.top) * scaleY)), 0, level.height - 1);
            const int sourceRight = std::clamp(static_cast<int>(std::ceil((visible.right - rect.left) * scaleX)), sourceLeft + 1, level.width);
            const int sourceBottom = std::clamp(static_cast<int>(std::ceil((visible.bottom - rect.top) * scaleY)), sourceTop + 1, level.height);
            // Map the whole source cells back so partial pixels do not shift while panning.
            const int destLeft = rect.left + static_cast<int>(std::lround(sourceLeft / scaleX));
            const int destTop = rect.top + static_cast<int>(std::lround(sourceTop / scaleY));
            const int destRight = rect.left + static_cast<int>(std::lround(sourceRight / scaleX));
            const int destBottom = rect.top + static_cast<int>(std::lround(sourceBottom / scaleY));
            HDC source = CreateCompatibleDC(target);
            if (source) {
                HGDIOBJ old = SelectObject(source, level.bitmap);
                const bool shrinking = scaleX > 1.0 || scaleY > 1.0;
                SetStretchBltMode(paint, shrinking ? HALFTONE : COLORONCOLOR);
                SetBrushOrgEx(paint, 0, 0, nullptr);
                StretchBlt(paint, destLeft, destTop, destRight - destLeft, destBottom - destTop,
                           source, sourceLeft, sourceTop, sourceRight - sourceLeft, sourceBottom - sourceTop, SRCCOPY);
                SelectObject(source, old);
                DeleteDC(source);
            }
        }
    } else if (!message_.empty()) {
        RECT text = client;
        InflateRect(&text, -reader_next::Px(hwnd_, 24), -reader_next::Px(hwnd_, 24));
        SetBkMode(paint, TRANSPARENT);
        SetTextColor(paint, reader_next::kMuted);
        HGDIOBJ oldFont = SelectObject(paint, reinterpret_cast<HFONT>(SendMessageW(GetParent(hwnd_), WM_GETFONT, 0, 0)));
        DrawTextW(paint, message_.c_str(), static_cast<int>(message_.size()), &text, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(paint, oldFont);
    }
    if (buffer) {
        BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBuffer);
        DeleteObject(buffer);
    }
    if (dc) DeleteDC(dc);
}

LRESULT CALLBACK ImagePreview::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* self = static_cast<ImagePreview*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    auto* self = reinterpret_cast<ImagePreview*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);
    if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->Handle(message, wParam, lParam);
}

LRESULT ImagePreview::Handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd_, &ps);
            Paint(dc);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_SIZE:
            if (fit_) zoom_ = FitZoom();
            UpdateScrollbars();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_HSCROLL:
        case WM_VSCROLL: {
            const int bar = message == WM_HSCROLL ? SB_HORZ : SB_VERT;
            SCROLLINFO info{sizeof(info), SIF_ALL};
            GetScrollInfo(hwnd_, bar, &info);
            int position = info.nPos;
            const int line = reader_next::Px(hwnd_, 48);
            switch (LOWORD(wParam)) {
                case SB_LINEUP: position -= line; break;
                case SB_LINEDOWN: position += line; break;
                case SB_PAGEUP: position -= static_cast<int>(info.nPage); break;
                case SB_PAGEDOWN: position += static_cast<int>(info.nPage); break;
                case SB_THUMBTRACK: case SB_THUMBPOSITION: position = info.nTrackPos; break;
                case SB_TOP: position = 0; break;
                case SB_BOTTOM: position = info.nMax; break;
                default: break;
            }
            if (bar == SB_HORZ) ScrollTo(position, scrollY_);
            else ScrollTo(scrollX_, position);
            return 0;
        }
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (message == WM_MOUSEWHEEL && (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL)) {
                POINT anchor{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ScreenToClient(hwnd_, &anchor);
                SetZoomAt(delta > 0 ? zoom_ * kZoomStep : zoom_ / kZoomStep, anchor);
                return 0;
            }
            const int step = MulDiv(delta, reader_next::Px(hwnd_, 120), WHEEL_DELTA);
            const bool horizontal = message == WM_MOUSEHWHEEL || (GET_KEYSTATE_WPARAM(wParam) & MK_SHIFT);
            if (horizontal) ScrollTo(scrollX_ + (message == WM_MOUSEHWHEEL ? step : -step), scrollY_);
            else ScrollTo(scrollX_, scrollY_ - step);
            return 0;
        }
        case WM_LBUTTONDOWN:
            SetFocus(hwnd_);
            dragging_ = true;
            dragOrigin_ = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            dragScrollX_ = scrollX_;
            dragScrollY_ = scrollY_;
            SetCapture(hwnd_);
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            return 0;
        case WM_MOUSEMOVE:
            if (dragging_) {
                ScrollTo(dragScrollX_ - (GET_X_LPARAM(lParam) - dragOrigin_.x),
                         dragScrollY_ - (GET_Y_LPARAM(lParam) - dragOrigin_.y));
            }
            return 0;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if (dragging_) {
                dragging_ = false;
                if (message == WM_LBUTTONUP) ReleaseCapture();
            }
            return 0;
        case WM_LBUTTONDBLCLK:
            if (fit_) {
                POINT anchor{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                SetZoomAt(1.0, anchor);
            } else {
                FitToWindow();
            }
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS;
        case WM_KEYDOWN: {
            RECT client{}; GetClientRect(hwnd_, &client);
            const int line = reader_next::Px(hwnd_, 48);
            switch (wParam) {
                case VK_LEFT: ScrollTo(scrollX_ - line, scrollY_); return 0;
                case VK_RIGHT: ScrollTo(scrollX_ + line, scrollY_); return 0;
                case VK_UP: ScrollTo(scrollX_, scrollY_ - line); return 0;
                case VK_DOWN: ScrollTo(scrollX_, scrollY_ + line); return 0;
                case VK_PRIOR: ScrollTo(scrollX_, scrollY_ - client.bottom); return 0;
                case VK_NEXT: ScrollTo(scrollX_, scrollY_ + client.bottom); return 0;
                case VK_HOME: ScrollTo(0, 0); return 0;
                case VK_END: ScrollTo(scrollX_, std::numeric_limits<int>::max() / 2); return 0;
                default: break;
            }
            break;
        }
        case WM_CHAR:
            return 0; // Never forward typed characters to a default beep path.
        default:
            break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

} // namespace readonly_viewer
