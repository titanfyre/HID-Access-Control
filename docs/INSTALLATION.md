# Installation and removal

Windows 10 2004+ / Windows 11 x64 is the target. Installation and live configuration require administrator rights. Windows kernel filter association changes take effect when device stacks are rebuilt; use Windows Restart when setup requests it.

## Complete release bundle

The release ZIP contains the application, example, both setup EXEs, public C++ sources and the Microsoft-signed driver package. Each setup EXE embeds the complete package and can install it on its own. No separate driver download, test mode or developer certificate is needed. Evaluation driver source is documented in [driver/README.md](../driver/README.md).

Keep the app, SDK and driver from the same product release. `HidAccessApp.exe --preview` is an optional labelled synthetic interface; it does not test physical-device compatibility or enable a driver session.

## Normal installation

1. Extract the complete ZIP. Run **HidAccessSetup.exe** and accept elevation.
2. Choose **Install / reinstall**. Setup checks the exact INF identity, trusted catalog and catalog membership of both SYS files before installation.
3. If requested, choose **Restart** or restart Windows yourself.
4. Open **HidAccessApp.exe** as administrator. Refresh devices, select a supported controller collection and configure its access rules.

Console automation uses:

```powershell
.\HidAccessSetupConsole.exe /install
.\HidAccessSetupConsole.exe /uninstall
```

Exit code `3010` means a normal Windows restart is needed. No forced restart is performed. Setup appends its own filter name to existing supported class lists, preserving other filters. It skips optional controller classes Windows has not created yet; rerun installation after adding such a controller and restart if requested. It never creates an incomplete Windows device class.

The root control device is `ROOT\TITANHIDACCESS`. Services `TitanHidAccess` and `TitanHidAccessClock` belong to this product. The clock is a kernel driver, not a user-mode licensing service.

## Removal

Stop/close the application that owns the policy. Use **Uninstall** in setup, or the console command above. Setup removes this product's root node and its own class-filter registrations, preserving unrelated values. A resident clock or an in-use package can require a restart; rerun removal afterwards if Windows still retains the package. Other drivers and their device settings are outside the uninstaller's ownership.

The UI has session-scoped policies. Closing its owner clears those rules even if explicit reset failed. Existing application handles are outside policy revocation and must be closed by the owning application itself.

## Troubleshooting

- **Driver missing:** install this product's matching Microsoft-signed bundle; a HidHide or Interceptor installation uses a different interface.
- **Access denied opening configuration:** use an administrator process.
- **Configuration in use:** close the other UI/SDK session; one owner controls the filter at a time.
- **Device is protected:** system keyboard/mouse paths, transport parents and unclassified HID nodes cannot be selected. Choose a supported controller collection.
- **A program still reads the controller:** close its existing handle/restart that program; check which executable or broker actually opens the device.
- **Rules stop after one hour:** the per-boot demo expired. Restart Windows, or contact Discord **titancsa** about a full license.

Windows device inventories can contain several nodes per physical controller. VID/PID describe individual interfaces and are not a unique stable device identity. Use the current SDK token/instance details and the access log when diagnosing a rule.
