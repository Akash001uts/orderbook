#pragma once

#include <cmath>
#include <concepts>
#include <cstdint>
#include <iomanip>
#include <ios>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

// A minimal, output only JSON writer.
//
// Hand written rather than fetched, for two reasons that both come from how these
// files are consumed. The results site binds to the schema of these documents, and
// a CI job regenerates them and diffs the bytes against the committed copies, so
// what matters is deterministic output and no new dependency. Nothing here ever
// parses JSON, which removes most of what a JSON library exists to do.
//
// Three decisions the diff guard depends on:
//
// Output is pretty printed, two space indentation, one value per line. A
// regenerated file then diffs line by line instead of as one enormous line, so a
// guard failure names the field that changed.
//
// Every double is written with an explicit decimal count chosen by the caller,
// never with the default shortest representation. A shortest round trip form can
// differ between standard library versions, so the same number would fail the
// guard on a compiler change rather than on a code change. The decimal counts
// mirror the precision the human readable output already uses, so the site shows
// the same figures the documentation does.
//
// Non-finite doubles are written as null. JSON has no encoding for NaN or
// infinity, and a Sharpe ratio over a degenerate sample is exactly the case that
// produces one.

namespace ob::json {

// Decimal counts used across the artifacts, named so that a field's precision is
// a stated choice rather than a literal repeated at each call site.
constexpr int MONEY_DECIMALS = 2;
constexpr int RATIO_DECIMALS = 4;
constexpr int PERCENT_DECIMALS = 4;
constexpr int RATE_DECIMALS = 6;

class Writer {
 public:
  explicit Writer(std::ostream& out) : out_(&out) {}

  // Objects and arrays. Every open must be closed; the destructor does not close
  // for you, because a silently repaired document would hide a bug in the caller
  // rather than surface it.
  void begin_object() { begin(Kind::object, '{'); }

  void end_object() { end(Kind::object, '}'); }

  void begin_array() { begin(Kind::array, '['); }

  void end_array() { end(Kind::array, ']'); }

  // A key inside an object. The following call writes its value.
  void key(std::string_view name) {
    separate();
    write_string(name);
    *out_ << ": ";
    pending_value_ = true;
  }

  void value(std::string_view text) {
    prepare();
    write_string(text);
  }

  void value(const std::string& text) { value(std::string_view(text)); }

  void value(const char* text) { value(std::string_view(text)); }

  void value(bool flag) {
    prepare();
    *out_ << (flag ? "true" : "false");
  }

  // One template rather than an overload per width. The tools hold their counters
  // in every integer type the standard library offers, and on this platform
  // several of those are the same type, so a set of fixed width overloads is both
  // incomplete and ambiguous. Streaming an integral value is already exact, so
  // there is nothing to hand format.
  template <typename T>
    requires std::integral<T> && (!std::same_as<T, bool>) && (!std::same_as<T, char>)
  void value(T number) {
    prepare();
    *out_ << number;
  }

  void value(double number, int decimals) {
    prepare();
    if (!std::isfinite(number)) {
      *out_ << "null";
      return;
    }
    const std::ios_base::fmtflags flags = out_->flags();
    const std::streamsize precision = out_->precision();
    *out_ << std::fixed << std::setprecision(decimals) << number;
    out_->flags(flags);
    out_->precision(precision);
  }

  void null_value() {
    prepare();
    *out_ << "null";
  }

  // Key and value in one call, which is what almost every call site wants.
  void field(std::string_view name, std::string_view text) {
    key(name);
    value(text);
  }

  void field(std::string_view name, const std::string& text) {
    key(name);
    value(std::string_view(text));
  }

  void field(std::string_view name, const char* text) {
    key(name);
    value(std::string_view(text));
  }

  void field(std::string_view name, bool flag) {
    key(name);
    value(flag);
  }

  template <typename T>
    requires std::integral<T> && (!std::same_as<T, bool>) && (!std::same_as<T, char>)
  void field(std::string_view name, T number) {
    key(name);
    value(number);
  }

  void field(std::string_view name, double number, int decimals) {
    key(name);
    value(number, decimals);
  }

  void null_field(std::string_view name) {
    key(name);
    null_value();
  }

  // A trailing newline, so the file ends the way every other text file in the
  // repository does and a diff does not report a missing one.
  void finish() { *out_ << '\n'; }

 private:
  enum class Kind : std::uint8_t { object, array };

  struct Frame {
    Kind kind = Kind::object;
    bool empty = true;
  };

  void begin(Kind kind, char open) {
    prepare();
    *out_ << open;
    frames_.push_back(Frame{.kind = kind, .empty = true});
  }

  void end(Kind kind, char close) {
    if (frames_.empty()) {
      return;
    }
    const Frame frame = frames_.back();
    frames_.pop_back();
    if (frame.kind != kind) {
      return;
    }
    if (!frame.empty) {
      *out_ << '\n';
      indent();
    }
    *out_ << close;
  }

  // A value that is not preceded by a key needs the comma and the newline an
  // array element gets. A value that follows a key must not, because the key
  // already wrote them.
  void prepare() {
    if (pending_value_) {
      pending_value_ = false;
      return;
    }
    separate();
  }

  void separate() {
    if (frames_.empty()) {
      return;
    }
    Frame& frame = frames_.back();
    if (!frame.empty) {
      *out_ << ',';
    }
    frame.empty = false;
    *out_ << '\n';
    indent();
  }

  void indent() {
    for (std::size_t level = 0; level < frames_.size(); ++level) {
      *out_ << "  ";
    }
  }

  // Escapes the six characters JSON requires plus every control character. The
  // inputs here are symbol names, file paths, and compiler flag strings, so the
  // path that matters in practice is the backslash in a Windows path.
  void write_string(std::string_view text) {
    *out_ << '"';
    for (const char character : text) {
      switch (character) {
        case '"':
          *out_ << "\\\"";
          break;
        case '\\':
          *out_ << "\\\\";
          break;
        case '\n':
          *out_ << "\\n";
          break;
        case '\r':
          *out_ << "\\r";
          break;
        case '\t':
          *out_ << "\\t";
          break;
        case '\b':
          *out_ << "\\b";
          break;
        case '\f':
          *out_ << "\\f";
          break;
        default:
          write_plain(character);
          break;
      }
    }
    *out_ << '"';
  }

  void write_plain(char character) {
    const auto code = static_cast<unsigned char>(character);
    if (code >= 0x20U) {
      *out_ << character;
      return;
    }

    const std::ios_base::fmtflags flags = out_->flags();
    const char fill = out_->fill();
    *out_ << "\\u";
    *out_ << std::hex << std::setw(4) << std::setfill('0');
    *out_ << static_cast<unsigned>(code);
    out_->flags(flags);
    out_->fill(fill);
  }

  std::ostream* out_;
  std::vector<Frame> frames_;
  bool pending_value_ = false;
};

}  // namespace ob::json
