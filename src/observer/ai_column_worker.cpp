/* Copyright (c) 2026 OceanBase. Licensed under Apache License, Version 2.0. */
#define USING_LOG_PREFIX SERVER
#include "observer/ai_column_worker.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "sql/session/ai_column_session_scope.h"
#include "share/schema/ai_column_definition.h"
#include "lib/time/ob_time_utility.h"
#include <algorithm>
#include <cctype>
#include <unistd.h>

namespace oceanbase { namespace observer {
using namespace common;
using namespace common::sqlclient;
using namespace share::schema;
using sql::AiColumnSessionScope;
namespace {
constexpr int64_t MAX_VALUE = 16 * 1024 * 1024;
struct Task
{
  uint64_t table_id = 0, column_id = 0;
  int64_t version = 0, retries = 0, sql_mode = 0, state = 0;
  ObString hash, predicate, request, packet, input, options, time_zone;
  ObString database, table, column, state_column, request_column, spec_column, result;
  bool input_null = false, options_null = false;
};
int quote(ObSqlString &sql, const ObString &value)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    if (value.ptr()[i] == '`') { ret = sql.append("`"); }
    if (OB_SUCC(ret)) { ret = sql.append(value.ptr()+i, 1); }
  }
  if (OB_SUCC(ret)) { ret = sql.append("`"); }
  return ret;
}
int literal(ObSqlString &sql, const ObString &value, bool text = false)
{
  int ret = text ? sql.append("CONVERT(X'") : sql.append("X'");
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    ret = sql.append_fmt("%02x", static_cast<unsigned char>(value.ptr()[i]));
  }
  if (OB_SUCC(ret)) { ret = sql.append(text ? "' USING utf8mb4)" : "'"); }
  return ret;
}
int table_name(ObSqlString &sql, const Task &task)
{
  int ret = quote(sql, task.database);
  if (OB_SUCC(ret)) { ret = sql.append("."); }
  if (OB_SUCC(ret)) { ret = quote(sql, task.table); }
  return ret;
}
int write(ObISQLConnection &connection, const ObSqlString &sql, int64_t *rows = nullptr)
{
  int64_t affected = 0;
  const int ret = connection.execute_write(sql.string(), affected);
  if (rows) { *rows = affected; }
  return ret;
}
int text(ObMySQLResult &row, const char *name, ObIAllocator &arena, ObString &value, bool *is_null = nullptr)
{
  ObString borrowed;
  int ret = row.get_varchar(name, borrowed);
  if (is_null) { *is_null = ret == OB_ERR_NULL_VALUE; }
  if (ret == OB_ERR_NULL_VALUE && is_null) { ret = OB_SUCCESS; }
  if (OB_SUCC(ret) && borrowed.length() > MAX_VALUE) { ret = OB_SIZE_OVERFLOW; }
  if (OB_SUCC(ret)) { ret = ob_write_string(arena, borrowed, value); }
  return ret;
}
int key(ObSqlString &sql, const Task &task, bool owner, const ObString &owner_id)
{
  int ret = sql.append_fmt("table_id=%lu AND column_id=%lu AND rowkey_hash=", task.table_id, task.column_id);
  if (OB_SUCC(ret)) { ret = literal(sql, task.hash); }
  if (OB_SUCC(ret)) { ret = sql.append(" AND request_id="); }
  if (OB_SUCC(ret)) { ret = literal(sql, task.request); }
  if (OB_SUCC(ret)) { ret = sql.append_fmt(" AND definition_version=%ld", task.version); }
  if (OB_SUCC(ret) && owner) { ret = sql.append(" AND state=1 AND lease_owner="); }
  if (OB_SUCC(ret) && owner) { ret = literal(sql, owner_id); }
  return ret;
}
int group_key(ObSqlString &sql, const Task &task)
{
  int ret = sql.append_fmt("table_id=%lu AND rowkey_hash=", task.table_id);
  if (OB_SUCC(ret)) { ret = literal(sql, task.hash); }
  if (OB_SUCC(ret)) { ret = sql.append(" AND request_id="); }
  if (OB_SUCC(ret)) { ret = literal(sql, task.request); }
  return ret;
}
int row_guard(ObSqlString &sql, const Task &task)
{
  int ret = sql.append_fmt("(%.*s) AND ", task.predicate.length(), task.predicate.ptr());
  if (OB_SUCC(ret)) { ret = quote(sql, task.request_column); }
  if (OB_SUCC(ret)) { ret = sql.append("="); }
  if (OB_SUCC(ret)) { ret = literal(sql, task.request); }
  if (OB_SUCC(ret)) { ret = sql.append_fmt(" AND EXISTS(SELECT 1 FROM oceanbase.__all_ai_column WHERE table_id=%lu AND column_id=%lu AND definition_version=%ld)", task.table_id, task.column_id, task.version); }
  return ret;
}
const char *TASK_SELECT =
  "SELECT w.table_id,w.column_id,w.rowkey_hash,w.rowkey_predicate,w.request_id,"
  "w.definition_version,w.retry_count,w.sql_mode,w.time_zone,w.input_value,w.options_value,w.state,w.result_value,"
  "a.definition,d.database_name,t.table_name,a.column_name,s.column_name AS state_name,"
  "r.column_name AS request_name,p.column_name AS spec_name "
  "FROM oceanbase.__all_ai_column_task w JOIN oceanbase.__all_ai_column a "
  "ON a.table_id=w.table_id AND a.column_id=w.column_id AND a.definition_version=w.definition_version "
  "JOIN oceanbase.__all_table t ON t.table_id=w.table_id "
  "JOIN oceanbase.__all_database d ON d.database_id=t.database_id "
  "JOIN oceanbase.__all_column s ON s.table_id=a.table_id AND s.column_id=a.state_column_id "
  "JOIN oceanbase.__all_column r ON r.table_id=a.table_id AND r.column_id=a.request_column_id "
  "JOIN oceanbase.__all_column p ON p.table_id=a.table_id AND p.column_id=a.result_spec_column_id ";
