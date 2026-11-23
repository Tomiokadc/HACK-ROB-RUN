#pragma once
#include "../driver_client/driver_client.hpp"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// Silent aim for ROB RUN (Unity 6000.3.18f1, il2cpp).
// Read side: all offsets below are taken from the 2026-11-17 dump (dump.txt) unless marked FILL.
// Write side: the local player's look direction is overwritten only while the fire key is held,
// and restored the frame the key is released. The visible camera is never touched.
// Positions are assumed to be metres, Unity left-handed space (forward = +Z).

namespace silent_off {
    // GameAssembly.dll static root for CinemachineCore (see main.cpp)
    constexpr std::uint64_t CinemachineKlassRva = 0x8162748;

    // BattleEntitiesModel
    constexpr std::uint64_t EntitiesPlayer      = 0xD0;   // PlayerEntityModel
    constexpr std::uint64_t EntitiesModels      = 0x50;   // Dictionary<int, IEntityModel>

    // PlayerEntityModel
    constexpr std::uint64_t PlayerServerModel   = 0x20;   // BattleCharacterModel

    // BattleCharacterModel
    constexpr std::uint64_t CharLastPosition    = 0x68;   // FILL: old build value, not in 2026-11-17 dump, re-verify
    constexpr std::uint64_t CharLookDirection   = 0x70;   // FILL: old build value, not in 2026-11-17 dump, re-verify

    // LastPositionModel (floats)
    constexpr std::uint64_t PosX = 0x10, PosY = 0x14, PosZ = 0x18;

    // CinemachineBrain (dump.txt section 7)
    constexpr std::uint64_t BrainRawPosition    = 0xFC;   // Vector3
    constexpr std::uint64_t BrainRawOrientation = 0x108;  // Quaternion xyzw
    constexpr std::uint64_t BrainFov            = 0xB0;   // float, vertical degrees

    // Dictionary<int,V> (dump.txt section 14 layout)
    constexpr std::uint64_t DictEntries = 0x18;
    constexpr std::uint64_t DictCount   = 0x20;
    constexpr std::uint64_t EntrySize   = 24;
    constexpr std::uint64_t EntryNext   = 4;
    constexpr std::uint64_t EntryValue  = 0x10;
    // il2cpp array
    constexpr std::uint64_t ArrayMaxLen = 0x18;
    constexpr std::uint64_t ArrayData   = 0x20;
}

struct SilentAimConfig {
    bool  enabled     = false;
    float fovDeg      = 3.0f;     // max angle from view axis
    float maxDistance = 250.0f;   // metres
    float headHeight  = 1.6f;     // added to LastPosition for the aim point (tunable)
};

class SilentAim {
public:
    SilentAim(DriverClient& driver, DWORD pid) : driver_(driver), pid_(pid) {}

    // entities = BattleEntitiesModel*, brain = CinemachineBrain* (both resolved by the caller)
    void Tick(std::uint64_t entities, std::uint64_t brain, bool fireHeld, const SilentAimConfig& cfg);

    // Unit aim vector (Unity space) toward the current target from the last Tick.
    // Returns false when there is no valid target this frame.
    bool GetAimVector(float out[3]) const {
        if (!hasAim_) return false;
        out[0] = aim_.x; out[1] = aim_.y; out[2] = aim_.z;
        return true;
    }

private:
    struct V3 { float x, y, z; };
    V3 aim_{};
    bool hasAim_ = false;

    std::uint64_t Ptr(std::uint64_t addr) {
        std::uint64_t v{};
        if (!driver_.Read(pid_, addr, &v, sizeof(v))) return 0;
        return (v < 0x10000 || v > 0x7FFFFFFFFFFF) ? 0 : v;
    }
    bool ReadF(std::uint64_t addr, void* out, std::size_t n) { return driver_.Read(pid_, addr, out, n); }
    bool ReadV3(std::uint64_t addr, V3& out) { return ReadF(addr, &out, sizeof(out)); }

