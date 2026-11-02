/* Copyright (c) 2026 OceanBase. Licensed under Apache License, Version 2.0. */
#pragma once
#include <algorithm>
#include "share/schema/ai_column_definition.h"
#include "share/schema/ob_column_schema.h"
#include "share/schema/ob_ai_model_mgr.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/resolver/ob_schema_checker.h"
#include "sql/printer/ob_raw_expr_printer.h"
#include "query/vector/ob_vector_index_util.h"

namespace oceanbase { namespace sql {
class AiColumnResolver
{
public:
  static int attribute(const ParseNode &node, share::schema::ObColumnSchemaV2 &column,
                       common::ObIAllocator &arena)
  {
    using namespace common;
    using namespace share::schema;
    if (node.type_ != T_AI_COLUMN_DEFINITION || node.num_child_ != 2 || !node.children_[0]
        || column.is_ai_column()) { return OB_INVALID_ARGUMENT; }
    AiColumnDefinition definition;
    const ParseNode &expression = *node.children_[0];
    definition.raw_expr_.assign_ptr(expression.str_value_, expression.str_len_);
    bool saw_sync = false;
    bool saw_retries = false;
    const ParseNode *options = node.children_[1];
    for (int64_t i = 0; options && i < options->num_child_; ++i) {
      const ParseNode *option = options->children_[i];
      if (!option || option->type_ != T_AI_COLUMN_OPTION || option->num_child_ != 2
          || !option->children_[0] || !option->children_[1]) { return OB_INVALID_ARGUMENT; }
      const ObString name(option->children_[0]->str_len_, option->children_[0]->str_value_);
      const ParseNode &value = *option->children_[1];
      if (name.case_compare("sync_mode") == 0 && !saw_sync) {
        saw_sync = true;
        if (value.type_ != T_VARCHAR || ObString(value.str_len_, value.str_value_).case_compare("async") != 0) {
          return unsupported("AI COLUMN sync_mode other than async");
        }
      } else if (name.case_compare("max_retries") == 0 && !saw_retries) {
        saw_retries = true;
        if ((value.type_ != T_INT && value.type_ != T_UINT64) || value.value_ < 0 || value.value_ > 100) {
          return OB_INVALID_ARGUMENT;
        }
        definition.max_retries_ = value.value_;
      } else { return OB_INVALID_ARGUMENT; }
    }
    ObString packet;
    int ret = definition.encode(arena, packet, false);
    if (ret == OB_SUCCESS) { ret = column.set_ai_column_info(packet); }
    if (ret == OB_SUCCESS) { column.add_column_flag(AI_COLUMN_FLAG); }
    return ret;
  }

