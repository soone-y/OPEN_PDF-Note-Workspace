#include "readonly_viewer/reader_math.h"

#include <objidl.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdint>
#include <unordered_map>

#include "math/math_render.h"
#include "note/note_model.h"
#include "note/note_parser.h"
#include "readonly_viewer/reader_chrome.h"

namespace readonly_viewer {
namespace {

constexpr size_t kMaximumFormulaSpans = 4096;
constexpr size_t kMaximumDistinctFormulas = 1024;
// Pixels of the supersampled drawing surface; beyond it the picture quality is reduced.
constexpr unsigned long long kMaximumRenderPixels = 40ull * 1024ull * 1024ull;
constexpr int kMaximumPictureEdge = 8192;

template <typename T>
class ComPtr final {
public:
    ComPtr() = default;
    ~ComPtr() { if (value_) value_->Release(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T** Put() { if (value_) { value_->Release(); value_ = nullptr; } return &value_; }
    T* Get() const { return value_; }
    T* operator->() const { return value_; }
private:
    T* value_ = nullptr;
};

class DibSurface final {
public:
    DibSurface(HDC reference, int width, int height) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        dc_ = CreateCompatibleDC(reference);
        void* bits = nullptr;
        bitmap_ = dc_ ? CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
        bits_ = static_cast<uint32_t*>(bits);
        if (bitmap_) old_ = SelectObject(dc_, bitmap_);
    }
    ~DibSurface() {
        if (dc_ && old_) SelectObject(dc_, old_);
        if (bitmap_) DeleteObject(bitmap_);
        if (dc_) DeleteDC(dc_);
    }
    DibSurface(const DibSurface&) = delete;
    DibSurface& operator=(const DibSurface&) = delete;
    [[nodiscard]] bool Valid() const { return dc_ && bitmap_ && bits_; }
    [[nodiscard]] HDC Dc() const { return dc_; }
    [[nodiscard]] uint32_t* Bits() const { return bits_; }
private:
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ old_ = nullptr;
    uint32_t* bits_ = nullptr;
};

[[nodiscard]] uint32_t Bgrx(COLORREF colour) {
    return 0xff000000u | (GetRValue(colour) << 16) | (GetGValue(colour) << 8) | GetBValue(colour);
}

[[nodiscard]] bool EncodePng(const std::vector<uint32_t>& pixels, int width, int height, std::vector<unsigned char>* png) {
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IWICImagingFactory, reinterpret_cast<void**>(factory.Put())))) return false;
    ComPtr<IWICBitmap> bitmap;
    const UINT stride = static_cast<UINT>(width) * 4;
    if (FAILED(factory->CreateBitmapFromMemory(static_cast<UINT>(width), static_cast<UINT>(height), GUID_WICPixelFormat32bppBGR,
            stride, stride * static_cast<UINT>(height),
            reinterpret_cast<BYTE*>(const_cast<uint32_t*>(pixels.data())), bitmap.Put()))) return false;
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.Put()))) return false;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.Put())) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(frame.Put(), nullptr)) ||
        FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height))) ||
        FAILED(frame->WriteSource(bitmap.Get(), nullptr)) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) return false;
    HGLOBAL global = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.Get(), &global)) || !global) return false;
    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart == 0 ||
        stat.cbSize.QuadPart > 64ull * 1024ull * 1024ull) return false;
    const void* data = GlobalLock(global);
    if (!data) return false;
    const auto* bytes = static_cast<const unsigned char*>(data);
    png->assign(bytes, bytes + static_cast<size_t>(stat.cbSize.QuadPart));
    GlobalUnlock(global);
    return true;
}

} // namespace

std::shared_ptr<ReaderMathSet> CollectReaderMath(const std::wstring& fileName, const std::wstring& text,
                                                 ReaderMathSource source) {
    auto set = std::make_shared<ReaderMathSet>();
    if (text.empty()) return set;
    try {
        const note::NoteTextModel model = note::MakeNoteTextModel({fileName, L""}, text, 1);
        const note::NoteDocument document = source == ReaderMathSource::TeX
            ? note::ParseTeXMathDocument(model) : note::ParseNoteDocument(model);
        if (source == ReaderMathSource::Note && note::NoteDocumentHasFatalParseFailure(document)) return set;
        std::vector<const note::MathSpan*> ordered;
        for (const auto& span : document.math_spans) ordered.push_back(&span);
        std::stable_sort(ordered.begin(), ordered.end(), [](const note::MathSpan* a, const note::MathSpan* b) {
            return a->span.start.value < b->span.start.value;
        });
        std::unordered_map<std::wstring, std::shared_ptr<const mathrender::Formula>> parsed;
        size_t previousEnd = 0;
        for (const note::MathSpan* span : ordered) {
            if (set->spans.size() >= kMaximumFormulaSpans) break;
            const size_t start = span->span.start.value;
            const size_t end = span->span.end.value;
            if (start < previousEnd || end <= start || end > text.size() || span->normalized_tex.empty()) continue;
            std::shared_ptr<const mathrender::Formula> formula;
            if (const auto found = parsed.find(span->normalized_tex); found != parsed.end()) {
                formula = found->second;
            } else if (parsed.size() < kMaximumDistinctFormulas) {
                formula = std::shared_ptr<const mathrender::Formula>(mathrender::Parse(span->normalized_tex));
                parsed.emplace(span->normalized_tex, formula);
            }
            if (!formula) continue; // Unparseable or over budget: the source stays readable text.
            set->spans.push_back({start, end, span->kind == note::MathKind::Block, span->normalized_tex});
            set->formulas.push_back(std::move(formula));
            previousEnd = end;
        }
    } catch (...) {
        set->spans.clear();
        set->formulas.clear();
    }
    return set;
}

