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

TEST_F(AIPromptTest, completion_rejects_missing_empty_or_non_string_content)
{
  ObOpenAIUtils::ObOpenAIComplete provider;
  const char *invalid[] = {
    R"({})", R"({"choices":[]})",
    R"({"choices":[{"message":{"content":null}}]})",
    R"({"choices":[{"message":{"content":123}}]})",
    R"({"choices":[{"message":{"content":""}}]})"
  };
  for (const char *json : invalid) {
    ObIJsonBase *result = nullptr;
    EXPECT_EQ(OB_INVALID_DATA, provider.parse_output(allocator_, parse(json), result)) << json;
  }
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator_,
      parse(R"({"choices":[{"message":{"content":"answer"}}]})"), result));
  ASSERT_NE(nullptr, result);
  EXPECT_EQ(ObJsonNodeType::J_STRING, result->json_type());
  EXPECT_EQ("answer", std::string(static_cast<ObJsonString *>(result)->get_str().ptr(),
                                  static_cast<ObJsonString *>(result)->get_str().length()));
}

TEST_F(AIPromptTest, completion_forwards_configuration_fields)
{
  ObOpenAIUtils::ObOpenAIComplete provider;
  ObString model = ObString::make_string("example-model");
  ObString system;
  ObString content = ObString::make_string("question");
  ObJsonObject *config = parse(R"({"temperature":0.2,"max_tokens":16})");
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.get_body(allocator_, model, system, content, config, body));
  ASSERT_NE(nullptr, body);
  ObString rendered;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::print_json_to_str(allocator_, body, rendered));
  const std::string json(rendered.ptr(), rendered.length());
  EXPECT_NE(std::string::npos, json.find("\"temperature\": 0.2"));
  EXPECT_NE(std::string::npos, json.find("\"max_tokens\": 16"));
  EXPECT_NE(std::string::npos, json.find("\"question\""));
}

TEST_F(AIPromptTest, embedding_response_maps_indices_and_rejects_invalid_shapes)
{
  ObOpenAIUtils::ObOpenAIEmbed provider;
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator_,
      parse(R"({"data":[{"index":1,"embedding":[3,4]},{"index":0,"embedding":[1,2]}]})"), result));
  ObString rendered;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::print_json_to_str(allocator_, result, rendered));
  EXPECT_EQ("[[1, 2], [3, 4]]", std::string(rendered.ptr(), rendered.length()));

  const char *invalid[] = {
    R"({"data":{}})", R"({"data":[{"index":0,"embedding":[1]},{"index":0,"embedding":[2]}]})",
    R"({"data":[{"index":1,"embedding":[1]}]})", R"({"data":[{"embedding":[1]}]})",
    R"({"data":[{"index":-1,"embedding":[1]}]})", R"({"data":[{"index":0,"embedding":"bad"}]})"
  };
  for (const char *json : invalid) {
    result = nullptr;
    EXPECT_EQ(OB_INVALID_DATA, provider.parse_output(allocator_, parse(json), result)) << json;
  }
}

TEST_F(AIPromptTest, ollama_endpoint_and_native_completion_protocol)
{
  oceanbase::share::ObAiModelEndpointInfo endpoint;
  ObString name = ObString::make_string("local_endpoint");
  ASSERT_EQ(OB_SUCCESS, endpoint.parse_from_json_base(allocator_, name,
      *parse(R"({"scope":"all","ai_model_name":"m","url":"http://127.0.0.1:1/api/generate","access_key":"","provider":"Ollama"})")));
  ObString key;
  ASSERT_EQ(OB_SUCCESS, endpoint.get_unencrypted_access_key(allocator_, key));
  EXPECT_TRUE(key.empty());
  ObAIFuncIComplete *provider = nullptr;
  ObString provider_name = ObString::make_string("oLlAmA");
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_complete_provider(allocator_, provider_name, provider));
  ASSERT_NE(nullptr, provider);
  ObAIFuncIEmbed *embed = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_embed_provider(allocator_, provider_name, embed));
  ASSERT_NE(nullptr, embed);
  ObArray<ObString> headers;
  ASSERT_EQ(OB_SUCCESS, provider->get_header(allocator_, key, headers));
  ASSERT_EQ(1, headers.count());
  EXPECT_EQ(0, headers.at(0).compare("Content-Type: application/json"));
  ObString model = ObString::make_string("model");
  ObString system;
  ObString content = ObString::make_string("question");
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider->get_body(allocator_, model, system, content,
      parse(R"({"stream":true,"temperature":0.2})"), body));
  ASSERT_NE(nullptr, body->get_value("stream"));
  EXPECT_EQ(ObJsonNodeType::J_BOOLEAN, body->get_value("stream")->json_type());
  EXPECT_FALSE(body->get_value("stream")->get_boolean());
  ObIJsonBase *result = nullptr;
  EXPECT_EQ(OB_INVALID_DATA, provider->parse_output(allocator_, parse(R"({"response":""})"), result));
  ASSERT_EQ(OB_SUCCESS, provider->parse_output(allocator_, parse(R"({"response":"answer"})"), result));
  ASSERT_NE(nullptr, result);
  EXPECT_EQ(ObJsonNodeType::J_STRING, result->json_type());
}
