#include "pch.h"
#include "skeet_patches.h"
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <intrin.h>
#include "skCrypter.h"

// ===========================================================================
// Runtime-resolved function pointers into the mapped binary
// ===========================================================================
static auto sub_8CF74 = reinterpret_cast<int (*)(void*)>(0x43310000u + 0x08CF74);
static auto sub_DC30F = reinterpret_cast<int (*)()>   (0x43310000u + 0x0DC30F);

// ===========================================================================
// Common hook installer — 6-byte push+ret
// ===========================================================================
static void InstallHook(uint32_t target, uint32_t replacement, const char* /*name*/)
{
    DWORD oldProtect = 0;
    if (!VirtualProtect((void*)target, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) return;
    *(uint8_t*)(target + 0) = 0x68;
    *(uint32_t*)(target + 1) = replacement;
    *(uint8_t*)(target + 5) = 0xC3;
    VirtualProtect((void*)target, 6, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (void*)target, 6);
    LPRINT(skCrypt("[INFO] patch installed\n"));
}

 

//int __fastcall Hooked_F0F87(int a3, int /*edx_unused*/, int a6, int a7)
/*{
    if (!*(uint32_t*)0x4346754C) return 0;
    uint8_t* ctx = (uint8_t*)a3;
    if (*(void**)(ctx + 76) == *(void**)(ctx + 80)) return 0;
    if (*(void**)(ctx + 224) == *(void**)(ctx + 228)) return 0;
    uint32_t* it = *(uint32_t**)(ctx + 224);
    uint32_t* end = *(uint32_t**)(ctx + 228);
    while (*it) { it += 8; if (it == end) return 0; }
    void* a6_obj = (void*)a6;
    MatchFn matcher = *(MatchFn*)(*(uint32_t*)a6_obj + 76);
    MatchRequest requests[4] = {
        { kMatchSignature, kCriteria_5Arg, 5, 0 },
        { kMatchSignature, kCriteria_A,    1, 0 },
        { kMatchSignature, kCriteria_B,    1, 0 },
        { kMatchSignature, kCriteria_C,    1, 0 },
    };
    for (auto& r : requests) { if (matcher(a6_obj, &r)) return (int)r.result; }
    uint32_t obj_a = *(uint32_t*)(ctx + 52);
    int probe_a = (*(int(__thiscall**)(uint32_t, int))(*(uint32_t*)obj_a + 36))(obj_a, 0);
    int probe_b = (*(int(__fastcall**)(int))(*(uint32_t*)(a7 + 8) + 40))(a7 + 8);
    if (probe_a != probe_b) return 0;
    struct MatchResult { uint32_t flag, r2, r3, r4; };
    MatchResult mr = { 1, requests[1].result, requests[2].result, requests[3].result };
    return sub_8CF74(&mr);
}*/

// ===========================================================================
// [3] sub_ED134 — Float quantizer (0x433FD134)
// ===========================================================================
constexpr float kPrecisionLadder[5] = { 1.0f, 0.5f, 0.25f, 0.125f, 0.0625f };
constexpr float kMinSignificantRange = 1e-6f;

/*extern "C" float __cdecl QuantizeFloat_Impl(int bits, float value, float lo, float hi)
{
    if (bits <= 0) return lo;
    if (bits > 32) bits = 32;
    const uint32_t max_code = (bits == 32) ? 0xFFFFFFFEu : ((1u << bits) - 1u);
    const int32_t  max_code_signed = (bits == 32) ? -2 : (int32_t)max_code;
    const double   bias = (max_code_signed < 0) ? -0.5 : +0.5;
    const float    range_f = hi - lo;
    const double   range_d = (double)range_f;
    const double   level_count = (double)max_code_signed + bias;
    const bool     significant = (std::fabs(range_f) > kMinSignificantRange);
    float          inv_step = (float)(significant ? (level_count / range_d) : level_count);
    if (sub_DC30F() > (int)max_code) {
        bool found = false;
        for (int i = 0; i < 5; ++i) {
            inv_step = kPrecisionLadder[i] * (float)(level_count / range_d);
            if (sub_DC30F() <= (int)max_code) { found = true; break; }
        }
        if (!found) return lo;
    }
    if (lo > value) return lo;
    if (value > hi) return hi;
    int64_t truncated = (int64_t)((value - lo) * inv_step);
    if (truncated > (int64_t)max_code_signed) truncated = max_code_signed;
    if (truncated < 0) truncated = 0;
    const double bias2 = (truncated < 0) ? -0.5 : +0.5;
    return (float)(((double)truncated + bias2) / inv_step) + lo;
}

__declspec(naked) void Naked_ED134()
{
    __asm {
        sub     esp, 12
        movss   dword ptr[esp + 0], xmm0
        movss   dword ptr[esp + 4], xmm1
        movss   dword ptr[esp + 8], xmm2
        push    dword ptr[esp + 8]
        push    dword ptr[esp + 8]
        push    dword ptr[esp + 8]
        push    ecx
        call    QuantizeFloat_Impl
        add     esp, 28
        ret
    }
}
*/
// ===========================================================================
// [4] sub_108772 — Bullet raycast (0x43418772)
// ===========================================================================
constexpr float kMinCapsuleRadius = 0.01f;
constexpr float kDegenerateLength = 1e-6f;
constexpr float kMinDirectionLen = 1e-6f;
constexpr float kPointInsideEps = 1e-4f;
constexpr float kDiscTangentEps = 1e-8f;
constexpr float kAParallelEps = 1e-8f;
constexpr float kFloatMax = 3.402823466e+38f;

struct RayHit { float t; float point[3]; float normal[3]; uint8_t pad[4]; uint8_t valid; uint8_t pad2[3]; };
static_assert(sizeof(RayHit) == 36, "RayHit layout");

static inline bool ContainsNaN(const float* p, int n)
{
    for (int i = 0; i < n; ++i) if (std::isnan(p[i])) return true;
    return false;
}

extern "C" void __cdecl Raycast_Impl(const float* origin, RayHit* out, float radius,
    const float* dir, const float* capsule)
{
    out->valid = 0; out->t = kFloatMax;
    if (ContainsNaN(origin, 3) || ContainsNaN(dir, 3) || ContainsNaN(capsule, 6) || std::isnan(radius)) return;

    const float p0x = capsule[0], p0y = capsule[1], p0z = capsule[2];
    const float p1x = capsule[3], p1y = capsule[4], p1z = capsule[5];
    const float seg_x = p1x - p0x, seg_y = p1y - p0y, seg_z = p1z - p0z;
    const float seg_len = std::sqrt(seg_x * seg_x + seg_y * seg_y + seg_z * seg_z);
    const float ox = origin[0] - p0x, oy = origin[1] - p0y, oz = origin[2] - p0z;

    if (radius < kMinCapsuleRadius) return;
    const float dir_len_sq = dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2];

    if (seg_len < kDegenerateLength) {
        const float origin_len_sq = ox * ox + oy * oy + oz * oz;
        if (dir_len_sq <= kMinDirectionLen) {
            out->t = 0.0f;
            out->point[0] = ox; out->point[1] = oy; out->point[2] = oz;
            out->normal[0] = 0; out->normal[1] = 0; out->normal[2] = 1;
            out->valid = 1; return;
        }
        const float proj = dir[0] * ox + dir[1] * oy + dir[2] * oz;
        const float b = -proj / dir_len_sq;
        const float c = (origin_len_sq - radius * radius) / dir_len_sq;
        const float disc = b * b - c;
        if (disc < 0.0f) return;
        float t = b - std::sqrt(disc);
        if (t < 0.0f) t = 0.0f;
        out->t = t;
        out->point[0] = origin[0] + dir[0] * t;
        out->point[1] = origin[1] + dir[1] * t;
        out->point[2] = origin[2] + dir[2] * t;
        const float inv = 1.0f / std::sqrt(origin_len_sq);
        out->normal[0] = ox * inv;
        out->normal[1] = oy * inv;
        out->normal[2] = oz * inv;
        out->valid = 1; return;
    }

    const float inv_seg = 1.0f / seg_len;
    const float ux = seg_x * inv_seg, uy = seg_y * inv_seg, uz = seg_z * inv_seg;

    float tangent[3], bitangent[3];
    if (std::fabs(ux) < 0.9f) { tangent[0] = 0;    tangent[1] = -uz;  tangent[2] = uy; }
    else { tangent[0] = -uz;  tangent[1] = 0;    tangent[2] = ux; }
    const float tlen = std::sqrt(tangent[0] * tangent[0] + tangent[1] * tangent[1] + tangent[2] * tangent[2]);
    const float inv_tlen = 1.0f / tlen;
    tangent[0] *= inv_tlen; tangent[1] *= inv_tlen; tangent[2] *= inv_tlen;
    bitangent[0] = uy * tangent[2] - uz * tangent[1];
    bitangent[1] = uz * tangent[0] - ux * tangent[2];
    bitangent[2] = ux * tangent[1] - uy * tangent[0];

    if (dir_len_sq <= kMinDirectionLen) {
        const float along = ox * ux + oy * uy + oz * uz;
        const float cl = (along < 0) ? 0.0f : (along > seg_len) ? seg_len : along;
        const float cx = p0x + ux * cl, cy = p0y + uy * cl, cz = p0z + uz * cl;
        const float dx = origin[0] - cx, dy = origin[1] - cy, dz = origin[2] - cz;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > radius * radius) return;
        out->t = 0.0f;
        out->point[0] = cx; out->point[1] = cy; out->point[2] = cz;
        out->valid = 1;
        if (d2 <= kPointInsideEps) {
            out->normal[0] = 0; out->normal[1] = 0; out->normal[2] = 1;
        }
        else {
            const float inv = 1.0f / std::sqrt(d2);
            out->normal[0] = dx * inv; out->normal[1] = dy * inv; out->normal[2] = dz * inv;
        }
        return;
    }

    const float ox_perp = ox * tangent[0] + oy * tangent[1] + oz * tangent[2];
    const float oy_perp = ox * bitangent[0] + oy * bitangent[1] + oz * bitangent[2];
    const float dx_perp = dir[0] * tangent[0] + dir[1] * tangent[1] + dir[2] * tangent[2];
    const float dy_perp = dir[0] * bitangent[0] + dir[1] * bitangent[1] + dir[2] * bitangent[2];

    const float A = dx_perp * dx_perp + dy_perp * dy_perp;
    const float B = 2.0f * (ox_perp * dx_perp + oy_perp * dy_perp);
    const float C = ox_perp * ox_perp + oy_perp * oy_perp - radius * radius;

    if (A < kAParallelEps) {
        const float lateral = std::sqrt(C + radius * radius);
        if (lateral > radius) return;
        const float d_proj = dir[0] * ux + dir[1] * uy + dir[2] * uz;
        const float o_proj = ox * ux + oy * uy + oz * uz;
        const float cl = (o_proj < 0) ? 0.0f : (o_proj > seg_len) ? seg_len : o_proj;
        const float t = (cl - o_proj) / (d_proj + (d_proj < 0 ? -kDegenerateLength : kDegenerateLength));
        out->t = (t < 0.0f) ? 0.0f : t;
        out->point[0] = origin[0] + dir[0] * out->t;
        out->point[1] = origin[1] + dir[1] * out->t;
        out->point[2] = origin[2] + dir[2] * out->t;
        const float inv_lat = 1.0f / lateral;
        out->normal[0] = ox_perp * inv_lat;
        out->normal[1] = oy_perp * inv_lat;
        out->normal[2] = 0.0f;
        out->valid = 1; return;
    }

    float disc = B * B - 4.0f * A * C;
    if (disc < 0.0f) { if (disc < -kDiscTangentEps) return; disc = 0.0f; }

    const float sqrt_disc = std::sqrt(disc);
    float t = (-B - sqrt_disc) / (2.0f * A);
    if (t < 0.0f) t = 0.0f;

    const float d_proj = dir[0] * ux + dir[1] * uy + dir[2] * uz;
    const float o_proj = ox * ux + oy * uy + oz * uz;
    const float axial = o_proj + t * d_proj;

    float cx, cy, cz;
    if (axial < 0.0f) { cx = p0x; cy = p0y; cz = p0z; }
    else if (axial > seg_len) { cx = p1x; cy = p1y; cz = p1z; }
    else { cx = p0x + ux * axial; cy = p0y + uy * axial; cz = p0z + uz * axial; }

    const float hx = origin[0] + dir[0] * t;
    const float hy = origin[1] + dir[1] * t;
    const float hz = origin[2] + dir[2] * t;
    float nx = hx - cx, ny = hy - cy, nz = hz - cz;
    float nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (nlen < kPointInsideEps) { nx = 0; ny = 0; nz = 1; nlen = 1.0f; }

    out->t = t;
    out->point[0] = hx; out->point[1] = hy; out->point[2] = hz;
    const float inv_n = 1.0f / nlen;
    out->normal[0] = nx * inv_n;
    out->normal[1] = ny * inv_n;
    out->normal[2] = nz * inv_n;
    out->valid = 1;
}

