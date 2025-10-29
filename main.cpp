#include <cassert>
#include <fstream>
#include <iostream>
#include <map>
#include <string>

#include "bytecode/bytefile.h"
#include "runtime/gc.h"
#include "runtime/runtime.h"
#include "runtime/runtime_common.h"

#define FAIL failure("ERROR\n");


extern aint *__gc_stack_top, *__gc_stack_bottom;

constexpr int VSTACK_SIZE = 1 << 20;
constexpr int CSTACK_SIZE = 1 << 20;

aint vstack[VSTACK_SIZE];
aint cstack[CSTACK_SIZE];

aint* cstack_top = cstack + CSTACK_SIZE;
aint* cstack_bottom = cstack + CSTACK_SIZE;


constexpr aint END = INT_MIN;

#define SP (__gc_stack_top + 1)

inline aint vstack_pop() {
    assert(__gc_stack_top < __gc_stack_bottom);
    __gc_stack_top += 1;
    return *(SP - 1);
}

inline void vstack_push(aint val) {
    assert(vstack < __gc_stack_top);
    __gc_stack_top -= 1;
    *SP = val;
}

inline void init_vstack(bytefile* bf) {
    __gc_stack_bottom = vstack + VSTACK_SIZE;
    __gc_stack_top = __gc_stack_bottom;
    for (auto i = 0; i < bf->global_area_size; i++) {
        vstack_push(bf->global_ptr[bf->global_area_size - i - 1]);
    }
    vstack_push(0);
    vstack_push(0); // argc argv
}

inline void cstack_push(aint val) {
    assert(cstack < cstack_top);
    *--cstack_top = val;
}

inline aint cstack_pop() {
    assert(cstack_top < cstack_bottom);
    return *cstack_top++;
}

inline bool is_closure() {
    assert(cstack_top + 4 < cstack_bottom);
    return (bool)*(cstack_top + 4);
}

inline aint ret_addr() {
    assert(cstack_top + 3 < cstack_bottom);
    return *(cstack_top + 3);
}

inline aint* frame_pointer() {
    assert(cstack_top + 2 < cstack_bottom);
    return (aint*)*(cstack_top + 2);
}

inline aint nargs() {
    assert(cstack_top + 1 < cstack_bottom);
    return *(cstack_top + 1);
}

inline aint nlocals() {
    assert(cstack_top < cstack_bottom);
    return *cstack_top;
}

// ==== vstack accessors ====
// all return corresponding locations on the stack frame, not the value

inline aint* global(bytefile* bf, int ind) {
    assert(ind >= 0); // TODO
    return __gc_stack_bottom - bf->global_area_size + ind;
}

inline aint* arg(int ind) {
    assert(ind >= 0 && ind < nargs());
    return frame_pointer() + nargs() - 1 - ind;
}

inline aint* local(int ind) {
    auto nlcls = nlocals();
    assert(ind >= 0 && ind < nlcls);
    return frame_pointer() - nlcls + ind;
}

inline aint* closure_loc() {
    assert(is_closure());
    return frame_pointer() + nargs();
}

// ==== vstack accessors ====

inline aint get_closure(int ind) {
    auto closureLoc = closure_loc();
    auto closureData = TO_DATA(*closureLoc);
    if (TAG(closureData->data_header) != CLOSURE_TAG) {
        failure("Not a closure"); // TODO: more traces
    }
    return ((aint*)closureData->contents)[ind + 1];
}

inline void set_closure(int ind, aint value) {
    auto closureLoc = closure_loc();
    auto closureData = TO_DATA(*closureLoc);
    if (TAG(closureData->data_header) != CLOSURE_TAG) {
        failure("Not a closure"); // TODO: more traces
    }
    ((aint*)closureData->contents)[ind + 1] = value;
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

inline char readByte(char * &ip) {
    return *ip++;
}

inline int readInt(bytefile *f, char * &ip) {
    // TODO: check out of bounds
    ip += sizeof(int);
    return *reinterpret_cast<int *>(ip - sizeof(int));
}

inline char *readString(bytefile *f, char * &ip) {
    int pos = readInt(f, ip);
    // TODO: check string section size
    return &f->string_ptr[pos];
}

inline Loc readLoc(bytefile *f, char * &ip, char byte) {
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
            failure("Unsupported loc type");
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

inline void execString(char * string) {
    vstack_push((aint)Bstring((aint*)&string));
}

inline void execSexp(char * tag, int nargs) {
    aint args[nargs + 1];

    for (int i = nargs - 1; i >= 0; i--) {
        args[i] = vstack_pop();
    }

    args[nargs] = LtagHash(tag);

    auto result = (aint)Bsexp(args, BOX(nargs + 1));

    vstack_push(result);
    fprintf(stderr, "Sexp addr %.9llx", result);
}

inline void execSti() {
    // TODO
}

inline void execSta() {
    auto val = vstack_pop();
    auto ind = vstack_pop();
    auto dst = vstack_pop();

    void* result = Bsta((void*) dst, ind, (void*)val);

    vstack_push((aint)result);
}

inline void execSt(bytefile* bf, Loc& loc) {
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
            set_closure(loc.value, value);
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

    auto res = Belem((void*) src, ind);

    vstack_push((aint)res);
}

inline void execLd(bytefile* bf, Loc& loc) {
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
            value = get_closure(loc.value);
            break;
        }
    }

    vstack_push(value);
}