int load_task(ObMySQLResult &row, ObIAllocator &arena, Task &task, int64_t *result_budget = nullptr)
{
  int ret = OB_SUCCESS;
  int64_t table = 0, column = 0;
  if (OB_FAIL(row.get_int("table_id", table))) {
  } else if (OB_FAIL(row.get_int("column_id", column))) {
  } else if (OB_FAIL(row.get_int("definition_version", task.version))) {
  } else if (OB_FAIL(row.get_int("retry_count", task.retries))) {
  } else if (OB_FAIL(row.get_int("sql_mode", task.sql_mode))) {
  } else if (OB_FAIL(row.get_int("state", task.state))) {
  } else if (OB_FAIL(text(row, "rowkey_hash", arena, task.hash))) {
  } else if (OB_FAIL(text(row, "rowkey_predicate", arena, task.predicate))) {
  } else if (OB_FAIL(text(row, "request_id", arena, task.request))) {
  } else if (OB_FAIL(text(row, "definition", arena, task.packet))) {
  } else if (OB_FAIL(text(row, "input_value", arena, task.input, &task.input_null))) {
  } else if (OB_FAIL(text(row, "options_value", arena, task.options, &task.options_null))) {
  } else if (OB_FAIL(text(row, "time_zone", arena, task.time_zone))) {
  } else if (OB_FAIL(text(row, "database_name", arena, task.database))) {
  } else if (OB_FAIL(text(row, "table_name", arena, task.table))) {
  } else if (OB_FAIL(text(row, "column_name", arena, task.column))) {
  } else if (OB_FAIL(text(row, "state_name", arena, task.state_column))) {
  } else if (OB_FAIL(text(row, "request_name", arena, task.request_column))) {
  } else if (OB_FAIL(text(row, "spec_name", arena, task.spec_column))) {
  } else {
    ObString result;
    ret = row.get_varchar("result_value", result);
    if (ret == OB_ERR_NULL_VALUE) { ret = OB_SUCCESS; }
    if (OB_SUCC(ret) && result_budget && result.length() > *result_budget) {
      task.state = 4; // Bounded publication: an oversized value is a failed result.
    } else if (OB_SUCC(ret)) {
      if (result_budget) { *result_budget -= result.length(); }
      ret = ob_write_string(arena, result, task.result);
    }
  }
  task.table_id = table;
  task.column_id = column;
  return ret;
}
int environment(ObISQLConnection &connection, const Task &task)
{
  int ret = connection.set_session_variable(ObString::make_string("time_zone"), task.time_zone);
  // Native column assignment must reject truncation, overflow and invalid dates.
  const uint64_t strict = task.sql_mode | SMO_STRICT_ALL_TABLES | SMO_NO_ZERO_DATE | SMO_NO_ZERO_IN_DATE;
  if (OB_SUCC(ret)) { ret = connection.set_session_variable(ObString::make_string("sql_mode"), strict); }
  return ret;
}
int validate_result(ObISQLConnection &connection, const Task &task,
                    const AiColumnDefinition &definition, const ObString &value)
{
  if (value.length() > MAX_VALUE) { return OB_SIZE_OVERFLOW; }
  // MySQL accepts some numeric prefixes even in casts. AI integer responses
  // must be a complete base-10 integer before native range validation.
  if (definition.result_type_ == "BIGINT") {
    int64_t first = 0, last = value.length();
    while (first < last && isspace(static_cast<unsigned char>(value.ptr()[first]))) { ++first; }
    while (last > first && isspace(static_cast<unsigned char>(value.ptr()[last-1]))) { --last; }
    if (first < last && (value.ptr()[first] == '+' || value.ptr()[first] == '-')) { ++first; }
    if (first == last) { return OB_INVALID_DATA; }
    for (; first < last; ++first) { if (value.ptr()[first] < '0' || value.ptr()[first] > '9') { return OB_INVALID_DATA; } }
  }
  int ret = connection.start_transaction();
  if (OB_FAIL(ret)) { return ret; }
  ObSqlString sql;
  if (OB_FAIL(sql.append("UPDATE "))) {
  } else if (OB_FAIL(table_name(sql, task))) {
  } else if (OB_FAIL(sql.append(" SET "))) {
  } else if (OB_FAIL(quote(sql, task.column))) {
  } else if (OB_FAIL(sql.append("="))) {
  } else if (OB_FAIL(literal(sql, value, true))) {
  } else if (OB_FAIL(sql.append(" WHERE "))) {
  } else if (OB_FAIL(row_guard(sql, task))) {
  } else { ret = write(connection, sql); }
  // This is a worker-owned validation transaction. No intermediate result or
  // index item is committed; the entire request group is published separately.
  const int rollback = connection.rollback();
  if (OB_SUCC(ret)) { ret = rollback; }
  return ret;
}
int call_model(ObISQLConnection &connection, sql::ObSQLSessionInfo &session,
               const Task &task, const AiColumnDefinition &definition,
               ObIAllocator &arena, ObString &value)
{
  int ret = environment(connection, task);
  ObSqlString sql;
  if (OB_FAIL(ret)) { return ret; }
  AiColumnSessionScope typed(session, definition.system_prompt_);
  if (OB_FAIL(sql.append(definition.function_ == AiColumnDefinition::EMBED ? "SELECT AI_EMBED(" : "SELECT AI_COMPLETE("))) {
  } else if (OB_FAIL(literal(sql, definition.model_, true))) {
  } else if (OB_FAIL(sql.append(","))) {
  } else if (task.input_null) { ret = sql.append("NULL");
  } else {
    if (definition.input_is_json_ && OB_FAIL(sql.append("CAST("))) {
    } else { ret = literal(sql, task.input, true); }
    if (OB_SUCC(ret) && definition.input_is_json_) { ret = sql.append(" AS JSON)"); }
  }
  if (OB_SUCC(ret) && definition.function_ == AiColumnDefinition::EMBED) {
    ret = sql.append_fmt(",%ld", definition.dimension_);
  } else if (OB_SUCC(ret) && !task.options_null) {
    if (OB_FAIL(sql.append(",CAST("))) {
    } else if (OB_FAIL(literal(sql, task.options, true))) {
    } else { ret = sql.append(" AS JSON)"); }
  }
  if (OB_SUCC(ret)) { ret = sql.append(") AS ai_value"); }
  if (OB_SUCC(ret)) {
    ObISQLClient::ReadResult result;
    if (OB_FAIL(connection.execute_read(sql.string(), result))) {
    } else if (!result.get_result()) { ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(result.get_result()->next())) {
    } else { ret = text(*result.get_result(), "ai_value", arena, value); }
  }
  if (OB_SUCC(ret)) { ret = validate_result(connection, task, definition, value); }
  return ret;
}
int select_prefix(ObSqlString &sql, bool inputs)
{
  if (inputs) { return sql.append(TASK_SELECT); }
  const char *start = strstr(TASK_SELECT, "w.input_value,w.options_value");
  if (!start) { return OB_ERR_UNEXPECTED; }
  int ret = sql.append(TASK_SELECT, start-TASK_SELECT);
  if (OB_SUCC(ret)) { ret = sql.append("NULL AS input_value,NULL AS options_value"); }
  if (OB_SUCC(ret)) { ret = sql.append(start+strlen("w.input_value,w.options_value")); }
  return ret;
}
int group_delete(ObISQLConnection &connection, const Task &task)
{
  ObSqlString sql;
  int ret = sql.append("DELETE FROM oceanbase.__all_ai_column_task WHERE ");
  if (OB_SUCC(ret)) { ret = group_key(sql, task); }
  if (OB_SUCC(ret)) { ret = write(connection, sql); }
  return ret;
}
int lock_row(ObISQLConnection &connection, const Task &task, bool &exists)
{
  ObSqlString sql;
  int ret = sql.append("SELECT 1 FROM ");
  exists = false;
  if (OB_SUCC(ret)) { ret = table_name(sql, task); }
  if (OB_SUCC(ret)) { ret = sql.append(" WHERE "); }
  if (OB_SUCC(ret)) { ret = row_guard(sql, task); }
  if (OB_SUCC(ret)) { ret = sql.append(" FOR UPDATE"); }
  if (OB_SUCC(ret)) {
    ObISQLClient::ReadResult result;
    if (OB_FAIL(connection.execute_read(sql.string(), result))) {
    } else if (!result.get_result()) { ret = OB_ERR_UNEXPECTED;
    } else if ((ret = result.get_result()->next()) == OB_ITER_END) { ret = OB_SUCCESS;
    } else if (OB_SUCC(ret)) { exists = true; }
  }
  return ret;
}
int publish(ObISQLConnection &connection, const Task &group)
{
  int ret = environment(connection, group);
  bool exists = false;
  if (OB_FAIL(ret)) { return ret; }
  if (OB_FAIL(connection.start_transaction())) { return ret; }
  if (OB_FAIL(lock_row(connection, group, exists))) {
  } else if (!exists) { ret = group_delete(connection, group);
  } else {
    ObArenaAllocator arena("AiColPublish");
    ObArray<Task> tasks;
    int64_t budget = MAX_VALUE / 2;
    bool all_terminal = true;
    {
      ObSqlString sql;
      ObISQLClient::ReadResult result;
      if (OB_FAIL(select_prefix(sql, false))) {
      } else if (OB_FAIL(sql.append(" WHERE w.table_id="))) {
      } else if (OB_FAIL(sql.append_fmt("%lu AND w.rowkey_hash=", group.table_id))) {
      } else if (OB_FAIL(literal(sql, group.hash))) {
      } else if (OB_FAIL(sql.append(" AND w.request_id="))) {
      } else if (OB_FAIL(literal(sql, group.request))) {
      } else if (OB_FAIL(sql.append(" ORDER BY w.column_id LIMIT 4097"))) {
      } else if (OB_FAIL(connection.execute_read(sql.string(), result))) {
      } else if (!result.get_result()) { ret = OB_ERR_UNEXPECTED;
      } else {
        while (OB_SUCC(ret) && (ret = result.get_result()->next()) == OB_SUCCESS) {
          Task task;
          if (OB_FAIL(load_task(*result.get_result(), arena, task, &budget))) {
          } else if (tasks.count() >= 4096) { ret = OB_SIZE_OVERFLOW;
          } else {
            all_terminal &= task.state == 2 || task.state == 4;
            ret = tasks.push_back(task);
          }
        }
        if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
      }
    }
    if (OB_SUCC(ret) && all_terminal && !tasks.empty()) {
      ObSqlString sql;
      if (OB_FAIL(sql.append("UPDATE "))) {
      } else if (OB_FAIL(table_name(sql, group))) {
      } else { ret = sql.append(" SET "); }
      for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
        const Task &task = tasks.at(i);
        if (i > 0 && OB_FAIL(sql.append(","))) {
        } else if (OB_FAIL(quote(sql, task.column))) {
        } else if (OB_FAIL(sql.append("="))) {
        } else if (task.state == 2) { ret = literal(sql, task.result, true);
        } else { ret = sql.append("NULL"); }
        if (OB_SUCC(ret)) { ret = sql.append(","); }
        if (OB_SUCC(ret)) { ret = quote(sql, task.state_column); }
        if (OB_SUCC(ret)) { ret = sql.append(task.state == 2 ? "='READY'," : "='FAILED',"); }
        if (OB_SUCC(ret)) { ret = quote(sql, task.spec_column); }
        if (OB_SUCC(ret)) { ret = sql.append_fmt("='%ld'", task.version); }
      }
      if (OB_SUCC(ret)) { ret = sql.append(" WHERE "); }
      for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
        if (i > 0 && OB_FAIL(sql.append(" AND "))) {
        } else { ret = row_guard(sql, tasks.at(i)); }
      }
      // One UPDATE and one transaction publish every terminal member, including
      // failed members, through normal DML/vector-index maintenance.
      if (OB_SUCC(ret)) { ret = write(connection, sql); }
      if (OB_SUCC(ret)) { ret = group_delete(connection, group); }
    }
  }
  if (OB_SUCC(ret)) { ret = connection.commit(); }
  if (OB_FAIL(ret)) { (void)connection.rollback(); }
  return ret;
}
}

