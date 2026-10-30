#include <gtest/gtest.h>
#include "share/system_variable/ob_system_variable_init.h"
#include "share/system_variable/ob_sys_var_meta.h"

using namespace oceanbase::common;
using namespace oceanbase::share;

TEST(FileSqlVariable, RegistryIsCompleteAndDisabledByDefault)
{
  ASSERT_EQ(int64_t{ObSysVarMeta::ALL_SYS_VARS_COUNT}, ObSysVariables::get_amount());
  bool found = false;
  for (int64_t i = 0; i < ObSysVariables::get_amount(); ++i) {
    if (ObSysVariables::get_sys_var_id(i) == SYS_VAR_ENABLE_FILE_SQL) {
      found = true;
      EXPECT_EQ(ObString::make_string("enable_file_sql"), ObSysVariables::get_name(i));
      EXPECT_EQ(ObString::make_string("0"), ObSysVariables::get_value(i));
      EXPECT_TRUE((ObSysVariables::get_flags(i) & ObSysVarFlag::SESSION_SCOPE) != 0);
      EXPECT_TRUE((ObSysVariables::get_flags(i) & ObSysVarFlag::INFLUENCE_PLAN) != 0);
    }
  }
  EXPECT_TRUE(found);
}
