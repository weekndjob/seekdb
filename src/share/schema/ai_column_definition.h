/* Copyright (c) 2026 OceanBase. Licensed under Apache License, Version 2.0. */
#pragma once
#include "lib/container/ob_array.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_unify_serialize.h"
#include "share/ob_errno.h"

namespace oceanbase { namespace share { namespace schema {
// The column schema owns the encoded packet. Decoded strings borrow that
// packet; callers must keep its owner alive for the entire use of a definition.
struct AiColumnDefinition
{
  static constexpr int64_t MAX_PACKET = 256 * 1024;
  enum Function : int64_t { COMPLETE = 0, EMBED = 1 };
  int64_t format_version_ = 1;
  int64_t definition_version_ = 0;
  int64_t function_ = COMPLETE;
  int64_t max_retries_ = 2;
  int64_t dimension_ = 0;
  bool input_is_json_ = false;
  uint64_t state_column_id_ = 0;
  uint64_t request_column_id_ = 0;
  uint64_t result_spec_column_id_ = 0;
  common::ObString raw_expr_;
  common::ObString model_;
  common::ObString input_expr_;
  common::ObString options_expr_;
  common::ObString normalized_expr_;
  common::ObString result_type_;
  common::ObString system_prompt_;
  common::ObArray<uint64_t> dependencies_;
  common::ObArray<common::ObString> dependency_names_;

  int validate(bool finalized = true) const
  {
    using namespace common;
    int ret = OB_SUCCESS;
    if (!finalized) {
      return format_version_ == 1 && !raw_expr_.empty() && raw_expr_.length() <= MAX_PACKET
          && max_retries_ >= 0 && max_retries_ <= 100 && dependencies_.count() <= 1024
          && dependency_names_.count() == dependencies_.count() ? OB_SUCCESS : OB_INVALID_ARGUMENT;
    }
    if (format_version_ != 1 || function_ < COMPLETE || function_ > EMBED
        || max_retries_ < 0 || max_retries_ > 100 || dimension_ < 0 || dimension_ > 65535
        || model_.empty() || model_.length() > 128 || input_expr_.empty()
        || normalized_expr_.empty() || result_type_.empty() || dependencies_.count() > 1024
        || dependency_names_.count() != dependencies_.count()
        || (function_ == EMBED && dimension_ == 0)) {
      ret = OB_INVALID_ARGUMENT;
    } else if (finalized && (definition_version_ <= 0 || state_column_id_ == 0
        || request_column_id_ == 0 || result_spec_column_id_ == 0
        || state_column_id_ == request_column_id_ || state_column_id_ == result_spec_column_id_
        || request_column_id_ == result_spec_column_id_)) {
      ret = OB_INVALID_ARGUMENT;
    }
    for (int64_t i = 0; ret == OB_SUCCESS && i < dependencies_.count(); ++i) {
      if (dependencies_.at(i) == 0 || (i > 0 && dependencies_.at(i-1) >= dependencies_.at(i))) {
        ret = OB_INVALID_ARGUMENT;
      }
    }
    return ret;
  }
  int serialize(char *buf, int64_t buf_len, int64_t &pos) const
  {
    int ret = common::OB_SUCCESS;
    const int64_t count = dependencies_.count();
    LST_DO_CODE(OB_UNIS_ENCODE, format_version_, definition_version_, function_, max_retries_,
        dimension_, input_is_json_, state_column_id_, request_column_id_, result_spec_column_id_,
        raw_expr_, model_, input_expr_, options_expr_, normalized_expr_, result_type_, system_prompt_, count);
    for (int64_t i = 0; ret == common::OB_SUCCESS && i < count; ++i) {
      OB_UNIS_ENCODE(dependencies_.at(i));
      OB_UNIS_ENCODE(dependency_names_.at(i));
    }
    return ret;
  }
  int64_t get_serialize_size() const
  {
    int64_t len = 0;
    const int64_t count = dependencies_.count();
    LST_DO_CODE(OB_UNIS_ADD_LEN, format_version_, definition_version_, function_, max_retries_,
        dimension_, input_is_json_, state_column_id_, request_column_id_, result_spec_column_id_,
        raw_expr_, model_, input_expr_, options_expr_, normalized_expr_, result_type_, system_prompt_, count);
    for (int64_t i = 0; i < count; ++i) {
      OB_UNIS_ADD_LEN(dependencies_.at(i)); OB_UNIS_ADD_LEN(dependency_names_.at(i));
    }
    return len;
  }
  int encode(common::ObIAllocator &allocator, common::ObString &packet, bool finalized = true) const
  {
    using namespace common;
    int ret = validate(finalized);
    if (ret != OB_SUCCESS) { return ret; }
    const int64_t length = get_serialize_size();
    if (length <= 0 || length > MAX_PACKET) { return OB_SIZE_OVERFLOW; }
    char *buffer = static_cast<char *>(allocator.alloc(length));
    if (!buffer) { return OB_ALLOCATE_MEMORY_FAILED; }
    int64_t pos = 0;
    if (OB_SUCC(ret = serialize(buffer, length, pos))) { packet.assign_ptr(buffer, pos); }
    return ret;
  }
  int decode(const common::ObString &packet, bool finalized = true)
  {
    using namespace common;
    if (packet.empty() || packet.length() > MAX_PACKET) { return OB_INVALID_DATA; }
    const char *buf = packet.ptr();
    const int64_t data_len = packet.length();
    int64_t pos = 0;
    int64_t count = 0;
    int ret = OB_SUCCESS;
    dependencies_.reset();
    dependency_names_.reset();
    OB_UNIS_DECODE(format_version_);
    if (ret == OB_SUCCESS && format_version_ != 1) { return OB_INVALID_DATA; }
    LST_DO_CODE(OB_UNIS_DECODE, definition_version_, function_, max_retries_,
        dimension_, input_is_json_, state_column_id_, request_column_id_, result_spec_column_id_,
        raw_expr_, model_, input_expr_, options_expr_, normalized_expr_, result_type_, system_prompt_, count);
    if (ret == OB_SUCCESS && (count < 0 || count > 1024)) { ret = OB_INVALID_DATA; }
    for (int64_t i = 0; ret == OB_SUCCESS && i < count; ++i) {
      uint64_t column = 0;
      ObString name;
      OB_UNIS_DECODE(column);
      OB_UNIS_DECODE(name);
      if (ret == OB_SUCCESS) { ret = dependencies_.push_back(column); }
      if (ret == OB_SUCCESS) { ret = dependency_names_.push_back(name); }
    }
    if (ret == OB_SUCCESS && pos != data_len) { ret = OB_INVALID_DATA; }
    if (ret == OB_SUCCESS) { ret = validate(finalized); }
    return ret;
  }
};
} } }
