// memtest (OpenRFG64-memtest): rfgphys's keep_debris experiment, ported into OpenRFG64's source, and an automated
// demolition for unattended runs.
//
// rfgphys patches rfg.exe so that the debris of the destruction stays (research/docs/01 and 02 of this project):
// shattering keeps its pieces, the load governor deletes nothing, pieces do not fade out, the general_mover pool and
// the destroyable render instances are bigger, and a budget keeps some object handles free in every world zone (the
// farthest debris is recycled below it). Its patches go through hooks at fixed addresses of rfg.exe and through the
// static initialization of the game; OpenRFG64 has the game's source, so here the same settings are read by the
// recovered code at the places rfgphys hooks (each call is marked "// memtest" there):
//
//   Experiment.KeepDebris=1          OPENRFG_EXPERIMENT_KEEPDEBRIS      keep the debris (single player)
//   Experiment.DebrisPool=32768      OPENRFG_EXPERIMENT_DEBRISPOOL      general_mover slots (game 3,072; at most 65,000)
//   Experiment.RenderInstances=65000 OPENRFG_EXPERIMENT_RENDERINSTANCES destroyable render instances (game 2,100); with
//                                    grow-on-demand memory on, where they start: they grow when they run out
//   Experiment.ZoneReserve=1200      OPENRFG_EXPERIMENT_ZONERESERVE     handles kept free in every world zone
//
// The values above are the ones of the installed rfgphys.ini. The run script passes them with -Set (or -KeepDebris).
//
// Memtest.Demolish=1 (OPENRFG_MEMTEST_DEMOLISH): once a Wrecking Crew match is in the player's turn, vehicle bombs go
// off through every building's box, building by building, nearest first, the way rfgphys's Test.StressPieces
// demolishes in rfg.exe; a new cycle then takes what still stands (big chunks included). The player is made
// invulnerable. Memtest.DemolishCycles (default 6) and Memtest.DemolishBlastsPerFrame (default 4).
//
// A generated map's ground slab (tools/maps: the avenues' plateau, a building of indestructible tiles) is made
// terrain to Havok, so that Havok's debris land on it (ground_slab_frame; Memtest.GroundAsTerrain=0 leaves it a
// building). Diagnostics: Memtest.FloorProbe and Memtest.FloorY (floor_frame).

#include "memtest/memtest.h"
#include "scaling/scaling.h" // scaling

#include "volition/rfg/code/camera/rfg_camera.h"
#include "volition/rfg/code/code_effects/explosion.h"
#include "volition/rfg/code/code_effects/explosion_shared.h"
#include "volition/rfg/code/destruction/rfg_mover.h"
#include "volition/rfg/code/gameplay/wrecking_crew/wrecking_crew.h"
#include "volition/rfg/code/gamewide/game_render.h"
#include "volition/rfg/code/gamewide/gameseq.h"
#include "volition/rfg/code/havok/havok.h"
#include "volition/rfg/code/havok/havok_raycast.h" // the floor probe
#include "volition/rfg/code/destruction/rfg_shapes.h" // the ground slab: its elements
#include "volition/libmodules/havok/hk2014_2_5_r1/source/physics2012/collide/agent/collidable/hkpcollidable.h"
#include "volition/libmodules/havok/hk2014_2_5_r1/source/physics2012/dynamics/entity/hkprigidbody.h"
#include "volition/rfg/code/level/world.h"
#include "volition/rfg/code/level/world_internal.h"
#include "volition/rfg/code/multi/multi_core/include/multi.h"
#include "volition/rfg/code/objects/humans/player.h"
#include "volition/rfg/code/objects/movers/general_mover.h"
#include "volition/rfg/code/objects/movers/object_mover.h"
#include "volition/render_lib/code/rl_compiled_render_group.h"
#include "volition/render_lib/code/rl_destroyable_model_instance.h"
#include "volition/render_lib/code/rl_factory_rtti.h"
#include "volition/render_lib/code/rl_fp_kdtree.h"
#include "volition/render_lib/code/rl_scene.h"
#include "volition/render_lib/code/rl_scene_renderer.h"
#include "volition/vfxlib/code/vfx_particle_system_filter_instance.h"
#include "volition/vlib/code/math/matrix.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

namespace
{
    const int Zone_handles = 7280; // handle_manager<7280> in every world_zone
    const int Recycle_batch = 64; // recycle at least this many at a time
    const unsigned int Max_debris_pool = 65000; // the pool links its elements with 16-bit indices
    // scaling (OpenRFG64-memtest): rfgphys found that rfg.exe does nothing with more than 65,000 ("16-bit"); OpenRFG64 numbers
    // them with nothing narrower than an int (the factory's pool is a list of pointers, the transforms' index an int). So
    // the setting is only where they start, and with grow-on-demand memory they grow up to the ceiling as they run out.
    const int Max_render_instances = 1 << 22;
    const int Render_instances_ceiling = 1 << 22;
    const int Vfx_filter_instances = 4000; // rfgphys: the game crashes when its 500 run out (vfx_effect::init)

    // The settings: read once (the first call may come from a static initializer: plain globals only).
    bool Settings_read;
    bool Keep;
    int Zone_reserve = 1200;
    unsigned int Debris_pool;
    int Render_instances;
    bool Demolish;
    int Demolish_cycles = 6;
    int Demolish_per_frame = 4;
    bool Ground_as_terrain = true; // Memtest.GroundAsTerrain (ground_slab_frame)

