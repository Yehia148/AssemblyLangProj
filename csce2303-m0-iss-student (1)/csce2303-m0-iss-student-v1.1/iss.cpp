/*   Build: g++ -std=c++17 -O2 -Wall -Wextra -o iss iss.cpp */
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <deque>
#include <fstream>
#include <string>
#include <vector>
#include <cerrno>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

using namespace std;

constexpr uint32_t ROM_BASE = 0x00000000u;
constexpr uint32_t ROM_SIZE = (1u << 20);
constexpr uint32_t RAM_BASE = 0x20000000u;
constexpr uint32_t RAM_SIZE = (256u << 10);
constexpr uint64_t MAX_INSTR = 100000000ull;

uint8_t  rom[ROM_SIZE], ram[RAM_SIZE];
uint32_t r[16];
int  N, Z, C, V;
uint64_t instret;


uint64_t max_instr = MAX_INSTR;
uint32_t seed = 2463534242u;
string cpu = "m0";
FILE* trace_file = nullptr;
int exit_status = -1;

struct TextRange {
    uint32_t start, end;
};
struct MemWrite {
    uint32_t address, value;
    int size;
};
vector<TextRange> text_ranges;
vector<MemWrite> writes;

void fault_at(const char* kind, uint32_t pc)
{
    fprintf(stderr, "FAULT %s at pc=0x%08x\n", kind, unsigned(pc));
    exit(255);
}

void fault(const char* kind)
{
    fault_at(kind, r[15]);
}

uint8_t *mem_ptr(uint32_t addr)
{
    if (addr-ROM_BASE<ROM_SIZE) return &rom[addr-ROM_BASE];
    if (addr-RAM_BASE<RAM_SIZE) return &ram[addr-RAM_BASE];
    return nullptr;
}

