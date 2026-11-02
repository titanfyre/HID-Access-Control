# C++ SDK

Add `SDK/HidAccess.cpp`, `SDK/HidAccess.h` and `Protocol/HidAccessProtocol.h` to a
Windows x64 C++17 project. Link `advapi32.lib`. The SDK has no runtime DLL or
background service. Install the matching driver and run live clients as
administrator.

```cpp
#include "HidAccess.h"

HidAccess::Session access;
for (const auto& device : access.Devices()) {
    if (!device.supported || device.protected_input) continue;
    access.Apply({{device.token, HidAccess::Mode::AllowListed,
                   {L"C:\\Tools\\MyControllerApp.exe"}}});
    break; // This small example chooses the first supported controller.
}
// Session destruction restores normal access, including after process exit.
```

The complete [example](../Examples/access.cpp) selects a supported source
and resolves its own executable path. Device tokens are attachment-scoped: obtain
them again after reconnecting a device or restarting the driver.

| Operation | Purpose |
| --- | --- |
| `Probe()` | Read installation/service status without starting the demo or hiding anything. |
| `Session()` | Open the exclusive administrator control session. |
| `Devices()` | List attached filter identities, support and protected-input flags. |
| `GetStatus()` | Read active state, demo time, generation and open-decision counters. |
| `Apply(rules)` | Atomically replace all rules; infer counts and resolve executable paths. An empty list stops hiding. |
| `Stop()` | Best-effort reset; closes the owner handle if reset fails. |
| `ReadEvents()` | Drain a bounded batch of access decisions and English reasons. |

`AllowListed` permits only selected applications to open a device. `DenyListed`
blocks the selected applications and permits other user-mode callers. `BlockAll`
blocks user-mode opens without an application list. Kernel opens and protected
input devices are outside these policies. Allow/Deny lists require at least one
application; `BlockAll` takes none. The SDK resolves existing file paths to native
device paths, deduplicates them without case sensitivity and validates the whole
policy before sending it. Maximums are 32 rules and 32 distinct applications.

The control interface is exclusive: stop the UI before using another SDK client.
Calls on one live session are serialized. Stop all callers before moving or
destroying a session. Errors throw `std::system_error` with an English stage and a
Windows error code. A moved-from session rejects operations. `Stop()` does not
throw; after an exceptional reset failure, create a new session before reuse.

Policies affect **new opens**, not already-open handles. Executable-path matching
is an access routing tool, not an anti-tamper or sandbox boundary: aliases,
administrators, kernel callers and shared Windows input brokers have separate
implications. See the main documentation for tested transports and limitations.
The demo starts with the first active policy, not with queries. Expiry and owner
exit restore normal access.
