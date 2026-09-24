#include <bit>
#include <unordered_map>
#include <cmath>
#include "pch.h"
#include "Binary/pattern.h"
#include "Binary/initializer.h"
#include "Binary/menu_fonts.h"
#include "Binary/hitbox.h"
#include "skeet_patches.h"
#include "VM/recompilation.h"
#include "ntstructs.h"
#include "Binary/mem_int3.h"
#include "Binary/menu.h"
#include <thread>
#include "skCrypter.h"
#include "resource.h"
#include "aim_improvements.h"

FILETIME ftime = {};

// ===========================================================================
// [TRACE UTILITY] Improved line-of-sight trace
// ===========================================================================
struct TraceVector {
    float x, y, z;
};

#pragma pack(push, 4)
struct CSRay {
    float  start[4];
    float  delta[4];
    float  start_offset[4];
    float  extents[4];
    void* world_axis_transform;
    uint8_t is_ray;
    uint8_t is_swept;
    uint8_t pad[2];
};
#pragma pack(pop)
static_assert(sizeof(CSRay) == 72, "CSRay layout mismatch");

struct CSTrace {
    uint8_t raw[84];

    float    fraction()    const { return *reinterpret_cast<const float*>(raw + 0x30); }
    int      contents()    const { return *reinterpret_cast<const int*>(raw + 0x34); }
    uint16_t disp_flags()  const { return *reinterpret_cast<const uint16_t*>(raw + 0x38); }
    uint8_t  all_solid()   const { return raw[0x3A]; }
    uint8_t  start_solid() const { return raw[0x3B]; }
};
static_assert(sizeof(CSTrace) == 84, "CSTrace layout mismatch");

constexpr uint32_t MASK_SHOT = 0x4600400B;

struct ITraceFilter {
    virtual bool ShouldHitEntity(void* entity, int contents_mask) = 0;
    virtual int  GetTraceType() const = 0;
    virtual ~ITraceFilter() {}
};

bool TraceLOS(const TraceVector& eye,
    const TraceVector& target,
    ITraceFilter* filter,
    CSTrace* out,
    uint32_t mask = MASK_SHOT,
    float    min_distance = 1.0f,
    float    max_distance = 8192.0f)
{
    if (!out)
        return false;

    float dx = target.x - eye.x;
    float dy = target.y - eye.y;
    float dz = target.z - eye.z;
    float dist_sq = dx * dx + dy * dy + dz * dz;
    float length = sqrtf(dist_sq);

    if (length < min_distance || length > max_distance)
        return false;

    CSRay ray = {};
    ray.start[0] = eye.x;
    ray.start[1] = eye.y;
    ray.start[2] = eye.z;
    ray.delta[0] = dx;
    ray.delta[1] = dy;
    ray.delta[2] = dz;
    ray.world_axis_transform = nullptr;
    ray.is_ray = 1;
    ray.is_swept = (dist_sq > 1e-6f);

    void* engine_trace = *(void**)0x43468304;
    if (!engine_trace)
        return false;

    using TraceRayFn = void(__thiscall*)(void*, CSRay*, uint32_t, void*, CSTrace*);
    void** vtable = *(void***)engine_trace;
    auto trace_ray = reinterpret_cast<TraceRayFn>(vtable[3]);
    if (!trace_ray)
        return false;

    trace_ray(engine_trace, &ray, mask, filter, out);

    if (out->start_solid() || out->all_solid())
        return false;

    float frac = out->fraction();
    if (std::isnan(frac) || frac < 0.0f)
        return false;

    return true;
}

// ===========================================================================
// [BACKTRACK HOOK] Improved backtrack position resolver (0x4340C148)
// ===========================================================================
float* __fastcall Backtrack_GetPosition(int player, int /*unused*/, float* out, int mgr)
{
    struct backtrack_manager {
        char pad_000[1492];
        int  current_index;
        int  num_records;
        void* ring[32];
    };

    auto* m = reinterpret_cast<backtrack_manager*>(mgr);
    bool found = false;

    if (m->num_records)
    {
        float sim_time = *(float*)(player + 2092);

        if (sim_time != 0.0f)
        {
            float interval_per_tick = *(float*)0x4346A338;
            int   global_tickcount = *(int*)(*(uint32_t*)0x43475A60 + 4);

            if (interval_per_tick <= 0.0f)
                interval_per_tick = 1.0f / 64.0f;

            int target_tick = global_tickcount
                - (int)(sim_time / interval_per_tick + 0.5f);

            const int window = 12;
            int min_tick = target_tick - window;
            int max_tick = target_tick;

            void* best = nullptr;
            int   best_delta = INT_MAX;

            int start = m->current_index & 0x1F;

            for (int i = 0; i < m->num_records; ++i)
            {
                int idx = (start + i) & 0x1F;
                void* rec = m->ring[idx];
                if (!rec) continue;

                int rec_tick = *(int*)((char*)rec + 72);
                if (rec_tick < min_tick || rec_tick > max_tick)
                    continue;

                int delta = abs(rec_tick - target_tick);
                if (delta < best_delta) {
                    best_delta = delta;
                    best = rec;
                }
            }

            if (best) {
                float* pos = (float*)((char*)best + 12);
                out[0] = pos[0];
                out[1] = pos[1];
                out[2] = pos[2];
                found = true;
            }
        }
    }

    if (!found) {
        float* origin = (float*)(player + 0x134);
        out[0] = origin[0];
        out[1] = origin[1];
        out[2] = origin[2];
    }

    return out;
}

static void InstallBacktrackHook()
{
    const uint32_t target = 0x4340C148;
    const uint32_t replacement = (uint32_t)&Backtrack_GetPosition;

    DWORD oldProtect = 0;
    if (!VirtualProtect((void*)target, 6, PAGE_EXECUTE_READWRITE, &oldProtect))
        return;

    *(uint8_t*)(target + 0) = 0x68;
    *(uint32_t*)(target + 1) = replacement;
    *(uint8_t*)(target + 5) = 0xC3;

    VirtualProtect((void*)target, 6, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (void*)target, 6);
    LPRINT(skCrypt("[INFO] Backtrack hook installed\n"));
}

// ===========================================================================
// [HITBOX HOOK] Bridge functions
// ===========================================================================
static hitbox::Vec3 Bridge_GetBonePos(int player, int bone)
{
    uintptr_t bone_matrix = *(uintptr_t*)(player + 0x26A8);
    if (!bone_matrix) return { 0, 0, 0 };

    struct matrix3x4 { float m[3][4]; };
    auto* m = (matrix3x4*)(bone_matrix + 0x30 * bone);
    return { m->m[0][3], m->m[1][3], m->m[2][3] };
}

static hitbox::QAngle Bridge_VecToAngles(const hitbox::Vec3& dir)
{
    hitbox::QAngle ang;
    ang.pitch = -asinf(dir.z) * (180.0f / 3.14159265358979323846f);
    ang.yaw = atan2f(dir.y, dir.x) * (180.0f / 3.14159265358979323846f);
    ang.roll = 0.0f;
    return ang;
}

static bool Bridge_IsVisible(const hitbox::Vec3& from, const hitbox::Vec3& to, int /*player*/)
{
    TraceVector eye = { from.x, from.y, from.z };
    TraceVector target = { to.x,   to.y,   to.z };

    CSTrace tr;
    if (!TraceLOS(eye, target, nullptr, &tr, MASK_SHOT, 1.0f, 8192.0f))
        return false;

    constexpr float kVisibleThreshold = 0.97f;
    return tr.fraction() >= kVisibleThreshold;
}

static hitbox::Vec3 Bridge_GetEyePos()
{
    return { 0, 0, 0 };
}

static hitbox::QAngle Bridge_GetViewAngles()
{
    return { 0, 0, 0 };
}

static int Bridge_GetWeaponId()
{
    return 0;
}

static bool Bridge_GetTargetInfo(int player, bool* has_armor, float* health)
{
    if (!player) return false;

    *health = *(float*)(player + 0x100);
    int armor = *(int*)(player + 0x117C);
    *has_armor = (armor > 0);
    return true;
}

