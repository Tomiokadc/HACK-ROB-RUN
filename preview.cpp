#include <Windows.h>
#include "dx11_renderer.hpp"
#include <cmath>
#include <cwchar>
#include <cstdio>
#include <algorithm>
#include <vector>
#include <array>
#include <cstdlib>
#include <map>
#include <thread>
#include <mutex>
#include <chrono>
#include <string>
#include "../game/model_search.hpp"

namespace {
constexpr wchar_t windowClass[] = L"RobRunEspPreview";

struct Vec3 { float x, y, z; };
struct Entity { Vec3 feet; int hp; const wchar_t* name; };

struct Quaternion { float x, y, z, w; };
struct Camera { Vec3 position; Quaternion rotation; float fov; };
Camera camera{};
struct Player { Vec3 position; std::wstring nickname; int id; Vec3 previous{}; };
std::vector<Player> players;
ULONGLONG snapshotTime{};
Vec3 marker{};
bool cameraReady{};
bool markerPlaced{};

Vec3 Add(Vec3 a, Vec3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
Vec3 Scale(Vec3 v, float k) { return {v.x*k, v.y*k, v.z*k}; }
Vec3 Cross(Vec3 a, Vec3 b) {
    return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
}
Vec3 Rotate(Quaternion q, Vec3 v) {
    const Vec3 axis{q.x,q.y,q.z};
    const Vec3 twice = Scale(Cross(axis,v),2.f);
    return Add(Add(v,Scale(twice,q.w)),Cross(axis,twice));
}

bool WorldToScreen(Vec3 point, int width, int height, POINT& screen) {
    const Quaternion q = camera.rotation;
    point = Rotate({-q.x,-q.y,-q.z,q.w}, Add(point, Scale(camera.position,-1.f)));
    if (point.z <= 0.05f || width <= 0 || height <= 0) return false;
    const float focal = height * 0.5f / std::tan(camera.fov * 0.00872664626f);
    const float x = width * 0.5f + point.x * focal / point.z;
    const float y = height * 0.5f - point.y * focal / point.z;
    if (!std::isfinite(x) || !std::isfinite(y) ||
        std::abs(x) > 100000.f || std::abs(y) > 100000.f) return false;
    screen = {std::lround(x),std::lround(y)};
    return true;
}

bool ReadCamera(DriverClient& driver, DWORD pid, std::uint64_t base, Camera& result) {
    const auto ptr = [&](std::uint64_t address) {
        std::uint64_t value{};
        if (!driver.Read(pid,address,&value,sizeof(value)) ||
            value < 0x10000 || value > 0x7FFFFFFFFFFF) return std::uint64_t{};
        return value;
    };
    const auto klass = ptr(base + 0x8162748);
    const auto fields = klass ? ptr(klass + 0xB8) : 0;
    const auto core = fields ? ptr(fields + 8) : 0;
    const auto brains = core ? ptr(core + 0x10) : 0;
    int count{};
    // Камера проверена только для случая с одним активным Brain.
    if (!brains || !driver.Read(pid,brains+0x18,&count,sizeof(count)) || count != 1) return false;
    const auto items = ptr(brains+0x10);
    const auto brain = items ? ptr(items+0x20) : 0;
    struct State {
        float fov;
        std::byte beforePosition[0x48];
        Vec3 position;
        Quaternion rotation;
        std::byte beforeCorrection[0x10];
        Vec3 positionCorrection;
        Quaternion orientationCorrection;
    } state{};
    static_assert(offsetof(State,orientationCorrection) == 0x84);
    if (!brain || !driver.Read(pid,brain+0xB0,&state,sizeof(state)) ||
        !(state.fov > 1.f && state.fov < 179.f)) return false;
    const auto a = state.rotation;
    const auto b = state.orientationCorrection;
    Quaternion q{
        a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,
        a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
        a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,
        a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z
    };
    const float length = std::sqrt(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w);
    if (!(length > 0.5f && length < 1.5f)) return false;
    const auto position = Add(state.position,state.positionCorrection);
    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z)) return false;
    result = {position,{q.x/length,q.y/length,q.z/length,q.w/length},state.fov};
    return true;
}

