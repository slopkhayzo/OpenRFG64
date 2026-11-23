// memtest (OpenRFG64-memtest): the mission results screen without finishing a mission, and a dump of its document.
//
// Memtest.McTest=N (OPENRFG_MEMTEST_MCTEST): N seconds after gameplay first starts, the results screen of a won mission
// is shown, as a mission's end shows it (hud_ui_mission_complete_start), for Memtest.McMission (OPENRFG_MEMTEST_MCMISSION,
// default "Intro 1"). While the screen is up, its document (rfg_ui_mission_complete) goes to the game trace ("mcdump")
// at set times: each element with its visibility, alpha, scale, anchor, offset, screen size and text, and each
// animation and tween of the document with its state, times, target and values. Written for the reward rows that do
// not show at a mission's end (research/docs/08-mission-rewards.md).

#include "memtest/memtest.h"
#include "volition/interface_lib/code/objects/animation/vint_object_animation.h"
#include "volition/interface_lib/code/objects/animation/vint_object_tween.h"
#include "volition/interface_lib/code/objects/vint_element_base.h"
#include "volition/interface_lib/code/objects/vint_object_base.h"
#include "volition/interface_lib/code/objects/vint_object_factory.h"
#include "volition/interface_lib/code/objects/vint_object_types.h"
#include "volition/interface_lib/code/vint_document.h"
#include "volition/interface_lib/code/vint_variant.h"
#include "volition/rfg/code/gameplay/mission_info.h"
#include "volition/rfg/code/gamewide/game_save.h"
#include "volition/rfg/code/gamewide/game_trace.h"
#include "volition/rfg/code/gamewide/gameseq.h"
#include "volition/rfg/code/gamewide/verify_gamesave.h"
#include "volition/rfg/code/hud/hud_ui_mission_complete.h"
#include "volition/rfg/code/level/world.h"
#include "volition/vlib/code/math/crc.h"