int ReaderMathDescentTwips(const ReaderMathSet& set, const ReaderMath& math, int dpi) {
    const auto key = std::make_tuple(math.span, math.display, ReaderHeadingFontHalfPoints(math.headingLevel), dpi);
    const auto found = set.descents.find(key);
    return found == set.descents.end() ? 0 : found->second;
}

std::string BuildReaderMathPicture(ReaderMathSet& set, const ReaderMath& math, int dpi) {
    if (math.span >= set.formulas.size() || !set.formulas[math.span] || dpi <= 0) return {};
    const int halfPoints = ReaderHeadingFontHalfPoints(math.headingLevel);
    const auto key = std::make_tuple(math.span, math.display, halfPoints, dpi);
    if (const auto found = set.pictures.find(key); found != set.pictures.end()) return found->second;
    std::string& cached = set.pictures[key];
    try {
        const mathrender::Formula& formula = *set.formulas[math.span];
        const mathrender::RenderStyle style = math.display ? mathrender::RenderStyle::Display : mathrender::RenderStyle::Inline;
        const int fontPx = std::max(6, MulDiv(halfPoints, dpi, 144));
        HDC screen = GetDC(nullptr);
        if (!screen) return {};
        // Draw supersampled, then average blocks: antialiased glyphs that stay
        // sharp at 100% and are stored at twice the device resolution for zoom.
        std::string result;
        for (const int factor : {3, 1}) {
            const int outputScale = factor == 3 ? 2 : 1;
            const int renderScale = outputScale * factor;
            DibSurface probe(screen, 1, 1);
            if (!probe.Valid()) break;
            const mathrender::Layout layout = mathrender::Measure(formula, probe.Dc(), fontPx * renderScale, style);
            if (layout.width <= 0 || layout.height <= 0) break;
            const int width = (layout.width + factor - 1) / factor * factor;
            const int height = (layout.height + factor - 1) / factor * factor;
            if (static_cast<unsigned long long>(width) * height > kMaximumRenderPixels ||
                width / factor > kMaximumPictureEdge || height / factor > kMaximumPictureEdge) continue;
            DibSurface surface(screen, width, height);
            if (!surface.Valid()) continue;
            const uint32_t paper = Bgrx(reader_next::kPaper);
            std::fill(surface.Bits(), surface.Bits() + static_cast<size_t>(width) * height, paper);
            if (!mathrender::Draw(formula, layout, surface.Dc(), 0, 0, fontPx * renderScale, reader_next::kInk, style)) break;
            GdiFlush();
            const int outWidth = width / factor;
            const int outHeight = height / factor;
            std::vector<uint32_t> pixels(static_cast<size_t>(outWidth) * outHeight);
            const uint32_t* input = surface.Bits();
            for (int y = 0; y < outHeight; ++y) {
                for (int x = 0; x < outWidth; ++x) {
                    unsigned int sum[3] = {0, 0, 0};
                    for (int dy = 0; dy < factor; ++dy) {
                        const uint32_t* row = input + static_cast<size_t>(y * factor + dy) * width + static_cast<size_t>(x) * factor;
                        for (int dx = 0; dx < factor; ++dx) {
                            sum[0] += row[dx] & 0xffu;
                            sum[1] += (row[dx] >> 8) & 0xffu;
                            sum[2] += (row[dx] >> 16) & 0xffu;
                        }
                    }
                    const unsigned int count = static_cast<unsigned int>(factor * factor);
                    pixels[static_cast<size_t>(y) * outWidth + x] = 0xff000000u |
                        ((sum[2] + count / 2) / count << 16) | ((sum[1] + count / 2) / count << 8) | ((sum[0] + count / 2) / count);
                }
            }
            std::vector<unsigned char> png;
            if (!EncodePng(pixels, outWidth, outHeight, &png)) break;
            // RTF goal sizes are twips at device resolution; the stored bitmap is outputScale times larger.
            const int goalWidth = MulDiv(outWidth, 1440, dpi * outputScale);
            const int goalHeight = MulDiv(outHeight, 1440, dpi * outputScale);
            set.descents[key] = math.display ? 0 :
                MulDiv(std::max(0, layout.height - layout.baseline), 1440, dpi * renderScale);
            result.reserve(png.size() * 2 + 160);
            result += "{{\\pict\\pngblip\\picw" + std::to_string(outWidth) + "\\pich" + std::to_string(outHeight) +
                      "\\picwgoal" + std::to_string(goalWidth) + "\\pichgoal" + std::to_string(goalHeight) + "\n";
            static const char kHex[] = "0123456789abcdef";
            for (size_t i = 0; i < png.size(); ++i) {
                result += kHex[png[i] >> 4];
                result += kHex[png[i] & 15];
                if (i % 64 == 63) result += '\n';
            }
            result += "}}";
            break;
        }
        ReleaseDC(nullptr, screen);
        cached = std::move(result);
        return cached;
    } catch (...) {
        cached.clear();
        return {};
    }
}

} // namespace readonly_viewer
