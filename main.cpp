#include <fstream>
#include <iostream>
#include <string>

#include "bytecode/bytefile.h"
#include "runtime/gc.h"
#include "runtime/runtime.h"
#include "runtime/runtime_common.h"

extern aint *__gc_stack_top, *__gc_stack_bottom; // NOLINT(*-reserved-identifier)

FILE *debugFile = stderr;
#ifdef DEBUG_OUT
#define DEBUG(fmt, ...) \
    do { fprintf(debugFile, fmt, __VA_ARGS__); } while(0);
#else
#define DEBUG(fmt, ...) \
;
#endif

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

enum class Patts {
    STR = 0,
    STR_TAG = 1,
    ARRAY = 2,
    SEXP = 3,
    BOXED = 4,
    UNBOXED = 5,
    CLOSURE = 6
};

constexpr static int VSTACK_SIZE = 1 << 20;
constexpr static int CSTACK_SIZE = 1 << 20;

static aint vstack[VSTACK_SIZE];
static aint cstack[CSTACK_SIZE];

static aint *cstack_top = cstack + CSTACK_SIZE;
static aint *cstack_bottom = cstack + CSTACK_SIZE;

#define SP (__gc_stack_top + 1)

struct State {
    bytefile *bf = nullptr;
    char *ip = nullptr;
    unsigned char opcode = -1;

    void fail(const char *s, ...) const {
        va_list args;
        va_start(args, s);

        if (bf != nullptr && ip != nullptr) {
            fprintf(
                stderr, "Failure.\n\tinstruction offset: 0x%.8lx\n\topcode: %d\n\tvstack: 0x%.8lx\n\tcstack: 0x%.8lx\n",
                ip - bf->code_ptr - 1, opcode, __gc_stack_top, cstack_top);
        }

        fprintf(stderr, "*** FAILURE: ");
        vfprintf(stderr, s, args);
        exit(255);
    }
};

State state;

static inline void verify_vstack(aint *location, const std::string &trace) {
    if (location >= __gc_stack_bottom) {
        state.fail("Virtual stack underflow! .loc: %.8x, .bot: %.8x, trace: %s", location, __gc_stack_bottom,
                   trace.c_str());
    }
    if (location <= vstack) {
        state.fail("Virtual stack overflow! .loc: %.8x, .top: %.8x, trace: %s", location, vstack, trace.c_str());
    }
}

static inline void verify_cstack(aint *location, const std::string &trace) {
    if (location >= cstack_bottom) {
        state.fail("Call stack underflow! .loc: %.8x, .bot: %.8x, trace: %s", location, cstack_bottom, trace.c_str());
    }
    if (location <= cstack) {
        state.fail("Call stack overflow! .loc: %.8x, .top: %.8x, trace: %s", location, cstack, trace.c_str());
    }
}

static inline aint vstack_pop() {
    if (__gc_stack_top >= __gc_stack_bottom) {
        state.fail("Virtual stack underflow!");
    }
    __gc_stack_top += 1;
    return *(SP - 1);
}

static inline void vstack_push(aint val) {
    if (vstack >= __gc_stack_top) {
        state.fail("Virtual stack overflow!");
    }
    __gc_stack_top -= 1;
    *SP = val;
}

static inline void init_vstack(const bytefile *bf) {
    DEBUG("Init vstack %s\n", "")
    __gc_stack_bottom = vstack + VSTACK_SIZE;
    __gc_stack_top = __gc_stack_bottom;

    DEBUG("Allocate %d globals\n", bf->global_area_size)
    for (auto i = 0; i < bf->global_area_size; i++) {
        vstack_push(bf->global_ptr[bf->global_area_size - i - 1]);
    }
    vstack_push(0);
    vstack_push(0); // argc argv
}

static inline void verify_cstack_underflow(int loc = 0, const char *msg = "Call stack underflow!") {
    if (cstack_top + loc >= cstack_bottom) {
        state.fail(msg);
    }
}

static inline void cstack_push(aint val) {
    if (cstack_top <= cstack) {
        state.fail("Call stack overflow!");
    }
    *--cstack_top = val;
}

