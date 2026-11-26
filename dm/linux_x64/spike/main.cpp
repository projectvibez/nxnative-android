// Phase 0 spike for the host on Linux on x86_64 (Steam Deck): see README.md and docs/plan_linux_x64.md.
// A guest arm64 function runs under dynarmic on this x86_64 host; the guest computes fib(10), writes a message
// and executes `svc #0`; the host answers from its own side (a stand-in for the HLE service dispatch), the guest
// checks back, and stops at `brk #0`. Every later phase builds on this round trip.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "dynarmic/interface/A64/a64.h"

namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using Vector = std::array<u64, 2>;

constexpr u64 CODE = 0x0000'1000, DATA = 0x0001'0000;
constexpr u64 FIB_ADDR = DATA + 0x00, MSG_ADDR = DATA + 0x20, SUM_ADDR = DATA + 0x30, FINAL_ADDR = DATA + 0x40;
constexpr std::size_t MEM_SIZE = 0x0002'0000;
constexpr char MESSAGE[4] = {'D', 'M', 'H', 'L'};

// A tiny A64 encoder: only the instructions the spike needs, encodings from the ARM ARM.
struct Asm {
    std::vector<u32> w;
    std::unordered_map<std::string, size_t> labels;
    std::vector<std::pair<size_t, std::string>> fixups;

