#include <gtest/gtest.h>
#include <string>
#include <unordered_set>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "lib/allocator/ob_allocator.h"

using namespace oceanbase::common;

class FreeTrackingAllocator : public ObWrapperAllocator
{
public:
  explicit FreeTrackingAllocator(ObIAllocator &allocator) : ObWrapperAllocator(allocator) {}
  void free(void *ptr) override
  {
    freed_.insert(ptr);
    ObWrapperAllocator::free(ptr);
  }
  std::unordered_set<const void *> freed_;
};

class AIPromptTest : public ::testing::Test
{
protected:
  ObArenaAllocator allocator_;

  ObJsonObject *parse(const char *json)
  {
    ObString input = ObString::make_string(json);
    ObJsonObject *object = nullptr;
    EXPECT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator_, input, object));
    return object;
  }

  void expect_render(const char *json, const std::string &expected)
  {
    ObString result;
    ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
        allocator_, parse(json), result));
    EXPECT_EQ(expected, std::string(result.ptr() == nullptr ? "" : result.ptr(), result.length()));
  }
};

TEST_F(AIPromptTest, repeats_and_reorders_without_truncation)
{
  const std::string argument(4096, 'x');
  const std::string json = "{\"template\":\"{1}/{0}{0}{0}{0}{0}\",\"args\":[\""
      + argument + "\",\"end\"]}";
  expect_render(json.c_str(), "end/" + argument + argument + argument + argument + argument);
}

TEST_F(AIPromptTest, json_array_object_and_scalars_keep_structure)
{
  expect_render(R"({"template":"{0}/{1}/{2}/{3}","args":[[1,true],{"k":2},null,3]})",
                "[1, true]/{\"k\": 2}/null/3");
}

TEST_F(AIPromptTest, nested_prompts_render_recursively)
{
  expect_render(R"({"template":"outer {0}/{0}","args":[{"template":"inner {0}","args":["世界"]}]})",
                "outer inner 世界/inner 世界");
}

TEST_F(AIPromptTest, unicode_escaping_and_embedded_nul_keep_bytes)
{
  expect_render(R"({"template":"{{0}}:{0}:\\\n","args":["你\u0000好"]})",
                std::string("{0}:你") + std::string(1, '\0') + "好:\\\n");
}

TEST_F(AIPromptTest, empty_argument_can_produce_empty_result)
{
  expect_render(R"({"template":"{0}","args":[""]})", "");
  expect_render(R"({"template":"literal {word} {}","args":[]})", "literal {word} {}");
}

TEST_F(AIPromptTest, output_remains_owned_by_caller_allocator)
{
  FreeTrackingAllocator tracked(allocator_);
  ObString result;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
      tracked, parse(R"({"template":"{0}/{0}","args":["value"]})"), result));
  EXPECT_EQ(0U, tracked.freed_.count(result.ptr()));
  EXPECT_EQ("value/value", std::string(result.ptr(), result.length()));

  ObString serialized;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::print_json_to_str(
      tracked, parse(R"({"k":"value"})"), serialized));
  EXPECT_EQ(0U, tracked.freed_.count(serialized.ptr()));
  EXPECT_EQ("{\"k\": \"value\"}", std::string(serialized.ptr(), serialized.length()));
}

TEST_F(AIPromptTest, malformed_shapes_are_rejected_before_casting)
{
  const char *invalid[] = {
    R"({"template":1,"args":[]})", R"({"template":"x","args":"x"})",
    R"({"template":"x"})", R"({"template":"","args":[]})",
    R"({"template":"x","args":[],"extra":0})"
  };
  for (const char *json : invalid) {
    ObString result;
    EXPECT_EQ(OB_INVALID_ARGUMENT, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
        allocator_, parse(json), result)) << json;
  }
  ObString result;
  EXPECT_EQ(OB_INVALID_ARGUMENT, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
      allocator_, nullptr, result));
}

TEST_F(AIPromptTest, invalid_and_overflowing_indices_are_rejected)
{
  const char *templates[] = {"{1}", "{-1}", "{+0}", "{0x}", "{0",
      "{9223372036854775808}", "{18446744073709551616}", "{999999999999999999999999999999}"};
  for (const char *text : templates) {
    const std::string json = std::string("{\"template\":\"") + text + "\",\"args\":[\"x\"]}";
    ObString result;
    EXPECT_EQ(OB_INVALID_ARGUMENT, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
        allocator_, parse(json.c_str()), result)) << text;
  }
}

TEST_F(AIPromptTest, recursion_limit_rejects_excessively_nested_prompts)
{
  ObJsonObject *object = parse(R"({"template":"leaf","args":[]})");
  for (int i = 0; i < 64; ++i) {
    ObJsonArray *args = nullptr;
    ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator_, args));
    ASSERT_EQ(OB_SUCCESS, args->append(object));
    ObString text = ObString::make_string("{0}");
    ObJsonObject *outer = nullptr;
    ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::construct_prompt_object(
        allocator_, text, args, outer));
    object = outer;
  }
  ObString result;
  EXPECT_EQ(OB_INVALID_ARGUMENT, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
      allocator_, object, result));
}
