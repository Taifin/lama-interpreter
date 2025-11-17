#ifndef VIRTUAL_MACHINES_PROCESSOR_H
#define VIRTUAL_MACHINES_PROCESSOR_H
#include "common.h"
#include "../bytecode/bytefile.h"
#include "../runtime/runtime_common.h"
#include "../runtime/runtime.h"

extern aint *__gc_stack_top, *__gc_stack_bottom; // NOLINT(*-reserved-identifier)

struct ProcessorState {
    bytefile *bf = nullptr;
    char *ip = nullptr;
    unsigned char opcode = -1;

    void fail(const char *s, ...) const {
        va_list args;
        va_start(args, s);

        if (bf != nullptr && ip != nullptr) {
            fprintf(
                stderr, "Failure.\n\tinstruction offset: 0x%.8lx\n\topcode: %d\n",
                ip - bf->code_ptr - 1, opcode);
        }

        fprintf(stderr, "*** FAILURE: ");
        vfprintf(stderr, s, args);
        exit(255);
    }

    void update_ip(aint offset) {
        if (offset < 0 || offset > bf->code_size) {
            fail("Cannot move instruction pointer %.8x by offset %d, is out of bounds for [%.8x, %.8x] (%d)", ip,
                 offset,
                 bf->code_ptr, bf->code_ptr + bf->code_size, bf->code_size);
        }

        ip = bf->code_ptr + offset;
    }

    char readByte() {
        if (ip < bf->code_ptr || ip + 1 >= bf->code_ptr + bf->code_size) {
            fail("Instruction pointer %.8x out of bounds [%.8x, %.8x)", ip, bf->code_ptr, bf->code_ptr + bf->code_size);
        }
        return *ip++;
    }

    int readInt() {
        if (ip < bf->code_ptr || ip + sizeof(int) >= bf->code_ptr + bf->code_size) {
            fail("Instruction pointer %.8x out of bounds [%.8x, %.8x)", ip, bf->code_ptr, bf->code_ptr + bf->code_size);
        }
        ip += sizeof(int);
        return *reinterpret_cast<int *>(ip - sizeof(int));
    }

    char *readString() {
        int pos = readInt();
        if (pos < 0 || pos > bf->stringtab_size) {
            fail("Requested string %d is out of bounds for [0, %d)", pos, bf->stringtab_size);
        }
        return &bf->string_ptr[pos];
    }

    // ReSharper disable once CppNotAllPathsReturnValue
    Loc readLoc(unsigned char byte) {
        int val = readInt();
        switch (auto locType = static_cast<Loc::Type>(byte)) {
            case Loc::Type::G:
            case Loc::Type::L:
            case Loc::Type::A:
            case Loc::Type::C:
                return Loc(locType, val);
            default:
                fail("Unsupported loc type %d", byte);
        }
    }
};


template<typename Processor>
void processInstruction(Processor &processor, ProcessorState& state) {
    unsigned char opcode = state.readByte(),
            h = (opcode & 0xF0) >> 4, // NOLINT(cppcoreguidelines-narrowing-conversions)
            l = opcode & 0x0F; // NOLINT(cppcoreguidelines-narrowing-conversions)
    state.opcode = opcode;

    DEBUG("0x%.8lx:\t", state.ip - bf->code_ptr - 1);

    auto hi = static_cast<Instruction>(h), li = static_cast<Instruction>(l);
    switch (hi) {
        case Instruction::STOP:
            return;

        case Instruction::BINOP:
            processor.processBinop(state, static_cast<BinOp>(l - 1));
            break;

        case Instruction::CONST_H:
            switch (li) {
                case Instruction::CONST: {
                    auto cnst = state.readInt();
                    processor.processConst(state, cnst);
                    break;
                }

                case Instruction::STRING: {
                    auto str = state.readString();
                    processor.processString(state, str);
                    break;
                }

                case Instruction::SEXP: {
                    auto str = state.readString();
                    auto i = state.readInt();
                    processor.processSexp(state, str, i);
                    break;
                }

                case Instruction::STI: {
                    processor.processSti(state);
                    break;
                }

                case Instruction::STA: {
                    processor.processSta(state);
                    break;
                }

                case Instruction::JMP: {
                    auto addr = state.readInt();
                    processor.processJmp(state, addr);
                    break;
                }

                case Instruction::END: {
                    processor.processEnd(state);
                    break;
                }

                case Instruction::RET: {
                    processor.processRet(state);
                    break;
                }

                case Instruction::DROP: {
                    processor.processDrop(state);
                    break;
                }

                case Instruction::DUP: {
                    processor.processDup(state);
                    break;
                }

                case Instruction::SWAP: {
                    processor.processSwap(state);
                    break;
                }

                case Instruction::ELEM: {
                    processor.processElem(state);
                    break;
                }

                default:
                    state.fail("unexpected opcode %d", opcode);
            }
            break;

        case Instruction::LD: {
            auto loc = state.readLoc(l);
            processor.processLd(state, loc);
            break;
        }
        case Instruction::LDA: {
            auto loc = state.readLoc(l);
            processor.processLda(state, loc);
            break;
        }
        case Instruction::ST: {
            auto loc = state.readLoc(l);
            processor.processSt(state, loc);
            break;
        }

        case Instruction::CJMP_H:
            switch (li) {
                case Instruction::CJMPZ:
                case Instruction::CJMPNZ: {
                    auto i = state.readInt();
                    processor.processCJmp(state, i, l == 1); // 0 - z, 1 -- nz
                    break;
                }

                case Instruction::BEGIN:
                case Instruction::CBEGIN: {
                    auto nargs = state.readInt();
                    auto nlocals = state.readInt();
                    processor.processBegin(state, nargs, nlocals);
                    break;
                }

                case Instruction::CLOSURE: {
                    auto addr = state.readInt();
                    auto nLocs = state.readInt();
                    processor.processClosure(state, nLocs, addr);
                    break;
                }

                case Instruction::CALLC: {
                    auto nargs = state.readInt();
                    processor.processCallC(state, nargs);
                    break;
                }

                case Instruction::CALL: {
                    auto addr = state.readInt();
                    auto nargs = state.readInt();
                    processor.processCall(state, addr, nargs);
                    break;
                }

                case Instruction::TAG: {
                    auto tag = state.readString();
                    auto len = state.readInt();
                    processor.processTag(state, tag, len);
                    break;
                }

                case Instruction::ARRAY: {
                    auto i = state.readInt();
                    processor.processArray(state, i);
                    break;
                }

                case Instruction::FAIL: {
                    auto ln = state.readInt();
                    auto cl = state.readInt();
                    processor.processFail(state, ln, cl);
                    break;
                }

                case Instruction::LINE: {
                    auto i = state.readInt();
                    processor.processLine(state, i);
                    break;
                }

                default:
                    state.fail("unexpected opcode %d", opcode);
            }
            break;

        case Instruction::PATT_H: {
            processor.processPatt(state, l);
            break;
        }

        case Instruction::CALL_BUILTIN: {
            switch (li) {
                case Instruction::LREAD: {
                    processor.processLread(state);
                    break;
                }

                case Instruction::LWRITE: {
                    processor.processLwrite(state);
                    break;
                }

                case Instruction::LLENGTH: {
                    processor.processLlength(state);
                    break;
                }

                case Instruction::LSTRING: {
                    processor.processLstring(state);
                    break;
                }

                case Instruction::BARRAY: {
                    auto n = state.readInt();
                    processor.processBarray(state, n);
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
}

#endif //VIRTUAL_MACHINES_PROCESSOR_H