    static V3 Sub(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    static float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    static V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
    static V3 Scale(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
    static V3 Add(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    static V3 Norm(V3 a) {
        const float l = std::sqrt(Dot(a, a));
        return l > 1e-6f ? Scale(a, 1.0f / l) : V3{ 0, 0, 1 };
    }
    // Rotate v by unit quaternion q = (x,y,z,w)
    static V3 Rotate(const float q[4], V3 v) {
        V3 qv{ q[0], q[1], q[2] };
        V3 t = Scale(Cross(qv, v), 2.0f);
        return Add(Add(v, Scale(t, q[3])), Cross(qv, t));
    }

    void Restore();

    DriverClient& driver_;
    DWORD pid_;
    bool armed_ = false;
    std::uint64_t localBcm_ = 0;
    float saved_[3]{};
};

inline void SilentAim::Restore() {
    if (!armed_ || !localBcm_) { armed_ = false; return; }
    driver_.Write(pid_, localBcm_ + silent_off::CharLookDirection, saved_, sizeof(saved_));
    armed_ = false;
}

inline void SilentAim::Tick(std::uint64_t entities, std::uint64_t brain, bool fireHeld, const SilentAimConfig& cfg) {
    using namespace silent_off;
    hasAim_ = false;
    if (!cfg.enabled || !entities || !brain) { Restore(); return; }

    // camera pose from CinemachineBrain
    float fov = 0.0f, q[4]{};
    V3 camPos{};
    if (!ReadF(brain + BrainFov, &fov, sizeof(fov)) || fov <= 0.0f || fov >= 179.0f) { Restore(); return; }
    if (!ReadV3(brain + BrainRawPosition, camPos) || !ReadF(brain + BrainRawOrientation, q, sizeof(q))) { Restore(); return; }
    const V3 fwd = Rotate(q, { 0, 0, 1 });
    const V3 right = Rotate(q, { 1, 0, 0 });
    const V3 up = Rotate(q, { 0, 1, 0 });

    // local player character
    const auto player = Ptr(entities + EntitiesPlayer);
    const auto local = player ? Ptr(player + PlayerServerModel) : 0;
    if (!local) { Restore(); return; }
    localBcm_ = local;

    // enumerate Dictionary<int, IEntityModel> entries
    const auto dict = Ptr(entities + EntitiesModels);
    const auto entries = dict ? Ptr(dict + DictEntries) : 0;
    if (!entries) { Restore(); return; }
    std::int32_t maxLen = 0;
    if (!ReadF(entries + ArrayMaxLen, &maxLen, sizeof(maxLen)) || maxLen <= 0 || maxLen > 4096) { Restore(); return; }
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(maxLen) * EntrySize);
    if (!ReadF(entries + ArrayData, buf.data(), buf.size())) { Restore(); return; }

    bool found = false;
    V3 bestAim{};
    float bestAngle = cfg.fovDeg;
    for (std::int32_t i = 0; i < maxLen; ++i) {
        const std::uint8_t* e = buf.data() + static_cast<std::size_t>(i) * EntrySize;
        std::int32_t next{};
        std::memcpy(&next, e + EntryNext, sizeof(next));
        if (next < -1) continue; // free slot

        std::uint64_t value{};
        std::memcpy(&value, e + EntryValue, sizeof(value));
        const auto server = (value && value < 0x7FFFFFFFFFFF) ? Ptr(value + PlayerServerModel) : 0;
        if (!server || server == local) continue;

        const auto lastPos = Ptr(server + CharLastPosition);
        V3 pos{};
        if (!lastPos || !ReadF(lastPos + PosX, &pos.x, 4) || !ReadF(lastPos + PosY, &pos.y, 4) || !ReadF(lastPos + PosZ, &pos.z, 4)) continue;

        const V3 aimPoint = Add(pos, { 0, cfg.headHeight, 0 });
        const V3 d = Sub(aimPoint, camPos);
        const float dist = std::sqrt(Dot(d, d));
        if (dist < 0.5f || dist > cfg.maxDistance) continue;

        const float z = Dot(d, fwd);
        if (z <= 0.1f) continue; // behind camera

        // angle off the view axis, degrees
        const float lateral = std::sqrt(Dot(d, right) * Dot(d, right) + Dot(d, up) * Dot(d, up));
        const float angle = std::atan2(lateral, z) * 57.29578f;
        if (angle < bestAngle) {
            bestAngle = angle;
            bestAim = aimPoint;
            found = true;
        }
    }

    if (!found) { Restore(); return; }

    // aim vector from camera to target, unit length
    const V3 dirN = Norm(Sub(bestAim, camPos));
    aim_ = dirN;
    hasAim_ = true;

    if (!fireHeld) { Restore(); return; }

    // write the look direction toward the chosen target
    float dir[3] = { dirN.x, dirN.y, dirN.z };
    if (!armed_) {
        if (!ReadF(local + CharLookDirection, saved_, sizeof(saved_))) return;
        armed_ = true;
    }
    driver_.Write(pid_, local + CharLookDirection, dir, sizeof(dir));
}
