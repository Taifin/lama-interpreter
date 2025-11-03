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

constexpr int VSTACK_SIZE = 1 << 20;
constexpr int CSTACK_SIZE = 1 << 20;

aint vstack[VSTACK_SIZE];
aint cstack[CSTACK_SIZE];

aint *cstack_top = cstack + CSTACK_SIZE;
aint *cstack_bottom = cstack + CSTACK_SIZE;

#define SP (__gc_stack_top + 1)

inline void verify_vstack(aint *location, const std::string& trace) {
    if (location >= __gc_stack_bottom) {
        failure("Virtual stack underflow! .loc: %.8x, .bot: %.8x, trace: %s", location, __gc_stack_bottom, trace.c_str());
    }
    if (location <= vstack) {
        failure("Virtual stack overflow! .loc: %.8x, .top: %.8x, trace: %s", location, vstack, trace.c_str());
    }
}

inline void verify_cstack(aint *location, const std::string& trace) {
    if (location >= cstack_bottom) {
        failure("Call stack underflow! .loc: %.8x, .bot: %.8x, trace: %s", location, cstack_bottom, trace.c_str());
    }
    if (location <= cstack) {
        failure("Call stack overflow! .loc: %.8x, .top: %.8x, trace: %s", location, cstack, trace.c_str());
    }
}

inline aint vstack_pop() {
    if (__gc_stack_top >= __gc_stack_bottom) {
        failure("Virtual stack underflow!");
    }
    __gc_stack_top += 1;
    return *(SP - 1);
}

inline void vstack_push(aint val) {
    if (vstack >= __gc_stack_top) {
        failure("Virtual stack overflow!");
    }
    __gc_stack_top -= 1;
    *SP = val;
}

