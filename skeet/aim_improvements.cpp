#include "pch.h"
#include "aim_improvements.h"
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <intrin.h>
#include "skCrypter.h"

// ===========================================================================
// Shared hook installer (6-byte push+ret)
// ===========================================================================
static void InstallHook6(uint32_t target, uint32_t replacement)
{
    DWORD oldProtect = 0;
    if (!VirtualProtect((void*)target, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) return;
    *(uint8_t*)(target + 0) = 0x68;
    *(uint32_t*)(target + 1) = replacement;
    *(uint8_t*)(target + 5) = 0xC3;
    VirtualProtect((void*)target, 6, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (void*)target, 6);
    LPRINT(skCrypt("[INFO] aim improvement installed\n"));
}

// ===========================================================================
// [1] sub_18AE6 — angle wrap (0x43428AE6)
// ===========================================================================
void __fastcall Improved_18AE6(float* angles)
{
    if (!angles) return;

    float p = angles[0];
    float y = angles[1];
    float r = angles[2];

    if (std::isnan(p)) p = 0.0f;
    if (std::isnan(y)) y = 0.0f;
    if (std::isnan(r)) r = 0.0f;

    angles[0] = std::remainderf(p, 360.0f);
    angles[1] = std::remainderf(y, 360.0f);
    angles[2] = std::remainderf(r, 360.0f);   // roll is used by roll AA
}

// ===========================================================================
// [2] sub_181C0 — accuracy penalty decay (0x434281C0)
// ===========================================================================
static uint32_t g_scale_node = 0;
static uint32_t g_scale_frame = 0;

void __fastcall Improved_181C0(int* ctx)
{
    if (!ctx) return;

    void* state = *(void**)0x43468994;
    if (!state) return;

    *(uint32_t*)((char*)state + 8) = 0;

    int* lp = *(int**)0x43475808;
    if (!lp) return;

    uint32_t so = *(uint32_t*)0x43469218 ^
        **(uint32_t**)0x43468248 ^
        0xFBE8C5EA;
    if (ctx[10] == 100 || *(uint32_t*)(so + (uint32_t)lp) == 45) return;

    if (ctx[10] == 0) {
        *(float*)((char*)state + 8) = 1.0f;
        return;
    }

    uint32_t vtable = *(uint32_t*)lp;

    using GetFloatFn = float(__fastcall*)(void*);
    using GetIntFn = int(__fastcall*)(void*);

    float current = ((GetFloatFn)(*(uint32_t*)(0x530 + vtable)))((void*)lp);
    int   reference = ((GetIntFn)(*(uint32_t*)(0x534 + vtable)))((void*)lp);

    if (std::isnan(current)) return;

    int current_i = (int)(current + 0.5f);

    if (current_i == reference) return;

    int ref_safe = (reference == 0) ? 1 : reference;
    int delta = current_i - ref_safe;

    float decayed;
    if (delta != 0) {
        decayed = (float)(ref_safe
            + (int)((float)ctx[10] * *(float*)0x4346F908
                * (float)delta + 0.5f));
    }
    else {
        decayed = current;
    }

    uint32_t now = *(uint32_t*)0x4346A334;
    if (g_scale_frame != now || !g_scale_node) {
        uint32_t node = *(uint32_t*)0x43467A48 ^ 0x1EEEF408;
        uint32_t guard = 0;
        while (node != *(uint32_t*)(node + 28) && guard++ < 64)
            node = *(uint32_t*)(node + 28);
        g_scale_node = node;
        g_scale_frame = now;
    }

    float weapon_scale = *(float*)(g_scale_node + 44);

    double mult = ((double)decayed / (double)ref_safe) * (double)weapon_scale;
    *(float*)((char*)state + 8) = (float)mult;
}

// ===========================================================================
// [3] sub_110530 — angle jitter accumulator (0x43420530)
// ===========================================================================
float* __fastcall Improved_110530(void* self, float* out_sum, float* out_count)
{
    if (!self || !out_sum || !out_count) return out_count;

    uint32_t count = *(uint32_t*)((char*)self + 4);
    if (count <= 1) return out_count;

    uint32_t base = *(uint32_t*)self;
    float sum = *out_sum;
    float cnt = *out_count;
    int   prev = 0;

    for (uint32_t i = 1; i < count; ++i) {
        int sa = (prev + base) % 6;
        int sb = (i + base) % 6;

        float a = *(float*)((char*)self + 32 * sa + 36);
        float b = *(float*)((char*)self + 32 * sb + 36);

        if (std::isnan(a) || std::isnan(b)) { prev = i; continue; }

        float delta = std::remainderf(a - b, 360.0f);
        sum += std::fabs(delta);
        cnt += 1.0f;
        prev = i;
    }

    *out_sum = sum;
    *out_count = cnt;
    return out_count;
}

// ===========================================================================
// [4] sub_19D2B — max-damage tracer (0x43429D2B)
// ===========================================================================
constexpr uint32_t kAddr_19D2B = 0x43429D2B;
constexpr uint32_t kPrologue_19D2B = 12;
static uint8_t  g_19D2B_prologue[kPrologue_19D2B] = {};
static uint8_t* g_19D2B_trampoline = nullptr;

extern "C" int __fastcall Hooked_19D2B(int a1, float* eye, float* target,
    void* trace_result, void* ctx)
{
    using OrigFn = int(__fastcall*)(int, float*, float*, void*, void*);
    auto orig = (OrigFn)g_19D2B_trampoline;

    int rc = orig(a1, eye, target, trace_result, ctx);

    if (rc == 0 && ctx) {
        float dmg = *(float*)((char*)ctx + 8);   // out_damage
        if (dmg > 0.0f && dmg < 1.0f) {
            rc = (int)(dmg * 1000.0f);
        }
    }
    return rc;
}

static void InstallImpactTracer()
{
    if (g_19D2B_trampoline) return;

    memcpy(g_19D2B_prologue, (void*)kAddr_19D2B, kPrologue_19D2B);
    g_19D2B_trampoline = (uint8_t*)VirtualAlloc(nullptr, 32,
        MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!g_19D2B_trampoline) return;

    memcpy(g_19D2B_trampoline, g_19D2B_prologue, kPrologue_19D2B);
    uint8_t* jmp = g_19D2B_trampoline + kPrologue_19D2B;
    jmp[0] = 0xE9;
    *(int32_t*)(jmp + 1) = (int32_t)((kAddr_19D2B + kPrologue_19D2B)
        - (uint32_t)jmp - 5);
    FlushInstructionCache(GetCurrentProcess(), g_19D2B_trampoline, 17);

    DWORD old = 0;
    VirtualProtect((void*)kAddr_19D2B, kPrologue_19D2B,
        PAGE_EXECUTE_READWRITE, &old);
    uint8_t* patch = (uint8_t*)kAddr_19D2B;
    patch[0] = 0xE9;
    *(int32_t*)(patch + 1) = (int32_t)((uint32_t)&Hooked_19D2B
        - kAddr_19D2B - 5);
    for (uint32_t i = 5; i < kPrologue_19D2B; ++i) patch[i] = 0x90;
    VirtualProtect((void*)kAddr_19D2B, kPrologue_19D2B, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)kAddr_19D2B, kPrologue_19D2B);

    LPRINT(skCrypt("[INFO] max-damage tracer installed\n"));
}

