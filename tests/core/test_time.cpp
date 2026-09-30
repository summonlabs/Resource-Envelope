#include "support/testing.hpp"

#include "resource_envelope/time.hpp"

using namespace resource_envelope;

RE_TEST(timestamp_round_trips_exactly) {
  const Timestamp values[] = {0LL, 1LL, 999999999LL, 1000000000LL, 1767225600000000000LL,
                              253402300799999999LL};
  for (const Timestamp value : values) {
    const std::string text = format_timestamp(value);
    Timestamp parsed = -1;
    RE_REQUIRE(parse_timestamp(text, parsed));
    RE_CHECK_EQ(parsed, value);
  }
  RE_CHECK_EQ(format_timestamp(0LL), std::string("1970-01-01T00:00:00.000000000Z"));
  RE_CHECK_EQ(format_timestamp(1767225600000000000LL),
              std::string("2026-01-01T00:00:00.000000000Z"));
  RE_CHECK_EQ(format_timestamp(999999999LL), std::string("1970-01-01T00:00:00.999999999Z"));
}

RE_TEST(timestamp_parsing_rejects_malformed_forms) {
  Timestamp value = 0;
  RE_CHECK(!parse_timestamp("", value));
  RE_CHECK(!parse_timestamp("2026-01-01T00:00:00", value));
  RE_CHECK(!parse_timestamp("2026-13-01T00:00:00Z", value));
  RE_CHECK(!parse_timestamp("2026-02-30T00:00:00Z", value));
  RE_CHECK(!parse_timestamp("2026-01-01T24:00:00Z", value));
  RE_CHECK(!parse_timestamp("2026-01-01T00:00:60Z", value));
  RE_CHECK(!parse_timestamp("2026-01-01T00:00:00.1234567890Z", value));
  RE_CHECK(!parse_timestamp("1969-12-31T23:59:59Z", value));
  RE_CHECK(!parse_timestamp("2026-01-01T00:00:00", value));
  RE_CHECK(!parse_timestamp("2026-01-01 00:00:00Z", value));
}

RE_TEST(timestamp_accepts_numeric_offsets_and_normalises_to_utc) {
  Timestamp value = 0;
  RE_REQUIRE(parse_timestamp("2026-01-01T02:00:00+02:00", value));
  RE_CHECK_EQ(value, 1767225600000000000LL);
  RE_REQUIRE(parse_timestamp("2025-12-31T22:00:00-02:00", value));
  RE_CHECK_EQ(value, 1767225600000000000LL);
}

RE_TEST(timestamp_boundaries_and_leap_years) {
  Timestamp value = 0;
  RE_REQUIRE(parse_timestamp("2024-02-29T12:00:00Z", value));
  RE_CHECK_EQ(format_timestamp(value), std::string("2024-02-29T12:00:00.000000000Z"));
  RE_CHECK(!parse_timestamp("2023-02-29T12:00:00Z", value));
  RE_REQUIRE(parse_timestamp("2000-02-29T00:00:00Z", value));
  RE_CHECK(!parse_timestamp("1900-02-29T00:00:00Z", value));
  RE_CHECK(!is_valid_timestamp(-1));
  RE_CHECK(is_valid_timestamp(0));
  RE_CHECK(is_valid_timestamp(kTimestampMax));
}

RE_TEST(timestamp_addition_fails_instead_of_wrapping) {
  Timestamp result = 0;
  RE_CHECK(checked_add_timestamp(10, 5, result));
  RE_CHECK_EQ(result, 15);
  RE_CHECK(!checked_add_timestamp(kTimestampMax, 1, result));
  RE_CHECK(!checked_add_timestamp(0, -1, result));
}

#include <string>
