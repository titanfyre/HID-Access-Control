#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace HidAccess {

enum class Mode : std::uint32_t { AllowListed, DenyListed, BlockAll };

struct Device {
    // Valid only for this driver's current device attachment, not a serial number.
    std::string token;
    std::wstring instance_id;
    std::wstring name;
    std::uint16_t vid = 0;
    std::uint16_t pid = 0;
    bool supported = false;
    bool protected_input = false;
    std::uint32_t flags = 0;
};

struct Rule {
    std::string device;
    Mode mode = Mode::AllowListed;
    // Existing executable paths. The SDK resolves native device paths itself.
    std::vector<std::wstring> applications;
};

struct Status {
    bool installed = false;
    bool running = false;
    bool active = false;
    bool demo_expired = false;
    std::uint64_t remaining_100ns = 0;
    std::uint64_t generation = 0;
    std::uint64_t allowed_opens = 0;
    std::uint64_t denied_opens = 0;
    std::uint64_t dropped_events = 0;
    std::wstring detail;
};

struct Event {
    std::uint64_t sequence = 0;
    std::uint64_t timestamp_100ns = 0;
    std::uint64_t device_id = 0;
    std::uint32_t process_id = 0;
    bool denied = false;
    std::wstring application;
    std::wstring reason;
};

// Installation/service query; never activates a policy or starts the demo.
// detail distinguishes a missing driver, stopped service and a busy control owner.
Status Probe();

class Session {
public:
    // Requires administrator rights and sole ownership of the control interface.
    Session();
    ~Session();
    Session(Session&&) noexcept;
    Session& operator=(Session&&) noexcept;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    std::vector<Device> Devices();
    Status GetStatus();
    // Atomically replace the session policy. An empty list stops hiding.
    void Apply(const std::vector<Rule>& rules);
    void Stop() noexcept;
    std::vector<Event> ReadEvents();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Failures throw std::system_error with an English stage and a Windows code.
// Calls on a live Session are serialized internally. Stop all callers before
// moving or destroying it. Destruction closes the owner handle, restoring access
// even if explicit Stop failed. Policies affect new device opens, not old handles.

} // namespace HidAccess
