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
    int length;
};

struct Idiom {
    BytecodeSeq *seq;
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
    std::stack<int> next;
    bool isJmp = false;
    std::vector<bool> visited;
    bytefile* bf;

    explicit ReachableProcessor(bytefile *bf, const std::unordered_set<int>& entrypoints) : visited(bf->code_size, false), bf(bf) {
        for (auto &e : entrypoints) {
            next.push(e);
        }
    }

    bool isVisited(int addr) {
        return visited[addr];
    }

    void visit(int addr) {
        if (!isVisited(addr)) {
            visited[addr] = true;
            next.push(addr);
        }
    }

    void processJmp(ProcessorState&, int addr) {
        isJmp = true;
        visit(addr);
    }

    void processCJmp(ProcessorState&, aint addr, bool) {
        visit(addr);
    }

    void processCall(ProcessorState&, size_t addr, int) {
        visit(addr);
    }

    void processClosure(ProcessorState& state, int nargs, int addr) {
        for (int i = 0; i < nargs; i++) {
            state.readByte();
            state.readInt();
        }

        visit(addr);
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

    std::unordered_set<int> entrypoints;
    for (int i = 0; i < file->public_symbols_number; i++) {
        entrypoints.insert(get_public_offset(file, i));
    }

    ReachableProcessor p(file, entrypoints);
    std::vector<BytecodeSeq> sequences;
    while (!p.next.empty()) {
        auto nextOffset = p.next.top();
        p.next.pop();

        auto ip = file->code_ptr + nextOffset;
        ProcessorState state = {file, ip};
        processInstruction(p, state);

        sequences.emplace_back(ip, state.ip - ip);

        // ReSharper disable once CppDFAConstantConditions
        if (!p.isJmp) {
            p.visit(state.ip - file->code_ptr);
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
        if (!isJmp.isJump && !entrypoints.contains(sequences[i + 1].begin - file->code_ptr)) {
            sequences.emplace_back(sequences[i].begin, sequences[i].length + sequences[i + 1].length);
        }
    }
    sequences.shrink_to_fit();

    constexpr auto compare = [](const BytecodeSeq &i1, const BytecodeSeq &i2) {
        for (auto i = 0; i != i1.length && i != i2.length; i++) {
            if (i1.begin[i] != i2.begin[i]) {
                return (int)i2.begin[i] - (int)i1.begin[i];
            }
        }

        return i2.length - i1.length;
    };

    std::ranges::sort(sequences, [&](const BytecodeSeq& s1, const BytecodeSeq& s2) {
        return compare(s1, s2) < 0;
    });
    std::vector<Idiom> squashed;
    for (auto &e : sequences) {
        if (squashed.empty() || compare(*squashed.back().seq, e) != 0) {
            squashed.emplace_back(&e, 1);
        } else {
            squashed.back().count++;
        }
    }

    std::ranges::sort(squashed, [](const Idiom &i1, const Idiom &i2) {return i1.count > i2.count; });
    for (auto [seq, count] : squashed) {
        PrintProcessor pp;
        ProcessorState s = {file, seq->begin};
        processInstruction(pp, s);
        if (s.ip - seq->begin != seq->length) {
            processInstruction(pp, s);
        }
        std::cout << "Sequence <" << pp.ss.str() << ">:\n\t" << count << " times" << std::endl;
    }
}