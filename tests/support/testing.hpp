#ifndef RESOURCE_ENVELOPE_TESTING_HPP
#define RESOURCE_ENVELOPE_TESTING_HPP

// Minimal, dependency-free test harness. Every check reports the file and line that
// failed so a failure is directly actionable, and the process exit code is non-zero
// when anything failed. No test relies on a timeout: a hang is a defect to diagnose.

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

namespace testing {

struct TestCase {
  std::string name;
  std::function<void()> body;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

inline int& failure_count() {
  static int count = 0;
  return count;
}

inline int& check_count() {
  static int count = 0;
  return count;
}

inline std::string& current_test() {
  static std::string name;
  return name;
}

struct Registration {
  Registration(const char* name, std::function<void()> body) {
    registry().push_back(TestCase{name, std::move(body)});
  }
};

inline void report_failure(const char* file, int line, const std::string& message) {
  ++failure_count();
  std::fprintf(stdout, "FAIL %s (%s:%d): %s\n", current_test().c_str(), file, line, message.c_str());
  std::fflush(stdout);
}

inline int run_all() {
  for (TestCase& test : registry()) {
    current_test() = test.name;
    const int before = failure_count();
    std::fprintf(stdout, "RUN  %s\n", test.name.c_str());
    std::fflush(stdout);
    try {
      test.body();
    } catch (const std::exception& error) {
      report_failure(__FILE__, __LINE__, std::string("uncaught exception: ") + error.what());
    } catch (...) {
      report_failure(__FILE__, __LINE__, "uncaught non-standard exception");
    }
    if (failure_count() == before) {
      std::fprintf(stdout, "PASS %s\n", test.name.c_str());
    }
  }
  std::fprintf(stdout, "%d checks, %d failures\n", check_count(), failure_count());
  std::fflush(stdout);
  return failure_count() == 0 ? 0 : 1;
}

// Renders a value for a failure message. Integral values use std::to_string; anything
// else falls back to the type name, so an enum without a formatter still produces a
// readable message instead of failing to compile.
template <typename T>
std::string describe(const T& value) {
  if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {
    return std::to_string(value);
  } else if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<std::underlying_type_t<T>>(value));
  } else {
    static_cast<void>(value);
    return std::string("<value>");
  }
}

inline std::string describe(const std::string& value) { return "\"" + value + "\""; }
inline std::string describe(const char* value) { return std::string("\"") + value + "\""; }
inline std::string describe(bool value) { return value ? "true" : "false"; }

}  // namespace testing

#define RE_TEST(name)                                                    \
  static void name();                                                   \
  static const ::testing::Registration re_registration_##name(#name, name); \
  static void name()

#define RE_CHECK(condition)                                                             \
  do {                                                                                  \
    ++::testing::check_count();                                                          \
    if (!(condition)) {                                                                   \
      ::testing::report_failure(__FILE__, __LINE__, std::string("expected ") + #condition); \
    }                                                                                     \
  } while (false)

#define RE_CHECK_EQ(actual, expected)                                                          \
  do {                                                                                          \
    ++::testing::check_count();                                                                  \
    const auto& re_actual = (actual);                                                             \
    const auto& re_expected = (expected);                                                         \
    if (!(re_actual == re_expected)) {                                                             \
      ::testing::report_failure(__FILE__, __LINE__,                                                 \
                               std::string(#actual) + " == " + #expected + " (got " +             \
                                   ::testing::describe(re_actual) + ", want " +                    \
                                   ::testing::describe(re_expected) + ")");                         \
    }                                                                                               \
  } while (false)

#define RE_CHECK_NE(actual, expected)                                                          \
  do {                                                                                          \
    ++::testing::check_count();                                                                  \
    if ((actual) == (expected)) {                                                                 \
      ::testing::report_failure(__FILE__, __LINE__,                                                 \
                               std::string(#actual) + " != " + #expected + " was unexpectedly equal"); \
    }                                                                                               \
  } while (false)

#define RE_REQUIRE(condition)                                                          \
  do {                                                                                  \
    ++::testing::check_count();                                                          \
    if (!(condition)) {                                                                   \
      ::testing::report_failure(__FILE__, __LINE__, std::string("required ") + #condition); \
      return;                                                                             \
    }                                                                                     \
  } while (false)

#endif  // RESOURCE_ENVELOPE_TESTING_HPP
