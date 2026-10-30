#include <gtest/gtest.h>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include "sql/resolver/ob_resolver_utils.h"

using namespace oceanbase::common;
using oceanbase::sql::ObResolverUtils;

TEST(SecureFilePath, RejectsMissingDirectoryAndPrefixSibling)
{
  char pattern[] = "/tmp/seekdb-secure-path-XXXXXX";
  char *directory = mkdtemp(pattern);
  ASSERT_NE(nullptr, directory);
  const std::string root(directory);
  const std::string inside = root + "/data.csv";
  const std::string sibling = root + "-sibling/data.csv";
  EXPECT_EQ(OB_SUCCESS, ObResolverUtils::check_secure_path(ObString::make_string(root.c_str()),
                                                           ObString::make_string(inside.c_str())));
  EXPECT_EQ(OB_ERR_NO_PRIVILEGE, ObResolverUtils::check_secure_path(ObString::make_string(root.c_str()),
                                                                    ObString::make_string(sibling.c_str())));
  ASSERT_EQ(0, rmdir(directory));
  EXPECT_EQ(OB_ERR_NO_PRIVILEGE, ObResolverUtils::check_secure_path(ObString::make_string(root.c_str()),
                                                                    ObString::make_string(inside.c_str())));
}
