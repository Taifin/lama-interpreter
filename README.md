# Lama-interpreter

Bytecode-based iterative interpreter for the Lama language. To build:

```
mkdir build && cd build && cmake .. && make 
```

To run:

```
./lama_interpreter <input>.bc
```

The `DEBUG` compile definition enables extra logs, namely the commands being interpreted.

## Tests

To run tests, execute the `run_tests.sh` script. The test suite contains all tests available in the main Lama repository
at version 1.30, except `test054, test110, test111, test803` as the bytecode compiler is unable to process them.

The directory structure was changed a bit to simplify testing.

All the tests from the present suite are passing:

```
# ./run_tests.sh
# ...
Total tests: 11031
Passed: 11031
```

### Performance

The `performance` directory contains a single test `Sort.lama`, which was slightly modified compared to the original
test:
it now prints the sorted array.

To run the performance tests, execute the `performance_test.sh` script. It will run `lamac -i`, `lamac -s` and the
bytecode
interpreter against the sort test.

Reference performance results:

```
# ./performance_test.sh
Run lamac -i
Run lamac -s
Run bytecode interpreter
Timings (seconds):
lamac -i: 526.486181000
lamac -s: 159.162314000
lama_interpreter (bytecode): 153.401944000
```

## Frequency Analyzer

Run

```
./lama_analyzer <input>.bc
```

to analyze the frequency of one- and two-bytecode sequences. ([analyzer.cpp](src/analyzer.cpp))

Example output:

```
❯ ./build/lama_analyzer output/regression/regression/test001.bc                                               
Sequence <DROP>:
        3 times
Sequence <LREAD>:
        2 times
Sequence <BINOP 2>:
        2 times
Sequence <ST 0 0>:
        1 times
Sequence <LWRITE, END>:
        1 times
Sequence <LINE 3, LD 0 2>:
        1 times
```

Frequency for the original performance test [Sort_orig.lama](performance/Sort_orig.lama) (the `print` function is not
used compared to the modified [Sort.lama](performance/Sort.lama)) can be found
here: [Sort_orig.stats](performance/Sort_orig.stats)