#include <windows.h>
#include <tlhelp32.h> // Memtest.BmWriteWatch: the process's threads

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
    // The document's object names (its string table), to name the hashes the objects keep.
    const char* const Names[] = {
        "mc_screen_main", "mc_morale_main", "mc_morale_equation", "mc_morale_multiplier", "mc_morale_percentage",
        "mc_morale_x", "mc_morale_text2", "mc_morale_text1", "mc_morale_num", "mc_morale_num_bg", "mc_morale_bg1",
        "mc_morale_bg2", "mc_morale_bg3", "mc_morale_bg4", "mc_morale_dags", "mc_morale_bg5", "mc_morale_logo",
        "mc_morale_logo_shadow", "mc_morale_salvage", "mc_morale_salvage_bg", "mc_mission_title_main", "mc_logo_bg",
        "new_bitmap5", "mc_mission_bg_mask", "mc_header_bg_group", "mc_header_bg_7", "mc_header_bg_6", "mc_header_bg_5",
        "mc_header_bg_4", "mc_header_bg_3", "mc_header_bg_2", "mc_header_bg_1", "mc_text_mask", "mc_mission_title",
        "mc_mission_title_shadow", "mc_sub_text_mask", "mc_mission_dags1", "mc_mission_dags2", "mc_mission_label",
        "mc_mission_label_shadow", "mc_mission_subtext", "mc_mission_subtext_shadow", "mc_mission_sector",
        "mc_mission_sector_shadow", "mc_mission_dirt_group", "new_bitmap1", "new_bitmap2", "new_bitmap3", "mc_hints_main",
        "mc_hint_bg4", "mc_hint_group_2", "mc_hint_2", "mc_hint_2_text", "mc_hint_bg2", "mc_hint_bg1", "mc_hint_bg3",
        "mc_hint_group_1", "mc_hint_1", "mc_hint_1_text", "mc_credits_main", "mc_credits_text2", "mc_credits_text1",
        "mc_credits_num", "mc_credits_num_bg", "mc_credits_bg1", "mc_credits_bg2", "mc_credits_bg3", "mc_credits_bg4",
        "mc_credits_dags", "mc_credits_salvage", "mc_credits_salvage_bg", "mc_credits_bg5", "mc_total_main",
        "mc_total_text2", "mc_total_num", "mc_total_num_bg", "mc_total_bg1", "mc_total_bg2", "mc_total_bg3", "mc_total_bg4",
        "mc_total_dags", "mc_total_text1", "new_bitmap10", "mc_total_salvage", "mc_total_salvage_bg", "mc_dust_main",
        "mc_dirt_group1", "mc_dirt_group2", "mc_dirt_group3", "mc_dust_group_1", "mc_dust_group_2", "mc_dust_group_3",
        "mc_failed_anim", "mc_text_mask_clip_size_twn_4", "mc_text_mask_clip_size_twn_3", "mc_sub_text_mask_clip_size_twn_2",
        "mc_sub_text_mask_clip_size_twn_1", "mc_mission_bg_mask_clip_size_twn_2", "mc_mission_bg_mask_clip_size_twn_1",
        "mc_hints_main_alpha_twn_1", "mc_hints_main_alpha_twn_12", "mc_logo_bg_scale_twn_12", "mc_logo_bg_alpha_twn_12",
        "mc_logo_bg_anim_1", "mc_dust_group_1_anchor_twn_1", "mc_dust_group_1_alpha_twn_1", "mc_logo_bg_scale_twn_1",
        "mc_logo_bg_alpha_twn_1", "mc_mission_subtext_alpha_twn_1", "mc_mission_subtext_alpha_twn_2",
        "mc_credits_main_alpha_twn_1", "mc_credits_main_alpha_twn_2", "mc_credits_main_scale_twn_2",
        "mc_morale_main_alpha_twn_1", "mc_morale_main_alpha_twn_2", "mc_morale_main_scale_twn_2",
        "mc_total_main_alpha_twn_1", "mc_total_main_alpha_twn_2", "mc_total_main_scale_twn_2",
        "mc_dust_group_1_anchor_twn_2", "mc_dust_group_2_alpha_twn_1", "mc_dust_group_2_anchor_twn_1",
        "mc_dust_group_2_anchor_twn_2", "mc_dust_group_3_alpha_twn_1", "mc_dust_group_3_anchor_twn_1",
        "mc_dust_group_3_anchor_twn_2", "mc_dirt_group1_alpha_twn_1", "mc_dirt_group1_scale_twn_1",
        "mc_dirt_group2_alpha_twn_1", "mc_dirt_group2_scale_twn_1", "mc_dirt_group3_alpha_twn_1",
        "mc_dirt_group3_scale_twn_1", "mc_mission_bg_mask_clip_size_tw", "mc_text_mask_clip_size_twn_1",
        "mc_text_mask_clip_size_twn_2", "mc_sub_text_mask_clip_size_twn_", "mc_mission_dirt_group_alpha_twn",
        "mc_mission_dirt_group_scale_twn", "mc_screen_main_anchor_twn_1", "mc_screen_main_anchor_twn_2",
        "mc_screen_main_anchor_twn_3", "mc_screen_main_anchor_twn_4", "mc_screen_main_anchor_twn_5",
        "mc_screen_main_anchor_twn_6", "mc_screen_main_anchor_twn_7", "mc_screen_main_anchor_twn_8",
        "mc_screen_main_anchor_twn_9", "mc_screen_main_anchor_twn_10", "mc_screen_main_anchor_twn_11",
        "mc_screen_main_anchor_twn_12", "mc_hint_bg1_alpha_twn_1", "mc_hint_bg1_alpha_twn_2", "mc_hint_bg2_alpha_twn_1",
        "mc_hint_bg2_alpha_twn_2", "mc_hint_bg3_alpha_twn_1", "mc_hint_bg3_alpha_twn_2", "hint_clip_clip_size_twn_1",
        "hint_clip_clip_size_twn_2", "hint2_clip_clip_size_twn_1", "hint2_clip_clip_size_twn_2", "safe_frame",
        "mission_dags_group", "hint_clip", "hint2_clip"};

    const char* name_of(unsigned int crc, char* buffer, size_t size)
    {
        for (const char* name : Names)
        {
            if (crc_stri(name, 0) == crc)
            {
                return name;
            }
        }
        _snprintf_s(buffer, size, _TRUNCATE, "#%08X", crc);
        return buffer;
    }

    int env_int(const char* name, int fallback)
    {
        char value[32];
        const DWORD n = GetEnvironmentVariableA(name, value, sizeof(value));
        return n > 0 && n < sizeof(value) ? atoi(value) : fallback;
    }

    // The properties an object has, of those below, as " name=value" pairs.
    void describe(const vint_object_base* obj, char* out, size_t size)
    {
        static const char* const Props[] = {"visible", "alpha", "scale", "anchor", "offset", "screen_size", "screen_nw",
                                            "depth", "tint", "render_mode", "auto_offset", "res_offset_x", "res_offset_y",
                                            "res_scale_x", "res_scale_y", "mask", "text_tag", "text_tag_crc", "clip_size",
                                            "state", "start_time", "duration", "target_name_crc", "target_property",
                                            "start_value", "end_value", "loop_mode", "reverse", "is_paused"};
        size_t used = 0;
        out[0] = '\0';
        for (const char* prop : Props)
        {
            vint_variant value;
            if (!obj->get_property_value(prop, value, 0))
            {
                continue;
            }
            char text[160];
            if (strcmp(prop, "target_name_crc") == 0)
            {
                char hex[16];
                strncpy_s(text, name_of((unsigned int)value, hex, sizeof(hex)), _TRUNCATE);
            }
            else if (value.get_type() == VINT_PROP_TYPE_ENUM)
            {
                _snprintf_s(text, sizeof(text), _TRUNCATE, "%d", (int)value);
            }
            else
            {
                value.to_string(text, sizeof(text));
            }
            const int n = _snprintf_s(out + used, size - used, _TRUNCATE, " %s=%s", prop, text);
            if (n < 0)
            {
                return;
            }
            used += (size_t)n;
        }
    }

    void dump_object(const vint_object_base* obj, int depth, float t)
    {
        char hex[16];
        char props[1024];
        const vint_object_type_info* info = vint_object_types_get_type_info(obj->get_type());
        describe(obj, props, sizeof(props));
        GAME_TRACE("mcdump", "t=%.2f %*s%s (%s)%s", t, depth * 2, "", name_of(obj->get_name_crc(), hex, sizeof(hex)),
                   info != nullptr && info->name != nullptr ? info->name : "?", props);
    }

    void dump_tree(const vint_object_base* obj, int depth, float t)
    {
        for (; obj != nullptr; obj = obj->get_next_sibling())
        {
            dump_object(obj, depth, t);
            dump_tree(obj->get_first_child(), depth + 1, t);
        }
    }

    const vint_document* Dump_doc = nullptr;
    float Dump_t = 0.0f;

    // The document's animations and tweens (they are not in the element tree), as the tween processor finds them.
    bool dump_if_in_doc(vint_object_base* obj)
    {
        if (obj != nullptr && obj->get_parent_document() == Dump_doc)
        {
            dump_object(obj, 1, Dump_t);
        }
        return true;
    }

    void dump(const vint_document* doc, float t)
    {
        GAME_TRACE("mcdump", "t=%.2f document %s: time index %.2f", t, doc->get_name(), doc->get_time_index());
        dump_tree(doc->get_root_element(), 0, t);
        Dump_doc = doc;
        Dump_t = t;
        const unsigned int types[2] = {vint_object_animation::Object_type, vint_object_tween::Object_type};
        for (unsigned int type : types)
        {
            const vint_object_type_info* info = vint_object_types_get_type_info(type);
            if (info != nullptr && info->factory != nullptr)
            {
                GAME_TRACE("mcdump", "t=%.2f the document's %s objects:", t, info->name != nullptr ? info->name : "?");
                info->factory->for_each_active(dump_if_in_doc);
            }
        }
        Dump_doc = nullptr;
    }

    // ---- Memtest.Reload=N (OPENRFG_MEMTEST_RELOAD): every N seconds of gameplay a save is loaded the way the in-game
    // Load Game screen loads one (verify_gamesave_start), Memtest.ReloadCount times (OPENRFG_MEMTEST_RELOADCOUNT, default 3),
    // the slots taken in turn from Memtest.ReloadSlots (OPENRFG_MEMTEST_RELOADSLOTS, default "3,2"). For what a level's
    // reload leaves behind: the bitmap lists your campaign session found broken after two in-game loads
    // (research/docs/10-reload-texture-crash.md).
    int Reload_s = -1;
    int Reloads_left = 0;
    int Reloads_done = 0;
    unsigned long long Reload_gameplay_since = 0;
    int Reload_slots[12];
    int Reload_slot_count = 0;

    void reload_test(unsigned long long now, game_state state)
    {
        if (Reload_s < 0)
        {
            Reload_s = env_int("OPENRFG_MEMTEST_RELOAD", 0);
            Reloads_left = env_int("OPENRFG_MEMTEST_RELOADCOUNT", 3);
            char slots[64] = "3,2";
            GetEnvironmentVariableA("OPENRFG_MEMTEST_RELOADSLOTS", slots, sizeof(slots));
            for (const char* p = slots; *p != '\0' && Reload_slot_count < 12;)
            {
                Reload_slots[Reload_slot_count++] = atoi(p);
                while (*p != '\0' && *p != ',')
                {
                    p++;
                }
                if (*p == ',')
                {
                    p++;
                }
            }
        }
        if (Reload_s <= 0 || Reloads_left <= 0 || Reload_slot_count == 0)
        {
            return;
        }
        if (state != GS_GAMEPLAY || gameseq_in_transition())
        {
            Reload_gameplay_since = 0;
            return;
        }
        if (Reload_gameplay_since == 0)
        {
            Reload_gameplay_since = now;
            return;
        }
        if (now - Reload_gameplay_since < (unsigned long long)Reload_s * 1000ull)
        {
            return;
        }
        Reload_gameplay_since = 0;
        const int slot = Reload_slots[Reloads_done % Reload_slot_count];
        // Magic number: the save file has 12 slots (game_save_generic.cpp's Num_save_slots), as command_line_load_slot.
        static game_save_info Saves[12];
        World.dlc_bundle_id = -1;
        const int count = game_save_get_list(Saves, game_save_get_list_buf_size(12), 12, false);
        for (int i = 0; i < count; i++)
        {
            if (Saves[i].used_slot && (int)Saves[i].save_slot_index == slot)
            {
                Reloads_done++;
                Reloads_left--;
                memtest_event("reload test: loading save slot %d from gameplay (load %d, %d more to come)", slot, Reloads_done, Reloads_left);
                GAME_TRACE("memtest", "reload test: loading save slot %d from gameplay (load %d)", slot, Reloads_done);
                verify_gamesave_start(&Saves[i], false);
                return;
            }
        }
        memtest_event("reload test: no save in slot %d: the test ends", slot);
        Reloads_left = 0;
    }

    int Mc_test_s = -1;
    unsigned long long Gameplay_since = 0;
    bool Shown = false;
    unsigned long long Screen_since = 0;
    int Next_dump = 0;
    const float Dump_times[] = {0.3f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.5f, 8.0f, 10.0f};
}