    int env_int(const char* name, int default_value)
    {
        char value[32];
        const DWORD n = GetEnvironmentVariableA(name, value, sizeof(value));
        return n > 0 && n < sizeof(value) ? atoi(value) : default_value;
    }

    void read_settings()
    {
        if (Settings_read)
        {
            return;
        }
        Settings_read = true;
        Keep = env_int("OPENRFG_EXPERIMENT_KEEPDEBRIS", 0) != 0;
        const int reserve = env_int("OPENRFG_EXPERIMENT_ZONERESERVE", 1200);
        Zone_reserve = reserve < 200 ? 200 : reserve > 5000 ? 5000 : reserve;
        const int pool = env_int("OPENRFG_EXPERIMENT_DEBRISPOOL", 0);
        Debris_pool = pool <= 0 ? 0 : (unsigned int)pool > Max_debris_pool ? Max_debris_pool : (unsigned int)pool;
        const int instances = env_int("OPENRFG_EXPERIMENT_RENDERINSTANCES", 0);
        Render_instances = instances <= 0 ? 0 : instances > Max_render_instances ? Max_render_instances : instances;
        Demolish = env_int("OPENRFG_MEMTEST_DEMOLISH", 0) != 0;
        Demolish_cycles = env_int("OPENRFG_MEMTEST_DEMOLISHCYCLES", 6);
        const int per_frame = env_int("OPENRFG_MEMTEST_DEMOLISHBLASTSPERFRAME", 4);
        Demolish_per_frame = per_frame < 1 ? 1 : per_frame > 32 ? 32 : per_frame;
        Ground_as_terrain = env_int("OPENRFG_MEMTEST_GROUNDASTERRAIN", 1) != 0;
    }

    std::atomic<int> Zone_free{ Zone_handles }; // free handles in the fullest zone, less the pieces kept since
    int Zone_used; // the fullest zone's handles in use at the last frame end
    thread_local int Fade_scope; // > 0: inside a caller whose fades go through
    bool Recycling; // sim thread: our own fade marks
    bool Recycle_logged;
    bool Settings_logged;

    std::atomic<unsigned int> Fades_blocked, Deletes_kept, Deletes_gated, Vaporize_refused, Recycled, Stream_skips, Recompiles_cancelled,
        Render_slots_skipped, Island_trims, Bodies_trimmed, Havok_low_memory;
    unsigned int Blasts;

    bool single_player()
    {
        return !Multiplayer;
    }

    void log_settings()
    {
        if (Settings_logged)
        {
            return;
        }
        Settings_logged = true;
        if (Keep || Debris_pool != 0 || Render_instances != 0)
        {
            memtest_event("debris: KeepDebris %d, DebrisPool %u, RenderInstances %d, ZoneReserve %d (rfgphys's keep_debris, in the source)",
                          Keep ? 1 : 0, Debris_pool, Render_instances, Zone_reserve);
        }
        if (Demolish)
        {
            memtest_event("demolish: on (%d cycles, %d vehicle bombs a frame)", Demolish_cycles, Demolish_per_frame);
        }
    }

    // ---- the zone budget (keep_debris's update_budget and recycle)

    bool object_visible(object_mover* mover)
    {
        if (!object_mover_is_visible(mover))
        {
            return false;
        }
        vector bmin;
        vector bmax;
        mover->get_bbox(bmin, bmax);
        return game_render_is_object_visible(bmin, bmax);
    }

    struct candidate
    {
        float d2;
        general_mover* gm;
    };
    std::vector<candidate> Candidates; // sim thread

    unsigned int Rng = 0x9E3779B9u;

    // Frees `need` handles in the zone with the given gid: the debris farthest from the camera first (pieces already
    // fading or destroyed count).
    void recycle(unsigned short gid, int need)
    {
        const int n = World.get_num_objects<general_mover>();
        int in_zone = 0;
        int leaving = 0;
        Candidates.clear();
        const vector& cam = Rfg_camera.real_pos;
        for (int i = 0; i < n; i++)
        {
            general_mover* gm = World.get_object<general_mover>(i);
            if (gm == nullptr || (gm->get_handle() >> 16) != gid)
            {
                continue;
            }
            in_zone++;
            if ((gm->mover_flags & 0x4) == 0) // load balanced: debris the governor may delete
            {
                continue;
            }
            if (gm->obj_flags.destroyed || object_mover_is_fading_for_delete(gm))
            {
                leaving++;
                continue;
            }
            const float dx = gm->pos.x - cam.x;
            const float dy = gm->pos.y - cam.y;
            const float dz = gm->pos.z - cam.z;
            Candidates.push_back({ dx * dx + dy * dy + dz * dz, gm });
        }
        if (!Recycle_logged)
        {
            Recycle_logged = true;
            memtest_event("debris: world zone %u is %d handles short of its reserve: %d general movers in it, %d of them debris (%d already leaving); recycling the farthest",
                          (unsigned int)gid, need, in_zone, (int)Candidates.size() + leaving, leaving);
        }
        need -= leaving;
        if (need <= 0 || Candidates.empty())
        {
            return;
        }
        const size_t take = (size_t)need < Candidates.size() ? (size_t)need : Candidates.size();
        std::nth_element(Candidates.begin(), Candidates.begin() + (ptrdiff_t)(take - 1), Candidates.end(),
                         [](const candidate& a, const candidate& b) { return a.d2 > b.d2; });
        // Havok out of memory: the game removes instead of fading (rfgl_find_and_delete_object_mover).
        const bool remove_all = havok_get_status(HAVOK_SYSTEM_MEMORY) == HAVOK_STATUS_ERROR;
        unsigned int done = 0;
        Recycling = true;
        for (size_t i = 0; i < take; i++)
        {
            general_mover* gm = Candidates[i].gm;
            if (gm->has_script_point_effect(0xFFFFFFFF, nullptr))
            {
                continue;
            }
            if (!remove_all && object_visible(gm))
            {
                Rng = Rng * 1664525u + 1013904223u;
                object_mover_mark_for_fade(gm, (int)((Rng >> 8) % 2000u)); // like the game: within 2 s
            }
            else
            {
                World.destroy_object(gm, false);
            }
            done++;
        }
        Recycling = false;
        Recycled.fetch_add(done, std::memory_order_relaxed);
    }

