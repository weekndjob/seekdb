# 全文索引调优

题面：https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/za7cfo5cazcrihhq

## 分析与修改

基线源码为 `38906386fab9ea2885ec9778711656b26604c9f6`。本次只修改分词热路径，保留全文索引格式、字典、分词规则和 MATCH 执行规则。

1. `ObFTCharUtil::do_classify` 原先依次调用字符分类辅助函数，各函数重新解码同一个字符。现在只解码一次，再按原有顺序分类；UTF-16 代理字符分支和解码错误处理保持原有行为。
2. `ObIKFTParser::process_one_char` 已经取得字符、长度和类型，原先每个处理器的 `process` 又从上下文读取这些值。现在直接传给 `do_process`。处理器没有推进上下文游标，游标仍由原来的外层循环推进。
3. `ObStopWordChecker::check_stopword` 原先每个词都构造临时 arena，而相同字符集的默认 `charset_convert` 只返回输入视图。现在有效且相同字符集走直接查找；查找仍使用停用词集合的排序规则，保证大小写行为不受输入排序规则影响。不同字符集保留原转换路径。

没有修改官方基准的数据、SQL、参数、计时逻辑、评分脚本和固定基线。

## 正确性验证

- Release 原生构建通过；CTest 的 `sql_tests` 通过。新增测试覆盖字符类别边界、全角字母与数字、无效/截断 UTF-8、空输入和跨输入排序规则的停用词匹配。
- 320 组 TOKENIZE 输出对比，包含 IK/BENG、中英日韩文字、补充平面字符、全角字符、长文本和固定随机种子的混合文本。
- 四类 MATCH 查询逐行对比全部有序命中 ID 和相关性分数。为控制索引统计状态，前后两个二进制读取同一份已构建索引；全部输出完全相同。
- `fts_index/tokenize_function.test` 的 SQL 和错误断言通过。当前环境的 MariaDB 测试客户端不支持原用例的 `result_format 4`；临时副本移除输出格式指令，将对应预期的表格边框转成制表符、移除格式产生的空行，并将 MySQL 错误名 `ER_INVALID_JSON_TEXT_IN_PARAM` 替换为数字 `3141`。原用例和原预期文件没有修改。

当前构建不注册 UTF-16 排序规则，测试确认这些排序规则仍返回不支持；没有将它们标记为已完成 SQL 端到端验证。

## 完整基准结果

环境为同一台 Ubuntu 虚拟机、同一 Release 构建配置、同一隔离实例，实例配置为 CPU 2、memory_limit 1G、datafile_size 512M。优化前后串行运行，测试期间没有并行构建。保留基线二进制后再编译候选版本。

两轮均使用官方完整参数：`ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`。每轮重新生成数据并构建索引。下表列出最终候选版本及其基线的两轮结果；时间单位见指标名，分词和查询单位为 ms。

| 指标 | 基线首轮 | 优化首轮 | 基线配对复测 | 优化配对复测 |
| --- | ---: | ---: | ---: | ---: |
| build_ik_all_sec | 9.661 | 10.058 | 9.867 | 10.088 |
| build_ik_content_sec | 7.723 | 7.913 | 7.924 | 7.622 |
| build_beng_en_sec | 4.174 | 3.972 | 3.889 | 3.981 |
| tokenize_ik_avg_ms | 0.2381 | 0.2554 | 0.2564 | 0.2445 |
| tokenize_beng_avg_ms | 0.1891 | 0.1881 | 0.1745 | 0.1756 |
| query_cn_avg_ms | 2.7407 | 2.3875 | 2.3500 | 2.3521 |
| query_beng_avg_ms | 4.0309 | 3.4468 | 3.2614 | 3.2603 |
| query_mixed_avg_ms | 3.1163 | 2.5332 | 2.5420 | 2.5308 |
| query_limit_avg_ms | 3.0754 | 3.0426 | 2.9370 | 2.9370 |

所有轮次的查询命中数分别为 `8001 / 11000 / 7332 / 20`。

按官方三类等权方法计算，配对复测的构建提升为 **-0.26%**、分词提升 **2.01%**、查询提升 **0.10%**，综合提升约 **0.61%**。其中 IK 分词降低约 4.64%。首轮与复测存在波动，不能将这种较小的综合变化认定为稳定的显著整体收益。

原始固定 CI 基线与本机硬件不同。官方评分脚本检查完整参数并对候选复测报告输出性能参考分 100/100；基线版本在本机也已经明显快于该固定 CI 基线，因此这个本地参考分不能证明本次修改取得官方满分。正式成绩以大赛统一环境为准。

## 复现官方评测

在专用实例运行，因为脚本会删除并重建 `fts_large_bench` 数据库：

```bash
export MYSQL='mysql -h127.0.0.1 -P2885 -uroot -N -s -A --default-character-set=utf8mb4'
export MYSQL_VERBOSE='mysql -h127.0.0.1 -P2885 -uroot -A --default-character-set=utf8mb4'
REPORT_FILE=$(mktemp /tmp/fts-bench-report.XXXXXX)
LABEL=local ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 \
  SAMPLES=3 WARMUP=30 SKIP_LOAD=0 OUTPUT="$REPORT_FILE" \
  bash tools/benchmark/fts_large_bench.sh
python3 tools/benchmark/fts_large_bench_score.py "$REPORT_FILE"
```

本分支保留此前已经完成的 AI 函数、文件 SQL 和 AI 列实现。本次修改没有新增服务、后台线程或持久化格式。