void bm_memtest_watch_lists(int first, int last); // bmpman.cpp (memtest): Memtest.BmWatch
void* bm_memtest_shown_next_field(int handle);   // bmpman.cpp (memtest): Memtest.BmWriteWatch
const char* bm_memtest_peg_of(const void* p);    // bmpman.cpp (memtest)

namespace
{
    // ---- Memtest.BmWriteWatch=h[,h...] (OPENRFG_MEMTEST_BMWRITEWATCH, up to 4 bitmaps): from the first gameplay frame,
    // a hardware write watch (debug registers DR0-DR3, on every thread, armed again each second for new threads) on
    // the link (next offset) of the entry each bitmap shows. In the reload runs the always_loaded entries' links of
    // bitmaps 6-11 were all rewritten during the second in-game load's teardown, by no list edit of bmpman
    // (research/docs/10-reload-texture-crash.md). Each write is written to the game trace ("bmwrite") with the
    // instruction after it, the value and what it points to, and the program addresses on the writing thread's stack
    // (tools/re/pdb_addr.py names them). Writes from the kernel (a file read into that memory) do not trap.
    void* Ww_fields[4] = {};
    int Ww_handles[4] = {};
    int Ww_count = 0;
    volatile long Ww_hits = 0;
    uintptr_t Ww_image_lo = 0;
    uintptr_t Ww_image_hi = 0;