inline void execLda(bytefile* bf, Loc& loc) {
    failure("lda is not supported"); // TODO: unsupported
}

inline void execEnd(bytefile* bf, char* &ip) {
    aint retval = 0;
    bool isRetval = false;
    if (SP < frame_pointer() + nlocals()) {
        retval = vstack_pop();
        isRetval = true;
    }

    __gc_stack_top = frame_pointer() + nargs() + static_cast<int>(is_closure()) - 1;

    if (isRetval) {
        vstack_push(retval);
    }

    ip = (char*)ret_addr();
    cstack_top += 5; // TODO: check underflow
}

inline void execRet() {
    // TODO
}

inline void execCJmp(bytefile*bf, char* &ip, aint addr, bool isNz) {
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
    ip = bf->code_ptr + target; // TODO: check
}

inline void execBegin(int n_args, int n_locals) {
    cstack_push((aint)SP);
    cstack_push(n_args);
    cstack_push(n_locals);
    for (int i = 0; i < n_locals; i++) {
        vstack_push(BOX(0));
    }
}

inline void execTag(char* tag, int len) {
    auto dest = vstack_pop();
    vstack_push(Btag((void*)dest, LtagHash(tag), BOX(len)));
}

inline void execArray(int n) {
    auto dest = vstack_pop();
    vstack_push(Barray_patt((void*)dest, BOX(n)));
}

inline void execFail(int l, int c) {
    failure("Failed at %d %d", l, c);
}

inline void execLine(int line) {
    // TODO: print line
}