template<class T>
bool ReadValue(DriverClient& driver, DWORD pid, std::uint64_t address, T& value) {
    return address >= 0x10000 && address <= 0x7FFFFFFFFFFF &&
           driver.Read(pid, address, &value, sizeof(value));
}

struct DictionaryEntry { int hash; int next; int key; int padding; std::uint64_t value; };
static_assert(sizeof(DictionaryEntry) == 24);

template<class ReadPointer>
void RefreshNames(DriverClient& driver, DWORD pid, std::uint64_t entities,
                  ReadPointer ptr, std::map<int, std::wstring>& names) {
    names.clear();
    const auto slots = ptr(entities + 0x80);
    if (!slots) return;
    const auto dictionary = ptr(slots + 0x18);
    if (!dictionary) return;
    const auto entries = ptr(dictionary + 0x18);
    int count{};
    std::uint64_t capacity{};
    if (!entries || !ReadValue(driver, pid, dictionary + 0x20, count) ||
        !ReadValue(driver, pid, entries + 0x18, capacity) ||
        count <= 0 || count > 4096 || static_cast<std::uint64_t>(count) > capacity) return;

    std::vector<DictionaryEntry> snapshot(count);
    if (!driver.Read(pid, entries + 0x20, snapshot.data(), snapshot.size() * sizeof(DictionaryEntry))) return;
    for (const auto& entry : snapshot) {
        if (entry.hash < 0 || !entry.value) continue;
        const auto data = ptr(entry.value + 0x10);
        const auto nickname = data ? ptr(data + 0x30) : 0;
        const auto string = nickname ? ptr(nickname + 0x18) : 0;
        int length{};
        if (!string || !ReadValue(driver, pid, string + 0x10, length) ||
            length <= 0 || length > 64) continue;
        std::wstring value(length, L'\0');
        if (driver.Read(pid, string + 0x14, value.data(), value.size() * sizeof(wchar_t)) &&
            std::none_of(value.begin(), value.end(), [](wchar_t ch) { return ch < L' '; })) {
            names.emplace(entry.key, std::move(value));
        }
    }
}

bool ReadPlayers(DriverClient& driver, DWORD pid, std::uint64_t base,
                 std::uint64_t model, std::vector<Player>& output) {
    output.clear();
    const auto ptr = [&](std::uint64_t address) {
        std::uint64_t value{};
        return ReadValue(driver, pid, address, value) &&
               value >= 0x10000 && value <= 0x7FFFFFFFFFFF ? value : 0;
    };
    const auto modelClass = ptr(base + 0x81465C0);
    if (!modelClass || ptr(model) != modelClass) return false;
    const auto battle = ptr(model + 0x10);
    const auto entities = ptr(battle + 0x90);
    const auto local = ptr(entities + 0xD0);
    const auto dict = ptr(entities + 0x50);
    const auto entries = ptr(dict + 0x18);
    const auto characterClass = ptr(base + 0x81CDD90);
    if (!local || !entries || !characterClass) return false;

    static std::map<int, std::wstring> names;
    static std::uint64_t previousEntities{};
    static ULONGLONG nextNames{};
    const auto now = GetTickCount64();
    if (entities != previousEntities || now >= nextNames) {
        RefreshNames(driver, pid, entities, ptr, names);
        previousEntities = entities;
        nextNames = now + 1000;
    }
    std::int32_t slots{};
    std::uint64_t capacity{};
    if (!ReadValue(driver, pid, dict + 0x20, slots) ||
        !ReadValue(driver, pid, entries + 0x18, capacity) ||
        slots <= 0 || slots > 4096 || static_cast<std::uint64_t>(slots) > capacity) return false;
    std::vector<DictionaryEntry> snapshot(static_cast<std::size_t>(slots));
    if (!driver.Read(pid, entries + 0x20, snapshot.data(), snapshot.size() * sizeof(DictionaryEntry))) return false;

    for (const auto& entry : snapshot) {
        if (entry.hash < 0 || !entry.value || entry.value == local) continue;
        std::array<std::uint64_t, 7> fields{};
        if (!ReadValue(driver, pid, entry.value, fields) || fields[0] != characterClass) continue;
        const auto character = fields[6] ? fields[6] : fields[5];
        const auto transform = character ? ptr(character + 0x58) : 0;
        if (!transform) continue;
        Vec3 point{};
        if (ReadValue(driver, pid, transform + 0x10, point) &&
            std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z) &&
            (point.x != 0.f || point.y != 0.f || point.z != 0.f)) {
            const auto name = names.find(entry.key);
            output.push_back({point, name != names.end() ? name->second : L"Player", entry.key, point});
        }
    }
    return true;
}