// ===========================================================================
// [HITBOX HOOK] Replaces sub_FBF38 (0x4340BF38)
// ===========================================================================
int __fastcall Hooked_FBF38(int a2_entity_id, int a1_player_index)
{
    void* cache = *(void**)0x43467460;
    if (!cache) return 0;

    auto cb = *(int(__cdecl**)(void*, int, int, void*, int, int, int))0x4346832C;
    if (!cb) return 0;

    uint32_t count = *(uint32_t*)((char*)cache + 12);
    if ((unsigned)a1_player_index >= count)
        return 0;

    void* array_base = *(void**)cache;
    void* entry = (char*)array_base + 56 * a1_player_index;

    float    timestamp = *(float*)((char*)entry + 20);
    uint32_t type = *(uint32_t*)entry;
    uint32_t id = *(uint32_t*)((char*)entry + 4);
    uint8_t  force = *(uint8_t*)((char*)entry + 40);
    float    thresh = *(float*)0x4346EE6C;

    float now = *(float*)0x43475A60;
    if (now - timestamp > 1.0f && !force)
        return 0;

    bool fresh = (fabsf(timestamp - thresh) > 1e-4f) || (force != 0);
    if (!fresh || id != (uint32_t)a2_entity_id || type != 0xFFFFFFFE)
        return 0;

    hitbox::Context ctx{};
    ctx.target_player = a2_entity_id;
    ctx.get_bone_pos = Bridge_GetBonePos;
    ctx.vec_to_angles = Bridge_VecToAngles;
    ctx.is_visible = Bridge_IsVisible;
    ctx.get_eye_pos = Bridge_GetEyePos;
    ctx.get_view_angles = Bridge_GetViewAngles;
    ctx.get_weapon_id = Bridge_GetWeaponId;
    ctx.get_target_info = Bridge_GetTargetInfo;
    ctx.backtrack_age_ms = 0.0f;
    ctx.head_radius = 4.0f;
    ctx.max_range = 4000.0f;
    ctx.max_view_angle = 180.0f;

    hitbox::Selection target = hitbox::select_best(ctx);

    struct scratch_t { float x, y, z; };
    scratch_t scratch = { 0, 0, 0 };

    int hitgroup_to_use = -1;
    if (target.valid) {
        scratch.x = target.point.x;
        scratch.y = target.point.y;
        scratch.z = target.point.z;
        hitgroup_to_use = target.hitgroup;
    }

    uint32_t saved_index = *(uint32_t*)((char*)cache + 20);
    *(uint32_t*)((char*)cache + 20) = (uint32_t)a1_player_index;

    int result = cb(cache, 0, a2_entity_id, &scratch, 0, 0, hitgroup_to_use);

    *(uint32_t*)((char*)cache + 20) = saved_index;
    return result;
}

static void InstallHitboxHook()
{
    const uint32_t target = 0x4340BF38;
    const uint32_t replacement = (uint32_t)&Hooked_FBF38;

    DWORD oldProtect = 0;
    if (!VirtualProtect((void*)target, 6, PAGE_EXECUTE_READWRITE, &oldProtect))
        return;

    *(uint8_t*)(target + 0) = 0x68;
    *(uint32_t*)(target + 1) = replacement;
    *(uint8_t*)(target + 5) = 0xC3;

    VirtualProtect((void*)target, 6, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (void*)target, 6);
    LPRINT(skCrypt("[INFO] Hitbox hook installed\n"));
}

// ===========================================================================
// [TRACKER HOOK] Improved per-weapon motion tracker (0x4340B419)
// ===========================================================================
typedef void(__fastcall* LagCompensationFn)(int ctx, int unused, float* out_xyz, void* weapon);
static const LagCompensationFn sub_FC148 = (LagCompensationFn)0x4340C148;

struct TrackerEntry {
    char     pad_00[20];
    float    last_x;
    float    last_y;
    float    last_z;
    uint8_t  has_prev;
    char     pad_21[3];
    float    scale;
};

struct TrackerState {
    float    last_x_raw, last_y_raw, last_z_raw;
    float    dir_x, dir_y, dir_z;
    float    scale;
    uint32_t generation;
    uint8_t  has_prev;
};

static std::unordered_map<uint64_t, TrackerState> s_tracker_state;

static float GetFrameDeltaSeconds()
{
    static uint64_t s_last = 0;
    uint64_t now = GetTickCount64();
    if (s_last == 0) { s_last = now; return 1.0f / 64.0f; }
    float dt = (now - s_last) / 1000.0f;
    s_last = now;
    if (dt < 0.001f) dt = 0.001f;
    if (dt > 0.100f) dt = 0.100f;
    return dt;
}

struct MagProfile { float min_mag; float max_mag; };

static MagProfile MagProfileForWeapon(int id)
{
    switch (id) {
    case 9:  case 38: case 40:             return { 100.0f, 5000.0f };
    case 7:  case 16: case 60:             return { 50.0f,  3000.0f };
    case 1:                                return { 50.0f,  2500.0f };
    case 34: case 17: case 24: case 19:    return { 30.0f,  1500.0f };
    default:                               return { 20.0f,  1000.0f };
    }
}

int __fastcall Tracker_Update(int player, int /*unused*/, void* weapon, float* accumulator)
{
    if (!player || !weapon || !accumulator)
        return 0;

    float pos[3] = { 0.0f, 0.0f, 0.0f };
    sub_FC148(player, 0, pos, weapon);

    float x_raw = pos[0];
    float y_raw = pos[1];
    float z_raw = pos[2];

    void** vtable = *(void***)((char*)weapon + 8);
    if (!vtable || !vtable[10]) return 0;

    int raw_id = ((int(__fastcall*)(void*))vtable[10])((char*)weapon + 8);
    if (raw_id < 0 || raw_id > 0x3F) return 0;

    uint32_t entity_id = *(uint32_t*)((char*)player + 0x64);
    uint64_t key = ((uint64_t)entity_id << 32) | (uint32_t)raw_id;

    int health = *(int*)((char*)player + 0x100);
    if (health <= 0) {
        s_tracker_state.erase(key);
        return 0;
    }

    float duck = *(float*)((char*)weapon + 0x1F94);
    if (duck < 0.0f) duck = 0.0f;
    if (duck > 1.0f) duck = 1.0f;
    float z_scale = *(float*)0x434723EC * (1.0f + duck * 0.5f);

    TrackerState& st = s_tracker_state[key];

    uint32_t cur_gen = (uint32_t)(GetTickCount64() >> 8);
    if (st.has_prev && st.generation != cur_gen) {
        st.has_prev = 0;
        st.dir_x = st.dir_y = st.dir_z = 0.0f;
    }
    st.generation = cur_gen;

    bool visible = true;

    float out_x_raw = x_raw;
    float out_y_raw = y_raw;
    float out_z_raw = z_raw;

    if (st.has_prev)
    {
        float dt = GetFrameDeltaSeconds();
        constexpr float kMaxSpeed = 250.0f;
        float max_move = kMaxSpeed * dt;

        float dx = x_raw - st.last_x_raw;
        float dy = y_raw - st.last_y_raw;
        float dz = z_raw - st.last_z_raw;

        float dist = sqrtf(dx * dx + dy * dy + dz * dz);

        if (dist > 1e-6f)
        {
            st.dir_x = dx / dist;
            st.dir_y = dy / dist;
            st.dir_z = dz / dist;

            float proj = dx * st.dir_x + dy * st.dir_y + dz * st.dir_z;

            if (proj > max_move || proj < -max_move) {
                float k = max_move / dist;
                out_x_raw = st.last_x_raw + dx * k;
                out_y_raw = st.last_y_raw + dy * k;
                out_z_raw = st.last_z_raw + dz * k;
            }
        }
    }

    if (!visible && st.has_prev) {
        out_x_raw = st.last_x_raw;
        out_y_raw = st.last_y_raw;
        out_z_raw = st.last_z_raw;
    }

    if (!st.has_prev) {
        st.has_prev = 1;
        st.dir_x = st.dir_y = st.dir_z = 0.0f;
    }
    st.last_x_raw = out_x_raw;
    st.last_y_raw = out_y_raw;
    st.last_z_raw = out_z_raw;

    float out_x = out_x_raw;
    float out_y = out_y_raw;
    float out_z = out_z_raw * z_scale;

    int weapon_idx = raw_id + 58;
    TrackerEntry* entry = (TrackerEntry*)((char*)player + 40 * weapon_idx);
    entry->last_x = out_x;
    entry->last_y = out_y;
    entry->last_z = out_z;
    entry->has_prev = 1;

    float scale = st.scale;
    if (scale <= 0.0f || scale > 10.0f)
        scale = 1.0f;
    st.scale = scale;

    MagProfile mp = MagProfileForWeapon(raw_id);
    float mag = sqrtf(out_x * out_x + out_y * out_y + out_z * out_z);

    if (mp.min_mag <= mag && mag <= mp.max_mag)
    {
        constexpr float kQuantum = 0.03f;
        float qx = roundf(out_x / kQuantum) * kQuantum;
        float qy = roundf(out_y / kQuantum) * kQuantum;
        float qz = roundf(out_z / kQuantum) * kQuantum;

        accumulator[0] += scale * qx;
        accumulator[1] += scale * qy;
        accumulator[2] += scale * qz;

        float acc_mag = sqrtf(accumulator[0] * accumulator[0]
            + accumulator[1] * accumulator[1]
            + accumulator[2] * accumulator[2]);
        if (acc_mag > 1e6f) {
            accumulator[0] = 0.0f;
            accumulator[1] = 0.0f;
            accumulator[2] = 0.0f;
        }
    }

    return 0;
}

