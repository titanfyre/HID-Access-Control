# Validation and compatibility boundary

This page records exercised behavior rather than claiming universal HidHide parity. The release includes the Microsoft-signed one-hour evaluation driver. Windows 10 2004+ / Windows 11 x64 is the intended target; live checks are performed in an owned disposable Windows 11 VM. Other physical hardware, Windows versions and device stacks require qualification.

## Offline checks

- User-mode C++17 targets compile with `/W4 /WX` and explicit Windows x64 structures.
- SDK checks exercise ABI sizes, malformed/truncated strings, counts, native path resolution, identity lifetime, duplicate/protected devices, application masks and ordered bounded event batches.
- User-mode protocol checks also run under AddressSanitizer.
- Installer self-tests validate filter-list preservation, idempotency and malformed registry values without editing live Windows state.
- A missing production payload is rejected before installation changes. Public setup never accepts a private laboratory certificate as a Microsoft production signature.

## Kernel package checks

The private build runs compiler/PREfast analysis, Windows and universal INF verification and catalog generation. The submission CAB contains its package beneath a short directory, with no loose files at the CAB root, and is extracted again to check hashes. A laboratory duration override is build-only, private and excluded from the production one-hour CAB.

The Microsoft return for driver version **0.1.0.0** has valid Microsoft Windows Hardware Compatibility Publisher signatures on the catalog and both SYS files. Windows kernel-policy signature verification confirms that the catalog covers the INF and both SYS files. The returned INF is unchanged, and every executable/data section in both returned SYS files matches the production code tested before submission.

## Live checks

The lab used owned virtual HID controller, keyboard and mouse fixtures supplied by an existing Microsoft-signed Vdevice driver, in Windows 11 build 26200. The final production driver code, privately test-signed for that guest, passed a normal reboot and the access suite with **Driver Verifier standard checks enabled for both product SYS files**.

The returned Microsoft-signed package was then installed using the release's self-contained console setup with **Secure Boot enabled, test signing disabled and code-integrity enforcement active**. After the requested Windows restart, the driver was running and the eight-case functional suite passed again. These tests used owned virtual fixtures and did not send input reports to physical devices.

| Check | Result |
| --- | --- |
| SDK ABI, bounds, identity and policy tests | 49 assertions passed, including an AddressSanitizer run |
| Installer filter-list validation | Passed offline, without system changes |
| New device opens from different executable paths | Allow-list, deny-list and BlockAll decisions passed |
| Existing HID handle | Remained usable, matching the documented boundary |
| Protected and stale device policies | Refused while retaining the previous active policy |
| Access-decision log | Reported the application identity and decision reason |
| Stop | Restored subsequent opens from both probe applications |
| Short-clock regression | A private 12-second build restored access and refused reactivation at expiry; the production clock remains one hour |
| Control endpoint | Eight elevated/restricted-token direct/suffix/zero-access cases passed |
| Installer trust refusal | Missing payload rejected with error 2; a locally trusted laboratory-signed catalog was rejected with error 577 |
| Native interface | Synthetic self-test, compact three-row layout and screenshot generation passed |
| Microsoft-signed return | Signature/catalog membership checks, embedded-payload install and the eight functional cases passed with normal signing enforcement and Secure Boot |
| Signed-package removal | Uninstall returned the restart request; after restart, a repeat removal reported not installed, with no product services, root nodes or class-filter registrations remaining |

The first Verifier boot caught an invalid KMDF send-and-forget use for create requests with framework file objects. The filter now uses a proper completion routine and propagates the lower driver's status/information. The final corrected driver then passed the Verifier boot and functional suite above. This evidence does not establish absence of all defects.

A real one-hour wall-clock expiry and physical XUSB/GIP controllers have not been exhaustively exercised in this initial validation. The Microsoft signature establishes package trust; it is not a claim of universal hardware compatibility or complete Windows Hardware Lab Kit qualification.

## Important boundaries

- Policy decisions happen on new user-mode opens. Existing handles are not revoked, and input reports are not modified or delayed by a policy loop.
- Allowed requests may still fail below this filter. The journal records this filter's own decision, not every final driver or application outcome.
- A Windows broker's handles belong to that broker, not each app/tab using it. Kernel-mode opens remain transparent.
- Only identified controller collection/class paths can be selected. Unknown HID nodes, transport parents and keyboard/mouse paths are protected in the kernel.
- Path rules match the actual requestor's normalized native image path. They are not executable-signature authorization or an anti-tamper boundary against administrators, renamed/hard-linked files, handle duplication, injected code or kernel code.
- XUSB and GIP class attachment support does not imply every game's, browser's or controller's access path has been validated.
- Rules are session-scoped and end with their owning application. Persistent startup policies are not part of this initial version.
- All configuration, device inventory and audit buffers are bounded. Overflow is reported. Very long native paths can be truncated in diagnostics; overlong executable policy paths are rejected rather than silently shortened.
