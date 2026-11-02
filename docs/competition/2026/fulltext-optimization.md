# 全文索引调优

官方题面：https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/za7cfo5cazcrihhq

## 问题与修改依据

原提交 `a582e19f149a71bb23881f0e17ba92154d1ac592` 在平台执行成功，但综合提升率为 -8.0159%，得分为 0。它通过了功能检查，却没有证明稳定的整体性能收益。本次保留该提交的二进制，使用相同机器、编译配置、实例和官方完整参数进行配对测试；不使用仓库内较旧的固定 CI 基线来宣称本次提升或官方得分。

查询执行栈采样显示，普通 MATCH 查询使用 DaaT，热点包括逐文档归并、通用文档 ID 比较和 score 的 BOOL 表达式求值；LIMIT 查询使用 Block-Max WAND。采样使用 gdb 启动独立实例，未附加或修改系统数据库。采样仅用于定位路径，不将有限样本解释成精确 CPU 占比。

分词代码检查还发现每篇文档创建独立范围词典 arena、仲裁哈希表，以及加载一份实际未参与 IK 分词的停用词词典。这些工作可以在保持现有行为的前提下减少。

## 内核修改

- 范围词典使用调用方分配器，仅释放自身的包装对象和数组。数组扩容失败也释放新对象；不会重置调用方分配器。
- IK 仲裁链使用有序小数组，短文本无需分配哈希表，查找使用二分；SMART/MAX 的冲突选择规则保持原有实现。
- 移除 IK 从未使用的停用词词典加载和包装。原代码中的 IK 停用词匹配本来处于注释状态；没有关闭任何已有过滤规则，也没有更改公共停用词检查器或 add-word 标志。
- 词频归并由两次哈希查找改为一次更新，保留首次计数为 1 的原有约定，以及等价词最后一次键表示。
- 对 Varchar + CS_TYPE_BINARY 文档 ID 使用原始字节及长度比较，普通归并和 BMW 范围比较共用相同排序语义。保留空值、MIN/MAX 范围边界；其他类型和排序规则继续使用原比较函数。
- 仅对向量执行中精确的 BOOL(已计算的非空 double score) 使用等价真假判断；保留负数、正负零和 NaN 的 double 语义。其他过滤表达式保留通用求值路径。BM25 分数仍正常计算、投影，排序和剪枝规则未改。
- 每批结果只设置一次表达式的 projected/evaluated 标志，仍逐行设置对应结果位。
- 修复部分初始化失败时的重复释放：reset 后清空 ctx 和词典指针，使失败清理与随后析构可以安全衔接。新增回归在修复前复现重复释放并退出 139，修复后通过。

原提交的单次字符解码、直接传递已取得字符信息和同字符集停用词检查优化保留。实验中的英语 normalizer 视图优化因完整基准未证明收益而撤回。没有修改官方工作流、数据、SQL、参数、计时逻辑、评分脚本或固定基线；没有跳过必要分词、相关性计算或检索处理。

## 正确性与构建

- Release 原生构建通过；CTest `sql_tests` 通过，完整执行 58 个单元测试。新增覆盖分配失败、分配器所有权、重复清理、80 条仲裁链、重叠词、词频与最后键表示、二进制比较及范围边界。二进制 ID 包含空值、空串、NUL、尾空格和固定随机种子的字节串，与原比较函数逐对核对。
- 334 项前后输出对比完全一致：320 个多语言/随机/长文本 TOKENIZE 样本，四类 MATCH 全部有序命中 ID 和分数，以及十项阈值过滤、BOOLEAN、NOT MATCH、排序、LIMIT、OFFSET 和无命中查询。两个二进制读取同一份物理索引，避免重建引起 BM25 统计状态变化。
- 官方 `fts_index/tokenize_function.test` 的 SQL、结果和错误断言通过。MariaDB 测试客户端不支持原用例 `result_format 4`：临时副本只去除该格式指令，将表格边框转为制表符、去除格式空行，并将错误名 `ER_INVALID_JSON_TEXT_IN_PARAM` 替换为同一错误码 3141。仓库原用例及预期文件未改。
- 当前构建没有注册 UTF-16 排序规则，没有声称完成 UTF-16 SQL 端到端支持。

## 官方完整参数的同机配对结果

Ubuntu 虚拟机，CPU 2、memory_limit 1G、datafile_size 512M；相同 Release 配置，同一隔离实例。未在计时期间并行构建或运行其他测试。顺序为候选、原提交、原提交、候选，每次使用新报告文件，并重新导入和构建索引：

`ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`

| 指标 | 原提交 1 | 候选 1 | 原提交 2 | 候选 2 |
| --- | ---: | ---: | ---: | ---: |
| build_ik_all_sec | 9.782 | 9.369 | 9.66 | 9.674 |
| build_ik_content_sec | 7.613 | 7.407 | 7.611 | 7.412 |
| build_beng_en_sec | 3.871 | 3.67 | 3.875 | 3.671 |
| tokenize_ik_avg_ms | 0.2896 | 0.2812 | 0.299 | 0.2627 |
| tokenize_beng_avg_ms | 0.185 | 0.1893 | 0.1855 | 0.1691 |
| query_cn_avg_ms | 2.2076 | 2.7088 | 2.3182 | 2.0032 |
| query_beng_avg_ms | 3.2516 | 2.7699 | 3.2357 | 2.8054 |
| query_mixed_avg_ms | 2.5669 | 2.1672 | 2.5688 | 2.134 |
| query_limit_avg_ms | 2.9616 | 2.7402 | 2.9446 | 2.7634 |

按官方的“类别内平均、三类等权”公式，对比原提交二进制计算：

| 类别 | 配对 1 提升率 | 配对 2 提升率 |
| --- | ---: | ---: |
| build | 4.04% | 2.58% |
| tokenize | 0.29% | 10.49% |
| query | 3.79% | 12.49% |
| 综合 | 2.71% | 8.52% |

以上是对原提交的本机提升率，不是对平台固定基线的提升率或正式分数。分词与构建计时存在机器负载波动，逐项退化也计入上述综合结果，没有删样本或以中位数代替官方均值。各轮命中数均为 `8001 / 11000 / 7332 / 20`。正式成绩需再次提交后由平台确认，本地验证不能保证正式得分。

原始报告、测试输出、二进制哈希、正确性快照哈希与源码输入哈希见 [fulltext-results](fulltext-results/paired-summary.json)。报告中的 `git_head=a582...`、`git_dirty=1` 表示对原提交工作树上的候选修改进行测量，不能用该字段将候选误认为原二进制；来源以对应二进制 SHA-256 和源码清单区分。

## 复现

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

本分支保留此前的 AI 函数、文件 SQL 和 AI 列实现。本次没有新增服务、后台线程或持久化格式。
