/* Copyright (c) 2026 OceanBase. Licensed under Apache License, Version 2.0. */
#pragma once
#include <charconv>
#include <chrono>
#include <cmath>
#include "common/object/ob_object.h"
#include "sql/engine/basic/file_csv_reader.h"

namespace oceanbase { namespace sql {
class FileValueConversion
{
public:
  static bool integer(common::ObString value, int64_t &result)
  {
    value = value.trim();
    if (value.empty()) { return false; }
    const char *start = value.ptr(), *end = start + value.length();
    if (*start == '+') {
      ++start;
      if (start == end || *start == '+' || *start == '-') { return false; }
    }
    const auto parsed = std::from_chars(start, end, result);
    return parsed.ec == std::errc{} && parsed.ptr == end;
  }
  static bool floating(common::ObString value, double &result)
  {
    value = value.trim();
    if (value.empty()) { return false; }
    const char *start = value.ptr(), *end = start + value.length();
    if (*start == '+') {
      ++start;
      if (start == end || *start == '+' || *start == '-') { return false; }
    }
    const auto parsed = std::from_chars(start, end, result);
    return parsed.ec == std::errc{} && parsed.ptr == end && std::isfinite(result);
  }
  static bool date(common::ObString value, int32_t &days)
  {
    if (value.length() != 10 || value[4] != '-' || value[7] != '-') { return false; }
    int year = 0, month = 0, day = 0;
    if (!digits(value.ptr(), 4, year) || !digits(value.ptr()+5, 2, month)
        || !digits(value.ptr()+8, 2, day) || year < 1 || year > 9999) { return false; }
    const std::chrono::year_month_day date{std::chrono::year{year},
        std::chrono::month{static_cast<unsigned>(month)}, std::chrono::day{static_cast<unsigned>(day)}};
    if (!date.ok()) { return false; }
    days = static_cast<int32_t>(std::chrono::sys_days{date}.time_since_epoch().count());
    return true;
  }
  static bool datetime(common::ObString value, int64_t &micros)
  {
    if (value.length() < 19 || value.length() > 26 || (value[10] != ' ' && value[10] != 'T')
        || value[13] != ':' || value[16] != ':') { return false; }
    int32_t days = 0;
    int hour = 0, minute = 0, second = 0, fraction = 0;
    if (!date(common::ObString(10, value.ptr()), days) || !digits(value.ptr()+11, 2, hour)
        || !digits(value.ptr()+14, 2, minute) || !digits(value.ptr()+17, 2, second)
        || hour > 23 || minute > 59 || second > 59) { return false; }
    if (value.length() > 19) {
      const int length = value.length() - 20;
      if (value[19] != '.' || length < 1 || !digits(value.ptr()+20, length, fraction)) { return false; }
      for (int i = length; i < 6; ++i) { fraction *= 10; }
    }
    micros = ((static_cast<int64_t>(days)*24 + hour)*3600 + minute*60 + second)*1000000 + fraction;
    return true;
  }
  static common::ObObjType infer(const CsvField &field)
  {
    using namespace common;
    if (field.value_.empty() && !field.quoted_) { return ObNullType; }
    const ObString value = field.value_.trim();
    if (value.case_compare("true") == 0 || value.case_compare("false") == 0) { return ObTinyIntType; }
    int64_t integer_value = 0;
    double double_value = 0;
    int32_t days = 0;
    if (integer(value, integer_value)) { return ObIntType; }
    // Overflowing integer text stays text so its exact digits are preserved.
    bool decimal_notation = false;
    for (int64_t i = 0; i < value.length(); ++i) {
      if (value[i] == '.' || value[i] == 'e' || value[i] == 'E') { decimal_notation = true; }
    }
    if (decimal_notation && floating(value, double_value)) { return ObDoubleType; }
    if (date(value, days)) { return ObDateType; }
    if (datetime(value, integer_value)) { return ObDateTimeType; }
    return ObVarcharType;
  }
  static common::ObObjType widen(common::ObObjType a, common::ObObjType b)
  {
    using namespace common;
    if (a == ObNullType) { return b; }
    if (b == ObNullType || a == b) { return a; }
    if ((a == ObIntType && b == ObDoubleType) || (b == ObIntType && a == ObDoubleType)) { return ObDoubleType; }
    if ((a == ObDateType && b == ObDateTimeType) || (b == ObDateType && a == ObDateTimeType)) { return ObDateTimeType; }
    return ObVarcharType;
  }
  static int convert(const CsvField &field, common::ObObjType type, common::ObObj &result)
  {
    using namespace common;
    int64_t i = 0;
    int32_t days = 0;
    double d = 0;
    if (field.value_.empty() && !field.quoted_) { result.set_null(); return OB_SUCCESS; }
    const ObString value = field.value_.trim();
    switch (type) {
      case ObVarcharType: result.set_varchar(field.value_); result.set_collation_type(CS_TYPE_UTF8MB4_BIN); break;
      case ObIntType: if (!integer(value, i)) { return OB_INVALID_DATA; } result.set_int(i); break;
      case ObDoubleType: if (!floating(value, d)) { return OB_INVALID_DATA; } result.set_double(d); break;
      case ObTinyIntType:
        if (value.case_compare("true") == 0) { result.set_tinyint(1); }
        else if (value.case_compare("false") == 0) { result.set_tinyint(0); }
        else { return OB_INVALID_DATA; }
        break;
      case ObDateType: if (!date(value, days)) { return OB_INVALID_DATA; } result.set_date(days); break;
      case ObDateTimeType:
        if (date(value, days)) { i = static_cast<int64_t>(days)*86400000000LL; }
        else if (!datetime(value, i)) { return OB_INVALID_DATA; }
        result.set_datetime(i); break;
      default: return OB_INVALID_DATA;
    }
    return OB_SUCCESS;
  }
private:
  static bool digits(const char *text, int length, int &result)
  {
    result = 0;
    for (int i = 0; i < length; ++i) {
      if (text[i] < '0' || text[i] > '9') { return false; }
      result = result*10 + text[i]-'0';
    }
    return length > 0;
  }
};
} }
