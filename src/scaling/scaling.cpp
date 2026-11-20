// Grow-on-demand memory for OpenRFG64 (new code, OpenRFG64-memtest). See scaling.h.

#include "scaling/scaling.h"

#include <windows.h>

#include <intrin.h>
#include <string.h>

namespace
{
    const uint64_t Granule = 64 * 1024;
    const int Max_regions = 4096;

    // Regions are only ever appended: an entry is written whole, then published by raising Region_count, so readers
    // (the fault handler among them) need no lock.
    struct region
    {
        char* base;
        uint64_t size;
        volatile LONG64 committed;
        long* bits; // one bit per granule: committed
        const char* name;
    };

    region Regions[Max_regions];
    volatile LONG Region_count;
    SRWLOCK Append_lock = SRWLOCK_INIT;

    // The granule bitmaps, from one static arena: 4 MB of bits cover 2 TB of regions, without an allocation (regions
    // are made during static initialization too).
    const size_t Bitmap_longs = 1u << 20;
    long Bitmap_arena[Bitmap_longs];
    size_t Bitmap_used;

    volatile LONG Faults;
    volatile LONG Commit_failures;
    volatile LONG Handler_installed;

    int read_enabled()
    {
        char value[8] = "";
        const DWORD n = GetEnvironmentVariableA("OPENRFG_SCALING", value, sizeof(value));
        return n > 0 && n < sizeof(value) && value[0] == '0' ? 0 : 1;
    }

    region* find(const void* p)
    {
        const LONG n = Region_count;
        const char* c = (const char*)p;
        for (LONG i = 0; i < n; i++)
        {
            region& r = Regions[i];
            if (c >= r.base && c < r.base + r.size)
            {
                return &r;
            }
        }
        return nullptr;
    }

    bool is_committed(const region& r, uint64_t g)
    {
        return _bittest(&r.bits[g >> 5], (long)(g & 31)) != 0;
    }

    uint64_t granule_bytes(const region& r, uint64_t g)
    {
        const uint64_t offset = g * Granule;
        return r.size - offset < Granule ? r.size - offset : Granule;
    }

    // Granules first..last (inclusive).
    bool commit_granules(region& r, uint64_t first, uint64_t last)
    {
        for (uint64_t g = first; g <= last; g++)
        {
            if (is_committed(r, g))
            {
                continue;
            }
            const uint64_t bytes = granule_bytes(r, g);
            if (VirtualAlloc(r.base + g * Granule, (SIZE_T)bytes, MEM_COMMIT, PAGE_READWRITE) == nullptr)
            {
                InterlockedIncrement(&Commit_failures);
                return false;
            }
            if (!_interlockedbittestandset(&r.bits[g >> 5], (long)(g & 31)))
            {
                InterlockedExchangeAdd64(&r.committed, (LONG64)bytes);
            }
        }
        return true;
    }

    // Zeroes the committed parts of [a, b) (offsets in the region); what is not committed reads as zero anyway.
    void zero_committed(region& r, uint64_t a, uint64_t b)
    {
        while (a < b)
        {
            const uint64_t g = a / Granule;
            const uint64_t g_end = (g + 1) * Granule < b ? (g + 1) * Granule : b;
            if (is_committed(r, g))
            {
                memset(r.base + a, 0, (size_t)(g_end - a));
            }
            a = g_end;
        }
    }

