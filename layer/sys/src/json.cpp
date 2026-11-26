#include "any360/sys/json.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace any360::sys::json {

const Value* Value::Find(std::string_view name) const {
  if (type != Type::kObject) return nullptr;
  for (const auto& [key, value] : object)
    if (key == name) return &value;
  return nullptr;
}

namespace {

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  bool Document(Value& out, std::string* error) {
    Space();
    bool ok = ParseValue(out, 0);
    if (ok) {
      Space();
      if (at_ != text_.size()) ok = Fail("data after the document");
    }
    if (!ok) {
      if (error) *error = Where() + ": " + error_;
      return false;
    }
    return true;
  }

 private:
  bool Fail(const char* what) {
    if (error_.empty()) error_ = what;
    return false;
  }
  std::string Where() const {
    size_t line = 1, column = 1;
    for (size_t i = 0; i < at_ && i < text_.size(); ++i) {
      if (text_[i] == '\n') ++line, column = 1;
      else ++column;
    }
    return "line " + std::to_string(line) + ", column " + std::to_string(column);
  }
  void Space() {
    while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\n' || text_[at_] == '\r')) ++at_;
  }
  bool Literal(std::string_view word) {
    if (text_.substr(at_, word.size()) != word) return Fail("unknown literal");
    at_ += word.size();
    return true;
  }

  bool ParseValue(Value& v, int depth) {
    if (depth >= kMaxDepth) return Fail("nested too deeply");
    if (at_ >= text_.size()) return Fail("unexpected end");
    switch (text_[at_]) {
      case '{': return ParseObject(v, depth);
      case '[': return ParseArray(v, depth);
      case '"': v.type = Value::Type::kString; return ParseString(v.string);
      case 't': v.type = Value::Type::kBool; v.boolean = true; return Literal("true");
      case 'f': v.type = Value::Type::kBool; v.boolean = false; return Literal("false");
      case 'n': v.type = Value::Type::kNull; return Literal("null");
      default: return ParseNumber(v);
    }
  }

  bool ParseObject(Value& v, int depth) {
    v.type = Value::Type::kObject;
    ++at_;
    Space();
    if (at_ < text_.size() && text_[at_] == '}') return ++at_, true;
    for (;;) {
      Space();
      if (at_ >= text_.size() || text_[at_] != '"') return Fail("expected a member name");
      std::string key;
      if (!ParseString(key)) return false;
      Space();
      if (at_ >= text_.size() || text_[at_] != ':') return Fail("expected ':'");
      ++at_;
      Space();
      Value member;
      if (!ParseValue(member, depth + 1)) return false;
      v.object.push_back(Member{std::move(key), std::move(member)});
      Space();
      if (at_ < text_.size() && text_[at_] == ',') { ++at_; continue; }
      if (at_ < text_.size() && text_[at_] == '}') return ++at_, true;
      return Fail("expected ',' or '}'");
    }
  }

  bool ParseArray(Value& v, int depth) {
    v.type = Value::Type::kArray;
    ++at_;
    Space();
    if (at_ < text_.size() && text_[at_] == ']') return ++at_, true;
    for (;;) {
      Space();
      Value element;
      if (!ParseValue(element, depth + 1)) return false;
      v.array.push_back(std::move(element));
      Space();
      if (at_ < text_.size() && text_[at_] == ',') { ++at_; continue; }
      if (at_ < text_.size() && text_[at_] == ']') return ++at_, true;
      return Fail("expected ',' or ']'");
    }
  }

  bool Hex4(uint32_t& code) {
    if (at_ + 4 > text_.size()) return Fail("short \\u escape");
    code = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[at_++];
      code <<= 4;
      if (c >= '0' && c <= '9') code |= uint32_t(c - '0');
      else if (c >= 'a' && c <= 'f') code |= uint32_t(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') code |= uint32_t(c - 'A' + 10);
      else return Fail("bad \\u escape");
    }
    return true;
  }
  static void Utf8(uint32_t c, std::string& out) {
    if (c < 0x80) {
      out += char(c);
    } else if (c < 0x800) {
      out += char(0xC0 | c >> 6), out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += char(0xE0 | c >> 12), out += char(0x80 | (c >> 6 & 0x3F)), out += char(0x80 | (c & 0x3F));
    } else {
      out += char(0xF0 | c >> 18), out += char(0x80 | (c >> 12 & 0x3F)), out += char(0x80 | (c >> 6 & 0x3F)),
          out += char(0x80 | (c & 0x3F));
    }
  }

  bool ParseString(std::string& out) {
    ++at_;  // the opening quote
    while (at_ < text_.size()) {
      const char c = text_[at_++];
      if (c == '"') return true;
      if (uint8_t(c) < 0x20) return Fail("control character in a string");
      if (c != '\\') {
        out += c;
        continue;
      }
      if (at_ >= text_.size()) break;
      switch (const char e = text_[at_++]) {
        case '"': case '\\': case '/': out += e; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          uint32_t code;
          if (!Hex4(code)) return false;
          if (code >= 0xD800 && code < 0xDC00) {  // a high surrogate: its low half must follow
            uint32_t low;
            if (text_.substr(at_, 2) != "\\u" || (at_ += 2, !Hex4(low)) || low < 0xDC00 || low >= 0xE000)
              return Fail("unpaired surrogate");
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
          } else if (code >= 0xDC00 && code < 0xE000) {
            return Fail("unpaired surrogate");
          }
          Utf8(code, out);
          break;
        }
        default: return Fail("bad escape");
      }
    }
    return Fail("unterminated string");
  }

  bool ParseNumber(Value& v) {
    // JSON's grammar: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
    const size_t start = at_;
    auto digits = [&] {
      const size_t from = at_;
      while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') ++at_;
      return at_ > from;
    };
    if (at_ < text_.size() && text_[at_] == '-') ++at_;
    if (at_ < text_.size() && text_[at_] == '0') ++at_;
    else if (!digits()) return Fail("unexpected character");
    if (at_ < text_.size() && text_[at_] == '.' && (++at_, !digits())) return Fail("bad number");
    if (at_ < text_.size() && (text_[at_] == 'e' || text_[at_] == 'E')) {
      ++at_;
      if (at_ < text_.size() && (text_[at_] == '+' || text_[at_] == '-')) ++at_;
      if (!digits()) return Fail("bad number");
    }
    const std::string number(text_.substr(start, at_ - start));
    v.type = Value::Type::kNumber;
    v.number = std::strtod(number.c_str(), nullptr);
    if (!std::isfinite(v.number)) return Fail("number out of range");
    return true;
  }

  std::string_view text_;
  size_t at_ = 0;
  std::string error_;
};

}  // namespace

bool Parse(std::string_view text, Value& out, std::string* error) {
  out = Value{};
  return Parser(text).Document(out, error);
}

}  // namespace any360::sys::json
