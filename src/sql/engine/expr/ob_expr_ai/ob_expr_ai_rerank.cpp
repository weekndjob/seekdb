/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_rerank.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "lib/utility/utility.h"
#include "common/json_type/ob_json_common.h"
#include "share/json/ob_json_helper.h"
#include <algorithm>
#include <cmath>
 
using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{

ObExprAIRerank::ObExprAIRerank(common::ObIAllocator &alloc)
: ObFuncExprOperator(alloc, 
                    T_FUN_SYS_AI_RERANK, 
                    N_AI_RERANK, 
                    MORE_THAN_ZERO,
                    NOT_VALID_FOR_GENERATED_COL, 
                    NOT_ROW_DIMENSION) 
{
}

ObExprAIRerank::~ObExprAIRerank() 
{
}

int ObExprAIRerank::calc_result_typeN(ObExprResType &type,
                                    ObExprResType *types_stack,
                                    int64_t param_num,
                                    common::ObExprTypeCtx &type_ctx) const {
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num < 3 || param_num > 4)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    if (!ob_is_string_tc(types_stack[MODEL_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model key must be string type");
    } else if (!ob_is_string_tc(types_stack[QUERY_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, query must be string type");
    } else {
      types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      types_stack[QUERY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    }
    ObObjType in_type = types_stack[DOCUMENTS_IDX].get_type();
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types_stack, DOCUMENTS_IDX, N_AI_RERANK))) {
    } else if (ob_is_string_type(in_type) && types_stack[DOCUMENTS_IDX].get_collation_type() != CS_TYPE_BINARY) {
      if (types_stack[DOCUMENTS_IDX].get_charset_type() != CHARSET_UTF8MB4) {
        types_stack[DOCUMENTS_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (param_num == 4) {
      ObObjType doc_key_type = types_stack[DOC_KEY_IDX].get_type();
      if (doc_key_type == ObNullType) {
      } else if (doc_key_type == ObJsonType) {
      } else if (ob_is_string_type(doc_key_type)) {
        types_stack[DOC_KEY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      } else {  
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key must be string type");
      }
    }

    if (OB_SUCC(ret)) {
      type.set_json();
      type.set_length((ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType]).get_length());
    }
  }
  return ret;
}

struct RerankEntry
{
  int64_t index_;
  double score_;
  ObJsonObject *result_;
  ObJsonNode *score_node_;
};

int ObExprAIRerank::eval_ai_rerank(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  int ret = OB_SUCCESS;
  ObDatum *model_arg = nullptr;
  ObDatum *query_arg = nullptr;
  ObDatum *docs_arg = nullptr;
  ObDatum *key_arg = nullptr;
  if (OB_FAIL(expr.eval_param_value(ctx, model_arg, query_arg, docs_arg, key_arg))) {
  } else if (model_arg->is_null() || query_arg->is_null() || docs_arg->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model id, query or documents is null");
  } else {
    ObEvalCtx::TempAllocGuard guard(ctx);
    MultimodeAlloctor allocator(guard.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_RERANK));
    const bool has_key = key_arg != nullptr && !key_arg->is_null();
    const ObString doc_key = has_key ? key_arg->get_string() : ObString();
    ObString model_id = model_arg->get_string();
    ObString query = query_arg->get_string();
    ObIJsonBase *base = nullptr;
    bool is_null = false;
    ObJsonArray *docs = nullptr;
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo endpoint;
    query::ObIAiEndpointResolver *resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObArray<RerankEntry> entries;
    ObJsonArray *output = nullptr;
    if (has_key && doc_key.empty()) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, allocator, DOCUMENTS_IDX, base, is_null))) {
    } else if (OB_ISNULL(base) || base->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_ISNULL(docs = static_cast<ObJsonArray *>(base)) || docs->element_count() == 0) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model_id, info))) {
    } else if (OB_ISNULL(info) || OB_ISNULL(resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ObAIFuncUtils::check_info_type_rerank(info))) {
    } else if (OB_FAIL(resolver->resolve_by_model_name(model_id, allocator, endpoint))) {
    } else {
      ObAIFuncModel model(allocator, *info, endpoint);
      const int64_t batch_size = 20;
      for (int64_t start = 0; OB_SUCC(ret) && start < docs->element_count(); start += batch_size) {
        const int64_t count = std::min(batch_size, static_cast<int64_t>(docs->element_count()) - start);
        ObJsonArray *inputs = nullptr;
        ObJsonArray *batch_result = nullptr;
        bool seen[20] = {};
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, inputs))) {
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
          ObJsonNode *node = docs->get_value(start + i);
          if (OB_ISNULL(node)) {
            ret = OB_INVALID_ARGUMENT;
          } else if (has_key) {
            if (node->json_type() != ObJsonNodeType::J_OBJECT) {
              ret = OB_INVALID_ARGUMENT;
            } else {
              node = static_cast<ObJsonObject *>(node)->get_value(doc_key);
            }
          }
          if (OB_FAIL(ret)) {
          } else if (OB_ISNULL(node) || node->json_type() != ObJsonNodeType::J_STRING) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            ObJsonNode *copy = node->clone(&allocator, true);
            if (OB_ISNULL(copy)) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            } else if (OB_FAIL(inputs->append(copy))) {
            }
          }
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(model.call_rerank(query, inputs, batch_result))) {
        } else if (OB_ISNULL(batch_result) || batch_result->element_count() != count) {
          ret = OB_INVALID_DATA;
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
          ObJsonNode *node = batch_result->get_value(i);
          ObJsonObject *object = nullptr;
          ObJsonNode *index_node = nullptr;
          ObJsonNode *score_node = nullptr;
          int64_t index = -1;
          double score = 0;
          if (OB_ISNULL(node) || node->json_type() != ObJsonNodeType::J_OBJECT) {
            ret = OB_INVALID_DATA;
          } else {
            object = static_cast<ObJsonObject *>(node);
            index_node = object->get_value(INDEX_KEY);
            score_node = object->get_value(SCORE_KEY);
            if (OB_NOT_NULL(index_node) && index_node->json_type() == ObJsonNodeType::J_INT) {
              index = index_node->get_int();
            } else if (OB_NOT_NULL(index_node) && index_node->json_type() == ObJsonNodeType::J_UINT
                       && index_node->get_uint() < static_cast<uint64_t>(count)) {
              index = static_cast<int64_t>(index_node->get_uint());
            }
            if (index < 0 || index >= count || seen[index] || OB_ISNULL(score_node)
                || !share::ObJsonHelper::is_number_type(score_node)) {
              ret = OB_INVALID_DATA;
            } else if (OB_FAIL(score_node->to_double(score)) || !std::isfinite(score)) {
              ret = OB_INVALID_DATA;
            } else {
              seen[index] = true;
              RerankEntry entry{start + index, score, object, score_node};
              if (OB_FAIL(entries.push_back(entry))) {
              }
            }
          }
        }
      }
      if (OB_SUCC(ret)) {
        std::sort(entries.begin(), entries.end(), [](const RerankEntry &a, const RerankEntry &b) {
          return a.score_ != b.score_ ? a.score_ > b.score_ : a.index_ < b.index_;
        });
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, output))) {
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < entries.count(); ++i) {
          const RerankEntry &entry = entries.at(i);
          ObJsonNode *source = has_key ? docs->get_value(entry.index_) : entry.result_;
          ObJsonObject *item = static_cast<ObJsonObject *>(source->clone(&allocator, true));
          if (OB_ISNULL(item)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else if (has_key) {
            ObJsonNode *score = entry.score_node_->clone(&allocator, true);
            if (OB_ISNULL(score)) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            } else if (OB_FAIL(item->add(MODEL_SCORE_KEY, score))) {
            }
          } else {
            ObJsonInt *index = nullptr;
            if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, entry.index_, index))) {
            } else if (OB_FAIL(item->add(INDEX_KEY, index))) {
            }
          }
          if (OB_SUCC(ret) && OB_FAIL(output->append(item))) {
          }
        }
        ObString raw;
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(output->get_raw_binary(raw, &allocator))) {
        } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw))) {
        }
      }
    }
    if (ret == OB_INVALID_ARGUMENT) {
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, invalid documents or document key");
    }
  }
  return ret;
}

int ObExprAIRerank::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                        const ObRawExpr &raw_expr,
                        ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   bool got_data = false;
  //   ObObj const_data;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else {
  //     ObAIFuncExprInfo *info = OB_NEWx(ObAIFuncExprInfo, allocator, *allocator, T_FUN_SYS_AI_RERANK);
  //     if (OB_ISNULL(info)) {
  //       ret = OB_ALLOCATE_MEMORY_FAILED;
  //       LOG_WARN("failed to allocate memory for ai rerank info", K(ret));
  //     } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //       LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //     } else {
  //       rt_expr.extra_info_ = info;
  //     }
  //   }
  // }
  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIRerank::eval_ai_rerank;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