static inline aint cstack_pop() {
    verify_cstack_underflow();
    return *cstack_top++;
}

static inline bool is_closure() {
    verify_cstack_underflow(4, "Invalid call stack: expected closure flag");
    return (bool) *(cstack_top + 4);
}

static inline aint ret_addr() {
    verify_cstack_underflow(3, "Invalid call stack: expected return address");
    return *(cstack_top + 3);
}

static inline aint *frame_pointer() {
    verify_cstack_underflow(2, "Invalid call stack: expected frame pointer");
    return (aint *) *(cstack_top + 2);
}

static inline aint nargs() {
    verify_cstack_underflow(1, "Invalid call stack: expected number of args");
    return *(cstack_top + 1);
}

static inline aint nlocals() {
    verify_cstack_underflow(0, "Invalid call stack: expected number of locals");
    return *cstack_top;
}


static inline aint *global(bytefile *bf, int ind) {
    if (ind < 0 || ind >= bf->global_area_size) {
        state.fail("Requested global %d is out of bounds for [0, %d)", ind, bf->global_area_size);
    }

    auto loc = __gc_stack_bottom - bf->global_area_size + ind;
    verify_vstack(loc, ".global");
    return loc;
}

static inline aint *arg(int ind) {
    if (ind < 0 || ind >= nargs()) {
        state.fail("Requested argument %d is out of bounds for [0, %d)", ind, nargs());
    }

    auto loc = frame_pointer() + nargs() - 1 - ind;
    verify_vstack(loc, ".arg");
    return loc;
}

static inline aint *local(int ind) {
    auto nlcls = nlocals();
    if (ind < 0 || ind >= nlocals()) {
        state.fail("Requested local %d is out of bounds for [0, %d)", ind, nlocals());
    }

    auto loc = frame_pointer() - nlcls + ind;
    verify_vstack(loc, ".local");
    return loc;
}

static inline aint *closure_loc() {
    if (!is_closure()) {
        state.fail("Requested closure, but closure is not placed on stack");
    }

    auto loc = frame_pointer() + nargs();
    verify_vstack(loc, ".closure");
    return loc;
}

static inline aint *closure(int ind) {
    auto closureLoc = closure_loc();
    auto closureData = TO_DATA(*closureLoc);

    if (TAG(closureData->data_header) != CLOSURE_TAG) {
        state.fail("Requested closure element %d, but the value on stack is not a closure", ind);
    }

    return &((aint *) closureData->contents)[ind + 1];
}


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

static inline char readByte(const bytefile *f, char * &ip) {
    if (ip < f->code_ptr || ip + 1 > f->code_ptr + f->code_size) {
        state.fail("Instruction pointer %.8x out of bounds [%.8x, %.8x)", ip, f->code_ptr, f->code_ptr + f->code_size);
    }
    return *ip++;
}

static inline int readInt(const bytefile *f, char * &ip) {
    if (ip < f->code_ptr || ip + sizeof(int) > f->code_ptr + f->code_size) {
        state.fail("Instruction pointer %.8x out of bounds [%.8x, %.8x)", ip, f->code_ptr, f->code_ptr + f->code_size);
    }
    ip += sizeof(int);
    return *reinterpret_cast<int *>(ip - sizeof(int));
}

static inline char *readString(const bytefile *f, char * &ip) {
    int pos = readInt(f, ip);
    if (pos < 0 || pos > f->stringtab_size) {
        state.fail("Requested string %d is out of bounds for [0, %d)", pos, f->stringtab_size);
    }
    return &f->string_ptr[pos];
}

// ReSharper disable once CppNotAllPathsReturnValue
static inline Loc readLoc(const bytefile *f, char * &ip, unsigned char byte) {
    int val = readInt(f, ip);
    switch (auto locType = static_cast<Loc::Type>(byte)) {
        case Loc::Type::G:
        case Loc::Type::L:
        case Loc::Type::A:
        case Loc::Type::C:
            return Loc(locType, val);
        default:
            state.fail("Unsupported loc type %d", byte);
    }
}

#define BINOP(op)             \
    {                                   \
        auto rhs = UNBOX(vstack_pop()); \
        auto lhs = UNBOX(vstack_pop()); \
        auto v = lhs op rhs;            \
        vstack_push(BOX(v));            \
        break;                          \
    }