    void update_budget()
    {
        int used = 0;
        const world_zone* zone = World.memtest_fullest_zone(&used);
        Zone_used = used;
        const int free = Zone_handles - used;
        Zone_free.store(free, std::memory_order_relaxed);
        if (Keep && zone != nullptr && free < Zone_reserve && single_player())
        {
            recycle(zone->get_gid(), Zone_reserve + Recycle_batch - free);
        }
    }

    // ---- the demolition (rfgphys's Test.StressPieces, simplified)

    const float Min_volume = 300.0f; // m3: buildings and big chunks
    const float Blast_step_xz = 7.0f;
    const float Blast_step_y = 6.0f;
    const int Max_blasts_per_building = 160;
    const float Player_clearance = 12.0f; // m between the player and a blast
    const unsigned long long Start_after_ms = 10000; // after the player's turn begins
    const unsigned long long Building_gap_ms = 3000;
    const unsigned long long Cycle_gap_ms = 15000;

    struct target
    {
        unsigned int handle;
        float d2;
    };
    std::vector<target> Targets; // this cycle's buildings, nearest first
    std::vector<vector> Blast_queue; // the current building's blasts
    size_t Target_at;
    size_t Blast_at;
    int Cycle;
    unsigned long long Turn_since;
    unsigned long long Next_ms;
    bool Demolish_done;
    const explosion_info* Bomb;

    float box_volume(const vector& bmin, const vector& bmax)
    {
        return (bmax.x - bmin.x) * (bmax.y - bmin.y) * (bmax.z - bmin.z);
    }

    // The standing rfg_movers of Min_volume or more, nearest the player first.
    int pick_targets(const vector& from)
    {
        Targets.clear();
        const int n = World.get_num_objects<rfg_mover>();
        for (int i = 0; i < n; i++)
        {
            rfg_mover* rm = World.get_object<rfg_mover>(i);
            if (rm == nullptr || rm->obj_flags.destroyed)
            {
                continue;
            }
            vector bmin;
            vector bmax;
            rm->get_bbox(bmin, bmax);
            if (box_volume(bmin, bmax) < Min_volume)
            {
                continue;
            }
            const float dx = rm->pos.x - from.x;
            const float dz = rm->pos.z - from.z;
            Targets.push_back({ rm->get_handle(), dx * dx + dz * dz });
        }
        std::sort(Targets.begin(), Targets.end(), [](const target& a, const target& b) { return a.d2 < b.d2; });
        Target_at = 0;
        return (int)Targets.size();
    }

    // A grid of blasts through the building's box, top first.
    bool queue_building(rfg_mover* rm, const vector& player_pos)
    {
        vector lo;
        vector hi;
        rm->get_bbox(lo, hi);
        int nx = (int)ceilf((hi.x - lo.x) / Blast_step_xz);
        int ny = (int)ceilf((hi.y - lo.y) / Blast_step_y);
        int nz = (int)ceilf((hi.z - lo.z) / Blast_step_xz);
        nx = nx < 1 ? 1 : nx;
        ny = ny < 1 ? 1 : ny;
        nz = nz < 1 ? 1 : nz;
        while (nx * ny * nz > Max_blasts_per_building)
        {
            if (nx >= ny && nx >= nz && nx > 1)
            {
                nx--;
            }
            else if (nz >= ny && nz > 1)
            {
                nz--;
            }
            else
            {
                ny--;
            }
        }
        Blast_queue.clear();
        Blast_at = 0;
        for (int iy = ny - 1; iy >= 0; iy--)
        {
            for (int ix = 0; ix < nx; ix++)
            {
                for (int iz = 0; iz < nz; iz++)
                {
                    vector p;
                    p.x = lo.x + (hi.x - lo.x) * ((float)ix + 0.5f) / (float)nx;
                    p.y = lo.y + (hi.y - lo.y) * ((float)iy + 0.5f) / (float)ny;
                    p.z = lo.z + (hi.z - lo.z) * ((float)iz + 0.5f) / (float)nz;
                    const float dx = p.x - player_pos.x;
                    const float dy = p.y - player_pos.y;
                    const float dz = p.z - player_pos.z;
                    if (dx * dx + dy * dy + dz * dz >= Player_clearance * Player_clearance)
                    {
                        Blast_queue.push_back(p);
                    }
                }
            }
        }
        return !Blast_queue.empty();
    }

