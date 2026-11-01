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

#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/ddl/ob_explain_resolver.h"
#include "sql/resolver/ob_resolver.h"
#include "sql/engine/basic/file_scan.h"
#include "sql/parser/ob_parser.h"

using namespace oceanbase;
using namespace sql;
using namespace common;
const static int64_t EXPLAIN_FORMAT = 0;
const static int64_t EXPLAIN_DISPLAY_OPTION = 1;
const static int64_t EXPLAIN_INTO_TABLE = 2;
const static int64_t EXPLAIN_STMT_ID = 3;
const static int64_t EXPLAIN_CHILD_STMT = 4;

static int describe_file_table(ObResolverParams &params, const file_sql::Definition &definition, ObStmt *&statement)
{
  int ret = OB_SUCCESS;
  const char *types[] = {"NULL", "BIGINT", "DOUBLE", "BOOLEAN", "VARCHAR", "DATE", "DATETIME"};
  const char hex[] = "0123456789abcdef";
  std::string query = "SELECT column_name,column_type,`null`,`key`,`default`,extra FROM (";
  for (size_t i = 0; i < definition.columns.size(); ++i) {
    const auto &column = definition.columns[i];
    if (i) query += " UNION ALL ";
    std::string name;
    for (unsigned char byte : column.name) { name += hex[byte >> 4]; name += hex[byte & 15]; }
    query += "SELECT CONVERT(X'" + name + "' USING utf8mb4) AS column_name,'" + types[column.type] +
             "' AS column_type,'" + (column.nullable ? "YES" : "NO") +
             "' AS `null`,NULL AS `key`,NULL AS `default`,NULL AS extra," + std::to_string(i) + " AS ordinal";
  }
  query += ") file_schema ORDER BY ordinal";
  ObParser parser(*params.allocator_, params.session_info_->get_sql_mode(), params.session_info_->get_charsets4parser());
  ParseResult parsed;
  ObResolver resolver(params);
  OZ (parser.parse(ObString(query.size(), query.data()), parsed));
  CK (parsed.result_tree_ != nullptr && parsed.result_tree_->num_child_ == 1);
  if (OB_SUCC(ret)) OZ (resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT, *parsed.result_tree_->children_[0], statement));
  return ret;
}

int ObExplainResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObExplainStmt *explain_stmt = NULL;
  if (!(T_EXPLAIN == parse_tree.type_  && 5 == parse_tree.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid EXPLAIN syntax",
             "type", parse_tree.type_,
             "# of child", parse_tree.num_child_);
  } else if (NULL == (explain_stmt = create_stmt<ObExplainStmt>())) {
    ret = OB_SQL_RESOLVER_NO_MEMORY;
    LOG_WARN("failed to create explain stmt");
  } else {
    ParseNode *child_node = parse_tree.children_[EXPLAIN_CHILD_STMT];
    ObStmt *child_stmt = NULL;
    ObExplainDisplayOpt opt;
    opt.with_tree_line_ = true;
    if (OB_ISNULL(child_node)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("empty parse tree for stmt");
    } else if (!(T_SELECT == child_node->type_
                 || T_DELETE == child_node->type_
                 || T_INSERT == child_node->type_
                 || T_UPDATE == child_node->type_
                 || T_REPLACE == child_node->type_)) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("invalid stmt type for EXPLAIN", "type", child_node->type_);
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "stmt type for EXPLAIN");
    } else {
      ObResolver resolver(params_);
      if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                   *child_node,
                                   child_stmt))) {
      } else if (OB_ISNULL(child_stmt)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to generate stmt for child stmt");
      } else {
        if (parse_tree.is_hidden_const_ && child_stmt->is_select_stmt()) {
          const auto *select = static_cast<ObSelectStmt *>(child_stmt);
          if (select->get_table_size() == 1) {
            const TableItem *table = select->get_table_item(0);
            if (table != nullptr && table->is_function_table() && table->function_table_expr_ != nullptr && table->function_table_expr_->is_const_raw_expr()) {
              const ObObj &value = static_cast<const ObConstRawExpr *>(table->function_table_expr_)->get_value();
              file_sql::Definition definition;
              if (!value.is_varchar() || file_sql::FileScan::decode(std::string(value.get_string().ptr(), value.get_string().length()), definition) != file_sql::OK) return OB_INVALID_ARGUMENT;
              ObStmt *description = nullptr;
              ret = describe_file_table(params_, definition, description);
              if (OB_SUCC(ret)) stmt_ = static_cast<ObDMLStmt *>(description);
              return ret;
            }
          }
        }
        explain_stmt->set_explain_query_stmt(static_cast<ObDMLStmt *>(child_stmt));
        ParseNode *format = parse_tree.children_[EXPLAIN_FORMAT];
        if (NULL != format) {
          switch (format->type_) {
          case T_OUTLINE:
          {
            explain_stmt->set_explain_format(EXPLAIN_OUTLINE);
            opt.with_tree_line_ = true;
          } break;
          case T_EXTENDED:
          {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED);
            opt.with_tree_line_ = true;
          } break;
          case T_PARTITIONS:
          {
            explain_stmt->set_explain_format(EXPLAIN_PARTITIONS);
            opt.with_tree_line_ = true;
          } break;
          case T_TRADITIONAL:
          {
            explain_stmt->set_explain_format(EXPLAIN_TRADITIONAL);
            opt.with_tree_line_ = true;
          } break;
          case T_FORMAT_JSON:
          {
            explain_stmt->set_explain_format(EXPLAIN_FORMAT_JSON);
            opt.with_tree_line_ = false;
          } break;
          case T_BASIC: {
            explain_stmt->set_explain_format(EXPLAIN_BASIC);
            opt.with_tree_line_ = true;
            break;
          }
          case T_EXTENDED_NOADDR: {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED_NOADDR);
            opt.with_tree_line_ = true;
            break;
          }
          case T_PLANREGRESS: {
            explain_stmt->set_explain_format(EXPLAIN_PLANREGRESS);
            opt.with_tree_line_ = true;
            break;
          }
          default:
            ret = OB_ERROR;
            LOG_WARN("unknown explain format", "format", format->type_);
            break;
          }
        }
      }
    }
    ParseNode *opt_node = parse_tree.children_[EXPLAIN_DISPLAY_OPTION];
    if (OB_SUCC(ret) && NULL != opt_node) {
      switch (opt_node->type_) {
      case T_PRETTY_COLOR:
        opt.with_color_ = true;
        // pass
      case T_PRETTY:
        opt.with_tree_line_ = true;
        break;
      default:
        ret = OB_ERR_UNEXPECTED;
      }
    }
    if (OB_SUCC(ret)) {
      explain_stmt->set_display_opt(opt);
    }
    ParseNode *into_table_node = parse_tree.children_[EXPLAIN_INTO_TABLE];
    if (OB_SUCC(ret) && NULL != into_table_node) {
      ObString into_table(into_table_node->str_len_, into_table_node->str_value_);
      explain_stmt->set_into_table(into_table);
    }
    ParseNode *statement_id_node = parse_tree.children_[EXPLAIN_STMT_ID];
    if (OB_SUCC(ret) && NULL != statement_id_node) {
      ObString statement_id(statement_id_node->str_len_, statement_id_node->str_value_);
      explain_stmt->set_statement_id(statement_id);
    }
  }

  return ret;
}
