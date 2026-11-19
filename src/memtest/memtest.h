// OpenRFG64 memory test (new code, not recovered; OpenRFG64-memtest research/docs/02-plan.md M1).
//
// Measures what limits a 64-bit OpenRFG under a heavy load: the process's memory, the game's fixed budgets (the
// memory layout's keen allocators, every vlib mempool), Havok's heap, the C runtime heap that replaces dlmalloc on x64,
// the far table of et_ptr_offset (32-bit self-relative offsets), the string tables' arena fallbacks and the counts
// held in 16-bit fields. It also starts a Wrecking Crew match from the command line (--wc), stands in for Sledge's
// loader for the mods enabled in a Sledge folder (--sledge), and loads x64 plugins (--plugins).
//
// Command line (read before WinMain, like main_options.cpp):
//   --wc MAP              start a Wrecking Crew match on the map with this file name (e.g. wcavenue) from the main menu
//   --wc-options HEX      the 44-byte wrecking_crew_options as hex (rfgphys's Harness.AutoStartOptions format); the map
//                         index in it is replaced by --wc's. Default: total chaos, 1 player, thermobaric rockets
//                         (weapon 4), no time limit
//   --sledge DIR          a Sledge folder (the game's sledge\): the mods enabled in its sledge.toml are applied
//   --plugins DIR         every *.asi in DIR is loaded at the start of WinMain (x64 plugins: rfgrt, rfgphys)
//   --memtest-dir DIR     where the census goes (default: OPENRFG_RUN_DIR, else the working directory)
//   --exit-after S        end the process S seconds after a Wrecking Crew match has started (unattended runs)
// Environment: OPENRFG_CONFIG_INI (config.ini path), OPENRFG_MEMTEST_INTERVALMS (census period, default 1000),
// OPENRFG_MEMTEST_<budget> overrides (memtest_budget).
#pragma once

#include <stddef.h>
#include <stdint.h>

class mempool_base;
struct cf_found_info;

// ---- lifecycle
void memtest_init();         // the start of WinMain: plugins, the Sledge folder, the output files
void memtest_frame();        // every frame (rfg_do_frame): the census and the Wrecking Crew start
void memtest_level_event(const char* what);

// ---- what the recovered code reports
void memtest_pool_register(mempool_base* pool);
void memtest_pool_unregister(mempool_base* pool);
void memtest_native_print(const char* text);                          // keen::debug::nativePrint
void memtest_event(const char* format, ...);                           // memtest_events.log (and the census marker)
void memtest_crt_alloc(size_t bytes);                                  // dlmalloc's x64 replacement
void memtest_crt_free(size_t bytes);
void memtest_strpool_fallback();                                       // string_pool.cpp: a record outside its arena
void memtest_strpool_arena_unregistered();                             // string_pool.cpp: no arena slot left
void memtest_havok_out_of_memory(unsigned int size, bool final_failure); // havok_memory.cpp
void memtest_level_alloc_failed(const void* manager, unsigned int size, const char* name); // split_memmgr.cpp

// ---- settings
const char* memtest_config_ini_path();                                 // OPENRFG_CONFIG_INI, or nullptr
// A budget override: OPENRFG_MEMTEST_<NAME> (bytes, or with a K/M/G suffix), else the default.
uint64_t memtest_budget(const char* name, uint64_t default_bytes);

// ---- the Wrecking Crew start
const char* memtest_wc_map();                                          // --wc, or nullptr
bool memtest_wc_continue();                                            // unattended: press the match's continue

// ---- the Sledge layer
// The folders to mount beside /gamebuild: returns the native path of mount i (and its mount name), nullptr past the
// last one.
const char* memtest_overlay_mount(int i, const char** mount_name);
// Called first by cf_find_file: a registered loose file becomes a standard hit on its mount.
bool memtest_overlay_find(cf_found_info* found_info, const char* filename);
// The Wrecking Crew map entries the enabled mods add: copies of an entry under another file name, in the table of
// dlc_id (-1: wrecking_crew.xtbl, N: dlcNN_wrecking_crew.xtbl).
struct memtest_wc_map_addition
{
    char file[64];
    char copy_of[64];
    int dlc_id;
};
int memtest_wc_map_additions(const memtest_wc_map_addition** additions);

// ---- accessors added to recovered files (their statics)
int openrfg_num_collision_models();                                    // havok_cm_manager.cpp
int openrfg_max_collision_models();
int openrfg_general_movers_in_use();                                   // general_mover.cpp
int openrfg_general_movers_capacity();
void openrfg_main_menu_select(int choice);                             // main_menu.cpp: main_menu_handle_selection
int openrfg_wc_menu_state();                                           // ui_document_menu_wrecking_crew.cpp

// ---- rfgphys's keep_debris in the source, and the automated demolition (memtest_debris.cpp)
// Settings: OPENRFG_EXPERIMENT_KEEPDEBRIS / DEBRISPOOL / RENDERINSTANCES / ZONERESERVE (rfgphys.ini's [Experiment]
// keys) and OPENRFG_MEMTEST_DEMOLISH / DEMOLISHCYCLES / DEMOLISHBLASTSPERFRAME. Off unless set.
unsigned int memtest_debris_pool(unsigned int game_size);              // general_mover.cpp: the pool's slots (static init)
void* memtest_debris_pool_storage(unsigned int bytes);                  // general_mover.cpp: the pool's storage (static init)
bool memtest_keeping_debris();                                         // KeepDebris, single player, the zone budget not short
bool memtest_fade_allowed();                                           // object_mover_mark_for_fade: kept debris does not fade
struct memtest_fade_scope                                              // callers whose fades go through (the game's cleanups)
{
    memtest_fade_scope();
    ~memtest_fade_scope();
};
int memtest_shatter_deletes(int pieces, int deletes);                  // rfgl_desired_values: the pieces a shatter deletes
bool memtest_refuse_vaporize();                                        // rfg_dyn_maybe_delete: no "vaporized" pieces
void memtest_stream_parent_gone();                                     // world::collect_sorted_list: a parent that is gone
void memtest_recompile_pool_empty();                                   // rl_start_recompile_mesh_instance: no request free
unsigned int memtest_batched_cap(unsigned int game);                   // rfgphys's BatchedCap: instances drawn batched a frame
unsigned int memtest_unbatched_cap(unsigned int game);                 // rfgphys's UnbatchedCap: drawn one by one a frame
void memtest_render_slot_skipped();                                    // rl_compiled_render_group::build: no transform
void memtest_havok_island_trim(int bodies, unsigned int need, unsigned int limit, int remove); // havok_sim_islands_process_post
void memtest_havok_low_memory();                                       // the watch dog's free-memory request
void memtest_render_limits();                                          // game_render_system_init: render instances, effect filters
void memtest_sim_frame();                                              // gamestate_gp_simulate_frame's end: zone budget, demolition
struct memtest_debris_counts
{
    int render_instances, render_instances_max, zone_free;
    unsigned int kept, gated, fades_blocked, vaporize_refused, recycled, stream_skips, blasts, recompiles_cancelled;
    unsigned int island_trims, bodies_trimmed, havok_low_memory;
    bool keep;
};
void memtest_debris_numbers(memtest_debris_counts* out);