    void demolish_frame()
    {
        if (!Demolish || Demolish_done || Multiplayer || !wrecking_crew_is_active() || wrecking_crew_get_state() != WCS_PLAYERS_TURN)
        {
            return;
        }
        player* pp = player_get_local_player();
        if (pp == nullptr)
        {
            return;
        }
        const unsigned long long now = GetTickCount64();
        if (Turn_since == 0)
        {
            Turn_since = now;
            Next_ms = now + Start_after_ms;
            return;
        }
        if (now < Next_ms)
        {
            return;
        }
        if (Bomb == nullptr)
        {
            Bomb = explosion_lookup("vehicle_bomb");
            if (Bomb == nullptr)
            {
                memtest_event("demolish: no vehicle_bomb explosion in this game data; off");
                Demolish_done = true;
                return;
            }
            player_set_player_invulnerable(pp, true);
        }
        // The current building's blasts, a few a frame.
        if (Blast_at < Blast_queue.size())
        {
            for (int k = 0; k < Demolish_per_frame && Blast_at < Blast_queue.size(); k++)
            {
                explosion_create(Bomb, nullptr, nullptr, Blast_queue[Blast_at++], IDENTITY_MATRIX, nullptr, nullptr, false);
                Blasts++;
            }
            if (Blast_at >= Blast_queue.size())
            {
                Next_ms = now + Building_gap_ms;
            }
            return;
        }
        // The next building of the cycle, or the next cycle.
        while (Target_at < Targets.size())
        {
            const target t = Targets[Target_at++];
            rfg_mover* rm = World.object_from_handle<rfg_mover>(t.handle);
            if (rm != nullptr && !rm->obj_flags.destroyed && queue_building(rm, pp->pos))
            {
                return;
            }
        }
        if (Cycle >= Demolish_cycles)
        {
            memtest_event("demolish: done after %d cycles, %u vehicle bombs", Cycle, Blasts);
            Demolish_done = true;
            return;
        }
        Cycle++;
        const int n = pick_targets(pp->pos);
        memtest_event("demolish: cycle %d: %d standing buildings and big chunks (%u bombs so far, %d world objects, %d render instances in use)",
                      Cycle, n, Blasts, World.get_num_objects(), rl_destroyable_model_instance::get_objects_used());
        if (n == 0)
        {
            memtest_event("demolish: nothing left standing after %u vehicle bombs", Blasts);
            Demolish_done = true;
            return;
        }
        Next_ms = now + (Cycle == 1 ? 0 : Cycle_gap_ms);
    }

    // ---- the floor checks (diagnostics, off unless set): does Havok have a map's floor, do debris stay on it
    // Memtest.FloorProbe=x0,z0,x1,z1,step,top (OPENRFG_MEMTEST_FLOORPROBE): 15 s into the match, vertical rays every
    // `step` metres over the box, from top + 20 m to top - 60 m, against the static level and against destroyable
    // geometry: floor_probe.csv in the run folder (each ray's nearest hit of both) and an event with the counts: the
    // first surface from above at the floor (top, within 0.5 m), above it (a building), below it (under the floor) or
    // none. Memtest.FloorY=Y (OPENRFG_MEMTEST_FLOORY): every 5 s, the debris (load-balanced general movers) below Y,
    // with a few positions (pieces that went through a map's floor).
    bool Floor_read;
    bool Probe_on;
    bool Probe_done;
    float Probe_box[6]; // x0, z0, x1, z1, step, top
    bool Floor_y_on;
    float Floor_y;
    unsigned long long Floor_start_ms;
    unsigned long long Floor_last_ms;
    int Floor_last_below = -1;
    int Floor_lines;

    float env_float(const char* name, bool* set)
    {
        char value[32];
        const DWORD n = GetEnvironmentVariableA(name, value, sizeof(value));
        *set = n > 0 && n < sizeof(value);
        return *set ? (float)atof(value) : 0.0f;
    }

    // The y of a vertical ray's nearest hit, or -1e9 for none.
    float ray_hit_y(float x, float z, float y0, float y1, bool destroyable)
    {
        raycast_input in;
        in.start = vector(x, y0, z);
        in.end = vector(x, y1, z);
        in.radius = 0.0f;
        in.filter_params = nullptr;
        in.filter_callback = nullptr;
        in.m_hit_collector_type = raycast_input::COLLECT_CLOSEST_HIT;
        in.m_flags = 0;
        raycast_output out;
        const bool hit = destroyable ? havok_raycast_against_destroyable_geometry(in, out) : havok_raycast_against_static_level_only(in, out);
        if (!hit || out.m_hits.getSize() == 0)
        {
            return -1e9f;
        }
        float best = 2.0f;
        for (int i = 0; i < out.m_hits.getSize(); i++)
        {
            best = out.m_hits[i].hit_fraction < best ? out.m_hits[i].hit_fraction : best;
        }
        return y0 + (y1 - y0) * best;
    }