__declspec(naked) void Naked_108772()
{
    __asm {
        push    dword ptr[esp + 8]
        push    dword ptr[esp + 8]
        sub     esp, 4
        movss   dword ptr[esp], xmm0
        push    ecx
        push    edx
        call    Raycast_Impl
        add     esp, 20
        ret     8
    }
}
// ===========================================================================
// [5] sub_10DDA0 — Fake-angle bruteforce (0x4341DDA0)
// ===========================================================================
static constexpr float kOffsetTable[] = {
    +12.0f,  -12.0f,
    +37.0f,  -37.0f,
    +58.0f,  -58.0f,
    +90.0f,  -90.0f,
    +120.0f, -120.0f,
    +180.0f,
};
static constexpr int kOffsetCount = sizeof(kOffsetTable) / sizeof(kOffsetTable[0]);
static int g_offset_index = 0;

static inline float WrapYaw(float a)
{
    a = std::fmodf(a + 180.0f, 360.0f);
    if (a < 0.0f) a += 360.0f;
    return a - 180.0f;
}

static inline float NextOffset()
{
    float off = kOffsetTable[g_offset_index];
    g_offset_index = (g_offset_index + 1) % kOffsetCount;
    return off;
}

constexpr uint32_t kAimResolverAddr = 0x4341DDA0;
constexpr uint32_t kAimResolverSave = 6;

