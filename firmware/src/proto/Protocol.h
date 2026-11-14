#pragma once

#include <Arduino.h>

#include "config/AppConfig.h"

// Бинарный протокол колонки. Формат описан в docs/02-audio-protocol.md.
//
//  0        1        2        3        4        5        6
// +--------+--------+--------+--------+--------+--------+--------- - - -
// |  type  | flags  |  streamId (u16 LE)  |  seq (u16 LE)   |  payload
// +--------+--------+--------+--------+--------+--------+--------- - - -
namespace proto {

enum Type : uint8_t {
  HELLO         = 0x01,
  HELLO_ACK     = 0x81,
  MIC           = 0x02,
  SAY           = 0x03,
  SPEECH_BEGIN  = 0x04,
  SPEECH_END    = 0x05,
  VAD           = 0x06,
  CONTROL       = 0x07,
  STATE         = 0x08,
  EVENT         = 0x09,
  CMD           = 0x0A,
  CODEC         = 0x0B,
  INTERCOM      = 0x0C,
  MEDIA         = 0x0D,
};

enum Flags : uint8_t {
  FLAG_BEGIN = 0x01,
  FLAG_END   = 0x02,
  FLAG_RETRY = 0x04,
  FLAG_MARK  = 0x08,
};

constexpr size_t HEADER_SIZE = 6;
constexpr size_t MAX_PAYLOAD = 1200;

struct Frame {
  uint8_t type = 0;
  uint8_t flags = 0;
  uint16_t streamId = 0;
  uint16_t seq = 0;
  const uint8_t* payload = nullptr;
  size_t length = 0;
};

inline void writeHeader(uint8_t* out, uint8_t type, uint8_t flags, uint16_t streamId, uint16_t seq) {
  out[0] = type;
  out[1] = flags;
  out[2] = static_cast<uint8_t>(streamId & 0xFF);
  out[3] = static_cast<uint8_t>(streamId >> 8);
  out[4] = static_cast<uint8_t>(seq & 0xFF);
  out[5] = static_cast<uint8_t>(seq >> 8);
}

inline Frame readFrame(const uint8_t* data, size_t length) {
  Frame frame;
  if (length < HEADER_SIZE) {
    return frame;
  }
  frame.type = data[0];
  frame.flags = data[1];
  frame.streamId = static_cast<uint16_t>(data[2] | (data[3] << 8));
  frame.seq = static_cast<uint16_t>(data[4] | (data[5] << 8));
  frame.payload = data + HEADER_SIZE;
  frame.length = length - HEADER_SIZE;
  return frame;
}

}  // namespace proto