    void floor_probe()
    {
        const float x0 = Probe_box[0], z0 = Probe_box[1], x1 = Probe_box[2], z1 = Probe_box[3];
        const float step = Probe_box[4] >= 0.5f ? Probe_box[4] : 0.5f, top = Probe_box[5];
        const int nx = (int)((x1 - x0) / step) + 1;
        const int nz = (int)((z1 - z0) / step) + 1;
        if (nx <= 0 || nz <= 0 || (long long)nx * nz > 200000)
        {
            memtest_event("floor probe: %d x %d rays: not run (a box of at most 200,000 rays)", nx, nz);
            return;
        }
        char path[MAX_PATH];
        const DWORD n = GetEnvironmentVariableA("OPENRFG_RUN_DIR", path, MAX_PATH - 20);
        strcpy_s(path + (n > 0 && n < MAX_PATH - 20 ? n : 0), 20, n > 0 && n < MAX_PATH - 20 ? "\\floor_probe.csv" : "floor_probe.csv");
        FILE* f = nullptr;
        fopen_s(&f, path, "w");
        if (f != nullptr)
        {
            fprintf(f, "x,z,static_y,destroyable_y\n");
        }
        int at_floor = 0, above = 0, below = 0, none = 0, logged = 0;
        float bx0 = 1e9f, bz0 = 1e9f, bx1 = -1e9f, bz1 = -1e9f; // the box of the rays that miss the floor
        const unsigned long long t0 = GetTickCount64();
        for (int i = 0; i < nx; i++)
        {
            for (int j = 0; j < nz; j++)
            {
                const float x = x0 + step * (float)i;
                const float z = z0 + step * (float)j;
                const float s = ray_hit_y(x, z, top + 20.0f, top - 60.0f, false);
                const float d = ray_hit_y(x, z, top + 20.0f, top - 60.0f, true);
                if (f != nullptr)
                {
                    fprintf(f, "%.2f,%.2f,", x, z);
                    s > -1e8f ? fprintf(f, "%.3f,", s) : fprintf(f, ",");
                    d > -1e8f ? fprintf(f, "%.3f\n", d) : fprintf(f, "\n");
                }
                const float y = s > d ? s : d;
                if (y < -1e8f || y < top - 0.5f)
                {
                    y < -1e8f ? none++ : below++;
                    bx0 = x < bx0 ? x : bx0;
                    bz0 = z < bz0 ? z : bz0;
                    bx1 = x > bx1 ? x : bx1;
                    bz1 = z > bz1 ? z : bz1;
                    if (logged < 12)
                    {
                        logged++;
                        memtest_event("floor probe: no floor at (%.1f, %.1f): static level %s%.2f, destroyable geometry %s%.2f", x, z,
                                      s > -1e8f ? "" : "none ", s > -1e8f ? s : 0.0f, d > -1e8f ? "" : "none ", d > -1e8f ? d : 0.0f);
                    }
                }
                else if (y > top + 0.5f)
                {
                    above++;
                }
                else
                {
                    at_floor++;
                }
            }
        }
        if (f != nullptr)
        {
            fclose(f);
        }
        memtest_event("floor probe: %d rays every %.1f m over x %.0f..%.0f, z %.0f..%.0f (%llu ms): the floor at %.1f %d, above it %d, under it %d, nothing %d%s",
                      nx * nz, step, x0, x1, z0, z1, GetTickCount64() - t0, top, at_floor, above, below, none, below + none > 0 ? "; misses within:" : "");
        if (below + none > 0)
        {
            memtest_event("floor probe: misses within x %.1f..%.1f, z %.1f..%.1f (floor_probe.csv has every ray)", bx0, bx1, bz0, bz1);
        }
    }

    // The debris' collision types (their bodies' filter infos), counted: the first 6 distinct ones.
    struct type_tally
    {
        unsigned int info[6];
        int count[6];
        int other;

        void add(unsigned int v)
        {
            for (int k = 0; k < 6; k++)
            {
                if (count[k] == 0)
                {
                    info[k] = v;
                }
                if (info[k] == v)
                {
                    count[k]++;
                    return;
                }
            }
            other++;
        }

        void print(char* out, size_t size) const
        {
            size_t used = 0;
            out[0] = '\0';
            for (int k = 0; k < 6 && count[k] > 0 && used < size; k++)
            {
                const int w = snprintf(out + used, size - used, " %08X:%d", info[k], count[k]);
                used += w > 0 ? (size_t)w : 0;
            }
            if (other > 0 && used < size)
            {
                snprintf(out + used, size - used, " other:%d", other);
            }
        }
    };

    void floor_census()
    {
        const int n = World.get_num_objects<general_mover>();
        int debris = 0, below = 0;
        char where[256] = "";
        size_t used = 0;
        type_tally all = {};
        type_tally under = {};
        for (int i = 0; i < n; i++)
        {
            general_mover* gm = World.get_object<general_mover>(i);
            if (gm == nullptr || (gm->mover_flags & 0x4) == 0 || gm->obj_flags.destroyed)
            {
                continue;
            }
            debris++;
            hkpRigidBody* rb = gm->havok_handle != 0xFFFFFFFF ? havok_body_get_pointer(gm->havok_handle) : nullptr;
            const unsigned int info = rb != nullptr ? rb->getCollidable()->getCollisionFilterInfo() : 0xFFFFFFFFu;
            all.add(info);
            if (gm->pos.y < Floor_y)
            {
                if (below < 4 && used < sizeof(where) - 48)
                {
                    const int w = snprintf(where + used, sizeof(where) - used, " (%.1f, %.1f, %.1f)", gm->pos.x, gm->pos.y, gm->pos.z);
                    used += w > 0 ? (size_t)w : 0;
                }
                under.add(info);
                below++;
            }
        }
        if (below != Floor_last_below && Floor_lines < 120)
        {
            Floor_lines++;
            Floor_last_below = below;
            char all_types[160];
            char under_types[160];
            all.print(all_types, sizeof(all_types));
            under.print(under_types, sizeof(under_types));
            memtest_event("floor: %d of %d debris below y %.1f%s%s; collision types of all:%s; below:%s", below, debris, Floor_y,
                          below > 0 ? ", e.g." : "", where, all_types, under_types);
        }
    }