static uint8_t  g_aim_prologue[kAimResolverSave] = {};
static uint8_t* g_aim_trampoline = nullptr;
static bool     g_aim_hook_ready = false;

extern "C" int __fastcall AimBruteforce_Handler(int ctx, int player_handle,
    int prev_state, int full_update);

extern "C" __declspec(naked) void AimBruteforce_Trampoline()
{
    __asm {
        push    ebp
        mov     ebp, esp
        push    ebx
        push    edi
        push    esi

        push    dword ptr[ebp + 12]
        push    dword ptr[ebp + 8]
        call    AimBruteforce_Handler
        // No add esp — __fastcall is callee-cleanup

        pop     esi
        pop     edi
        pop     ebx
        pop     ebp
        ret     8
    }
}

extern "C" int __fastcall AimBruteforce_Handler(int ctx, int player_handle,
    int prev_state, int full_update)
{
    using OriginalFn = int(__fastcall*)(int, int, int, int);
    auto original = reinterpret_cast<OriginalFn>(g_aim_trampoline);
    int rc = original(ctx, player_handle, prev_state, full_update);

    if (rc != 1 && rc != 5 && rc != 7)
        return rc;

    const uint32_t view_off = *(uint32_t*)0x4346A74C ^
        **(uint32_t**)0x43468EA0 ^
        0xE23A9BE1;
    float* yaw_ptr = reinterpret_cast<float*>((uintptr_t)view_off + player_handle + 4);
    *yaw_ptr = WrapYaw(*yaw_ptr + NextOffset());
    return rc;
}