void DrawEntity(FrameBuffer& buffer, const RECT& viewport, const Entity& entity) {
    const Vec3 offset = Add(entity.feet, Scale(camera.position, -1.f));
    if (offset.x*offset.x + offset.y*offset.y + offset.z*offset.z > buffer.maxDistance * buffer.maxDistance) return;
    POINT feet{}, head{};
    const Vec3 top{entity.feet.x, entity.feet.y + 1.8f, entity.feet.z};
    if (!WorldToScreen(entity.feet, viewport.right, viewport.bottom, feet) ||
        !WorldToScreen(top, viewport.right, viewport.bottom, head)) return;
    const float height = static_cast<float>(feet.y - head.y);
    if (height < 4) return;
    const float left = head.x - height / 4;
    const float topY = static_cast<float>(head.y);
    buffer.brush->SetColor(D2D1::ColorF(0xE6EFF4));
    buffer.dc->DrawRectangle(D2D1::RectF(left, topY, left + height / 2, static_cast<float>(feet.y)),
                             buffer.brush.Get());
    wchar_t label[96];
    swprintf_s(label, L"%ls | %.1f m", entity.name,
               std::sqrt(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z));
    buffer.Text(label, left, topY - buffer.font->GetFontSize() - 4);
}

void PassCursorToGame(HWND overlay, POINT origin, RECT viewport) {
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;
    cursor.x -= origin.x;
    cursor.y -= origin.y;
    static POINT previous{LONG_MIN, LONG_MIN};
    static RECT previousViewport{};
    if (cursor.x == previous.x && cursor.y == previous.y &&
        EqualRect(&viewport, &previousViewport)) return;

    // An empty spot in the window region is excluded from cross-process hit testing.
    HRGN windowRegion = CreateRectRgn(0, 0, viewport.right, viewport.bottom);
    if (!windowRegion) return;
    HRGN cursorRegion = CreateRectRgn(cursor.x - 2, cursor.y - 2, cursor.x + 3, cursor.y + 3);
    const bool combined = cursorRegion &&
        CombineRgn(windowRegion, windowRegion, cursorRegion, RGN_DIFF) != ERROR;
    if (cursorRegion) DeleteObject(cursorRegion);
    if (combined && SetWindowRgn(overlay, windowRegion, TRUE)) {
        previous = cursor;
        previousViewport = viewport;
    } else {
        DeleteObject(windowRegion);
    }
}