    // ---- a map's ground slab is terrain to Havok
    // In Wrecking Crew every small piece of debris is world-only (COLLIDE_RFG_WORLD_ONLY: layer 44, system group 6),
    // and the game's layer table lets world-only pieces meet the terrain and characters only: they fall through
    // buildings onto the terrain. The avenues' ground is a building (tools/maps rfg_gen.make_ground: a fixed mover of
    // indestructible tiles, COLLIDE_RFG_INITIAL_POSITION, layer 39 like every building), so Havok's debris fell through
    // it, onto wc2's terrain under it or past it for good. PhysX's debris met it (rfgphys mirrors it). Letting
    // world-only pieces meet every fixed mover (as rfgphys's DebrisCollide does) put them in contact with the buildings
    // as well: 17 fps became 2.7 on ten avenues. So the slab alone gets the terrain's collision type (COLLIDE_WORLD):
    // debris land on it as on terrain, and whatever meets terrain meets it. It is found by its elements (dlods), named
    // "ground" or "groundN" by the generator (and "apronN": THE AVENUE's, the PhysX project's build), whose name hash is
    // zlib's CRC-32 of the name. Every 2 s the movers are looked at again (a level's load, the game setting a type
    // back). Memtest.GroundAsTerrain=0 (OPENRFG_MEMTEST_GROUNDASTERRAIN) leaves the slab a building. Kept debris that
    // goes through terrain as well (Havok, keep debris: 40-75% on vanilla wc2 under the demolition) is another matter.
    unsigned int Ground_hashes[17]; // "ground", "ground0" .. "ground15"
    unsigned int Apron_hashes[16];  // "apron0" .. "apron15" (THE AVENUE's ground carries on past the slab in aprons)
    bool Ground_hashes_made;
    unsigned long long Ground_next_ms;
    int Ground_lines;

    unsigned int crc32_of(const char* s)
    {
        unsigned int c = 0xFFFFFFFFu;
        for (; *s != '\0'; s++)
        {
            c ^= (unsigned char)*s;
            for (int k = 0; k < 8; k++)
            {
                c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
            }
        }
        return ~c;
    }

    // 1: an element named "ground" or "groundN", 2: "apronN", 0: neither.
    int ground_element(unsigned int h)
    {
        if (!Ground_hashes_made)
        {
            Ground_hashes_made = true;
            Ground_hashes[0] = crc32_of("ground");
            char name[16];
            for (int i = 0; i < 16; i++)
            {
                snprintf(name, sizeof(name), "ground%d", i);
                Ground_hashes[i + 1] = crc32_of(name);
                snprintf(name, sizeof(name), "apron%d", i);
                Apron_hashes[i] = crc32_of(name);
            }
        }
        for (const unsigned int g : Ground_hashes)
        {
            if (g == h)
            {
                return 1;
            }
        }
        for (const unsigned int a : Apron_hashes)
        {
            if (a == h)
            {
                return 2;
            }
        }
        return 0;
    }

    // Every element of its shape named "ground", "groundN" or "apronN", one of them at least a ground.
    bool is_ground_shape(const rfg_destroyable_shape* shape)
    {
        if (shape == nullptr || shape->parent == nullptr || shape->parent->num_dlods <= 0)
        {
            return false;
        }
        const rfg_dlod_base* dlods = shape->parent->base_dlods.to_pointer();
        bool ground = false;
        for (int i = 0; i < shape->parent->num_dlods; i++)
        {
            const int kind = ground_element(dlods[i].name_hash);
            if (kind == 0)
            {
                return false;
            }
            ground = ground || kind == 1;
        }
        return ground;
    }

    bool is_ground_slab(rfg_mover* rm)
    {
        return is_ground_shape(rm->get_shape());
    }

    std::atomic<unsigned int> Ground_typed{ 0 }; // the slab's type given at its creation and the game's resets

    void ground_slab_frame()
    {
        if (!Ground_as_terrain || !single_player())
        {
            return;
        }
        const unsigned long long now = GetTickCount64();
        if (now < Ground_next_ms)
        {
            return;
        }
        Ground_next_ms = now + 2000;
        const int n = World.get_num_objects<rfg_mover>();
        for (int i = 0; i < n; i++)
        {
            rfg_mover* rm = World.get_object<rfg_mover>(i);
            if (rm == nullptr || rm->obj_flags.destroyed || rm->havok_handle == 0xFFFFFFFF || !is_ground_slab(rm))
            {
                continue;
            }
            hkpRigidBody* rb = havok_body_get_pointer(rm->havok_handle);
            if (rb == nullptr)
            {
                continue;
            }
            const unsigned int before = rb->getCollidable()->getCollisionFilterInfo();
            if (before == COLLIDE_WORLD)
            {
                continue;
            }
            havok_body_set_collision_type(rm->havok_handle, COLLIDE_WORLD, false, false);
            if (Ground_lines < 16)
            {
                Ground_lines++;
                vector bmin;
                vector bmax;
                rm->get_bbox(bmin, bmax);
                memtest_event("ground: the ground slab (%d elements, x %.0f..%.0f, z %.0f..%.0f, top %.1f) is terrain to Havok now: collision type %08X, was %08X",
                              rm->get_shape()->parent->num_dlods, bmin.x, bmax.x, bmin.z, bmax.z, bmax.y, rb->getCollidable()->getCollisionFilterInfo(), before);
            }
        }
    }

