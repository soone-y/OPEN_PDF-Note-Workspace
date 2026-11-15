#pragma once
#include "clrop/pair_discovery.h"
#include <windows.h>
#include <functional>

using PdfPairPreview = std::function<bool(HWND, const std::filesystem::path&, const std::filesystem::path&)>;
void ShowPdfPairFinder(HWND owner, clrop::discovery::Mode mode,
    const std::filesystem::path& initialDirectory, PdfPairPreview preview);