int AiColumnWorker::initialize(ObMySQLProxy &proxy)
{
  proxy_ = &proxy;
  ObISQLConnectionGuard guard;
  int ret = proxy.acquire_connection(guard, 0);
  auto *session = OB_SUCC(ret) ? query::ObInnerSQLConnectionAccess::get_session(guard.get_ptr()) : nullptr;
  if (OB_SUCC(ret) && !session) { ret = OB_ERR_UNEXPECTED; }
  if (OB_SUCC(ret)) {
    AiColumnSessionScope trusted(*session);
    ObSqlString sql;
    // Startup owns this database instance exclusively. Work left by the old
    // process is reclaimable immediately; persisted successful members remain.
    if (OB_FAIL(sql.append("UPDATE oceanbase.__all_ai_column_task SET state=0,lease_owner=NULL,lease_until=NULL WHERE state=1"))) {
    } else { ret = write(*guard.get_ptr(), sql); }
  }
  if (OB_SUCC(ret)) { ret = set_thread_count(4); }
  return ret;
}
void AiColumnWorker::run1()
{
  int64_t last_reconcile = 0;
  int64_t idle_pause = 10000;
  while (!has_set_stop()) {
    const int ret = process_group();
    if (ret != OB_SUCCESS && ret != OB_ITER_END) {
      if (REACH_TIME_INTERVAL(5000000)) { LOG_WARN("AI column worker iteration failed", K(ret)); }
    }
    if (get_thread_idx() == 0 && ObTimeUtility::current_time()-last_reconcile >= 1000000) {
      last_reconcile = ObTimeUtility::current_time();
      const int reconcile = reconcile_added_columns();
      if (reconcile != OB_SUCCESS && reconcile != OB_ITER_END && REACH_TIME_INTERVAL(5000000)) {
        LOG_WARN("AI column post-DDL reconciliation failed", K(reconcile));
      }
    }
    // Idle polling is bounded, cancellation-aware and independent of clients.
    if (ret == OB_SUCCESS) {
      idle_pause = 10000;
    } else {
      idle_pause = std::min<int64_t>(1000000, idle_pause*2);
      for (int64_t remaining = idle_pause; remaining > 0 && !has_set_stop(); remaining -= 100000) {
        usleep(std::min<int64_t>(remaining, 100000));
      }
    }
  }
}

