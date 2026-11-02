#include "../SDK/HidAccess.h"
#include <windows.h>
#include <algorithm>
#include <iostream>
#include <system_error>

int main() try {
    std::cout << "Hide one supported controller from new user-mode opens, except this application.\n"
                 "Run as administrator. Press Enter or close this program to restore access.\n";
    HidAccess::Session access;
    const auto devices = access.Devices();
    const auto controller = std::find_if(devices.begin(), devices.end(), [](const auto& device) {
        return device.supported && !device.protected_input;
    });
    if (controller == devices.end()) {
        std::cerr << "No supported controller filter is attached. Connect a controller and restart if setup requests it.\n";
        return 1;
    }
    wchar_t application[32768]{};
    const auto length = GetModuleFileNameW(nullptr, application, 32768);
    if (!length || length >= 32768) {
        throw std::system_error(length ? ERROR_FILENAME_EXCED_RANGE : GetLastError(),
                                std::system_category(), "Locate this executable");
    }
    std::wcout << L"Selected: " << controller->name << L'\n';
    access.Apply({{controller->token, HidAccess::Mode::AllowListed, {application}}});
    std::cout << "Policy is active. Existing open handles are unaffected; reopen the application you want to test.\n";
    std::cin.get();
    // Destruction closes the owner session and removes its policy automatically.
} catch (const std::system_error& error) {
    std::cerr << "Driver error: " << error.what() << " (Windows " << error.code().value() << ")\n";
    return 1;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