// ===========================================================================
// [5] sub_276C3 — penetration sim B (0x435976C3)
// ===========================================================================
constexpr int kPenB_MaxSteps = 8;

static int g_penB_last = 0;
static int g_penB_best = 0;

extern "C" int __cdecl PenB_Impl(int info_ptr, int entity, float angle, int flags_obj)
{
    if (!info_ptr || !entity || !flags_obj) return 0;
    if (std::isnan(angle)) return 0;

    uint32_t vtable = *(uint32_t*)entity;
    if (!vtable) return 0;

    int* pos = (int*)(*(int(__fastcall**)(int))(vtable + 40))(entity);
    if (!pos) return 0;
    int default_result = pos[0];

    const uint32_t weapon_flags = *(uint32_t*)(flags_obj + 48);
    const uint32_t pos_off = *(uint32_t*)0x434688CC ^
        **(uint32_t**)0x4346A8E4 ^
        0x36375C23;
    const uint32_t time_off = *(uint32_t*)0x43468C24 ^
        **(uint32_t**)0x434688D0 ^
        0xE2FBE695;

    const bool weapon_ok = (weapon_flags & 0x618) != 0;
    const bool sentinel_ok = *(uint32_t*)(info_ptr + 8) != 0x7F7FFFFF;
    const float vel_sq = *(float*)(info_ptr + 36) * *(float*)(info_ptr + 36)
        + *(float*)(info_ptr + 40) * *(float*)(info_ptr + 40);
    const bool velocity_ok = vel_sq > *(float*)0x434724A8;

    const float e_x = *(float*)(pos_off + entity);
    const float e_y = *(float*)(pos_off + entity + 4);
    const float e_z = *(float*)(pos_off + entity + 8);
    const bool far_ok = (e_x * e_x + e_y * e_y + e_z * e_z) > *(float*)0x4346DD40;

    const bool fast_target = weapon_ok && sentinel_ok && velocity_ok;
    const bool far_target = far_ok && sentinel_ok;
    if (!fast_target && !far_target) return default_result;

    struct SimState {
        uint8_t  info[100];
        uint32_t angle_dword;
        uint8_t  pad[12];
    } sim = {};
    memcpy(sim.info, (void*)info_ptr, 100);
    sim.angle_dword = *(uint32_t*)&angle;

    // Engine bullet-fire helper (sub_A0D5C at 0xA0D5C offset).
    using FireBulletFn = void(__cdecl*)(int, int, int, int,
        float, float, float, int, int);
    ((FireBulletFn)0x433B0D5C)(
        entity,
        pos[0], pos[1], pos[2],
        e_x, e_y, e_z,
        *(int*)(time_off + entity),
        0);

    // Extended step loop with early-exit on convergence.
    uint8_t scratch[8] = {};
    int best = 0, prev = -1, stable = 0;

    for (int i = 0; i < kPenB_MaxSteps; ++i) {
        ((void(__thiscall*)(void*, int, void*))0x433DC606)(scratch, entity, &sim);

        const int rc = scratch[0]
            | (scratch[1] << 8)
            | (scratch[2] << 16)
            | (scratch[3] << 24);
        if (rc > best) best = rc;

        if (rc == prev) { if (++stable >= 2) break; }
        else { stable = 0; prev = rc; }
    }

    g_penB_last = best;
    if (best > g_penB_best) g_penB_best = best;
    return best;
}

