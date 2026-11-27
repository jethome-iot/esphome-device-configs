#pragma once

#include "esphome/components/modbus/modbus.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace esphome::modbus_tcp {

/// Transaction id(2) + protocol id(2) + length(2) + unit id(1).
static constexpr size_t MBAP_HEADER_SIZE = 7;
/// The MBAP length field counts the unit id and the PDU.
static constexpr size_t MAX_ADU_SIZE = MBAP_HEADER_SIZE + modbus::MAX_PDU_SIZE;

/// The unit id a client sends when it addresses the device by its IP alone (Modbus TCP 4.4.2).
static constexpr uint8_t UNIT_ID_DIRECT = 0xFF;

enum class FrameStatus : uint8_t {
  INCOMPLETE,  ///< More bytes are needed.
  COMPLETE,    ///< The first `size` bytes are one request.
  INVALID,     ///< Not Modbus: the stream cannot be resynchronised, so the connection goes.
};

struct Frame {
  FrameStatus status;
  size_t size;
};

/// Where the first request in @p buffer ends.
Frame find_frame(std::span<const uint8_t> buffer);

/// Answers one request PDU as the RTU hub answers it: the same function codes, the same checks
/// and the same exceptions, from the same device handlers. Returns the length of the response PDU
/// written to @p response, which must hold modbus::MAX_PDU_SIZE bytes.
size_t answer_pdu(modbus::ModbusServerDevice &device, std::span<const uint8_t> request, std::span<uint8_t> response);

/// Answers one complete request (as find_frame() delimited it) addressed to @p unit_id. Returns the
/// length of the response written to @p response, which must hold MAX_ADU_SIZE bytes.
size_t answer_adu(modbus::ModbusServerDevice &device, uint8_t unit_id, std::span<const uint8_t> request,
                  std::span<uint8_t> response);

}  // namespace esphome::modbus_tcp
