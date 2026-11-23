#include "driver_client.hpp"

#include <TlHelp32.h>
#include <winioctl.h>
#include <cstring>
#include <cwchar>
#include <vector>

namespace {
constexpr DWORD version = 2;
constexpr DWORD maxTransfer = 1024 * 1024;
constexpr DWORD bindCode = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr DWORD readCode = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_READ_DATA);
constexpr DWORD writeCode = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_WRITE_DATA);
constexpr DWORD moduleCode = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x809, METHOD_BUFFERED, FILE_READ_DATA);

struct BindRequest { DWORD version; DWORD pid; };
struct TransferRequest { DWORD version; DWORD size; std::uint64_t address; };
struct ModuleRequest { DWORD version; DWORD reserved; wchar_t name[64]; };
struct ModuleResponse { std::uint64_t base; DWORD size; DWORD reserved; };
static_assert(sizeof(BindRequest) == 8 && sizeof(TransferRequest) == 16);
static_assert(sizeof(ModuleRequest) == 136 && sizeof(ModuleResponse) == 16);
}

DriverClient::DriverClient(const wchar_t* device)
    : device_(CreateFileW(device, GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {
    if (device_ == INVALID_HANDLE_VALUE) error_ = GetLastError();
}

DriverClient::~DriverClient() {
    if (device_ != INVALID_HANDLE_VALUE) CloseHandle(device_);
}

bool DriverClient::Request(DWORD code, void* input, DWORD inputSize, void* output,
                           DWORD outputSize, DWORD expectedSize) {
    if (device_ == INVALID_HANDLE_VALUE) return false;
    DWORD received = 0;
    if (!DeviceIoControl(device_, code, input, inputSize, output, outputSize, &received, nullptr)) {
        error_ = GetLastError();
        return false;
    }
    error_ = received == expectedSize ? ERROR_SUCCESS : ERROR_INVALID_DATA;
    return error_ == ERROR_SUCCESS;
}

// Один экземпляр клиента используется одним потоком: BIND меняет PID дескриптора.
bool DriverClient::Bind(DWORD pid) {
    if (pid != 0 && pid == boundPid_) return true;
    BindRequest request{version, pid};
    if (!Request(bindCode, &request, sizeof(request), nullptr, 0, 0)) return false;
    boundPid_ = pid;
    return true;
}

bool DriverClient::Read(DWORD pid, std::uint64_t address, void* buffer, std::size_t size) {
    if (!buffer || size == 0 || size > maxTransfer) {
        error_ = ERROR_INVALID_PARAMETER;
        return false;
    }
    TransferRequest request{version, static_cast<DWORD>(size), address};
    return Bind(pid) && Request(readCode, &request, sizeof(request), buffer, request.size, request.size);
}

bool DriverClient::Write(DWORD pid, std::uint64_t address, const void* buffer, std::size_t size) {
    if (!buffer || size == 0 || size > maxTransfer) {
        error_ = ERROR_INVALID_PARAMETER;
        return false;
    }
    TransferRequest request{version, static_cast<DWORD>(size), address};
    std::vector<std::byte> packet(sizeof(request) + size);
    std::memcpy(packet.data(), &request, sizeof(request));
    std::memcpy(packet.data() + sizeof(request), buffer, size);
    return Bind(pid) && Request(writeCode, packet.data(), static_cast<DWORD>(packet.size()), nullptr, 0, 0);
}

std::uint64_t DriverClient::GetModuleBase(DWORD pid, const wchar_t* name) {
    ModuleRequest request{version, 0, {}};
    if (!name || wcsncpy_s(request.name, name, _TRUNCATE) != 0) {
        error_ = ERROR_INVALID_PARAMETER;
        return 0;
    }
    ModuleResponse response{};
    if (!Bind(pid) || !Request(moduleCode, &request, sizeof(request), &response,
                               sizeof(response), sizeof(response))) return 0;
    return response.base;
}

DWORD DriverClient::GetPidByName(const wchar_t* name) {
    if (!name) return 0;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD pid = 0;
    for (BOOL found = Process32FirstW(snapshot, &entry); found; found = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, name) == 0) {
            pid = entry.th32ProcessID;
            break;
        }
    }
    CloseHandle(snapshot);
    return pid;
}