    void floor_frame()
    {
        if (!Floor_read)
        {
            Floor_read = true;
            char value[96];
            const DWORD n = GetEnvironmentVariableA("OPENRFG_MEMTEST_FLOORPROBE", value, sizeof(value));
            Probe_on = n > 0 && n < sizeof(value) &&
                       sscanf_s(value, "%f,%f,%f,%f,%f,%f", &Probe_box[0], &Probe_box[1], &Probe_box[2], &Probe_box[3], &Probe_box[4], &Probe_box[5]) == 6;
            Floor_y = env_float("OPENRFG_MEMTEST_FLOORY", &Floor_y_on);
        }
        if (!Probe_on && !Floor_y_on)
        {
            return;
        }
        const unsigned long long now = GetTickCount64();
        if (Floor_start_ms == 0)
        {
            Floor_start_ms = now;
        }
        if (Probe_on && !Probe_done && now - Floor_start_ms >= 15000)
        {
            Probe_done = true;
            floor_probe();
        }
        if (Floor_y_on && now - Floor_last_ms >= 5000)
        {
            Floor_last_ms = now;
            floor_census();
        }
    }
} // namespace

// ---- settings read by the recovered code

unsigned int memtest_debris_pool(unsigned int game_size)
{
    read_settings();
    return Debris_pool > game_size ? Debris_pool : game_size;
}

// The general_mover pool's storage, at static initialization (pages of its own, as rfgphys gives the bigger pool).
void* memtest_debris_pool_storage(unsigned int bytes)
{
    return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

bool memtest_keeping_debris()
{
    read_settings();
    return Keep && single_player() && Zone_free.load(std::memory_order_relaxed) > Zone_reserve / 2;
}

bool memtest_fade_allowed()
{
    if (Recycling || Fade_scope > 0 || !memtest_keeping_debris())
    {
        return true;
    }
    Fades_blocked.fetch_add(1, std::memory_order_relaxed);
    return false;
}

memtest_fade_scope::memtest_fade_scope()
{
    Fade_scope++;
}

memtest_fade_scope::~memtest_fade_scope()
{
    Fade_scope--;
}

// rfgl_desired_values: the pieces a shatter deletes at once. delete_count is not capped at the pieces (after a big
// shatter it is several times their number), so only up to `pieces` of it is real. The pieces the game creates
// anyway, then the extra ones that fit while half the zone reserve stays free, are kept.
int memtest_shatter_deletes(int pieces, int deletes)
{
    if (!Keep || !single_player())
    {
        return deletes;
    }
    const int n = pieces > 0 ? pieces : 0;
    const int real = deletes < 0 ? 0 : deletes > n ? n : deletes;
    const int created = n - real;
    const int room = Zone_free.load(std::memory_order_relaxed) - Zone_reserve / 2 - created;
    const int kept = room <= 0 ? 0 : room < real ? room : real;
    Zone_free.fetch_sub(created + kept, std::memory_order_relaxed);
    if (kept > 0)
    {
        Deletes_kept.fetch_add((unsigned int)kept, std::memory_order_relaxed);
    }
    if (real > kept)
    {
        Deletes_gated.fetch_add((unsigned int)(real - kept), std::memory_order_relaxed);
    }
    return real - kept;
}

bool memtest_refuse_vaporize()
{
    if (!memtest_keeping_debris())
    {
        return false;
    }
    Vaporize_refused.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// A fixed mover's collision type where the game sets it (its creation, set_move_type, the end of its destruction
// mode): a map's ground slab gets the terrain's (ground_slab_frame explains why), single player only.
unsigned int memtest_mover_collision_type(const rfg_destroyable_shape* shape, unsigned int collision_type)
{
    read_settings();
    if (!Ground_as_terrain || Multiplayer || collision_type != COLLIDE_RFG_INITIAL_POSITION || !is_ground_shape(shape))
    {
        return collision_type;
    }
    if (Ground_typed.fetch_add(1, std::memory_order_relaxed) < 8)
    {
        memtest_event("ground: a ground slab (%d elements) gets the terrain's collision type %08X instead of %08X", shape->parent->num_dlods,
                      COLLIDE_WORLD, collision_type);
    }
    return COLLIDE_WORLD;
}

void memtest_stream_parent_gone()
{
    const unsigned int n = Stream_skips.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 5 || n % 1000 == 0)
    {
        memtest_event("debris: world streaming met an object whose parent is gone (%u so far); its hierarchy is skipped", n);
    }
}

// scaling (OpenRFG64-memtest): the lists' capacities are at least 262,144 batched and 65,536 unbatched whatever the
// settings say (BatchedCap / UnbatchedCap only raise the game's), and those are their most. They are capacities only:
// the lists and the frame's instance stream are sized from them once (about 40 MB), and a frame draws what it has.
// Generated buildings place their repeated parts (modules) as instances: ten avenues of tall ones filled the game's
// 24,576 (+ 5,671 of stream slack) in every frame, and the rest was not drawn (walls missing, windows left).
unsigned int memtest_batched_cap(unsigned int game)
{
    (void)env_int("OPENRFG_EXPERIMENT_BATCHEDCAP", 0);
    return game < 262144u ? 262144u : game;
}

unsigned int memtest_unbatched_cap(unsigned int game)
{
    (void)env_int("OPENRFG_EXPERIMENT_UNBATCHEDCAP", 0);
    return game < 65536u ? 65536u : game;
}

void memtest_render_slot_skipped()
{
    const unsigned int n = Render_slots_skipped.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || n % 100000 == 0)
    {
        memtest_event("render: an instance whose transform did not fit in the frame's stream is not drawn (%u so far)", n);
    }
}

