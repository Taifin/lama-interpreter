#include <fstream>
#include <iostream>
#include <string>

#include "common.h"
#include "processor.h"
#include "../bytecode/bytefile.h"
#include "../runtime/gc.h"
#include "../runtime/runtime.h"
#include "../runtime/runtime_common.h"

constexpr static int VSTACK_SIZE = 1 << 20;
constexpr static int CSTACK_SIZE = 1 << 20;

aint vstack[VSTACK_SIZE]{};
aint cstack[CSTACK_SIZE]{};

aint *cstack_top = cstack + CSTACK_SIZE;
aint *cstack_bottom = cstack + CSTACK_SIZE;

#define BINOP(op)                       \
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

struct Interpreter {
    ProcessorState& state;

    explicit Interpreter(ProcessorState &state) : state(state) {
    }

#define SP (__gc_stack_top + 1)

    inline void verify_vstack(aint *location, const std::string &trace) {
        if (location >= __gc_stack_bottom) {
            state.fail("Virtual stack underflow! .loc: %.8x, .bot: %.8x, trace: %s", location, __gc_stack_bottom,
                       trace.c_str());
        }
        if (location <= vstack) {
            state.fail("Virtual stack overflow! .loc: %.8x, .top: %.8x, trace: %s", location, vstack, trace.c_str());
        }
    }

    inline void verify_cstack(aint *location, const std::string &trace) {
        if (location >= cstack_bottom) {
            state.fail("Call stack underflow! .loc: %.8x, .bot: %.8x, trace: %s", location, cstack_bottom,
                       trace.c_str());
        }
        if (location <= cstack) {
            state.fail("Call stack overflow! .loc: %.8x, .top: %.8x, trace: %s", location, cstack, trace.c_str());
        }
    }

    inline aint vstack_pop() const {
        if (__gc_stack_top >= __gc_stack_bottom) {
            state.fail("Virtual stack underflow!");
        }
        __gc_stack_top += 1;
        return *(SP - 1);
    }

