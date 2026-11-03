#pragma once

#include <filesystem>
#include <string>

#include "types.h"

namespace clrop {

class ItemLoadSink {
public:
    virtual ~ItemLoadSink() = default;
    virtual bool OnClropItem(int page, clrop::Item&& item, std::wstring& err) = 0;
    // Called after full validation, even when metadata follows the pages.
    // Unspecified marker modes use Legacy in document v1 and Multiply in v2.
    virtual bool OnClropDocumentVersion(int, std::wstring&) { return true; }
};

enum class LoadFileFailureKind {
    None,
    Read,
    Parse,
};

// Document output is replaced only after the entire input parses successfully.
bool ParseClropFromJson(const std::string& json, clrop::Document& out, std::wstring& err);
// Sink callbacks run while parsing. A parse error or sink rejection may leave
// earlier items in the sink; callers must stage them and publish only on true.
// outPdfId is replaced only after the entire input parses successfully.
bool ParseClropFromJsonToSink(const std::string& json,
                              clrop::PdfId* outPdfId,
                              clrop::ItemLoadSink& sink,
                              std::wstring& err);
bool LoadClropFile(const std::wstring& path,
                   clrop::Document& out,
                   std::wstring& err,
                   LoadFileFailureKind* failureKind = nullptr);
// The same partial-sink contract applies after the file has been read.
bool LoadClropFileToSink(const std::wstring& path,
                         clrop::PdfId* outPdfId,
                         clrop::ItemLoadSink& sink,
                         std::wstring& err,
                         LoadFileFailureKind* failureKind = nullptr);
bool SerializeClrop(const clrop::Document& doc, std::string& out, std::wstring& err);
bool SaveClropFile(const std::wstring& path, const clrop::Document& doc, std::wstring& err);
bool SaveClropFile(const std::wstring& path,
                   const clrop::Document& doc,
                   std::wstring& err,
                   const std::filesystem::path& preferredTempDir,
                   const std::filesystem::path& quarantineDir);

} // namespace clrop
