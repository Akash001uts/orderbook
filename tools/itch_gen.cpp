#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "itch/synthetic.hpp"

// Writes a synthetic NASDAQ TotalView-ITCH 5.0 file.
//
// The point of this tool is that the repository's tests and benchmarks are
// runnable by anyone who clones it, with no multi gigabyte download behind a
// NASDAQ login. The output is real ITCH 5.0 binary and the parser reads it through
// exactly the same code path as a real capture, with no branch anywhere on where
// the bytes came from.

namespace {

void print_usage() {
  std::cout <<
      R"(usage: itch_gen [options] <output-file>

  --symbol NAME        symbol for the target stock (default TEST)
  --messages N         number of messages to write (default 1000000)
  --seed N             RNG seed; the same seed always produces the same file
  --price N            initial price in scaled integer units, 4 implied decimals
  --tick N             tick size in scaled units (default 100, a penny)
  --volatility F       stddev of the mid's random walk, in ticks per message
  --depth N            how far from the mid quotes are placed, in ticks
  --max-shares N       largest order size
  --interval N         nanoseconds between messages
  --decoys N           number of extra symbols interleaved into the stream

Higher volatility walks the mid further and forces the book's band to rebase more
often, which is the workload worth generating when testing that path.
)";
}

// Returns true when the argument was consumed. Deliberately simple: this is a
// developer tool, not a product, and a real option parser would be more code than
// the generator it configures.
[[nodiscard]] bool match_option(std::string_view argument,
                                std::string_view name,
                                int& index,
                                int argc,
                                char** argv,
                                std::string& value) {
  if (argument != name) {
    return false;
  }
  if (index + 1 >= argc) {
    std::cerr << "missing value for " << name << '\n';
    std::exit(2);
  }
  ++index;
  value = argv[index];
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  ob::itch::SyntheticConfig config;
  config.message_count = 1000000;

  std::string output;
  std::string value;

  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];

    if (argument == "--help" || argument == "-h") {
      print_usage();
      return 0;
    }
    if (match_option(argument, "--symbol", index, argc, argv, value)) {
      config.symbol = value;
      continue;
    }
    if (match_option(argument, "--messages", index, argc, argv, value)) {
      config.message_count = static_cast<std::size_t>(std::stoull(value));
      continue;
    }
    if (match_option(argument, "--seed", index, argc, argv, value)) {
      config.seed = std::stoull(value);
      continue;
    }
    if (match_option(argument, "--price", index, argc, argv, value)) {
      config.initial_price = std::stoll(value);
      continue;
    }
    if (match_option(argument, "--tick", index, argc, argv, value)) {
      config.tick_size = std::stoll(value);
      continue;
    }
    if (match_option(argument, "--volatility", index, argc, argv, value)) {
      config.volatility_ticks = std::stod(value);
      continue;
    }
    if (match_option(argument, "--depth", index, argc, argv, value)) {
      config.quote_depth_ticks = static_cast<std::int32_t>(std::stoi(value));
      continue;
    }
    if (match_option(argument, "--max-shares", index, argc, argv, value)) {
      config.max_shares = static_cast<std::uint32_t>(std::stoul(value));
      continue;
    }
    if (match_option(argument, "--interval", index, argc, argv, value)) {
      config.nanos_between_messages = std::stoull(value);
      continue;
    }
    if (match_option(argument, "--decoys", index, argc, argv, value)) {
      config.decoy_symbols = static_cast<std::uint16_t>(std::stoul(value));
      continue;
    }

    if (!argument.empty() && argument.front() == '-') {
      std::cerr << "unknown option: " << argument << '\n';
      print_usage();
      return 2;
    }

    output = argument;
  }

  if (output.empty()) {
    print_usage();
    return 2;
  }

  ob::itch::SyntheticGenerator generator(config);
  const std::vector<std::byte> data = generator.generate();

  std::ofstream out(output, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::cerr << "cannot open " << output << " for writing\n";
    return 1;
  }

  // std::byte and char are both byte types, so viewing one as the other is
  // permitted rather than an aliasing violation, and ostream has no std::byte
  // overload.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  if (!out.good()) {
    std::cerr << "write failed for " << output << '\n';
    return 1;
  }

  const ob::itch::SyntheticSummary& summary = generator.summary();
  std::cout << "wrote " << output << '\n'
            << "  messages            " << summary.messages << '\n'
            << "  bytes               " << summary.bytes << '\n'
            << "  system events       " << summary.system_events << '\n'
            << "  stock directories   " << summary.stock_directories << '\n'
            << "  adds                " << summary.adds << '\n'
            << "  executions          " << summary.executions << '\n'
            << "  cancels             " << summary.cancels << '\n'
            << "  deletes             " << summary.deletes << '\n'
            << "  replaces            " << summary.replaces << '\n'
            << "  trades              " << summary.trades << '\n'
            << "  cross trades        " << summary.cross_trades << '\n'
            << "  broken trades       " << summary.broken_trades << '\n'
            << "  decoy messages      " << summary.decoy_messages << '\n'
            << "  orders left resting " << summary.expected_live_orders << '\n'
            << "  shares left resting " << summary.expected_total_shares << '\n';

  return 0;
}
