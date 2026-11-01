#include <gtest/gtest.h>
#include "sql/engine/basic/file_value_conversion.h"
using namespace oceanbase::common;
using oceanbase::sql::CsvField;
using oceanbase::sql::FileValueConversion;

TEST(FileValueConversion, PreservesIntegerBoundariesAndRejectsNonFiniteNumbers)
{
  int64_t integer = 0;
  double floating = 0;
  EXPECT_TRUE(FileValueConversion::integer(ObString::make_string("-9223372036854775808"), integer));
  EXPECT_EQ(INT64_MIN, integer);
  EXPECT_FALSE(FileValueConversion::integer(ObString::make_string("9223372036854775808"), integer));
  EXPECT_FALSE(FileValueConversion::integer(ObString::make_string("+-1"), integer));
  EXPECT_FALSE(FileValueConversion::floating(ObString::make_string("+-1.0"), floating));
  EXPECT_EQ(ObVarcharType, FileValueConversion::infer(CsvField{ObString::make_string("9223372036854775808"), false}));
  EXPECT_FALSE(FileValueConversion::floating(ObString::make_string("NaN"), floating));
  EXPECT_FALSE(FileValueConversion::floating(ObString::make_string("1e999"), floating));
  EXPECT_EQ(ObDoubleType, FileValueConversion::infer(CsvField{ObString::make_string("1.25e2"), false}));
}

TEST(FileValueConversion, ValidatesCalendarAndFractionalTimes)
{
  int32_t days = 1;
  int64_t value = 1;
  EXPECT_TRUE(FileValueConversion::date(ObString::make_string("1970-01-01"), days));
  EXPECT_EQ(0, days);
  EXPECT_TRUE(FileValueConversion::date(ObString::make_string("2024-02-29"), days));
  EXPECT_FALSE(FileValueConversion::date(ObString::make_string("2023-02-29"), days));
  EXPECT_FALSE(FileValueConversion::date(ObString::make_string("2026-13-01"), days));
  EXPECT_TRUE(FileValueConversion::datetime(ObString::make_string("1970-01-01T00:00:01.25"), value));
  EXPECT_EQ(1250000, value);
  EXPECT_FALSE(FileValueConversion::datetime(ObString::make_string("2026-01-01 24:00:00"), value));
  EXPECT_FALSE(FileValueConversion::datetime(ObString::make_string("2026-01-01 00:00:00."), value));
}

TEST(FileValueConversion, WideningAndNullsKeepValuesConsistent)
{
  EXPECT_EQ(ObDoubleType, FileValueConversion::widen(ObIntType, ObDoubleType));
  EXPECT_EQ(ObDateTimeType, FileValueConversion::widen(ObDateType, ObDateTimeType));
  EXPECT_EQ(ObVarcharType, FileValueConversion::widen(ObIntType, ObVarcharType));
  EXPECT_EQ(ObNullType, FileValueConversion::infer(CsvField{ObString(), false}));
  EXPECT_EQ(ObVarcharType, FileValueConversion::infer(CsvField{ObString(), true}));
  ObObj output;
  EXPECT_EQ(OB_SUCCESS, FileValueConversion::convert(CsvField{ObString::make_string("true"), false}, ObTinyIntType, output));
  EXPECT_EQ(1, output.get_tinyint());
  EXPECT_EQ(OB_SUCCESS, FileValueConversion::convert(CsvField{ObString(), false}, ObIntType, output));
  EXPECT_TRUE(output.is_null());
}