static void InstallTrackerHook()
{
    const uint32_t target = 0x4340B419;
    const uint32_t replacement = (uint32_t)&Tracker_Update;

    DWORD oldProtect = 0;
    if (!VirtualProtect((void*)target, 6, PAGE_EXECUTE_READWRITE, &oldProtect))
        return;

    *(uint8_t*)(target + 0) = 0x68;
    *(uint32_t*)(target + 1) = replacement;
    *(uint8_t*)(target + 5) = 0xC3;

    VirtualProtect((void*)target, 6, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (void*)target, 6);
    LPRINT(skCrypt("[INFO] Tracker hook installed\n"));
}

// ===========================================================================
// Pattern scan
// ===========================================================================
std::uint8_t* PatternScan(void* module, const char* signature)
{
    static auto pattern_to_byte = [](const char* pattern) {
        auto bytes = std::vector<int>{};
        auto start = const_cast<char*>(pattern);
        auto end = const_cast<char*>(pattern) + strlen(pattern);

        for (auto current = start; current < end; ++current) {
            if (*current == '?') {
                ++current;
                if (*current == '?') ++current;
                bytes.push_back(-1);
            }
            else {
                bytes.push_back(strtoul(current, &current, 16));
            }
        }
        return bytes;
        };

    auto dosHeader = (PIMAGE_DOS_HEADER)module;
    auto ntHeaders = (PIMAGE_NT_HEADERS)((std::uint8_t*)module + dosHeader->e_lfanew);
    auto sizeOfImage = ntHeaders->OptionalHeader.SizeOfImage;
    auto patternBytes = pattern_to_byte(signature);
    auto scanBytes = reinterpret_cast<std::uint8_t*>(module);

    auto s = patternBytes.size();
    auto d = patternBytes.data();

    for (auto i = 0ul; i < sizeOfImage - s; ++i) {
        bool found = true;
        for (auto j = 0ul; j < s; ++j) {
            if (scanBytes[i + j] != d[j] && d[j] != -1) {
                found = false;
                break;
            }
        }
        if (found) return &scanBytes[i];
    }
    return nullptr;
}

// ===========================================================================
// INT3 handler
// ===========================================================================
static bool handle_int3(CONTEXT* ctx, ctx_t* dump_ctx) {
    auto skeet = skeet_t::getInstance();

    auto offset = static_cast<int32_t>(dump_ctx->offset);
    auto size = static_cast<int32_t>(dump_ctx->size);

    if (offset > 0) ctx->Esp -= size;
    else if (offset < 0) ctx->Esp += size;

    if ((dump_ctx->current_rip == 0x43493904 || dump_ctx->current_rip == 0x43493908)) {
        if (!skeet_t::is_stack_range(dump_ctx->rax) || skeet_t::is_image_range(dump_ctx->rax))
            ctx->Eax = dump_ctx->rax;

        if (dump_ctx->current_rip == 0x43493908) {
            MessageBoxA(0, std::format("0x{:X}", ctx->Esi).c_str(), "ESI", 0);
            std::memcpy((void*)ctx->Esi, menuBin, sizeof(menuBin));
        }
    }
    else if (dump_ctx->current_rip == 0x43493900) {
        if (!skeet_t::is_stack_range(dump_ctx->rax) || skeet_t::is_image_range(dump_ctx->rax))
            ctx->Eax = dump_ctx->rax;
    }

    if (dump_ctx->current_rip == 0x43493900 || dump_ctx->current_rip == 0x434938FC) {
        ctx->Eax = (u32)skeet_t::getInstance()->menuFonts() + (dump_ctx->rax - 0x940000);
    }
    if (dump_ctx->rip == 0x43482439) {
        ctx->Ebp = (u32)((u32)GetModuleHandleA("client.dll") + 0xDB61DC);
    }

    ctx->Eip = dump_ctx->rip;

    if (dump_ctx->rbp == 0x6CD10000)
        ctx->Ebp = (u32)GetModuleHandleA("comctl32.dll");
    if (dump_ctx->rbp == 0x285b0000)
        ctx->Ebp = (u32)GetModuleHandleA("client.dll");

    return true;
}

static Value fromERegToContext(ereg reg, CONTEXT& ctx) {
    switch (reg) {
    case ereg::eax: return Value(&ctx.Eax, 4);
    case ereg::ebx: return Value(&ctx.Ebx, 4);
    case ereg::ecx: return Value(&ctx.Ecx, 4);
    case ereg::edx: return Value(&ctx.Edx, 4);
    case ereg::esi: return Value(&ctx.Esi, 4);
    case ereg::edi: return Value(&ctx.Edi, 4);
    case ereg::ebp: return Value(&ctx.Ebp, 4);
    case ereg::esp: return Value(&ctx.Esp, 4);
    }
}

Value Operand::get(CONTEXT& ctx)
{
    switch (type) {
    case etype::reg: return fromERegToContext(reg, ctx);
    case etype::imm: return Value(&this->imm, 4);
    case etype::mem: return Value((void*)this->imm, 4);
    }
}

void Handler::handle(CONTEXT& ctx)
{
    if (ctx.Eip == 0x43495251) {
        std::memcpy((void*)ctx.Eax, initialization_info, sizeof(initialization_info));
    }

    switch (this->mnem) {
    case emnemonic::mov: {
        auto op1 = this->operands[0].get(ctx);
        auto op2 = this->operands[1].get(ctx);
        op1.setValue(op2.value());
        break;
    }
    case emnemonic::push: {
        auto op1 = this->operands[0].get(ctx);
        ctx.Esp -= 4;
        *(uint32_t*)(ctx.Esp) = op1.value();
        break;
    }
    case emnemonic::call: {
        auto op1 = this->operands[0].get(ctx);
        ctx.Esp -= 4;
        *(uint32_t*)(ctx.Esp) = this->addr + this->len;
        ctx.Eip = op1.value();
        return;
    }
    }
    ctx.Eip += this->len;
}

static PCONTEXT saver;

