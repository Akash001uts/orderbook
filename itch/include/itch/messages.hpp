#pragma once

#include <cstddef>
#include <cstdint>

namespace ob::itch {

// NASDAQ TotalView-ITCH 5.0 message types.
//
// The enum is char valued because the wire format is: byte zero of every message
// is the type, so a switch on the raw byte and a switch on this enum compile to
// the same thing while the second one reads.
//
// Note that 'h' is lower case in the specification while every other type is upper
// case. That is not a transcription error here, it is the format.
enum class MessageType : char {
  system_event = 'S',
  stock_directory = 'R',
  stock_trading_action = 'H',
  reg_sho_restriction = 'Y',
  market_participant_position = 'L',
  mwcb_decline_level = 'V',
  mwcb_status = 'W',
  ipo_quoting_period_update = 'K',
  luld_auction_collar = 'J',
  operational_halt = 'h',
  add_order = 'A',
  add_order_with_mpid = 'F',
  order_executed = 'E',
  order_executed_with_price = 'C',
  order_cancel = 'X',
  order_delete = 'D',
  order_replace = 'U',
  trade = 'P',
  cross_trade = 'Q',
  broken_trade = 'B',
  net_order_imbalance = 'I',
  retail_price_improvement = 'N',
};

// Every message begins with the same eleven bytes:
//
//   0      message type
//   1..2   stock locate, big endian uint16
//   3..4   tracking number, big endian uint16
//   5..10  timestamp, big endian 48-bit, nanoseconds since midnight
//
// The six byte timestamp is the first thing that makes this format awkward to
// handle: there is no integer type that width, so it has to be assembled by hand.
constexpr std::size_t HEADER_LENGTH = 11;

constexpr std::size_t OFFSET_MESSAGE_TYPE = 0;
constexpr std::size_t OFFSET_STOCK_LOCATE = 1;
constexpr std::size_t OFFSET_TRACKING_NUMBER = 3;
constexpr std::size_t OFFSET_TIMESTAMP = 5;

// Symbols are eight bytes, right padded with spaces rather than terminated.
constexpr std::size_t SYMBOL_LENGTH = 8;

// Prices are four byte unsigned integers with four implied decimal places, so
// 100.05 dollars arrives as 1000500. This is exactly the fixed point
// representation ob::Price uses, which is why PriceConfig defaults to a scale of
// 10000: no conversion is needed at the boundary beyond a widening cast.
constexpr std::int64_t PRICE_SCALE = 10000;

// Total on-wire length of each message including the type byte. Returns zero for a
// type this build does not recognise.
//
// Two independent things establish a message's length: this table, and the two byte
// length prefix that the sample files carry. The parser checks them against each
// other for known types, because a disagreement means the stream is corrupt or the
// version assumption is wrong, and finding that out immediately is far better than
// decoding garbage from a plausible looking offset.
[[nodiscard]] std::size_t message_length(char type) noexcept;

[[nodiscard]] bool is_known_type(char type) noexcept;

// ---------------------------------------------------------------------------
// Field offsets, per message type.
//
// These are spelled out rather than derived from a struct layout because
// reinterpret_cast over the buffer is undefined behaviour here: the fields are
// unaligned, an eight byte order reference number sits at offset 11, and the
// sanitizers correctly object. Named offsets and explicit byte assembly are the
// price of being able to run this under ubsan at all.
// ---------------------------------------------------------------------------

namespace system_event {
constexpr std::size_t LENGTH = 12;
constexpr std::size_t EVENT_CODE = 11;
}  // namespace system_event

namespace stock_directory {
constexpr std::size_t LENGTH = 39;
constexpr std::size_t STOCK = 11;
constexpr std::size_t MARKET_CATEGORY = 19;
constexpr std::size_t ROUND_LOT_SIZE = 21;
}  // namespace stock_directory

namespace add_order {
constexpr std::size_t LENGTH = 36;
constexpr std::size_t ORDER_REFERENCE = 11;
constexpr std::size_t BUY_SELL_INDICATOR = 19;
constexpr std::size_t SHARES = 20;
constexpr std::size_t STOCK = 24;
constexpr std::size_t PRICE = 32;
}  // namespace add_order

namespace add_order_with_mpid {
// Identical to add_order with a four byte attribution field appended, so the
// offsets above are reused rather than duplicated.
constexpr std::size_t LENGTH = 40;
constexpr std::size_t ATTRIBUTION = 36;
}  // namespace add_order_with_mpid

namespace order_executed {
constexpr std::size_t LENGTH = 31;
constexpr std::size_t ORDER_REFERENCE = 11;
constexpr std::size_t EXECUTED_SHARES = 19;
constexpr std::size_t MATCH_NUMBER = 23;
}  // namespace order_executed

namespace order_executed_with_price {
constexpr std::size_t LENGTH = 36;
constexpr std::size_t ORDER_REFERENCE = 11;
constexpr std::size_t EXECUTED_SHARES = 19;
constexpr std::size_t MATCH_NUMBER = 23;
constexpr std::size_t PRINTABLE = 31;
constexpr std::size_t EXECUTION_PRICE = 32;
}  // namespace order_executed_with_price

namespace order_cancel {
constexpr std::size_t LENGTH = 23;
constexpr std::size_t ORDER_REFERENCE = 11;
constexpr std::size_t CANCELLED_SHARES = 19;
}  // namespace order_cancel

namespace order_delete {
constexpr std::size_t LENGTH = 19;
constexpr std::size_t ORDER_REFERENCE = 11;
}  // namespace order_delete

namespace order_replace {
constexpr std::size_t LENGTH = 35;
constexpr std::size_t ORIGINAL_ORDER_REFERENCE = 11;
constexpr std::size_t NEW_ORDER_REFERENCE = 19;
constexpr std::size_t SHARES = 27;
constexpr std::size_t PRICE = 31;
}  // namespace order_replace

namespace trade {
constexpr std::size_t LENGTH = 44;
constexpr std::size_t ORDER_REFERENCE = 11;
constexpr std::size_t BUY_SELL_INDICATOR = 19;
constexpr std::size_t SHARES = 20;
constexpr std::size_t STOCK = 24;
constexpr std::size_t PRICE = 32;
constexpr std::size_t MATCH_NUMBER = 36;
}  // namespace trade

namespace cross_trade {
constexpr std::size_t LENGTH = 40;
// Cross trade share counts are eight bytes wide, not four like every other
// message. Auction volumes exceed what 32 bits holds.
constexpr std::size_t SHARES = 11;
constexpr std::size_t STOCK = 19;
constexpr std::size_t CROSS_PRICE = 27;
constexpr std::size_t MATCH_NUMBER = 31;
constexpr std::size_t CROSS_TYPE = 39;
}  // namespace cross_trade

namespace broken_trade {
constexpr std::size_t LENGTH = 19;
constexpr std::size_t MATCH_NUMBER = 11;
}  // namespace broken_trade

// Types the replay driver does not act on but must still skip by the correct
// length. They are listed so that message_length can answer for them, which is
// what keeps an unknown type from desynchronising a stream that has no length
// prefix.
namespace other_lengths {
constexpr std::size_t STOCK_TRADING_ACTION = 25;
constexpr std::size_t REG_SHO_RESTRICTION = 20;
constexpr std::size_t MARKET_PARTICIPANT_POSITION = 26;
constexpr std::size_t MWCB_DECLINE_LEVEL = 35;
constexpr std::size_t MWCB_STATUS = 12;
constexpr std::size_t IPO_QUOTING_PERIOD_UPDATE = 28;
constexpr std::size_t LULD_AUCTION_COLLAR = 35;
constexpr std::size_t OPERATIONAL_HALT = 21;
constexpr std::size_t NET_ORDER_IMBALANCE = 50;
constexpr std::size_t RETAIL_PRICE_IMPROVEMENT = 20;
}  // namespace other_lengths

// The largest message this format defines. Used to size the generator's staging
// buffer and to reject an obviously wrong length prefix before it is trusted.
constexpr std::size_t MAX_MESSAGE_LENGTH = other_lengths::NET_ORDER_IMBALANCE;

constexpr char BUY_INDICATOR = 'B';
constexpr char SELL_INDICATOR = 'S';

}  // namespace ob::itch