    LONG CALLBACK ww_handler(EXCEPTION_POINTERS* info)
    {
        if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        CONTEXT* c = info->ContextRecord;
        int which = -1;
        for (int k = 0; k < Ww_count; k++)
        {
            if ((c->Dr6 & (1ull << k)) != 0)
            {
                which = k;
                break;
            }
        }
        if (which < 0)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        c->Dr6 = 0;
        if (_InterlockedIncrement(&Ww_hits) <= 60)
        {
            char stack[600] = "";
            size_t len = 0;
            const unsigned long long* sp = (const unsigned long long*)c->Rsp;
            for (int i = 0, found = 0; i < 160 && found < 16 && len + 24 < sizeof(stack); i++)
            {
                const unsigned long long v = sp[i]; // the thread's own stack: readable
                if (v >= Ww_image_lo && v < Ww_image_hi)
                {
                    len += (size_t)snprintf(stack + len, sizeof(stack) - len, " exe+0x%llX", v - Ww_image_lo);
                    found++;
                }
            }
            const int value = *(const int*)Ww_fields[which];
            const char* target = value == 0 || value == -1 ? nullptr : (const char*)Ww_fields[which] + value;
            GAME_TRACE("bmwrite", "bitmap %d: the link of its shown entry (%p, %s) written: offset %d -> %p (%s); thread %lu, the instruction "
                       "after the write at %s0x%llX; program addresses on its stack:%s",
                       Ww_handles[which], Ww_fields[which], bm_memtest_peg_of(Ww_fields[which]), value, (const void*)target,
                       target != nullptr ? bm_memtest_peg_of(target) : "-", GetCurrentThreadId(),
                       c->Rip >= Ww_image_lo && c->Rip < Ww_image_hi ? "exe+" : "", (unsigned long long)(c->Rip >= Ww_image_lo && c->Rip < Ww_image_hi ? c->Rip - Ww_image_lo : c->Rip),
                       stack);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Arms the watch on one thread (suspended meanwhile; nothing here allocates).
    void ww_arm(DWORD id)
    {
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, id);
        if (t == nullptr)
        {
            return;
        }
        if (SuspendThread(t) != (DWORD)-1)
        {
            CONTEXT c = {};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &c))
            {
                DWORD64* drs[4] = {&c.Dr0, &c.Dr1, &c.Dr2, &c.Dr3};
                DWORD64 dr7 = c.Dr7;
                bool change = false;
                for (int k = 0; k < Ww_count; k++)
                {
                    if (*drs[k] != (DWORD64)(uintptr_t)Ww_fields[k])
                    {
                        *drs[k] = (DWORD64)(uintptr_t)Ww_fields[k];
                        change = true;
                    }
                    // Lk on; RWk = 01 (writes), LENk = 11 (4 bytes)
                    const DWORD64 mask = (3ull << (k * 2)) | (0xFull << (16 + k * 4));
                    dr7 = (dr7 & ~mask) | (1ull << (k * 2)) | (1ull << (16 + k * 4)) | (3ull << (18 + k * 4));
                }
                if (dr7 != c.Dr7)
                {
                    c.Dr7 = dr7;
                    change = true;
                }
                if (change)
                {
                    SetThreadContext(t, &c);
                }
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }

    DWORD WINAPI ww_installer(void*)
    {
        const DWORD self = GetCurrentThreadId();
        const DWORD process = GetCurrentProcessId();
        static DWORD ids[4096];
        for (;;)
        {
            int n = 0;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap != INVALID_HANDLE_VALUE)
            {
                THREADENTRY32 te;
                te.dwSize = sizeof(te);
                for (BOOL ok = Thread32First(snap, &te); ok && n < 4096; ok = Thread32Next(snap, &te))
                {
                    if (te.th32OwnerProcessID == process && te.th32ThreadID != self)
                    {
                        ids[n++] = te.th32ThreadID;
                    }
                }
                CloseHandle(snap);
            }
            for (int i = 0; i < n; i++)
            {
                ww_arm(ids[i]);
            }
            Sleep(1000);
        }
    }

