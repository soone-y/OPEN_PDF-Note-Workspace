#include "search/search_process.h"
#include <shellapi.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <vector>
#include <stdexcept>

namespace search {
namespace {
constexpr uint32_t kMagic = 0x31534e50;
constexpr size_t kRequestLimit = 64 * 1024 * 1024;
constexpr size_t kMessageLimit = 1024 * 1024;
struct Header { uint32_t magic, requestBytes, messageBytes; };
char* Output(void* view) {
    return static_cast<char*>(view) + sizeof(Header) + static_cast<Header*>(view)->requestBytes;
}
void Close(HANDLE& handle) { if (handle) CloseHandle(handle); handle = nullptr; }
std::wstring Executable() {
    std::wstring value(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
    if (!length || length >= value.size()) return {};
    value.resize(length); return value;
}
}
Process::~Process() { Stop(); }
void Process::Stop() noexcept {
    if (cancel_) SetEvent(cancel_);
    // Only a read-only worker belongs to this job. No UI-thread join, disk
    // cleanup or waiting for PDFium occurs, including cancellation mid-read.
    if (job_) TerminateJobObject(job_, 1);
    Close(job_); Close(process_);
    if (view_) UnmapViewOfFile(view_);
    view_ = nullptr;
    requestBytes_ = 0;
    Close(mapping_); Close(ready_); Close(ack_); Close(cancel_); Close(resume_);
}
bool Process::Start(std::string_view request) {
    Stop();
    if (request.empty() || request.size() > kRequestLimit) return false;
    const auto executable = Executable();
    if (executable.empty()) return false;
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    const auto size = sizeof(Header) + request.size() + kMessageLimit;
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE,
                                  0, static_cast<DWORD>(size), nullptr);
    ready_ = CreateEventW(&security, FALSE, FALSE, nullptr);
    ack_ = CreateEventW(&security, FALSE, FALSE, nullptr);
    cancel_ = CreateEventW(&security, TRUE, FALSE, nullptr);
    resume_ = CreateEventW(&security, FALSE, FALSE, nullptr);
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
                                             JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.ProcessMemoryLimit = 512ull * 1024 * 1024;
    if (!mapping_ || !ready_ || !ack_ || !cancel_ || !resume_ || !job_ ||
        !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        Stop(); return false;
    }
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};
    cpu.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
    cpu.CpuRate = 1500; // At most 15% of total machine CPU, independently of priority.
    if (!SetInformationJobObject(job_, JobObjectCpuRateControlInformation, &cpu, sizeof(cpu))) {
        Stop(); return false;
    }
    view_ = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!view_) { Stop(); return false; }
    requestBytes_ = request.size();
    *static_cast<Header*>(view_) = {kMagic, static_cast<uint32_t>(request.size()), 0};
    std::memcpy(static_cast<char*>(view_) + sizeof(Header), request.data(), request.size());
    HANDLE inherited[] = {mapping_, ready_, ack_, cancel_, resume_};
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<unsigned char> storage(attributeBytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes)) { Stop(); return false; }
    const bool configured = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited, sizeof(inherited), nullptr, nullptr) != FALSE;
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION child{};
    auto command = L"\"" + executable + L"\" --search-worker";
    for (const auto handle : inherited)
        command += L" " + std::to_wstring(reinterpret_cast<uintptr_t>(handle));
    const auto directory = std::filesystem::path(executable).parent_path();
    const bool launched = configured && CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
        TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | IDLE_PRIORITY_CLASS | EXTENDED_STARTUPINFO_PRESENT,
        nullptr, directory.c_str(), &startup.StartupInfo, &child);
    DeleteProcThreadAttributeList(attributes);
    if (!launched) { Stop(); return false; }
    process_ = child.hProcess;
    const bool started = AssignProcessToJobObject(job_, process_) &&
                         ResumeThread(child.hThread) != static_cast<DWORD>(-1);
    CloseHandle(child.hThread);
    if (!started) { TerminateProcess(process_, 1); Stop(); return false; }
    return true;
}
Process::PollResult Process::Poll(std::string& message) {
    message.clear();
    if (!process_ || !view_) return PollResult::Failed;
    const auto ready = WaitForSingleObject(ready_, 0);
    if (ready == WAIT_OBJECT_0) {
        const auto header = *static_cast<const Header*>(view_);
        if (header.magic != kMagic || header.requestBytes != requestBytes_ ||
            !header.messageBytes || header.messageBytes > kMessageLimit)
            return PollResult::Failed;
        message.assign(static_cast<const char*>(view_) + sizeof(Header) + requestBytes_, header.messageBytes);
        return SetEvent(ack_) ? PollResult::Message : PollResult::Failed;
    }
    if (ready != WAIT_TIMEOUT) return PollResult::Failed;
    const auto exited = WaitForSingleObject(process_, 0);
    if (exited == WAIT_TIMEOUT) return PollResult::Pending;
    DWORD code = 1;
    if (exited != WAIT_OBJECT_0 || !GetExitCodeProcess(process_, &code) || code) return PollResult::Failed;
    return PollResult::Exited;
}
bool Process::Continue() { return resume_ && SetEvent(resume_); }
Channel::Channel(void* view, HANDLE ready, HANDLE ack, HANDLE cancel, HANDLE resume)
    : view_(view), ready_(ready), ack_(ack), cancel_(cancel), resume_(resume) {}
