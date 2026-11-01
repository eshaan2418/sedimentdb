#pragma once
// Internal keys: user_key + fixed64(sequence << 8 | type). Ordered by user
// key ascending, then sequence descending, so a seek to (key, snapshot_seq)
// lands on the newest version visible at that snapshot.

#include <cstdint>
#include <string>
#include <string_view>

#include "coding.h"

namespace sediment {

using SequenceNumber = uint64_t;
enum ValueType : uint8_t { kTypeDeletion = 0, kTypeValue = 1 };
constexpr SequenceNumber kMaxSequence = (uint64_t{1} << 56) - 1;
constexpr int kNumLevels = 7;

inline uint64_t PackSeqType(SequenceNumber s, ValueType t) { return (s << 8) | t; }

inline void AppendInternalKey(std::string* dst, std::string_view user, SequenceNumber s, ValueType t) {
  dst->append(user);
  PutFixed64(dst, PackSeqType(s, t));
}

inline std::string MakeInternalKey(std::string_view user, SequenceNumber s, ValueType t) {
  std::string k;
  AppendInternalKey(&k, user, s, t);
  return k;
}

inline std::string_view ExtractUserKey(std::string_view ikey) { return ikey.substr(0, ikey.size() - 8); }
inline uint64_t ExtractTag(std::string_view ikey) { return DecodeFixed64(ikey.data() + ikey.size() - 8); }
inline SequenceNumber ExtractSeq(std::string_view ikey) { return ExtractTag(ikey) >> 8; }
inline ValueType ExtractType(std::string_view ikey) { return static_cast<ValueType>(ExtractTag(ikey) & 0xff); }

inline int CompareUser(std::string_view a, std::string_view b) {
  int c = a.compare(b);
  return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

inline int CompareInternal(std::string_view a, std::string_view b) {
  int c = CompareUser(ExtractUserKey(a), ExtractUserKey(b));
  if (c != 0) return c;
  uint64_t ta = ExtractTag(a), tb = ExtractTag(b);
  return ta > tb ? -1 : (ta < tb ? 1 : 0);
}

// The key to seek for "user_key as of snapshot s".
inline std::string LookupKey(std::string_view user, SequenceNumber s) { return MakeInternalKey(user, s, kTypeValue); }

}  // namespace sediment