    inline void vstack_push(aint val) const {
        if (vstack >= __gc_stack_top) {
            state.fail("Virtual stack overflow!");
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

    inline void verify_cstack_underflow(int loc = 0, const char *msg = "Call stack underflow!") const {
        if (cstack_top + loc >= cstack_bottom) {
            state.fail(msg);
        }
    }

    inline void cstack_push(aint val) {
        if (cstack_top <= cstack) {
            state.fail("Call stack overflow!");
        }
        *--cstack_top = val;
    }

    inline aint cstack_pop() {
        verify_cstack_underflow();
        return *cstack_top++;
    }

    inline bool is_closure() const {
        verify_cstack_underflow(4, "Invalid call stack: expected closure flag");
        return (bool) *(cstack_top + 4);
    }

    inline aint ret_addr() const {
        verify_cstack_underflow(3, "Invalid call stack: expected return address");
        return *(cstack_top + 3);
    }

    inline aint *frame_pointer() const {
        verify_cstack_underflow(2, "Invalid call stack: expected frame pointer");
        return (aint *) *(cstack_top + 2);
    }

    inline aint nargs() const {
        verify_cstack_underflow(1, "Invalid call stack: expected number of args");
        return *(cstack_top + 1);
    }

    inline aint nlocals() const {
        verify_cstack_underflow(0, "Invalid call stack: expected number of locals");
        return *cstack_top;
    }

    inline aint *global(const bytefile *bf, int ind) {
        if (ind < 0 || ind >= bf->global_area_size) {
            state.fail("Requested global %d is out of bounds for [0, %d)", ind, bf->global_area_size);
        }

        auto loc = __gc_stack_bottom - bf->global_area_size + ind;
        verify_vstack(loc, ".global");
        return loc;
    }

    inline aint *arg(int ind) {
        if (ind < 0 || ind >= nargs()) {
            state.fail("Requested argument %d is out of bounds for [0, %d)", ind, nargs());
        }

        auto loc = frame_pointer() + nargs() - 1 - ind;
        verify_vstack(loc, ".arg");
        return loc;
    }

    inline aint *local(int ind) {
        auto nlcls = nlocals();
        if (ind < 0 || ind >= nlocals()) {
            state.fail("Requested local %d is out of bounds for [0, %d)", ind, nlocals());
        }

        auto loc = frame_pointer() - nlcls + ind;
        verify_vstack(loc, ".local");
        return loc;
    }

    inline aint *closure_loc() {
        if (!is_closure()) {
            state.fail("Requested closure, but closure is not placed on stack");
        }

        auto loc = frame_pointer() + nargs();
        verify_vstack(loc, ".closure");
        return loc;
    }

    inline aint *closure(int ind) {
        auto closureLoc = closure_loc();
        auto closureData = TO_DATA(*closureLoc);

        if (TAG(closureData->data_header) != CLOSURE_TAG) {
            state.fail("Requested closure element %d, but the value on stack is not a closure", ind);
        }

        return &((aint *) closureData->contents)[ind + 1];
    }

    inline void processBinop(ProcessorState& _, const BinOp &op) const {
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

    inline void processConst(ProcessorState& _, int cnst) const {
        vstack_push(BOX(cnst));
    }

    inline void processJmp(ProcessorState& _, int addr) const {
        state.ip = state.bf->code_ptr + addr;
    }

    inline void processString(ProcessorState& _, char *string) const {
        vstack_push((aint) Bstring((aint *) &string));
    }

    inline void processSexp(ProcessorState& _, char *tag, int nargs) {
        if (nargs < 0) {
            state.fail("Invalid SEXP op: negative length %d", nargs);
        }

        verify_vstack(SP + nargs, ".sexp");
        vstack_push(LtagHash(tag));

        auto result = (aint) Bsexp(SP, BOX(nargs + 1));

        __gc_stack_top += nargs + 1;
        vstack_push(result);
    }

    inline void processSti(ProcessorState& _) const {
        state.fail("Unsupported instruction STI");
    }

    inline void processSta(ProcessorState& _) const {
        auto val = vstack_pop();
        auto ind = vstack_pop();
        auto dst = vstack_pop();

        void *result = Bsta((void *) dst, ind, (void *) val);

        vstack_push((aint) result);
    }

    inline void processSt(ProcessorState& _, const Loc &loc) {
        auto value = vstack_pop();
        switch (loc.type) {
            case Loc::Type::G: {
                *global(state.bf, loc.value) = value;
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

    inline void processDrop(ProcessorState& _) const {
        vstack_pop();
    }

    inline void processDup(ProcessorState& _) const {
        auto val = vstack_pop();
        vstack_push(val);
        vstack_push(val);
    }

    inline void processSwap(ProcessorState& _) const {
        auto x = vstack_pop();
        auto y = vstack_pop();

        vstack_push(y);
        vstack_push(x);
    }

    inline void processElem(ProcessorState& _) const {
        auto ind = vstack_pop();
        auto src = vstack_pop();

        auto res = Belem((void *) src, ind);

        vstack_push((aint) res);
    }

    inline aint load(ProcessorState& _, const Loc &loc) {
        aint value{};
        switch (loc.type) {
            case Loc::Type::G: {
                value = *global(state.bf, loc.value);
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

    inline void processLd(ProcessorState& _,  const Loc &loc) {
        vstack_push(load(state, loc));
    }

    inline void processLda(ProcessorState& _,  const Loc &) const {
        state.fail("LDA is not supported");
    }

    inline void processEnd(ProcessorState& _) {
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

        state.update_ip(ret_addr());

        verify_cstack(cstack_top + 4, ".end"); // same
        cstack_top += 5;
    }

    inline void processRet(ProcessorState& _) const {
        state.fail("RET is not supported");
    }

    inline void processCJmp(ProcessorState& _, aint addr, bool isNz) const {
        if (auto val = UNBOX(vstack_pop()); isNz != !val) {
            state.update_ip(addr);
        }
    }

    inline void processBegin(ProcessorState& _, int n_args, int n_locals) {
        cstack_push((aint) SP);
        cstack_push(n_args);
        cstack_push(n_locals);
        for (int i = 0; i < n_locals; i++) {
            vstack_push(BOX(0));
        }
    }

    inline void processTag(ProcessorState& _, char *tag, int len) const {
        auto dest = vstack_pop();
        vstack_push(Btag((void *) dest, LtagHash(tag), BOX(len)));
    }

    inline void processArray(ProcessorState& _, int n) const {
        auto dest = vstack_pop();
        vstack_push(Barray_patt((void *) dest, BOX(n)));
    }

    inline void processFail(ProcessorState& _, int l, int c) const {
        state.fail("Failed at %d %d", l, c);
    }

    static inline void processLine(ProcessorState& _, int) {
    }

    inline void processPatt(ProcessorState& _, int patt) const {
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

    inline void processLread(ProcessorState& _) const {
        vstack_push(Lread());
    }

    inline void processLwrite(ProcessorState& _) const {
        auto x = vstack_pop();
        vstack_push(Lwrite(x));
    }

    inline void processLlength(ProcessorState& _) const {
        auto x = vstack_pop();
        vstack_push(Llength((void *) x));
    }

    inline void processLstring(ProcessorState& _) const {
        vstack_push((aint) Lstring(SP));
    }

    inline void processBarray(ProcessorState& _, int n) {
        verify_vstack(SP + n, ".barray");
        auto arrayPtr = (aint) Barray(SP, BOX(n));
        __gc_stack_top += n;
        vstack_push(arrayPtr);
    }

    inline void processClosure(ProcessorState& _, int nargs, int addr) {
        for (int i = 0; i < nargs; i++) {
            char locType = state.readByte();
            auto loc = state.readLoc(locType);
            vstack_push(load(state, loc));
        }
        vstack_push(addr);
        auto *closurePtr = Bclosure(SP, BOX(nargs));
        __gc_stack_top += nargs + 1;
        vstack_push((aint) closurePtr);
    }

    inline void processCall(ProcessorState& _, size_t addr, int nargs) {
        verify_vstack(SP + nargs, ".call");

        cstack_push(false); // not a closure
        cstack_push(state.ip - state.bf->code_ptr);

        state.update_ip((aint) addr);
    }

    inline void processCallC(ProcessorState& _, int nargs) {
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
        cstack_push(state.ip - state.bf->code_ptr);

        state.update_ip(target);
    }
};

int main(const int argc, char **argv) {
    if (argc != 2) {
        std::cout << "Usage: ./lama-interpreter <bytecode-file>\n";
        return 1;
    }

    bytefile *bf = readFile(argv[1]);

    ProcessorState state = {bf, bf->entrypoint_ptr};
    Interpreter interpreter{state};

    __gc_init();
    interpreter.init_vstack(bf);
    interpreter.cstack_push(false);
    interpreter.cstack_push(bf->code_size);
    do {
        processInstruction(interpreter, state);
        if (state.ip == bf->code_ptr + bf->code_size) break;
    } while (true);


    free(bf);
}
