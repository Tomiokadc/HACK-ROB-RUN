#pragma once
#include "../driver_client/driver_client.hpp"
#include <vector>

inline std::uint64_t FindGameModel(DriverClient& driver, DWORD pid, std::uint64_t base) {
    const auto pointer = [&](std::uint64_t address) {
        std::uint64_t value{};
        if (!driver.Read(pid, address, &value, sizeof(value))) return std::uint64_t{};
        if (value < 0x10000 || value > 0x7FFFFFFFFFFF) return std::uint64_t{};
        return value;
    };
    const auto modelClass = pointer(base + 0x81465C0);
    if (!modelClass) return 0;
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!process) return 0;
    std::vector<std::uint64_t> page(0x100000 / 8);
    MEMORY_BASIC_INFORMATION region{};
    for (std::uintptr_t cursor = 0x10000;
         VirtualQueryEx(process, reinterpret_cast<void*>(cursor), &region, sizeof(region));) {
        const auto start = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        cursor = start + region.RegionSize;
        if (region.State != MEM_COMMIT || region.Type != MEM_PRIVATE ||
            region.Protect != PAGE_READWRITE) continue;
        for (std::size_t offset = 0; offset < region.RegionSize; offset += 0x100000) {
            const auto left = region.RegionSize - offset;
            const auto bytes = left < 0x100000 ? left : 0x100000;
            if (!driver.Read(pid, start + offset, page.data(), bytes)) continue;
            for (std::size_t slot = 0; slot < bytes / 8; ++slot) {
                if (page[slot] != modelClass) continue;
                const auto model = start + offset + slot * 8;
                const auto battle = pointer(model + 0x10);
                const auto entities = battle ? pointer(battle + 0x90) : 0;
                const auto player = entities ? pointer(entities + 0xD0) : 0;
                const auto dict = entities ? pointer(entities + 0x50) : 0;
                std::int32_t count{};
                if (player && dict && driver.Read(pid, dict + 0x20, &count, sizeof(count)) &&
                    count > 0 && count < 100000) {
                    CloseHandle(process);
                    return model;
                }
            }
        }
    }

    CloseHandle(process);
    return 0;
}