  // Resolve after all ordinary columns are available. The outer AI function is
  // allowed; its inputs retain the normal generated-expression restrictions.
  // No expression is evaluated and no endpoint is contacted during resolution.
  static int normalize(share::schema::ObColumnSchemaV2 &column,
                       const share::schema::ObTableSchema &table, ObResolverParams &params)
  {
    using namespace common;
    using namespace share::schema;
    if (!column.is_ai_column()) { return OB_SUCCESS; }
    int ret = OB_SUCCESS;
    if (!params.allocator_ || !params.session_info_ || !params.expr_factory_ || !params.schema_checker_) {
      LOG_WARN("AI COLUMN resolver parameters missing", KP(params.allocator_), KP(params.session_info_),
          KP(params.expr_factory_), KP(params.schema_checker_));
      return OB_INVALID_ARGUMENT;
    }
    if (!column.is_nullable() || column.get_rowkey_position() != 0 || column.is_generated_column()
        || !column.get_cur_default_value().is_null() || column.is_autoincrement()) {
      return unsupported("AI COLUMN keys, NOT NULL, defaults or generated-column combinations");
    }
    AiColumnDefinition definition;
    ObRawExpr *expression = nullptr;
    ret = definition.decode(column.get_ai_column_info(), false);
    if (ret != OB_SUCCESS) { LOG_WARN("AI COLUMN packet decode failed", K(ret)); return ret; }
    ObSEArray<ObQualifiedName, 8> unresolved;
    if (OB_FAIL(ObRawExprUtils::build_generated_column_expr(definition.raw_expr_,
        *params.expr_factory_, *params.session_info_, params.session_info_->get_sql_mode(),
        params.session_info_->get_local_collation_connection(), expression, unresolved,
        &table, nullptr, params.schema_checker_, ObResolverUtils::DISABLE_CHECK))) {
      LOG_WARN("AI COLUMN expression parse failed", K(ret));
      return ret;
    }
    for (int64_t i = 0; i < unresolved.count(); ++i) {
      const auto &name = unresolved.at(i);
      if (name.is_pl_udf() || !name.database_name_.empty()
          || (!name.tbl_name_.empty() && !ObCharset::case_insensitive_equal(name.tbl_name_, table.get_table_name_str()))) {
        return unsupported("AI COLUMN external or procedural dependencies");
      }
      const auto *dependency = table.get_column_schema(name.col_name_);
      if (!dependency || !name.ref_expr_) { return OB_ERR_BAD_FIELD_ERROR; }
      if (OB_FAIL(ObRawExprUtils::init_column_expr(*dependency, params.session_info_, *name.ref_expr_))) {
        LOG_WARN("AI COLUMN dependency initialization failed", K(ret));
        return ret;
      }
      name.ref_expr_->set_ref_id(table.get_table_id(), dependency->get_column_id());
    }
    if (!expression) { return OB_ERR_UNEXPECTED; }
    if (OB_FAIL(expression->formalize(params.session_info_, true))) {
      LOG_WARN("AI COLUMN expression formalization failed", K(ret));
      return ret;
    }
    if (!expression || (expression->get_expr_type() != T_FUN_SYS_AI_COMPLETE
        && expression->get_expr_type() != T_FUN_SYS_AI_EMBED)) { return unsupported("AI COLUMN functions other than AI_COMPLETE/AI_EMBED"); }
    definition.function_ = expression->get_expr_type() == T_FUN_SYS_AI_EMBED ? AiColumnDefinition::EMBED : AiColumnDefinition::COMPLETE;
    if (expression->get_param_count() < 2 || expression->get_param_count() > 3) {
      LOG_WARN("AI COLUMN function has wrong argument count", K(expression->get_param_count()));
      return OB_INVALID_ARGUMENT;
    }
    ObRawExpr *model = expression->get_param_expr(0);
    const ObConstRawExpr *model_literal = constant_literal(model);
    if (!model_literal) { return unsupported("nonconstant AI COLUMN model names"); }
    const ObObj &name = model_literal->get_value();
    if (!name.is_string_type() || name.is_null()) {
      LOG_WARN("AI COLUMN model name is not a string literal", K(name), K(name.get_type()),
          K(definition.raw_expr_));
      return OB_INVALID_ARGUMENT;
    }
    definition.model_ = name.get_string();
    const ObAiModelSchema *model_schema = nullptr;
    if (OB_FAIL(params.schema_checker_->get_schema_guard()->get_ai_model_schema(definition.model_, model_schema))) {
      LOG_WARN("AI COLUMN model schema lookup failed", K(ret));
      return ret;
    }
    if (!model_schema) {
      LOG_USER_ERROR(OB_AI_FUNC_MODEL_NOT_FOUND, definition.model_.length(), definition.model_.ptr());
      return OB_AI_FUNC_MODEL_NOT_FOUND;
    }
    const auto expected_model = definition.function_ == AiColumnDefinition::EMBED
        ? share::EndpointType::DENSE_EMBEDDING : share::EndpointType::COMPLETION;
    if (model_schema->get_type() != expected_model) {
      LOG_WARN("AI COLUMN model type mismatch", K(model_schema->get_type()), K(expected_model));
      return OB_INVALID_ARGUMENT;
    }
    if (definition.function_ == AiColumnDefinition::EMBED) {
      if (!column.is_collection()) { return unsupported("AI_EMBED results other than dense VECTOR"); }
      if (OB_FAIL(share::ObVectorIndexUtil::get_vector_dim_from_extend_type_info(column.get_extended_type_info(), definition.dimension_))) { return ret; }
      if (expression->get_param_count() == 3) {
        const ObConstRawExpr *dimension = constant_literal(expression->get_param_expr(2));
        if (!dimension) { return OB_INVALID_ARGUMENT; }
        const ObObj &value = dimension->get_value();
        if (!value.is_integer_type() || value.is_null()) { return OB_INVALID_ARGUMENT; }
        const int64_t actual = value.get_int();
        if (actual != definition.dimension_) {
          LOG_USER_ERROR(OB_ERR_INVALID_VECTOR_DIM, static_cast<unsigned>(definition.dimension_), static_cast<unsigned>(actual));
          return OB_ERR_INVALID_VECTOR_DIM;
        }
      }
      definition.result_type_ = ObString::make_string("VECTOR");
    } else {
      if (column.is_collection()) { return unsupported("AI_COMPLETE vector results"); }
      const ObObjType type = column.get_data_type();
      if (column.is_json()) { definition.result_type_ = ObString::make_string("JSON"); }
      else if (ob_is_integer_type(type)) { definition.result_type_ = ObString::make_string("BIGINT"); }
      else if (ob_is_numeric_type(type)) { definition.result_type_ = ObString::make_string("NUMBER"); }
      else if (ob_is_string_type(type) || ob_is_text_tc(type)) { definition.result_type_ = ObString::make_string("VARCHAR"); }
      else if (type == ObDateType) { definition.result_type_ = ObString::make_string("DATE"); }
      else if (type == ObDateTimeType) { definition.result_type_ = ObString::make_string("DATETIME"); }
      else { return unsupported("this AI COLUMN result type"); }
    }
    for (int64_t i = 1; i < expression->get_param_count(); ++i) {
      if (definition.function_ == AiColumnDefinition::EMBED && i == 2) { continue; }
      ObRawExpr *input = expression->get_param_expr(i);
      if (OB_FAIL(ObRawExprUtils::check_deterministic(input, *params.allocator_, ObResolverUtils::CHECK_FOR_GENERATED_COLUMN))) {
        LOG_WARN("AI COLUMN input determinism check failed", K(ret));
        return ret;
      }
      ObSEArray<ObRawExpr *, 8> references;
      if (OB_FAIL(ObRawExprUtils::extract_column_exprs(input, references))) {
        LOG_WARN("AI COLUMN dependency extraction failed", K(ret));
        return ret;
      }
      for (int64_t j = 0; j < references.count(); ++j) {
        const uint64_t id = static_cast<ObColumnRefRawExpr *>(references.at(j))->get_column_id();
        const ObColumnSchemaV2 *dependency = table.get_column_schema(id);
        if (!dependency || dependency->is_hidden() || dependency->is_ai_column() || dependency->is_generated_column()) {
          return OB_NOT_SUPPORTED;
        }
        bool present = false;
        for (int64_t k = 0; k < definition.dependencies_.count(); ++k) { present |= definition.dependencies_.at(k) == id; }
        if (!present && OB_FAIL(definition.dependencies_.push_back(id))) { return ret; }
      }
    }
    if (definition.dependencies_.count() > 1) {
      lib::ob_sort(&definition.dependencies_.at(0), &definition.dependencies_.at(0)+definition.dependencies_.count());
    }
    for (int64_t i = 0; i < definition.dependencies_.count(); ++i) {
      const auto *dependency = table.get_column_schema(definition.dependencies_.at(i));
      if (!dependency) { return OB_ERR_UNEXPECTED; }
      if (OB_FAIL(definition.dependency_names_.push_back(dependency->get_column_name_str()))) { return ret; }
    }
    definition.input_is_json_ = expression->get_param_expr(1)->get_result_type().is_json();
    if (OB_FAIL(print(expression->get_param_expr(1), params, definition.input_expr_))) {
      LOG_WARN("AI COLUMN input expression printing failed", K(ret));
      return ret;
    }
    if (definition.function_ == AiColumnDefinition::COMPLETE && expression->get_param_count() == 3
        && OB_FAIL(print(expression->get_param_expr(2), params, definition.options_expr_))) { return ret; }
    if (OB_FAIL(print(expression, params, definition.normalized_expr_))) {
      LOG_WARN("AI COLUMN expression printing failed", K(ret));
      return ret;
    }
    if (definition.function_ == AiColumnDefinition::COMPLETE) {
      const char *instruction = nullptr;
      if (definition.result_type_ == "JSON") { instruction = "Return exactly one valid JSON value, without Markdown or explanatory text."; }
      else if (definition.result_type_ == "BIGINT") { instruction = "Return exactly one base-10 integer, without Markdown or explanatory text."; }
      else if (definition.result_type_ == "NUMBER") { instruction = "Return exactly one finite base-10 number, without Markdown or explanatory text."; }
      else if (definition.result_type_ == "DATE") { instruction = "Return exactly one valid date in YYYY-MM-DD format, without additional text."; }
      else if (definition.result_type_ == "DATETIME") { instruction = "Return exactly one valid datetime in YYYY-MM-DD HH:MM:SS.ffffff format, without additional text."; }
      if (instruction) {
        definition.system_prompt_ = ObString::make_string(instruction);
        ObSqlString normalized;
        if (OB_FAIL(normalized.append(definition.normalized_expr_))) { return ret; }
        if (OB_FAIL(normalized.append(" /* AI COLUMN system prompt: "))) { return ret; }
        if (OB_FAIL(normalized.append(definition.system_prompt_))) { return ret; }
        if (OB_FAIL(normalized.append(" */"))) { return ret; }
        if (OB_FAIL(ob_write_string(*params.allocator_, normalized.string(), definition.normalized_expr_))) { return ret; }
      }
    }
    ObString packet;
    if (OB_FAIL(definition.encode(*params.allocator_, packet, false))) {
      LOG_WARN("AI COLUMN normalized packet encode failed", K(ret));
      return ret;
    }
    return column.set_ai_column_info(packet);
  }

private:
  // Formalization may wrap a literal in an implicit CAST. is_const_expr()
  // also accepts such function nodes, which must never be cast to ObConstRawExpr.
  static const ObConstRawExpr *constant_literal(const ObRawExpr *expr)
  {
    while (expr && expr->get_expr_type() == T_FUN_SYS_CAST && expr->get_param_count() > 0) {
      expr = expr->get_param_expr(0);
    }
    return expr && expr->is_immutable_const_expr()
        ? static_cast<const ObConstRawExpr *>(expr) : nullptr;
  }
  static int unsupported(const char *reason)
  { using namespace common; LOG_USER_ERROR(OB_NOT_SUPPORTED, reason); return OB_NOT_SUPPORTED; }
  static int print(ObRawExpr *expression, ObResolverParams &params, common::ObString &text)
  {
    using namespace common;
    char *buffer = static_cast<char *>(params.allocator_->alloc(share::schema::AiColumnDefinition::MAX_PACKET));
    if (!buffer) { return OB_ALLOCATE_MEMORY_FAILED; }
    int64_t pos = 0;
    ObObjPrintParams printing = CREATE_OBJ_PRINT_PARAM(params.session_info_);
    printing.character_hex_safe_represent_ = true;
    ObRawExprPrinter printer(buffer, share::schema::AiColumnDefinition::MAX_PACKET, &pos,
        params.schema_checker_->get_schema_guard(), printing);
    int ret = printer.do_print(expression, T_NONE_SCOPE, true);
    if (ret == OB_SUCCESS) { text.assign_ptr(buffer, pos); }
    return ret;
  }
};
} }
