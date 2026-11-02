// Offline parser/serialization checks only. Including the implementation gives
// this translation unit access to its private pure helpers, without adding a
// test backdoor or alternate transport to the production SDK.
#include "../SDK/HidAccess.cpp"
#include <iostream>

namespace {
unsigned assertions = 0;
void Require(bool condition, const char* description) {
    ++assertions;
    if (!condition) throw std::runtime_error(description);
}

template<class F> void Reject(DWORD code, F operation) {
    ++assertions;
    try {
        operation();
    } catch (const std::system_error& error) {
        if (static_cast<DWORD>(error.code().value()) != code)
            throw std::runtime_error("Unexpected Windows error code");
        return;
    }
    throw std::runtime_error("Invalid input was accepted");
}

std::wstring FixtureResolve(const std::wstring& path) {
    if (path.compare(0, 3, L"C:\\") == 0)
        return L"\\Device\\HarddiskVolume77\\" + path.substr(3);
    return path;
}

template<std::size_t N> void Put(std::uint16_t (&out)[N], const std::wstring& text) {
    Require(text.size() < N, "Fixture string exceeds its wire field");
    for (std::size_t i = 0; i < text.size(); ++i) out[i] = static_cast<std::uint16_t>(text[i]);
    out[text.size()] = 0;
}

void IdentityAndReplies() {
    using namespace HidAccess;
    Require(ParseToken(Token(0xFEDCBA9876543210ull)) == 0xFEDCBA9876543210ull,
            "A wide attachment identity was truncated");
    for (const std::string token : {"", "hidac:1", "hidac:0000000000000000", "other:0000000000000001",
                                    "hidac:000000000000000z", "hidac:00000000000000010"}) {
        Reject(ERROR_INVALID_PARAMETER, [&] { ParseToken(token); });
    }
    auto devices = std::make_unique<HA_DEVICES>();
    devices->size = sizeof(HA_DEVICES);
    devices->version = HA_VERSION;
    devices->count = 1;
    devices->devices[0].id = 0xFEDCBA9876543210ull;
    devices->devices[0].flags = HA_DEVICE_SUPPORTED | HA_DEVICE_HID;
    Put(devices->devices[0].instance_id, L"HID\\VID_FFFE&PID_B100\\fixture");
    Put(devices->devices[0].name, L"Unicode fixture \u00e9 \U0001F3AE");
    auto decoded = DecodeDevices(*devices);
    Require(decoded.size() == 1 && decoded[0].token == "hidac:fedcba9876543210",
            "Device identity did not survive a reply");
    Require(decoded[0].name == L"Unicode fixture \u00e9 \U0001F3AE", "UTF-16 name changed");
    Require(decoded[0].supported && !decoded[0].protected_input, "Support flags changed");
    devices->version = HA_VERSION + 1;
    Reject(ERROR_REVISION_MISMATCH, [&] { DecodeDevices(*devices); });
    devices->version = HA_VERSION;
    devices->count = HA_MAX_DEVICES + 1;
    Reject(ERROR_INVALID_DATA, [&] { DecodeDevices(*devices); });
    devices->count = 2;
    devices->devices[1] = devices->devices[0];
    Reject(ERROR_INVALID_DATA, [&] { DecodeDevices(*devices); });
    devices->count = 1;
    devices->devices[0].flags |= HA_DEVICE_PROTECTED;
    Reject(ERROR_INVALID_DATA, [&] { DecodeDevices(*devices); });
    devices->devices[0].flags = HA_DEVICE_SUPPORTED | HA_DEVICE_HID;
    std::fill(std::begin(devices->devices[0].name), std::end(devices->devices[0].name), std::uint16_t{0x41});
    Reject(ERROR_INVALID_DATA, [&] { DecodeDevices(*devices); });
    HA_STATUS status{};
    status.size = sizeof(status);
    status.version = HA_VERSION;
    status.flags = HA_STATUS_ACTIVE;
    status.generation = 0x8000000000000001ull;
    const auto info = DecodeStatus(status);
    Require(info.active && info.running && info.installed && info.generation == status.generation,
            "Status identity or activity changed");
    status.flags |= 0x80000000;
    Reject(ERROR_INVALID_DATA, [&] { DecodeStatus(status); });
}

void AtomicPolicyEncoding() {
    using namespace HidAccess;
    Device first;
    first.token = Token(1);
    first.supported = true;
    Device second = first;
    second.token = Token(2);
    const std::vector<Device> devices{first, second};
    auto policy = BuildPolicy({{first.token, Mode::AllowListed, {L"C:\\Tool.exe", L"C:\\TOOL.exe"}},
                               {second.token, Mode::DenyListed, {L"C:\\Other.exe"}}},
                              devices, FixtureResolve);
    Require(policy->rule_count == 2 && policy->app_count == 2 && policy->enabled,
            "Policy did not deduplicate applications or infer counts");
    Require(policy->rules[0].applications == 1 && policy->rules[1].applications == 2,
            "Per-device application masks were merged");
    Require(ReadString(policy->apps[0].path) == L"\\Device\\HarddiskVolume77\\Tool.exe",
            "Policy did not encode native UTF-16 application identity");
    Require(policy->reserved == 0 && policy->rules[0].reserved == 0 && policy->apps[2].path[0] == 0,
            "Unused policy fields were not zero initialized");
    auto none = BuildPolicy({}, devices, FixtureResolve);
    Require(!none->enabled && !none->rule_count && !none->app_count, "Empty policy was activated");
    auto block = BuildPolicy({{first.token, Mode::BlockAll, {}}}, devices, FixtureResolve);
    Require(block->rules[0].mode == HA_MODE_BLOCK_ALL && !block->rules[0].applications,
            "BlockAll acquired an application mask");
    Reject(ERROR_INVALID_PARAMETER, [&] {
        BuildPolicy({{first.token, Mode::AllowListed, {}}}, devices, FixtureResolve);
    });
    Reject(ERROR_INVALID_PARAMETER, [&] {
        BuildPolicy({{first.token, Mode::BlockAll, {L"C:\\Tool.exe"}}}, devices, FixtureResolve);
    });
    Reject(ERROR_INVALID_PARAMETER, [&] {
        BuildPolicy({{first.token, static_cast<Mode>(3), {}}}, devices, FixtureResolve);
    });
    Reject(ERROR_INVALID_PARAMETER, [&] {
        BuildPolicy({{first.token, Mode::BlockAll, {}}, {first.token, Mode::BlockAll, {}}}, devices, FixtureResolve);
    });
    Reject(ERROR_NOT_FOUND, [&] {
        BuildPolicy({{Token(3), Mode::BlockAll, {}}}, devices, FixtureResolve);
    });
    Device protectedDevice = first;
    protectedDevice.protected_input = true;
    Reject(ERROR_ACCESS_DENIED, [&] {
        BuildPolicy({{first.token, Mode::BlockAll, {}}}, {protectedDevice}, FixtureResolve);
    });
    protectedDevice.protected_input = false;
    protectedDevice.supported = false;
    Reject(ERROR_ACCESS_DENIED, [&] {
        BuildPolicy({{first.token, Mode::BlockAll, {}}}, {protectedDevice}, FixtureResolve);
    });
    Reject(ERROR_BUFFER_OVERFLOW, [&] {
        BuildPolicy(std::vector<Rule>(HA_MAX_RULES + 1), devices, FixtureResolve);
    });
    Rule many{first.token, Mode::AllowListed, {}};
    for (unsigned i = 0; i < HA_MAX_APPS; ++i)
        many.applications.push_back(L"C:\\App" + std::to_wstring(i) + L".exe");
    auto full = BuildPolicy({many}, devices, FixtureResolve);
    Require(full->app_count == HA_MAX_APPS && full->rules[0].applications == 0xffffffffull,
            "The top supported application bit was lost");
    Reject(ERROR_BUFFER_OVERFLOW, [&] {
        BuildPolicy({many, {second.token, Mode::DenyListed, {L"C:\\Extra.exe"}}}, devices, FixtureResolve);
    });
    many.applications.push_back(L"C:\\Extra.exe");
    Reject(ERROR_BUFFER_OVERFLOW, [&] { BuildPolicy({many}, devices, FixtureResolve); });
    Reject(ERROR_INVALID_PARAMETER, [&] {
        BuildPolicy({{first.token, Mode::DenyListed, {std::wstring(L"C:\\a\0b.exe", 10)}}}, devices, FixtureResolve);
    });
    Reject(ERROR_INVALID_NAME, [&] {
        BuildPolicy({{first.token, Mode::DenyListed, {L"not-native"}}}, devices, FixtureResolve);
    });
    Reject(ERROR_INVALID_NAME, [&] {
        BuildPolicy({{first.token, Mode::DenyListed, {L"\\Device\\" + std::wstring(HA_PATH_CHARS, L'x')}}},
                    devices, FixtureResolve);
    });
}

void EventBoundsAndFiles() {
    using namespace HidAccess;
    auto events = std::make_unique<HA_EVENTS>();
    events->size = sizeof(HA_EVENTS);
    events->version = HA_VERSION;
    events->count = 1;
    events->events[0].sequence = 0x8000000000000001ull;
    events->events[0].device_id = 1;
    events->events[0].process_id = 42;
    events->events[0].flags = HA_EVENT_DENIED;
    events->events[0].reason = HA_REASON_LIST_DENY;
    Put(events->events[0].application, L"\\Device\\HarddiskVolume77\\Tool.exe");
    const auto decoded = DecodeEvents(*events);
    Require(decoded.size() == 1 && decoded[0].denied && decoded[0].process_id == 42,
            "Event decision changed");
    events->count = HA_EVENT_BATCH + 1;
    Reject(ERROR_INVALID_DATA, [&] { DecodeEvents(*events); });
    events->count = 2;
    events->events[1] = events->events[0];
    Reject(ERROR_INVALID_DATA, [&] { DecodeEvents(*events); });
    events->count = 1;
    events->events[0].reason = 99;
    Reject(ERROR_INVALID_DATA, [&] { DecodeEvents(*events); });
    events->events[0].reason = HA_REASON_LIST_DENY;
    events->events[0].flags = 2;
    Reject(ERROR_INVALID_DATA, [&] { DecodeEvents(*events); });
    wchar_t self[32768]{};
    Require(GetModuleFileNameW(nullptr, self, 32768) != 0, "Locate test executable");
    Require(IsNativePath(CanonicalExecutable(self)), "A real executable did not resolve to an NT path");
    Reject(ERROR_DIRECTORY, [&] { CanonicalExecutable(L"."); });
    Reject(ERROR_INVALID_PARAMETER, [&] { CanonicalExecutable(std::wstring(L"a\0b", 3)); });
}
} // namespace

int main() try {
    IdentityAndReplies();
    AtomicPolicyEncoding();
    EventBoundsAndFiles();
    std::cout << "PASS: " << assertions << " offline ABI, bounds, identity, policy and reply checks. No driver opened.\n";
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
