#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "clrop/json.h"
#include "fpdfview.h"
#include "note/note_model.h"
#include "note/note_parser.h"

#define PDF_NOTE_ANNOT_HISTORY_PARSER_ONLY
#include "pdf_view/annotation_store.cppinc"
#undef PDF_NOTE_ANNOT_HISTORY_PARSER_ONLY

std::wstring UTF8ToWide(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                            static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), result.data(), length) != length) {
        return {};
    }
    return result;
}

namespace {

constexpr uint32_t kMutationRounds = 256;

uint32_t NextRandom(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

template <typename Char>
std::basic_string<Char> Mutate(std::basic_string_view<Char> seed, uint32_t round) {
    std::basic_string<Char> value(seed);
    uint32_t state = 0x9e3779b9u ^ (round * 0x85ebca6bu);
    const unsigned operations = 1u + (NextRandom(state) % 4u);
    for (unsigned operation = 0; operation < operations; ++operation) {
        const unsigned kind = NextRandom(state) % 5u;
        if (kind == 0 && !value.empty()) {
            const size_t pos = NextRandom(state) % value.size();
            const uint32_t mask = sizeof(Char) == 1 ? 0xffu : 0xffffu;
            value[pos] = static_cast<Char>(NextRandom(state) & mask);
        } else if (kind == 1 && !value.empty()) {
            value.erase(NextRandom(state) % value.size(), 1);
        } else if (kind == 2 && value.size() < seed.size() + 64) {
            const size_t pos = value.empty() ? 0 : NextRandom(state) % (value.size() + 1);
            value.insert(value.begin() + static_cast<std::ptrdiff_t>(pos),
                         static_cast<Char>(NextRandom(state) & (sizeof(Char) == 1 ? 0xffu : 0xffffu)));
        } else if (kind == 3 && !value.empty()) {
            value.resize(NextRandom(state) % (value.size() + 1));
        } else if (kind == 4 && !value.empty() && value.size() < seed.size() + 64) {
            const size_t pos = NextRandom(state) % value.size();
            const size_t count = std::min<size_t>(8, value.size() - pos);
            value.append(value.substr(pos, count));
        }
    }
    return value;
}

std::vector<uint8_t> ReadBytes(const wchar_t* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
}

bool ExercisePdf(std::basic_string_view<uint8_t> bytes) {
    if (bytes.empty()) return false;
    FPDF_DOCUMENT document = FPDF_LoadMemDocument64(bytes.data(), bytes.size(), nullptr);
    if (!document) return false;
    const int pageCount = FPDF_GetPageCount(document);
    if (pageCount > 0 && pageCount < 10000) {
        FPDF_PAGE page = FPDF_LoadPage(document, 0);
        if (page) FPDF_ClosePage(page);
    }
    FPDF_CloseDocument(document);
    return true;
}

bool ExerciseImage(IWICImagingFactory* factory, std::basic_string_view<uint8_t> bytes) {
    if (!factory || bytes.empty() || bytes.size() > std::numeric_limits<UINT>::max()) return false;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (!memory) return false;
    void* destination = GlobalLock(memory);
    if (!destination) {
        GlobalFree(memory);
        return false;
    }
    std::copy(bytes.begin(), bytes.end(), static_cast<uint8_t*>(destination));
    GlobalUnlock(memory);

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &stream))) {
        GlobalFree(memory);
        return false;
    }
    IWICBitmapDecoder* decoder = nullptr;
    const HRESULT decodeResult = factory->CreateDecoderFromStream(
        stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    stream->Release();
    if (FAILED(decodeResult) || !decoder) return false;

    IWICBitmapFrameDecode* frame = nullptr;
    const HRESULT frameResult = decoder->GetFrame(0, &frame);
    decoder->Release();
    if (FAILED(frameResult) || !frame) return false;
    UINT width = 0;
    UINT height = 0;
    const bool validSize = SUCCEEDED(frame->GetSize(&width, &height)) && width > 0 && height > 0 &&
                           width <= 4096 && height <= 4096;
    bool decoded = false;
    if (validSize) {
        IWICFormatConverter* converter = nullptr;
        if (SUCCEEDED(factory->CreateFormatConverter(&converter)) && converter) {
            if (SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                                                WICBitmapDitherTypeNone, nullptr, 0.0,
                                                WICBitmapPaletteTypeCustom))) {
                const uint64_t stride64 = static_cast<uint64_t>(width) * 4u;
                const uint64_t bytes64 = stride64 * height;
                if (stride64 <= std::numeric_limits<UINT>::max() &&
                    bytes64 <= 64u * 1024u * 1024u) {
                    std::vector<uint8_t> pixels(static_cast<size_t>(bytes64));
                    decoded = SUCCEEDED(converter->CopyPixels(
                        nullptr, static_cast<UINT>(stride64), static_cast<UINT>(bytes64), pixels.data()));
                }
            }
            converter->Release();
        }
    }
    frame->Release();
    return decoded;
}

