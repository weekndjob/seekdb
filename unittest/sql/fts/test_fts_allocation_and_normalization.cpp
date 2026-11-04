/* Copyright (c) 2026 OceanBase. Licensed under Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <random>
#include <set>
#include <memory>
#include <thread>
#include <string>
#include <map>
#include <unordered_set>
#include <vector>
#include "storage/fts/dict/ob_ft_range_dict.h"
#include "storage/fts/dict/ob_ft_cache_dict.h"
#include "storage/fts/dict/ob_ft_trie.h"
#include "storage/fts/ik/ob_ik_arbitrator.h"
#include "storage/fts/ik/ob_ik_processor.h"
#include "storage/fts/ob_fts_stop_word.h"
#include "storage/fts/ob_beng_ft_parser.h"
#include "data_plane/fts/ob_fts_parser_property.h"
#include "share/text_analysis/ob_text_analyzer.h"
#include "storage/retrieval/ob_sparse_daat_iter.h"
#include "storage/retrieval/ob_sparse_retrieval_util.h"
#include "data_plane/fts/ob_fts_parser.h"
#include "data_plane/fts/ob_fts_parser_helper.h"
// Include dependencies above before exposing just this class's cleanup path.
#define private public
#include "storage/fts/ob_ik_ft_parser.h"
#undef private

using namespace oceanbase::common;
using namespace oceanbase::storage;

namespace {
class TrackingAllocator final : public ObIAllocator
{
public:
  std::unordered_set<void *> live;
  int fail_after = -1;
  int resets = 0;
  void *alloc(int64_t size) override
  {
    if (fail_after == 0) { return nullptr; }
    if (fail_after > 0) { --fail_after; }
    void *ptr = std::malloc(size);
    if (ptr) { live.insert(ptr); }
    return ptr;
  }
  void *alloc(int64_t size, const ObMemAttr &) override { return alloc(size); }
  void free(void *ptr) override
  {
    if (ptr) {
      EXPECT_EQ(1U, live.erase(ptr));
      std::free(ptr);
    }
  }
  void reset() override { ++resets; }
  ~TrackingAllocator() override
  {
    EXPECT_TRUE(live.empty());
    for (void *ptr : live) { std::free(ptr); }
  }
};
}

TEST(FtsDefaultProperties, TypedDefaultsMatchCanonicalJsonForEveryBuiltin)
{
  TrackingAllocator allocator;
  ObFTParseHelper helper;
  for (const char *name : {"ik.1", "beng.1", "space.1", "ngram.1", "ngram2.1", "ik.1"}) {
    ObArenaAllocator json_allocator;
    ObFTParserJsonProps canonical;
    ASSERT_EQ(OB_SUCCESS, canonical.init());
    ASSERT_EQ(OB_SUCCESS, canonical.parse_from_valid_str(ObString()));
    ASSERT_EQ(OB_SUCCESS, canonical.rebuild_props_for_ddl(ObString(name), CS_TYPE_UTF8MB4_BIN, true));
    ObString json;
    ASSERT_EQ(OB_SUCCESS, canonical.to_format_json(json_allocator, json));
    ASSERT_EQ(OB_SUCCESS, helper.init_default(&allocator, ObString(name)));
    bool same = false;
    ASSERT_EQ(OB_SUCCESS, helper.check_is_the_same(ObString(name), json, same));
    EXPECT_TRUE(same) << name;
    EXPECT_TRUE(allocator.live.empty());
    EXPECT_EQ(0, allocator.resets);
    helper.reset();
  }
}

TEST(FtsDefaultProperties, FailedInitializationAndResetKeepErrorContracts)
{
  TrackingAllocator allocator;
  ObFTParseHelper helper;
  EXPECT_EQ(OB_INVALID_ARGUMENT, helper.init_default(nullptr, ObString("ik.1")));
  EXPECT_EQ(OB_INVALID_ARGUMENT, helper.init_default(&allocator, ObString()));
  EXPECT_NE(OB_SUCCESS, helper.init_default(&allocator, ObString("unknown.1")));
  ASSERT_EQ(OB_SUCCESS, helper.init_default(&allocator, ObString("ik.1")));
  EXPECT_EQ(OB_INIT_TWICE, helper.init_default(&allocator, ObString("beng.1")));
  EXPECT_EQ(OB_INIT_TWICE, helper.init_default(&allocator, ObString("ik.1")));
  helper.reset();
  ASSERT_EQ(OB_SUCCESS, helper.init_default(&allocator, ObString("beng.1")));
  helper.reset();
  EXPECT_TRUE(allocator.live.empty());
  EXPECT_EQ(0, allocator.resets);
}

TEST(FtsDictionaryPlacement, DenseBranchesPreserveEveryWordAndMissingEdge)
{
  ObArenaAllocator allocator;
  ObFTTrie<void> trie(allocator, CS_TYPE_UTF8MB4_BIN);
  std::set<std::string> words;
  for (int first = 0; first < 16; ++first) {
    for (int second = 0; second < 16; ++second) {
      for (int third = 0; third < 8; ++third) {
        std::string word;
        word += static_cast<char>('a' + first);
        word += static_cast<char>('a' + second);
        word += static_cast<char>('a' + third);
        words.insert(word);
        if (third % 2 == 0) { words.insert(word + "tail"); }
      }
    }
  }
  for (const auto &word : words) {
    ASSERT_EQ(OB_SUCCESS, trie.insert(ObString(word.size(), word.data()), {}));
  }
  ObFTDATBuilder<void> builder(allocator);
  ASSERT_EQ(OB_SUCCESS, builder.init(trie));
  ASSERT_EQ(OB_SUCCESS, builder.build_from_trie(trie));
  ObFTDAT *block = nullptr;
  size_t size = 0;
  ASSERT_EQ(OB_SUCCESS, builder.get_mem_block(block, size));
  ObFTCacheDict dictionary(CS_TYPE_UTF8MB4_BIN, block);
  for (const auto &word : words) {
    for (const auto &probe : {word, word + "z", word.substr(0, 2)}) {
      bool matched = false;
      ASSERT_EQ(OB_SUCCESS, dictionary.match(ObString(probe.size(), probe.data()), matched));
      EXPECT_EQ(words.count(probe) != 0, matched) << probe;
    }
  }
}

TEST(FtsRangeDictionary, BinaryBoundariesPreserveLegacySelection)
{
  ObArenaAllocator allocator;
  ObFTCacheRangeContainer container(allocator);
  const std::vector<std::set<std::string>> ranges = {
      {"a", "f", "z"}, {"m", "q"}, {"中", "文", "汉"}};
  std::vector<ObFTDAT *> data;
  std::vector<std::unique_ptr<ObDictCacheValue>> values;
  for (const auto &words : ranges) {
    ObFTTrie<void> trie(allocator, CS_TYPE_UTF8MB4_BIN);
    for (const auto &word : words) {
      ASSERT_EQ(OB_SUCCESS, trie.insert(ObString(word.size(), word.data()), {}));
    }
    ObFTDATBuilder<void> builder(allocator);
    ASSERT_EQ(OB_SUCCESS, builder.init(trie));
    ASSERT_EQ(OB_SUCCESS, builder.build_from_trie(trie));
    ObFTDAT *block = nullptr;
    size_t size = 0;
    ASSERT_EQ(OB_SUCCESS, builder.get_mem_block(block, size));
    data.push_back(block);
    values.emplace_back(new ObDictCacheValue(block));
    ObFTCacheRangeHandle *handle = nullptr;
    ASSERT_EQ(OB_SUCCESS, container.fetch_info_for_dict(handle));
    handle->value_ = values.back().get();
  }
  ObFTDictDesc description("test", ObFTDictType::DICT_IK_MAIN, CHARSET_UTF8MB4, CS_TYPE_UTF8MB4_BIN);
  ObFTRangeDict dictionary(allocator, &container, description);
  ASSERT_EQ(OB_SUCCESS, dictionary.init());
  std::vector<std::string> probes = {"中", "文", "汉", "一", "末", "河", "😀", "𠀀"};
  for (int i = 0; i < 256; ++i) { probes.emplace_back(1, static_cast<char>(i)); }
  for (const auto &probe : probes) {
    const ObString key(probe.size(), probe.data());
    size_t selected = ranges.size();
    for (size_t i = 0; i < data.size(); ++i) {
      if (ObCharset::strcmp(CS_TYPE_UTF8MB4_BIN, data[i]->start_word_.get_word(), key) <= 0
          && ObCharset::strcmp(CS_TYPE_UTF8MB4_BIN, data[i]->end_word_.get_word(), key) >= 0) {
        selected = i;
        break;
      }
    }
    ObDATrieHit hit(nullptr, 0);
    ASSERT_EQ(OB_SUCCESS, dictionary.match(key, hit));
    EXPECT_EQ(selected != ranges.size(), hit.dict_ != nullptr);
    EXPECT_EQ(selected != ranges.size() && ranges[selected].count(probe) != 0, hit.is_match());
  }
}

TEST(FtsRangeDictionary, ReleasesOnlyOwnAllocationsIncludingFailurePaths)
{
  TrackingAllocator allocator;
  void *sentinel = allocator.alloc(32);
  std::memset(sentinel, 0x5a, 32);
  {
    ObFTCacheRangeContainer container(allocator);
    ObFTCacheRangeHandle *handle = nullptr;
    ASSERT_EQ(OB_SUCCESS, container.fetch_info_for_dict(handle));
    ObFTDAT data;
    ObDictCacheValue value(&data);
    handle->value_ = &value;
    const auto baseline = allocator.live.size();
    ObFTDictDesc description("test", ObFTDictType::DICT_IK_MAIN, CHARSET_UTF8MB4, CS_TYPE_UTF8MB4_BIN);
    for (int limit : {-1, 0, 1, 2}) {
      allocator.fail_after = limit;
      {
        ObFTRangeDict dictionary(allocator, &container, description);
        const int result = dictionary.init();
        EXPECT_TRUE(result == OB_SUCCESS || result == OB_ALLOCATE_MEMORY_FAILED);
      }
      allocator.fail_after = -1;
      EXPECT_EQ(baseline, allocator.live.size());
      EXPECT_EQ(0, allocator.resets);
      EXPECT_EQ(0x5a, static_cast<unsigned char *>(sentinel)[0]);
    }
  }
  allocator.free(sentinel);
}

TEST(FtsParserLifecycle, RepeatedCleanupAfterPartialInitialization)
{
  TrackingAllocator allocator;
  ObFTCacheRangeContainer container(allocator);
  ObFTDictDesc description("test", ObFTDictType::DICT_IK_MAIN, CHARSET_UTF8MB4, CS_TYPE_UTF8MB4_BIN);
  {
    ObIKFTParser parser(allocator, nullptr);
    parser.ctx_ = OB_NEWx(TokenizeContext, &allocator,
        CS_TYPE_UTF8MB4_BIN, allocator, "abc", 3, false);
    ASSERT_NE(nullptr, parser.ctx_);
    parser.dict_main_ = OB_NEWx(ObFTRangeDict, &allocator, allocator, &container, description);
    ASSERT_NE(nullptr, parser.dict_main_);
    parser.reset(); // init() also invokes reset() on partial failure.
    EXPECT_TRUE(allocator.live.empty());
    parser.reset(); // The caller then destroys the failed parser.
    EXPECT_TRUE(allocator.live.empty());
  }
  EXPECT_TRUE(allocator.live.empty());
  EXPECT_EQ(0, allocator.resets);
}

TEST(FtsArbitration, ManyOrderedChainsAndOverlappingAlternatives)
{
  ObArenaAllocator allocator;
  std::string text;
  for (int i = 0; i < 80; ++i) { text += "abc "; }
  for (bool smart : {false, true}) {
    TokenizeContext context(CS_TYPE_UTF8MB4_BIN, allocator, text.data(), text.size(), smart);
    ASSERT_EQ(OB_SUCCESS, context.init());
    for (int i = 79; i >= 0; --i) {
      ASSERT_EQ(OB_SUCCESS, context.add_token(text.data(), 4 * i, 3, 3, ObIKTokenType::IK_ENGLISH_TOKEN));
    }
    ObIKArbitrator arbitration;
    ASSERT_EQ(OB_SUCCESS, arbitration.process(context));
    ASSERT_EQ(OB_SUCCESS, arbitration.output_result(context));
    int expected = 0;
    for (const auto &token : context.result_list()) {
      EXPECT_EQ(expected, token.offset_);
      EXPECT_EQ(3, token.length_);
      expected += 4;
    }
    EXPECT_EQ(320, expected);
    EXPECT_EQ(OB_INIT_TWICE, arbitration.process(context));
  }
  for (bool smart : {false, true}) {
    const char *text = "abcd";
    TokenizeContext context(CS_TYPE_UTF8MB4_BIN, allocator, text, 4, smart);
    ASSERT_EQ(OB_SUCCESS, context.init());
    ASSERT_EQ(OB_SUCCESS, context.add_token(text, 0, 4, 4, ObIKTokenType::IK_ENGLISH_TOKEN));
    ASSERT_EQ(OB_SUCCESS, context.add_token(text, 0, 2, 2, ObIKTokenType::IK_ENGLISH_TOKEN));
    ASSERT_EQ(OB_SUCCESS, context.add_token(text, 2, 2, 2, ObIKTokenType::IK_ENGLISH_TOKEN));
    ObIKArbitrator arbitration;
    ASSERT_EQ(OB_SUCCESS, arbitration.process(context));
    ASSERT_EQ(OB_SUCCESS, arbitration.output_result(context));
    EXPECT_EQ(smart ? 1 : 3, context.result_list().size());
    EXPECT_EQ(4, context.result_list().get_first().length_);
  }
}

TEST(FtsGrouping, KeepsLegacyFrequencyAndLastEquivalentWordRepresentation)
{
  ObArenaAllocator allocator;
  ObFTWordMap words;
  ASSERT_EQ(OB_SUCCESS, words.create(10, "FtsUnitMap"));
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
  ObAddWordFlag flags;
  flags.set_groupby_word();
  ObFTParserProperty properties;
  ObAddWord output(properties, meta, flags, allocator, words);
  ASSERT_EQ(OB_SUCCESS, output.process_word("HELLO", 5, 5, 2));
  ASSERT_EQ(OB_SUCCESS, output.process_word("hello", 5, 5, 3));
  ASSERT_EQ(1, words.size());
  int64_t frequency = 0;
  ASSERT_EQ(OB_SUCCESS, words.get_refactored(ObFTWord(5, "HELLO", meta), frequency));
  EXPECT_EQ(4, frequency); // Existing behavior: first occurrence starts at one.
  EXPECT_EQ(ObString("hello"), words.begin()->first.get_word().get_string());
  words.destroy();
}

TEST(FtsGrouping, ConcurrentDocumentsKeepIndependentCountsAndReuse)
{
  std::vector<std::thread> workers;
  for (int worker = 0; worker < 4; ++worker) {
    workers.emplace_back([worker] {
      ObArenaAllocator allocator;
      std::vector<std::string> keys;
      for (int i = 0; i < 10; ++i) {
        keys.push_back("worker" + std::to_string(worker) + "word" + std::to_string(i));
      }
      ObFTWordMap words;
      ASSERT_EQ(OB_SUCCESS, words.create(2, ObMemAttr("FTWordMapTest")));
      ObObjMeta meta;
      meta.set_varchar();
      meta.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      ObAddWordFlag flags;
      flags.set_groupby_word();
      ObFTParserProperty properties;
      for (int document = 0; document < 32; ++document) {
        words.reuse();
        ObAddWord output(properties, meta, flags, allocator, words);
        for (int pass = 0; pass < 4; ++pass) {
          for (const auto &key : keys) {
            ASSERT_EQ(OB_SUCCESS, output.process_word(
                key.data(), key.size(), key.size(), document + 1));
          }
        }
        ASSERT_EQ(10, words.size());
        for (const auto &key : keys) {
          int64_t frequency = 0;
          ASSERT_EQ(OB_SUCCESS, words.get_refactored(ObFTWord(key.size(), key.data(), meta), frequency));
          EXPECT_EQ(1 + 3 * (document + 1), frequency);
        }
      }
      words.destroy();
    });
  }
  for (auto &worker : workers) { worker.join(); }
}

TEST(FtsEnglishNormalization, ParserTokensSurviveAdvanceAndResetWithoutSecondCopy)
{
  TrackingAllocator allocator;
  std::vector<std::pair<const char *, int64_t>> saved;
  const std::vector<std::string> expected = {"alpha", "beta", "caf\xc3\xa9ine", "gamma"};
  std::string text = "ALPHA BETA caf\xc3\xa9ine GAMMA";
  {
    ObBEngFTParser parser(allocator);
    ObFTParserParam param;
    param.cs_ = ObCharset::get_charset(CS_TYPE_UTF8MB4_BIN);
    param.fulltext_ = text.data();
    param.ft_length_ = text.size();
    param.allocator_ = &allocator;
    ASSERT_EQ(OB_SUCCESS, parser.init(&param));
    for (const auto &word : expected) {
      const size_t before = allocator.live.size();
      const char *ptr = nullptr;
      int64_t length = 0, chars = 0, frequency = 0;
      ASSERT_EQ(OB_SUCCESS, parser.get_next_token(ptr, length, chars, frequency));
      EXPECT_EQ(word, std::string(ptr, length));
      EXPECT_EQ(1, frequency);
      EXPECT_EQ(before + 1, allocator.live.size()); // one stable normalized buffer
      saved.emplace_back(ptr, length);
    }
    const char *ptr = nullptr;
    int64_t length = 0, chars = 0, frequency = 0;
    EXPECT_EQ(OB_ITER_END, parser.get_next_token(ptr, length, chars, frequency));
    parser.reset();
    parser.reset();
  }
  text.assign(text.size(), 'x');
  ASSERT_EQ(expected.size(), allocator.live.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(expected[i], std::string(saved[i].first, saved[i].second));
    allocator.free(const_cast<char *>(saved[i].first));
  }
  EXPECT_TRUE(allocator.live.empty());
  EXPECT_EQ(0, allocator.resets);
}

TEST(FtsEnglishNormalization, CallerAllocationFailureDoesNotResetCallerStorage)
{
  TrackingAllocator allocator;
  void *sentinel = allocator.alloc(16);
  ASSERT_NE(nullptr, sentinel);
  {
    ObBEngFTParser parser(allocator);
    ObFTParserParam param;
    param.cs_ = ObCharset::get_charset(CS_TYPE_UTF8MB4_BIN);
    param.fulltext_ = "ALPHA";
    param.ft_length_ = 5;
    param.allocator_ = &allocator;
    ASSERT_EQ(OB_SUCCESS, parser.init(&param));
    allocator.fail_after = 0;
    const char *ptr = nullptr;
    int64_t length = 0, chars = 0, frequency = 0;
    EXPECT_EQ(OB_ALLOCATE_MEMORY_FAILED, parser.get_next_token(ptr, length, chars, frequency));
    EXPECT_EQ(nullptr, ptr);
    EXPECT_EQ(0, length);
    allocator.fail_after = -1;
    parser.reset();
  }
  EXPECT_EQ(1U, allocator.live.size());
  EXPECT_EQ(0, allocator.resets);
  allocator.free(sentinel);
}

TEST(FtsEnglishNormalization, LowercaseUppercaseAndUnicodeAcrossDocuments)
{
  using namespace oceanbase::share;
  ObArenaAllocator allocator;
  ObTextAnalysisCtx config;
  config.cs_ = ObCharset::get_charset(CS_TYPE_UTF8MB4_BIN);
  config.filter_stopword_ = false;
  ObEnglishTextAnalyzer analyzer;
  ASSERT_EQ(OB_SUCCESS, analyzer.init(config, allocator));
  for (const auto &item : std::vector<std::pair<std::string, std::vector<std::string>>>{
      {"lowercase abc123 UPPER Mixed", {"lowercase", "abc123", "upper", "mixed"}},
      {"caf\xc3\xa9ine d\xc3\xa9j\xc3\xa0vu END", {"caf\xc3\xa9ine", "d\xc3\xa9j\xc3\xa0vu", "end"}},
      {"next document lower", {"next", "document", "lower"}}}) {
    ObDatum document;
    document.set_string(item.first.data(), item.first.size());
    ObITokenStream *stream = nullptr;
    ASSERT_EQ(OB_SUCCESS, analyzer.analyze(document, stream));
    for (const auto &expected : item.second) {
      ObDatum token;
      int64_t frequency = 0;
      ASSERT_EQ(OB_SUCCESS, stream->get_next(token, frequency));
      EXPECT_EQ(expected, std::string(token.ptr_, token.len_));
      EXPECT_EQ(1, frequency);
    }
    ObDatum token;
    int64_t frequency = 0;
    EXPECT_EQ(OB_ITER_END, stream->get_next(token, frequency));
  }
}

namespace {
class MergeFixtureDimension : public ObISRDaaTDimIter
{
public:
  MergeFixtureDimension() : position_(0), current_id_(0) { datum_.ptr_ = reinterpret_cast<const char *>(&current_id_); }
  int get_next_row() override
  {
    if (position_ == rows_.size()) { return OB_ITER_END; }
    current_id_ = rows_[position_++];
    datum_.set_uint(current_id_);
    return OB_SUCCESS;
  }
  int get_curr_score(double &score) const override { score = score_; return OB_SUCCESS; }
  int get_curr_id(const ObDatum *&datum) const override { datum = &datum_; return OB_SUCCESS; }
  std::vector<uint64_t> rows_;
  double score_ = 0;
private:
  size_t position_;
  uint64_t current_id_;
  ObDatum datum_;
};

class MergeFixture : public ObSRDaaTIterImpl
{
public:
  int prepare(ObIAllocator &allocator, ObIArray<ObISRDaaTDimIter *> &dims,
              ObSparseRetrievalMergeParam &param, ObSRDaaTRelevanceCollector &collector)
  {
    int ret = OB_SUCCESS;
    iter_allocator_ = &allocator;
    dim_iters_ = &dims;
    iter_param_ = &param;
    relevance_collector_ = &collector;
    iter_domain_ids_.set_allocator(&allocator);
    next_round_iter_idxes_.set_allocator(&allocator);
    oceanbase::sql::ObDatumMeta meta;
    meta.type_ = ObUInt64Type;
    meta.cs_type_ = CS_TYPE_BINARY;
    if (OB_FAIL(iter_domain_ids_.init(dims.count()))) {
    } else if (OB_FAIL(iter_domain_ids_.prepare_allocate(dims.count()))) {
    } else if (OB_FAIL(next_round_iter_idxes_.init(dims.count()))) {
    } else if (OB_FAIL(next_round_iter_idxes_.prepare_allocate(dims.count()))) {
    } else if (OB_FAIL(merge_cmp_.init(meta, &iter_domain_ids_))) {
    } else if (OB_FAIL(init_merge_heap(dims.count()))) {
    } else {
      next_round_cnt_ = dims.count();
      for (int64_t i = 0; i < dims.count(); ++i) { next_round_iter_idxes_[i] = i; }
    }
    return ret;
  }
  int next(uint64_t &id, double &score)
  {
    int ret = fill_merge_heap();
    const ObDatum *datum = nullptr;
    bool valid = false;
    if (OB_SUCCESS == ret) {
      ret = collect_dims_by_id(datum, score, valid);
      if (OB_SUCCESS == ret) {
        if (!valid || datum == nullptr) { ret = OB_ERR_UNEXPECTED; }
        else { id = datum->get_uint(); }
      }
    }
    return ret;
  }
};
}

TEST(FtsDaatMerge, SmallAndLargeMergersPreserveWeightedUnionAndEndOfInput)
{
  for (int64_t count = 1; count <= 5; ++count) {
    ObArenaAllocator allocator;
    MergeFixtureDimension dimensions[5];
    ObSEArray<ObISRDaaTDimIter *, 5> inputs;
    ObSEArray<double, 5> weights;
    std::map<uint64_t, double> expected;
    for (int64_t dim = 0; dim < count; ++dim) {
      dimensions[dim].score_ = static_cast<double>(1U << dim);
      ASSERT_EQ(OB_SUCCESS, inputs.push_back(&dimensions[dim]));
      ASSERT_EQ(OB_SUCCESS, weights.push_back(dim + 1.0));
      for (uint64_t id = 0; id < 44; ++id) {
        // Includes an exhausted dimension and both shared and disjoint IDs.
        if (dim != 1 && (id + dim) % 3 != 0) {
          dimensions[dim].rows_.push_back(id);
          expected[id] += dimensions[dim].score_ * 2.0 * (dim + 1.0);
        }
      }
    }
    ObSparseRetrievalMergeParam param;
    param.dim_weights_ = &weights;
    param.field_boost_ = 2.0;
    ObSRDaaTInnerProductRelevanceCollector collector;
    ASSERT_EQ(OB_SUCCESS, collector.init());
    MergeFixture merge;
    ASSERT_EQ(OB_SUCCESS, merge.prepare(allocator, inputs, param, collector));
    for (const auto &row : expected) {
      uint64_t actual_id = UINT64_MAX;
      double actual_score = -1;
      ASSERT_EQ(OB_SUCCESS, merge.next(actual_id, actual_score)) << count;
      EXPECT_EQ(row.first, actual_id) << count;
      EXPECT_DOUBLE_EQ(row.second, actual_score) << count;
    }
    uint64_t id = 0;
    double score = 0;
    EXPECT_EQ(OB_ITER_END, merge.next(id, score));
    EXPECT_EQ(OB_ITER_END, merge.next(id, score));
    merge.reset();
  }
}

TEST(FtsDocumentIdComparison, BinaryFastPathIsEquivalentToOriginalComparator)
{
  ObArenaAllocator allocator;
  ObFixedArray<const ObDatum *, ObIAllocator> ids(allocator);
  ASSERT_EQ(OB_SUCCESS, ids.init(2));
  ASSERT_EQ(OB_SUCCESS, ids.prepare_allocate(2));
  oceanbase::sql::ObDatumMeta meta;
  meta.type_ = ObVarcharType;
  meta.cs_type_ = CS_TYPE_BINARY;
  ObSRMergeCmp comparator;
  ASSERT_EQ(OB_SUCCESS, comparator.init(meta, &ids));
  ObObjMeta object_meta;
  object_meta.set_varchar();
  object_meta.set_collation_type(CS_TYPE_BINARY);
  ObDomainIdCmp domain_comparator;
  ASSERT_EQ(OB_SUCCESS, domain_comparator.init(object_meta));
  auto original = ObDatumFuncs::get_basic_func(meta.type_, meta.cs_type_)->null_first_cmp_;
  std::vector<std::string> values = {"", "a", "a ", "a  ", "b", std::string("a\0", 2),
                                   std::string("\0", 1), std::string("\xff", 1)};
  std::mt19937 generator(20261102);
  for (int i = 0; i < 100; ++i) {
    std::string value(generator() % 40, '\0');
    for (auto &byte : value) { byte = static_cast<char>(generator() & 255); }
    values.push_back(value);
  }
  ObSRMergeItem left_item;
  ObSRMergeItem right_item;
  left_item.iter_idx_ = 0;
  right_item.iter_idx_ = 1;
  for (int i = -1; i < static_cast<int>(values.size()); ++i) {
    for (int j = -1; j < static_cast<int>(values.size()); ++j) {
      ObDatum left;
      ObDatum right;
      if (i < 0) { left.set_null();
      } else { left.set_string(values[i].data(), values[i].size()); }
      if (j < 0) { right.set_null();
      } else { right.set_string(values[j].data(), values[j].size()); }
      ids.at(0) = &left;
      ids.at(1) = &right;
      int expected = 0;
      int64_t actual = 0;
      ASSERT_EQ(OB_SUCCESS, original(left, right, expected, nullptr));
      ASSERT_EQ(OB_SUCCESS, comparator.cmp(left_item, right_item, actual));
      EXPECT_EQ((expected > 0) - (expected < 0), (actual > 0) - (actual < 0));
      int domain_actual = 0;
      ASSERT_EQ(OB_SUCCESS, domain_comparator.compare(left, right, domain_actual));
      EXPECT_EQ((expected > 0) - (expected < 0), (domain_actual > 0) - (domain_actual < 0));
    }
  }
  meta.type_ = ObUInt64Type;
  ASSERT_EQ(OB_SUCCESS, comparator.init(meta, &ids));
  ObDatum left;
  ObDatum right;
  uint64_t left_storage = 0;
  uint64_t right_storage = UINT64_MAX;
  left.ptr_ = reinterpret_cast<const char *>(&left_storage);
  right.ptr_ = reinterpret_cast<const char *>(&right_storage);
  left.set_uint(0);
  right.set_uint(UINT64_MAX);
  ids.at(0) = &left;
  ids.at(1) = &right;
  int64_t actual = 0;
  ASSERT_EQ(OB_SUCCESS, comparator.cmp(left_item, right_item, actual));
  EXPECT_LT(actual, 0);
}

TEST(FtsDocumentIdComparison, BlockMaxBoundsAndReset)
{
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(CS_TYPE_BINARY);
  ObDomainIdCmp comparator;
  ObObj minimum;
  ObObj maximum;
  minimum.set_min_value();
  maximum.set_max_value();
  ObDatum low;
  ObDatum high;
  low.ptr_ = reinterpret_cast<const char *>(&minimum);
  high.ptr_ = reinterpret_cast<const char *>(&maximum);
  low.set_ext();
  high.set_ext();
  ObDatum value;
  value.set_string("abc");
  int result = 0;
  EXPECT_EQ(OB_NOT_INIT, comparator.compare(value, value, result));
  ASSERT_EQ(OB_SUCCESS, comparator.init(meta));
  for (const auto &pair : std::vector<std::pair<const ObDatum *, const ObDatum *>>{
      {&low, &value}, {&value, &high}, {&low, &high}}) {
    ASSERT_EQ(OB_SUCCESS, comparator.compare(*pair.first, *pair.second, result));
    EXPECT_LT(result, 0);
    ASSERT_EQ(OB_SUCCESS, comparator.compare(*pair.second, *pair.first, result));
    EXPECT_GT(result, 0);
  }
  ASSERT_EQ(OB_SUCCESS, comparator.compare(low, low, result));
  EXPECT_EQ(0, result);
  ASSERT_EQ(OB_SUCCESS, comparator.compare(high, high, result));
  EXPECT_EQ(0, result);
  comparator.reset();
  EXPECT_EQ(OB_NOT_INIT, comparator.compare(value, value, result));
}