int AiColumnWorker::reconcile_added_columns()
{
  int ret = OB_SUCCESS;
  ObISQLConnectionGuard guard;
  if (!proxy_) { return OB_NOT_INIT; }
  if (OB_FAIL(proxy_->acquire_connection(guard, 0))) { return ret; }
  auto *connection = guard.get_ptr();
  auto *session = query::ObInnerSQLConnectionAccess::get_session(connection);
  if (!session || !connection) { return OB_ERR_UNEXPECTED; }
  AiColumnSessionScope trusted(*session);
  ObArenaAllocator arena("AiColReconcile");
  ObArray<Task> columns;
  {
    ObSqlString sql;
    ObISQLClient::ReadResult result;
    if (OB_FAIL(sql.append_fmt("SELECT a.table_id,a.column_id,a.column_name,d.database_name,t.table_name,"
        "r.column_name AS request_name,s.column_name AS state_name FROM oceanbase.__all_ai_column a "
        "JOIN oceanbase.__all_table t ON t.table_id=a.table_id "
        "JOIN oceanbase.__all_database d ON d.database_id=t.database_id "
        "JOIN oceanbase.__all_column r ON r.table_id=a.table_id AND r.column_id=a.request_column_id "
        "JOIN oceanbase.__all_column s ON s.table_id=a.table_id AND s.column_id=a.state_column_id "
        "WHERE (a.table_id>%lu OR (a.table_id=%lu AND a.column_id>%lu)) "
        "ORDER BY a.table_id,a.column_id LIMIT 16", reconcile_table_, reconcile_table_, reconcile_column_))) {
    } else if (OB_FAIL(connection->execute_read(sql.string(), result))) {
    } else if (!result.get_result()) { ret = OB_ERR_UNEXPECTED;
    } else {
      while (OB_SUCC(ret) && (ret = result.get_result()->next()) == OB_SUCCESS) {
        Task column;
        int64_t table_id = 0, column_id = 0;
        auto &row = *result.get_result();
        if (OB_FAIL(row.get_int("table_id", table_id))) {
        } else if (OB_FAIL(row.get_int("column_id", column_id))) {
        } else if (OB_FAIL(text(row, "column_name", arena, column.column))) {
        } else if (OB_FAIL(text(row, "database_name", arena, column.database))) {
        } else if (OB_FAIL(text(row, "table_name", arena, column.table))) {
        } else if (OB_FAIL(text(row, "request_name", arena, column.request_column))) {
        } else if (OB_FAIL(text(row, "state_name", arena, column.state_column))) {
        } else {
          column.table_id = table_id;
          column.column_id = column_id;
          ret = columns.push_back(column);
        }
      }
      if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
    }
  }
  if (OB_FAIL(ret)) { return ret; }
  if (columns.empty()) { reconcile_table_ = reconcile_column_ = 0; return OB_ITER_END; }
  for (int64_t i = 0; OB_SUCC(ret) && !has_set_stop() && i < columns.count(); ++i) {
    const Task &column = columns.at(i);
    ObSqlString sql;
    if (OB_FAIL(sql.append("ALTER TABLE "))) {
    } else if (OB_FAIL(table_name(sql, column))) {
    } else if (OB_FAIL(sql.append(" RETRY AI COLUMN "))) {
    } else if (OB_FAIL(quote(sql, column.column))) {
    } else if (OB_FAIL(sql.append(" WHERE "))) {
    } else if (OB_FAIL(quote(sql, column.request_column))) {
    } else if (OB_FAIL(sql.append(" IS NULL AND ("))) {
    } else if (OB_FAIL(quote(sql, column.state_column))) {
    } else if (OB_FAIL(sql.append(" IS NULL OR "))) {
    } else if (OB_FAIL(quote(sql, column.state_column))) {
    } else { ret = sql.append("='PENDING')"); }
    if (OB_SUCC(ret)) {
      const bool old_internal = session->is_ai_column_internal();
      const bool old_hidden = session->is_ai_column_hidden_access();
      // Native RETRY must run its ordinary transactional invalidation/enqueue
      // path. Only hidden-column name access is granted for this reconciliation.
      session->set_ai_column_hidden_access(true);
      session->set_ai_column_internal(false);
      ret = write(*connection, sql);
      session->set_ai_column_internal(old_internal);
      session->set_ai_column_hidden_access(old_hidden);
    }
    if (OB_SUCC(ret)) { reconcile_table_ = column.table_id; reconcile_column_ = column.column_id; }
  }
  return ret;
}

