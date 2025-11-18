#include <iomanip>
#include <iostream>
#include <sstream>
#include <unordered_set>
#include <vector>

#include "common.h"
#include "bytefile.h"
#include "runtime_common.h"
#include "processor.h"

struct BytecodeSeq {
    char* begin;
    char* end;
};

struct Idiom {
    BytecodeSeq seq;
    int count;
};

struct IsJumpProcessor : NoOpProcessor {
    bool isJump = false;
    void processCJmp(ProcessorState&, aint, bool) {
        isJump = true;
    }
    void processCall(ProcessorState&, size_t, int) {
        isJump = true;
    }
    void processCallC(ProcessorState&, int) {
        isJump = true;
    }
    void processJmp(ProcessorState&, int) {
        isJump = true;
    }
    void processEnd(ProcessorState&) {
        isJump = true;
    }
};

struct ReachableProcessor : NoOpProcessor {
    // implicit blocks: the next instruction is always added after any jumps
    std::stack<char*> next;
    bool isJmp = false;
    std::unordered_set<int> visited;
    bytefile* bf;

    explicit ReachableProcessor(bytefile *bf) : bf(bf) {
        next.push(bf->entrypoint_ptr);
    }

    bool isVisited(int addr) {
        return visited.contains(addr);
    }

    void processJmp(ProcessorState&, int addr) {
        isJmp = true;
        if (!isVisited(addr)) {
            next.push(bf->code_ptr + addr);
        }
    }

    void processCJmp(ProcessorState&, aint addr, bool) {
        if (!isVisited(addr)) {
            next.push(bf->code_ptr + addr);
        }
    }

    void processCall(ProcessorState&, size_t addr, int) {
        if (!isVisited(addr)) {
            next.push(bf->code_ptr + addr);
        }
    }

    void processClosure(ProcessorState& state, int nargs, int addr) {
        for (int i = 0; i < nargs; i++) {
            state.readByte();
            state.readInt();
        }

        if (!isVisited(addr)) {
            next.push(bf->code_ptr + addr);
        }
    }

    void processEnd(ProcessorState&) {
        isJmp = true;
    }

    void processFail(ProcessorState&, int, int) {
        isJmp = true;
    }
};

struct PrintProcessor : NoOpProcessor {
    std::ostringstream ss;

    void opcode(const std::string& o) {
        if (ss.tellp() != 0) {
            ss << ", ";
        }
        ss << o;
    }

    template <typename T>
    void arg(T a) {
        ss << " " << a;
    }

    void hex(int i) {
        ss << " 0x" << std::hex << std::setfill('0') << std::setw(8) << i << std::dec << std::setw(0);
    }

    void processBinop(ProcessorState&, BinOp op) {
        opcode("BINOP");
        arg((int)op);
    }
    void processConst(ProcessorState&, int i) {
        opcode("CONST");
        arg(i);
    }
    void processString(ProcessorState&, char* c) {
        opcode("STRING");
        arg(c);
    }
    void processSexp(ProcessorState&, char* c, int i) {
        opcode("SEXP");
        arg(c);
        arg(i);
    }
    void processSti(ProcessorState&) { opcode("STI"); }
    void processSta(ProcessorState&) { opcode("STA"); }
    void processJmp(ProcessorState&, int i) { opcode("JMP"); hex(i); }
    void processEnd(ProcessorState&) { opcode("END"); }
    void processRet(ProcessorState&) { opcode("RET"); }
    void processDrop(ProcessorState&) { opcode("DROP"); }
    void processDup(ProcessorState&) { opcode("DUP"); }
    void processSwap(ProcessorState&) { opcode("SWAP"); }
    void processElem(ProcessorState&) { opcode("ELEM"); }

    void processLd(ProcessorState&, const Loc& l) {
        opcode("LD");
        arg((int)l.type);
        arg(l.value);
    }
    void processLda(ProcessorState&, const Loc& l) {
        opcode("LDA");
        arg((int)l.type);
        arg(l.value);
    }
    void processSt(ProcessorState&, const Loc& l) {
        opcode("ST");
        arg((int)l.type);
        arg(l.value);
    }