static void InstallAimBruteforceHook()
{
    if (g_aim_hook_ready) return;

    memcpy(g_aim_prologue, (void*)kAimResolverAddr, kAimResolverSave);

    g_aim_trampoline = (uint8_t*)VirtualAlloc(
        nullptr, 32, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!g_aim_trampoline) {
        LPRINT(skCrypt("[ERROR] aim bruteforce: trampoline alloc failed\n"));
        return;
    }

    memcpy(g_aim_trampoline, g_aim_prologue, kAimResolverSave);
    uint8_t* slot = g_aim_trampoline + kAimResolverSave;
    slot[0] = 0xE9;
    *(int32_t*)(slot + 1) = (int32_t)((kAimResolverAddr + kAimResolverSave)
        - (uint32_t)slot - 5);
    FlushInstructionCache(GetCurrentProcess(), g_aim_trampoline, kAimResolverSave + 5);

    DWORD oldProtect = 0;
    VirtualProtect((void*)kAimResolverAddr, kAimResolverSave,
        PAGE_EXECUTE_READWRITE, &oldProtect);

    uint8_t* patch = (uint8_t*)kAimResolverAddr;
    patch[0] = 0xE9;
    *(int32_t*)(patch + 1) = (int32_t)((uint32_t)&AimBruteforce_Trampoline
        - kAimResolverAddr - 5);
    for (uint32_t i = 5; i < kAimResolverSave; ++i) patch[i] = 0x90;

    VirtualProtect((void*)kAimResolverAddr, kAimResolverSave, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (void*)kAimResolverAddr, kAimResolverSave);

    g_aim_hook_ready = true;
    LPRINT(skCrypt("[INFO] aim bruteforce hook installed\n"));
}

