// Pure logic — no Arduino includes. Everything here runs (and is tested) on the host.
#pragma once
#include <cstdint>
#include <cstring>

namespace eden {

// ---------- Dose math ----------

struct DoseParams {
    float steps_per_ml = 0;
    float max_ml_per_dose = 0;
    uint32_t sps = 1000;  // steps per second while dosing
};

struct DosePlan {
    bool ok = false;
    const char* reject_reason = nullptr;  // "bad_json" | "over_cap"
    uint32_t steps = 0;
    uint32_t expected_ms = 0;
    uint32_t watchdog_ms = 0;
};

// Watchdog bound: min(expected * 1.5, max_ms). If max_ms is too small for the dose,
// the dose is allowed to start and the watchdog stops it (ack: failed/watchdog) —
// max_ms is a hard bound by contract, never stretched.
inline DosePlan planDose(const DoseParams& p, float ml, uint32_t max_ms) {
    DosePlan d;
    if (ml <= 0 || p.steps_per_ml <= 0 || p.sps == 0) {
        d.reject_reason = "bad_json";
        return d;
    }
    if (ml > p.max_ml_per_dose) {
        d.reject_reason = "over_cap";
        return d;
    }
    d.steps = static_cast<uint32_t>(ml * p.steps_per_ml + 0.5f);
    d.expected_ms = static_cast<uint32_t>(static_cast<uint64_t>(d.steps) * 1000u / p.sps);
    uint64_t w = d.expected_ms + d.expected_ms / 2;
    if (max_ms > 0 && w > max_ms) w = max_ms;
    if (w == 0) w = 1;
    d.watchdog_ms = static_cast<uint32_t>(w);
    d.ok = true;
    return d;
}

// ---------- Moisture mapping ----------

// Map raw ADC to percent using per-probe calibration; clamped to [0, 100].
// dry_raw > wet_raw for capacitive probes (drier = higher count).
inline float moisturePct(int raw, int dry_raw, int wet_raw) {
    if (dry_raw == wet_raw) return 0;
    float pct = 100.0f * (dry_raw - raw) / static_cast<float>(dry_raw - wet_raw);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

// ---------- Valve state machine ----------

enum class ValveState : uint8_t { Unhomed, Homing, AtPort, Moving };

// Shortest signed port delta (forward positive), for minimal valve travel.
inline int shortestPortDelta(int cur, int target, int num_ports) {
    int d = (target - cur) % num_ports;
    if (d < 0) d += num_ports;
    if (d > num_ports / 2) d -= num_ports;
    return d;
}

class ValveSm {
public:
    ValveState state() const { return state_; }
    int port() const { return port_; }

    // nullptr on success, else a contract failure reason.
    const char* requestHome(bool pumps_idle) {
        if (!pumps_idle) return "busy";
        if (state_ == ValveState::Homing || state_ == ValveState::Moving) return "busy";
        state_ = ValveState::Homing;
        return nullptr;
    }

    void homeDone() { state_ = ValveState::AtPort; port_ = 0; }
    void homeFailed() { state_ = ValveState::Unhomed; port_ = -1; }

    const char* requestGoto(int target, int num_ports, bool pumps_idle) {
        if (target < 0 || target >= num_ports) return "bad_json";
        if (!pumps_idle) return "busy";
        if (state_ == ValveState::Homing || state_ == ValveState::Moving) return "busy";
        if (state_ == ValveState::Unhomed) return "unhomed";
        pending_port_ = target;
        state_ = ValveState::Moving;
        return nullptr;
    }

    void moveDone() { state_ = ValveState::AtPort; port_ = pending_port_; }
    // A fault mid-move (watchdog, forced stop) loses position trust entirely.
    void moveFailed() { state_ = ValveState::Unhomed; port_ = -1; }

    // Interlock: pump may run only with the valve settled at the commanded port.
    const char* checkDoseAllowed(bool has_valve, int cmd_port) const {
        if (!has_valve) return nullptr;
        if (state_ == ValveState::Unhomed || state_ == ValveState::Homing) return "unhomed";
        if (state_ == ValveState::Moving) return "busy";
        if (cmd_port != port_) return "wrong_port";
        return nullptr;
    }

private:
    ValveState state_ = ValveState::Unhomed;
    int port_ = -1;
    int pending_port_ = -1;
};

// ---------- Command dedupe ----------

// QoS 1 can redeliver: remember the last-seen command id per channel and drop repeats.
class CmdDedupe {
public:
    static constexpr size_t kMaxIdLen = 40;

    // Returns true if this id was already processed (caller should drop it).
    bool seenBefore(const char* id) {
        if (!id || !id[0]) return false;
        if (std::strncmp(id, last_, kMaxIdLen) == 0) return true;
        std::strncpy(last_, id, kMaxIdLen - 1);
        last_[kMaxIdLen - 1] = '\0';
        return false;
    }

private:
    char last_[kMaxIdLen] = {0};
};

}  // namespace eden
