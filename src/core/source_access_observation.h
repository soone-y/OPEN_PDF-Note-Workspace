#pragma once

#include <windows.h>
#include <winioctl.h>

// Advisory observation on the SAME handle that denies destructive sharing.
// The source owner retains that handle throughout acknowledgments. This class
// owns only its asynchronous request/event; failure never releases protection.
// Its owner serializes Poll/Stop and closes the borrowed handle only after Stop.
class SourceAccessObservation {
public:
    SourceAccessObservation() = default;
    ~SourceAccessObservation() { Stop(); }
    SourceAccessObservation(const SourceAccessObservation&) = delete;
    SourceAccessObservation& operator=(const SourceAccessObservation&) = delete;

    void Bind(HANDLE handle) noexcept {
        Stop();
        handle_ = handle;
        nextAttempt_ = 0;
        disabled_ = false;
    }
    void Stop() noexcept {
        if (pending_ != Pending::None && handle_ != INVALID_HANDLE_VALUE) {
            (void)CancelIoEx(handle_, &request_);
            DWORD ignored = 0;
            (void)GetOverlappedResult(handle_, &request_, &ignored, TRUE);
        }
        if (event_) CloseHandle(event_);
        event_ = nullptr;
        handle_ = INVALID_HANDLE_VALUE;
        pending_ = Pending::None;
        needsAck_ = false;
        noticeAfterAck_ = false;
    }
    [[nodiscard]] bool active() const noexcept {
        return handle_ != INVALID_HANDLE_VALUE && !disabled_;
    }
    [[nodiscard]] bool watching() const noexcept { return pending_ == Pending::Request; }

    // A true result means Windows reported a conflicting open, not proof of a
    // particular user's intent. Ordinary compatible reads produce no notice.
    [[nodiscard]] bool Poll(ULONGLONG now) noexcept {
        if (!active()) return false;
        if (pending_ != Pending::None) {
            if (WaitForSingleObject(event_, 0) != WAIT_OBJECT_0) return false;
            const Pending completed = pending_;
            pending_ = Pending::None;
            DWORD ignored = 0;
            const bool ok = GetOverlappedResult(handle_, &request_, &ignored, FALSE) != FALSE;
            if (completed == Pending::Ack) {
                if (!ok) { needsAck_ = true; nextAttempt_ = now + 100; return false; }
                needsAck_ = false;
                nextAttempt_ = now + kRearmDelayMs;
                const bool notice = noticeAfterAck_;
                noticeAfterAck_ = false;
                return notice;
            }
            if (!ok) { nextAttempt_ = now + kRetryDelayMs; return false; }
            const bool conflict = (output_.Flags & REQUEST_OPLOCK_OUTPUT_FLAG_MODES_PROVIDED) != 0;
            if (output_.Flags & REQUEST_OPLOCK_OUTPUT_FLAG_ACK_REQUIRED) {
                noticeAfterAck_ = conflict;
                needsAck_ = true;
                nextAttempt_ = now;
            } else {
                nextAttempt_ = now + kRearmDelayMs;
                return conflict;
            }
        }
        if (now < nextAttempt_) return false;
        if (needsAck_) {
            if (!Submit(Pending::Ack, now)) return false;
            needsAck_ = false;
            nextAttempt_ = now + kRearmDelayMs;
            const bool notice = noticeAfterAck_;
            noticeAfterAck_ = false;
            return notice;
        }
        (void)Submit(Pending::Request, now);
        return false;
    }

private:
    enum class Pending { None, Request, Ack };
    static constexpr ULONGLONG kRearmDelayMs = 200;
    static constexpr ULONGLONG kRetryDelayMs = 1000;
    HANDLE handle_ = INVALID_HANDLE_VALUE; // borrowed from the source owner
    HANDLE event_ = nullptr;
    OVERLAPPED request_{};
    REQUEST_OPLOCK_INPUT_BUFFER input_{};
    REQUEST_OPLOCK_OUTPUT_BUFFER output_{};
    Pending pending_ = Pending::None;
    bool disabled_ = false;
    bool needsAck_ = false;
    bool noticeAfterAck_ = false;
    ULONGLONG nextAttempt_ = 0;

    // Returns true only for a synchronously completed acknowledgment. The
    // buffers remain alive until an asynchronous completion or cancellation.
    [[nodiscard]] bool Submit(Pending kind, ULONGLONG now) noexcept {
        if (!event_) event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event_) { nextAttempt_ = now + kRetryDelayMs; return false; }
        ResetEvent(event_);
        request_ = {}; request_.hEvent = event_;
        input_ = {};
        input_.StructureVersion = REQUEST_OPLOCK_CURRENT_VERSION;
        input_.StructureLength = sizeof(input_);
        input_.RequestedOplockLevel = kind == Pending::Request
            ? OPLOCK_LEVEL_CACHE_READ | OPLOCK_LEVEL_CACHE_HANDLE : 0;
        input_.Flags = kind == Pending::Request
            ? REQUEST_OPLOCK_INPUT_FLAG_REQUEST : REQUEST_OPLOCK_INPUT_FLAG_ACK;
        output_ = {};
        DWORD ignored = 0;
        if (DeviceIoControl(handle_, FSCTL_REQUEST_OPLOCK, &input_, sizeof(input_),
                &output_, sizeof(output_), &ignored, &request_)) {
            if (kind == Pending::Ack) return true;
            // A break can finish before DeviceIoControl returns. Process that
            // completion through the same path, including its acknowledgment.
            pending_ = Pending::Request;
            SetEvent(event_);
            return false;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            pending_ = kind;
        } else {
            nextAttempt_ = now + kRetryDelayMs;
            if (kind == Pending::Request && (error == ERROR_INVALID_FUNCTION ||
                error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_PARAMETER)) disabled_ = true;
        }
        return false;
    }
};
