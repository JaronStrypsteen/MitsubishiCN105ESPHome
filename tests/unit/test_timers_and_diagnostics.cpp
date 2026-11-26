/// test_timers_and_diagnostics.cpp — Tests for 0x05 timer decoding, filter status, and optimized lookups
#include <gtest/gtest.h>
#include "esphome_stubs.h"
#include "cn105_types.h"
#include "cn105_protocol.h"

TEST(TimerPacketTest, DecodesTimerModeCorrectly) {
    // 0x00: NONE, 0x01: OFF, 0x02: ON, 0x03: BOTH
    EXPECT_STREQ(TIMER_MODE_MAP[0], "NONE");
    EXPECT_STREQ(TIMER_MODE_MAP[1], "OFF");
    EXPECT_STREQ(TIMER_MODE_MAP[2], "ON");
    EXPECT_STREQ(TIMER_MODE_MAP[3], "BOTH");
}

TEST(TimerPacketTest, CalculatesMinutesIncrement) {
    uint8_t raw_on = 6;   // 60 minutes
    uint8_t raw_off = 12; // 120 minutes

    EXPECT_EQ(raw_on * TIMER_INCREMENT_MINUTES, 60);
    EXPECT_EQ(raw_off * TIMER_INCREMENT_MINUTES, 120);
}

TEST(FilterPacketTest, DecodesFilterSignFlag) {
    uint8_t payload_clean = 0x00;
    uint8_t payload_dirty = 0x01;
    uint8_t payload_with_other_bits = 0xF5; // bit 0 is 1

    EXPECT_FALSE((payload_clean & 0x01) != 0);
    EXPECT_TRUE((payload_dirty & 0x01) != 0);
    EXPECT_TRUE((payload_with_other_bits & 0x01) != 0);
}

TEST(OptimizedLookupTest, PointerFastPathMatch) {
    // Passing literal pointer directly from MODE_MAP
    const char* ptr = MODE_MAP[2]; // "COOL"
    int idx = cn105_protocol::lookup_index(MODE_MAP, 5, ptr);
    EXPECT_EQ(idx, 2);

    // Case-insensitive fallback
    int idx_lower = cn105_protocol::lookup_index(MODE_MAP, 5, "cool");
    EXPECT_EQ(idx_lower, 2);

    // Null pointer safely handled
    int idx_null = cn105_protocol::lookup_index(MODE_MAP, 5, nullptr);
    EXPECT_EQ(idx_null, -1);
}

TEST(OptimizedLookupTest, StdOptionalLookupFastPath) {
    auto opt = cn105_protocol::lookup_index_opt(POWER_MAP, 2, POWER_MAP[1]);
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(*opt, 1);

    auto opt_null = cn105_protocol::lookup_index_opt(POWER_MAP, 2, nullptr);
    EXPECT_FALSE(opt_null.has_value());
}