    void ww_start()
    {
        static bool tried = false;
        if (tried)
        {
            return;
        }
        tried = true;
        char value[64];
        const DWORD n = GetEnvironmentVariableA("OPENRFG_MEMTEST_BMWRITEWATCH", value, sizeof(value));
        if (n == 0 || n >= sizeof(value))
        {
            return;
        }
        for (const char* p = value; *p != 0 && Ww_count < 4;)
        {
            const int handle = atoi(p);
            void* field = bm_memtest_shown_next_field(handle);
            if (field != nullptr)
            {
                Ww_handles[Ww_count] = handle;
                Ww_fields[Ww_count] = field;
                Ww_count++;
                GAME_TRACE("bmwrite", "watching bitmap %d's shown entry's link at %p (%s), now offset %d", handle, field, bm_memtest_peg_of(field),
                           *(const int*)field);
            }
            const char* comma = strchr(p, ',');
            if (comma == nullptr)
            {
                break;
            }
            p = comma + 1;
        }
        if (Ww_count == 0)
        {
            return;
        }
        const unsigned char* base = (const unsigned char*)GetModuleHandleA(nullptr);
        const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
        Ww_image_lo = (uintptr_t)base;
        Ww_image_hi = Ww_image_lo + nt->OptionalHeader.SizeOfImage;
        AddVectoredExceptionHandler(1, ww_handler);
        HANDLE t = CreateThread(nullptr, 0, ww_installer, nullptr, 0, nullptr);
        if (t != nullptr)
        {
            CloseHandle(t);
        }
        memtest_event("bm write watch: %d fields watched on every thread (debug registers)", Ww_count);
    }
}

