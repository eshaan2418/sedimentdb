#pragma once
// Little-endian fixed-width and varint encodings, plus CRC32C.

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace sediment {

inline void PutFixed32(std::string* dst, uint32_t v) {
  char b[4];
  std::memcpy(b, &v, 4);  // all supported targets are little-endian
  dst->append(b, 4);
}
inline void PutFixed64(std::string* dst, uint64_t v) {
  char b[8];
  std::memcpy(b, &v, 8);
  dst->append(b, 8);
}
inline uint32_t DecodeFixed32(const char* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}
inline uint64_t DecodeFixed64(const char* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return v;
}
inline void EncodeFixed32(char* p, uint32_t v) { std::memcpy(p, &v, 4); }
inline void EncodeFixed64(char* p, uint64_t v) { std::memcpy(p, &v, 8); }

inline void PutVarint64(std::string* dst, uint64_t v) {
  while (v >= 0x80) {
    dst->push_back(static_cast<char>(v | 0x80));
    v >>= 7;
  }
  dst->push_back(static_cast<char>(v));
}
inline void PutVarint32(std::string* dst, uint32_t v) { PutVarint64(dst, v); }

// Parses a varint from the front of `in`, advancing it. Returns false on
// truncated or overlong input.
inline bool GetVarint64(std::string_view* in, uint64_t* v) {
  uint64_t result = 0;
  for (int shift = 0; shift <= 63 && !in->empty(); shift += 7) {
    auto byte = static_cast<uint8_t>((*in)[0]);
    in->remove_prefix(1);
    result |= static_cast<uint64_t>(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) {
      *v = result;
      return true;
    }
  }
  return false;
}
inline bool GetVarint32(std::string_view* in, uint32_t* v) {
  uint64_t x;
  if (!GetVarint64(in, &x) || x > UINT32_MAX) return false;
  *v = static_cast<uint32_t>(x);
  return true;
}

inline void PutLengthPrefixed(std::string* dst, std::string_view s) {
  PutVarint32(dst, static_cast<uint32_t>(s.size()));
  dst->append(s);
}
inline bool GetLengthPrefixed(std::string_view* in, std::string_view* out) {
  uint32_t n;
  if (!GetVarint32(in, &n) || in->size() < n) return false;
  *out = in->substr(0, n);
  in->remove_prefix(n);
  return true;
}

namespace crc32c {
// Uses the ARMv8 CRC32C instructions when available, else a table.
uint32_t Extend(uint32_t crc, const char* data, size_t n);
inline uint32_t Value(const char* data, size_t n) { return Extend(0, data, n); }
// Masked CRCs avoid pathological CRC-of-CRC values when checksums are stored
// inside checksummed data (as LevelDB does).
inline uint32_t Mask(uint32_t crc) { return ((crc >> 15) | (crc << 17)) + 0xa282ead8u; }
inline uint32_t Unmask(uint32_t m) {
  uint32_t rot = m - 0xa282ead8u;
  return (rot >> 17) | (rot << 15);
}
bool HardwareAccelerated();
}  // namespace crc32c

}  // namespace sediment