std::vector<uint8_t> MakeOnePixelBmp() {
    return {
        'B','M', 58,0,0,0, 0,0,0,0, 54,0,0,0,
        40,0,0,0, 1,0,0,0, 1,0,0,0, 1,0, 24,0,
        0,0,0,0, 4,0,0,0, 0,0,0,0, 0,0,0,0,
        0,0,0,0, 0,0,0,0, 0x20,0x40,0x80,0
    };
}

} // namespace

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    int failures = 0;

    const std::string clropSeed =
        R"({"version":1,"pdf_id":{"path":"sample.pdf","size":42,"page_count":1},"pages":[{"page":0,"items":[{"type":"text","id":"a","content":"hello","bbox":[1,2,3,4]}]}]})";
    for (uint32_t round = 0; round < kMutationRounds; ++round) {
        const std::string input = Mutate<char>(clropSeed, round);
        clrop::Document document;
        std::wstring error;
        (void)clrop::ParseClropFromJson(input, document, error);
    }
    {
        clrop::Document document;
        std::wstring error;
        if (!clrop::ParseClropFromJson(clropSeed, document, error)) ++failures;
    }

    const std::wstring noteSeed =
        L"# Heading\n\n- [x] item\n\nText **bold** $x^2$ <link=dest>jump</>\n";
    for (uint32_t round = 0; round < kMutationRounds; ++round) {
        note::NoteMetadata metadata;
        metadata.file_name = L"fuzz.md";
        note::NoteTextModel model = note::MakeNoteTextModel(
            std::move(metadata), Mutate<wchar_t>(noteSeed, round), round + 1);
        (void)note::ParseNoteDocument(model);
    }

    const std::string annotationHistorySeed =
        R"({"version":1,"undo":[{"type":"add","page":0,"annotation":{"type":"line","id":"a"}}],"redo":[]})";
    for (uint32_t round = 0; round < kMutationRounds; ++round) {
        const std::string input = Mutate<char>(annotationHistorySeed, round);
        AnnotHistoryJsonValue value;
        AnnotHistoryJsonParser parser(input);
        (void)parser.Parse(value);
    }
    {
        AnnotHistoryJsonValue value;
        AnnotHistoryJsonParser parser(annotationHistorySeed);
        if (!parser.Parse(value)) ++failures;
    }

    FPDF_InitLibrary();
    const std::vector<uint8_t> pdfSeed = ReadBytes(L"tests\\fixtures\\ui_automation_session\\sample.pdf");
    const std::basic_string_view<uint8_t> pdfSeedView(pdfSeed.data(), pdfSeed.size());
    if (pdfSeed.empty() || !ExercisePdf(pdfSeedView)) {
        ++failures;
    } else {
        for (uint32_t round = 0; round < kMutationRounds; ++round) {
            const std::basic_string<uint8_t> input = Mutate<uint8_t>(pdfSeedView, round);
            (void)ExercisePdf(input);
        }
    }
    FPDF_DestroyLibrary();

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) {
        ++failures;
    } else {
        const std::vector<uint8_t> imageSeed = MakeOnePixelBmp();
        const std::basic_string_view<uint8_t> seedView(imageSeed.data(), imageSeed.size());
        if (!ExerciseImage(factory, seedView)) ++failures;
        for (uint32_t round = 0; round < kMutationRounds; ++round) {
            const std::basic_string<uint8_t> input = Mutate<uint8_t>(seedView, round);
            (void)ExerciseImage(factory, input);
        }
        factory->Release();
    }
    if (SUCCEEDED(comResult)) CoUninitialize();

    if (failures != 0) {
        std::cerr << "Input fuzz regression tests failed: " << failures << "\n";
        return 1;
    }
    std::cout << "Input fuzz regression tests passed (CLROP, annotation history, note, PDFium, WIC; "
              << kMutationRounds << " deterministic mutations each).\n";
    return 0;
}
