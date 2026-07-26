#include "itch/parser.hpp"

#include <cstddef>
#include <cstdint>

#include "itch/messages.hpp"

namespace ob::itch {

// A switch rather than a lookup table. The type codes are scattered across the
// ASCII range, so a table would be a 256 entry array that is almost entirely zero,
// and both compilers turn a dense switch like this into a jump table or a small
// comparison tree anyway.
std::size_t message_length(char type) noexcept {
  switch (static_cast<MessageType>(type)) {
    case MessageType::system_event:
      return system_event::LENGTH;
    case MessageType::stock_directory:
      return stock_directory::LENGTH;
    case MessageType::stock_trading_action:
      return other_lengths::STOCK_TRADING_ACTION;
    case MessageType::reg_sho_restriction:
      return other_lengths::REG_SHO_RESTRICTION;
    case MessageType::market_participant_position:
      return other_lengths::MARKET_PARTICIPANT_POSITION;
    case MessageType::mwcb_decline_level:
      return other_lengths::MWCB_DECLINE_LEVEL;
    case MessageType::mwcb_status:
      return other_lengths::MWCB_STATUS;
    case MessageType::ipo_quoting_period_update:
      return other_lengths::IPO_QUOTING_PERIOD_UPDATE;
    case MessageType::luld_auction_collar:
      return other_lengths::LULD_AUCTION_COLLAR;
    case MessageType::operational_halt:
      return other_lengths::OPERATIONAL_HALT;
    case MessageType::add_order:
      return add_order::LENGTH;
    case MessageType::add_order_with_mpid:
      return add_order_with_mpid::LENGTH;
    case MessageType::order_executed:
      return order_executed::LENGTH;
    case MessageType::order_executed_with_price:
      return order_executed_with_price::LENGTH;
    case MessageType::order_cancel:
      return order_cancel::LENGTH;
    case MessageType::order_delete:
      return order_delete::LENGTH;
    case MessageType::order_replace:
      return order_replace::LENGTH;
    case MessageType::trade:
      return trade::LENGTH;
    case MessageType::cross_trade:
      return cross_trade::LENGTH;
    case MessageType::broken_trade:
      return broken_trade::LENGTH;
    case MessageType::net_order_imbalance:
      return other_lengths::NET_ORDER_IMBALANCE;
    case MessageType::retail_price_improvement:
      return other_lengths::RETAIL_PRICE_IMPROVEMENT;
  }
  return 0;
}

bool is_known_type(char type) noexcept {
  return message_length(type) != 0;
}

// The write side, used only by the synthetic generator and the round trip test.
// Kept beside the read side so that a change to one is obviously a change to the
// other, which is what makes the round trip test meaningful.
void store_be16(std::byte* data, std::uint16_t value) noexcept {
  data[0] = static_cast<std::byte>((value >> 8U) & 0xFFU);
  data[1] = static_cast<std::byte>(value & 0xFFU);
}

void store_be32(std::byte* data, std::uint32_t value) noexcept {
  data[0] = static_cast<std::byte>((value >> 24U) & 0xFFU);
  data[1] = static_cast<std::byte>((value >> 16U) & 0xFFU);
  data[2] = static_cast<std::byte>((value >> 8U) & 0xFFU);
  data[3] = static_cast<std::byte>(value & 0xFFU);
}

void store_be48(std::byte* data, std::uint64_t value) noexcept {
  data[0] = static_cast<std::byte>((value >> 40U) & 0xFFU);
  data[1] = static_cast<std::byte>((value >> 32U) & 0xFFU);
  data[2] = static_cast<std::byte>((value >> 24U) & 0xFFU);
  data[3] = static_cast<std::byte>((value >> 16U) & 0xFFU);
  data[4] = static_cast<std::byte>((value >> 8U) & 0xFFU);
  data[5] = static_cast<std::byte>(value & 0xFFU);
}

void store_be64(std::byte* data, std::uint64_t value) noexcept {
  data[0] = static_cast<std::byte>((value >> 56U) & 0xFFU);
  data[1] = static_cast<std::byte>((value >> 48U) & 0xFFU);
  data[2] = static_cast<std::byte>((value >> 40U) & 0xFFU);
  data[3] = static_cast<std::byte>((value >> 32U) & 0xFFU);
  data[4] = static_cast<std::byte>((value >> 24U) & 0xFFU);
  data[5] = static_cast<std::byte>((value >> 16U) & 0xFFU);
  data[6] = static_cast<std::byte>((value >> 8U) & 0xFFU);
  data[7] = static_cast<std::byte>(value & 0xFFU);
}

}  // namespace ob::itch
