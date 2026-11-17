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
uint32_t add_with_carry(uint32_t a, uint32_t b, uint32_t carry)
{
    uint64_t sum = uint64_t(a) + uint64_t(b) + carry;
    uint32_t result = uint32_t(sum);

    set_nz(result);
    C = int(sum >> 32);
    V = int((~(a ^ b) & (a ^ result)) >> 31);

    return result;
}
uint32_t subtract(uint32_t a, uint32_t b)
{
    return add_with_carry(a, ~b, 1);
}
bool condition_passed(unsigned condition)
{
    switch (condition) {
        case 0:  return Z == 1;
        case 1:  return Z == 0;
        case 2:  return C == 1;
        case 3:  return C == 0;
        case 4:  return N == 1;
        case 5:  return N == 0;
        case 6:  return V == 1;
        case 7:  return V == 0;
        case 8:  return C == 1 && Z == 0;
        case 9:  return C == 0 || Z == 1;
        case 10: return N == V;
        case 11: return N != V;
        case 12: return Z == 0 && N == V;
        case 13: return Z == 1 || N != V;
        default:
            fault("UNDEFINED");
            return false;
    } 
}
uint32_t shift_value(uint32_t value, unsigned type, unsigned amount)
{
    uint32_t result = value;

    if (amount == 0) {
        set_nz(result);
        return result;
    }

    if (type == 0) { 
        if (amount < 32) {
            C = (value >> (32 - amount)) & 1;
            result = value << amount;
        } else {
            C = 0;
            if (amount == 32)
                C = value & 1;
            result = 0;
        }
    }

    else if (type == 1) { 
        if (amount < 32) {
            C = (value >> (amount - 1)) & 1;
            result = value >> amount;
        } else {
            C = 0;
            if (amount == 32)
                C = value >> 31;
            result = 0;
        }
    }

    else if (type == 2) { 
        bool negative = (value >> 31) != 0;

        if (amount < 32) {
            C = (value >> (amount - 1)) & 1;
            result = value >> amount;

            if (negative)
                result |= 0xffffffffu << (32 - amount);
        } else {
            C = negative;
            result = 0;

            if (negative)
                result = 0xffffffffu;
        }
    }

    else if (type == 3) { 
        unsigned rotation = amount % 32;

        if (rotation != 0)
            result = (value >> rotation) |
                     (value << (32 - rotation));

        C = result >> 31;
    }

    set_nz(result);
    return result;
}
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
bool execute_b(uint16_t hw, uint16_t hw2, bool wide,
               uint32_t pc, uint32_t& next)
{
    if (wide) {
        if ((hw & 0xf800) != 0xf000)
            return false;

        if ((hw2 & 0xd000) != 0xd000)
            return false;

        unsigned sign = (hw >> 10) & 1;
        unsigned j1 = (hw2 >> 13) & 1;
        unsigned j2 = (hw2 >> 11) & 1;
        unsigned i1 = 1 ^ (j1 ^ sign);
        unsigned i2 = 1 ^ (j2 ^ sign);

        uint32_t bits = (sign << 24) |
                        (i1 << 23) |
                        (i2 << 22) |
                        ((hw & 0x3ff) << 12) |
                        ((hw2 & 0x7ff) << 1);

        int32_t offset = int32_t(bits);

        if (sign == 1)
            offset -= 0x02000000;

        r[14] = (pc + 4u) | 1u;
        next = pc + 4u + uint32_t(offset);

        return true;
    }

    if ((hw & 0xe000) == 0x0000 &&
        (hw & 0x1800) != 0x1800) {
        unsigned type = (hw >> 11) & 3;
        unsigned amount = (hw >> 6) & 31;
        unsigned source = (hw >> 3) & 7;
        unsigned destination = hw & 7;

        if (type != 0 && amount == 0)
            amount = 32;

        r[destination] = shift_value(r[source], type, amount);

        return true;
    }

    if ((hw & 0xf800) == 0x1800) {
        unsigned destination = hw & 7;
        unsigned source = (hw >> 3) & 7;
        unsigned operand = (hw >> 6) & 7;

        uint32_t value;

        if (hw & 0x0400)
            value = operand;
        else
            value = r[operand];

        if (hw & 0x0200)
            r[destination] = subtract(r[source], value);
        else
            r[destination] = add_with_carry(r[source], value, 0);

        return true;
    }

    if ((hw & 0xe000) == 0x2000) {
        unsigned operation = (hw >> 11) & 3;
        unsigned destination = (hw >> 8) & 7;
        uint32_t value = hw & 0xff;

        if (operation == 0) {
            r[destination] = value;
            set_nz(value);
        }
        else if (operation == 1) {
            subtract(r[destination], value);
        }
        else if (operation == 2) {
            r[destination] =
                add_with_carry(r[destination], value, 0);
        }
        else {
            r[destination] = subtract(r[destination], value);
        }

        return true;
    }

    if ((hw & 0xfc00) == 0x4000) {
        unsigned operation = (hw >> 6) & 15;
        unsigned source = (hw >> 3) & 7;
        unsigned destination = hw & 7;

        uint32_t a = r[destination];
        uint32_t b = r[source];

        if (operation == 0) {
            r[destination] = a & b;
            set_nz(r[destination]);
        }
        else if (operation == 1) {
            r[destination] = a ^ b;
            set_nz(r[destination]);
        }
        else if (operation == 2) {
            r[destination] = shift_value(a, 0, b & 0xff);
        }
        else if (operation == 3) {
            r[destination] = shift_value(a, 1, b & 0xff);
        }
        else if (operation == 4) {
            r[destination] = shift_value(a, 2, b & 0xff);
        }
        else if (operation == 5) {
            r[destination] = add_with_carry(a, b, C);
        }
        else if (operation == 6) {
            r[destination] = add_with_carry(a, ~b, C);
        }
        else if (operation == 7) {
            r[destination] = shift_value(a, 3, b & 0xff);
        }
        else if (operation == 8) {
            set_nz(a & b);
        }
        else if (operation == 9) {
            r[destination] = subtract(0, b);
        }
        else if (operation == 10) {
            subtract(a, b);
        }
        else if (operation == 11) {
            add_with_carry(a, b, 0);
        }
        else if (operation == 12) {
            r[destination] = a | b;
            set_nz(r[destination]);
        }
        else if (operation == 13) {
            r[destination] = uint32_t(uint64_t(a) * b);
            set_nz(r[destination]);
        }
        else if (operation == 14) {
            r[destination] = a & ~b;
            set_nz(r[destination]);
        }
        else {
            r[destination] = ~b;
            set_nz(r[destination]);
        }

        return true;
    }

    if ((hw & 0xfc00) == 0x4400) {
        unsigned operation = (hw >> 8) & 3;
        unsigned source = (hw >> 3) & 15;
        unsigned destination = (hw & 7) | ((hw >> 4) & 8);

        uint32_t a = r[destination];
        uint32_t b = r[source];

        if (destination == 15)
            a = pc + 4u;

        if (source == 15)
            b = pc + 4u;

        if (operation == 3) {
            if ((hw & 7) != 0)
                fault("UNDEFINED");

            if ((b & 1) == 0)
                fault("INVSTATE");

            if (hw & 0x0080)
                r[14] = (pc + 2u) | 1u;

            next = b & ~1u;
        }
        else if (operation == 1) {
            subtract(a, b);
        }
        else {
            uint32_t result;

            if (operation == 0)
                result = a + b;
            else
                result = b;

            if (destination == 15)
                next = result & ~1u;
            else if (destination == 13)
                r[13] = result & ~3u;
            else
                r[destination] = result;
        }

        return true;
    }

    if ((hw & 0xf000) == 0xa000) {
        unsigned destination = (hw >> 8) & 7;
        uint32_t offset = (hw & 0xff) * 4u;
        uint32_t base;

        if (hw & 0x0800)
            base = r[13];
        else
            base = (pc + 4u) & ~3u;

        r[destination] = base + offset;

        return true;
    }

    if ((hw & 0xff00) == 0xb000) {
        uint32_t offset = (hw & 0x7f) * 4u;

        if (hw & 0x0080)
            r[13] -= offset;
        else
            r[13] += offset;

        r[13] &= ~3u;

        return true;
    }

    if ((hw & 0xf000) == 0xd000) {
        unsigned condition = (hw >> 8) & 15;

        if (condition == 15)
            return false;

        if (condition == 14)
            fault("UNDEFINED");

        int32_t offset = hw & 0xff;

        if (offset >= 128)
            offset -= 256;

        if (condition_passed(condition))
            next = pc + 4u + uint32_t(offset * 2);

        return true;
    }

    return false;
}
// Shared instruction dispatch: Member A and B extend the starter cases.
void execute_instruction(uint16_t hw, uint16_t hw2, bool wide,
                         uint32_t pc, uint32_t& next)
{
        if (execute_b(hw, hw2, wide, pc, next))
        return;
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
