#include "resource_envelope/time.hpp"

#include <array>

namespace resource_envelope {
namespace {

constexpr std::int64_t kNanosPerSecond = 1000000000LL;
constexpr std::int64_t kSecondsPerDay = 86400LL;
constexpr std::int64_t kMaxDays = 2932896LL;  // 9999-12-31 relative to 1970-01-01

// Days-from-civil and civil-from-days, using the era-based algorithm that is exact
// for the whole proleptic Gregorian calendar and independent of any locale or
// library time-zone database.
constexpr std::int64_t days_from_civil(std::int64_t year, std::int64_t month, std::int64_t day) noexcept {
  year -= month <= 2 ? 1 : 0;
  const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
  const std::int64_t year_of_era = year - era * 400;
  const std::int64_t day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const std::int64_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return era * 146097 + day_of_era - 719468;
}

struct CivilDate {
  std::int64_t year;
  std::int64_t month;
  std::int64_t day;
};

constexpr CivilDate civil_from_days(std::int64_t days) noexcept {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const std::int64_t day_of_era = days - era * 146097;
  const std::int64_t year_of_era =
      (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
  std::int64_t year = year_of_era + era * 400;
  const std::int64_t day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
  const std::int64_t month_prime = (5 * day_of_year + 2) / 153;
  const std::int64_t day = day_of_year - (153 * month_prime + 2) / 5 + 1;
  const std::int64_t month = month_prime + (month_prime < 10 ? 3 : -9);
  year += month <= 2 ? 1 : 0;
  return CivilDate{year, month, day};
}

bool is_leap_year(std::int64_t year) noexcept {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

std::int64_t days_in_month(std::int64_t year, std::int64_t month) noexcept {
  static constexpr std::array<std::int64_t, 12> kMonthLengths = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  if (month == 2 && is_leap_year(year)) return 29;
  return kMonthLengths[static_cast<std::size_t>(month - 1)];
}

bool parse_fixed_digits(std::string_view text, std::size_t offset, std::size_t count,
                        std::int64_t& out) noexcept {
  if (offset + count > text.size()) return false;
  std::int64_t value = 0;
  for (std::size_t index = 0; index < count; ++index) {
    const char character = text[offset + index];
    if (character < '0' || character > '9') return false;
    value = value * 10 + (character - '0');
  }
  out = value;
  return true;
}

}  // namespace

bool is_valid_timestamp(Timestamp value) noexcept { return value >= kUnixEpoch && value <= kTimestampMax; }

std::string format_timestamp(Timestamp value) {
  if (value < kUnixEpoch) value = kUnixEpoch;
  if (value > kTimestampMax) value = kTimestampMax;
  const std::int64_t seconds = value / kNanosPerSecond;
  const std::int64_t nanos = value % kNanosPerSecond;
  const std::int64_t days = seconds / kSecondsPerDay;
  const std::int64_t second_of_day = seconds % kSecondsPerDay;
  const CivilDate date = civil_from_days(days);
  const std::int64_t hour = second_of_day / 3600;
  const std::int64_t minute = (second_of_day % 3600) / 60;
  const std::int64_t second = second_of_day % 60;

  // Fixed-width formatting. No locale-dependent conversion is used anywhere.
  std::string out(30, '0');
  const auto append_two = [&out](std::size_t position, std::int64_t number) {
    out[position] = static_cast<char>('0' + ((number / 10) % 10));
    out[position + 1U] = static_cast<char>('0' + (number % 10));
  };
  out[0] = static_cast<char>('0' + ((date.year / 1000) % 10));
  out[1] = static_cast<char>('0' + ((date.year / 100) % 10));
  out[2] = static_cast<char>('0' + ((date.year / 10) % 10));
  out[3] = static_cast<char>('0' + (date.year % 10));
  out[4] = '-';
  append_two(5, date.month);
  out[7] = '-';
  append_two(8, date.day);
  out[10] = 'T';
  append_two(11, hour);
  out[13] = ':';
  append_two(14, minute);
  out[16] = ':';
  append_two(17, second);
  out[19] = '.';
  out[20] = static_cast<char>('0' + ((nanos / 100000000) % 10));
  out[21] = static_cast<char>('0' + ((nanos / 10000000) % 10));
  out[22] = static_cast<char>('0' + ((nanos / 1000000) % 10));
  out[23] = static_cast<char>('0' + ((nanos / 100000) % 10));
  out[24] = static_cast<char>('0' + ((nanos / 10000) % 10));
  out[25] = static_cast<char>('0' + ((nanos / 1000) % 10));
  out[26] = static_cast<char>('0' + ((nanos / 100) % 10));
  out[27] = static_cast<char>('0' + ((nanos / 10) % 10));
  out[28] = static_cast<char>('0' + (nanos % 10));
  out[29] = 'Z';
  return out;
}

std::string format_timestamp_seconds(Timestamp value) {
  const std::string full = format_timestamp(value);
  return full.substr(0, 19) + "Z";
}

bool parse_timestamp(std::string_view text, Timestamp& out) noexcept {
  // Shortest accepted form is 1970-01-01T00:00:00Z.
  if (text.size() < 20U || text.size() > 35U) return false;
  if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':') return false;
  std::int64_t year = 0;
  std::int64_t month = 0;
  std::int64_t day = 0;
  std::int64_t hour = 0;
  std::int64_t minute = 0;
  std::int64_t second = 0;
  if (!parse_fixed_digits(text, 0, 4, year)) return false;
  if (!parse_fixed_digits(text, 5, 2, month)) return false;
  if (!parse_fixed_digits(text, 8, 2, day)) return false;
  if (!parse_fixed_digits(text, 11, 2, hour)) return false;
  if (!parse_fixed_digits(text, 14, 2, minute)) return false;
  if (!parse_fixed_digits(text, 17, 2, second)) return false;
  if (month < 1 || month > 12) return false;
  if (day < 1 || day > days_in_month(year, month)) return false;
  if (hour > 23 || minute > 59 || second > 59) return false;

  std::size_t index = 19;
  std::int64_t nanos = 0;
  if (index < text.size() && text[index] == '.') {
    ++index;
    std::size_t digits = 0;
    while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
      // More fractional precision than the model carries is refused rather than
      // truncated, so a parsed timestamp always round-trips exactly.
      if (digits >= 9U) return false;
      nanos = nanos * 10 + (text[index] - '0');
      ++digits;
      ++index;
    }
    if (digits == 0U) return false;
    for (std::size_t pad = digits; pad < 9U; ++pad) nanos *= 10;
  }

