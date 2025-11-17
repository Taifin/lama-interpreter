#ifndef VIRTUAL_MACHINES_COMMON_H
#define VIRTUAL_MACHINES_COMMON_H

#include <cstdio>

struct bytefile;
inline FILE *debugFile = stderr;
#ifdef DEBUG_OUT
#define DEBUG(fmt, ...) \
do { fprintf(debugFile, fmt, __VA_ARGS__); } while(0);
#else
#define DEBUG(fmt, ...) \
;
#endif


struct Loc {
    enum class Type {
        G = 0,
        L = 1,
        A = 2,
        C = 3
    };

    Type type;
    int value;
};

enum class BinOp {
    PLUS,
    MINUS,
    TIMES,
    DIV,
    MOD,
    LT,
    LTQ,
    GT,
    GTQ,
    EQ,
    NEQ,
    AND,
    OR
};

enum class Patts {
    STR = 0,
    STR_TAG = 1,
    ARRAY = 2,
    SEXP = 3,
    BOXED = 4,
    UNBOXED = 5,
    CLOSURE = 6
};

enum class Instruction {
    BINOP = 0,
    CONST_H = 1,
    LD = 2,
    LDA = 3,
    ST = 4,
    CJMP_H = 5,
    PATT_H = 6,
    CALL_BUILTIN = 7,
    STOP = 15,

    CONST = 0,
    STRING = 1,
    SEXP = 2,
    STI = 3,
    STA = 4,
    JMP = 5,
    END = 6,
    RET = 7,
    DROP = 8,
    DUP = 9,
    SWAP = 10,
    ELEM = 11,

    CJMPZ = 0,
    CJMPNZ = 1,
    BEGIN = 2,
    CBEGIN = 3,
    CLOSURE = 4,
    CALLC = 5,
    CALL = 6,
    TAG = 7,
    ARRAY = 8,
    FAIL = 9,
    LINE = 10,

    LREAD = 0,
    LWRITE = 1,
    LLENGTH = 2,
    LSTRING = 3,
    BARRAY = 4,

};

#endif //VIRTUAL_MACHINES_COMMON_H