    size_t here() const { return w.size(); }
    void emit(u32 i) { w.push_back(i); }
    void label(const char* s) { labels.emplace(s, here()); }
    bool fixup()
    {
        for (auto& [at, name] : fixups) {
            auto it = labels.find(name);
            if (it == labels.end()) return false;
            std::int64_t off = static_cast<std::int64_t>(it->second) - static_cast<std::int64_t>(at);
            w[at] = 0x5400'0000u | ((static_cast<u32>(off) & 0x7FFFF) << 5) | 0xB;   // B.cond: imm19 word offset, cond=LT
        }
        return true;
    }
    void movz(int rd, u16 imm) { emit(0xD280'0000u | (u32(imm) << 5) | u32(rd)); }
    void movzw(int rd, u16 imm) { emit(0x5280'0000u | (u32(imm) << 5) | u32(rd)); }
    void movkw(int rd, u16 imm, int hw) { emit(0x72A0'0000u | (u32(hw) << 21) | (u32(imm) << 5) | u32(rd)); }
    void orr_xzr(int rd, int rm) { emit(0xAA00'03E0u | (u32(rm) << 16) | u32(rd)); }   // MOV (register)
    void add_reg(int rd, int rn, int rm) { emit(0x8B00'0000u | (u32(rm) << 16) | (u32(rn) << 5) | u32(rd)); }
    void add_imm(int rd, int rn, u16 imm12) { emit(0x9100'0000u | (u32(imm12) << 10) | (u32(rn) << 5) | u32(rd)); }
    void cmp(int rn, int rm) { emit(0xEB00'0000u | (u32(rm) << 16) | (u32(rn) << 5) | 31u); }   // SUBS XZR, Xn, Xm
    void str64(int rt, int rn, u16 imm12) { emit(0xF900'0000u | ((u32(imm12) / 8) << 10) | (u32(rn) << 5) | u32(rt)); }
    void ldr64(int rt, int rn, u16 imm12) { emit(0xF940'0000u | ((u32(imm12) / 8) << 10) | (u32(rn) << 5) | u32(rt)); }
    void str32(int rt, int rn, u16 imm12) { emit(0xB900'0000u | ((u32(imm12) / 4) << 10) | (u32(rn) << 5) | u32(rt)); }
    void b_lt(const char* target) { fixups.emplace_back(here(), target); emit(0); }
    void svc(u16 swi) { emit(0xD400'0001u | (u32(swi) << 5)); }
    void brk(u16 imm) { emit(0xD420'0000u | (u32(imm) << 5)); }
};

u64 le_load(const std::array<u8, MEM_SIZE>& m, u64 vaddr, size_t n)
{
    u64 v = 0;
    for (size_t i = 0; i < n; i++) v |= u64(m[vaddr + i]) << (8 * i);
    return v;
}

class FlatEnv final : public Dynarmic::A64::UserCallbacks {
public:
    std::array<u8, MEM_SIZE> mem{};
    Dynarmic::A64::Jit* cpu = nullptr;
    bool svc_seen = false, breakpoint_seen = false, fallback_seen = false;

    static bool in_code(u64 vaddr) { return vaddr >= CODE && vaddr < CODE + 0x1000; }

    std::optional<u32> MemoryReadCode(u64 vaddr) override
    {
        if (!in_code(vaddr) || vaddr + 4 > MEM_SIZE) return std::nullopt;
        return u32(le_load(mem, vaddr, 4));
    }
    u8 MemoryRead8(u64 vaddr) override { return vaddr < MEM_SIZE ? mem[vaddr] : 0; }
    u16 MemoryRead16(u64 vaddr) override { return u16(le_load(mem, vaddr, 2)); }
    u32 MemoryRead32(u64 vaddr) override { return u32(le_load(mem, vaddr, 4)); }
    u64 MemoryRead64(u64 vaddr) override { return le_load(mem, vaddr, 8); }
    Vector MemoryRead128(u64 vaddr) override { return {MemoryRead64(vaddr), MemoryRead64(vaddr + 8)}; }

    void MemoryWrite8(u64 vaddr, u8 v) override { if (vaddr < MEM_SIZE) mem[vaddr] = v; }
    void MemoryWrite16(u64 vaddr, u16 v) override { for (int i = 0; i < 2; i++) MemoryWrite8(vaddr + i, u8(v >> (8 * i))); }
    void MemoryWrite32(u64 vaddr, u32 v) override { for (int i = 0; i < 4; i++) MemoryWrite8(vaddr + i, u8(v >> (8 * i))); }
    void MemoryWrite64(u64 vaddr, u64 v) override { for (int i = 0; i < 8; i++) MemoryWrite8(vaddr + i, u8(v >> (8 * i))); }
    void MemoryWrite128(u64 vaddr, Vector v) override { MemoryWrite64(vaddr, v[0]); MemoryWrite64(vaddr + 8, v[1]); }

    bool MemoryWriteExclusive8(u64 vaddr, u8 v, u8) override { MemoryWrite8(vaddr, v); return true; }
    bool MemoryWriteExclusive16(u64 vaddr, u16 v, u16) override { MemoryWrite16(vaddr, v); return true; }
    bool MemoryWriteExclusive32(u64 vaddr, u32 v, u32) override { MemoryWrite32(vaddr, v); return true; }
    bool MemoryWriteExclusive64(u64 vaddr, u64 v, u64) override { MemoryWrite64(vaddr, v); return true; }
    bool MemoryWriteExclusive128(u64 vaddr, Vector v, Vector) override { MemoryWrite128(vaddr, v); return true; }

    void InterpreterFallback(u64 pc, size_t) override
    {
        std::fprintf(stderr, "[RED] InterpreterFallback at pc=0x%llx\n", static_cast<unsigned long long>(pc));
        fallback_seen = true;
        if (cpu) cpu->HaltExecution();
    }

    void CallSVC(u32 swi) override
    {
        std::fprintf(stderr, "host: svc #%u answered\n", swi);
        svc_seen = true;
        if (std::memcmp(mem.data() + MSG_ADDR, MESSAGE, sizeof MESSAGE) != 0) {
            std::fprintf(stderr, "[RED] the guest's message at the svc is not 'DMHL'\n");
            return;
        }
        u64 sum = 0;
        for (size_t i = 0; i < sizeof MESSAGE; i++) sum += mem[MSG_ADDR + i];
        for (size_t i = 0; i < 8; i++) MemoryWrite8(SUM_ADDR + i, u8(sum >> (8 * i)));   // the host writes back the checksum
    }

    void ExceptionRaised(u64 pc, Dynarmic::A64::Exception e) override
    {
        if (e == Dynarmic::A64::Exception::Breakpoint) {
            breakpoint_seen = true;
        } else {
            std::fprintf(stderr, "[RED] exception %d at pc=0x%llx\n", static_cast<int>(e), static_cast<unsigned long long>(pc));
        }
        if (cpu) cpu->HaltExecution();
    }

    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1'000'000; }
    u64 GetCNTPCT() override { return 0; }
};

std::vector<u32> guest_code()
{
    Asm a;
    a.movz(0, 10);            // x0 = n = 10
    a.movz(1, 0);             // x1 = a = fib(0)
    a.movz(2, 1);             // x2 = b = fib(1)
    a.movz(3, 0);             // x3 = i
    a.label("loop");
    a.add_reg(4, 1, 2);       //   x4 = a + b
    a.orr_xzr(1, 2);          //   a = b
    a.orr_xzr(2, 4);          //   b = c
    a.add_imm(3, 3, 1);       //   i++
    a.cmp(3, 0);              //   i < n ?
    a.b_lt("loop");
    a.str64(1, 9, FIB_ADDR - DATA);         // fib(10) -> x9 points at DATA (set by the host)
    a.movzw(4, 0x4D44);                     // 'D','M' in the low half...
    a.movkw(4, 0x4C48, 1);                  // ...and 'H','L' in the high half
    a.str32(4, 9, u16(MSG_ADDR - DATA));    // four bytes: "DMHL"
    a.svc(0);                               // the host answers the service call
    a.ldr64(5, 9, u16(SUM_ADDR - DATA));    // the checksum the host wrote
    a.add_reg(0, 1, 5);                     // x0 = fib(10) + checksum
    a.str64(0, 9, u16(FINAL_ADDR - DATA));
    a.brk(0);                               // halt: ExceptionRaised(Breakpoint)
    if (!a.fixup()) std::fprintf(stderr, "[RED] assembler fixup failed\n");
    return a.w;
}

}   // namespace

int main()
{
    FlatEnv env;
    const std::vector<u32> code = guest_code();
    if (code.empty()) return 2;
    std::memcpy(env.mem.data() + CODE, code.data(), code.size() * sizeof(u32));

    Dynarmic::A64::UserConfig config;
    config.callbacks = &env;
    config.enable_cycle_counting = false;   // AddTicks/GetTicksRemaining stay unused
    config.wall_clock_cntpct = true;        // GetCNTPCT is a wall clock here
    Dynarmic::A64::Jit cpu(config);
    env.cpu = &cpu;

    cpu.SetPC(CODE);
    cpu.SetRegister(0, 10);
    cpu.SetRegister(9, DATA);
    cpu.Run();

    const u64 fib = le_load(env.mem, FIB_ADDR, 8);
    const u64 final = le_load(env.mem, FINAL_ADDR, 8);
    const u64 sum = le_load(env.mem, SUM_ADDR, 8);

    int failures = 0;
    auto check = [&failures](const char* what, bool ok) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) failures++;
    };
    check("the guest's brk halted the jit", env.breakpoint_seen);
    check("the guest's svc reached the host", env.svc_seen);
    check("no interpreter fallback", !env.fallback_seen);
    check("the guest computed fib(10) == 55", fib == 55);
    check("the host wrote the checksum of 'DMHL' (0x125)", sum == 0x125);
    check("the guest read the checksum back and stored 55 + 0x125 == 0x15C", final == 0x15C);

    std::printf("spike: %s (%d failure%s)\n", failures == 0 ? "all checks passed" : "FAILURES", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