static LONG __stdcall skeet_exception_handler(EXCEPTION_POINTERS* ExceptionInfo) {
    auto exception_ctx = ExceptionInfo->ContextRecord;
    static int i = 0;
    if (exception_ctx->Eip == 0x434938FF)
    {
        i++;
        if (i == 314)
        {
            saver = new CONTEXT;
            memcpy(saver, exception_ctx, sizeof(CONTEXT));
            extern void LoadStub();
            exception_ctx->Eip = (DWORD)LoadStub;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        else if (i == 315)
        {
            memcpy(exception_ctx, saver, sizeof(CONTEXT));
            delete saver;
        };
    };

    if (ExceptionInfo->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) {
        if (ExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
            std::wstring msg = std::format(L"access violation at 0x{:x}\nPress CTRL + C and send to the topic", exception_ctx->Eip);
            MessageBoxW(0, msg.c_str(), L"error", MB_ICONWARNING);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    static int c = 0;
    auto& ctx = contexts[c];
    if (exception_ctx->Eip >= skeet_t::getInstance()->base() && exception_ctx->Eip < skeet_t::getInstance()->base() + skeet_t::getInstance()->size()) {
        auto it = std::find_if(handlers.begin(), handlers.end(), [&](const Handler& other) {
            return exception_ctx->Eip >= other.addr && exception_ctx->Eip < other.addr + other.len;
            });

        if (it == handlers.end()) {
            if (!(exception_ctx->Eip >= ctx.current_rip - 5 && exception_ctx->Eip < ctx.current_rip + 5)) {
                auto it = std::find_if(contexts.begin() + c, contexts.end(), [&](const ctx_t& ctx) {
                    return exception_ctx->Eip >= ctx.current_rip - 1 && exception_ctx->Eip < ctx.current_rip + 1;
                    });

                if (it == contexts.end()) {
                    auto it = std::find_if(removedHandlers.begin(), removedHandlers.end(), [&](const removed_handler_t& other) {
                        return other.vip == exception_ctx->Ebp;
                        });

                    if (it == removedHandlers.end())
                        return EXCEPTION_CONTINUE_SEARCH;

                    exception_ctx->Edi -= 4;
                    *(uint32_t*)exception_ctx->Edi = it->value;
                    exception_ctx->Ebx = it->key;
                    exception_ctx->Ebp += 8;
                    exception_ctx->Esi = it->next_handler;
                    exception_ctx->Eip = it->next_handler;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }

                ctx = *it;
                c = it - contexts.begin();
            }

            c++;

            if (!handle_int3(exception_ctx, &ctx)) {
                MessageBoxA(0, "undefined behaviour", "error", 2);
                return EXCEPTION_CONTINUE_SEARCH;
            }
            LPRINT(skCrypt("[INFO]") << std::dec << c - 1 << " | " << std::hex << " [RAX]: " << ctx.rax << " |  rip handling... " << ctx.current_rip << " to rip " << ctx.rip << " | total size " << std::dec << contexts.size() << "\n");
        }
        else {
            it->handle(*exception_ctx);
        }

        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void entry_thread() {
    return ((void(__stdcall*)())0x1337)();
}

static vm_element_t parse_vm_element(const nlohmann::json& json) {
    static std::map<std::string, eoperation> op_correspondence = {
        { "ROL", eoperation::ROL }, { "DEC", eoperation::DEC },
        { "NOT", eoperation::NOT }, { "BSWAP", eoperation::BSWAP },
        { "ADD", eoperation::ADD }, { "XOR", eoperation::XOR },
        { "ROR", eoperation::ROR }, { "INC", eoperation::INC },
        { "NEG", eoperation::NEG }, { "SUB", eoperation::SUB }
    };
    vm_element_t element = {};

    element.addr = json["address"].get<uint32_t>();
    element.encryption_key = json["key"].get<uint32_t>();

    for (const nlohmann::json& operation : json["operations"]) {
        vm_operation_t vm_op = {};
        vm_op.addr = std::stoull(operation[0].get<std::string>());
        vm_op.operation = op_correspondence[operation[1]];
        vm_op.value = std::stoul(operation[2].get<std::string>());
        element.operations.push_back(vm_op);
    }

    element.size = json["size"];
    element.value = json["value"];
    element.vip = json["vip"];

    return element;
}

skeet_t::skeet_t(HMODULE base) : _base(0x43310000), page(nullptr), _stack(nullptr) {
    if (singleton) return;
    singleton = this;

    HRSRC hRes = FindResource(base, MAKEINTRESOURCE(IDR_BINARY1), skCrypt(L"BINARY"));
    HGLOBAL hResData = LoadResource(base, hRes);
    LPVOID pData = LockResource(hResData);
    DWORD size = SizeofResource(base, hRes);

    _size = size;
    skeet_bin = new char[size];
    std::memcpy(skeet_bin, pData, size);

    hRes = FindResource(base, MAKEINTRESOURCE(IDR_EXPORTS1), skCrypt(L"EXPORTS"));
    hResData = LoadResource(base, hRes);
    pData = LockResource(hResData);
    size = SizeofResource(base, hRes);

    std::string strFileExports((char*)pData, size);
    nlohmann::json jsExports = nlohmann::json::parse(strFileExports);

    for (const auto& [addr, value] : jsExports.items()) {
        import_t import_;
        import_.old_addr = static_cast<uint32_t>(std::stoul(addr));
        for (const auto& [lib, api] : value.items()) {
            import_.lib = lib;
            import_.name = api;
        }
        exports[import_.old_addr] = import_;
    }

    hRes = FindResource(base, MAKEINTRESOURCE(IDR_BYTECODE1), skCrypt(L"BYTECODE"));
    hResData = LoadResource(base, hRes);
    pData = LockResource(hResData);
    size = SizeofResource(base, hRes);

    std::string strFileBytecode((char*)pData, size);
    nlohmann::json jsBytecode = nlohmann::json::parse(strFileBytecode);

    for (const auto& unit : jsBytecode) {
        if (unit["main"].empty()) continue;

        std::vector<vm_element_t> infoHandlers;
        infoHandlers.push_back(parse_vm_element(unit["main"]));

        if (unit.contains("others")) {
            for (const nlohmann::json& other : unit["others"]) {
                infoHandlers.push_back(parse_vm_element(other));
            }
        }
        handlers.push_back(infoHandlers);
    }

    skeetFonts = VirtualAlloc(0, sizeof(menu_fonts), MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    std::memcpy(skeetFonts, menu_fonts, sizeof(menu_fonts));
}

bool skeet_t::map()
{
    page = (void*)_base;
    _size = 0x2fc000;
    try { memcpy(page, skeet_bin, _size); }
    catch (...) { return false; }
    return true;
}

bool skeet_t::fix_imports()
{
    for (auto& _import : imports) {
        auto old_addr = 0;
        switch (*(uint8_t*)(_import.rip)) {
        case 0xe8: case 0xe9:
            old_addr = *(u32*)(_import.rip + 1) + _import.rip + 5; break;
        default:
            old_addr = *(u32*)(_import.rip + 1); break;
        }

        if (exports.find(old_addr) == exports.end())
            LPRINT("(fix_imports) failed to get " << std::hex << old_addr << "\n");

        auto& export_ = exports[old_addr];
        auto import_addr = (u32)(GetProcAddress(LoadLibraryA(export_.lib.c_str()), export_.name.c_str()));

        if (!import_addr) {
            LPRINT("(fix_imports) failed to get " << std::hex << export_.lib.c_str() << " " << export_.name.c_str() << "\n");
            return false;
        }

        switch (*(uint8_t*)(_import.rip)) {
        case 0xe8: case 0xe9:
            *(u32*)(_import.rip + 1) = import_addr - _import.rip - 5; break;
        default:
            *(u32*)(_import.rip + 1) = import_addr; break;
        }
    }
    return true;
}

using HkRecoverFn = int(__fastcall*)(int base, int size);
HkRecoverFn detourCrcCheck = nullptr;

static int __fastcall crcCheck(int base, int size) {
    if (base == 0x4331E000 && size == 0x105B12) {
        return 0x20547A5B ^ 0x92CA5DF1;
    }
    return detourCrcCheck(base, size);
}

bool skeet_t::extra()
{
    InstallBacktrackHook();
   //InstallHitboxHook(); this causes the crash
    InstallTrackerHook();
    InstallAllPatches();
    InstallAimImprovements();

    const uint32_t hashes[] = { 0xC0BA92C9, 0xBBFFA7E0, 0xA42BE2F8 };

    std::thread([&]() {
        int c = 0;
        while (*(uint32_t*)(0x4346D4CC) == 0 && c < 3) {
            *(uint32_t*)(0x4346D4CC) = hashes[c];
        }
        }).detach();

    LPRINT(skCrypt("[INFO] adding exception handler...\n"));
    AddVectoredExceptionHandler(0, skeet_exception_handler);
    LPRINT(skCrypt("[INFO] added!\n"));
    memcpy((void*)0x43490c82, "\xC7\x02\x00\x00\x00\x00\xC3", sizeof("\xC7\x02\x00\x00\x00\x00\xC3") - 1);

    *(uint8_t*)0x43466D7C = 0xeb;
    *(uint8_t*)0x4348B4FC = 0xeb;

    auto hWnd = FindWindowA("Valve001", "Counter-Strike: Global Offensive - Direct3D 9");
    *(uint32_t*)0x433A068A = (uint32_t)hWnd;
    *(uint32_t*)0x4341F644 = (uint32_t)hWnd;
    *(uint32_t*)0x4341F7C0 = (uint32_t)hWnd;

    memset((void*)0x4350F0C7, 0x90, 5);

    *(uint32_t*)0x43467F64 = ((u32)GetModuleHandleA("shaderapidx9.dll") + 0xA62C0);

    *(uint32_t*)(0x4346837C) = (uint32_t)((uint32_t)GetModuleHandleA("engine.dll") + 0x2BDC20);
    *(uint32_t*)(0x43467570) = (uint32_t)((uint32_t)GetModuleHandleA("engine.dll") + 0x2BD020);
    *(uint32_t*)(0x43468234) = (uint32_t)((uint32_t)GetModuleHandleA("studiorender.dll") + 0x57DF0);
    *(uint32_t*)(0x43468C98) = (uint32_t)((uint32_t)GetModuleHandleA("studiorender.dll") + 0x44E2EC);

    *(uint32_t*)(0x434670AC) = (uint32_t)((uint32_t)GetModuleHandleA("matchmaking.dll"));
    *(uint32_t*)(0x434670E4) = (uint32_t)((uint32_t)GetModuleHandleA("server.dll"));
    *(uint32_t*)(0x4346762C) = (uint32_t)((uint32_t)GetModuleHandleA("panoramauiclient.dll"));
    *(uint32_t*)(0x43467AFC) = (uint32_t)((uint32_t)GetModuleHandleA("vguimatsurface.dll"));
    *(uint32_t*)(0x43467B74) = (uint32_t)((uint32_t)GetModuleHandleA("filesystem_stdio.dll"));
    *(uint32_t*)(0x43467DAC) = (uint32_t)((uint32_t)GetModuleHandleA("inputsystem.dll"));
    *(uint32_t*)(0x4346889C) = (uint32_t)((uint32_t)GetModuleHandleA("vgui2.dll"));
    *(uint32_t*)(0x43468A7C) = (uint32_t)((uint32_t)GetModuleHandleA("vstdlib.dll"));
    *(uint32_t*)(0x43468B74) = (uint32_t)((uint32_t)GetModuleHandleA("vphysics.dll"));
    *(uint32_t*)(0x43468D08) = (uint32_t)((uint32_t)GetModuleHandleA("gameoverlayrenderer.dll"));
    *(uint32_t*)(0x43468D20) = (uint32_t)((uint32_t)GetModuleHandleA("engine.dll"));
    *(uint32_t*)(0x43468D40) = (uint32_t)((uint32_t)GetModuleHandleA("soundsystem.dll"));
    *(uint32_t*)(0x43468D98) = (uint32_t)((uint32_t)GetModuleHandleA("datacache.dll"));
    *(uint32_t*)(0x43469234) = (uint32_t)((uint32_t)GetModuleHandleA("panorama.dll"));
    *(uint32_t*)(0x4346A55C) = (uint32_t)((uint32_t)GetModuleHandleA("client.dll"));
    *(uint32_t*)(0x4346A600) = (uint32_t)((uint32_t)GetModuleHandleA("materialsystem.dll"));
    *(uint32_t*)(0x4346A604) = (uint32_t)((uint32_t)GetModuleHandleA("shaderapidx9.dll"));
    *(uint32_t*)(0x4346A650) = (uint32_t)((uint32_t)GetModuleHandleA("studiorender.dll"));

    *(uint32_t*)(0x434693F0) = (uint32_t)((uint32_t)GetModuleHandleA("client.dll") + 0x43E9E0);
    *(uint32_t*)(0x43467F0C) = (uint32_t)((uint32_t)GetModuleHandleA("client.dll") + 0x443481);

    *(uint32_t*)(0x43468E44) = **(uint32_t**)((uint32_t)GetModuleHandleA("engine.dll") + 0x23790);
    *(uint32_t*)(0x4346D4D0) = (uint32_t)((uint32_t)GetModuleHandleA("engine.dll") + 0xB7550);

    *(uint32_t*)(0x4346D4C8) = (uint32_t)((uint32_t)GetModuleHandleA("materialsystem.dll") + 0xBBF4);
    *(uint32_t*)(0x4346D4C4) = (uint32_t)((uint32_t)GetModuleHandleA("engine.dll") + 0x1A3656);
    *(uint32_t*)(0x4346D4C0) = (uint32_t)((uint32_t)GetModuleHandleA("engine.dll") + 0x59E9CC);

    *(uint32_t*)(0x434689A0) = (uint32_t)((uint32_t)GetModuleHandleA("client.dll") + 0x2F10D3);
    *(uint32_t*)(0x4346A454) = (uint32_t)((uint32_t)GetModuleHandleA("engine.dll") + 0xD99C0);
    *(uint32_t*)(0x434674E0) = (uint32_t)((uint32_t)GetModuleHandleA("client.dll") + 0x4123AC);
    *(uint32_t*)(0x43468518) = (uint32_t)((uint32_t)GetModuleHandleA("client.dll") + 0x52EFAC);
    *(uint32_t*)(0x434688C4) = (uint32_t)((uint32_t)GetModuleHandleA("client.dll") + 0x4413A0);

    *(uint32_t*)(0x4346D668) = (uint32_t)((uint32_t)GetModuleHandleA("engine.dll") + 0x00228908);
    *(uint32_t*)(0x4346D66C) = (uint32_t)((uint32_t)GetModuleHandleA("engine.dll") + 0x000F03FD);
    *(uint32_t*)(0x43468B9C) = (uint32_t)((uint32_t)GetModuleHandleA("client.dll") + 0x00712740);

    *(uint32_t*)(0x4346A31C) = (uint32_t)((uint32_t)GetModuleHandleA("localize.dll") + 0x00038A70);

    *(uint32_t*)(0x43468B2C) = (uint32_t)((uint32_t)GetModuleHandleA("engine.dll") + 0x52C810);
    *(uint32_t*)(0x43467288) = (uint32_t)((uint32_t)GetModuleHandleA("engine.dll") + 0x22F08F);
    *(uint32_t*)(0x4346728C) = (uint32_t)((uint32_t)GetModuleHandleA("client.dll") + 0x9A2CF0);
    *(uint32_t*)(0x4346728C) = (uint32_t)((uint32_t)GetModuleHandleA("client.dll") + 0xDF14C0);
    *(uint32_t*)(0x4346D540) = (uint32_t)((uint32_t)GetModuleHandleA("client.dll") + 0x3DC9B0);
    *(uint32_t*)(0x43476A40) = (uint32_t)((uint32_t)GetModuleHandleA("engine.dll") + 0x52B270);
    *(uint32_t*)(0x434671B4) = (uint32_t)((uint32_t)GetModuleHandleA("engine.dll") + 0x52B248);
    *(uint32_t*)(0x434678C4) = (uint32_t)((uint32_t)GetModuleHandleA("client.dll") + 0x2C8F50);
    *(uint32_t*)0x43468B9C = ((uint32_t)GetModuleHandleA("client.dll") + 0x712740);
    *(uint32_t*)0x43467C04 = ((uint32_t)GetModuleHandleA("client.dll") + 0x5344CE0);
    *(uint32_t*)0x43467040 = ((uint32_t)GetModuleHandleA("client.dll") + 0x70ADB0);
    *(uint32_t*)0x43468994 = ((uint32_t)GetModuleHandleA("client.dll") + 0x523BC98);

    *(uint32_t*)0x4346D67C = ((uint32_t)GetModuleHandleA("panorama.dll") + 0x9E220);
    *(uint32_t*)0x4346D680 = ((uint32_t)GetModuleHandleA("panorama.dll") + 0x9A320);
    *(uint32_t*)0x4346D684 = ((uint32_t)GetModuleHandleA("panorama.dll") + 0xA1B80);
    *(uint32_t*)0x4346D688 = ((uint32_t)GetModuleHandleA("panorama.dll") + 0xA3C80);
    *(uint32_t*)0x4346D68C = ((uint32_t)GetModuleHandleA("panorama.dll") + 0xAB6F0);

    *(uint32_t*)0x43468D0C = ((uint32_t)GetModuleHandleA("client.dll") + 0xDF14C0);
    *(uint32_t*)0x43467A70 = ((uint32_t)GetModuleHandleA("client.dll") + 0xDF14D4);
    *(uint32_t*)0x43468D50 = ((uint32_t)GetModuleHandleA("client.dll") + 0x5334E08);

    *(uint32_t*)0x4346D560 = ((uint32_t)GetModuleHandleA("engine.dll") + 0x8E55C);
    *(uint32_t*)0x4346D55C = ((uint32_t)GetModuleHandleA("engine.dll") + 0x22BE36);

    *(uint32_t*)0x4346A374 = ((uint32_t)GetModuleHandleA("client.dll") + 0x2D3A40);
    *(uint32_t*)0x4346D65C = ((uint32_t)GetModuleHandleA("client.dll") + 0x346AC0);
    *(uint32_t*)0x4346D660 = ((uint32_t)GetModuleHandleA("client.dll") + 0x344610);
    *(uint32_t*)0x4346A624 = ((uint32_t)GetModuleHandleA("client.dll") + 0x52632C8);
    *(uint32_t*)0x4346A364 = ((uint32_t)GetModuleHandleA("client.dll") + 0x1D94A0);
    *(uint32_t*)0x434679AC = ((uint32_t)GetModuleHandleA("client.dll") + 0x9A6740);
    *(uint32_t*)0x4346A38C = ((uint32_t)GetModuleHandleA("client.dll") + 0x1D2CA0);
    *(uint32_t*)0x434683D0 = ((uint32_t)GetModuleHandleA("client.dll") + 0xDACC0F);
    *(uint32_t*)0x43468F80 = ((uint32_t)GetModuleHandleA("client.dll") + 0x432E20);
    *(uint32_t*)0x434688C8 = ((uint32_t)GetModuleHandleA("client.dll") + 0x1EA950);
    *(uint32_t*)0x4346A6B0 = ((uint32_t)GetModuleHandleA("client.dll") + 0x1D1BA0);
    *(uint32_t*)0x43468C10 = ((uint32_t)GetModuleHandleA("client.dll") + 0x1AEC40);
    *(uint32_t*)0x43467C58 = ((uint32_t)GetModuleHandleA("client.dll") + 0x5334DA4);
    *(uint32_t*)0x43467E44 = ((uint32_t)GetModuleHandleA("client.dll") + 0x1EA790);
    *(uint32_t*)0x4346A598 = ((uint32_t)GetModuleHandleA("client.dll") + 0x442740);
    *(uint32_t*)0x43467C48 = ((uint32_t)GetModuleHandleA("client.dll") + 0x307260);
    *(uint32_t*)0x43468F30 = ((uint32_t)GetModuleHandleA("client.dll") + 0x320BD0);
    *(uint32_t*)0x434691DC = ((uint32_t)GetModuleHandleA("client.dll") + 0x9A2C40);
    *(uint32_t*)0x4346830C = ((uint32_t)GetModuleHandleA("client.dll") + 0xDF7FC0);
    *(uint32_t*)0x4346A3A4 = ((uint32_t)GetModuleHandleA("client.dll") + 0x5334764);
    *(uint32_t*)0x4346A4F8 = ((uint32_t)GetModuleHandleA("client.dll") + 0x19D140);
    *(uint32_t*)0x434678C8 = ((uint32_t)GetModuleHandleA("client.dll") + 0x1D9440);
    *(uint32_t*)0x4346832C = ((uint32_t)GetModuleHandleA("client.dll") + 0x1B1AC0);
    *(uint32_t*)0x43467CF0 = ((uint32_t)GetModuleHandleA("client.dll") + 0x7DE760);
    *(uint32_t*)0x43467B54 = ((uint32_t)GetModuleHandleA("client.dll") + 0x526388C);
    *(uint32_t*)0x43468C3C = ((uint32_t)GetModuleHandleA("client.dll") + 0x7DEB00);

    *(uint32_t*)0x43467000 = ((uint32_t)GetModuleHandleA("client.dll") + 0x32C0A59);
    *(uint32_t*)0x434693B0 = ((uint32_t)GetModuleHandleA("client.dll") + 0x292CA6C);
    *(uint32_t*)0x4346A770 = ((uint32_t)GetModuleHandleA("client.dll") + 0x4F741CB);
    *(uint32_t*)0x43469198 = ((uint32_t)GetModuleHandleA("client.dll") + 0x1F4E28D);
    *(uint32_t*)0x43469060 = ((uint32_t)GetModuleHandleA("client.dll") + 0x5332374);
    *(uint32_t*)0x434688C8 = ((uint32_t)GetModuleHandleA("client.dll") + 0x1EA950);
    *(uint32_t*)0x43467CF0 = ((uint32_t)GetModuleHandleA("client.dll") + 0x7DE760);
    *(uint32_t*)0x4346830C = ((uint32_t)GetModuleHandleA("client.dll") + 0xDF7FC0);
    *(uint32_t*)0x4346728C = ((uint32_t)GetModuleHandleA("client.dll") + 0x9A2CF0);
    *(uint32_t*)0x43468C10 = ((uint32_t)GetModuleHandleA("client.dll") + 0x1AEC40);
    *(uint32_t*)0x4346A3A4 = ((uint32_t)GetModuleHandleA("client.dll") + 0x5334764);
    *(uint32_t*)0x4346D628 = ((uint32_t)GetModuleHandleA("panorama.dll") + 0x3DFE0);
    *(uint32_t*)0x4346D62C = ((uint32_t)GetModuleHandleA("panorama.dll") + 0x3E020);
    *(uint32_t*)0x43467C48 = ((uint32_t)GetModuleHandleA("client.dll") + 0x307260);
    *(uint32_t*)0x4346D658 = ((uint32_t)GetModuleHandleA("client.dll") + 0x52448F8);
    *(uint32_t*)0x4346D65C = ((uint32_t)GetModuleHandleA("client.dll") + 0x346AC0);
    *(uint32_t*)0x43467E44 = ((uint32_t)GetModuleHandleA("client.dll") + 0x1EA790);
    *(uint32_t*)0x4346A5DC = ((uint32_t)GetModuleHandleA("client.dll") + 0x3E2E673);
    *(uint32_t*)0x43468994 = ((uint32_t)GetModuleHandleA("client.dll") + 0x523BC98);
    *(uint32_t*)0x4346A7CC = ((uint32_t)GetModuleHandleA("engine.dll") + 0x20CE50);
    *(uint32_t*)0x4346A374 = ((uint32_t)GetModuleHandleA("client.dll") + 0x2D3A40);
    *(uint32_t*)0x4346D67C = ((uint32_t)GetModuleHandleA("panorama.dll") + 0x9E220);
    *(uint32_t*)0x4346D680 = ((uint32_t)GetModuleHandleA("panorama.dll") + 0x9A320);
    *(uint32_t*)0x4346D688 = ((uint32_t)GetModuleHandleA("panorama.dll") + 0xA3C80);
    *(uint32_t*)0x43468FF0 = ((uint32_t)GetModuleHandleA("client.dll") + 0x3231380);
    *(uint32_t*)0x434679EC = ((uint32_t)GetModuleHandleA("client.dll") + 0xE10580);
    *(uint32_t*)0x43467F0C = ((uint32_t)GetModuleHandleA("client.dll") + 0x443481);
    *(uint32_t*)0x4346D664 = ((uint32_t)GetModuleHandleA("client.dll") + 0x525E110);
    *(uint32_t*)0x43468250 = ((uint32_t)GetModuleHandleA("client.dll") + 0x5334848);
    *(uint32_t*)0x4346A64C = ((uint32_t)GetModuleHandleA("engine.dll") + 0xD9A18);
    *(uint32_t*)0x43468390 = ((uint32_t)GetModuleHandleA("client.dll") + 0x12170B3);
    *(uint32_t*)0x43467550 = ((uint32_t)GetModuleHandleA("engine.dll") + 0x8DAB0);
    *(uint32_t*)0x43468F80 = ((uint32_t)GetModuleHandleA("client.dll") + 0x432E20);
    *(uint32_t*)0x434688C4 = ((uint32_t)GetModuleHandleA("client.dll") + 0x4413A0);
    *(uint32_t*)0x43468B9C = ((uint32_t)GetModuleHandleA("client.dll") + 0x712740);
    *(uint32_t*)0x43468C94 = ((uint32_t)GetModuleHandleA("client.dll") + 0x52BF6D8);
    *(uint32_t*)0x434688A4 = ((uint32_t)GetModuleHandleA("client.dll") + 0x215080);
    *(uint32_t*)0x4346A624 = ((uint32_t)GetModuleHandleA("client.dll") + 0x52632C8);
    *(uint32_t*)0x434689A0 = ((uint32_t)GetModuleHandleA("client.dll") + 0x2F10D3);
    *(uint32_t*)0x434692F4 = ((uint32_t)GetModuleHandleA("client.dll") + 0x430D0E);
    *(uint32_t*)0x4346D64C = ((uint32_t)GetModuleHandleA("client.dll") + 0x4364D0);
    *(uint32_t*)0x434691DC = ((uint32_t)GetModuleHandleA("client.dll") + 0x9A2C40);
    *(uint32_t*)0x4346D660 = ((uint32_t)GetModuleHandleA("client.dll") + 0x344610);
    *(uint32_t*)0x434688C0 = ((uint32_t)GetModuleHandleA("client.dll") + 0x525D440);
    *(uint32_t*)0x43467DDC = ((uint32_t)GetModuleHandleA("client.dll") + 0x43C750);
    *(uint32_t*)0x43468308 = ((uint32_t)GetModuleHandleA("engine.dll") + 0x2A5982);
    *(uint32_t*)0x434679AC = ((uint32_t)GetModuleHandleA("client.dll") + 0x9A6740);
    *(uint32_t*)0x4346D684 = ((uint32_t)GetModuleHandleA("panorama.dll") + 0xA1B80);
    *(uint32_t*)0x4346D68C = ((uint32_t)GetModuleHandleA("panorama.dll") + 0xAB6F0);

    *(uint32_t*)0x4346A884 = ((uint32_t)GetModuleHandleA("client.dll") + 0x2A78A0);
    *(uint32_t*)0x43468E88 = ((uint32_t)GetModuleHandleA("client.dll") + 0x2A77E0);
    *(uint32_t*)0x4346A7F0 = ((uint32_t)GetModuleHandleA("client.dll") + 0x28CC90);
    *(uint32_t*)0x43467040 = ((uint32_t)GetModuleHandleA("client.dll") + 0x70ADB0);
    *(uint32_t*)0x4346A724 = ((uint32_t)GetModuleHandleA("engine.dll") + 0x1791916);
    *(uint32_t*)0x43472B64 = ((uint32_t)GetModuleHandleA("engine.dll") + 0x2AC6E69);
    *(uint32_t*)0x4346EF18 = ((uint32_t)GetModuleHandleA("engine.dll") + 0x2AC6E69);
    *(uint32_t*)0x4346D558 = ((uint32_t)GetModuleHandleA("engine.dll") + 0x38FDCC8);
    *(uint32_t*)0x434674E0 = ((uint32_t)GetModuleHandleA("client.dll") + 0x4123AC);
    *(uint32_t*)0x4346756C = ((uint32_t)GetModuleHandleA("client.dll") + 0x5B1E2E);
    *(uint32_t*)0x4346832C = ((uint32_t)GetModuleHandleA("client.dll") + 0x1B1AC0);
    *(uint32_t*)0x43468FA4 = ((uint32_t)GetModuleHandleA("client.dll") + 0x5334C84);
    *(uint32_t*)0x4346A364 = ((uint32_t)GetModuleHandleA("client.dll") + 0x1D94A0);
    *(uint32_t*)0x43468320 = ((uint32_t)GetModuleHandleA("client.dll") + 0x1E69D0);
    *(uint32_t*)0x4346A6B0 = ((uint32_t)GetModuleHandleA("client.dll") + 0x1D1BA0);
    *(uint32_t*)0x43467EF4 = ((uint32_t)GetModuleHandleA("client.dll") + 0xE047DC);
    *(uint32_t*)0x434670C4 = ((uint32_t)GetModuleHandleA("client.dll") + 0x525EBC4);
    *(uint32_t*)0x43468C3C = ((uint32_t)GetModuleHandleA("client.dll") + 0x7DEB00);
    *(uint32_t*)0x43468CA4 = ((uint32_t)GetModuleHandleA("client.dll") + 0x7DE8A0);
    *(uint32_t*)0x434693F0 = ((uint32_t)GetModuleHandleA("client.dll") + 0x43E9E0);
    *(uint32_t*)0x434683D0 = ((uint32_t)GetModuleHandleA("client.dll") + 0xDACC0F);
    *(uint32_t*)0x43469348 = ((uint32_t)GetModuleHandleA("client.dll") + 0x36CAC0);
    *(uint32_t*)0x4346A4F8 = ((uint32_t)GetModuleHandleA("client.dll") + 0x19D140);
    *(uint32_t*)0x434679C0 = ((uint32_t)GetModuleHandleA("client.dll") + 0x525CAA0);
    *(uint32_t*)0x4346A454 = ((uint32_t)GetModuleHandleA("engine.dll") + 0xD99C0);
    *(uint32_t*)0x43468F30 = ((uint32_t)GetModuleHandleA("client.dll") + 0x320BD0);
    *(uint32_t*)0x43468DC0 = ((uint32_t)GetModuleHandleA("client.dll") + 0x19D600);
    *(uint32_t*)0x43469370 = ((uint32_t)GetModuleHandleA("engine.dll") + 0x159296);
    *(uint32_t*)0x4346A82C = ((uint32_t)GetModuleHandleA("client.dll") + 0x3D1820);
    *(uint32_t*)0x43468518 = ((uint32_t)GetModuleHandleA("client.dll") + 0x52EFAC);
    *(uint32_t*)0x43469304 = ((uint32_t)GetModuleHandleA("client.dll") + 0x535E4CC);
    *(uint32_t*)0x43467288 = ((uint32_t)GetModuleHandleA("engine.dll") + 0x22F08F);
    *(uint32_t*)0x4346746C = ((uint32_t)GetModuleHandleA("engine.dll") + 0x1C5D7D3);
    *(uint32_t*)0x4346A598 = ((uint32_t)GetModuleHandleA("client.dll") + 0x442740);
    *(uint32_t*)0x434678C8 = ((uint32_t)GetModuleHandleA("client.dll") + 0x1D9440);
    *(uint32_t*)0x43468D6C = ((uint32_t)GetModuleHandleA("engine.dll") + 0x8CD5F0);
    *(uint32_t*)0x4346A7E4 = ((uint32_t)GetModuleHandleA("client.dll") + 0x5344B38);
    *(uint32_t*)0x4346A38C = ((uint32_t)GetModuleHandleA("client.dll") + 0x1D2CA0);
    *(uint32_t*)0x4346D644 = ((uint32_t)GetModuleHandleA("client.dll") + 0x1E8100);

    *(uint32_t*)0x4346A898 = ((uint32_t)GetModuleHandleA("client.dll") + 0xDF98A0);

    uint32_t ret_addr = (uint32_t)PatternScan(GetModuleHandleA("gameoverlayrenderer.dll"), "3D ? ? ? ? 73 ? 68 ? ? ? ? E8 ? ? ? ? 8B 0D ? ? ? ? 83 C4 ? ? ? 6A ? FF 50 ? 3B 5F");
    if (!ret_addr)
        ret_addr = (uint32_t)PatternScan(GetModuleHandleA("gameoverlayrenderer.dll"), "3D ? ? ? ? 73 1A 68 ? ? ? ? E8 ? ? ? ? 8B 0D ? ? ? ? 83 C4 04 8B 01 6A 00 FF 50 14 3B 7B 3C");
    *(uint32_t*)0x43468D94 = ret_addr;

    uint32_t xref = (uint32_t)PatternScan(GetModuleHandleA("gameoverlayrenderer.dll"), "89 3D ? ? ? ? F3 0F 10 87");
    if (!xref)
        xref = (uint32_t)PatternScan(GetModuleHandleA("gameoverlayrenderer.dll"), "89 1D ? ? ? ? F3 0F 10 83");
    if (!xref)
        MessageBoxW(0, L"failed to find pattern[0]", L"error", 0);

    *(uint32_t*)0x43468350 = *(uint32_t*)(xref + 2);

    // [PATCH] Log prefix: [gamesense] -> [pbdlsense]
    *(uint32_t*)0x4341CE04 = 0x1E7E911C;
    *(uint32_t*)0x4341CE0C = 0x1479822B;

    LPRINT(skCrypt("[INFO] recompiling vm...\n"));
    recompile();
    LPRINT(skCrypt("[INFO] recompiled!\n"));
    return true;
}

bool skeet_t::entry()
{
    DWORD tid = 0;
    auto hThread = CreateThread(0, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(entry_thread), 0, CREATE_SUSPENDED, &tid);
    if (hThread == INVALID_HANDLE_VALUE) return false;

    _stack = VirtualAlloc(nullptr, 0x100000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);

    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_FULL;
    ctx.Esp = (uint32_t)_stack + 0x20000;
    ctx.Eip = 0x43481e2c;
    ctx.Esi = 0x43481e2c;
    ctx.Edi = 0x43481e2c;

    typedef struct _LSA_UNICODE_STRING { USHORT Length; USHORT MaximumLength; PWSTR Buffer; } UNICODE_STRING, * PUNICODE_STRING;
    typedef struct _OBJECT_ATTRIBUTES { ULONG Length; HANDLE RootDirectory; PUNICODE_STRING ObjectName; ULONG Attributes; PVOID SecurityDescriptor; PVOID SecurityQualityOfService; } OBJECT_ATTRIBUTES, * POBJECT_ATTRIBUTES;
    using myNtCreateSection = NTSTATUS(NTAPI*)(OUT PHANDLE SectionHandle, IN ULONG DesiredAccess, IN POBJECT_ATTRIBUTES ObjectAttributes OPTIONAL, IN PLARGE_INTEGER MaximumSize OPTIONAL, IN ULONG PageAttributess, IN ULONG SectionAttributes, IN HANDLE FileHandle OPTIONAL);

    myNtCreateSection fNtCreateSection = (myNtCreateSection)(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtCreateSection"));

    SIZE_T size = 0x00100000;
    LARGE_INTEGER sectionSize = { size };
    HANDLE sectionHandle = NULL;

    fNtCreateSection(&sectionHandle, SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_MAP_EXECUTE, NULL, (PLARGE_INTEGER)&sectionSize, PAGE_EXECUTE_READWRITE, SEC_COMMIT, NULL);

    *(uint32_t*)(ctx.Esp + 0xC) = (uint32_t)sectionHandle;

    SetThreadContext(hThread, &ctx);
    ResumeThread(hThread);
    CloseHandle(hThread);
    return true;
}

uint32_t reverse_emulate(eoperation action, int size, uint32_t op1, uint32_t op2) {
    switch (action) {
    case eoperation::ROL: { switch (size) { case 1: return std::rotr<uint8_t>(op1, op2); case 2: return std::rotr<uint16_t>(op1, op2); case 4: return std::rotr<uint32_t>(op1, op2); } }
    case eoperation::ROR: { switch (size) { case 1: return std::rotl<uint8_t>(op1, op2); case 2: return std::rotl<uint16_t>(op1, op2); case 4: return std::rotl<uint32_t>(op1, op2); } }
    case eoperation::BSWAP: { switch (size) { case 1: return std::byteswap<uint8_t>(op1); case 2: return std::byteswap<uint16_t>(op1); case 4: return std::byteswap<uint32_t>(op1); } }
    case eoperation::INC: return op1 - 1;
    case eoperation::DEC: return op1 + 1;
    case eoperation::SUB: { switch (size) { case 1: return static_cast<uint8_t>(op1 + op2); case 2: return static_cast<uint16_t>(op1 + op2); case 4: return static_cast<uint32_t>(op1 + op2); } }
    case eoperation::ADD: { switch (size) { case 1: return static_cast<uint8_t>(op1 - op2); case 2: return static_cast<uint16_t>(op1 - op2); case 4: return static_cast<uint32_t>(op1 - op2); } }
    case eoperation::NOT: { switch (size) { case 1: return static_cast<uint8_t>(~op1); case 2: return static_cast<uint16_t>(~op1); case 4: return static_cast<uint32_t>(~op1); } }
    case eoperation::XOR: { switch (size) { case 1: return static_cast<uint8_t>(op1 ^ op2); case 2: return static_cast<uint16_t>(op1 ^ op2); case 4: return static_cast<uint32_t>(op1 ^ op2); } }
    case eoperation::NEG: { switch (size) { case 1: return static_cast<uint8_t>(0 - op1); case 2: return static_cast<uint16_t>(0 - op1); case 4: return static_cast<uint32_t>(0 - op1); } }
    }
    return op1;
}

uint32_t fnv1a(const std::wstring& data) {
    const uint32_t FNV_prime = 0x1000193;
    const uint32_t offset_basis = 0x811C9DC5;
    uint32_t hash = offset_basis;
    for (wchar_t c : data) { hash ^= c; hash *= FNV_prime; }
    return hash;
}

void SubtractMinutesFromSystemTime(int minutes, FILETIME& ft) {
    ULARGE_INTEGER li;
    li.LowPart = ft.dwLowDateTime;
    li.HighPart = ft.dwHighDateTime;
    const ULONGLONG intervalsPerMinute = 60 * 10000000ULL;
    li.QuadPart += (minutes * intervalsPerMinute);
    ft.dwLowDateTime = li.LowPart;
    ft.dwHighDateTime = li.HighPart;
}

void skeet_t::recompile()
{
    std::map<uint32_t, uint32_t> patches;

    patches[0x000000004349D3E2] = (u32)GetModuleHandleA("ntdll.dll");

    GetSystemTimeAsFileTime(&ftime);
    SubtractMinutesFromSystemTime(10, ftime);
    auto arg1 = ftime.dwLowDateTime - 0x0D53E8000;
    auto arg2 = ftime.dwHighDateTime - 0x19DB1DE;
    auto time_check = ((int(__stdcall*)(int arg1, int arg2, int arg3, int arg4))0x433AE31E)(arg1, arg2, 0x989680, 0);
    patches[0x000000004353C3D1] = time_check;

    auto crc_hash = ((int(__fastcall*)(int base, int size))0x4333D6A2)(0x4331E000, 0x105B12);
    patches[0x00000000434C6B29] = crc_hash ^ 0x92CA5DF1;

    patches[0x00000000434EB360] = GetCurrentProcessId();
    patches[0x00000000435507ED] = time_check;

    PPEB peb = (PPEB)(__readfsdword(0x30));
    auto ldr_entry = peb->Ldr;
    PLDR_DATA_TABLE_ENTRY entry = (PLDR_DATA_TABLE_ENTRY)ldr_entry->InLoadOrderModuleList->Flink;

    auto val = *(uint32_t*)((uint32_t)entry + 0x88) - 0xD53E8000;
    auto val2 = *(uint32_t*)((uint32_t)entry + 0x8c) - 0x19DB1DE;
    time_check = ((int(__stdcall*)(int arg1, int arg2, int arg3, int arg4))0x433AE31E)(val, val2, 0x989680, 0);
    patches[0x000000004352A5D6] = time_check;

    SetEnvironmentVariable(L"STEAMID", L"7612345678901");
    patches[0x000000004356C0A1] = fnv1a(L"7612345678901");
    patches[0x000000004352EE8F] = *(uint16_t*)0x7FFE0260;

    int cpu_info[4] = {};
    __cpuidex(cpu_info, 1, 0);

    patches[0x434bc39e] = cpu_info[2];
    patches[0x4356afc4] = cpu_info[2];
    patches[0x000000004352C91C] = cpu_info[2];

    patches[0x000000004351B2B5] = (u32)GetModuleHandleA("ntdll.dll");
    patches[0x434d4faa] = (u32)GetCurrentProcessId();

    uint32_t hash = 0;
    for (uint32_t i = 0x80000002; i <= 0x80000004; i++) {
        __cpuidex(cpu_info, i, 0);
        hash += cpu_info[0] + cpu_info[1] + cpu_info[2] + cpu_info[3];
    }

    auto it = std::find_if(removedHandlers.begin(), removedHandlers.end(), [](const removed_handler_t& other) {
        return other.vip == 0x000000004359257D;
        });

    it->value = hash;
    patches[0x43507C7F] = hash;
    patches[0x435194de] = (u32)GetModuleHandleA("kernel32.dll");

    uint32_t hashKUserSharedData = 0x811C9DC5;
    for (int i = 0; i < 0x40; i++) {
        unsigned char byte = *(unsigned char*)(0x7FFE0274 + i);
        uint32_t tmpValue = byte + i;
        tmpValue = tmpValue ^ hashKUserSharedData;
        hashKUserSharedData = tmpValue * 0x1000193;
    }

    patches[0x434eb732] = hashKUserSharedData;
    patches[0x435469B5] = (uint32_t)GetModuleHandleA("kernelbase.dll");

    for (int i = 0; i < handlers.size(); i++) {
        auto& handlers_info = handlers[i];

        if (patches.find(handlers_info[0].vip) != patches.end()) {
            handlers_info[0].value = patches[handlers_info[0].vip];
        }
        else {
            auto import_dest = handlers_info[0].value;

            if (exports.find(import_dest) != exports.end()) {
                auto& export_ = exports[import_dest];
                handlers_info[0].value = (u32)GetProcAddress(LoadLibraryA(export_.lib.c_str()), export_.name.c_str());
            }
        }

        uint32_t encryption_key = handlers_info[0].encryption_key;

        for (auto& handler_info : handlers_info) {
            if (!skeet_t::is_image_range(handler_info.vip)) continue;

            uint32_t value = handler_info.value;
            uint8_t size = handler_info.size;

            for (auto it = handler_info.operations.rbegin(); it != handler_info.operations.rend(); it++) {
                value = reverse_emulate(it->operation, size, value, it->value);
            }
            switch (size) {
            case 4: value ^= encryption_key; *(uint32_t*)handler_info.vip = value; break;
            case 2: value ^= static_cast<uint16_t>(encryption_key); *(uint16_t*)handler_info.vip = value; break;
            case 1: value ^= static_cast<uint8_t>(encryption_key); *(uint8_t*)handler_info.vip = value; break;
            }
            encryption_key ^= static_cast<uint32_t>(handler_info.value);
        }
    }
}

skeet_t* skeet_t::getInstance(HMODULE base)
{
    if (!singleton) singleton = new skeet_t(base);
    return singleton;
}

bool skeet_t::is_stack_range(u32 addr)
{
    auto skeet = getInstance();
    return addr >= 0x10D0000 && addr < 0x10D0000 + 0x100000;
}

bool skeet_t::is_image_range(u32 addr)
{
    auto skeet = getInstance();
    return addr >= skeet->base() && addr < skeet->base() + skeet->size();
}

bool skeet_t::is_exception(u32 addr)
{
    if (addr >= 0x434F8532 && addr <= 0x434F853A) return true;
    return false;
}