#define BINOP_DIV(op)                                                                           \
    {                                                                                           \
        auto rhs = UNBOX(vstack_pop());                                                         \
        auto lhs = UNBOX(vstack_pop());                                                         \
        if (rhs == 0) {                                                                         \
            state.fail("Attempt to divide %d by zero when executing operation %s", lhs, #op);   \
        }                                                                                       \
        auto v = lhs op rhs;                                                                    \
        vstack_push(BOX(v));                                                                    \
        break;                                                                                  \
    }

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

static inline void execBinop(const BinOp &op) {
    switch (op) {
        case BinOp::PLUS: BINOP(+)
        case BinOp::MINUS: BINOP(-)
        case BinOp::TIMES: BINOP(*)
        case BinOp::DIV: BINOP_DIV(/)
        case BinOp::MOD: BINOP_DIV(%)
        case BinOp::LT: BINOP(<)
        case BinOp::LTQ: BINOP(<=)
        case BinOp::GT: BINOP(>)
        case BinOp::GTQ: BINOP(>=)
        case BinOp::EQ: BINOP(==)
        case BinOp::NEQ: BINOP(!=)
        case BinOp::AND: BINOP(&&)
        case BinOp::OR: BINOP(||)
    }
}

static inline void execConst(int cnst) {
    vstack_push(BOX(cnst));
}

static inline void execString(char *string) {
    vstack_push((aint) Bstring((aint *) &string));
}

static inline void execSexp(char *tag, int nargs) {
    if (nargs < 0) {
        state.fail("Invalid SEXP op: negative length %d", nargs);
    }

    verify_vstack(SP + nargs, ".sexp");
    vstack_push(LtagHash(tag));

    auto result = (aint) Bsexp(SP, BOX(nargs + 1));

    __gc_stack_top += nargs + 1;
    vstack_push(result);
}

static inline void execSti() {
    state.fail("Unsupported instruction STI");
}

static inline void execSta() {
    auto val = vstack_pop();
    auto ind = vstack_pop();
    auto dst = vstack_pop();

    void *result = Bsta((void *) dst, ind, (void *) val);

    vstack_push((aint) result);
}

static inline void execSt(bytefile *bf, const Loc &loc) {
    auto value = vstack_pop();
    switch (loc.type) {
        case Loc::Type::G: {
            *global(bf, loc.value) = value;
            break;
        }
        case Loc::Type::L: {
            *local(loc.value) = value;
            break;
        }
        case Loc::Type::A: {
            *arg(loc.value) = value;
            break;
        }
        case Loc::Type::C: {
            *closure(loc.value) = value;
            break;
        }
    }
    vstack_push(value);
}

static inline void execDrop() {
    vstack_pop();
}

static inline void execDup() {
    auto val = vstack_pop();
    vstack_push(val);
    vstack_push(val);
}

static inline void execSwap() {
    auto x = vstack_pop();
    auto y = vstack_pop();

    vstack_push(y);
    vstack_push(x);
}

static inline void execElem() {
    auto ind = vstack_pop();
    auto src = vstack_pop();

    auto res = Belem((void *) src, ind);

    vstack_push((aint) res);
}

static inline aint load(bytefile *bf, const Loc &loc) {
    aint value{};
    switch (loc.type) {
        case Loc::Type::G: {
            value = *global(bf, loc.value);
            break;
        }
        case Loc::Type::L: {
            value = *local(loc.value);
            break;
        }
        case Loc::Type::A: {
            value = *arg(loc.value);
            break;
        }
        case Loc::Type::C: {
            value = *closure(loc.value);
            break;
        }
    }
    return value;
}

static inline void execLd(bytefile *bf, const Loc &loc) {
    vstack_push(load(bf, loc));
}

static inline void execLda(bytefile *, const Loc &) {
    state.fail("LDA is not supported");
}

static inline void update_ip(const bytefile *bf, char * &ip, aint offset) {
    if (offset < 0 || offset > bf->code_size) {
        state.fail("Cannot move instruction pointer %.8x by offset %d, is out of bounds for [%.8x, %.8x] (%d)", ip, offset,
                   bf->code_ptr, bf->code_ptr + bf->code_size, bf->code_size);
    }

    ip = bf->code_ptr + offset;
}

static inline void execEnd(const bytefile *bf, char * &ip) {
    aint retval = 0;
    bool isRetval = false;
    if (SP < frame_pointer() + nlocals()) {
        retval = vstack_pop();
        isRetval = true;
    }

    auto loc = frame_pointer() + nargs() + static_cast<int>(is_closure()) - 1;
    verify_vstack(loc - 1, ".end"); // it's ok to have an empty vstack after end
    __gc_stack_top = loc;

    if (isRetval) {
        vstack_push(retval);
    }

    update_ip(bf, ip, ret_addr());

    verify_cstack(cstack_top + 4, ".end"); // same
    cstack_top += 5;
}

static inline void execRet() {
    state.fail("RET is not supported");
}

static inline void execCJmp(const bytefile *bf, char * &ip, aint addr, bool isNz) {
    if (auto val = UNBOX(vstack_pop()); isNz != !val) {
        update_ip(bf, ip, addr);
    }
}

static inline void execBegin(int n_args, int n_locals) {
    cstack_push((aint) SP);
    cstack_push(n_args);
    cstack_push(n_locals);
    for (int i = 0; i < n_locals; i++) {
        vstack_push(BOX(0));
    }
}

static inline void execTag(char *tag, int len) {
    auto dest = vstack_pop();
    vstack_push(Btag((void *) dest, LtagHash(tag), BOX(len)));
}

static inline void execArray(int n) {
    auto dest = vstack_pop();
    vstack_push(Barray_patt((void *) dest, BOX(n)));
}

static inline void execFail(int l, int c) {
    state.fail("Failed at %d %d", l, c);
}

static inline void execLine(int) {
}

static inline void execPatt(int patt) {
    auto x = (void *) vstack_pop();
    switch (static_cast<Patts>(patt)) {
        case Patts::STR: {
            auto y = (void *) vstack_pop();
            vstack_push(Bstring_patt(x, y));
            break;
        }
        case Patts::STR_TAG: {
            vstack_push(Bstring_tag_patt(x));
            break;
        }
        case Patts::ARRAY: {
            vstack_push(Barray_tag_patt(x));
            break;
        }
        case Patts::SEXP: {
            vstack_push(Bsexp_tag_patt(x));
            break;
        }
        case Patts::BOXED: {
            vstack_push(Bboxed_patt(x));
            break;
        }
        case Patts::UNBOXED: {
            vstack_push(Bunboxed_patt(x));
            break;
        }
        case Patts::CLOSURE: {
            vstack_push(Bclosure_tag_patt(x));
            break;
        }
        default:
            state.fail("Unexpected pattern %s", patt);
    }
}

static inline void execLread() {
    vstack_push(Lread());
}

static inline void execLwrite() {
    auto x = vstack_pop();
    vstack_push(Lwrite(x));
}

static inline void execLlength() {
    auto x = vstack_pop();
    vstack_push(Llength((void *) x));
}

static inline void execLstring() {
    vstack_push((aint) Lstring(SP));
}

static inline void execBarray(int n) {
    verify_vstack(SP + n, ".barray");
    auto arrayPtr = (aint) Barray(SP, BOX(n));
    __gc_stack_top += n;
    vstack_push(arrayPtr);
}

static inline void execClosure(int nargs, int addr) {
    vstack_push(addr);
    auto *closurePtr = Bclosure(SP, BOX(nargs));
    __gc_stack_top += nargs + 1;
    vstack_push((aint)closurePtr);
}

static inline void execCall(const bytefile *bf, char * &ip, size_t addr, int nargs) {
    verify_vstack(SP + nargs, ".call");

    cstack_push(false); // not a closure
    cstack_push(ip - bf->code_ptr);

    update_ip(bf, ip, (aint)addr);
}

static inline void execCallC(const bytefile *bf, char * &ip, int nargs) {
    /*  stack frame is not yet complete:
     *  bottom
     *  ...
     *  *closure -> target, capture[0], capture[1], ...
     *  arg[n]
     *  ...
     *  arg[0] = sp
     */
    verify_vstack(SP + nargs, ".callC");

    auto closureLoc = SP + nargs;
    verify_vstack(closureLoc, ".callC");

    auto target = ((aint *) *closureLoc)[0];
    cstack_push(true); // closure
    cstack_push(ip - bf->code_ptr);

    update_ip(bf, ip, target);
}

static inline void interpret(bytefile *bf) {
    state = {bf, bf->entrypoint_ptr};
    do {
        unsigned char opcode = readByte(bf, state.ip),
                h = (opcode & 0xF0) >> 4, // NOLINT(cppcoreguidelines-narrowing-conversions)
                l = opcode & 0x0F; // NOLINT(cppcoreguidelines-narrowing-conversions)
        state.opcode = opcode;

        DEBUG("0x%.8lx:\t", state.ip - bf->code_ptr - 1);

        auto hi = static_cast<Instruction>(h), li = static_cast<Instruction>(l);

        switch (hi) {
            case Instruction::STOP:
                goto stop;

            case Instruction::BINOP:
                DEBUG("BINOP\t%d", l);
                execBinop(static_cast<BinOp>(l - 1));
                break;

            case Instruction::CONST_H:
                switch (li) {
                    case Instruction::CONST: {
                        auto cnst = readInt(bf, state.ip);
                        DEBUG("CONST\t%d", cnst);
                        execConst(cnst);
                        break;
                    }

                    case Instruction::STRING: {
                        auto str = readString(bf, state.ip);
                        DEBUG("STRING\t%s", str);
                        execString(str);
                        break;
                    }

                    case Instruction::SEXP: {
                        auto str = readString(bf, state.ip);
                        auto i = readInt(bf, state.ip);
                        DEBUG("SEXP\t%s ", str);
                        DEBUG("%d", i);
                        execSexp(str, i);
                        break;
                    }

                    case Instruction::STI: {
                        DEBUG("STI%s", "");
                        execSti();
                        break;
                    }

                    case Instruction::STA: {
                        DEBUG("STA%s", "");
                        execSta();
                        break;
                    }

                    case Instruction::JMP: {
                        auto addr = readInt(bf, state.ip);
                        DEBUG("JMP\t0x%.8x", addr);
                        state.ip = bf->code_ptr + addr;
                        break;
                    }

                    case Instruction::END: {
                        DEBUG("END%s", "");
                        execEnd(bf, state.ip);
                        if (state.ip == bf->code_ptr + bf->code_size) return;
                        break;
                    }

                    case Instruction::RET: {
                        DEBUG("RET%s", "");
                        execRet();
                        break;
                    }

                    case Instruction::DROP: {
                        DEBUG("DROP%s", "");
                        execDrop();
                        break;
                    }

                    case Instruction::DUP: {
                        DEBUG("DUP%s", "");
                        execDup();
                        break;
                    }

                    case Instruction::SWAP: {
                        DEBUG("SWAP%s", "");
                        execSwap();
                        break;
                    }

                    case Instruction::ELEM: {
                        DEBUG("ELEM%s", "");
                        execElem();
                        break;
                    }

                    default:
                        state.fail("unexpected opcode %d", opcode);
                }
                break;

            case Instruction::LD: {
                auto loc = readLoc(bf, state.ip, l);
                DEBUG("LD loc %d val %d", loc.type, loc.value);
                execLd(bf, loc);
                break;
            }
            case Instruction::LDA: {
                auto loc = readLoc(bf, state.ip, l);
                DEBUG("LDA loc %d val %d", loc.type, loc.value);
                execLda(bf, loc);
                break;
            }
            case Instruction::ST: {
                auto loc = readLoc(bf, state.ip, l);
                DEBUG("ST loc %d val %d", loc.type, loc.value);
                execSt(bf, loc);
                break;
            }

            case Instruction::CJMP_H:
                switch (li) {
                    case Instruction::CJMPZ:
                    case Instruction::CJMPNZ: {
                        auto i = readInt(bf, state.ip);
                        DEBUG("CJMP%d\t0x%.8x", l, i);
                        execCJmp(bf, state.ip, i, l == 1); // 0 - z, 1 -- nz
                        break;
                    }

                    case Instruction::BEGIN:
                    case Instruction::CBEGIN: {
                        auto nargs = readInt(bf, state.ip);
                        auto nlocals = readInt(bf, state.ip);
                        DEBUG("BEGIN\t%d ", nargs);
                        DEBUG("%d", nlocals);
                        execBegin(nargs, nlocals);
                        break;
                    }

                    case Instruction::CLOSURE: {
                        auto addr = readInt(bf, state.ip);
                        auto nLocs = readInt(bf, state.ip);
                        DEBUG("CLOSURE\t0x%.8x\t%d", addr, nLocs);
                        for (int i = 0; i < nLocs; i++) {
                            char locType = readByte(bf, state.ip);
                            auto loc = readLoc(bf, state.ip, locType);
                            vstack_push(load(bf, loc));
                        }
                        execClosure(nLocs, addr);
                        break;
                    }

                    case Instruction::CALLC: {
                        auto nargs = readInt(bf, state.ip);
                        DEBUG("CALLC\t%d", nargs);
                        execCallC(bf, state.ip, nargs);
                        break;
                    }

                    case Instruction::CALL: {
                        auto addr = readInt(bf, state.ip);
                        auto nargs = readInt(bf, state.ip);
                        DEBUG("CALL\t0x%.8x ", addr);
                        DEBUG("%d", nargs);
                        execCall(bf, state.ip, addr, nargs);
                        break;
                    }

                    case Instruction::TAG: {
                        auto tag = readString(bf, state.ip);
                        auto len = readInt(bf, state.ip);
                        DEBUG("TAG\t%s ", tag);
                        DEBUG("%d", len);
                        execTag(tag, len);
                        break;
                    }

                    case Instruction::ARRAY: {
                        auto i = readInt(bf, state.ip);
                        DEBUG("ARRAY\t%d", i);
                        execArray(i);
                        break;
                    }

                    case Instruction::FAIL: {
                        auto ln = readInt(bf, state.ip);
                        auto cl = readInt(bf, state.ip);
                        DEBUG("FAIL\t%d", ln);
                        DEBUG("%d", cl);
                        execFail(ln, cl);
                        break;
                    }

                    case Instruction::LINE: {
                        auto i = readInt(bf, state.ip);
                        DEBUG("LINE\t%d", i);
                        execLine(i);
                        break;
                    }

                    default:
                        state.fail("unexpected opcode %d", opcode);
                }
                break;

            case Instruction::PATT_H: {
                DEBUG("PATT\t%d", l);
                execPatt(l);
                break;
            }

            case Instruction::CALL_BUILTIN: {
                switch (li) {
                    case Instruction::LREAD: {
                        DEBUG("CALL\t%s", "Lread");
                        execLread();
                        break;
                    }

                    case Instruction::LWRITE: {
                        DEBUG("CALL\t%s", "Lwrite");
                        execLwrite();
                        break;
                    }

                    case Instruction::LLENGTH: {
                        DEBUG("CALL\t%s", "Llength");
                        execLlength();
                        break;
                    }

                    case Instruction::LSTRING: {
                        DEBUG("CALL\t%s", "Lstring");
                        execLstring();
                        break;
                    }

                    case Instruction::BARRAY: {
                        auto n = readInt(bf, state.ip);
                        DEBUG("CALL\tBarray\t%d", n);
                        execBarray(n);
                        break;
                    }

                    default:
                        state.fail("unexpected opcode %d", opcode);
                }
            }
            break;

            default:
                state.fail("unexpected opcode %d", opcode);
        }
        DEBUG("\n%s", "")
    } while (true);
stop:
    DEBUG("<end>%s\n", "");
}

int main(const int argc, char **argv) {
    if (argc != 2) {
        std::cout << "Usage: ./lama-interpreter <bytecode-file>\n";
        return 1;
    }

    bytefile *bf = readFile(argv[1]);
    __gc_init();
    init_vstack(bf);
    cstack_push(false);
    cstack_push((aint) (bf->code_size));

    interpret(bf);

    free(bf);
}