// Group ownership and publication are implemented below. HTTP is never issued
// while a business/claim/publication transaction is open.
int AiColumnWorker::process_group()
{
  int ret = OB_SUCCESS;
  ObISQLConnectionGuard guard;
  if (!proxy_) { return OB_NOT_INIT; }
  if (OB_FAIL(proxy_->acquire_connection(guard, 0))) { return ret; }
  ObISQLConnection *connection = guard.get_ptr();
  auto *session = query::ObInnerSQLConnectionAccess::get_session(connection);
  if (!connection || !session) { return OB_ERR_UNEXPECTED; }
  AiColumnSessionScope trusted(*session);
  connection->set_user_timeout(30L*1000*1000);
  ObArenaAllocator arena("AiColClaim");
  Task group;
  ObString owner;
  {
    ObSqlString sql;
    ObISQLClient::ReadResult result;
    if (OB_FAIL(select_prefix(sql, false))) {
    } else if (OB_FAIL(sql.append(
        " WHERE ((w.state IN (0,3) AND w.eligible_ts<=NOW(6)) OR "
        "(w.state=1 AND w.lease_until<NOW(6)) OR (w.state IN (2,4) AND NOT EXISTS "
        "(SELECT 1 FROM oceanbase.__all_ai_column_task n WHERE n.table_id=w.table_id "
        "AND n.rowkey_hash=w.rowkey_hash AND n.request_id=w.request_id AND n.state NOT IN (2,4)))) "
        "ORDER BY w.eligible_ts,w.column_id LIMIT 1"))) {
    } else if (OB_FAIL(connection->execute_read(sql.string(), result))) {
    } else if (!result.get_result()) { ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(result.get_result()->next())) {
    } else { ret = load_task(*result.get_result(), arena, group); }
  }
  if (OB_FAIL(ret)) { return ret; }
  if (OB_FAIL(environment(*connection, group))) { return ret; }
  {
    ObISQLClient::ReadResult result;
    if (OB_FAIL(connection->execute_read(ObString::make_string("SELECT UUID() AS owner_id"), result))) {
    } else if (!result.get_result()) { ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(result.get_result()->next())) {
    } else { ret = text(*result.get_result(), "owner_id", arena, owner); }
  }
  if (OB_FAIL(ret)) { return ret; }
  if (OB_FAIL(connection->start_transaction())) { return ret; }
  bool exists = false;
  if (OB_FAIL(lock_row(*connection, group, exists))) {
  } else if (!exists) { ret = group_delete(*connection, group);
  } else {
    ObSqlString sql;
    if (OB_FAIL(sql.append("UPDATE oceanbase.__all_ai_column_task SET state=1,lease_owner="))) {
    } else if (OB_FAIL(literal(sql, owner))) {
    } else if (OB_FAIL(sql.append(",lease_until=DATE_ADD(NOW(6),INTERVAL 300 SECOND) WHERE "))) {
    } else if (OB_FAIL(group_key(sql, group))) {
    } else if (OB_FAIL(sql.append(" AND ((state IN (0,3) AND eligible_ts<=NOW(6)) OR (state=1 AND lease_until<NOW(6)))"))) {
    } else { ret = write(*connection, sql); }
  }
  if (OB_SUCC(ret)) { ret = connection->commit(); }
  if (OB_FAIL(ret)) { (void)connection->rollback(); return ret; }
  // Claim committed and all row locks released before the first model request.
  while (OB_SUCC(ret) && exists && !has_set_stop()) {
    ObArenaAllocator task_arena("AiColTask");
    Task task;
    {
      ObSqlString sql;
      ObISQLClient::ReadResult result;
      if (OB_FAIL(select_prefix(sql, true))) {
      } else if (OB_FAIL(sql.append(" WHERE w.state=1 AND w.lease_owner="))) {
      } else if (OB_FAIL(literal(sql, owner))) {
      } else if (OB_FAIL(sql.append(" ORDER BY w.column_id LIMIT 1"))) {
      } else if (OB_FAIL(connection->execute_read(sql.string(), result))) {
      } else if (!result.get_result()) { ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result.get_result()->next())) {
      } else { ret = load_task(*result.get_result(), task_arena, task); }
    }
    if (ret == OB_ITER_END) { ret = OB_SUCCESS; break; }
    if ((ret == OB_SIZE_OVERFLOW || ret == OB_INVALID_DATA) && task.table_id != 0 && !task.hash.empty() && !task.request.empty()) {
      const int input_error = ret;
      ObSqlString failed;
      ret = failed.append_fmt("UPDATE oceanbase.__all_ai_column_task SET state=4,error_code=%d,result_value=NULL,lease_owner=NULL,lease_until=NULL WHERE ", input_error);
      if (OB_SUCC(ret)) { ret = key(failed, task, true, owner); }
      if (OB_SUCC(ret)) { ret = write(*connection, failed); }
      if (OB_SUCC(ret)) { continue; }
    }
    if (OB_FAIL(ret)) { break; }
    {
      ObSqlString renew;
      if (OB_FAIL(renew.append("UPDATE oceanbase.__all_ai_column_task SET lease_until=DATE_ADD(NOW(6),INTERVAL 300 SECOND) WHERE state=1 AND lease_owner="))) {
      } else if (OB_FAIL(literal(renew, owner))) {
      } else { ret = write(*connection, renew); }
    }
    if (OB_FAIL(ret)) { break; }
    AiColumnDefinition definition;
    ObString result;
    int result_code = definition.decode(task.packet);
    if (result_code == OB_SUCCESS) { result_code = call_model(*connection, *session, task, definition, task_arena, result); }
    ObSqlString sql;
    if (OB_FAIL(sql.append("UPDATE oceanbase.__all_ai_column_task SET lease_owner=NULL,lease_until=NULL,"))) {
    } else if (result_code == OB_SUCCESS) {
      if (OB_FAIL(sql.append("state=2,error_code=0,result_value="))) {
      } else { ret = literal(sql, result, true); }
    } else if (task.retries < definition.max_retries_) {
      const int64_t delay = 1L << std::min<int64_t>(task.retries, 6);
      ret = sql.append_fmt("state=3,retry_count=%ld,error_code=%d,eligible_ts=DATE_ADD(NOW(6),INTERVAL %ld SECOND),result_value=NULL",
                          task.retries+1, result_code, delay);
    } else { ret = sql.append_fmt("state=4,error_code=%d,result_value=NULL", result_code); }
    if (OB_SUCC(ret)) { ret = sql.append(" WHERE "); }
    if (OB_SUCC(ret)) { ret = key(sql, task, true, owner); }
    if (OB_SUCC(ret)) { ret = write(*connection, sql); }
  }
  if (has_set_stop() || OB_FAIL(ret)) {
    // Release unfinished claims during orderly shutdown. A crash instead relies
    // on persisted leases and the exclusive startup recovery above.
    ObSqlString release;
    if (OB_SUCC(release.append("UPDATE oceanbase.__all_ai_column_task SET state=0,lease_owner=NULL,lease_until=NULL WHERE state=1 AND lease_owner="))
        && OB_SUCC(literal(release, owner))) { (void)write(*connection, release); }
  }
  if (OB_SUCC(ret)) { ret = publish(*connection, group); }
  return ret;
}
} }
