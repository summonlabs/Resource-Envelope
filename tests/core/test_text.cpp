#include "support/testing.hpp"

#include "resource_envelope/text.hpp"

using namespace resource_envelope;

RE_TEST(utf8_validation_rejects_overlong_and_surrogate_forms) {
  RE_CHECK(is_valid_utf8("plain-ascii"));
  RE_CHECK(is_valid_utf8(std::string_view("\xC3\xA9")));
  RE_CHECK(!is_valid_utf8(std::string_view("\xC0\xAF")));
  RE_CHECK(!is_valid_utf8(std::string_view("\xED\xA0\x80")));
  RE_CHECK(!is_valid_utf8(std::string_view("\xF4\x90\x80\x80")));
  RE_CHECK(!is_valid_utf8(std::string_view("\xE2\x82")));
  RE_CHECK(!is_valid_utf8(std::string_view("a\0b", 3)));
}

RE_TEST(identifier_validation_reports_the_exact_defect) {
  RE_CHECK_EQ(validate_identifier("tenant-a", IdentifierUse::Canonical), TextDefect::None);
  RE_CHECK_EQ(validate_identifier("", IdentifierUse::Canonical), TextDefect::Empty);
  RE_CHECK_EQ(validate_identifier("Tenant", IdentifierUse::Canonical), TextDefect::InvalidCharacter);
  RE_CHECK_EQ(validate_identifier("-lead", IdentifierUse::Canonical), TextDefect::LeadingOrTrailingHyphen);
  RE_CHECK_EQ(validate_identifier("trail-", IdentifierUse::Canonical), TextDefect::LeadingOrTrailingHyphen);
  RE_CHECK_EQ(validate_identifier("two--hyphens", IdentifierUse::Canonical), TextDefect::ConsecutiveHyphens);
  RE_CHECK_EQ(validate_identifier("con", IdentifierUse::Canonical), TextDefect::ReservedDeviceName);
  RE_CHECK_EQ(validate_identifier("nul.txt", IdentifierUse::Canonical), TextDefect::InvalidCharacter);
  RE_CHECK_EQ(validate_identifier(std::string(65, 'a'), IdentifierUse::Canonical), TextDefect::TooLong);
  RE_CHECK_EQ(validate_identifier(std::string(64, 'a'), IdentifierUse::Canonical), TextDefect::None);
  RE_CHECK_EQ(validate_identifier(" padded ", IdentifierUse::Label), TextDefect::SurroundingWhitespace);
}

RE_TEST(reserved_device_names_cover_the_windows_set) {
  const char* reserved[] = {"CON", "prn", "Aux", "nul", "COM1", "lpt9", "con. "};
  for (const char* name : reserved) RE_CHECK(is_reserved_device_name(name));
  RE_CHECK(!is_reserved_device_name("console"));
  RE_CHECK(!is_reserved_device_name("com10"));
  RE_CHECK(!is_reserved_device_name(""));
}

RE_TEST(text_splitting_and_trimming_are_exact) {
  const std::vector<std::string> parts = split_commas(" a , b ,, c ");
  RE_REQUIRE(parts.size() == 3U);
  RE_CHECK_EQ(parts[0], std::string("a"));
  RE_CHECK_EQ(parts[1], std::string("b"));
  RE_CHECK_EQ(parts[2], std::string("c"));
  RE_CHECK_EQ(trim("  x  "), std::string("x"));
  RE_CHECK_EQ(trim("   "), std::string(""));
  RE_CHECK(starts_with("segment-0001", "segment-"));
  RE_CHECK(ends_with("segment-0001.renv", ".renv"));
  RE_CHECK(!ends_with("segment", ".renv"));
}

#include <string>
#include <vector>
