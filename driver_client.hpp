#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>

class DriverClient {
public:
    explicit DriverClient(const wchar_t* device = LR"(\\.\RobRunProbe)");
    ~DriverClient();
    DriverClient(const DriverClient&) = delete;
    DriverClient& operator=(const DriverClient&) = delete;

    bool Read(DWORD pid, std::uint64_t address, void* buffer, std::size_t size);
    bool Write(DWORD pid, std::uint64_t address, const void* buffer, std::size_t size);
    std::uint64_t GetModuleBase(DWORD pid, const wchar_t* name);
    static DWORD GetPidByName(const wchar_t* name);
    bool IsOpen() const { return device_ != INVALID_HANDLE_VALUE; }
    DWORD Error() const { return error_; }

private:
    bool Bind(DWORD pid);
    bool Request(DWORD code, void* input, DWORD inputSize, void* output,
                 DWORD outputSize, DWORD expectedSize);
    HANDLE device_;
    DWORD boundPid_ = 0;
    DWORD error_ = ERROR_SUCCESS;
};



