#include "driver_client/driver_client.hpp"
#include <iostream>
#include "game/model_search.hpp"
#include <cstring>

int wmain(int argc, wchar_t** argv) {
    const DWORD pid = DriverClient::GetPidByName(L"ROB RUN.exe");
    if (!pid) {
        std::cerr << "ROB RUN.exe is not running\n";
        return 1;
    }
    DriverClient driver(argc > 1 ? argv[1] : LR"(\\.\RobRunProbe)");
    if (!driver.IsOpen()) {
        std::cerr << "Device open failed. Win32 error: " << driver.Error() << std::endl;
        return 4;
    }
    const auto base = driver.GetModuleBase(pid, L"GameAssembly.dll");
    if (!base) {
        std::cerr << "Module lookup failed. Win32 error: " << driver.Error() << '\n';
        return 2;
    }
    std::uint16_t magic{};
    if (!driver.Read(pid, base, &magic, sizeof(magic)) || magic != 0x5A4D) {
        std::cerr << "PE header check failed. Win32 error: " << driver.Error() << '\n';
        return 3;
    }
    std::cout << "GameAssembly.dll base: 0x" << std::hex << base << '\n';

    const auto pointer = [&](std::uint64_t address) {
        std::uint64_t value{};
        if (!driver.Read(pid, address, &value, sizeof(value))) return std::uint64_t{};
        if (value < 0x10000 || value > 0x7FFFFFFFFFFF) return std::uint64_t{};
        return value;
    };
    if (argc > 2 && wcscmp(argv[2], L"--entities") == 0) {
        const auto model = FindGameModel(driver, pid, base);
        if (!model) return 8;
        std::cout << "model=0x" << std::hex << model << '\n';
        return 0;
    }

    const auto klass = pointer(base + 0x8162748);
    const auto fields = klass ? pointer(klass + 0xB8) : 0;
    const auto core = fields ? pointer(fields + 0x08) : 0;
    const auto brains = core ? pointer(core + 0x10) : 0;
    if (!brains) {
        std::cerr << "Cinemachine chain unavailable (class/static fields/core/list): "
                  << std::hex << klass << '/' << fields << '/' << core << '/' << brains << '\n';
        return 5;
    }

    // List<T> stores the backing array at +0x10 and its logical size at +0x18.
    std::int32_t count{};
    if (!driver.Read(pid, brains + 0x18, &count, sizeof(count)) || count < 0 || count > 256) {
        std::cerr << "Invalid Cinemachine list size\n";
        return 6;
    }
    std::cout << "Active Cinemachine brains: " << std::dec << count << '\n';
    const auto items = count ? pointer(brains + 0x10) : 0;
    if (count && !items) {
        std::cerr << "Cinemachine list backing array unavailable\n";
        return 7;
    }
    for (std::int32_t i = 0; i < count; ++i) {
        const auto brain = pointer(items + 0x20 + static_cast<std::uint64_t>(i) * 8);
        float fov{};
        float position[3]{};
        if (!brain || !driver.Read(pid, brain + 0xB0, &fov, sizeof(fov)) ||
            !driver.Read(pid, brain + 0xB0 + 0x4C, position, sizeof(position))) {
            std::cerr << "Unable to read camera " << i << '\n';
            continue;
        }
        std::cout << "Camera " << i << " FOV=" << fov << " position=("
                  << position[0] << ", " << position[1] << ", " << position[2] << ")\n";
    }
}