extern "C" __declspec(naked) void Naked_276C3()
{
    __asm {
        // Entry: EDX = info_ptr, ECX = entity, XMM0 = angle, [esp+4] = flags_obj
        push    ebp
        mov     ebp, esp
        push    ebx
        push    edi
        sub     esp, 16

        movss   dword ptr[ebp - 12], xmm0      // angle
        mov     dword ptr[ebp - 16], edx       // info_ptr
        mov     dword ptr[ebp - 20], ecx       // entity

        push    dword ptr[ebp + 8]             // flags_obj
        push    dword ptr[ebp - 12]            // angle
        push    dword ptr[ebp - 20]            // entity
        push    dword ptr[ebp - 16]            // info_ptr
        call    PenB_Impl
        add     esp, 16

        movd    xmm0, eax
        pop     edi
        pop     ebx
        mov     esp, ebp
        pop     ebp
        ret     4
    }
}

// ===========================================================================
// [6] sub_EE04 — multi-iteration penetration (0x433FEE04)
// ===========================================================================
constexpr uint32_t kAddr_EE04 = 0x433FEE04;
constexpr uint32_t kPrologue_EE04 = 12;
static uint8_t  g_EE04_prologue[kPrologue_EE04] = {};
static uint8_t* g_EE04_trampoline = nullptr;

extern "C" int __fastcall Hooked_EE04(int ctx, int* target, int* descriptor)
{
    if (!descriptor) return 0;

    using OrigFn = int(__fastcall*)(int, int*, int*);
    auto orig = (OrigFn)g_EE04_trampoline;
    return orig(ctx, target, descriptor);
}