// ===========================================================================
// [6] sub_110AD5 — Impact resolver with extended penetration (0x43420AD5)
// ===========================================================================
constexpr int kMaxImpactSteps = 8;

static int g_last_impact_result = 0;
static int g_best_impact_result = 0;

struct ImpactScratch {
    uint32_t field_0;
    uint32_t result;
    uint32_t field_8;
    uint32_t field_C;
    uint32_t pad[12];
};

using ImpactSetupFn = void(__cdecl*)(void*, int, int, int, int,
    float, float, float, int, int);
using ImpactStepFn = void(__cdecl*)(void*, int, void*);

extern "C" int __cdecl Impact_Impl(int info_ptr, int entity, float angle, int flags_obj)
{
    if (!info_ptr || !entity || !flags_obj) return 0;
    if (std::isnan(angle)) return 0;

    uint32_t vtable = *(uint32_t*)entity;
    if (!vtable) return 0;

    int* pos = (int*)(*(int(__fastcall**)(int))(vtable + 40))(entity);
    if (!pos) return 0;
    int default_result = pos[0];

    const uint32_t weapon_flags = *(uint32_t*)(flags_obj + 48);
    const uint32_t pos_off = *(uint32_t*)0x434688CC ^ **(uint32_t**)0x4346A8E4 ^ 0x36375C23;
    const uint32_t time_off = *(uint32_t*)0x43468C24 ^ **(uint32_t**)0x434688D0 ^ 0xE2FBE695;

    const bool weapon_ok = (weapon_flags & 0x618) != 0;
    const bool sentinel_ok = *(uint32_t*)(info_ptr + 8) != 0x7F7FFFFF;
    const float vel_sq =
        *(float*)(info_ptr + 36) * *(float*)(info_ptr + 36) +
        *(float*)(info_ptr + 40) * *(float*)(info_ptr + 40);
    const bool velocity_ok = vel_sq > *(float*)0x434724A8;

    const float e_x = *(float*)(pos_off + entity);
    const float e_y = *(float*)(pos_off + entity + 4);
    const float e_z = *(float*)(pos_off + entity + 8);
    const bool far_ok = (e_x * e_x + e_y * e_y + e_z * e_z) > *(float*)0x4346DD40;

    const bool fast_target = weapon_ok && sentinel_ok && velocity_ok;
    if (!fast_target && !far_ok)
        return default_result;

    struct SimState {
        uint8_t  info[100];
        uint32_t angle_dword;
        uint8_t  pad[12];
    } sim = {};

    memcpy(sim.info, (void*)info_ptr, 100);
    sim.angle_dword = *(uint32_t*)&angle;

    ImpactScratch scratch = {};
    ((ImpactSetupFn)0x07E40BEB)(
        &scratch,
        entity,
        pos[0], pos[1], pos[2],
        e_x, e_y, e_z,
        *(int*)(time_off + entity),
        0);

    int best = 0;
    int prev = -1;
    int stable = 0;

    for (int i = 0; i < kMaxImpactSteps; ++i)
    {
        ((ImpactStepFn)0x05330C04)(&scratch, entity, &sim);

        const int rc = (int)scratch.result;
        if (rc > best) best = rc;

        if (rc == prev) {
            if (++stable >= 2) break;
        }
        else {
            stable = 0;
            prev = rc;
        }
    }

    g_last_impact_result = best;
    if (best > g_best_impact_result)
        g_best_impact_result = best;

    return best;
}