void memtest_ui_frame()
{
    reload_test(GetTickCount64(), gameseq_get_state());
    // Memtest.BmWatch=first,last (OPENRFG_MEMTEST_BMWATCH): those bitmaps' entry lists, written each time one changes.
    static int Bm_watch_first = -2;
    static int Bm_watch_last = -1;
    if (Bm_watch_first == -2)
    {
        Bm_watch_first = -1;
        char value[32];
        const DWORD n = GetEnvironmentVariableA("OPENRFG_MEMTEST_BMWATCH", value, sizeof(value));
        if (n > 0 && n < sizeof(value))
        {
            Bm_watch_first = atoi(value);
            const char* comma = strchr(value, ',');
            Bm_watch_last = comma != nullptr ? atoi(comma + 1) : Bm_watch_first;
        }
    }
    if (Bm_watch_first >= 0)
    {
        bm_memtest_watch_lists(Bm_watch_first, Bm_watch_last);
    }
    if (gameseq_get_state() == GS_GAMEPLAY)
    {
        ww_start(); // Memtest.BmWriteWatch, from the first gameplay frame
    }
    if (Mc_test_s < 0)
    {
        Mc_test_s = env_int("OPENRFG_MEMTEST_MCTEST", 0);
    }
    if (Mc_test_s <= 0)
    {
        return;
    }
    const unsigned long long now = GetTickCount64();
    const game_state state = gameseq_get_state();
    if (!Shown)
    {
        if (state != GS_GAMEPLAY)
        {
            return;
        }
        if (Gameplay_since == 0)
        {
            Gameplay_since = now;
        }
        if (now - Gameplay_since < (unsigned long long)Mc_test_s * 1000ull)
        {
            return;
        }
        Shown = true;
        char name[64] = "Intro 1";
        GetEnvironmentVariableA("OPENRFG_MEMTEST_MCMISSION", name, sizeof(name));
        const mission_info* mi = mission_info_get(name);
        const bool started = mi != nullptr && hud_ui_mission_complete_start(true, mi);
        memtest_event("mctest: the results screen of mission \"%s\" (%s): %s", name, mi != nullptr ? "found" : "not found",
                      started ? "shown" : "not shown");
        GAME_TRACE("mcdump", "the results screen of mission \"%s\": %s", name, started ? "shown" : "not shown");
        return;
    }
    if (state != GS_INGAME_MISSION_COMPLETE || Next_dump >= (int)(sizeof(Dump_times) / sizeof(Dump_times[0])))
    {
        return;
    }
    if (Screen_since == 0)
    {
        Screen_since = now;
    }
    const float t = (float)(now - Screen_since) / 1000.0f;
    if (t < Dump_times[Next_dump])
    {
        return;
    }
    Next_dump++;
    if (const vint_document* doc = vint_document::find("rfg_ui_mission_complete"))
    {
        dump(doc, t);
    }
    else
    {
        GAME_TRACE("mcdump", "t=%.2f the document is not loaded", t);
    }
}
