// Grow-on-demand memory for OpenRFG64 (new code, OpenRFG64-memtest; research/docs/04-scaling-plan.md there).
//
// The game sizes its pools once, at start-up, from a fixed memory layout. A 64-bit process has address space to
// spare, so a pool can instead reserve a range far larger than it will need and have memory committed only as it is
// used. The range never moves, so the game's raw pointers into it stay valid; it costs nothing until it is touched;
// and it grows with the session.
//
// A region is committed in 64 KB granules, two ways:
// - explicitly, scaling_commit, where the kernel may write into the memory (a file read into level memory does not
//   fault, it fails);
// - on first touch: an access to a reserved granule faults, the vectored exception handler commits it (scaling_fault)
//   and the instruction runs again.
//
// OPENRFG_SCALING=0 turns it off: every pool keeps the game's fixed size (for comparisons).
#pragma once

#include <stddef.h>
#include <stdint.h>

// Whether pools grow (OPENRFG_SCALING, on unless 0). Read once; safe during static initialization.
bool scaling_enabled();

// Reserves a region of reserve_bytes (rounded up to 64 KB) and commits its first commit_bytes. Null when scaling is
// off or the reservation fails (the caller keeps the game's fixed memory then).
void* scaling_reserve(const char* name, size_t reserve_bytes, size_t commit_bytes);

// Registers [base, base + size) of a reservation made elsewhere as a region (nothing committed). Several regions may
// share one reservation, to keep them next to each other.
void* scaling_adopt(const char* name, void* base, size_t size);
// A region by its name: false when there is none.
bool scaling_find(const char* name, void** base, size_t* size);

// Commits [p, p + size) when it lies in a region (nothing otherwise). False when the system has no memory left.
bool scaling_commit(const void* p, size_t size);

// Decommits the whole 64 KB granules of [p, p + size) in a region and zeroes the rest of the range (pages come back
// zeroed when touched again). False when the range is not in a region (the caller zeroes it itself).
bool scaling_reset(void* p, size_t size);

// A stream2 container past 2 GB (a big map's World container): a block of the "big containers" region, which lies
// right below Havok's heap in its reservation with level memory (memorylayout_win32.cpp). Blocks are taken from the
// region's top, next to Havok's heap, so that their links into it stay as near as they can. Any size the region holds
// (64-bit); committed whole, given back at its free. Null when it does not fit or there is no such region.
void* scaling_big_alloc(unsigned long long bytes, unsigned int align, const char* name);
// True when p lies in the "big containers" region; false: not one of them. A block of parts loses one (and goes with
// its last); a whole block goes when p is its start.
bool scaling_big_free(void* p);
// A block whose primitives are carved from it (a big container of primitives, stream2's SCM_PRIM): delta parts more
// (+1 for each primitive placed in it). Each part's free (scaling_big_free) takes one off.
void scaling_big_parts(void* block, int delta);

// Whole committed blocks straight from the system (Havok's heap pages: they come and go whole, they are not regions).
void* scaling_system_alloc(size_t bytes);
void scaling_system_free(void* p);

// Whether an address lies in a region.
bool scaling_contains(const void* p);

// The vectored exception handlers' part: commits the granule a fault hit when it lies in a region and is not committed
// yet. True when the faulting instruction can run again.
bool scaling_fault(const void* address);

// Object pools (object_pool<T, N, K>, vlib): how many objects a pool may grow to (its region's room), 0 for the game's
// fixed N. Only pools made during static initialization grow (globals: a pool inside an object made later keeps its
// storage), and only those on the list in scaling.cpp, each with the most its indices allow elsewhere (a 16-bit
// index stored outside the pool caps it at 65,535). OPENRFG_SCALING_POOLS=none turns them off.
unsigned int scaling_pool_reserve(const char* pool_name, unsigned int game_size);
// Called when static initialization is over (WinMain): pools made from then on keep their fixed storage.
void scaling_static_init_done();
// A pool's count of objects after growing (for the census), and a pool that ran out (once per pool).
void scaling_note_pool(const void* region, const char* pool_name, unsigned int objects);
void scaling_pool_exhausted(const char* pool_name, unsigned int objects);
struct scaling_pool_info
{
    const char* name;
    unsigned int objects;
    unsigned int initial;
};
int scaling_grown_pools(scaling_pool_info* out, int max);     // pools that grew, in no order
int scaling_exhausted_pools(scaling_pool_info* out, int max); // pools that ran out (objects: their size then)

// For the census: the regions in creation order.
struct scaling_region_info
{
    const char* name;
    const void* base;
    uint64_t reserved;
    uint64_t committed;
};
int scaling_region_count();
bool scaling_region(int index, scaling_region_info* out);
uint32_t scaling_fault_count();       // granules committed on first touch
uint32_t scaling_commit_failures();   // commits the system refused