uint32_t read_mem(uint32_t addr, int size)
{
    if (addr % size) fault("UNALIGNED");
    uint8_t *p = mem_ptr(addr);
    if (!p || !mem_ptr(addr + unsigned(size - 1))) fault("BUS");
    uint32_t v = 0;
    for (int i = size - 1; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

void write_mem(uint32_t addr, uint32_t v, int size)
{
    if (addr % size) fault("UNALIGNED");
    if (addr - RAM_BASE > RAM_SIZE - unsigned(size)) fault("BUS");
    for (int i = 0; i < size; i++)
        ram[addr - RAM_BASE + i] = (uint8_t)(v >> (8 * i));

    // Member C: retain each instruction's RAM writes for the trace.
    if (trace_file) {
        uint32_t mask = size == 4 ? 0xffffffffu : (1u << (8 * size)) - 1u;
        writes.push_back({addr, v & mask, size});
    }
}


void load_error() {
    fprintf(stderr, "load error\n");
    exit(253);
}
int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c-'0';
    if (c >= 'a' && c <= 'f') return c-'a'+10;
    if (c >= 'A' && c <= 'F') return c-'A'+ 10;
    return -1;
}
void load_dump(const char* path)
{
    ifstream in(path);
    if (!in) load_error();
    memset(rom, 0, sizeof rom);
    memset(ram, 0, sizeof ram);
    text_ranges.clear();
    string line;
    bool in_text = false;
    while (getline(in, line)) {
        if (line.find("Hex dump of section") != string::npos) {
            in_text = line.find("'.text'") != string::npos;
            continue;
        }
        auto first = line.find_first_not_of(" \t\r");
        if (first == string::npos || line.compare(first, 2, "0x") != 0) continue;
        auto addr_end = line.find_first_of(" \t", first);
        if (addr_end == string::npos || addr_end == first + 2) load_error();
        uint64_t addr = 0;
        for (auto i = first + 2; i < addr_end; ++i) {
            int d = hex_digit(line[i]);
            if (d < 0 || addr > 0x0fffffffu) load_error();
            addr = addr * 16 + unsigned(d);
        }
        const uint64_t start = addr;

        string field = line.substr(addr_end + 1, 35);
        for (size_t i = 0; i < field.size();) {
            if (field[i] == ' ' || field[i] == '\t' || field[i] == '\r') { ++i; continue; }
            int hi = hex_digit(field[i]);
            int lo = i + 1 < field.size() ? hex_digit(field[i + 1]) : -1;
            if (hi < 0 || lo < 0 || addr > 0xffffffffull) load_error();
            auto p = mem_ptr(uint32_t(addr));
            if (!p) load_error();
            *p = uint8_t(hi * 16 + lo);
            ++addr; i += 2;
        }
        if (in_text && addr > start) {
            if (!text_ranges.empty() && text_ranges.back().end == start)
                text_ranges.back().end = uint32_t(addr);
            else text_ranges.push_back({uint32_t(start), uint32_t(addr)});
        }
    }
    if (in.bad()) load_error();
}

void reset()
{
    fill(r, r + 16, 0u);
    N = Z = C = V = 0;
    instret = 0;
    exit_status = -1;
    writes.clear();
    r[13] = read_mem(0, 4) & ~3u;
    uint32_t pc = read_mem(4, 4);
    r[15] = pc;
    if (!(pc & 1)) fault("INVSTATE");
    r[15] = pc & ~1u;
    r[14] = 0xFFFFFFFFu;
}

void set_nz(uint32_t x) { N = x >> 31; Z = (x == 0); }

deque<int> pending_input;
int get_byte() {
    if (pending_input.empty()) return getchar();
    int c = pending_input.front();
    pending_input.pop_front();
    return c;
}
void unread_byte(int c) { if (c != EOF) pending_input.push_front(c); }
bool ascii_space(int c) { return c == ' ' || (c >= 9 && c <= 13); }
bool digit(int c) { return c >= '0' && c <= '9'; }
uint32_t get_int() {
    int c;
    do { c = get_byte(); } while (ascii_space(c));
    int sign = 0;
    if (c == '+' || c == '-') { sign = c; c = get_byte(); }
    if (!digit(c)) {
        unread_byte(c);
        if (sign) unread_byte(sign);
        return 0;
    }
    uint32_t value = 0;
    do {
        value = value * 10u + unsigned(c - '0'); // wraps modulo 2^32
        c = get_byte();
    } while (digit(c));
    unread_byte(c);
    return sign == '-' ? 0u - value : value;
}

void svc(int n)
{
    switch (n) {
    case 0x00: exit_status = int(r[0] & 0xffu); break;
    case 0x10: putchar(r[0] & 0xffu); break;
    case 0x11: { 
        uint32_t a = r[0];
        for (;;) {
            uint32_t byte = read_mem(a++, 1);
            if (!byte) break;
            putchar(int(byte));
        }
        break;
    }
    case 0x12: {
        int64_t value = r[0];
        if (r[0] & 0x80000000u) value -= (int64_t(1) << 32);
        printf("%lld", static_cast<long long>(value));
        break;
    }
    // Member C: new console services.
    case 0x13: printf("0x%08X", unsigned(r[0])); break;
    case 0x14: {
        int c = get_byte();
        r[0] = c == EOF ? 0xffffffffu : uint32_t(c);
        break;
    }
    case 0x15: r[0] = get_int(); break;
    default: fault("UNDEFINED");
    }
}

void write_trace(uint32_t pc, uint16_t hw, uint16_t hw2, bool wide, const uint32_t before[16])
{
    if (!trace_file) return;
    fprintf(trace_file, "%08x %04x", unsigned(pc), unsigned(hw));
    if (wide) fprintf(trace_file, "%04x", unsigned(hw2));
    for (int i = 0; i < 15; ++i) {
        if (r[i] == before[i]) continue;
        if (i < 13) fprintf(trace_file, " r%d=%08x", i, unsigned(r[i]));
        else fprintf(trace_file, " %s=%08x", i == 13 ? "sp" : "lr", unsigned(r[i]));
    }
    stable_sort(writes.begin(), writes.end(), [](const MemWrite& a, const MemWrite& b) {
        return a.address < b.address;
    });
    for (const auto& w : writes)
        fprintf(trace_file, " [%08x]=%0*x", unsigned(w.address), w.size * 2, unsigned(w.value));
    fprintf(trace_file, " NZCV=%d%d%d%d\n", N, Z, C, V);
}

uint16_t fetch_halfword(uint32_t addr) {
    if (addr & 1u) fault_at("UNALIGNED", addr);
    if (!mem_ptr(addr) || !mem_ptr(addr + 1u)) fault_at("BUS", addr);
    return uint16_t(read_mem(addr, 2));
}

// Shared instruction dispatch: Member A and B extend the starter cases.
void execute_instruction(uint16_t hw, uint16_t hw2, bool wide,
                         uint32_t pc, uint32_t& next)
{
    if (wide) {
        (void)hw2;
        fault("UNDEFINED"); // Member B TODO: BL
    } else if ((hw & 0xf800) == 0x2000) {
        unsigned rd = (hw >> 8) & 7;
        r[rd] = hw & 0xff;
        set_nz(r[rd]);
    } else if ((hw & 0xf800) == 0xe000) {
        int32_t off = hw & 0x7ff;
        if (off & 0x400) off -= 0x800;
        next = pc + 4 + off * 2;
    } else if ((hw & 0xff00) == 0xdf00) {
        svc(hw & 0xff);
    } else if (hw == 0xbf00) {
    } else {
        // Member A: loads/stores, PUSH/POP, LDM/STM.
        // Member B: other arithmetic/logic/shifts/branches.
        fault("UNDEFINED");
    }
}

void step()
{
    const uint32_t pc = r[15];
    const uint16_t hw = fetch_halfword(pc);
    const bool wide = (hw >> 11) >= 0x1d;
    const uint16_t hw2 = wide ? fetch_halfword(pc + 2u) : 0;
    uint32_t next = pc + (wide ? 4u : 2u);
    uint32_t before[16];
    if (trace_file) copy(r, r + 16, before);
    writes.clear();
    execute_instruction(hw, hw2, wide, pc, next);
    r[13] &= ~3u;
    ++instret;
    write_trace(pc, hw, hw2, wide, before);
    r[15] = next;
    if (exit_status >= 0) exit(exit_status); // EXIT retires and is traced
}

// Member A replaces this stub with the actual disassembly listing.

int disassemble() {
    fprintf(stderr, "--disasm: waiting for Member A's decoder/disassembler\n");
    return 2;
}

bool parse_number(const char* s, uint64_t& out) {
    if (!*s || *s == '-' || *s == '+') return false;
    for (const char* p = s; *p; ++p) if (!digit(*p)) return false;
    errno = 0;
    char* end = nullptr;
    unsigned long long value = strtoull(s, &end, 10);
    if (errno == ERANGE || *end) return false;
    out = value;
    return true;
}

int usage(const char* name) {
    fprintf(stderr, "usage: %s [--disasm] [--trace FILE] [--max-instr N] [--seed N] [--cpu m0|m0plus] PROGRAM\n", name);
    return 2;
}

int main(int argc, char** argv)
{

#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif

    const char* program = nullptr;
    const char* trace_path = nullptr;
    bool disasm = false;
    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];
        if (arg == "--disasm") disasm = true;
        else if (arg == "--trace" || arg == "--max-instr" || arg == "--seed" || arg == "--cpu") {
            if (++i >= argc) return usage(argv[0]);
            if (arg == "--trace") trace_path = argv[i];
            else if (arg == "--cpu") {
                cpu = argv[i];
                if (cpu != "m0" && cpu != "m0plus") return usage(argv[0]);
            } else {
                uint64_t n;
                if (!parse_number(argv[i], n)) return usage(argv[0]);
                if (arg == "--max-instr") max_instr = n;
                else {
                    if (n > 0xffffffffull) return usage(argv[0]);
                    seed = uint32_t(n);
                }
            }
        } else {
            if (program || (!arg.empty() && arg[0] == '-')) return usage(argv[0]);
            program = argv[i];
        }
    }
    if (!program) return usage(argv[0]);
    load_dump(program);
    if (disasm) return disassemble();
    if (trace_path) {
        trace_file = fopen(trace_path, "wb");
        if (!trace_file) { perror(trace_path); return 2; }
    }

    reset();
    for (;;) {
        if (instret >= max_instr) {
            fprintf(stderr, "LIMIT at pc=0x%08x\n", unsigned(r[15]));
            return 254;
        }
        step();
    }
}
