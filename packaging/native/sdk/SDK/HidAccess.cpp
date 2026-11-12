#include "HidAccess.h"
#include "../Protocol/HidAccessProtocol.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <iterator>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <unordered_set>

namespace HidAccess {
namespace {

static_assert(sizeof(wchar_t) == sizeof(std::uint16_t), "Windows UTF-16 is required");
static_assert(sizeof(HA_HEADER) == 8 && sizeof(HA_RULE) == 24, "Unexpected wire layout");
static_assert(sizeof(HA_STATUS) == 56 && sizeof(HA_DEVICE) == 1296, "Unexpected wire layout");
static_assert(sizeof(HA_EVENT) == 1064, "Unexpected wire layout");

[[noreturn]] void Fail(DWORD code, const char* stage) {
    throw std::system_error(static_cast<int>(code), std::system_category(), stage);
}

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept : value(handle) {}
    ~Handle() {
        if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

struct ServiceHandle {
    SC_HANDLE value = nullptr;
    explicit ServiceHandle(SC_HANDLE handle) noexcept : value(handle) {}
    ~ServiceHandle() { if (value) CloseServiceHandle(value); }
    ServiceHandle(const ServiceHandle&) = delete;
    ServiceHandle& operator=(const ServiceHandle&) = delete;
};

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
                                static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

template<std::size_t N> std::wstring ReadString(const std::uint16_t (&text)[N]) {
    const auto end = std::find(std::begin(text), std::end(text), std::uint16_t{});
    if (end == std::end(text)) Fail(ERROR_INVALID_DATA, "Driver returned an unterminated string");
    std::wstring result;
    result.reserve(static_cast<std::size_t>(end - std::begin(text)));
    for (auto i = std::begin(text); i != end; ++i) result.push_back(static_cast<wchar_t>(*i));
    return result;
}

bool IsNativePath(const std::wstring& path) {
    return path.size() > 8 && path.size() < HA_PATH_CHARS &&
           path.find(L'\0') == std::wstring::npos &&
           CompareStringOrdinal(path.c_str(), 8, L"\\Device\\", 8, TRUE) == CSTR_EQUAL;
}

std::wstring CanonicalExecutable(const std::wstring& path) {
    if (path.empty() || path.find(L'\0') != std::wstring::npos)
        Fail(ERROR_INVALID_PARAMETER, "An application path is empty or contains a null character");
    Handle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (file.value == INVALID_HANDLE_VALUE)
        Fail(GetLastError(), "Open the selected application file");
    FILE_STANDARD_INFO info{};
    if (!GetFileInformationByHandleEx(file.value, FileStandardInfo, &info, sizeof(info)))
        Fail(GetLastError(), "Read the selected application file attributes");
    if (info.Directory) Fail(ERROR_DIRECTORY, "Select an application file, not a directory");
    wchar_t native[HA_PATH_CHARS]{};
    const auto count = GetFinalPathNameByHandleW(file.value, native, HA_PATH_CHARS,
                                                FILE_NAME_NORMALIZED | VOLUME_NAME_NT);
    if (!count) Fail(GetLastError(), "Resolve the selected application's native path");
    if (count >= HA_PATH_CHARS)
        Fail(ERROR_FILENAME_EXCED_RANGE, "The application's native path exceeds 511 characters");
    std::wstring result(native, count);
    if (!IsNativePath(result))
        Fail(ERROR_INVALID_NAME, "The application did not resolve to a native device path");
    return result;
}

std::string Token(std::uint64_t id) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "hidac:0000000000000000";
    for (unsigned i = 0; i < 16; ++i) result[6 + i] = hex[(id >> ((15 - i) * 4)) & 15];
    return result;
}

std::uint64_t ParseToken(const std::string& token) {
    if (token.size() != 22 || token.compare(0, 6, "hidac:") != 0)
        Fail(ERROR_INVALID_PARAMETER, "Invalid device token; obtain it from Session::Devices()");
    std::uint64_t result = 0;
    for (std::size_t i = 6; i < token.size(); ++i) {
        const auto c = token[i];
        const unsigned digit = c >= '0' && c <= '9' ? static_cast<unsigned>(c - '0') :
            c >= 'a' && c <= 'f' ? static_cast<unsigned>(c - 'a' + 10) :
            c >= 'A' && c <= 'F' ? static_cast<unsigned>(c - 'A' + 10) : 16;
        if (digit == 16) Fail(ERROR_INVALID_PARAMETER, "Invalid hexadecimal device token");
        result = (result << 4) | digit;
    }
    if (!result) Fail(ERROR_INVALID_PARAMETER, "The zero device token cannot be selected");
    return result;
}

void ValidateStatus(const HA_STATUS& status) {
    if (status.size != sizeof(status) || status.version != HA_VERSION)
        Fail(ERROR_REVISION_MISMATCH, "The installed driver's status ABI does not match this SDK");
    if (status.flags & ~(HA_STATUS_ACTIVE | HA_STATUS_EXPIRED) ||
        status.device_count > HA_MAX_DEVICES)
        Fail(ERROR_INVALID_DATA, "Driver returned invalid status flags or device count");
}

Status DecodeStatus(const HA_STATUS& status) {
    ValidateStatus(status);
    Status result;
    result.installed = true;
    result.running = true;
    result.active = (status.flags & HA_STATUS_ACTIVE) != 0;
    result.demo_expired = (status.flags & HA_STATUS_EXPIRED) != 0;
    result.remaining_100ns = status.remaining_100ns;
    result.generation = status.generation;
    result.allowed_opens = status.allowed_opens;
    result.denied_opens = status.denied_opens;
    result.dropped_events = status.dropped_events;
    result.detail = result.demo_expired ? L"The demo has expired. Restart Windows for a new demo period." :
        result.active ? L"Hiding is active for the selected devices and new application opens." :
                        L"The driver is ready. Hiding is stopped.";
    return result;
}

std::vector<Device> DecodeDevices(const HA_DEVICES& input) {
    if (input.size != sizeof(input) || input.version != HA_VERSION)
        Fail(ERROR_REVISION_MISMATCH, "The installed driver's device ABI does not match this SDK");
    if (input.count > HA_MAX_DEVICES || input.reserved)
        Fail(ERROR_INVALID_DATA, "Driver returned an invalid device list");
    std::unordered_set<std::uint64_t> ids;
    std::vector<Device> result;
    result.reserve(input.count);
    for (std::uint32_t i = 0; i < input.count; ++i) {
        const auto& source = input.devices[i];
        if (!source.id || !ids.insert(source.id).second ||
            (source.flags & ~(HA_DEVICE_SUPPORTED | HA_DEVICE_PROTECTED | HA_DEVICE_HID |
                              HA_DEVICE_XUSB | HA_DEVICE_GIP)) ||
            ((source.flags & HA_DEVICE_SUPPORTED) && (source.flags & HA_DEVICE_PROTECTED)))
            Fail(ERROR_INVALID_DATA, "Driver returned invalid or duplicate device identities");
        Device device;
        device.token = Token(source.id);
        device.instance_id = ReadString(source.instance_id);
        device.name = ReadString(source.name);
        if (device.instance_id.empty()) Fail(ERROR_INVALID_DATA, "A driver device identity is empty");
        device.vid = source.vid;
        device.pid = source.pid;
        device.flags = source.flags;
        device.supported = (source.flags & HA_DEVICE_SUPPORTED) != 0;
        device.protected_input = (source.flags & HA_DEVICE_PROTECTED) != 0;
        result.push_back(std::move(device));
    }
    return result;
}

using Resolver = std::wstring (*)(const std::wstring&);
std::unique_ptr<HA_POLICY> BuildPolicy(const std::vector<Rule>& rules,
                                      const std::vector<Device>& devices,
                                      Resolver resolve = CanonicalExecutable) {
    if (rules.size() > HA_MAX_RULES)
        Fail(ERROR_BUFFER_OVERFLOW, "A session supports at most 32 device rules");
    auto result = std::make_unique<HA_POLICY>();
    result->size = sizeof(HA_POLICY);
    result->version = HA_VERSION;
    result->enabled = !rules.empty();
    result->rule_count = static_cast<std::uint32_t>(rules.size());
    std::vector<std::wstring> applications;
    std::unordered_set<std::uint64_t> selected;
    for (std::size_t i = 0; i < rules.size(); ++i) {
        const auto& rule = rules[i];
        const auto id = ParseToken(rule.device);
        if (!selected.insert(id).second)
            Fail(ERROR_INVALID_PARAMETER, "Select each device only once in a policy");
        const auto found = std::find_if(devices.begin(), devices.end(), [&](const auto& device) {
            return device.token == Token(id);
        });
        if (found == devices.end())
            Fail(ERROR_NOT_FOUND, "The selected device is no longer attached; refresh the device list");
        if (!found->supported || found->protected_input)
            Fail(ERROR_ACCESS_DENIED, "Protected or unsupported input devices cannot be hidden");
        const auto mode = static_cast<std::uint32_t>(rule.mode);
        if (mode > HA_MODE_BLOCK_ALL)
            Fail(ERROR_INVALID_PARAMETER, "Unknown device access mode");
        if (mode == HA_MODE_BLOCK_ALL && !rule.applications.empty())
            Fail(ERROR_INVALID_PARAMETER, "BlockAll does not take an application list");
        if (mode != HA_MODE_BLOCK_ALL && rule.applications.empty())
            Fail(ERROR_INVALID_PARAMETER, "Select at least one application, or choose BlockAll");
        if (rule.applications.size() > HA_MAX_APPS)
            Fail(ERROR_BUFFER_OVERFLOW, "A rule accepts at most 32 application entries");
        auto& wire = result->rules[i];
        wire.device_id = id;
        wire.mode = mode;
        for (const auto& application : rule.applications) {
            if (application.empty() || application.find(L'\0') != std::wstring::npos)
                Fail(ERROR_INVALID_PARAMETER, "An application path is empty or contains a null character");
            const auto native = resolve(application);
            if (!IsNativePath(native))
                Fail(ERROR_INVALID_NAME, "An application did not resolve to a bounded native device path");
            const auto existing = std::find_if(applications.begin(), applications.end(),
                [&](const auto& path) { return SamePath(path, native); });
            std::size_t index = static_cast<std::size_t>(existing - applications.begin());
            if (existing == applications.end()) {
                if (applications.size() == HA_MAX_APPS)
                    Fail(ERROR_BUFFER_OVERFLOW, "A policy supports at most 32 distinct applications");
                applications.push_back(native);
                index = applications.size() - 1;
                for (std::size_t c = 0; c < native.size(); ++c)
                    result->apps[index].path[c] = static_cast<std::uint16_t>(native[c]);
            }
            wire.applications |= std::uint64_t{1} << index;
        }
    }
    result->app_count = static_cast<std::uint32_t>(applications.size());
    return result;
}

HANDLE OpenControl() {
    return CreateFileW(HA_CONTROL_PATH, GENERIC_READ | GENERIC_WRITE, 0,
                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

template<class T> std::unique_ptr<T> Query(HANDLE handle, DWORD code, const char* stage) {
    HA_HEADER request{sizeof(HA_HEADER), HA_VERSION};
    auto result = std::make_unique<T>();
    DWORD returned = 0;
    if (!DeviceIoControl(handle, code, &request, sizeof(request), result.get(), sizeof(T),
                          &returned, nullptr)) Fail(GetLastError(), stage);
    if (returned != sizeof(T)) Fail(ERROR_INVALID_DATA, "Driver returned an unexpected response length");
    return result;
}

const wchar_t* Reason(std::uint32_t reason) {
    switch (reason) {
    case HA_REASON_NO_RULE: return L"No matching rule";
    case HA_REASON_LIST_ALLOW: return L"Application matched an allow list";
    case HA_REASON_LIST_DENY: return L"Application denied by the selected rule";
    case HA_REASON_BLOCK_ALL: return L"All user-mode opens blocked for this device";
    case HA_REASON_UNKNOWN_IMAGE: return L"Requesting application identity was unavailable";
    case HA_REASON_KERNEL: return L"Kernel-mode access passed through";
    case HA_REASON_DEMO_EXPIRED: return L"Demo expired; access restored";
    default: Fail(ERROR_INVALID_DATA, "Driver returned an unknown event reason");
    }
}

std::vector<Event> DecodeEvents(const HA_EVENTS& input) {
    if (input.size != sizeof(input) || input.version != HA_VERSION)
        Fail(ERROR_REVISION_MISMATCH, "The installed driver's event ABI does not match this SDK");
    if (input.count > HA_EVENT_BATCH || input.reserved)
        Fail(ERROR_INVALID_DATA, "Driver returned an invalid event list");
    std::vector<Event> result;
    result.reserve(input.count);
    for (std::uint32_t i = 0; i < input.count; ++i) {
        const auto& source = input.events[i];
        if (!source.sequence || source.reserved || source.flags & ~HA_EVENT_DENIED ||
            (i && source.sequence <= input.events[i - 1].sequence))
            Fail(ERROR_INVALID_DATA, "Driver returned malformed or unordered event records");
        Event event;
        event.sequence = source.sequence;
        event.timestamp_100ns = source.timestamp_100ns;
        event.device_id = source.device_id;
        event.process_id = source.process_id;
        event.denied = (source.flags & HA_EVENT_DENIED) != 0;
        event.application = ReadString(source.application);
        event.reason = Reason(source.reason);
        result.push_back(std::move(event));
    }
    return result;
}

} // namespace

struct Session::Impl {
    Handle handle;
    std::mutex gate;
    Impl() : handle(OpenControl()) {
        if (handle.value == INVALID_HANDLE_VALUE)
            Fail(GetLastError(), "Open HID Access Control: install this driver, elevate, and close other owners");
        DecodeStatus(*Query<HA_STATUS>(handle.value, HA_IOCTL_STATUS, "Read driver status"));
    }
};

Status Probe() {
    Status result;
    ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager.value) {
        result.detail = L"Cannot query Windows services. Windows error: " + std::to_wstring(GetLastError());
        return result;
    }
    ServiceHandle service(OpenServiceW(manager.value, HA_SERVICE_NAME, SERVICE_QUERY_STATUS));
    if (!service.value) {
        const auto error = GetLastError();
        result.detail = error == ERROR_SERVICE_DOES_NOT_EXIST ? L"HID Access Control is not installed. Run HidAccessSetup.exe first." :
            L"Cannot query the driver service. Windows error: " + std::to_wstring(error);
        return result;
    }
    result.installed = true;
    SERVICE_STATUS_PROCESS state{};
    DWORD needed = 0;
    if (!QueryServiceStatusEx(service.value, SC_STATUS_PROCESS_INFO,
                              reinterpret_cast<BYTE*>(&state), sizeof(state), &needed)) {
        result.detail = L"Cannot read the driver service state. Windows error: " + std::to_wstring(GetLastError());
        return result;
    }
    result.running = state.dwCurrentState == SERVICE_RUNNING;
    if (!result.running) {
        result.detail = L"The driver is installed but not running. Restart Windows if setup requested it.";
        return result;
    }
    Handle control(OpenControl());
    if (control.value == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        result.detail = error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED ?
            L"The driver is running. Its control session is busy or administrator access is required." :
            L"The service is running but its interface is unavailable. Windows error: " + std::to_wstring(error);
        return result;
    }
    try {
        result = DecodeStatus(*Query<HA_STATUS>(control.value, HA_IOCTL_STATUS, "Read driver status"));
    } catch (const std::system_error& error) {
        result.detail = L"The running driver did not return a compatible status. Windows error: " +
            std::to_wstring(error.code().value());
    }
    return result;
}

Session::Session() : impl_(std::make_unique<Impl>()) {}
Session::~Session() = default;
Session::Session(Session&&) noexcept = default;
Session& Session::operator=(Session&&) noexcept = default;

std::vector<Device> Session::Devices() {
    if (!impl_) Fail(ERROR_INVALID_HANDLE, "The session was moved from");
    const std::lock_guard<std::mutex> lock(impl_->gate);
    return DecodeDevices(*Query<HA_DEVICES>(impl_->handle.value, HA_IOCTL_DEVICES, "Enumerate attached filters"));
}

Status Session::GetStatus() {
    if (!impl_) Fail(ERROR_INVALID_HANDLE, "The session was moved from");
    const std::lock_guard<std::mutex> lock(impl_->gate);
    return DecodeStatus(*Query<HA_STATUS>(impl_->handle.value, HA_IOCTL_STATUS, "Read driver status"));
}

void Session::Apply(const std::vector<Rule>& rules) {
    if (!impl_) Fail(ERROR_INVALID_HANDLE, "The session was moved from");
    const std::lock_guard<std::mutex> lock(impl_->gate);
    if (rules.empty()) {
        HA_HEADER request{sizeof(HA_HEADER), HA_VERSION};
        DWORD returned = 0;
        if (!DeviceIoControl(impl_->handle.value, HA_IOCTL_RESET, &request, sizeof(request),
                              nullptr, 0, &returned, nullptr)) Fail(GetLastError(), "Stop device hiding");
        return;
    }
    const auto devices = DecodeDevices(*Query<HA_DEVICES>(impl_->handle.value, HA_IOCTL_DEVICES,
                                                          "Validate selected device attachments"));
    const auto policy = BuildPolicy(rules, devices);
    DWORD returned = 0;
    if (!DeviceIoControl(impl_->handle.value, HA_IOCTL_APPLY, policy.get(), sizeof(HA_POLICY),
                          nullptr, 0, &returned, nullptr)) Fail(GetLastError(), "Apply device access policy");
}

void Session::Stop() noexcept {
    if (!impl_) return;
    try {
        const std::lock_guard<std::mutex> lock(impl_->gate);
        HA_HEADER request{sizeof(HA_HEADER), HA_VERSION};
        DWORD returned = 0;
        if (!DeviceIoControl(impl_->handle.value, HA_IOCTL_RESET, &request, sizeof(request),
                               nullptr, 0, &returned, nullptr)) {
            // A failed explicit reset must not leave a live owner policy behind.
            CloseHandle(impl_->handle.value);
            impl_->handle.value = INVALID_HANDLE_VALUE;
        }
    } catch (...) {
        // Closing the owner still causes the driver to discard the session policy.
    }
}

std::vector<Event> Session::ReadEvents() {
    if (!impl_) Fail(ERROR_INVALID_HANDLE, "The session was moved from");
    const std::lock_guard<std::mutex> lock(impl_->gate);
    return DecodeEvents(*Query<HA_EVENTS>(impl_->handle.value, HA_IOCTL_EVENTS, "Read access decisions"));
}

} // namespace HidAccess