__declspec(naked) void Naked_110AD5()
{
    __asm {
        push    ebp
        mov     ebp, esp
        push    ebx
        push    edi
        sub     esp, 16

        movss   dword ptr[ebp - 12], xmm0
        mov     dword ptr[ebp - 16], edx
        mov     dword ptr[ebp - 20], ecx

        push    dword ptr[ebp + 8]
        push    dword ptr[ebp - 12]
        push    dword ptr[ebp - 20]
        push    dword ptr[ebp - 16]
        call    Impact_Impl
        add     esp, 16

        movd    xmm0, eax

        pop     edi
        pop     ebx
        mov     esp, ebp
        pop     ebp
        ret     4
    }
}

static void InstallImpactHook()
{
    const uint32_t target = 0x43420AD5;
    const uint32_t replacement = (uint32_t)&Naked_110AD5;

    DWORD oldProtect = 0;
    if (!VirtualProtect((void*)target, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LPRINT(skCrypt("[ERROR] impact hook: VirtualProtect failed\n"));
        return;
    }

    *(uint8_t*)(target + 0) = 0x68;
    *(uint32_t*)(target + 1) = replacement;
    *(uint8_t*)(target + 5) = 0xC3;

    VirtualProtect((void*)target, 6, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (void*)target, 6);
    LPRINT(skCrypt("[INFO] impact resolver hook installed\n"));
}

// ===========================================================================
// [7] sub_110C41 — Hitbox damage reorder (0x43420C41)
// ===========================================================================
// Original: __userpurge(EDX=positions float*, ECX=entity*, XMM0/1/2=aim,
//                       [esp+4]=hitgroups int*) -> EAX
// Runs the original, then reorders the 6 output candidates so the highest-
// damage hitbox (head > stomach > chest > arms > legs) is always first.
// ===========================================================================
constexpr uint32_t kAddr_110C41 = 0x43420C41;
constexpr uint32_t kPrologueLen_110C41 = 12;   // verify in IDA

static uint8_t  g_110C41_prologue[kPrologueLen_110C41] = {};
static uint8_t* g_110C41_trampoline = nullptr;
static void* g_110C41_trampoline_fn = nullptr;
static bool     g_110C41_ready = false;

static constexpr float kHitgroupDamage[8] = {
    0.00f,   // 0: invalid / generic
    4.00f,   // 1: head
    1.00f,   // 2: chest
    1.25f,   // 3: stomach
    1.00f,   // 4: left arm
    1.00f,   // 5: right arm
    0.75f,   // 6: left leg
    0.75f,   // 7: right leg
};

extern "C" void __cdecl PostProcessHitboxes_110C41(float* positions, int* hitgroups)
{
    struct Cand { float x, y, z; int hg; float score; };
    Cand c[6];

    for (int i = 0; i < 6; ++i) {
        c[i].x = positions[3 * i + 0];
        c[i].y = positions[3 * i + 1];
        c[i].z = positions[3 * i + 2];
        c[i].hg = hitgroups[i];

        const int hg = c[i].hg & 7;
        c[i].score = kHitgroupDamage[hg];
        if (c[i].hg == 1) c[i].score += 0.001f;   // head tiebreaker
    }

    // Bubble sort descending by score
    for (int i = 0; i < 5; ++i)
        for (int j = i + 1; j < 6; ++j)
            if (c[j].score > c[i].score) {
                Cand t = c[i]; c[i] = c[j]; c[j] = t;
            }

    for (int i = 0; i < 6; ++i) {
        positions[3 * i + 0] = c[i].x;
        positions[3 * i + 1] = c[i].y;
        positions[3 * i + 2] = c[i].z;
        hitgroups[i] = c[i].hg;
    }
}

extern "C" __declspec(naked) void Naked_110C41()
{
    __asm {
        // Entry: EDX=positions, ECX=entity, XMM0/1/2=aim, [esp+4]=hitgroups
        push    ebp
        mov     ebp, esp
        push    ebx
        push    esi
        push    edi
        sub     esp, 64

        // Stash args
        mov     dword ptr[ebp - 4], edx
        mov     dword ptr[ebp - 8], ecx
        mov     eax, dword ptr[ebp + 8]
        mov     dword ptr[ebp - 12], eax
        movups  xmmword ptr[ebp - 32], xmm0
        movups  xmmword ptr[ebp - 48], xmm1
        movups  xmmword ptr[ebp - 64], xmm2

        // Restore args for the original
        mov     edx, dword ptr[ebp - 4]
        mov     ecx, dword ptr[ebp - 8]
        movups  xmm0, xmmword ptr[ebp - 32]
        movups  xmm1, xmmword ptr[ebp - 48]
        movups  xmm2, xmmword ptr[ebp - 64]
        push    dword ptr[ebp - 12]
        call    dword ptr[g_110C41_trampoline_fn]
        // Original's `ret 4` cleaned the a6 arg

        push    eax                                       // preserve return value

        push    dword ptr[ebp - 12]                      // hitgroups
        push    dword ptr[ebp - 4]                       // positions
        call    PostProcessHitboxes_110C41
        add     esp, 8

        pop     eax

        add     esp, 64
        pop     edi
        pop     esi
        pop     ebx
        pop     ebp
        ret     4
    }
}

static void InstallHitboxDamageReorder()
{
    if (g_110C41_ready) return;

    memcpy(g_110C41_prologue, (void*)kAddr_110C41, kPrologueLen_110C41);

    g_110C41_trampoline = (uint8_t*)VirtualAlloc(
        nullptr, 48, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!g_110C41_trampoline) {
        LPRINT(skCrypt("[ERROR] 110C41: trampoline alloc failed\n"));
        return;
    }

    memcpy(g_110C41_trampoline, g_110C41_prologue, kPrologueLen_110C41);

    uint8_t* jmp_slot = g_110C41_trampoline + kPrologueLen_110C41;
    jmp_slot[0] = 0xE9;
    *(int32_t*)(jmp_slot + 1) = (int32_t)((kAddr_110C41 + kPrologueLen_110C41)
        - (uint32_t)jmp_slot - 5);
    FlushInstructionCache(GetCurrentProcess(), g_110C41_trampoline,
        kPrologueLen_110C41 + 5);

    g_110C41_trampoline_fn = g_110C41_trampoline;

    DWORD oldProtect = 0;
    VirtualProtect((void*)kAddr_110C41, kPrologueLen_110C41,
        PAGE_EXECUTE_READWRITE, &oldProtect);   

    uint8_t* patch = (uint8_t*)kAddr_110C41;
    patch[0] = 0xE9;
    *(int32_t*)(patch + 1) = (int32_t)((uint32_t)&Naked_110C41
        - kAddr_110C41 - 5);
    for (uint32_t i = 5; i < kPrologueLen_110C41; ++i)
        patch[i] = 0x90;

    VirtualProtect((void*)kAddr_110C41, kPrologueLen_110C41,
        oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (void*)kAddr_110C41,
        kPrologueLen_110C41);

    g_110C41_ready = true;
    LPRINT(skCrypt("[INFO] hitbox damage reorder installed\n"));
}

// ===========================================================================
// Master installer
// ===========================================================================
void InstallAllPatches()
{
   // InstallHook(0x43407226, (uint32_t)&Hooked_F7226, "F7226");
    //InstallHook(0x43400F87, (uint32_t)&Hooked_F0F87, "F0F87");
    //InstallHook(0x433FD134, (uint32_t)&Naked_ED134, "ED134");
    InstallHook(0x43418772, (uint32_t)&Naked_108772, "108772");
   InstallAimBruteforceHook();
    InstallImpactHook();
    InstallHitboxDamageReorder();
}