    void processCJmp(ProcessorState&, aint i, bool b) {
        if (b) {
            opcode("CJMPnz");
        } else {
            opcode("CJMPz");
        }
        hex(i);
    }
    void processBegin(ProcessorState&, int i, int ii) {
        opcode("BEGIN");
        arg(i);
        arg(ii);
    }
    void processClosure(ProcessorState& state, int i, int ii) {
        for (int j = 0; j < i; j++) {
            auto b = state.readByte();
            state.readLoc(b);
        }

        opcode("CLOSURE");
        arg(i);
        hex(ii);
    }
    void processCallC(ProcessorState&, int i) {
        opcode("CALLC");
        arg(i);
    }
    void processCall(ProcessorState&, size_t i, int ii) {
        opcode("CALL");
        hex(i);
        arg(ii);
    }
    void processTag(ProcessorState&, char* c, int i) {
        opcode("TAG");
        arg(c);
        arg(i);
    }
    void processArray(ProcessorState&, int i) {
        opcode("ARRAY");
        arg(i);
    }
    void processFail(ProcessorState&, int i, int ii) {
        opcode("FAIL");
        arg(i);
        arg(ii);
    }
    void processLine(ProcessorState&, int i) {
        opcode("LINE");
        arg(i);
    }

    void processPatt(ProcessorState&, int i) {
        opcode("PATT");
        arg(i);
    }

    void processLread(ProcessorState&) { opcode("LREAD"); }
    void processLwrite(ProcessorState&) { opcode("LWRITE"); }
    void processLlength(ProcessorState&) { opcode("LLENGTH"); }
    void processLstring(ProcessorState&) { opcode("LSTRING"); }
    void processBarray(ProcessorState&, int i) { opcode("BARRAY"); arg(i); }
};

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <bytecode-file>" << std::endl;
        return 1;
    }

    auto *file = readFile(argv[1]);

    ReachableProcessor p(file);
    std::vector<BytecodeSeq> sequences;
    while (!p.next.empty()) {
        char* ip = p.next.top();
        p.next.pop();
        if (p.isVisited(ip - file->code_ptr)) {
            continue;
        }

        p.visited.insert(ip - file->code_ptr);
        ProcessorState state = {file, ip};
        processInstruction(p, state);

        sequences.emplace_back(ip, state.ip);

        // ReSharper disable once CppDFAConstantConditions
        if (!p.isJmp) {
            p.next.push(state.ip);
        }
        p.isJmp = false;
    }

    auto sequences_size = sequences.size();
    sequences.reserve(sequences_size * 2);
    for (int i = 0; i < sequences_size - 1; i++) {
        IsJumpProcessor isJmp;
        ProcessorState state = {file, sequences[i].begin};
        processInstruction(isJmp, state);

        // ReSharper disable once CppDFAConstantConditions
        if (!isJmp.isJump) {
            sequences.emplace_back(sequences[i].begin, sequences[i + 1].end);
        }
    }
    sequences.shrink_to_fit();

    constexpr auto compare = [](const BytecodeSeq &i1, const BytecodeSeq &i2) {
        for (auto i = 0; i1.begin + i != i1.end && i2.begin + i != i2.end; i++) {
            if (i1.begin[i] != i2.begin[i]) {
                return (int)i2.begin[i] - (int)i1.begin[i];
            }
        }

        return (int)(i2.end - i2.begin) - (int)(i1.end - i1.begin);
    };

    std::ranges::sort(sequences, [&](const BytecodeSeq& s1, const BytecodeSeq& s2) {
        return compare(s1, s2) < 0;
    });
    std::vector<Idiom> squashed;
    for (auto &e : sequences) {
        if (squashed.empty() || compare(squashed.back().seq, e) != 0) {
            squashed.emplace_back(e, 1);
        } else {
            squashed.back().count++;
        }
    }
    sequences.clear();

    std::ranges::sort(squashed, [](const Idiom &i1, const Idiom &i2) {return i1.count > i2.count; });
    for (auto [seq, count] : squashed) {
        PrintProcessor pp;
        ProcessorState s = {file, seq.begin};
        processInstruction(pp, s);
        if (s.ip != seq.end) {
            processInstruction(pp, s);
        }
        std::cout << "Sequence <" << pp.ss.str() << ">:\n\t" << count << " times" << std::endl;
    }
}