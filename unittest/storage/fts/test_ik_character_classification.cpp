/* Copyright (c) 2026 OceanBase. Licensed under Apache License, Version 2.0. */
#include "storage/fts/ik/ob_ik_char_util.h"
#include "storage/fts/ob_fts_stop_word.h"
#include <gtest/gtest.h>

using namespace oceanbase::common;
using oceanbase::storage::ObFTCharUtil;

TEST(IkCharacterClassification, Utf8AndUtf16Boundaries) {
  ASSERT_EQ(OB_SUCCESS, ObCharset::init_charset());
  using Type = ObFTCharUtil::CharType;
  struct Case {
    unsigned code;
    Type expected;
  };
  const Case cases[] = {{0, Type::USELESS},
                        {0x20, Type::USELESS},
                        {0x2f, Type::USELESS},
                        {'0', Type::ARABIC_LETTER},
                        {'9', Type::ARABIC_LETTER},
                        {':', Type::USELESS},
                        {'A', Type::ENGLISH_LETTER},
                        {'Z', Type::ENGLISH_LETTER},
                        {'[', Type::USELESS},
                        {'a', Type::ENGLISH_LETTER},
                        {'z', Type::ENGLISH_LETTER},
                        {0xff21, Type::ENGLISH_LETTER},
                        {0xff5a, Type::ENGLISH_LETTER},
                        {0xff10, Type::ARABIC_LETTER},
                        {0xff19, Type::ARABIC_LETTER},
                        {0x4e2d, Type::CHINESE},
                        {0x3042, Type::OTHER_CJK},
                        {0xac00, Type::OTHER_CJK},
                        {0x1f600, Type::USELESS}};
  for (const auto &item : cases) {
    char utf8[4];
    unsigned n = 0;
    const unsigned c = item.code;
    if (c < 0x80) {
      utf8[n++] = c;
    } else if (c < 0x800) {
      utf8[n++] = 0xc0 | (c >> 6);
      utf8[n++] = 0x80 | (c & 63);
    } else if (c < 0x10000) {
      utf8[n++] = 0xe0 | (c >> 12);
      utf8[n++] = 0x80 | ((c >> 6) & 63);
      utf8[n++] = 0x80 | (c & 63);
    } else {
      utf8[n++] = 0xf0 | (c >> 18);
      utf8[n++] = 0x80 | ((c >> 12) & 63);
      utf8[n++] = 0x80 | ((c >> 6) & 63);
      utf8[n++] = 0x80 | (c & 63);
    }
    Type actual;
    ASSERT_EQ(OB_SUCCESS, ObFTCharUtil::classify_first_char(CS_TYPE_UTF8MB4_BIN,
                                                            utf8, n, actual));
    EXPECT_EQ(item.expected, actual) << c;
    if (c < 0x10000) {
      char be[] = {static_cast<char>(c >> 8), static_cast<char>(c)};
      char le[] = {static_cast<char>(c), static_cast<char>(c >> 8)};
      for (const auto coll : {CS_TYPE_UTF16_BIN, CS_TYPE_UTF16LE_BIN}) {
        const char *bytes = coll == CS_TYPE_UTF16_BIN ? be : le;
        if (ObCharset::charset_type_by_coll(coll) == CHARSET_INVALID) {
          EXPECT_EQ(OB_NOT_SUPPORTED,
                    ObFTCharUtil::classify_first_char(coll, bytes, 2, actual));
        } else {
          ASSERT_EQ(OB_SUCCESS,
                    ObFTCharUtil::classify_first_char(coll, bytes, 2, actual));
          EXPECT_EQ(item.expected, actual) << c;
        }
      }
    }
  }
}

TEST(IkCharacterClassification, InvalidAndTruncatedUtf8KeepsErrorBehavior) {
  ObFTCharUtil::CharType actual;
  EXPECT_EQ(OB_INVALID_ARGUMENT, ObFTCharUtil::classify_first_char(
                                     CS_TYPE_UTF8MB4_BIN, nullptr, 1, actual));
  const char invalid[] = {static_cast<char>(0xff)};
  const char truncated[] = {static_cast<char>(0xe4), static_cast<char>(0xb8)};
  EXPECT_NE(OB_SUCCESS, ObFTCharUtil::classify_first_char(CS_TYPE_UTF8MB4_BIN,
                                                          invalid, 1, actual));
  EXPECT_NE(OB_SUCCESS, ObFTCharUtil::classify_first_char(
                            CS_TYPE_UTF8MB4_BIN, truncated, 2, actual));
}

TEST(FtsStopWords, PreservesStopwordCollationAcrossInputCollations) {
  oceanbase::storage::ObStopWordChecker checker;
  ASSERT_EQ(OB_SUCCESS, checker.init());
  for (const auto coll : {CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF8MB4_GENERAL_CI,
                          CS_TYPE_UTF8MB4_UNICODE_CI, CS_TYPE_BINARY}) {
    ObObjMeta meta;
    meta.set_varchar();
    meta.set_collation_type(coll);
    for (const char *text :
         {"the", "THE", "ThE", "a", "www", "oceanbase", "database"}) {
      const bool expected =
          STRCMP(text, "oceanbase") != 0 && STRCMP(text, "database") != 0;
      oceanbase::storage::ObFTWord word(STRLEN(text), text, meta);
      bool actual = !expected;
      ASSERT_EQ(OB_SUCCESS, checker.check_stopword(word, actual));
      EXPECT_EQ(expected, actual) << text << " collation=" << coll;
    }
    oceanbase::storage::ObFTWord empty(0, "", meta);
    bool ignored = false;
    EXPECT_EQ(OB_INVALID_ARGUMENT, checker.check_stopword(empty, ignored));
  }
}
