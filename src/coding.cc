#include "coding.h"

#include "sediment/status.h"

#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#define SEDIMENT_HW_CRC 1
#endif

namespace sediment {

std::string Status::ToString() const {
  const char* names[] = {"OK", "NotFound", "Corruption", "IOError", "InvalidArgument"};
  std::string s = names[static_cast<int>(code_)];
  if (!msg_.empty()) s += ": " + msg_;
  return s;
}

namespace crc32c {

namespace {
struct Table {
  uint32_t t[256];
  Table() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0x82f63b78u : c >> 1;
      t[i] = c;
    }
  }
};
[[maybe_unused]] const Table& table() {
  static const Table tab;
  return tab;
}
}  // namespace

bool HardwareAccelerated() {
#ifdef SEDIMENT_HW_CRC
  return true;
#else
  return false;
#endif
}

uint32_t Extend(uint32_t crc, const char* data, size_t n) {
  uint32_t c = ~crc;
  const auto* p = reinterpret_cast<const unsigned char*>(data);
#ifdef SEDIMENT_HW_CRC
  for (; n >= 8; n -= 8, p += 8) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    c = __crc32cd(c, v);
  }
  for (; n > 0; --n, ++p) c = __crc32cb(c, *p);
#else
  const Table& tab = table();
  for (; n > 0; --n, ++p) c = tab.t[(c ^ *p) & 0xff] ^ (c >> 8);
#endif
  return ~c;
}

}  // namespace crc32c
}  // namespace sediment
