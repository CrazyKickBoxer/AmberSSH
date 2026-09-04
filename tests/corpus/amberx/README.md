# AmberX fuzz corpus

One file per case, replayed against every fuzz target by
`tests/AmberXFuzzTests.cpp` on each test run. A case earns its place here by
having broken something: the file is the minimized input, and the line below
records what it broke and where the fix is.

Keeping them is the point. A fuzzer that finds a bug once and forgets it will
find the same bug again after the next refactor, and the second time nobody
will be watching.

| file | found | what it broke | fixed in |
|---|---|---|---|
| _(none yet)_ | | | |

To add one: write the exact bytes to a file here, name it after the parser and
the defect, and add a row. To reproduce a mutation-run failure, use the seed
the test prints: `AMBERX_FUZZ_SEED=0x... AMBERX_FUZZ_ITERS=200000`.
