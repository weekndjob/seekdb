/* Copyright (c) 2026 OceanBase. Licensed under Apache License, Version 2.0. */
#pragma once
#include "lib/container/ob_array.h"
#include "lib/utility/ob_unify_serialize.h"
#include "sql/engine/basic/file_csv_table.h"

namespace oceanbase { namespace sql {
// Only scan metadata enters the plan. Rows remain on disk and are streamed on
// every execution/rescan. ObString members decoded from a plan borrow its buffer.
struct FileScanDefinition
{
  enum Format : int64_t { CSV = 0, JSONL = 1, PARQUET = 2, GLOB = 3 };
  int64_t version_ = 1;
  int64_t format_ = CSV;
  bool header_ = true;
  int64_t delimiter_ = ',';
  int64_t quote_ = '"';
  int64_t escape_ = '"';
  common::ObString null_text_;
  common::ObArray<int64_t> identity_;
  common::ObArray<common::ObString> columns_;
  common::ObArray<int64_t> types_;
  common::ObArray<int64_t> nullable_;

  int serialize(char *buf, int64_t buf_len, int64_t &pos) const
  {
    int ret = common::OB_SUCCESS;
    OB_UNIS_ENCODE(version_);
    OB_UNIS_ENCODE(format_);
    OB_UNIS_ENCODE(header_);
    OB_UNIS_ENCODE(delimiter_);
    OB_UNIS_ENCODE(quote_);
    OB_UNIS_ENCODE(escape_);
    OB_UNIS_ENCODE(null_text_);
    OB_UNIS_ENCODE(identity_);
    OB_UNIS_ENCODE(columns_);
    OB_UNIS_ENCODE(types_);
    OB_UNIS_ENCODE(nullable_);
    return ret;
  }
  int64_t get_serialize_size() const
  {
    int64_t len = 0;
    OB_UNIS_ADD_LEN(version_);
    OB_UNIS_ADD_LEN(format_);
    OB_UNIS_ADD_LEN(header_);
    OB_UNIS_ADD_LEN(delimiter_);
    OB_UNIS_ADD_LEN(quote_);
    OB_UNIS_ADD_LEN(escape_);
    OB_UNIS_ADD_LEN(null_text_);
    OB_UNIS_ADD_LEN(identity_);
    OB_UNIS_ADD_LEN(columns_);
    OB_UNIS_ADD_LEN(types_);
    OB_UNIS_ADD_LEN(nullable_);
    return len;
  }
  int deserialize(const char *buf, int64_t data_len, int64_t &pos)
  {
    int ret = common::OB_SUCCESS;
    OB_UNIS_DECODE(version_);
    OB_UNIS_DECODE(format_);
    OB_UNIS_DECODE(header_);
    OB_UNIS_DECODE(delimiter_);
    OB_UNIS_DECODE(quote_);
    OB_UNIS_DECODE(escape_);
    OB_UNIS_DECODE(null_text_);
    OB_UNIS_DECODE(identity_);
    OB_UNIS_DECODE(columns_);
    OB_UNIS_DECODE(types_);
    OB_UNIS_DECODE(nullable_);
    if (ret == common::OB_SUCCESS && (version_ != 1 || format_ < CSV || format_ > GLOB
        || identity_.count() != 7 || columns_.count() < 1 || columns_.count() > 1024
        || columns_.count() != types_.count() || columns_.count() != nullable_.count())) {
      ret = common::OB_INVALID_DATA;
    }
    return ret;
  }
  int encode(common::ObIAllocator &allocator, common::ObString &result) const
  {
    const int64_t length = get_serialize_size();
    if (length <= 0 || length > 2 * 1024 * 1024) { return common::OB_SIZE_OVERFLOW; }
    char *buf = static_cast<char *>(allocator.alloc(length));
    if (!buf) { return common::OB_ALLOCATE_MEMORY_FAILED; }
    int64_t pos = 0;
    int ret = serialize(buf, length, pos);
    if (ret == common::OB_SUCCESS) { result.assign_ptr(buf, pos); }
    return ret;
  }
  int decode(const common::ObString &input)
  {
    if (input.length() > 2 * 1024 * 1024) { return common::OB_SIZE_OVERFLOW; }
    int64_t pos = 0;
    int ret = deserialize(input.ptr(), input.length(), pos);
    if (ret == common::OB_SUCCESS && pos != input.length()) { ret = common::OB_INVALID_DATA; }
    return ret;
  }
};
} }
