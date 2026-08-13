#pragma once

namespace main_close_policy {

enum class CloseRequestAction {
    IgnoreAlreadyExiting,
    WaitForActiveSave,
    BypassFinishedAutomation,
    RunExitFlow,
};

struct CloseRequestState {
    bool exit_in_progress = false;
    bool save_operation_in_progress = false;
    bool ui_automation_enabled = false;
    bool ui_automation_finished = false;
};

inline CloseRequestAction ResolveCloseRequestAction(const CloseRequestState& state) {
    if (state.exit_in_progress) return CloseRequestAction::IgnoreAlreadyExiting;
    if (state.save_operation_in_progress) return CloseRequestAction::WaitForActiveSave;
    if (state.ui_automation_enabled && state.ui_automation_finished) {
        return CloseRequestAction::BypassFinishedAutomation;
    }
    return CloseRequestAction::RunExitFlow;
}

struct ExitFlowCompletion {
    bool destroy_window = false;
    bool exit_in_progress = false;
};

inline ExitFlowCompletion ResolveExitFlowCompletion(bool exit_flow_succeeded) {
    return ExitFlowCompletion{exit_flow_succeeded, exit_flow_succeeded};
}

} // namespace main_close_policy