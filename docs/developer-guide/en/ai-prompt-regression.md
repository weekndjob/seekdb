# AI_PROMPT types and template rendering

`AI_PROMPT(template, args...)` constructs a JSON prompt object. The template may
be a string or TEXT/LONGTEXT, and arguments may additionally be JSON values.
JSON arrays and objects remain structured in the returned `args` array. SQL NULL
and unsupported SQL argument types produce the existing invalid-type error; an
empty template produces an invalid-argument error. An empty argument is allowed.

LOB values are materialized through the expression engine's text reader, without
narrowing TEXT/LONGTEXT to VARCHAR. The returned JSON is packed into the expression
result before the temporary allocator leaves scope.

Placeholder validation happens when `AI_COMPLETE` renders the prompt. Constructing
`AI_PROMPT('{0}/{1}', 'value')` remains valid. At render time, `{0}` selects argument
zero, repeated references insert the complete value each time, and nested prompt
objects render recursively. Other JSON arguments use the existing JSON printer.
Strings are inserted literally, including Unicode, quotes, backslashes and NUL.

`{{` and `}}` escape literal braces. Nonnumeric literal braces such as `{word}` and
`{}` remain text. Numeric placeholders must contain unsigned decimal digits and a
closing brace; signed, malformed, overflowing and out-of-range indices are
rejected. Recursive prompt expansion is limited to 64 prompt objects, and output
length is checked before narrowing to `ObString`'s 32-bit length. Output buffers
transfer ownership to the supplied allocator instead of returning a pointer owned
by a local `ObStringBuffer`.

The public 2026 baseline omits the unit-test CMake entry point described in its
developer guide. `unittest/CMakeLists.txt` supplies the Linux `sql_tests` and
`pretest` targets, discovers SQL test sources, links the actual production
`oceanbase` library, and uses Google Test's shared main. It registers the full
binary with CTest, without filtering other SQL tests.

```bash
./build.sh release
make -C build_release -j2 seekdb sql_tests
ctest --test-dir build_release -R '^sql_tests$' --output-on-failure
```

`unittest/sql/engine/expr/test_ai_prompt.cpp` covers repeated long arguments, JSON
arrays/objects/scalars, nested prompts, Unicode/NUL/escaping, empty output, malformed
prompt shapes, illegal and overflowing indices, output-buffer ownership and the
recursion bound. The unchanged official SQL case and expected result are:

```text
tools/deploy/mysql_test/test_suite/ai_function/t/ai_prompt.test
tools/deploy/mysql_test/test_suite/ai_function/r/mysql/ai_prompt.result
```

The broader `ai_function_enhancement.test` also exercises other AI subproblems.
This change implements the AI_PROMPT subproblem; it does not claim the other five
subproblems or a hidden-platform score. Implementation, focused tests and this
document were prepared with AI assistance and require actual build/runtime
validation before submission.
