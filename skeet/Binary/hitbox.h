#pragma once
#include <Windows.h>
#include <cmath>
#include <cfloat>
#include <algorithm>

// ===========================================================================
// Hitbox selection system - v2 with damage prediction and scoring
// ===========================================================================

namespace hitbox {

    // ---------------------------------------------------------------------------
    // Math helpers
    // ---------------------------------------------------------------------------
    struct Vec3 {
        float x, y, z;
        Vec3() : x(0), y(0), z(0) {}
        Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

        Vec3 operator+(const Vec3& o) const { return { x + o.x, y + o.y, z + o.z }; }
        Vec3 operator-(const Vec3& o) const { return { x - o.x, y - o.y, z - o.z }; }
        Vec3 operator*(float s) const { return { x * s, y * s, z * s }; }

        float Length() const { return sqrtf(x * x + y * y + z * z); }
        float Dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }

        Vec3 Normalized() const {
            float len = Length();
            if (len < 1e-6f) return { 0, 0, 0 };
            return { x / len, y / len, z / len };
        }
    };

    struct QAngle {
        float pitch, yaw, roll;
        QAngle() : pitch(0), yaw(0), roll(0) {}
        QAngle(float p, float y, float r) : pitch(p), yaw(y), roll(r) {}

        void Normalize() {
            while (pitch > 180.0f) pitch -= 360.0f;
            while (pitch < -180.0f) pitch += 360.0f;
            while (yaw > 180.0f) yaw -= 360.0f;
            while (yaw < -180.0f) yaw += 360.0f;
            roll = 0.0f;
        }
    };

    // ---------------------------------------------------------------------------
    // Hitgroups
    // ---------------------------------------------------------------------------
    enum HitGroup : int {
        HITGROUP_GENERIC = 0,
        HITGROUP_HEAD = 1,
        HITGROUP_CHEST = 2,
        HITGROUP_STOMACH = 3,
        HITGROUP_LEFTARM = 4,
        HITGROUP_RIGHTARM = 5,
        HITGROUP_LEFTLEG = 6,
        HITGROUP_RIGHTLEG = 7,
        HITGROUP_NECK = 8,
        HITGROUP_GEAR = 10,
    };

    // ---------------------------------------------------------------------------
    // Bones
    // ---------------------------------------------------------------------------
    enum Bone : int {
        BONE_PELVIS = 0,
        BONE_SPINE = 3,
        BONE_SPINE1 = 4,
        BONE_SPINE2 = 5,
        BONE_SPINE3 = 6,
        BONE_NECK = 7,
        BONE_HEAD = 8,
        BONE_HEAD_TOP = 9,
    };

    // ---------------------------------------------------------------------------
    // Damage multipliers per hitgroup
    // ---------------------------------------------------------------------------
    inline float damage_multiplier(int hitgroup) {
        switch (hitgroup) {
        case HITGROUP_HEAD:     return 4.00f;
        case HITGROUP_CHEST:    return 1.00f;
        case HITGROUP_STOMACH:  return 1.25f;
        case HITGROUP_LEFTARM:
        case HITGROUP_RIGHTARM: return 1.00f;
        case HITGROUP_LEFTLEG:
        case HITGROUP_RIGHTLEG: return 0.75f;
        default:                return 1.00f;
        }
    }

    // ---------------------------------------------------------------------------
    // CS:GO weapon range modifiers (per 500 units)
    // ---------------------------------------------------------------------------
    inline float weapon_range_modifier(int weapon_id) {
        switch (weapon_id) {
        case 1:  return 0.81f;
        case 9:  return 0.99f;
        case 38: return 0.99f;
        case 40: return 0.99f;
        case 7:  return 0.98f;
        case 16: return 0.97f;
        case 60: return 0.99f;
        case 34: return 0.75f;
        case 17: return 0.82f;
        case 24: return 0.75f;
        case 19: return 0.84f;
        case 13: return 0.75f;
        case 10: return 0.98f;
        default: return 0.90f;
        }
    }

    // ---------------------------------------------------------------------------
    // CS:GO armor ratios
    // ---------------------------------------------------------------------------
    inline float weapon_armor_ratio(int weapon_id) {
        switch (weapon_id) {
        case 9:  return 0.98f;
        case 1:  return 0.93f;
        case 7:  return 0.775f;
        case 16: return 0.70f;
        case 60: return 0.70f;
        case 34: return 0.60f;
        case 17: return 0.59f;
        case 24: return 0.65f;
        case 19: return 0.69f;
        default: return 0.75f;
        }
    }

    // ---------------------------------------------------------------------------
    // CS:GO base weapon damage
    // ---------------------------------------------------------------------------
    inline float weapon_base_damage(int weapon_id) {
        switch (weapon_id) {
        case 9:  return 115.0f;
        case 38: return 80.0f;
        case 40: return 80.0f;
        case 1:  return 63.0f;
        case 7:  return 36.0f;
        case 16: return 33.0f;
        case 60: return 33.0f;
        case 34: return 26.0f;
        case 17: return 29.0f;
        case 24: return 35.0f;
        case 19: return 26.0f;
        case 13: return 30.0f;
        case 10: return 30.0f;
        case 8:  return 20.0f;
        case 32: return 35.0f;
        default: return 30.0f;
        }
    }

    // ---------------------------------------------------------------------------
    // Weapon profile
    // ---------------------------------------------------------------------------
    struct WeaponProfile {
        bool  head_priority;
        float min_damage;
        bool  allow_penetration;
        float prefer_distance;
    };

    inline WeaponProfile profile_for_weapon(int weapon_id) {
        switch (weapon_id) {
        case 9:  case 38: case 40:
            return { false, 80.0f, true,  1500.0f };
        case 1:
            return { true,  60.0f, true,  800.0f };
        case 7:  case 16: case 60:
            return { true,  30.0f, true,  700.0f };
        case 34: case 17: case 24: case 19:
            return { false, 25.0f, false, 400.0f };
        default:
            return { false, 20.0f, false, 500.0f };
        }
    }

    // ---------------------------------------------------------------------------
    // Full damage prediction
    // ---------------------------------------------------------------------------
    inline float predict_damage(int weapon_id, float distance,
        int hitgroup, bool target_has_armor)
    {
        float base = weapon_base_damage(weapon_id);
        float range_mod = weapon_range_modifier(weapon_id);
        float after_range = base * powf(range_mod, distance / 500.0f);

        float after_armor = after_range;
        if (target_has_armor) {
            after_armor *= weapon_armor_ratio(weapon_id);
            after_armor -= 0.5f;
        }

        float after_hitgroup = after_armor * damage_multiplier(hitgroup);
        return (after_hitgroup < 0.0f) ? 0.0f : after_hitgroup;
    }

    // ---------------------------------------------------------------------------
    // Candidate
    // ---------------------------------------------------------------------------
    struct Candidate {
        Vec3  point;
        int   hitgroup;
        float angle;
        float distance;
        float damage;
        float score;
        bool  visible;
        bool  backtrack;
    };

    // ---------------------------------------------------------------------------
    // Callbacks
    // ---------------------------------------------------------------------------
    using BonePosFn = Vec3(*)(int player, int bone);
    using VecToAnglesFn = QAngle(*)(const Vec3& dir);
    using VisibleFn = bool(*)(const Vec3& from, const Vec3& to, int player);
    using EyePosFn = Vec3(*)();
    using ViewAnglesFn = QAngle(*)();
    using WeaponIdFn = int(*)();
    using TargetInfoFn = bool(*)(int player, bool* has_armor, float* health);

    // ---------------------------------------------------------------------------
    // Context
    // ---------------------------------------------------------------------------
    struct Context {
        int             target_player;
        BonePosFn       get_bone_pos;
        VecToAnglesFn   vec_to_angles;
        VisibleFn       is_visible;
        EyePosFn        get_eye_pos;
        ViewAnglesFn    get_view_angles;
        WeaponIdFn      get_weapon_id;
        TargetInfoFn    get_target_info;
        float           backtrack_age_ms;
        float           head_radius;
        float           max_range;
        float           max_view_angle;
    };

    // ---------------------------------------------------------------------------
    // Angle math
    // ---------------------------------------------------------------------------
    inline float angle_between(const Vec3& eye, const Vec3& point,
        const QAngle& view, VecToAnglesFn to_angles)
    {
        Vec3 dir = (point - eye).Normalized();
        QAngle aim = to_angles(dir);
        QAngle delta(aim.pitch - view.pitch, aim.yaw - view.yaw, 0.0f);
        delta.Normalize();
        return sqrtf(delta.pitch * delta.pitch + delta.yaw * delta.yaw);
    }

    inline float angle_to_sphere(const Vec3& eye, const Vec3& center, float radius,
        const QAngle& view, VecToAnglesFn to_angles)
    {
        float d = (center - eye).Length();
        if (d < 1.0f) d = 1.0f;
        float half_angle = atanf(radius / d) * (180.0f / 3.14159265358979323846f);
        float center_angle = angle_between(eye, center, view, to_angles);
        return (std::max)(0.0f, center_angle - half_angle);
    }

    // ---------------------------------------------------------------------------
    // Build a candidate
    // ---------------------------------------------------------------------------
    inline bool make_candidate(const Context& ctx,
        int bone, int hitgroup, float radius,
        const Vec3& eye, const QAngle& view,
        const WeaponProfile& profile,
        int weapon_id, bool target_has_armor,
        Candidate& out)
    {
        Vec3 pos = ctx.get_bone_pos(ctx.target_player, bone);
        if (pos.x == 0 && pos.y == 0 && pos.z == 0)
            return false;

        out.point = pos;
        out.hitgroup = hitgroup;
        out.distance = (pos - eye).Length();
        out.angle = angle_to_sphere(eye, pos, radius, view, ctx.vec_to_angles);
        out.visible = ctx.is_visible(eye, pos, ctx.target_player);
        out.backtrack = ctx.backtrack_age_ms > 0.5f;

        if (out.distance > ctx.max_range)                return false;
        if (out.angle > ctx.max_view_angle)           return false;
        if (!out.visible && !profile.allow_penetration)  return false;

        out.damage = predict_damage(weapon_id, out.distance, hitgroup, target_has_armor);
        if (out.damage < profile.min_damage)             return false;

        out.score = -FLT_MAX;
        return true;
    }

    // ---------------------------------------------------------------------------
    // Scoring
    // ---------------------------------------------------------------------------
    inline float score_candidate(const Candidate& c, const WeaponProfile& profile)
    {
        float score = c.damage / (c.angle + 0.5f);

        if (profile.head_priority && c.hitgroup == HITGROUP_HEAD)
            score *= 1.5f;

        if (!c.visible)
            score *= 0.35f;

        if (c.backtrack)
            score *= 0.70f;

        score += 5.0f / (c.angle + 1.0f);

        float dist_delta = fabsf(c.distance - profile.prefer_distance);
        score += 20.0f / (dist_delta + 100.0f);

        return score;
    }

    // ---------------------------------------------------------------------------
    // Result
    // ---------------------------------------------------------------------------
    struct Selection {
        bool  valid;
        Vec3  point;
        int   hitgroup;
        float score;
        float damage;
    };

    // ---------------------------------------------------------------------------
    // Main entry
    // ---------------------------------------------------------------------------
    inline Selection select_best(const Context& ctx)
    {
        Selection result{};
        result.valid = false;
        result.score = -FLT_MAX;

        int weapon_id = ctx.get_weapon_id();
        WeaponProfile profile = profile_for_weapon(weapon_id);

        bool  target_has_armor = false;
        float target_health = 100.0f;
        if (!ctx.get_target_info(ctx.target_player, &target_has_armor, &target_health))
            return result;

        if (target_health <= 0.0f)
            return result;

        Vec3   eye = ctx.get_eye_pos();
        QAngle view = ctx.get_view_angles();

        Candidate candidates[16];
        Candidate c;
        int n = 0;

        // Head: 3 points
        Vec3 head = ctx.get_bone_pos(ctx.target_player, BONE_HEAD);
        Vec3 head_top = head + Vec3(0, 0, ctx.head_radius * 0.6f);
        Vec3 head_bot = head - Vec3(0, 0, ctx.head_radius * 0.6f);
        Vec3 head_pts[3] = { head, head_top, head_bot };

        for (int i = 0; i < 3; ++i) {
            c.point = head_pts[i];
            c.hitgroup = HITGROUP_HEAD;
            c.distance = (c.point - eye).Length();
            c.angle = angle_to_sphere(eye, c.point, ctx.head_radius, view, ctx.vec_to_angles);
            c.visible = ctx.is_visible(eye, c.point, ctx.target_player);
            c.backtrack = ctx.backtrack_age_ms > 0.5f;
            c.damage = predict_damage(weapon_id, c.distance, HITGROUP_HEAD, target_has_armor);
            c.score = -FLT_MAX;

            if (c.distance > ctx.max_range)                 continue;
            if (c.angle > ctx.max_view_angle)            continue;
            if (!c.visible && !profile.allow_penetration)   continue;
            if (c.damage < profile.min_damage)            continue;

            candidates[n++] = c;
        }

        // Body points
        struct BodyPoint { int bone; int hitgroup; float radius; };
        static const BodyPoint body_pts[] = {
            { BONE_NECK,   HITGROUP_NECK,    4.0f },
            { BONE_SPINE3, HITGROUP_CHEST,   8.0f },
            { BONE_SPINE2, HITGROUP_CHEST,   9.0f },
            { BONE_SPINE1, HITGROUP_CHEST,   9.0f },
            { BONE_SPINE,  HITGROUP_STOMACH, 9.0f },
            { BONE_PELVIS, HITGROUP_STOMACH, 8.0f },
        };

        for (auto& bp : body_pts) {
            if (make_candidate(ctx, bp.bone, bp.hitgroup, bp.radius,
                eye, view, profile, weapon_id, target_has_armor, c))
                candidates[n++] = c;
            if (n >= 15) break;
        }

        for (int i = 0; i < n; ++i) {
            candidates[i].score = score_candidate(candidates[i], profile);
            if (candidates[i].score > result.score) {
                result.score = candidates[i].score;
                result.point = candidates[i].point;
                result.hitgroup = candidates[i].hitgroup;
                result.damage = candidates[i].damage;
                result.valid = true;
            }
        }

        return result;
    }

} // namespace hitbox