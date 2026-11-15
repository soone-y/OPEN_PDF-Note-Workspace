#pragma once

#include <filesystem>
#include <string>

#include "pdf_identity.h"

namespace clrop {

// Calculates a complete SHA-256 for a local regular file.  This is deliberately
// uncached: callers use it for persistent transaction preconditions, where a
// path/mtime cache would turn an external change into an unsafe false match.
bool ComputeFileSha256(const std::filesystem::path& path, std::string* outSha256);

// PDFファイルから size + page_count + page_sizes_pt を計算する。path は参照情報として格納。
// 取得できない値がある場合は、利用できた範囲まで返す。
clrop::PdfId ComputePdfFastId(const std::wstring& pdfPath);

// PDFファイルから size + page_count + page_sizes_pt + sha256 を計算する。
// sha256 は path + size + mtime ベースのプロセス内キャッシュで再利用する。
clrop::PdfId ComputePdfId(const std::wstring& pdfPath);

// Start a new protected open with fresh identity, even if an external change
// made while closed preserved size and mtime. Reuse remains within that open.
void InvalidatePdfIdCache(const std::wstring& pdfPath);

} // namespace clrop