// havok_sim_islands_process_post: an island of 64 or more bodies needs more solver memory than the runtime block,
// even split, and the game destroys all but 64 of its bodies (by auto-remove level).
void memtest_havok_island_trim(int bodies, unsigned int need, unsigned int limit, int remove)
{
    const unsigned int n = Island_trims.fetch_add(1, std::memory_order_relaxed) + 1;
    Bodies_trimmed.fetch_add(remove > 0 ? (unsigned int)remove : 0u, std::memory_order_relaxed);
    if (n <= 20 || n % 100 == 0)
    {
        memtest_event("havok: an island of %d bodies needs %u KB of solver memory, more than the %u KB block: the game destroys %d "
                      "of them (%u islands so far)", bodies, need >> 10, limit >> 10, remove, n);
    }
}

void memtest_havok_low_memory()
{
    const unsigned int n = Havok_low_memory.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 5 || n % 1000 == 0)
    {
        memtest_event("havok: the memory watch dog asks for free heap memory (%u times so far)", n);
    }
}

void memtest_recompile_pool_empty()
{
    const unsigned int n = Recompiles_cancelled.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 5 || n % 1000 == 0)
    {
        memtest_event("debris: the compile request pool is empty (%u times so far): the instance's pending compile is cancelled", n);
    }
}

// scaling: a debris render class grew (its factory ran out); the visible list follows the instances (rl_scene_renderer
// grows it between frames).
static void render_class_grew(const char* class_name, int before, int after)
{
    if (strcmp(class_name, "rl_destroyable_model_instance") == 0)
    {
        rl_scene_renderer::set_visible_renderable_instances_max(rl_scene_renderer::get_visible_renderable_instances_max() + (after - before));
    }
    memtest_event("scaling: %s %d -> %d", class_name, before, after);
}

// game_render_system_init, after the game's maxima and before rl_init sizes the factory from them.
void memtest_render_limits()
{
    read_settings();
    log_settings();
    int& filters = vfx_particle_system_filter_instance::ms_class_factory_info.m_objects_max;
    if (Keep && filters < Vfx_filter_instances)
    {
        memtest_event("debris: explosion effect filter instances %d -> %d (the game crashes when they run out)", filters, Vfx_filter_instances);
        filters = Vfx_filter_instances;
    }
    const int inst = rl_destroyable_model_instance::get_objects_max();
    if (Render_instances > inst)
    {
        const int add = Render_instances - inst;
        const int shared = rl_destroyable_model_instance_shared_base::get_objects_max();
        rl_destroyable_model_instance::set_objects_max(inst + add);
        rl_destroyable_model_instance_shared_base::set_objects_max(shared + add);
        rl_fp_kdtree::set_objects_max(rl_fp_kdtree::get_objects_max() + 2 * add);
        rl_scene_renderer::set_visible_renderable_instances_max(rl_scene_renderer::get_visible_renderable_instances_max() + add);
        rl_scene::set_renderable_instances_max(rl_scene::get_renderable_instances_max() + add);
        memtest_event("debris: destroyable render instances %d -> %d, shared bases %d -> %d, visible renderable instances %d, scene renderable instances %d",
                      inst, inst + add, shared, shared + add, rl_scene_renderer::get_visible_renderable_instances_max(),
                      rl_scene::get_renderable_instances_max());
    }
    // scaling (step 3): they grow when they run out, with their shared bases and kd-trees (two to an instance).
    if (scaling_enabled())
    {
        rl_factory_set_growth(rl_destroyable_model_instance::ms_class_factory_info, Render_instances_ceiling, &render_class_grew);
        rl_factory_set_growth(rl_destroyable_model_instance_shared_base::ms_class_factory_info, Render_instances_ceiling, &render_class_grew);
        rl_factory_set_growth(rl_fp_kdtree::ms_class_factory_info, 2 * Render_instances_ceiling, &render_class_grew);
        memtest_event("scaling: destroyable render instances grow from %d when they run out, up to %d",
                      rl_destroyable_model_instance::get_objects_max(), Render_instances_ceiling);
        // The batched pass's compiled groups (1,024): one for each group of a material that combines draws, and a
        // pass holds up to 16,384 groups on big maps (rl_batched_pass.cpp).
        rl_factory_set_growth(rl_compiled_render_group::ms_class_factory_info, 65536, &render_class_grew);
    }
}

// The end of gamestate_gp_simulate_frame (the sim thread, gameplay only).
void memtest_sim_frame()
{
    read_settings();
    log_settings();
    ground_slab_frame();
    update_budget();
    demolish_frame();
    floor_frame();
}

void memtest_debris_numbers(memtest_debris_counts* out)
{
    out->render_instances = rl_destroyable_model_instance::get_objects_used();
    out->render_instances_max = rl_destroyable_model_instance::get_objects_max();
    out->zone_free = Zone_handles - Zone_used;
    out->kept = Deletes_kept.load(std::memory_order_relaxed);
    out->gated = Deletes_gated.load(std::memory_order_relaxed);
    out->fades_blocked = Fades_blocked.load(std::memory_order_relaxed);
    out->vaporize_refused = Vaporize_refused.load(std::memory_order_relaxed);
    out->recycled = Recycled.load(std::memory_order_relaxed);
    out->stream_skips = Stream_skips.load(std::memory_order_relaxed);
    out->recompiles_cancelled = Recompiles_cancelled.load(std::memory_order_relaxed);
    out->island_trims = Island_trims.load(std::memory_order_relaxed);
    out->bodies_trimmed = Bodies_trimmed.load(std::memory_order_relaxed);
    out->havok_low_memory = Havok_low_memory.load(std::memory_order_relaxed);
    out->blasts = Blasts;
    out->keep = Keep;
}
