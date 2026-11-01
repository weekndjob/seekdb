#include <gtest/gtest.h>
#include "sql/resolver/ob_resolver_define.h"
#include "sql/resolver/dml/ob_column_namespace_checker.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/resolver/ob_schema_checker.h"
#include "sql/session/ob_sql_session_info.h"
using namespace oceanbase::common;
using namespace oceanbase::sql;
TEST(FileColumnNamespace, FindsDynamicColumnsWithoutTreatingThePathAsAFunction)
{
  ObResolverParams params;
  ObSchemaChecker schema;
  ObSQLSessionInfo session;
  params.schema_checker_ = &schema;
  params.session_info_ = &session;
  ObColumnNamespaceChecker checker(params);
  ObSelectStmt statement;
  TableItem table;
  table.type_ = TableItem::FILE_TABLE;
  table.table_id_ = 17;
  ColumnItem id;
  id.table_id_ = 17;
  id.column_name_ = ObString::make_string("ID");
  ASSERT_EQ(OB_SUCCESS, statement.get_column_items().push_back(id));
  checker.set_dml_stmt(&statement);
  bool found = false;
  ASSERT_EQ(OB_SUCCESS, checker.check_column_exists(table, ObString::make_string("id"), found));
  EXPECT_TRUE(found);
  ASSERT_EQ(OB_SUCCESS, checker.check_column_exists(table, ObString::make_string("missing"), found));
  EXPECT_FALSE(found);
  table.table_id_ = 18;
  ASSERT_EQ(OB_SUCCESS, checker.check_column_exists(table, ObString::make_string("id"), found));
  EXPECT_FALSE(found);
}
