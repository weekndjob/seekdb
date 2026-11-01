#include <gtest/gtest.h>
#include "sql/resolver/dml/ob_dml_stmt.h"
#include "sql/resolver/expr/ob_raw_expr_copier.h"
using namespace oceanbase::common;
using namespace oceanbase::sql;
TEST(FileTableCopy, KeepsOrdinaryTablesSafeWithDefaultAllocator)
{
  ObArenaAllocator arena;
  ObRawExprFactory factory(arena);
  ObRawExprCopier copier(factory);
  TableItem source, destination;
  destination.file_scan_definition_ = ObString::make_string("old definition");
  ASSERT_EQ(OB_SUCCESS, destination.deep_copy(copier, source));
  EXPECT_TRUE(destination.file_scan_definition_.empty());
}
TEST(FileTableCopy, OwnsMetadataWhenTheOptionalAllocatorIsOmitted)
{
  ObArenaAllocator arena;
  ObRawExprFactory factory(arena);
  ObRawExprCopier copier(factory);
  TableItem source, destination;
  source.type_ = TableItem::FILE_TABLE;
  char definition[] = "scan metadata";
  source.file_scan_definition_ = ObString::make_string(definition);
  ASSERT_EQ(OB_SUCCESS, destination.deep_copy(copier, source));
  EXPECT_NE(definition, destination.file_scan_definition_.ptr());
  definition[0] = 'X';
  EXPECT_EQ(0, destination.file_scan_definition_.compare("scan metadata"));
}