bool Present(FrameBuffer& buffer, HWND overlay, POINT origin, RECT viewport) {
    if (!buffer.Resize(viewport.right, viewport.bottom)) return false;
    RECT bounds{};
    GetWindowRect(overlay, &bounds);
    if (!IsWindowVisible(overlay) || bounds.left != origin.x || bounds.top != origin.y ||
        bounds.right - bounds.left != viewport.right || bounds.bottom - bounds.top != viewport.bottom) {
        if (!SetWindowPos(overlay, HWND_TOPMOST, origin.x, origin.y, viewport.right, viewport.bottom,
                          SWP_NOACTIVATE | SWP_SHOWWINDOW)) return false;
    }
    PassCursorToGame(overlay, origin, viewport);
    buffer.dc->BeginDraw();
    buffer.dc->Clear(D2D1::ColorF(0, 0.f));
    wchar_t status[80];
    swprintf_s(status, L"Camera %ls | Players: %zu", cameraReady ? L"OK" : L"waiting", players.size());
    buffer.Text(status, 24, 24);
    if (cameraReady) {
        const float fraction = (std::min)(1.f, static_cast<float>(GetTickCount64() - snapshotTime) / 25.f);
        for (const auto& player : players) {
            const auto point = Add(Scale(player.previous, 1.f - fraction), Scale(player.position, fraction));
            DrawEntity(buffer, viewport, {point, -1, player.nickname.c_str()});
        }
    }
    const auto submit = std::chrono::steady_clock::now();
    const auto drawResult = buffer.dc->EndDraw();
    const auto presentResult = SUCCEEDED(drawResult) ? buffer.swap->Present(1, 0) : drawResult;
    buffer.submitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - submit).count();
    return presentResult == S_OK;
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_SETCURSOR) {
        SetCursor(nullptr);
        return TRUE;
    }
    if ((message == WM_HOTKEY && wParam == 1) || message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_HOTKEY && wParam == 2) { markerPlaced = false; return 0; }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR arguments, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (wcscmp(arguments, L"--render-check") == 0) {
        const HWND check = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, L"STATIC",
            L"DX11 check", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
        if (!check) return 10;
        int result = 11;
        {
            FrameBuffer buffer;
            if (buffer.Init(check)) {
                result = Present(buffer, check, {32, 32}, {0, 0, 360, 90}) &&
                         Present(buffer, check, {32, 32}, {0, 0, 480, 120}) ? 0 : 12;
            }
        }
        DestroyWindow(check);
        return result;
    }

    DriverClient driver;
    if (!driver.IsOpen()) {
        MessageBoxW(nullptr, L"Load RobRunProbe.sys and run this client as administrator.", L"ROB RUN ESP", MB_ICONERROR);
        return 5;
    }
    const HWND game = FindWindowW(nullptr, L"ROB RUN");
    if (!game) { MessageBoxW(nullptr, L"Start ROB RUN first.", L"ROB RUN ESP", MB_ICONINFORMATION); return 1; }
    DWORD pid{};
    GetWindowThreadProcessId(game, &pid);
    const auto base = driver.GetModuleBase(pid,L"GameAssembly.dll");
    if (!base) { MessageBoxW(nullptr, L"Could not read GameAssembly.dll through RobRunProbe.", L"ROB RUN ESP", MB_ICONERROR); return 5; }

    WNDCLASSW cls{};
    cls.lpfnWndProc = WindowProc;
    cls.hInstance = instance;
    cls.lpszClassName = windowClass;
    if (!RegisterClassW(&cls)) return 2;

    const HWND overlay = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        windowClass, L"ROB RUN ESP Preview", WS_POPUP,
        0, 0, 0, 0, game, nullptr, instance, nullptr);
    if (!overlay) return 3;
    if (
        !RegisterHotKey(overlay, 1, MOD_NOREPEAT, VK_F10)) {
        DestroyWindow(overlay);
        return 4;
    }

    FrameBuffer buffer;
    if (!buffer.Init(overlay)) { MessageBoxW(nullptr, L"DX11 initialization failed.", L"ROB RUN ESP", MB_ICONERROR); return 6; }
    struct Snapshot {
        Camera camera{};
        bool ready{};
        std::vector<Player> players;
        ULONGLONG time{};
        double readMs{};
    } shared;
    std::mutex snapshotMutex;
    std::jthread reader([&](std::stop_token stop) {
        std::uint64_t model{};
        ULONGLONG retry{};
        Snapshot sample;
        while (!stop.stop_requested() && IsWindow(game)) {
            const auto started = std::chrono::steady_clock::now();
            const auto now = GetTickCount64();
            sample.ready = !IsIconic(game) && ReadCamera(driver, pid, base, sample.camera);
            sample.players.clear();
            if (sample.ready) {
                if (!model && now >= retry) {
                    model = FindGameModel(driver, pid, base);
                    retry = GetTickCount64() + 5000;
                }
                if (model && !ReadPlayers(driver, pid, base, model, sample.players)) model = 0;
            } else if (!IsIconic(game)) {
                model = 0;
            }
            sample.time = GetTickCount64();
            sample.readMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            {
                std::scoped_lock lock(snapshotMutex);
                for (auto& player : sample.players) {
                    const auto previous = std::find_if(shared.players.begin(), shared.players.end(),
                        [&](const Player& old) { return old.id == player.id; });
                    if (previous != shared.players.end()) {
                        const auto distance = Add(player.position, Scale(previous->position, -1.f));
                        if (distance.x*distance.x + distance.y*distance.y + distance.z*distance.z < 25.f)
                            player.previous = previous->position;
                    }
                }
                shared = sample;
            }
            std::this_thread::sleep_until(started + std::chrono::milliseconds(sample.ready ? 25 : 100));
        }
    });
    const HANDLE frameTimer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    struct TimerCleanup {
        HANDLE timer;
        ~TimerCleanup() { if (timer) CloseHandle(timer); }
    } timerCleanup{frameTimer};
    ULONGLONG sampleStart = GetTickCount64();
    unsigned frames = 0;
    unsigned fps = 0;
    RegisterHotKey(overlay, 2, MOD_NOREPEAT, VK_F9);
    while (IsWindow(game)) {
        const auto frameStart = GetTickCount64();
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return 0;
            DispatchMessageW(&message);
        }

        RECT bounds{};
        POINT origin{};
        if (IsWindowVisible(game) && !IsIconic(game) &&
            GetClientRect(game, &bounds) && ClientToScreen(game, &origin)) {
            double readMs{};
            {
                std::scoped_lock lock(snapshotMutex);
                snapshotTime = shared.time;
                cameraReady = shared.ready && GetTickCount64() - shared.time < 250;
                if (cameraReady) {
                    camera = shared.camera;
                    players = shared.players;
                } else {
                    players.clear();
                }
                readMs = shared.readMs;
            }
            const auto now = GetTickCount64();
            const auto drawStart = std::chrono::steady_clock::now();
            const bool presented = Present(buffer, overlay, origin, bounds);
            const double drawMs = (std::max)(0.0, std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - drawStart).count() - buffer.submitMs);
            const BOOL positioned = IsWindowVisible(overlay) || SetWindowPos(overlay, HWND_TOPMOST, origin.x, origin.y, bounds.right, bounds.bottom,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
            const DWORD positionError = positioned ? 0 : GetLastError();

            if (presented) ++frames;
            if (now - sampleStart >= 1000) {
                fps = static_cast<unsigned>(frames * 1000ULL / (now - sampleStart));
                frames = 0;
                sampleStart = now;
                wchar_t path[MAX_PATH]{};
                const DWORD length = GetTempPathW(MAX_PATH, path);
                if (length && length < MAX_PATH && wcscat_s(path, L"robrun-overlay-status.txt") == 0) {
                    FILE* log{};
                    if (_wfopen_s(&log, path, L"w") == 0) {
                        fprintf(log, "game=%p overlay=%p visible=%d bounds=%ld,%ld %ldx%ld positioned=%d error=%lu camera=%d presented=%d players=%zu named=%zu fps=%u font_fallback=%d read_ms=%.3f draw_ms=%.3f submit_ms=%.3f\n",
                                game, overlay, IsWindowVisible(overlay), origin.x, origin.y,
                                bounds.right, bounds.bottom, positioned, positionError, cameraReady, presented, players.size(), static_cast<std::size_t>(std::count_if(players.begin(), players.end(), [](const Player& p) { return p.nickname != L"Player"; })), fps, buffer.fontFallback, readMs, drawMs, buffer.submitMs);
                        fclose(log);
                    }
                }
            }
        } else {
            ShowWindow(overlay, SW_HIDE);
        }
        LARGE_INTEGER delay{};
        const ULONGLONG period = IsWindowVisible(overlay) ? (1000ULL + buffer.maxFps - 1) / buffer.maxFps : 100;
        const auto elapsed = GetTickCount64() - frameStart;
        if (elapsed >= period) continue;
        const auto remaining = period - elapsed;
        delay.QuadPart = -static_cast<LONGLONG>(remaining * 10000);
        if (frameTimer && SetWaitableTimer(frameTimer, &delay, 0, nullptr, nullptr, FALSE)) {
            MsgWaitForMultipleObjects(1, &frameTimer, FALSE, INFINITE, QS_ALLINPUT);
        } else {
            Sleep(static_cast<DWORD>(remaining));
        }
    }
    DestroyWindow(overlay);
    return 0;
}





















