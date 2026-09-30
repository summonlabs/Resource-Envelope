#ifndef RESOURCE_ENVELOPE_CLI_JSON_HPP
#define RESOURCE_ENVELOPE_CLI_JSON_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "resource_envelope/decision.hpp"
#include "resource_envelope/digest.hpp"
#include "resource_envelope/quantity.hpp"
#include "resource_envelope/status.hpp"

namespace resource_envelope {
namespace cli {

// Canonical JSON emitter for the command line tool. Object fields are written in the
// order the caller requests them, so two runs over equivalent inputs produce
// byte-identical output and the tool can be diffed. Nesting is tracked so separators
// are always correct, and nothing else is ever written to the stream.
class JsonWriter {
 public:
  explicit JsonWriter(std::ostream& out) : out_(out) {}

  void begin_object() {
    element_prefix();
    out_ << '{';
    frames_.push_back(Frame{true, false});
  }
  void end_object() {
    out_ << '}';
    frames_.pop_back();
    if (!frames_.empty()) frames_.back().first = false;
  }
  void begin_array() {
    element_prefix();
    out_ << '[';
    frames_.push_back(Frame{true, true});
  }
  void end_array() {
    out_ << ']';
    frames_.pop_back();
  }

  void string_field(const char* key, const std::string& value) {
    key_prefix(key);
    write_string(value);
  }
  void integer_field(const char* key, std::uint64_t value) {
    key_prefix(key);
    out_ << value;
  }
  void signed_field(const char* key, std::int64_t value) {
    key_prefix(key);
    out_ << value;
  }
  void bool_field(const char* key, bool value) {
    key_prefix(key);
    out_ << (value ? "true" : "false");
  }
  void null_field(const char* key) {
    key_prefix(key);
    out_ << "null";
  }
  void optional_string_field(const char* key, const std::optional<std::string>& value) {
    if (!value.has_value()) {
      null_field(key);
      return;
    }
    string_field(key, *value);
  }
  void quantity_field(const char* key, const std::optional<Nanounits>& value) {
    if (!value.has_value()) {
      null_field(key);
      return;
    }
    string_field(key, format_quantity(*value));
  }
  void digest_field(const char* key, const Digest& digest) { string_field(key, digest.to_string()); }
  void status_field(const char* key, StatusCode code) { integer_field(key, static_cast<std::uint64_t>(code)); }
  void string_value(const std::string& value) {
    element_prefix();
    write_string(value);
  }
  void quantity_value(Nanounits value) { string_value(format_quantity(value)); }
  void integer_element(std::uint64_t value) {
    element_prefix();
    out_ << value;
  }
  void string_element(const std::string& value) { string_value(value); }
  // Opens an array that is written as the value of one object field.
  void field_array_start(const char* key) {
    key_prefix(key);
    out_ << '[';
    frames_.push_back(Frame{true, true});
  }
  // Opens an object that is written as the value of one object field.
  void field_object_start(const char* key) {
    key_prefix(key);
    out_ << '{';
    frames_.push_back(Frame{true, false});
  }
  // Places one pre-rendered object element inside the open array. The callback writes
  // fields into a fresh object frame, so nested arrays stay well formed.
  template <typename Body>
  void object_element(Body&& body) {
    element_prefix();
    out_ << '{';
    frames_.push_back(Frame{true, false});
    body(*this);
    out_ << '}';
    frames_.pop_back();
  }

 private:
  struct Frame {
    bool first = true;
    bool is_array = false;
  };

  void element_prefix() {
    if (frames_.empty()) return;
    Frame& frame = frames_.back();
    if (!frame.first) out_ << ',';
    frame.first = false;
  }

  void key_prefix(const char* key) {
    element_prefix();
    write_string(key);
    out_ << ':';
  }

  void write_string(const std::string& value) {
    static const char* kHex = "0123456789abcdef";
    out_ << '"';
    for (const char character : value) {
      const unsigned char byte = static_cast<unsigned char>(character);
      switch (character) {
        case '"': out_ << "\\\""; break;
        case '\\': out_ << "\\\\"; break;
        case '\n': out_ << "\\n"; break;
        case '\r': out_ << "\\r"; break;
        case '\t': out_ << "\\t"; break;
        default:
          if (byte < 0x20U) {
            out_ << "\\u00" << kHex[(byte >> 4U) & 0x0FU] << kHex[byte & 0x0FU];
          } else {
            out_ << character;
          }
          break;
      }
    }
    out_ << '"';
  }

  std::ostream& out_;
  std::vector<Frame> frames_;
};

}  // namespace cli
}  // namespace resource_envelope

#endif  // RESOURCE_ENVELOPE_CLI_JSON_HPP
