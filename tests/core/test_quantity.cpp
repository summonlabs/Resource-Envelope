#include "support/testing.hpp"

#include "resource_envelope/quantity.hpp"

using namespace resource_envelope;

RE_TEST(quantity_parses_canonical_decimal) {
  Nanounits value = 0;
  RE_CHECK(parse_quantity("0", value));
  RE_CHECK_EQ(value, 0ULL);
  RE_CHECK(parse_quantity("12", value));
  RE_CHECK_EQ(value, 12ULL * kNanounitsPerUnit);
  RE_CHECK(parse_quantity("0.000000001", value));
  RE_CHECK_EQ(value, 1ULL);
  RE_CHECK(parse_quantity("1.5", value));
  RE_CHECK_EQ(value, 1500000000ULL);
}

RE_TEST(quantity_request_rejects_non_canonical_forms) {
  Nanounits value = 0;
  RE_CHECK(!parse_quantity("", value));
  RE_CHECK(!parse_quantity("-1", value));
  RE_CHECK(!parse_quantity("1e3", value));
  RE_CHECK(!parse_quantity(" 1", value));
  RE_CHECK(!parse_quantity("1.", value));
  RE_CHECK(!parse_quantity(".5", value));
  RE_CHECK(!parse_quantity("1,000", value));
  RE_CHECK(!parse_quantity("1.0000000000", value));
}

RE_TEST(quantity_format_is_canonical_and_round_trips) {
  const Nanounits values[] = {0ULL, 1ULL, 999999999ULL, kNanounitsPerUnit, 1500000000ULL,
                              1000000001ULL, 1000000000000ULL};
  for (const Nanounits value : values) {
    const std::string text = format_quantity(value);
    Nanounits parsed = 0;
    RE_REQUIRE(parse_quantity(text, parsed));
    RE_CHECK_EQ(parsed, value);
  }
  RE_CHECK_EQ(format_quantity(0ULL), std::string("0"));
  RE_CHECK_EQ(format_quantity(kNanounitsPerUnit), std::string("1"));
  RE_CHECK_EQ(format_quantity(1500000000ULL), std::string("1.5"));
}

RE_TEST(quantity_addition_detects_overflow_instead_of_wrapping) {
  const Nanounits maximum = std::numeric_limits<Nanounits>::max();
  const CheckedQuantity sum = checked_add(maximum, 1ULL);
  RE_CHECK(sum.overflow);
  RE_CHECK_EQ(sum.value, maximum);
  const CheckedQuantity underflow = checked_sub(0ULL, 1ULL);
  RE_CHECK(underflow.overflow);
  RE_CHECK_EQ(underflow.value, 0ULL);
  const CheckedQuantity exact = checked_add(maximum - 1ULL, 1ULL);
  RE_CHECK(exact.ok());
  RE_CHECK_EQ(exact.value, maximum);
}

RE_TEST(quantity_make_rejects_out_of_range_declarations) {
  Nanounits value = 0;
  RE_CHECK(make_quantity(0U, 999999999U, value));
  RE_CHECK_EQ(value, 999999999ULL);
  RE_CHECK(!make_quantity(0U, 1000000000U, value));
  RE_CHECK(!make_quantity(kQuantityReasonableMax / kNanounitsPerUnit + 1ULL, 0U, value));
  RE_CHECK(!make_quantity(kQuantityReasonableMax / kNanounitsPerUnit, kNanounitsPerUnit - 1U, value));
}

RE_TEST(quantization_rounds_up_and_reports_overflow) {
  const CheckedQuantity aligned = round_up_to_multiple(7ULL, 4ULL);
  RE_CHECK_EQ(aligned.value, 8ULL);
  RE_CHECK(!aligned.overflow);
  const CheckedQuantity exact = round_up_to_multiple(8ULL, 4ULL);
  RE_CHECK_EQ(exact.value, 8ULL);
  const CheckedQuantity none = round_up_to_multiple(7ULL, 0ULL);
  RE_CHECK_EQ(none.value, 7ULL);
  const Nanounits maximum = std::numeric_limits<Nanounits>::max();
  const CheckedQuantity overflowed = round_up_to_multiple(maximum, 4ULL);
  RE_CHECK(overflowed.overflow);
  RE_CHECK(is_multiple_of(8ULL, 4ULL));
  RE_CHECK(!is_multiple_of(9ULL, 4ULL));
  RE_CHECK(!is_multiple_of(8ULL, 0ULL));
}

RE_TEST(rate_times_count_rounds_up_to_whole_units) {
  // 2.5 Wh per unit over 3 units is 7.5 Wh, reported as 8 whole watt-hours because
  // rounding down would understate a cooling budget.
  const CheckedQuantity total = rate_times_count(2500000000ULL, 3U);
  RE_REQUIRE(total.ok());
  // The conversion is documented to report whole base units: 7.5 Wh rounds up to 8.
  RE_CHECK_EQ(total.value, 8ULL);
  const CheckedQuantity zero = rate_times_count(2500000000ULL, 0U);
  RE_CHECK_EQ(zero.value, 0ULL);
}

#include <limits>
