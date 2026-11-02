/* Copyright (c) 2026 OceanBase. Licensed under Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/ai_column_strict_read.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/ob_physical_plan.h"
#include "sql/session/ob_inner_sql_connection.h"
#include "sql/session/ai_column_session_scope.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "share/schema/ai_column_definition.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_column_schema.h"
#include "sql/das/ob_das_context.h"

namespace oceanbase { namespace sql {
using namespace common;
using namespace share::schema;
namespace {
int quote(ObSqlString &text, const ObString &name)
{
  int ret = text.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if (name.ptr()[i] == '`') { ret = text.append("`"); }
    if (OB_SUCC(ret)) { ret = text.append(name.ptr()+i, 1); }
  }
  if (OB_SUCC(ret)) { ret = text.append("`"); }
  return ret;
}
}
int AiColumnStrictRead::check(ObExecContext &ctx, const ObPhysicalPlan &plan)
{
  int ret = OB_SUCCESS;
  auto *session = ctx.get_my_session();
  const auto &refs = plan.get_ai_column_read_refs();
  if (!session) { return OB_ERR_UNEXPECTED; }
  if (session->is_ai_column_internal() || refs.empty() || plan.get_stmt_type() == stmt::T_EXPLAIN) { return ret; }
  int64_t enabled = 0;
  if (OB_FAIL(session->get_sys_variable_by_name(ObString::make_string("ai_column_strict_read"), enabled))) { return ret; }
  if (enabled == 0) { return ret; }
  if (refs.count()%2 != 0 || !ctx.get_sql_ctx() || !ctx.get_sql_ctx()->schema_guard_) { return OB_ERR_UNEXPECTED; }
  sqlclient::ObISQLConnectionGuard guard;
  sqlclient::ObISQLConnection *base = session->get_inner_conn();
  if (!base) {
    if (OB_FAIL(query::ObInnerSQLConnectionAccess::create_connection_with_external_session(session, guard))) { return ret; }
    base = guard.get_ptr();
  }
  auto *connection = as_inner_sql_connection(base);
  ObSQLSessionInfo::StmtSavedValue saved_session;
  ObIInnerSQLConnection::SavedValue saved_connection;
  if (!connection) { return OB_ERR_UNEXPECTED; }
  if (OB_FAIL(connection->begin_nested_session(saved_session, saved_connection, true))) { return ret; }
  {
    AiColumnSessionScope trusted(*session);
    // Pin the nested checks to the original statement's MVCC snapshot. A fresh
    // READ COMMITTED snapshot could otherwise certify a different set of rows.
    const auto *previous_snapshot = session->get_ai_column_read_snapshot();
    session->set_ai_column_read_snapshot(&ctx.get_das_ctx().get_snapshot());
    for (int64_t i = 0; OB_SUCC(ret) && i < refs.count(); i+=2) {
      const ObTableSchema *table = nullptr;
      const ObDatabaseSchema *database = nullptr;
      const ObColumnSchemaV2 *column = nullptr;
      AiColumnDefinition definition;
      ObSqlString sql;
      ObISQLClient::ReadResult result;
      if (OB_FAIL(ctx.get_sql_ctx()->schema_guard_->get_table_schema(refs.at(i), table))) {
      } else if (!table || !(column = table->get_column_schema(refs.at(i+1))) || !column->is_ai_column()) { ret = OB_SCHEMA_EAGAIN;
      } else if (OB_FAIL(definition.decode(column->get_ai_column_info()))) {
      } else if (OB_FAIL(ctx.get_sql_ctx()->schema_guard_->get_database_schema(table->get_database_id(), database))) {
      } else if (!database || !table->get_column_schema(definition.state_column_id_)) { ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(sql.append("SELECT 1 FROM "))) {
      } else if (OB_FAIL(quote(sql, database->get_database_name_str()))) {
      } else if (OB_FAIL(sql.append("."))) {
      } else if (OB_FAIL(quote(sql, table->get_table_name_str()))) {
      } else if (OB_FAIL(sql.append(" WHERE NOT ("))) {
      } else if (OB_FAIL(quote(sql, table->get_column_schema(definition.state_column_id_)->get_column_name_str()))) {
      } else if (OB_FAIL(sql.append(" <=> 'READY') LIMIT 1"))) {
      } else if (OB_FAIL(connection->execute_read(sql.string(), result))) {
      } else if (!result.get_result()) { ret = OB_ERR_UNEXPECTED;
      } else if ((ret = result.get_result()->next()) == OB_ITER_END) { ret = OB_SUCCESS;
      } else if (OB_SUCC(ret)) {
        ret = OB_AI_COLUMN_NOT_READY;
        LOG_USER_ERROR(OB_AI_COLUMN_NOT_READY, refs.at(i), refs.at(i+1));
      }
    }
    session->set_ai_column_read_snapshot(previous_snapshot);
  }
  const int end_ret = connection->end_nested_session(saved_session, saved_connection);
  if (OB_SUCC(ret)) { ret = end_ret; }
  return ret;
}
} }