    LONG CALLBACK fault_handler(EXCEPTION_POINTERS* info)
    {
        const EXCEPTION_RECORD* rec = info->ExceptionRecord;
        if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2 && rec->ExceptionInformation[0] != 8 &&
            scaling_fault((const void*)rec->ExceptionInformation[1]))
        {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
} // namespace

bool scaling_enabled()
{
    static const int enabled = read_enabled();
    return enabled != 0;
}

namespace
{
    // Registers [base, base + size) of an existing reservation as a region; `reservation` is freed when it cannot be.
    void* add_region(const char* name, char* base, uint64_t size, char* reservation, size_t commit_bytes);
}

void* scaling_reserve(const char* name, size_t reserve_bytes, size_t commit_bytes)
{
    if (!scaling_enabled() || reserve_bytes == 0)
    {
        return nullptr;
    }
    const uint64_t size = ((uint64_t)reserve_bytes + Granule - 1) & ~(Granule - 1);
    char* base = (char*)VirtualAlloc(nullptr, (SIZE_T)size, MEM_RESERVE, PAGE_READWRITE);
    if (base == nullptr)
    {
        return nullptr;
    }
    return add_region(name, base, size, base, commit_bytes);
}

namespace
{
void* add_region(const char* name, char* base, uint64_t size, char* reservation, size_t commit_bytes)
{
    const uint64_t granules = size / Granule;
    const size_t longs = (size_t)((granules + 31) / 32);
    AcquireSRWLockExclusive(&Append_lock);
    const LONG n = Region_count;
    if (n >= Max_regions || Bitmap_used + longs > Bitmap_longs)
    {
        ReleaseSRWLockExclusive(&Append_lock);
        if (reservation != nullptr)
        {
            VirtualFree(reservation, 0, MEM_RELEASE);
        }
        return nullptr;
    }
    region& r = Regions[n];
    r.base = base;
    r.size = size;
    r.committed = 0;
    r.bits = &Bitmap_arena[Bitmap_used];
    r.name = name;
    Bitmap_used += longs;
    MemoryBarrier();
    Region_count = n + 1;
    ReleaseSRWLockExclusive(&Append_lock);
    if (InterlockedExchange(&Handler_installed, 1) == 0)
    {
        AddVectoredExceptionHandler(1, fault_handler);
    }
    if (commit_bytes > 0)
    {
        scaling_commit(base, commit_bytes < size ? commit_bytes : (size_t)size);
    }
    return base;
}
} // namespace

void* scaling_adopt(const char* name, void* base, size_t size)
{
    if (!scaling_enabled() || base == nullptr || size == 0)
    {
        return nullptr;
    }
    return add_region(name, (char*)base, ((uint64_t)size + Granule - 1) & ~(Granule - 1), nullptr, 0);
}

bool scaling_find(const char* name, void** base, size_t* size)
{
    const LONG n = Region_count;
    for (LONG i = 0; i < n; i++)
    {
        if (Regions[i].name != nullptr && strcmp(Regions[i].name, name) == 0)
        {
            *base = Regions[i].base;
            *size = (size_t)Regions[i].size;
            return true;
        }
    }
    return false;
}

// ---- the "big containers" region: a few blocks, first fit, each committed whole at its alloc and given back at its free
namespace
{
    struct big_block
    {
        unsigned long long offset;
        unsigned long long size;
        unsigned int parts; // primitives carved from it (scaling_big_parts); 0: one block freed by its start
        char name[48];
    };
    const int Max_big_blocks = 16;
    big_block Big_blocks[Max_big_blocks]; // by offset
    int Big_count;
    SRWLOCK Big_lock = SRWLOCK_INIT;

