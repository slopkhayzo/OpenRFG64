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
#include "volition/rfg/code/gamewide/game_trace.h"
#include "volition/rfg/code/gamewide/gameseq.h"
#include "volition/rfg/code/hud/hud_ui_mission_complete.h"
#include "volition/vlib/code/math/crc.h"

#include <windows.h>

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

    int Mc_test_s = -1;
    unsigned long long Gameplay_since = 0;
    bool Shown = false;
    unsigned long long Screen_since = 0;
    int Next_dump = 0;
    const float Dump_times[] = {0.3f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.5f, 8.0f, 10.0f};
}

void memtest_ui_frame()
{
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
