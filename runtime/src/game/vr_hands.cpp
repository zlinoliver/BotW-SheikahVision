// VR: what the player holds in each hand (BetterVR hooking/weapon.cpp hook_ChangeWeaponMtx, which records the same
// from the same place). Every frame the game binds each carried item's model to a bone of its holder's model (the
// function at 0x03125758, by the look of it ModelBindInfo's calc: r31 the bind info, its bone name at +0x18, r29 the
// holder's gsys model, r30 the item actor). The player's items on Weapon_R / Weapon_L are what is in his hands; a
// binding not renewed for a few frames has been let go of. Used by the gestures (vr_gesture.cpp): throwing a weapon,
// the magnet glove. Dropping (vr_gesture.cpp asks, from the host input): done here, on the game's thread, as BetterVR's
// patch_CTRL_HookWeaponHands.asm does from the same place: the player's ActorWeapons (actor vtable +0x314), the equip
// slot holding the item (hook_FindWeaponEquipSlot: six BaseProcLinks of 0x10, a live link's proc +0x40 is the actor),
// Weapon's drop (vtable +0x57c, at its own position) and ActorWeapons::resetBaseProc (0x033bcd9c) for that slot.
#include <atomic>
#include <cstring>

#include "hooks.h"
#include "vr.h"

using mem::r32;

const char* vr_actor_name(uint32_t actor) { return actor ? mem::str(r32(actor + 4)) : nullptr; }

namespace {
std::atomic<uint32_t> g_item[2];        // 0 left hand (Weapon_L), 1 right (Weapon_R)
std::atomic<uint64_t> g_item_frame[2];  // the frame it was last bound
std::atomic<bool> g_drop[2];            // a drop asked for (vr_gesture.cpp)

// BetterVR isDroppable: not the special weapons (the Master Sword's forms, story items) nor what is not a weapon
bool droppable(const char* n) {
    if (!n || strncmp(n, "Weapon_", 7) != 0) return false;
    for (const char* keep : {"Weapon_Bow_071", "Weapon_Sword_056", "Weapon_Sword_070", "Weapon_Sword_080",
                             "Weapon_Sword_081", "Weapon_Sword_502"})
        if (strcmp(n, keep) == 0) return false;
    return true;
}

void drop(PPCContext* c, uint32_t player, uint32_t item) {
    const char* name = vr_actor_name(item);
    if (!droppable(name)) {
        LOG("vr: %s is not dropped", name ? name : "?");
        return;
    }
    const uint32_t r3 = c->r[3], r4 = c->r[4];
    const double f1 = c->f[1];
    const uint32_t weapons = guest_call(c, r32(r32(player + 0xe8) + 0x314), {player});
    int slot = -1;
    for (int i = 0; weapons && i < 6 && slot < 0; i++) {
        const uint32_t link = weapons + i * 0x10, proc = r32(link), id = r32(link + 4);
        if (proc && id != 0xffffffffu && r32(proc + 0x3c) == id && r32(proc + 0x40) == item) slot = i;
    }
    if (slot >= 0) {
        const uint32_t sp = c->r[1];
        c->r[1] = (sp - 0x40) & ~0xfu;
        const uint32_t at = c->r[1] + 0x20;  // where it falls from: where it is
        for (int k = 0; k < 3; k++) mem::wf32(at + 4 * k, mem::rf32(item + 0x1f8 + 12 + 16 * k));
        guest_call(c, r32(r32(item + 0xe8) + 0x57c), {item, at, 0, 0, 0, 0});
        guest_call(c, 0x033bcd9c, {weapons, (uint32_t)slot});
        c->r[1] = sp;
        LOG("vr: dropped %s (equip slot %d)", name, slot);
    } else {
        LOG("vr: %s is in no equip slot, not dropped", name);
    }
    c->r[3] = r3, c->r[4] = r4, c->f[1] = f1;
}
}  // namespace

// The item in a hand (its actor), 0 when empty
uint32_t vr_hand_item(int hand) {
    if (vr::stereo_frames() - g_item_frame[hand].load(std::memory_order_relaxed) > 4) return 0;
    return g_item[hand].load(std::memory_order_relaxed);
}

// vr_aim.cpp (a throw leaving the hand): stereo frames since the hand's item was last bound
uint32_t vr_hand_bound_age(int hand) {
    return (uint32_t)(vr::stereo_frames() - g_item_frame[hand].load(std::memory_order_relaxed));
}

// mr r4, r30 just before the bind's last call (bl 0x031253f0)
BOTW_HOOK(0x03125878) {
    const uint32_t player = vr::player_actor();
    if (!player || c->r[29] != r32(player + 0x330)) return;  // ActorWiiU's gsys model
    const char* bone = mem::str(r32(c->r[31] + 0x18));
    if (!bone || strncmp(bone, "Weapon_", 7) != 0 || (bone[7] != 'L' && bone[7] != 'R')) return;
    const int hand = bone[7] == 'R';
    const uint32_t item = c->r[30];
    if (g_drop[hand].exchange(false, std::memory_order_relaxed)) drop(c, player, item);
    if (g_item[hand].exchange(item, std::memory_order_relaxed) != item) {
        const char* name = vr_actor_name(item);
        LOG("vr: %s hand holds %s", hand ? "right" : "left", name ? name : "?");
    }
    g_item_frame[hand].store(vr::stereo_frames(), std::memory_order_relaxed);
}

// vr_gesture.cpp: drop what a hand holds (done at its next binding, on the game's thread)
void vr_drop_hand_item(int hand) { g_drop[hand].store(true, std::memory_order_relaxed); }
