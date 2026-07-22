// file: core/annot_commands.h
#pragma once

#include "core/app_core.h"

#include <string>
#include <string_view>
#include <vector>

enum class AnnotCommandKind { Add, Remove, Update, Reorder, ClearAll };

struct AnnotCommand {
    AnnotCommandKind kind = AnnotCommandKind::Add;
    int beforeIndex = -1;
    int afterIndex = -1;
    Annotation before;
    Annotation after;
    std::vector<Annotation> snapshot;
    std::vector<Annotation> afterSnapshot;
};

bool SerializeAnnotCommandsJson(const std::vector<AnnotCommand>& cmds, std::string* outJson);
bool DeserializeAnnotCommandsJson(std::string_view json,
                                  std::vector<AnnotCommand>* outCmds,
                                  std::wstring* outErr = nullptr);
void ApplyAnnotCommandToList(std::vector<Annotation>* annots, const AnnotCommand& cmd);
bool CollectPendingAnnotStageCommands(const std::wstring& pdfPath, std::vector<AnnotCommand>* outCmds);
void ClearPendingAnnotStageCommands(const std::wstring& pdfPath);
void InvalidatePendingAnnotStageCommands(const std::wstring& pdfPath);
void ResetPendingAnnotStageCommands();
bool TryBuildCurrentAnnotationSaveSnapshot(std::vector<Annotation>* outAnnotations);