  std::int64_t offset_seconds = 0;
  if (index >= text.size()) return false;
  if (text[index] == 'Z') {
    ++index;
  } else if (text[index] == '+' || text[index] == '-') {
    const bool negative = text[index] == '-';
    ++index;
    std::int64_t offset_hour = 0;
    std::int64_t offset_minute = 0;
    if (!parse_fixed_digits(text, index, 2, offset_hour)) return false;
    index += 2;
    if (index < text.size() && text[index] == ':') ++index;
    if (!parse_fixed_digits(text, index, 2, offset_minute)) return false;
    index += 2;
    if (offset_hour > 23 || offset_minute > 59) return false;
    offset_seconds = offset_hour * 3600 + offset_minute * 60;
    if (negative) offset_seconds = -offset_seconds;
  } else {
    return false;
  }
  if (index != text.size()) return false;

  const std::int64_t days = days_from_civil(year, month, day);
  if (days < 0 || days > kMaxDays) return false;
  const std::int64_t seconds = days * kSecondsPerDay + hour * 3600 + minute * 60 + second - offset_seconds;
  if (seconds < 0) return false;
  // The model is bounded by the 64-bit nanosecond representation, and the civil range
  // is bounded above, so the product below cannot overflow for any accepted date.
  const Timestamp result = seconds * kNanosPerSecond + nanos;
  if (result > kTimestampMax) return false;
  out = result;
  return true;
}

bool checked_add_timestamp(Timestamp base, Timestamp delta, Timestamp& out) noexcept {
  if (delta > 0 && base > kTimestampMax - delta) return false;
  if (delta < 0 && base < kUnixEpoch - delta) return false;
  const Timestamp result = base + delta;
  if (result < kUnixEpoch || result > kTimestampMax) return false;
  out = result;
  return true;
}

}  // namespace resource_envelope