inline void init_vstack(const bytefile *bf) {
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

inline void verify_cstack_underflow(int loc = 0, const char *msg = "Call stack underflow!") {
    if (cstack_top + loc >= cstack_bottom) {
        failure(msg);
    }
}

inline void cstack_push(aint val) {
    if (cstack_top <= cstack) {
        failure("Call stack overflow!");
    }
    *--cstack_top = val;
}

inline aint cstack_pop() {
    verify_cstack_underflow();
    return *cstack_top++;
}

inline bool is_closure() {
    verify_cstack_underflow(4, "Invalid call stack: expected closure flag");
    return (bool) *(cstack_top + 4);
}

inline aint ret_addr() {
    verify_cstack_underflow(3, "Invalid call stack: expected return address");
    return *(cstack_top + 3);
}

inline aint *frame_pointer() {
    verify_cstack_underflow(2, "Invalid call stack: expected frame pointer");
    return (aint *) *(cstack_top + 2);
}

inline aint nargs() {
    verify_cstack_underflow(1, "Invalid call stack: expected number of args");
    return *(cstack_top + 1);
}

inline aint nlocals() {
    verify_cstack_underflow(0, "Invalid call stack: expected number of locals");
    return *cstack_top;
}


inline aint *global(bytefile *bf, int ind) {
    if (ind < 0 || ind >= bf->global_area_size) {
        failure("Requested global %d is out of bounds for [0, %d)", ind, bf->global_area_size);
    }

    auto loc = __gc_stack_bottom - bf->global_area_size + ind;
    verify_vstack(loc, ".global");
    return loc;
}

inline aint *arg(int ind) {
    if (ind < 0 || ind >= nargs()) {
        failure("Requested argument %d is out of bounds for [0, %d)", ind, nargs());
    }

    auto loc = frame_pointer() + nargs() - 1 - ind;
    verify_vstack(loc, ".arg");
    return loc;
}

inline aint *local(int ind) {
    auto nlcls = nlocals();
    if (ind < 0 || ind >= nlocals()) {
        failure("Requested local %d is out of bounds for [0, %d)", ind, nlocals());
    }

    auto loc = frame_pointer() - nlcls + ind;
    verify_vstack(loc, ".local");
    return loc;
}

inline aint *closure_loc() {
    if (!is_closure()) {
        failure("Requested closure, but closure is not placed on stack");
    }

    auto loc = frame_pointer() + nargs();
    verify_vstack(loc, ".closure");
    return loc;
}

inline aint* closure(int ind) {
    auto closureLoc = closure_loc();
    auto closureData = TO_DATA(*closureLoc);

    if (TAG(closureData->data_header) != CLOSURE_TAG) {
        failure("Requested closure element %d, but the value on stack is not a closure", ind);
    }

    return &((aint *) closureData->contents)[ind + 1];
}


struct Loc {
    enum class Type {
        G,
        L,
        A,
        C
    };

    Type type;
    int value;
};

inline char readByte(const bytefile *f, char * &ip) {
    if (ip + 1 < f->code_ptr || ip + 1 >= f->code_ptr + f->size) {
        failure("Instruction pointer %.8x out of bounds [%.8x, %.8x)", ip, f->code_ptr, f->code_ptr + f->size);
    }
    return *ip++;
}

inline int readInt(const bytefile *f, char * &ip) {
    ip += sizeof(int);
    if (ip < f->code_ptr || ip >= f->code_ptr + f->size) {
        failure("Instruction pointer %.8x out of bounds [%.8x, %.8x)", ip, f->code_ptr, f->code_ptr + f->size);
    }
    return *reinterpret_cast<int *>(ip - sizeof(int));
}

inline char *readString(const bytefile *f, char * &ip) {
    int pos = readInt(f, ip);
    if (pos < 0 || pos > f->stringtab_size) {
        failure("Requested string %d is out of bounds for [0, %d)", pos, f->stringtab_size);
    }
    return &f->string_ptr[pos];
}

// ReSharper disable once CppNotAllPathsReturnValue
inline Loc readLoc(const bytefile *f, char * &ip, char byte) {
    int val = readInt(f, ip);
    switch (byte) {
        case 0:
            return Loc(Loc::Type::G, val);
        case 1:
            return Loc(Loc::Type::L, val);
        case 2:
            return Loc(Loc::Type::A, val);
        case 3:
            return Loc(Loc::Type::C, val);
        default:
            failure("Unsupported loc type %d", byte);
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

inline void execBinop(const BinOp &op) {
    switch (op) {
        case BinOp::PLUS: BINOP(+)
        case BinOp::MINUS: BINOP(-)
        case BinOp::TIMES: BINOP(*)
        case BinOp::DIV: BINOP(/)
        case BinOp::MOD: BINOP(%)
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

inline void execConst(int cnst) {
    vstack_push(BOX(cnst));
}

inline void execString(char *string) {
    vstack_push((aint) Bstring((aint *) &string));
}

inline void execSexp(char *tag, int nargs) {
    if (nargs < 0) {
        failure("Invalid SEXP op: negative length %d", nargs);
    }

    verify_vstack(SP + nargs, ".sexp");
    vstack_push(LtagHash(tag));

    auto result = (aint) Bsexp(SP, BOX(nargs + 1));

    __gc_stack_top += nargs + 1;
    vstack_push(result);
}

inline void execSti() {
    failure("Unsupported instruction STI");
}

inline void execSta() {
    auto val = vstack_pop();
    auto ind = vstack_pop();
    auto dst = vstack_pop();

    void *result = Bsta((void *) dst, ind, (void *) val);

    vstack_push((aint) result);
}

inline void execSt(bytefile *bf, const Loc &loc) {
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
            *closure(loc.value) =  value;
            break;
        }
    }
    vstack_push(value);
}

inline void execDrop() {
    vstack_pop();
}

inline void execDup() {
    auto val = vstack_pop();
    vstack_push(val);
    vstack_push(val);
}

inline void execSwap() {
    auto x = vstack_pop();
    auto y = vstack_pop();

    vstack_push(y);
    vstack_push(x);
}

inline void execElem() {
    auto ind = vstack_pop();
    auto src = vstack_pop();

    auto res = Belem((void *) src, ind);

    vstack_push((aint) res);
}

inline aint load(bytefile * bf, const Loc &loc) {
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

inline void execLd(bytefile *bf, const Loc &loc) {
    vstack_push(load(bf, loc));
}

inline void execLda(bytefile *, const Loc &) {
    failure("LDA is not supported");
}

inline void update_ip(const bytefile *bf, char * &ip, char *newIp) {
    if (newIp < bf->code_ptr || newIp > bf->code_ptr + bf->size) {
        failure("Cannot move instruction pointer %.8x to new %.8x, is out of bounds for [%.8x, %.8x]", ip, newIp,
                bf->code_ptr, bf->code_ptr + bf->size);
    }

    ip = newIp;
}

inline void execEnd(const bytefile *bf, char * &ip) {
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

    update_ip(bf, ip, (char *) ret_addr());

    verify_cstack(cstack_top + 4, ".end"); // same
    cstack_top += 5;
}

inline void execRet() {
    failure("RET is not supported");
}

inline void execCJmp(const bytefile *bf, char * &ip, aint addr, bool isNz) {
    auto val = UNBOX(vstack_pop());
    aint target{};
    if (isNz) {
        if (val != 0) {
            target = addr;
        } else {
            return;
        }
    } else {
        if (val == 0) {
            target = addr;
        } else {
            return;
        }
    }

    update_ip(bf, ip, bf->code_ptr + target);
}

inline void execBegin(int n_args, int n_locals) {
    cstack_push((aint) SP);
    cstack_push(n_args);
    cstack_push(n_locals);
    for (int i = 0; i < n_locals; i++) {
        vstack_push(BOX(0));
    }
}

inline void execTag(char *tag, int len) {
    auto dest = vstack_pop();
    vstack_push(Btag((void *) dest, LtagHash(tag), BOX(len)));
}

inline void execArray(int n) {
    auto dest = vstack_pop();
    vstack_push(Barray_patt((void *) dest, BOX(n)));
}

inline void execFail(int l, int c) {
    failure("Failed at %d %d", l, c);
}

inline void execLine(int) {
}

inline void execPatt(int patt) {
    std::string pats[] = {"=str", "#string", "#array", "#sexp", "#ref", "#val", "#fun"};
    auto x = (void *) vstack_pop();
    switch (patt) {
        case 0: {
            auto y = (void *) vstack_pop();
            vstack_push(Bstring_patt(x, y));
            break;
        }
        case 1: {
            vstack_push(Bstring_tag_patt(x));
            break;
        }
        case 2: {
            vstack_push(Barray_tag_patt(x));
            break;
        }
        case 3: {
            vstack_push(Bsexp_tag_patt(x));
            break;
        }
        case 4: {
            vstack_push(Bboxed_patt(x));
            break;
        }
        case 5: {
            vstack_push(Bunboxed_patt(x));
            break;
        }
        case 6: {
            vstack_push(Bclosure_tag_patt(x));
            break;
        }
        default:
            failure("Unexpected pattern %s", patt);
    }
}

inline void execLread() {
    vstack_push(Lread());
}

inline void execLwrite() {
    auto x = vstack_pop();
    vstack_push(Lwrite(x));
}

inline void execLlength() {
    auto x = vstack_pop();
    vstack_push(Llength((void *) x));
}

inline void execLstring() {
    vstack_push((aint) Lstring(SP));
}

inline void execBarray(int n) {
    aint args[n];
    for (auto i = 0; i < n; i++) {
        args[n - 1 - i] = vstack_pop();
    }
    vstack_push((aint) Barray(args, BOX(n)));
}

inline void execClosure(int nargs, aint* args) {
    vstack_push((aint) Bclosure(args, BOX(nargs)));
}

inline void execCall(const bytefile *bf, char * &ip, size_t addr, int nargs) {
    verify_vstack(SP + nargs, ".call");

    cstack_push(false); // not a closure
    cstack_push((aint) ip);

    update_ip(bf, ip, bf->code_ptr + addr);
}

inline void execCallC(const bytefile *bf, char * &ip, int nargs) {
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
    cstack_push((aint) ip);

    update_ip(bf, ip, bf->code_ptr + target);
}

void interpret(bytefile *bf) {
    char *ip = bf->entrypoint_ptr;
    void (*lds[3])(bytefile *, const Loc &) = {execLd, execLda, execSt};
    do {
        char opcode = readByte(bf, ip),
                h = (opcode & 0xF0) >> 4, // NOLINT(cppcoreguidelines-narrowing-conversions)
                l = opcode & 0x0F; // NOLINT(cppcoreguidelines-narrowing-conversions)

        DEBUG("0x%.8lx:\t", ip - bf->code_ptr - 1);

        switch (h) {
            case 15:
                goto stop;

            case 0:
                DEBUG("BINOP\t%d", l);
                execBinop(static_cast<BinOp>(l - 1));
                break;

            case 1:
                switch (l) {
                    case 0: {
                        auto cnst = readInt(bf, ip);
                        DEBUG("CONST\t%d", cnst);
                        execConst(cnst);
                        break;
                    }

                    case 1: {
                        auto str = readString(bf, ip);
                        DEBUG("STRING\t%s", str);
                        execString(str);
                        break;
                    }

                    case 2: {
                        auto str = readString(bf, ip);
                        auto i = readInt(bf, ip);
                        DEBUG("SEXP\t%s ", str);
                        DEBUG("%d", i);
                        execSexp(str, i);
                        break;
                    }

                    case 3: {
                        DEBUG("STI%s", "");
                        execSti();
                        break;
                    }

                    case 4: {
                        DEBUG("STA%s", "");
                        execSta();
                        break;
                    }

                    case 5: {
                        auto addr = readInt(bf, ip);
                        DEBUG("JMP\t0x%.8x", addr);
                        ip = bf->code_ptr + addr;
                        break;
                    }

                    case 6: {
                        DEBUG("END%s", "");
                        execEnd(bf, ip);
                        if (ip == bf->code_ptr + bf->size) return;
                        break;
                    }

                    case 7: {
                        DEBUG("RET%s" ,"");
                        execRet();
                        break;
                    }

                    case 8: {
                        DEBUG("DROP%s", "");
                        execDrop();
                        break;
                    }

                    case 9: {
                        DEBUG("DUP%s", "");
                        execDup();
                        break;
                    }

                    case 10: {
                        DEBUG("SWAP%s", "");
                        execSwap();
                        break;
                    }

                    case 11: {
                        DEBUG("ELEM%s", "");
                        execElem();
                        break;
                    }

                    default:
                        failure("unexpected opcode %d %d", h, l);
                }
                break;

            case 2:
            case 3:
            case 4: {
                DEBUG("lds %d\t", h - 2);
                auto loc = readLoc(bf, ip, l);
                DEBUG("loc %d val %d", loc.type, loc.value);
                lds[h - 2](bf, loc);

                break;
            }

            case 5:
                switch (l) {
                    case 0:
                    case 1: {
                        auto i = readInt(bf, ip);
                        DEBUG("CJMP%d\t0x%.8x", l, i);
                        execCJmp(bf, ip, i, l == 1); // 0 - z, 1 -- nz
                        break;
                    }

                    case 3:
                    case 2: {
                        auto nargs = readInt(bf, ip);
                        auto nlocals = readInt(bf, ip);
                        DEBUG("BEGIN\t%d ", nargs);
                        DEBUG("%d", nlocals);
                        execBegin(nargs, nlocals);
                        break;
                    }

                    case 4: {
                        auto addr = readInt(bf, ip);
                        auto nLocs = readInt(bf, ip);
                        DEBUG("CLOSURE\t0x%.8x\t%d", addr, nLocs);
                        aint args[nLocs + 1];
                        for (int i = 0; i < nLocs; i++) {
                            char locType = readByte(bf, ip);
                            auto loc = readLoc(bf, ip, locType);
                            args[i + 1] = load(bf, loc);
                        }
                        args[0] = addr;
                        execClosure(nLocs, args);
                        break;
                    }

                    case 5: {
                        auto nargs = readInt(bf, ip);
                        DEBUG("CALLC\t%d", nargs);
                        execCallC(bf, ip, nargs);
                        break;
                    }

                    case 6: {
                        auto addr = readInt(bf, ip);
                        auto nargs = readInt(bf, ip);
                        DEBUG("CALL\t0x%.8x ", addr);
                        DEBUG("%d", nargs);
                        execCall(bf, ip, addr, nargs);
                        break;
                    }

                    case 7: {
                        auto tag = readString(bf, ip);
                        auto len = readInt(bf, ip);
                        DEBUG("TAG\t%s ", tag);
                        DEBUG("%d", len);
                        execTag(tag, len);
                        break;
                    }

                    case 8: {
                        auto i = readInt(bf, ip);
                        DEBUG("ARRAY\t%d", i);
                        execArray(i);
                        break;
                    }

                    case 9: {
                        auto ln = readInt(bf, ip);
                        auto cl = readInt(bf, ip);
                        DEBUG("FAIL\t%d", ln);
                        DEBUG("%d", cl);
                        execFail(ln, cl);
                        break;
                    }

                    case 10: {
                        auto i = readInt(bf, ip);
                        DEBUG("LINE\t%d", i);
                        execLine(i);
                        break;
                    }

                    default:
                        failure("unexpected opcode 5");
                }
                break;

            case 6: {
                DEBUG("PATT\t%d", l);
                execPatt(l);
                break;
            }

            case 7: {
                switch (l) {
                    case 0: {
                        DEBUG("CALL\t%s", "Lread");
                        execLread();
                        break;
                    }

                    case 1: {
                        DEBUG("CALL\t%s", "Lwrite");
                        execLwrite();
                        break;
                    }

                    case 2: {
                        DEBUG("CALL\t%s", "Llength");
                        execLlength();
                        break;
                    }

                    case 3: {
                        DEBUG("CALL\t%s", "Lstring");
                        execLstring();
                        break;
                    }

                    case 4: {
                        auto n = readInt(bf, ip);
                        DEBUG("CALL\tBarray\t%d", n);
                        execBarray(n);
                        break;
                    }

                    default:
                        failure("unexpected opcode 7");
                }
            }
            break;

            default:
                failure("unexpected opcode default");
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
    cstack_push((aint) (bf->code_ptr + bf->size));

    interpret(bf);

    free(bf);
}