inline void execPatt(int patt) {
    std::string pats[] = {"=str", "#string", "#array", "#sexp", "#ref", "#val", "#fun"};
    auto x = (void*)vstack_pop();
    switch (patt) {
        case 0: {
            auto y = (void*)vstack_pop();
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
            failure("unexpected pattern");
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
    vstack_push(Llength((void*)x));
}

inline void execLstring() {
    vstack_push((aint)Lstring(SP));
}

inline void execBarray(int n) {
    aint args[n];
    for (auto i = 0; i < n; i++) {
        args[n - 1 - i] = vstack_pop();
    }
    vstack_push((aint)Barray(args, BOX(n)));
}

inline void execClosure(bytefile *bf, aint addr, int nargs, Loc* locs) {
    aint args[nargs + 1];
    for (int i = 1; i < nargs + 1; i++) {
        auto loc = locs[i - 1];
        switch (loc.type) {
            case Loc::Type::G: {
                args[i] = *global(bf, loc.value);
                break;
            }
            case Loc::Type::L: {
                args[i] = *local(loc.value);
                break;
            }
            case Loc::Type::A: {
                args[i] = *arg(loc.value);
                break;
            }
            case Loc::Type::C: {
                args[i] = get_closure(loc.value);
            }
        }
    }
    args[0] = addr;
    vstack_push((aint)Bclosure(args, BOX(nargs)));
}

inline void execCall(bytefile*bf, char* &ip, size_t addr, int nargs) {
    // TODO: check there is enough n_args on the vstack
    cstack_push(false); // not a closure
    cstack_push((aint)(ip)); // TODO: check ret address is in bytecode range
    // TODO: check addr is in bytecode range
    ip = bf->code_ptr + addr;
}

inline void execCallC(bytefile*bf, char* &ip, int nargs) {
    /*  stack frame is not yet complete:
     *  bottom
     *  ...
     *  *closure -> target, capture[0], capture[1], ...
     *  arg[n]
     *  ...
     *  arg[0] = sp
     */
    auto closureLoc = SP + nargs;
    auto target = ((aint*)*closureLoc)[0];
    cstack_push(true); // closure
    cstack_push((aint)(ip));
    ip = bf->code_ptr + target; // TODO: checks similar to call
}

void disassemble(FILE *f, bytefile *bf) {
    char *ip = bf->code_ptr;
    std::function<void(bytefile*, Loc&)> lds[] = {execLd, execLda, execSt};
    do {
        char opcode = readByte(ip),
                h = (opcode & 0b11110000) >> 4,
                l = opcode & 0b00001111;

        fprintf(f, "0x%.8x:\t", ip - bf->code_ptr - 1);

        switch (h) {
            case 15:
                goto stop;

            case 0:
                fprintf(f, "BINOP\t%d", l);
                execBinop(static_cast<BinOp>(l - 1));
                break;

            case 1:
                switch (l) {
                    case 0: {
                        auto cnst = readInt(bf, ip);
                        fprintf(f, "CONST\t%d", cnst);
                        execConst(cnst);
                        break;
                    }

                    case 1: {
                        auto str = readString(bf, ip);
                        fprintf(f, "STRING\t%s", str);
                        execString(str);
                        break;
                    }

                    case 2: {
                        auto str = readString(bf, ip);
                        auto i = readInt(bf, ip);
                        fprintf(f, "SEXP\t%s ", str);
                        fprintf(f, "%d", i);
                        execSexp(str, i);
                        break;
                    }

                    case 3: {
                        fprintf(f, "STI");
                        execSti();
                        break;
                    }

                    case 4: {
                        fprintf(f, "STA");
                        execSta();
                        break;
                    }

                    case 5: {
                        auto addr = readInt(bf, ip);
                        fprintf(f, "JMP\t0x%.8x", addr);
                        ip = bf->code_ptr + addr;
                        break;
                    }

                    case 6: {
                        fprintf(f, "END");
                        execEnd(bf, ip);
                        if (ip == (char*)END) return;
                        break;
                    }

                    case 7: {
                        fprintf(f, "RET");
                        execRet();
                        break;
                    }

                    case 8: {
                        fprintf(f, "DROP");
                        execDrop();
                        break;
                    }

                    case 9: {
                        fprintf(f, "DUP");
                        execDup();
                        break;
                    }

                    case 10: {
                        fprintf(f, "SWAP");
                        execSwap();
                        break;
                    }

                    case 11: {
                        fprintf(f, "ELEM");
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
                fprintf(f, "lds %d\t", h - 2);
                auto loc = readLoc(bf, ip, l);
                fprintf(f, "loc %d val %d", loc.type, loc.value);
                lds[h - 2](bf, loc);

                break;
            }

            case 5:
                switch (l) {
                    case 0:
                    case 1: {
                        auto i = readInt(bf, ip);
                        fprintf(f, "CJMP%d\t0x%.8x", l, i);
                        execCJmp(bf, ip, i, l == 1); // 0 - z, 1 -- nz
                        break;
                    }

                    case 3:
                    case 2: {
                        auto nargs = readInt(bf, ip);
                        auto nlocals = readInt(bf, ip);
                        fprintf(f, "BEGIN\t%d ", nargs);
                        fprintf(f, "%d", nlocals);
                        execBegin(nargs, nlocals);
                        break;
                    }

                    case 4: {
                        auto addr = readInt(bf, ip);
                        auto nLocs = readInt(bf, ip);
                        fprintf(f, "CLOSURE\t0x%.8x\t%d", addr, nLocs);
                        Loc locs[nLocs];
                        for (int i = 0; i < nLocs; i++) {
                            char locType = readByte(ip);
                            locs[i] = readLoc(bf, ip, locType);
                        }
                        execClosure(bf, addr, nLocs, locs);
                        break;
                    }

                    case 5: {
                        auto nargs = readInt(bf, ip);
                        fprintf(f, "CALLC\t%d", nargs);
                        execCallC(bf, ip, nargs);
                        break;
                    }

                    case 6: {
                        auto addr = readInt(bf, ip);
                        auto nargs = readInt(bf, ip);
                        fprintf(f, "CALL\t0x%.8x ", addr);
                        fprintf(f, "%d", nargs);
                        execCall(bf, ip, addr, nargs);
                        break;
                    }

                    case 7: {
                        auto tag = readString(bf, ip);
                        auto len = readInt(bf, ip);
                        fprintf(f, "TAG\t%s ", tag);
                        fprintf(f, "%d", len);
                        execTag(tag, len);
                        break;
                    }

                    case 8: {
                        auto i = readInt(bf, ip);
                        fprintf(f, "ARRAY\t%d", i);
                        execArray(i);
                        break;
                    }

                    case 9: {
                        auto ln = readInt(bf, ip);
                        auto cl = readInt(bf, ip);
                        fprintf(f, "FAIL\t%d", ln);
                        fprintf(f, "%d", cl);
                        execFail(ln, cl);
                        break;
                    }

                    case 10: {
                        auto i = readInt(bf, ip);
                        fprintf(f, "LINE\t%d", i);
                        execLine(i);
                        break;
                    }

                    default:
                        failure("unexpected opcode 5");
                }
                break;

            case 6: {
                fprintf(f, "PATT\t%d", l);
                execPatt(l);
                break;
            }

            case 7: {
                switch (l) {
                    case 0: {
                        fprintf(f, "CALL\tLread");
                        execLread();
                        break;
                    }

                    case 1: {
                        fprintf(f, "CALL\tLwrite");
                        execLwrite();
                        break;
                    }

                    case 2: {
                        fprintf(f, "CALL\tLlength");
                        execLlength();
                        break;
                    }

                    case 3: {
                        fprintf(f, "CALL\tLstring");
                        execLstring();
                        break;
                    }

                    case 4: {
                        auto n = readInt(bf, ip);
                        fprintf(f, "CALL\tBarray\t%d", n);
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

        fprintf(f, "\n");
    } while (1);
stop:
    fprintf(f, "<end>\n");
}

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cout << "Usage: ./lama-interpreter <bytecode-file>\n";
        return 1;
    }

    bytefile* bf = readFile(argv[1]);
    __gc_init();
    init_vstack(bf);
    cstack_push(false);
    cstack_push(END);
    disassemble(stderr, bf);

    free(bf);
}
