// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#include "data_plane/fts/ob_fts_parser_property.h"
#include "lib/allocator/ob_allocator.h"
#include "storage/fts/ob_fts_stop_word.h"
#include <gtest/gtest.h>

using namespace oceanbase::common;
using namespace oceanbase::storage;

TEST(FtsWordAggregation, PreservesFirstOccurrenceAndParserFrequencies) {
  ASSERT_EQ(OB_SUCCESS, ObCharset::init_charset());
  ObArenaAllocator allocator;
  ObFTWordMap words;
  ASSERT_EQ(OB_SUCCESS, words.create(2, ObMemAttr("FtsAggTest")));
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  ObFTParserProperty property;
  ObAddWordFlag flags;
  flags.set_groupby_word();
  ObAddWord add(property, meta, flags, allocator, words);
  ASSERT_EQ(OB_SUCCESS, add.process_word("repeat", 6, 6, 5));
  int64_t count = 0;
  ASSERT_EQ(OB_SUCCESS,
            words.get_refactored(ObFTWord(6, "repeat", meta), count));
  EXPECT_EQ(1, count);
  ASSERT_EQ(OB_SUCCESS, add.process_word("repeat", 6, 6, 7));
  ASSERT_EQ(OB_SUCCESS,
            words.get_refactored(ObFTWord(6, "repeat", meta), count));
  EXPECT_EQ(8, count);
  EXPECT_EQ(12, add.get_add_word_count());
  EXPECT_EQ(OB_INVALID_ARGUMENT, add.process_word("", 0, 0, 1));
  EXPECT_EQ(OB_INVALID_ARGUMENT, add.process_word("repeat", 6, 6, 0));
}

TEST(FtsWordAggregation, KeepsCollationEquivalenceAndDistinctWords) {
  ASSERT_EQ(OB_SUCCESS, ObCharset::init_charset());
  for (const auto coll : {CS_TYPE_UTF8MB4_BIN, CS_TYPE_UTF8MB4_GENERAL_CI}) {
    ObArenaAllocator allocator;
    ObFTWordMap words;
    ASSERT_EQ(OB_SUCCESS, words.create(2, ObMemAttr("FtsAggTest")));
    ObObjMeta meta;
    meta.set_varchar();
    meta.set_collation_type(coll);
    ObFTParserProperty property;
    ObAddWordFlag flags;
    flags.set_groupby_word();
    ObAddWord add(property, meta, flags, allocator, words);
    ASSERT_EQ(OB_SUCCESS, add.process_word("Database", 8, 8, 1));
    ASSERT_EQ(OB_SUCCESS, add.process_word("database", 8, 8, 1));
    ASSERT_EQ(OB_SUCCESS, add.process_word("database", 8, 8, 1));
    ASSERT_EQ(OB_SUCCESS, add.process_word("index", 5, 5, 1));
    int64_t count = 0;
    ASSERT_EQ(OB_SUCCESS,
              words.get_refactored(ObFTWord(8, "database", meta), count));
    EXPECT_EQ(coll == CS_TYPE_UTF8MB4_BIN ? 2 : 3, count);
    EXPECT_EQ(coll == CS_TYPE_UTF8MB4_BIN ? 3 : 2, words.size());
    EXPECT_EQ(4, add.get_add_word_count());
  }
}

TEST(FtsWordAggregation, PreservesNoGroupingDuplicateError) {
  ObArenaAllocator allocator;
  ObFTWordMap words;
  ASSERT_EQ(OB_SUCCESS, words.create(2, ObMemAttr("FtsAggTest")));
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  ObFTParserProperty property;
  ObAddWordFlag flags;
  ObAddWord add(property, meta, flags, allocator, words);
  ASSERT_EQ(OB_SUCCESS, add.process_word("repeat", 6, 6, 1));
  EXPECT_EQ(OB_HASH_EXIST, add.process_word("repeat", 6, 6, 1));
}