static void InstallPenEE04()
{
    if (g_EE04_trampoline) return;

    memcpy(g_EE04_prologue, (void*)kAddr_EE04, kPrologue_EE04);
    g_EE04_trampoline = (uint8_t*)VirtualAlloc(nullptr, 32,
        MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!g_EE04_trampoline) return;

    memcpy(g_EE04_trampoline, g_EE04_prologue, kPrologue_EE04);
    uint8_t* jmp = g_EE04_trampoline + kPrologue_EE04;
    jmp[0] = 0xE9;
    *(int32_t*)(jmp + 1) = (int32_t)((kAddr_EE04 + kPrologue_EE04)
        - (uint32_t)jmp - 5);
    FlushInstructionCache(GetCurrentProcess(), g_EE04_trampoline, 17);

    DWORD old = 0;
    VirtualProtect((void*)kAddr_EE04, kPrologue_EE04,
        PAGE_EXECUTE_READWRITE, &old);
    uint8_t* patch = (uint8_t*)kAddr_EE04;
    patch[0] = 0xE9;
    *(int32_t*)(patch + 1) = (int32_t)((uint32_t)&Hooked_EE04
        - kAddr_EE04 - 5);
    for (uint32_t i = 5; i < kPrologue_EE04; ++i) patch[i] = 0x90;
    VirtualProtect((void*)kAddr_EE04, kPrologue_EE04, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)kAddr_EE04, kPrologue_EE04);

    LPRINT(skCrypt("[INFO] penetration EE04 installed\n"));
}

// ===========================================================================
// [7] sub_10FE22 — 3D trajectory tracer (0x4341FE22)
// ===========================================================================
constexpr uint32_t kAddr_10FE22 = 0x4341FE22;
constexpr uint32_t kPrologue_10FE22 = 12;
static uint8_t  g_10FE22_prologue[kPrologue_10FE22] = {};
static uint8_t* g_10FE22_trampoline = nullptr;

extern "C" void __cdecl Tracer_Impl(float angle_x, int* m_a, int* m_b, int color)
{
    if (std::isnan(angle_x)) return;
    if (!m_a || !m_b) return;

    using OrigFn = void(__cdecl*)(float, int*, int*, int);
    ((OrigFn)g_10FE22_trampoline)(angle_x, m_a, m_b, color);
}

__declspec(naked) void Naked_10FE22()
{
    __asm {
        // Entry: EDX = angle_x bits, EDI = m_a, ESI = m_b, [esp+4] = color
        push    ebp
        mov     ebp, esp
        push    dword ptr[ebp + 8]             // color
        push    esi                             // m_b
        push    edi                             // m_a
        sub     esp, 4
        mov     dword ptr[esp], edx            // angle_x
        call    Tracer_Impl
        add     esp, 16
        pop     ebp
        ret     4
    }
}

static void InstallTracer_10FE22()
{
    if (g_10FE22_trampoline) return;

    memcpy(g_10FE22_prologue, (void*)kAddr_10FE22, kPrologue_10FE22);
    g_10FE22_trampoline = (uint8_t*)VirtualAlloc(nullptr, 32,
        MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!g_10FE22_trampoline) return;

    memcpy(g_10FE22_trampoline, g_10FE22_prologue, kPrologue_10FE22);
    uint8_t* jmp = g_10FE22_trampoline + kPrologue_10FE22;
    jmp[0] = 0xE9;
    *(int32_t*)(jmp + 1) = (int32_t)((kAddr_10FE22 + kPrologue_10FE22)
        - (uint32_t)jmp - 5);
    FlushInstructionCache(GetCurrentProcess(), g_10FE22_trampoline, 17);

    DWORD old = 0;
    VirtualProtect((void*)kAddr_10FE22, kPrologue_10FE22,
        PAGE_EXECUTE_READWRITE, &old);
    uint8_t* patch = (uint8_t*)kAddr_10FE22;
    patch[0] = 0xE9;
    *(int32_t*)(patch + 1) = (int32_t)((uint32_t)&Naked_10FE22
        - kAddr_10FE22 - 5);
    for (uint32_t i = 5; i < kPrologue_10FE22; ++i) patch[i] = 0x90;
    VirtualProtect((void*)kAddr_10FE22, kPrologue_10FE22, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)kAddr_10FE22, kPrologue_10FE22);

    LPRINT(skCrypt("[INFO] tracer 10FE22 installed\n"));
}

// ===========================================================================
// Master installer
// ===========================================================================
void InstallAimImprovements()
{
    // Simple 6-byte hooks
   // InstallHook6(0x43428AE6, (uint32_t)&Improved_18AE6);   // angle wrap
    InstallHook6(0x434281C0, (uint32_t)&Improved_181C0);   // accuracy decay
    InstallHook6(0x43420530, (uint32_t)&Improved_110530);  // jitter accumulator

    // Trampoline / naked hooks
    InstallImpactTracer();                                 // 19D2B
   // InstallPenEE04();                                      // EE04
   InstallHook6(0x435976C3, (uint32_t)&Naked_276C3);      // pen B
    InstallTracer_10FE22();                                // 10FE22
}