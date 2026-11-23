// A small JSON reader (RFC 8259) for the files Any360 is handed at launch (the input bindings): objects keep their
// members in order, numbers are doubles, strings are UTF-8 (\u escapes and surrogate pairs decoded). Nesting is
// bounded (kMaxDepth) so a hostile file cannot exhaust the stack. No writer: Any360 only reads these files.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace any360::sys::json {

struct Value {
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };
  Type type = Type::kNull;
  bool boolean = false;
  double number = 0;
  std::string string;
  std::vector<Value> array;
  std::vector<std::pair<std::string, Value>> object;

  bool is(Type t) const { return type == t; }
  // The member `name` of an object (the first one), or nullptr.
  const Value* Find(std::string_view name) const;
};

inline constexpr int kMaxDepth = 64;

// Parses the whole of `text`; false with `error` ("line L, column C: what") on malformed input or trailing data.
bool Parse(std::string_view text, Value& out, std::string* error = nullptr);

}  // namespace any360::sys::json