bool Channel::Canceled() const { return WaitForSingleObject(cancel_, 0) != WAIT_TIMEOUT; }
bool Channel::Send(std::string_view message) {
    if (Canceled() || message.empty() || message.size() > kMessageLimit) return false;
    std::memcpy(Output(view_), message.data(), message.size());
    static_cast<Header*>(view_)->messageBytes = static_cast<uint32_t>(message.size());
    if (!SetEvent(ready_)) return false;
    const HANDLE waits[] = {cancel_, ack_};
    return WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1;
}
bool Channel::WaitForContinue() {
    const HANDLE waits[] = {cancel_, resume_};
    return WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1;
}
bool Channel::Rest(DWORD milliseconds) const {
    return WaitForSingleObject(cancel_, milliseconds) == WAIT_TIMEOUT;
}
bool TryRunProcess(int* exitCode) {
    int count = 0;
    auto** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return false;
    const bool requested = count >= 2 && std::wstring(arguments[1]) == L"--search-worker";
    if (!requested) { LocalFree(arguments); return false; }
    *exitCode = 1;
    try {
        if (count != 7) throw std::runtime_error("search handles");
        HANDLE handles[5]{};
        for (int i = 0; i < 5; ++i) {
            const std::wstring value(arguments[i + 2]);
            if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos)
                throw std::runtime_error("search handle");
            const auto number = std::stoull(value);
            if (!number || number > UINTPTR_MAX) throw std::runtime_error("search handle range");
            handles[i] = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(number));
        }
        void* view = MapViewOfFile(handles[0], FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (!view) throw std::runtime_error("search mapping");
        struct Unmap { void* value; ~Unmap() { UnmapViewOfFile(value); } } unmap{view};
        const auto header = *static_cast<Header*>(view);
        MEMORY_BASIC_INFORMATION information{};
        if (header.magic != kMagic || !header.requestBytes || header.requestBytes > kRequestLimit ||
            !VirtualQuery(view, &information, sizeof(information)) ||
            information.RegionSize < sizeof(Header) + header.requestBytes + kMessageLimit)
            throw std::runtime_error("search mapping size");
        // CPU, disk and memory priority are lowered together. Failure is not
        // silently accepted as an ordinary foreground search.
        if (!SetPriorityClass(GetCurrentProcess(), PROCESS_MODE_BACKGROUND_BEGIN))
            throw std::runtime_error("search background priority");
        Channel channel(view, handles[1], handles[2], handles[3], handles[4]);
        const std::string request(static_cast<char*>(view) + sizeof(Header), header.requestBytes);
        *exitCode = RunWorker(request, channel) ? 0 : 1;
    } catch (...) { *exitCode = 1; }
    LocalFree(arguments);
    return true;
}
}
