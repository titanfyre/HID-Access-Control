#include "../Installer/Setup.h"
#include <iostream>

int main() {
    const auto result = Setup::SelfTest();
    std::wcout << result.detail << L'\n';
    return result.error ? 1 : 0;
}
