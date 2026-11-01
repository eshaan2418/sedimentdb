#pragma once

#include <string>
#include <utility>

namespace sediment {

class Status {
 public:
  enum class Code { kOk, kNotFound, kCorruption, kIOError, kInvalidArgument };

  Status() = default;
  static Status OK() { return {}; }
  static Status NotFound(std::string m = {}) { return {Code::kNotFound, std::move(m)}; }
  static Status Corruption(std::string m) { return {Code::kCorruption, std::move(m)}; }
  static Status IOError(std::string m) { return {Code::kIOError, std::move(m)}; }
  static Status InvalidArgument(std::string m) { return {Code::kInvalidArgument, std::move(m)}; }

  [[nodiscard]] bool ok() const { return code_ == Code::kOk; }
  [[nodiscard]] bool IsNotFound() const { return code_ == Code::kNotFound; }
  [[nodiscard]] bool IsCorruption() const { return code_ == Code::kCorruption; }
  [[nodiscard]] bool IsIOError() const { return code_ == Code::kIOError; }
  [[nodiscard]] Code code() const { return code_; }
  [[nodiscard]] const std::string& message() const { return msg_; }
  [[nodiscard]] std::string ToString() const;

 private:
  Status(Code c, std::string m) : code_(c), msg_(std::move(m)) {}
  Code code_ = Code::kOk;
  std::string msg_;
};

}  // namespace sediment
