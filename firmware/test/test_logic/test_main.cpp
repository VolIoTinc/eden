#include <unity.h>
#include "eden_logic.h"

using namespace eden;

// ---------- planDose ----------

static void test_dose_basic() {
    DoseParams p{480.0f, 200.0f, 2000};
    DosePlan d = planDose(p, 50.0f, 45000);
    TEST_ASSERT_TRUE(d.ok);
    TEST_ASSERT_EQUAL_UINT32(24000, d.steps);          // 50 * 480
    TEST_ASSERT_EQUAL_UINT32(12000, d.expected_ms);    // 24000 / 2000 * 1000
    TEST_ASSERT_EQUAL_UINT32(18000, d.watchdog_ms);    // 1.5x, under max_ms
}

static void test_dose_watchdog_capped_by_max_ms() {
    DoseParams p{480.0f, 200.0f, 2000};
    DosePlan d = planDose(p, 100.0f, 20000);           // expected 24 s > max 20 s
    TEST_ASSERT_TRUE(d.ok);
    TEST_ASSERT_EQUAL_UINT32(20000, d.watchdog_ms);    // hard bound wins
}

static void test_dose_over_cap_rejected() {
    DoseParams p{480.0f, 200.0f, 2000};
    DosePlan d = planDose(p, 200.1f, 45000);
    TEST_ASSERT_FALSE(d.ok);
    TEST_ASSERT_EQUAL_STRING("over_cap", d.reject_reason);
}

static void test_dose_garbage_rejected() {
    DoseParams p{480.0f, 200.0f, 2000};
    TEST_ASSERT_EQUAL_STRING("bad_json", planDose(p, 0, 1000).reject_reason);
    TEST_ASSERT_EQUAL_STRING("bad_json", planDose(p, -5, 1000).reject_reason);
    DoseParams uncal{0, 200.0f, 2000};
    TEST_ASSERT_EQUAL_STRING("bad_json", planDose(uncal, 50, 1000).reject_reason);
}

// ---------- moisturePct ----------

static void test_moisture_mapping() {
    TEST_ASSERT_FLOAT_WITHIN(0.1, 0.0, moisturePct(2800, 2800, 1100));
    TEST_ASSERT_FLOAT_WITHIN(0.1, 100.0, moisturePct(1100, 2800, 1100));
    TEST_ASSERT_FLOAT_WITHIN(0.1, 50.0, moisturePct(1950, 2800, 1100));
    TEST_ASSERT_FLOAT_WITHIN(0.1, 100.0, moisturePct(500, 2800, 1100));   // clamp high
    TEST_ASSERT_FLOAT_WITHIN(0.1, 0.0, moisturePct(3500, 2800, 1100));    // clamp low
    TEST_ASSERT_FLOAT_WITHIN(0.1, 0.0, moisturePct(1000, 1000, 1000));    // degenerate cal
}

// ---------- shortestPortDelta ----------

static void test_shortest_port_delta() {
    TEST_ASSERT_EQUAL_INT(1, shortestPortDelta(0, 1, 8));
    TEST_ASSERT_EQUAL_INT(-1, shortestPortDelta(0, 7, 8));   // backward is shorter
    TEST_ASSERT_EQUAL_INT(4, shortestPortDelta(0, 4, 8));    // tie goes forward
    TEST_ASSERT_EQUAL_INT(0, shortestPortDelta(3, 3, 8));
    TEST_ASSERT_EQUAL_INT(-2, shortestPortDelta(1, 7, 8));
}

// ---------- ValveSm ----------

static void test_valve_boots_unhomed_and_rejects() {
    ValveSm v;
    TEST_ASSERT_EQUAL(ValveState::Unhomed, v.state());
    TEST_ASSERT_EQUAL_STRING("unhomed", v.checkDoseAllowed(true, 0));
    TEST_ASSERT_EQUAL_STRING("unhomed", v.requestGoto(2, 8, true));
}

static void test_valve_home_then_goto_then_dose() {
    ValveSm v;
    TEST_ASSERT_NULL(v.requestHome(true));
    TEST_ASSERT_EQUAL(ValveState::Homing, v.state());
    TEST_ASSERT_EQUAL_STRING("unhomed", v.checkDoseAllowed(true, 0));  // mid-home: no dosing
    v.homeDone();
    TEST_ASSERT_EQUAL(ValveState::AtPort, v.state());
    TEST_ASSERT_EQUAL_INT(0, v.port());

    TEST_ASSERT_NULL(v.requestGoto(3, 8, true));
    TEST_ASSERT_EQUAL_STRING("busy", v.checkDoseAllowed(true, 3));     // mid-travel: no dosing
    v.moveDone();
    TEST_ASSERT_EQUAL_INT(3, v.port());
    TEST_ASSERT_NULL(v.checkDoseAllowed(true, 3));
    TEST_ASSERT_EQUAL_STRING("wrong_port", v.checkDoseAllowed(true, 5));
}

static void test_valve_guards() {
    ValveSm v;
    TEST_ASSERT_EQUAL_STRING("busy", v.requestHome(false));            // pumps running
    TEST_ASSERT_NULL(v.requestHome(true));
    v.homeDone();
    TEST_ASSERT_EQUAL_STRING("busy", v.requestGoto(1, 8, false));      // pumps running
    TEST_ASSERT_EQUAL_STRING("bad_json", v.requestGoto(8, 8, true));   // port out of range
    TEST_ASSERT_EQUAL_STRING("bad_json", v.requestGoto(-1, 8, true));
}

static void test_valve_fault_loses_position_trust() {
    ValveSm v;
    v.requestHome(true);
    v.homeDone();
    v.requestGoto(2, 8, true);
    v.moveFailed();
    TEST_ASSERT_EQUAL(ValveState::Unhomed, v.state());
    TEST_ASSERT_EQUAL_STRING("unhomed", v.checkDoseAllowed(true, 2));
}

static void test_valveless_node_always_allows() {
    ValveSm v;
    TEST_ASSERT_NULL(v.checkDoseAllowed(false, 0));
}

// ---------- CmdDedupe ----------

static void test_dedupe() {
    CmdDedupe d;
    TEST_ASSERT_FALSE(d.seenBefore("d-20260810-0001"));
    TEST_ASSERT_TRUE(d.seenBefore("d-20260810-0001"));    // QoS1 redelivery dropped
    TEST_ASSERT_FALSE(d.seenBefore("d-20260810-0002"));
    TEST_ASSERT_FALSE(d.seenBefore(""));                  // empty id never dedupes
    TEST_ASSERT_FALSE(d.seenBefore(nullptr));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_dose_basic);
    RUN_TEST(test_dose_watchdog_capped_by_max_ms);
    RUN_TEST(test_dose_over_cap_rejected);
    RUN_TEST(test_dose_garbage_rejected);
    RUN_TEST(test_moisture_mapping);
    RUN_TEST(test_shortest_port_delta);
    RUN_TEST(test_valve_boots_unhomed_and_rejects);
    RUN_TEST(test_valve_home_then_goto_then_dose);
    RUN_TEST(test_valve_guards);
    RUN_TEST(test_valve_fault_loses_position_trust);
    RUN_TEST(test_valveless_node_always_allows);
    RUN_TEST(test_dedupe);
    return UNITY_END();
}
