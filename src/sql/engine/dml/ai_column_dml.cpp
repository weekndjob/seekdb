/* Copyright (c) 2026 OceanBase. Licensed under Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/dml/ai_column_dml.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/session/ai_column_session_scope.h"
#include "share/schema/ai_column_definition.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_column_schema.h"
#include "sql/das/ob_das_context.h"

namespace oceanbase { namespace sql {
using namespace common;
using namespace share::schema;
namespace {
int identifier(ObSqlString &sql, const ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if (name.ptr()[i] == '`') { ret = sql.append("`"); }
    if (OB_SUCC(ret)) { ret = sql.append(name.ptr()+i, 1); }
  }
  if (OB_SUCC(ret)) { ret = sql.append("`"); }
  return ret;
}
int literal(ObSqlString &sql, const ObString &value)
{
  int ret = sql.append("X'");
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    ret = sql.append_fmt("%02x", static_cast<unsigned char>(value.ptr()[i]));
  }
  if (OB_SUCC(ret)) { ret = sql.append("'"); }
  return ret;
}
int table_schema(const ObDMLBaseCtDef &definition, ObTableModifyOp &op,
                 const ObTableSchema *&schema)
{
  int ret = OB_SUCCESS;
  auto *sql_ctx = op.get_exec_ctx().get_sql_ctx();
  if (!sql_ctx || !sql_ctx->schema_guard_) { ret = OB_ERR_UNEXPECTED;
  } else { ret = sql_ctx->schema_guard_->get_table_schema(definition.das_base_ctdef_.index_tid_, schema); }
  if (OB_SUCC(ret) && !schema) { ret = OB_ERR_UNEXPECTED; }
  return ret;
}
ObExpr *column_expr(const ObDMLBaseCtDef &definition, uint64_t id, bool old)
{
  const auto &row = old ? definition.old_row_ : definition.new_row_;
  for (int64_t i = 0; i < definition.column_ids_.count() && i < row.count(); ++i) {
    if (definition.column_ids_.at(i) == id) { return row.at(i); }
  }
  return nullptr;
}
int request(const ObDMLBaseCtDef &definition, uint64_t id, bool old,
            ObTableModifyOp &op, ObString &value, bool &is_null)
{
  ObExpr *expr = column_expr(definition, id, old);
  ObDatum *datum = nullptr;
  int ret = OB_SUCCESS;
  is_null = true;
  value.reset();
  if (!expr) { return ret; } // An unrelated UPDATE need not project this state.
  if (OB_FAIL(expr->eval(op.get_eval_ctx(), datum))) {
  } else if (!datum) { ret = OB_ERR_UNEXPECTED;
  } else if (!(is_null = datum->is_null())) { value = datum->get_string(); }
  return ret;
}
int predicate(const ObDMLBaseCtDef &definition, const ObTableSchema &schema,
              bool old, ObTableModifyOp &op, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  const auto &key = schema.get_rowkey_info();
  if (key.get_size() == 0) { return OB_ERR_UNEXPECTED; }
  for (int64_t i = 0; OB_SUCC(ret) && i < key.get_size(); ++i) {
    uint64_t id = 0;
    ObDatum *datum = nullptr;
    ObObj value;
    char text[8192];
    int64_t pos = 0;
    if (OB_FAIL(key.get_column_id(i, id))) { break; }
    const auto *column = schema.get_column_schema(id);
    ObExpr *expr = column_expr(definition, id, old);
    if (!column || !expr) { ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(expr->eval(op.get_eval_ctx(), datum))) {
    } else if (!datum) { ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(datum->to_obj(value, expr->obj_meta_))) {
    } else if (i > 0 && OB_FAIL(sql.append(" AND "))) {
    } else if (OB_FAIL(identifier(sql, column->get_column_name_str()))) {
    } else if (OB_FAIL(sql.append(" = "))) {
    } else if (value.is_string_type()) { ret = literal(sql, value.get_string());
    } else if (OB_FAIL(value.print_sql_literal(text, sizeof(text), pos, op.get_obj_print_params()))) {
    } else { ret = sql.append(text, pos); }
    if (OB_SUCC(ret) && sql.length() > 4000) { ret = OB_SIZE_OVERFLOW; }
  }
  return ret;
}
int qualified_table(const ObTableSchema &schema, ObTableModifyOp &op, ObSqlString &sql)
{
  const ObDatabaseSchema *database = nullptr;
  int ret = op.get_exec_ctx().get_sql_ctx()->schema_guard_->get_database_schema(schema.get_database_id(), database);
  if (OB_SUCC(ret) && !database) { ret = OB_ERR_UNEXPECTED; }
  if (OB_SUCC(ret)) { ret = identifier(sql, database->get_database_name_str()); }
  if (OB_SUCC(ret)) { ret = sql.append("."); }
  if (OB_SUCC(ret)) { ret = identifier(sql, schema.get_table_name_str()); }
  return ret;
}
int erase_tasks(const ObTableSchema &schema, const ObString &predicate, ObTableModifyOp &op)
{
  ObSqlString sql;
  int ret = sql.append_fmt("DELETE FROM oceanbase.__all_ai_column_task WHERE table_id=%lu AND rowkey_hash=SHA2(", schema.get_table_id());
  if (OB_SUCC(ret)) { ret = literal(sql, predicate); }
  if (OB_SUCC(ret)) { ret = sql.append(",256) AND rowkey_predicate="); }
  if (OB_SUCC(ret)) { ret = literal(sql, predicate); }
  if (OB_SUCC(ret)) { ret = op.execute_write(sql.ptr()); }
  return ret;
}
}

int AiColumnDml::before(const ObDMLBaseCtDef &ctdef, ObTableModifyOp &op, bool inserting)
{
  int ret = OB_SUCCESS;
  auto *session = op.get_exec_ctx().get_my_session();
  if (!session) { return OB_ERR_UNEXPECTED; }
  if (!ctdef.has_ai_columns_ || session->is_ai_column_internal() || session->get_ddl_info().is_ddl()) { return ret; }
  const ObTableSchema *schema = nullptr;
  ObString group;
  if (OB_FAIL(table_schema(ctdef, op, schema))) { return ret; }
  for (auto it = schema->column_begin(); OB_SUCC(ret) && it != schema->column_end(); ++it) {
    if (!(*it)->is_ai_column()) { continue; }
    AiColumnDefinition definition;
    ObString current, previous;
    bool current_null = true, previous_null = true;
    if (OB_FAIL(definition.decode((*it)->get_ai_column_info()))) {
    } else if (OB_FAIL(request(ctdef, definition.request_column_id_, false, op, current, current_null))) {
    } else if (!inserting && OB_FAIL(request(ctdef, definition.request_column_id_, true, op, previous, previous_null))) {
    } else if (!current_null && (inserting || previous_null || current != previous)) {
      // Each request expression is a volatile native UUID, with its own datum.
      // All changed columns on this row share the first such UUID as their group.
      if (group.empty() && OB_FAIL(ob_write_string(op.get_exec_ctx().get_allocator(), current, group))) {
      } else {
        auto *expr = column_expr(ctdef, definition.request_column_id_, false);
        expr->locate_expr_datum(op.get_eval_ctx()).set_string(group);
        expr->get_eval_info(op.get_eval_ctx()).evaluated_ = true;
      }
    }
  }
  return ret;
}

int AiColumnDml::after(const ObDMLBaseCtDef &ctdef, ObTableModifyOp &op, int event)
{
  int ret = OB_SUCCESS;
  auto *session = op.get_exec_ctx().get_my_session();
  if (!session) { return OB_ERR_UNEXPECTED; }
  if (!ctdef.has_ai_columns_ || session->is_ai_column_internal() || session->get_ddl_info().is_ddl()) { return ret; }
  const bool inserting = event == static_cast<int>(ObDmlEventType::DE_INSERTING);
  const bool deleting = event == static_cast<int>(ObDmlEventType::DE_DELETING);
  const ObTableSchema *schema = nullptr;
  ObSqlString current_key, old_key, name;
  if (OB_FAIL(table_schema(ctdef, op, schema))) {
  } else if (!inserting && OB_FAIL(predicate(ctdef, *schema, true, op, old_key))) {
  } else if (!deleting && OB_FAIL(predicate(ctdef, *schema, false, op, current_key))) {
  } else if (OB_FAIL(qualified_table(*schema, op, name))) {
  } else if (OB_FAIL(op.open_inner_conn())) {
  } else if (OB_FAIL(op.begin_nested_session(true))) {
  } else {
    {
      AiColumnSessionScope trusted(*session);
      if (!inserting && (deleting || current_key.string() != old_key.string())) {
        ret = erase_tasks(*schema, old_key.string(), op);
      }
      for (auto it = schema->column_begin(); OB_SUCC(ret) && !deleting && it != schema->column_end(); ++it) {
        const auto &column = **it;
        if (!column.is_ai_column()) { continue; }
        AiColumnDefinition definition;
        ObString current, previous;
        bool current_null = true, previous_null = true;
        if (OB_FAIL(definition.decode(column.get_ai_column_info()))) {
        } else if (OB_FAIL(request(ctdef, definition.request_column_id_, false, op, current, current_null))) {
        } else if (!inserting && OB_FAIL(request(ctdef, definition.request_column_id_, true, op, previous, previous_null))) {
        } else if (!current_null && (inserting || previous_null || current != previous)) {
          // SELECT materializes only deterministic inputs. No outer AI function
          // runs here. This nested connection shares the business transaction.
          ObSqlString sql;
          if (OB_FAIL(sql.append("REPLACE INTO oceanbase.__all_ai_column_task "
              "(table_id,column_id,rowkey_hash,rowkey_predicate,request_id,definition_version,"
              "input_value,options_value,sql_mode,time_zone,state,retry_count,eligible_ts) SELECT "))) {
          } else if (OB_FAIL(sql.append_fmt("%lu,%lu,SHA2(", schema->get_table_id(), column.get_column_id()))) {
          } else if (OB_FAIL(literal(sql, current_key.string()))) {
          } else if (OB_FAIL(sql.append(",256),"))) {
          } else if (OB_FAIL(literal(sql, current_key.string()))) {
          } else if (OB_FAIL(sql.append(","))) {
          } else if (OB_FAIL(literal(sql, current))) {
          } else if (OB_FAIL(sql.append_fmt(",%ld,CAST((%.*s) AS CHAR CHARACTER SET utf8mb4),",
              definition.definition_version_, definition.input_expr_.length(), definition.input_expr_.ptr()))) {
          } else if (definition.options_expr_.empty()) { ret = sql.append("NULL");
          } else { ret = sql.append_fmt("CAST((%.*s) AS CHAR CHARACTER SET utf8mb4)",
                      definition.options_expr_.length(), definition.options_expr_.ptr()); }
          if (OB_SUCC(ret)) { ret = sql.append_fmt(",%lu,@@time_zone,0,0,NOW(6) FROM %s WHERE (%s) AND ", session->get_sql_mode(), name.ptr(), current_key.ptr()); }
          if (OB_SUCC(ret)) { ret = identifier(sql, schema->get_column_schema(definition.request_column_id_)->get_column_name_str()); }
          if (OB_SUCC(ret)) { ret = sql.append("="); }
          if (OB_SUCC(ret)) { ret = literal(sql, current); }
          if (OB_SUCC(ret)) { ret = op.execute_write(sql.ptr()); }
        }
      }
    }
    const int end_ret = op.end_nested_session();
    if (OB_SUCC(ret)) { ret = end_ret; }
  }
  return ret;
}
} }
