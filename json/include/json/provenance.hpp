#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "json/writer.hpp"

// Provenance, written into every artifact the results site reads.
//
// The reason this exists is a limitation that cannot be engineered away. Only one
// symbol's raw tape is committed, so only one symbol's artifacts can be
// regenerated and diffed by CI. Every other symbol is derived from a capture that
// lives on one machine, and a number nobody can reproduce needs to say so on its
// face rather than in a paragraph somewhere else. So each file names the input it
// came from, that input's SHA-256, and the exact command line that produced it.
//
// The hash is supplied by the caller rather than computed here. Hashing a 7.68 GB
// capture inside every tool run would cost minutes per invocation, and a SHA-256
// implementation added to this repository to avoid calling sha256sum would be a
// worse trade than the one the dependency rule is protecting against. The
// generation script computes it once and passes it in.
//
// Paths are recorded exactly as they appeared on the command line. The CI guard
// compares bytes, and it runs in a different absolute directory than a developer
// does, so an absolute path in the artifact would fail the diff for a reason that
// has nothing to do with the code. Invoke the tools with repository relative
// paths.

namespace ob::json {

struct Provenance {
  std::string tool;
  std::string version;
  std::string source_path;
  std::uint64_t source_bytes = 0;
  // Empty when the caller did not supply one, which is honest rather than absent:
  // the field is written as null so a reader can tell "unhashed" from "unhashable".
  std::string source_sha256;
  std::string command;
};

// argv[0] reduced to the program name. The build directory and the executable
// suffix differ between a developer's machine and a CI runner, and the guard that
// regenerates these files compares bytes, so leaving the invocation path in would
// fail the diff for a reason that has nothing to do with the code.
[[nodiscard]] inline std::string program_name(std::string_view argument) {
  const std::size_t slash = argument.find_last_of("/\\");
  if (slash != std::string_view::npos) {
    argument.remove_prefix(slash + 1U);
  }
  if (argument.ends_with(".exe")) {
    argument.remove_suffix(4U);
  }
  return std::string(argument);
}

// argv joined back into one string. Arguments containing a space are quoted, which
// covers the only case this repository produces: a path under a user directory
// with a space in it.
[[nodiscard]] inline std::string command_line(int argc, char** argv) {
  std::string command;
  for (int index = 0; index < argc; ++index) {
    if (index > 0) {
      command += ' ';
    }
    if (index == 0) {
      command += program_name(argv[0]);
      continue;
    }
    const std::string_view argument = argv[index];
    const bool quote = argument.find(' ') != std::string_view::npos;
    if (quote) {
      command += '"';
    }
    command += argument;
    if (quote) {
      command += '"';
    }
  }
  return command;
}

inline void write_provenance(Writer& writer, const Provenance& provenance) {
  writer.key("provenance");
  writer.begin_object();
  writer.field("source", provenance.source_path);
  writer.field("source_bytes", provenance.source_bytes);
  if (provenance.source_sha256.empty()) {
    writer.null_field("source_sha256");
  } else {
    writer.field("source_sha256", provenance.source_sha256);
  }
  writer.field("command", provenance.command);
  writer.end_object();
}

// The header every artifact opens with. Schema version first, so a reader that
// does not recognise it can stop before interpreting anything else.
constexpr std::int64_t SCHEMA_VERSION = 1;

inline void write_header(Writer& writer, const Provenance& provenance) {
  writer.field("schema", SCHEMA_VERSION);
  writer.field("tool", provenance.tool);
  writer.field("version", provenance.version);
  write_provenance(writer, provenance);
}

}  // namespace ob::json