    bool big_region(char** base, unsigned long long* size)
    {
        static char* Base = nullptr;
        static unsigned long long Size = 0;
        if (Base == nullptr)
        {
            void* b = nullptr;
            size_t s = 0;
            if (!scaling_find("big containers", &b, &s))
            {
                return false;
            }
            Size = s;
            Base = (char*)b;
        }
        *base = Base;
        *size = Size;
        return true;
    }
}

void* scaling_big_alloc(unsigned long long bytes, unsigned int align, const char* name)
{
    char* base = nullptr;
    unsigned long long size = 0;
    if (bytes == 0 || !big_region(&base, &size))
    {
        return nullptr;
    }
    const unsigned long long a = align > Granule ? align : Granule;
    const unsigned long long need = (bytes + Granule - 1) / Granule * Granule;
    void* result = nullptr;
    AcquireSRWLockExclusive(&Big_lock);
    if (Big_count < Max_big_blocks && need <= size)
    {
        // The highest gap that holds it, at that gap's top (the region's top lies next to Havok's heap).
        for (int k = Big_count; k >= 0; k--)
        {
            const unsigned long long gap_start = k > 0 ? Big_blocks[k - 1].offset + Big_blocks[k - 1].size : 0;
            const unsigned long long gap_end = k < Big_count ? Big_blocks[k].offset : size;
            if (gap_end < need)
            {
                continue;
            }
            const unsigned long long at = (gap_end - need) / a * a;
            if (at >= gap_start)
            {
                if (scaling_commit(base + at, (size_t)need))
                {
                    for (int j = Big_count; j > k; j--)
                    {
                        Big_blocks[j] = Big_blocks[j - 1];
                    }
                    Big_blocks[k].offset = at;
                    Big_blocks[k].size = need;
                    Big_blocks[k].parts = 0;
                    strncpy_s(Big_blocks[k].name, name != nullptr ? name : "-", _TRUNCATE);
                    Big_count++;
                    result = base + at;
                }
                else
                {
                    scaling_reset(base + at, (size_t)need); // what was committed of it goes back
                }
                break;
            }
        }
    }
    ReleaseSRWLockExclusive(&Big_lock);
    return result;
}

bool scaling_big_free(void* p)
{
    char* base = nullptr;
    unsigned long long size = 0;
    if (p == nullptr || !big_region(&base, &size) || (char*)p < base || (char*)p >= base + size)
    {
        return false;
    }
    AcquireSRWLockExclusive(&Big_lock);
    for (int k = 0; k < Big_count; k++)
    {
        char* const start = base + Big_blocks[k].offset;
        if ((char*)p < start || (char*)p >= start + Big_blocks[k].size)
        {
            continue;
        }
        // A block of parts goes with its last part; a whole block, by its start only.
        const bool last = Big_blocks[k].parts > 0 ? --Big_blocks[k].parts == 0 : (char*)p == start;
        if (last)
        {
            scaling_reset(start, (size_t)Big_blocks[k].size);
            for (int j = k; j + 1 < Big_count; j++)
            {
                Big_blocks[j] = Big_blocks[j + 1];
            }
            Big_count--;
        }
        break;
    }
    ReleaseSRWLockExclusive(&Big_lock);
    return true; // in the region: never one of the level allocator's
}

void scaling_big_parts(void* block, int delta)
{
    char* base = nullptr;
    unsigned long long size = 0;
    if (block == nullptr || !big_region(&base, &size))
    {
        return;
    }
    AcquireSRWLockExclusive(&Big_lock);
    for (int k = 0; k < Big_count; k++)
    {
        if (base + Big_blocks[k].offset == (char*)block)
        {
            Big_blocks[k].parts = (unsigned int)((int)Big_blocks[k].parts + delta);
            break;
        }
    }
    ReleaseSRWLockExclusive(&Big_lock);
}

bool scaling_commit(const void* p, size_t size)
{
    if (size == 0)
    {
        return true;
    }
    region* r = find(p);
    if (r == nullptr)
    {
        return true;
    }
    const uint64_t start = (uint64_t)((const char*)p - r->base);
    uint64_t end = start + size;
    if (end > r->size)
    {
        end = r->size;
    }
    return commit_granules(*r, start / Granule, (end - 1) / Granule);
}

bool scaling_reset(void* p, size_t size)
{
    region* r = find(p);
    if (r == nullptr)
    {
        return false;
    }
    const uint64_t start = (uint64_t)((char*)p - r->base);
    const uint64_t end = start + size < r->size ? start + size : r->size;
    const uint64_t first_full = (start + Granule - 1) / Granule;
    const uint64_t end_full = end / Granule; // exclusive
    if (first_full >= end_full)
    {
        zero_committed(*r, start, end);
        return true;
    }
    zero_committed(*r, start, first_full * Granule);
    zero_committed(*r, end_full * Granule, end);
    for (uint64_t g = first_full; g < end_full; g++)
    {
        if (!is_committed(*r, g))
        {
            continue;
        }
        const uint64_t bytes = granule_bytes(*r, g);
        VirtualFree(r->base + g * Granule, (SIZE_T)bytes, MEM_DECOMMIT);
        if (_interlockedbittestandreset(&r->bits[g >> 5], (long)(g & 31)))
        {
            InterlockedExchangeAdd64(&r->committed, -(LONG64)bytes);
        }
    }
    return true;
}

void* scaling_system_alloc(size_t bytes)
{
    return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

void scaling_system_free(void* p)
{
    if (p != nullptr)
    {
        VirtualFree(p, 0, MEM_RELEASE);
    }
}

bool scaling_contains(const void* p)
{
    return find(p) != nullptr;
}

bool scaling_fault(const void* address)
{
    region* r = find(address);
    if (r == nullptr)
    {
        return false;
    }
    const uint64_t g = (uint64_t)((const char*)address - r->base) / Granule;
    if (is_committed(*r, g))
    {
        return true; // another thread committed it meanwhile: the access runs again
    }
    if (!commit_granules(*r, g, g))
    {
        return false;
    }
    InterlockedIncrement(&Faults);
    return true;
}

// ---- object pools

namespace
{
    // The pools that grow by default: those the destruction, its effects and the renderer run out of, each with the
    // most its indices allow (a 16-bit index kept outside the pool, with 0xFFFF for none: 65,535). Pools whose
    // indices live in 8-bit fields (sizes like 255) or whose index ranges are checked against constants (audio) are
    // not on it. World objects' pools are excluded by object_pool itself.
    struct pool_rule
    {
        const char* type;
        unsigned int most;
    };
    const pool_rule Default_pools[] = {
        { "rfg_dynamic_link", 65535 },       // rfg_subpiece::dynamic_link_index
        { "rfg_destruction_event", 1u << 20 },
        { "damaged_subpiece_event", 65535 }, // damaged_subpiece_event_first_index
        { "explosion_event", 65535 },        // rfg_mover::explosion_event_index
        { "explosion_create_info", 1u << 20 },
        { "constraint", 1u << 20 },
        { "attach_info_data", 1u << 20 },
        { "glass_shard", 1u << 20 },
        { "effect_candidate", 1u << 20 },
        { "rl_deformation_sphere", 1u << 20 },
        { "parametric_constant_data", 1u << 20 },
        { "dynamic_hole_request", 1u << 20 },
        { "rl_deferred_operation", 1u << 20 },
        { "rl_decal_deferred_info", 1u << 20 },
        { "destruction_event", 65535 },        // undergrowth destruction (8 in the game)
        { "c_chunk_base", 16384 },             // the loaded chunks (352): their name table holds 16,384
        { "mover_fader", 65535 },              // mover_fader::handle keeps the index in its upper 16 bits
        { "matlib_entry", 1u << 20 },          // the material library's registered materials (2,560): each chunk type
                                               // registers its own; ten avenues of 480 types filled them at load, and a
                                               // material left out did not resolve (the terrain's low LOD crashed)
    };

    volatile LONG Static_init_done;
    int Pools_mode = -1; // 0 none, 1 the list, 2 all (OPENRFG_SCALING_POOLS)

    // The pool's element type: what follows "object_pool<" (and "struct ", "class ") up to the comma.
    bool type_of(const char* pool_name, char* out, size_t size)
    {
        const char* p = pool_name != nullptr ? strstr(pool_name, "object_pool<") : nullptr;
        if (p == nullptr)
        {
            return false;
        }
        p += 12;
        if (strncmp(p, "struct ", 7) == 0)
        {
            p += 7;
        }
        else if (strncmp(p, "class ", 6) == 0)
        {
            p += 6;
        }
        size_t n = 0;
        while (p[n] != '\0' && p[n] != ',' && n + 1 < size)
        {
            out[n] = p[n];
            n++;
        }
        out[n] = '\0';
        return n > 0;
    }

    const int Max_pool_notes = 512;
    struct pool_note
    {
        const void* region;
        const char* name;
        unsigned int objects;
        unsigned int initial;
    };
    pool_note Grown[Max_pool_notes];
    int Grown_count;
    pool_note Exhausted[Max_pool_notes];
    int Exhausted_count;
    SRWLOCK Notes_lock = SRWLOCK_INIT;
} // namespace

void scaling_static_init_done()
{
    Static_init_done = 1;
}

unsigned int scaling_pool_reserve(const char* pool_name, unsigned int game_size)
{
    if (!scaling_enabled() || Static_init_done != 0)
    {
        return 0;
    }
    if (Pools_mode < 0)
    {
        char value[16] = "";
        const DWORD n = GetEnvironmentVariableA("OPENRFG_SCALING_POOLS", value, sizeof(value));
        Pools_mode = n == 0 || n >= sizeof(value) ? 1 : _stricmp(value, "none") == 0 ? 0 : _stricmp(value, "all") == 0 ? 2 : 1;
    }
    if (Pools_mode == 0)
    {
        return 0;
    }
    char type[128];
    if (!type_of(pool_name, type, sizeof(type)))
    {
        return 0;
    }
    // A nested type ("rl_renderable_particles_instance::parametric_constant_data") matches by its last part too.
    const char* last = strrchr(type, ':');
    last = last != nullptr ? last + 1 : type;
    for (const pool_rule& rule : Default_pools)
    {
        if (strcmp(type, rule.type) == 0 || strcmp(last, rule.type) == 0)
        {
            return rule.most > game_size ? rule.most : 0;
        }
    }
    // all: every other pool up to what a 16-bit index allows (the safe guess for the indices kept elsewhere)
    return Pools_mode == 2 && game_size < 65535 ? 65535 : 0;
}

void scaling_note_pool(const void* region, const char* pool_name, unsigned int objects)
{
    AcquireSRWLockExclusive(&Notes_lock);
    int i = 0;
    while (i < Grown_count && Grown[i].region != region)
    {
        i++;
    }
    if (i == Grown_count && Grown_count < Max_pool_notes)
    {
        Grown[i].region = region;
        Grown[i].name = pool_name;
        Grown[i].initial = objects;
        Grown_count++;
    }
    if (i < Grown_count)
    {
        Grown[i].objects = objects;
    }
    ReleaseSRWLockExclusive(&Notes_lock);
}

void scaling_pool_exhausted(const char* pool_name, unsigned int objects)
{
    AcquireSRWLockExclusive(&Notes_lock);
    if (Exhausted_count < Max_pool_notes)
    {
        Exhausted[Exhausted_count].region = nullptr;
        Exhausted[Exhausted_count].name = pool_name;
        Exhausted[Exhausted_count].objects = objects;
        Exhausted[Exhausted_count].initial = objects;
        Exhausted_count++;
    }
    ReleaseSRWLockExclusive(&Notes_lock);
}

int scaling_grown_pools(scaling_pool_info* out, int max)
{
    AcquireSRWLockShared(&Notes_lock);
    int n = 0;
    for (int i = 0; i < Grown_count && n < max; i++)
    {
        if (Grown[i].objects > Grown[i].initial)
        {
            out[n].name = Grown[i].name;
            out[n].objects = Grown[i].objects;
            out[n].initial = Grown[i].initial;
            n++;
        }
    }
    ReleaseSRWLockShared(&Notes_lock);
    return n;
}

int scaling_exhausted_pools(scaling_pool_info* out, int max)
{
    AcquireSRWLockShared(&Notes_lock);
    int n = 0;
    for (int i = 0; i < Exhausted_count && n < max; i++)
    {
        out[n].name = Exhausted[i].name;
        out[n].objects = Exhausted[i].objects;
        out[n].initial = Exhausted[i].initial;
        n++;
    }
    ReleaseSRWLockShared(&Notes_lock);
    return n;
}

int scaling_region_count()
{
    return (int)Region_count;
}

bool scaling_region(int index, scaling_region_info* out)
{
    if (index < 0 || index >= (int)Region_count)
    {
        return false;
    }
    const region& r = Regions[index];
    out->name = r.name;
    out->base = r.base;
    out->reserved = r.size;
    out->committed = (uint64_t)r.committed;
    return true;
}

uint32_t scaling_fault_count()
{
    return (uint32_t)Faults;
}

uint32_t scaling_commit_failures()
{
    return (uint32_t)Commit_failures;
}
