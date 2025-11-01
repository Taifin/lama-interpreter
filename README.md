# Lama-interpreter

Bytecode-based iterative interpreter for the Lama language. To build:
```
mkdir build && cd build && cmake .. && make 
```

To run:
```
./lama-interpreter <input>.bc
```

The `DEBUG` compile definition enables extra logs, namely the commands being interpreted. 

## Tests 

To run tests, execute the `run-tests.sh` script. The test suite contains all tests available in the main Lama repository 
at version 1.30, except `test054, test110, test111, test803` as the bytecode compiler is unable to process them. 

The directory structure was changed a bit to simplify testing. 

### Performance

The `performance` directory contains a single test `Sort.lama`, which was slightly modified compared to the original test: 
it now prints the sorted array.

To run the performance tests, execute the `performance_test.sh` script. It will run `lamac -i`, `lamac -s` and the bytecode
interpreter against the sort test. 

