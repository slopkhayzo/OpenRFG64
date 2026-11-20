// OpenRFG64 memory test (new code, not recovered). See memtest.h.
//
// Output (in the run folder):
//   memtest.csv          one line per census (default once a second): process, budgets, heaps, tables, counts
//   memtest_pools.csv    every vlib mempool every 10 s: name, used, size, peak
//   memtest_events.log   start-up, the Sledge layer, plugins, game states, pools that fill, the game's failure messages
//   memtest_native.log   everything the game prints (keen::debug::nativePrint)
//   memtest_summary.txt  peaks and what filled first; rewritten every 10 s, so a crash leaves the latest

#include "memtest/memtest.h"
#include "scaling/scaling.h"
#include "volition/rfg/code/havok/havok_memory.h"

// The game's headers first, then windows.h (OpenRFG's order: winsock2 must come before any windows.h).
#include "volition/libmodules/havok/hk2014_2_5_r1/source/common/base/memory/system/hkmemorysystem.h"
#include "volition/rfg/code/gameplay/wrecking_crew/wrecking_crew.h"
#include "volition/rfg/code/gamewide/gameseq.h"
#include "volition/rfg/code/level/world.h"
#include "volition/rfg/code/rfg.h"
#include "volition/vlib/code/cfile/cfile_common.h"
#include "volition/vlib/code/memory/mempool_base.h"
#include "volition/vlib/code/memory/split_memmgr.h"
#include "volition/vlib/code/util/endian_type.h"
#include "keen/base/tlsfmemoryallocator.hpp"
#include "keen/base/zonememoryallocator.hpp"

#include <windows.h>
#include <psapi.h>

#include <ctype.h>
#include <share.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "psapi.lib")

namespace keen
{
    namespace memory
    {
        // memorysystem.cpp: a block of the layout and its name.
        bool memtestLayoutBlock(unsigned int index, const void** pStart, unsigned long long* size, const char** pName);
    }
}

namespace
{
    // ---- options (parsed before WinMain)
    const char* Wc_map = nullptr;
    const char* Wc_options_hex = nullptr;
    const char* Sledge_dir = nullptr;
    const char* Plugins_dir = nullptr;
    const char* Out_dir_arg = nullptr;
    int Exit_after_s = 0;

    struct options_setup
    {
        options_setup()
        {
            for (int i = 1; i + 1 < __argc; i++)
            {
                const char* arg = __argv[i];
                const char* value = __argv[i + 1];
                if (strcmp(arg, "--wc") == 0) { Wc_map = value; i++; }
                else if (strcmp(arg, "--wc-options") == 0) { Wc_options_hex = value; i++; }
                else if (strcmp(arg, "--sledge") == 0) { Sledge_dir = value; i++; }
                else if (strcmp(arg, "--plugins") == 0) { Plugins_dir = value; i++; }
                else if (strcmp(arg, "--memtest-dir") == 0) { Out_dir_arg = value; i++; }
                else if (strcmp(arg, "--exit-after") == 0) { Exit_after_s = atoi(value); i++; }
            }
        }
    };
    options_setup Options_setup;

    // ---- output
    char Out_dir[MAX_PATH] = ".";
    FILE* Csv = nullptr;
    FILE* Pools_csv = nullptr;
    FILE* Events = nullptr;
    FILE* Native = nullptr;
    SRWLOCK Log_lock = SRWLOCK_INIT;
    bool Initialized = false;
    unsigned long long Start_ms = 0;
    unsigned int Interval_ms = 1000;

    double now_s()
    {
        return Start_ms == 0 ? 0.0 : (double)(GetTickCount64() - Start_ms) / 1000.0;
    }

    FILE* open_out(const char* name, const char* mode)
    {
        char path[MAX_PATH];
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", Out_dir, name);
        FILE* f = nullptr;
        // Shared reading: the run script and the agent may read the files while the game runs.
        f = _fsopen(path, mode, _SH_DENYWR);
        return f;
    }

    // ---- the pool registry (mempool_base::base_init registers, ~mempool_base unregisters)
    struct pool_entry
    {
        mempool_base* pool;
        char name[32];
        unsigned int size;
        unsigned int used;
        unsigned int peak_seen;
        double full_at;      // first census time it was at least 99.9 % used (-1: never, -2: full when first seen)
        bool dynamic;
        bool valid;
        bool seen;           // read at least once
    };
    const int Max_pools = 4096;
    pool_entry Pools[Max_pools];
    int Pool_count = 0;
    int Pool_registry_overflow = 0;
    SRWLOCK Pool_lock = SRWLOCK_INIT;
    uintptr_t Image_lo = 0;
    uintptr_t Image_hi = 0;

    // A registered pool read under SEH: the pool may have been released without its destructor (pools inside pools),
    // so its vtable must lie in the program image and an access violation only drops it.
    bool read_pool(mempool_base* pool, unsigned int* used, unsigned int* size, unsigned int* peak, bool* dynamic, char* name)
    {
        __try
        {
            const uintptr_t vptr = *(const uintptr_t*)pool;
            if (vptr < Image_lo || vptr >= Image_hi)
            {
                return false;
            }
            *used = pool->space_used();
            *size = pool->space_max();
            *peak = pool->peak_used();
            *dynamic = pool->is_dynamic();
            const char* n = pool->get_name();
            int i = 0;
            for (; i < 31 && n[i] != '\0'; i++)
            {
                name[i] = n[i];
            }
            name[i] = '\0';
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // ---- counters fed by the recovered code
    volatile long long Crt_bytes = 0;
    volatile long long Crt_blocks = 0;
    volatile long long Crt_peak = 0;
    volatile long Strpool_fallbacks = 0;
    volatile long Strpool_unregistered = 0;
    volatile long Failure_messages = 0;
    volatile long Havok_oom = 0;
    volatile long Level_failures = 0;
    int Peak_cm_models = 0;
    unsigned int Cm_high_water = 0; // scaling: the highest collision model index handed out, + 1
    int Peak_cm_side = 0;           // scaling: the most indices kept past the chunks' 16 bits
    int Peak_movers = 0;
    int Peak_zone_handles = 0;
    int Peak_render;
    long long Havok_system_used; // memtest_debris: Havok's system allocator in use (its block, 512 MB on x64)
    long long Havok_solver_used;
    int Render_max;
    memtest_debris_counts Debris;
    unsigned int Peak_level = 0;
    unsigned int Level_size = 0;
    char Level_first_failure[160] = "";

    // ---- peaks for the summary
    unsigned long long Peak_private = 0;
    unsigned long long Peak_ws = 0;
    double Over_4gb_at = -1.0;
    long long Peak_havok_inuse = 0;
    long long Havok_total = 0;
    unsigned int Peak_far = 0;
    int Peak_objects = 0;
    int Max_objects = 0;
    unsigned int Peak_provider = 0;
    unsigned int Provider_total = 0;
    unsigned int Frame = 0;
    int Last_state = -100;
    unsigned long long Last_census_ms = 0;
    unsigned long long Last_pools_ms = 0;
    unsigned long long Last_summary_ms = 0;
    char Last_failure[256] = "";

    // ---- the Wrecking Crew start
    bool Wc_selected = false;
    bool Wc_started = false;
    unsigned long long Wc_loaded_since = 0;
    unsigned long long Wc_started_ms = 0;

    // ---- the Sledge layer
    struct overlay_file
    {
        char name[128];       // the base name the game asks for
        char keen_path[320];  // /sledgeN\relative\path
        unsigned long long size;
    };
    const int Max_overlay_files = 512;
    overlay_file Overlay_files[Max_overlay_files];
    int Overlay_file_count = 0;
    char Mount_native[16][MAX_PATH];
    char Mount_name[16][32];
    int Mount_count = 0;
    memtest_wc_map_addition Wc_additions[32];
    int Wc_addition_count = 0;
    volatile long Overlay_hits = 0;

    void log_line(FILE* f, const char* text)
    {
        if (f == nullptr)
        {
            return;
        }
        AcquireSRWLockExclusive(&Log_lock);
        fprintf(f, "%9.3f %s\n", now_s(), text);
        fflush(f);
        ReleaseSRWLockExclusive(&Log_lock);
    }

    const char* state_name(int s)
    {
        switch (s)
        {
        case 0: return "MAINMENU";
        case 1: return "GAMEPLAY";
        case 2: return "LOAD";
        case 19: return "WRECKING_CREW_MAIN_MENU";
        case 59: return "WC_INIT";
        case 60: return "WC_SHUTDOWN";
        default: return "";
        }
    }

    // ---- Sledge folder
    bool read_text(const char* path, char** out, size_t* out_size)
    {
        FILE* f = nullptr;
        if (fopen_s(&f, path, "rb") != 0 || f == nullptr)
        {
            return false;
        }
        fseek(f, 0, SEEK_END);
        const long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char* buf = (char*)malloc((size_t)n + 1);
        const size_t got = fread(buf, 1, (size_t)n, f);
        fclose(f);
        buf[got] = '\0';
        *out = buf;
        *out_size = got;
        return true;
    }

    void add_overlay_files(int mount, const char* root, const char* rel)
    {
        char pattern[MAX_PATH];
        _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s%s%s\\*", root, rel[0] ? "\\" : "", rel);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE)
        {
            return;
        }
        do
        {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            {
                continue;
            }
            char child[MAX_PATH];
            _snprintf_s(child, sizeof(child), _TRUNCATE, "%s%s%s", rel, rel[0] ? "\\" : "", fd.cFileName);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                add_overlay_files(mount, root, child);
            }
            else if (Overlay_file_count < Max_overlay_files)
            {
                overlay_file& o = Overlay_files[Overlay_file_count++];
                strncpy_s(o.name, fd.cFileName, _TRUNCATE);
                _snprintf_s(o.keen_path, sizeof(o.keen_path), _TRUNCATE, "%s\\%s", Mount_name[mount], child);
                o.size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                memtest_event("sledge: file %s -> %s (%llu bytes)", o.name, o.keen_path, o.size);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    // The quoted string after "key" and "=": file = "wcavenue" -> wcavenue.
    bool lua_string_field(const char* from, const char* to, const char* key, char* out, size_t out_size)
    {
        const size_t klen = strlen(key);
        for (const char* p = from; p + klen < to; p++)
        {
            if (strncmp(p, key, klen) != 0 || (p > from && (isalnum((unsigned char)p[-1]) || p[-1] == '_')))
            {
                continue;
            }
            const char* q = p + klen;
            while (q < to && (*q == ' ' || *q == '\t'))
            {
                q++;
            }
            if (q >= to || *q != '=')
            {
                continue;
            }
            q++;
            while (q < to && (*q == ' ' || *q == '\t'))
            {
                q++;
            }
            if (q >= to || *q != '"')
            {
                continue;
            }
            q++;
            const char* e = q;
            while (e < to && *e != '"')
            {
                e++;
            }
            if (e >= to)
            {
                return false;
            }
            const size_t n = (size_t)(e - q) < out_size - 1 ? (size_t)(e - q) : out_size - 1;
            memcpy(out, q, n);
            out[n] = '\0';
            return true;
        }
        return false;
    }

    // What the avenue mod's mod.lua asks of Sledge, read from its text: the files it registers (every file in its
    // files\ folder) and the Wrecking Crew map entries of its MAPS table ({ file = "X", copy_of = "Y" }) added to the
    // table named in its register_xml_edit call.
    void apply_mod(const char* id, const char* mod_dir)
    {
        char files_dir[MAX_PATH];
        _snprintf_s(files_dir, sizeof(files_dir), _TRUNCATE, "%s\\files", mod_dir);
        const DWORD attr = GetFileAttributesA(files_dir);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0 && Mount_count < 16)
        {
            const int m = Mount_count++;
            strncpy_s(Mount_native[m], files_dir, _TRUNCATE);
            _snprintf_s(Mount_name[m], sizeof(Mount_name[m]), _TRUNCATE, "/sledge%d", m);
            add_overlay_files(m, files_dir, "");
        }
        char lua_path[MAX_PATH];
        _snprintf_s(lua_path, sizeof(lua_path), _TRUNCATE, "%s\\mod.lua", mod_dir);
        char* lua = nullptr;
        size_t lua_size = 0;
        if (!read_text(lua_path, &lua, &lua_size))
        {
            memtest_event("sledge: %s has no mod.lua", id);
            return;
        }
        // The table its xml edit goes to.
        int dlc_id = -2;
        const char* edit = strstr(lua, "register_xml_edit(\"");
        if (edit != nullptr)
        {
            const char* t = edit + strlen("register_xml_edit(\"");
            if (_strnicmp(t, "dlc", 3) == 0 && isdigit((unsigned char)t[3]) && isdigit((unsigned char)t[4]) && strncmp(t + 5, "_wrecking_crew.xtbl", 19) == 0)
            {
                dlc_id = (t[3] - '0') * 10 + (t[4] - '0');
            }
            else if (strncmp(t, "wrecking_crew.xtbl", 18) == 0)
            {
                dlc_id = -1;
            }
        }
        // Each { file = "X", copy_of = "Y" } entry.
        int found = 0;
        for (const char* p = strstr(lua, "copy_of"); p != nullptr; p = strstr(p + 7, "copy_of"))
        {
            const char* open = p;
            while (open > lua && *open != '{')
            {
                open--;
            }
            const char* close = strchr(p, '}');
            if (close == nullptr || *open != '{')
            {
                continue;
            }
            memtest_wc_map_addition a = {};
            if (!lua_string_field(open, close, "file", a.file, sizeof(a.file)) || !lua_string_field(open, close, "copy_of", a.copy_of, sizeof(a.copy_of)))
            {
                continue;
            }
            if (dlc_id == -2)
            {
                memtest_event("sledge: %s: map %s found but no wrecking crew table edit; ignored", id, a.file);
                continue;
            }
            a.dlc_id = dlc_id;
            if (Wc_addition_count < 32)
            {
                Wc_additions[Wc_addition_count++] = a;
                found++;
                memtest_event("sledge: %s: Wrecking Crew map %s (a copy of %s's entry) in %s", id, a.file, a.copy_of,
                              dlc_id < 0 ? "wrecking_crew.xtbl" : "a dlc wrecking crew table");
            }
        }
        if (strstr(lua, "register_") != nullptr && found == 0 && Mount_count == 0)
        {
            memtest_event("sledge: %s: nothing this layer understands (only register_file and the Wrecking Crew map entries are supported)", id);
        }
        free(lua);
    }

    void load_sledge(const char* dir)
    {
        char toml_path[MAX_PATH];
        _snprintf_s(toml_path, sizeof(toml_path), _TRUNCATE, "%s\\sledge.toml", dir);
        char* toml = nullptr;
        size_t toml_size = 0;
        if (!read_text(toml_path, &toml, &toml_size))
        {
            memtest_event("sledge: no %s", toml_path);
            return;
        }
        // [sledge.mods.'<id>'] followed by enabled = true|false (option sections, [sledge.mods.'<id>'.options], are skipped).
        char current[128] = "";
        char* line = toml;
        while (line != nullptr && *line != '\0')
        {
            char* next = strchr(line, '\n');
            if (next != nullptr)
            {
                *next++ = '\0';
            }
            while (*line == ' ' || *line == '\t')
            {
                line++;
            }
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' '))
            {
                line[--len] = '\0';
            }
            if (line[0] == '[')
            {
                current[0] = '\0';
                const char* prefix = "[sledge.mods.'";
                if (strncmp(line, prefix, strlen(prefix)) == 0)
                {
                    const char* id = line + strlen(prefix);
                    const char* q = strchr(id, '\'');
                    if (q != nullptr && strcmp(q, "']") == 0)
                    {
                        const size_t n = (size_t)(q - id) < sizeof(current) - 1 ? (size_t)(q - id) : sizeof(current) - 1;
                        memcpy(current, id, n);
                        current[n] = '\0';
                    }
                }
            }
            else if (current[0] != '\0' && strncmp(line, "enabled", 7) == 0)
            {
                const bool enabled = strstr(line, "true") != nullptr;
                memtest_event("sledge: mod %s %s", current, enabled ? "enabled" : "disabled");
                if (enabled)
                {
                    char mod_dir[MAX_PATH];
                    _snprintf_s(mod_dir, sizeof(mod_dir), _TRUNCATE, "%s\\mods\\%s", dir, current);
                    apply_mod(current, mod_dir);
                }
            }
            line = next;
        }
        free(toml);
    }

    void load_plugins(const char* dir)
    {
        char pattern[MAX_PATH];
        _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\*.asi", dir);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE)
        {
            memtest_event("plugins: none in %s", dir);
            return;
        }
        do
        {
            char path[MAX_PATH];
            _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", dir, fd.cFileName);
            HMODULE module = LoadLibraryA(path);
            if (module == nullptr)
            {
                memtest_event("plugins: %s did not load (error %lu)", path, GetLastError());
                continue;
            }
            memtest_event("plugins: %s loaded at %p", path, (void*)module);
            // Ultimate ASI Loader's convention: an exported InitializeASI is called once the plugin is in.
            typedef void (*initialize_fn)();
            if (initialize_fn init = (initialize_fn)GetProcAddress(module, "InitializeASI"))
            {
                init();
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    // ---- the census
    struct keen_alloc_sample
    {
        unsigned int used;
        unsigned int peak;
        unsigned int total;
    };

    template<typename T>
    keen_alloc_sample sample_allocator(const T& a)
    {
        keen_alloc_sample s;
        s.used = a.getAllocatedSize();
        s.peak = a.getMaxAllocatedSize();
        s.total = a.getTotalSize();
        return s;
    }

    void havok_stats(long long* in_use, long long* peak, long long* total)
    {
        *in_use = 0;
        *peak = 0;
        *total = 0;
        hkMemorySystem* system = hkMemorySystem::getInstancePtr();
        if (system == nullptr)
        {
            return;
        }
        hkMemorySystem::MemoryStatistics stats;
        system->getMemoryStatistics(stats);
        // hkFreeListMemorySystem::getMemoryStatistics: System (the block's allocator), Heap (the free lists on top of
        // it: Havok's objects), Debug, Solver. Their sum counted the heap twice; the heap and the system are apart now.
        for (int i = 0; i < stats.m_entries.getSize(); i++)
        {
            const hkMemorySystem::MemoryStatistics::Entry& e = stats.m_entries[i];
            if (i == 1)
            {
                *in_use = e.m_allocatorStats.m_inUse;
                *peak = e.m_allocatorStats.m_peakInUse;
            }
            else if (i == 0)
            {
                Havok_system_used = e.m_allocatorStats.m_inUse;
                if (e.m_allocatorStats.m_totalAvailable > 0) // -1: an allocator without a limit
                {
                    *total = e.m_allocatorStats.m_totalAvailable;
                }
                if (havok_memory_heap_page_limit() > 0) // scaling: the heap grows in pages up to this
                {
                    *total = havok_memory_heap_page_limit();
                }
            }
            else if (i == 3)
            {
                Havok_solver_used = e.m_allocatorStats.m_inUse;
            }
        }
    }

    double mb(unsigned long long bytes)
    {
        return (double)bytes / 1048576.0;
    }

    void write_summary(const char* reason);
    unsigned long long scaling_committed_total();
    const char* pool_label(const char* name, char* out, size_t size);

    void census()
    {
        const double t = now_s();
        PROCESS_MEMORY_COUNTERS_EX pmc = {};
        pmc.cb = sizeof(pmc);
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));
        MEMORYSTATUSEX ms = {};
        ms.dwLength = sizeof(ms);
        GlobalMemoryStatusEx(&ms);
        const unsigned long long va_used = ms.ullTotalVirtual - ms.ullAvailVirtual;
        if (pmc.PrivateUsage > Peak_private)
        {
            Peak_private = pmc.PrivateUsage;
        }
        if (pmc.WorkingSetSize > Peak_ws)
        {
            Peak_ws = pmc.WorkingSetSize;
        }
        if (Over_4gb_at < 0.0 && pmc.PrivateUsage > 4ull * 1024 * 1024 * 1024)
        {
            Over_4gb_at = t;
            memtest_event("process: private bytes passed 4 GB (%.0f MB)", mb(pmc.PrivateUsage));
        }

        keen_alloc_sample provider = sample_allocator(Rfg_mempool_provider_keen_allocator);
        const keen_alloc_sample vram = sample_allocator(Rfg_vram_mempool_provider_keen_allocator);
        const keen_alloc_sample bank = sample_allocator(Rfg_audio_bank_keen_allocator);
        // The keen allocators' own counters are not kept (they read 0, as in rfg.exe); the main block is a zone, whose
        // top is what it has handed out.
        provider.used = Rfg_mempool_provider_keen_allocator.getAllocator().getUsedSize();
        if (provider.used > Peak_provider)
        {
            Peak_provider = provider.used;
        }
        provider.peak = Peak_provider;
        Provider_total = provider.total;

        // The pools: totals, the fullest, the first time each becomes full.
        int pools = 0;
        unsigned long long pools_used = 0;
        double fullest_pct = 0.0;
        char fullest[32] = "-";
        AcquireSRWLockExclusive(&Pool_lock);
        for (int i = 0; i < Pool_count; i++)
        {
            pool_entry& e = Pools[i];
            unsigned int used, size, peak;
            bool dynamic;
            char name[32];
            if (!read_pool(e.pool, &used, &size, &peak, &dynamic, name))
            {
                e.valid = false;
                continue;
            }
            e.valid = true;
            memcpy(e.name, name, sizeof(name));
            e.used = used;
            e.size = size;
            e.dynamic = dynamic;
            if (used > e.peak_seen)
            {
                e.peak_seen = used;
            }
            pools++;
            pools_used += used;
            if (size >= 0x100000 && !dynamic)
            {
                const double pct = 100.0 * (double)used / (double)size;
                if (pct > fullest_pct)
                {
                    fullest_pct = pct;
                    memcpy(fullest, name, sizeof(name));
                }
                if (pct >= 99.9 && e.full_at == -1.0)
                {
                    // A pool full the first time it is seen took its memory whole at creation (alloc_all, single
                    // allocations): that is its design, not a pool running out.
                    e.full_at = e.seen ? t : -2.0;
                    if (e.seen)
                    {
                        char line[160];
                        _snprintf_s(line, sizeof(line), _TRUNCATE, "pool full: %s (%u of %u bytes)", name, used, size);
                        log_line(Events, line);
                    }
                }
            }
            e.seen = true;
        }
        ReleaseSRWLockExclusive(&Pool_lock);

        long long hk_in_use, hk_peak, hk_total;
        havok_stats(&hk_in_use, &hk_peak, &hk_total);
        if (hk_in_use > Peak_havok_inuse)
        {
            Peak_havok_inuse = hk_in_use;
        }
        if (hk_peak > Peak_havok_inuse)
        {
            Peak_havok_inuse = hk_peak;
        }
        Havok_total = hk_total;

        unsigned int far_count = 0;
#ifdef OPENRFG_X64
        far_count = et_far::get_tables().count;
#endif
        if (far_count > Peak_far)
        {
            Peak_far = far_count;
        }
        const int objects = World.get_num_objects();
        Max_objects = World.get_max_world_objects();
        if (objects > Peak_objects)
        {
            Peak_objects = objects;
        }
        // Level memory (layout block 6, RFG_Level): the avenue's wall in rfg.exe (two allocations fail there).
        const unsigned int level_used = Level_memmgr.space_used();
        Level_size = Level_memmgr.space_max();
        const unsigned int level_largest = Level_memmgr.largest_allocation_possible();
        if (level_used > Peak_level)
        {
            Peak_level = level_used;
        }
        const int cm_models = openrfg_num_collision_models();
        if (cm_models > Peak_cm_models)
        {
            Peak_cm_models = cm_models;
        }
        Cm_high_water = openrfg_collision_models_high_water();
        const int cm_side = openrfg_chunk_cm_side_entries();
        if (cm_side > Peak_cm_side)
        {
            Peak_cm_side = cm_side;
        }
        const int movers = openrfg_general_movers_in_use();
        if (movers > Peak_movers)
        {
            Peak_movers = movers;
        }
        int zones = 0;
        const int zone_handles = World.memtest_zone_handles_used(&zones);
        if (zone_handles > Peak_zone_handles)
        {
            Peak_zone_handles = zone_handles;
        }
        memtest_debris_counts dc = {};
        memtest_debris_numbers(&dc);
        if (dc.render_instances > Peak_render)
        {
            Peak_render = dc.render_instances;
        }
        Render_max = dc.render_instances_max;
        Debris = dc;

        if (Csv != nullptr)
        {
            fprintf(Csv, "%.1f,%u,%d,%.1f,%.1f,%.1f,%.2f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%d,%.1f,%s,%.1f,%.1f,%.1f,%.1f,%.1f,%lld,%.1f,%u,%u,%ld,%ld,%d,%d,%ld,%ld",
                    t, Frame, gameseq_get_state(), mb(pmc.PrivateUsage), mb(pmc.PeakPagefileUsage), mb(pmc.WorkingSetSize),
                    (double)va_used / (1024.0 * 1024.0 * 1024.0), mb(provider.used), mb(provider.peak), mb(provider.total),
                    mb(vram.used), mb(vram.total), mb(bank.used), mb(bank.total), pools, mb(pools_used), fullest, fullest_pct,
                    mb((unsigned long long)hk_in_use), mb((unsigned long long)hk_peak), mb((unsigned long long)hk_total),
                    mb((unsigned long long)Crt_bytes), Crt_blocks, mb((unsigned long long)Crt_peak), far_count,
#ifdef OPENRFG_X64
                    et_far::Max_entries,
#else
                    0u,
#endif
                    Strpool_fallbacks, Strpool_unregistered, objects, Max_objects, Failure_messages, Overlay_hits);
            // level memory (layout block 6)
            fprintf(Csv, ",%.1f,%.1f,%.1f,%.1f,%ld,%d,%d,%d,%d,%d,%d", mb(level_used), mb(Peak_level), mb(Level_size), mb(level_largest),
                    Level_failures, cm_models, openrfg_max_collision_models(), movers, openrfg_general_movers_capacity(), zone_handles, zones);
            // the debris (memtest_debris.cpp)
            fprintf(Csv, ",%d,%d,%d,%u,%u,%u,%u,%u,%u,%u,%.1f,%.1f,%u,%u,%u", dc.render_instances, dc.render_instances_max, dc.zone_free,
                    dc.kept, dc.gated, dc.fades_blocked, dc.vaporize_refused, dc.recycled, dc.stream_skips, dc.blasts,
                    mb((unsigned long long)Havok_system_used), mb((unsigned long long)Havok_solver_used), dc.island_trims, dc.bodies_trimmed,
                    dc.havok_low_memory);
            // the grow-on-demand memory (src/scaling)
            fprintf(Csv, ",%.1f,%.1f,%u\n", mb(scaling_committed_total()), mb((unsigned long long)havok_memory_heap_page_bytes(nullptr)),
                    scaling_fault_count());
            fflush(Csv);
        }
    }

    void write_pools()
    {
        if (Pools_csv == nullptr)
        {
            return;
        }
        const double t = now_s();
        AcquireSRWLockExclusive(&Pool_lock);
        for (int i = 0; i < Pool_count; i++)
        {
            const pool_entry& e = Pools[i];
            if (e.valid)
            {
                fprintf(Pools_csv, "%.1f,%s,%u,%u,%u,%d\n", t, e.name, e.used, e.size, e.peak_seen, e.dynamic ? 1 : 0);
            }
        }
        ReleaseSRWLockExclusive(&Pool_lock);
        fflush(Pools_csv);
    }

    int compare_pool_fill(const void* a, const void* b)
    {
        const pool_entry* x = *(const pool_entry* const*)a;
        const pool_entry* y = *(const pool_entry* const*)b;
        const double fx = x->size ? (double)x->peak_seen / x->size : 0.0;
        const double fy = y->size ? (double)y->peak_seen / y->size : 0.0;
        return fx < fy ? 1 : fx > fy ? -1 : 0;
    }

    // ---- the grow-on-demand memory (src/scaling)
    unsigned long long scaling_committed_total()
    {
        unsigned long long total = 0;
        scaling_region_info info;
        for (int i = 0; scaling_region(i, &info); i++)
        {
            total += info.committed;
        }
        return total;
    }

    // ---- far-offset census: each new entry of the far table (endian_type.inl), by the memory area of the field that
    // needed it and of its target: a scaling region, a layout block, the program's image, or another system allocation.
    struct far_area
    {
        int kind; // 0 none, 1 scaling region, 2 layout block, 3 a module's image, 4 another allocation
        unsigned long long id;
    };
    struct far_pair
    {
        far_area field;
        far_area target;
        unsigned int count;
        const void* first_field;
        const void* first_target;
    };
    far_pair Far_pairs[64];
    int Far_pair_count;
    unsigned int Far_unsorted;

    far_area far_area_of(const void* p)
    {
        far_area a = {0, 0};
        if (p == nullptr)
        {
            return a;
        }
        scaling_region_info info;
        for (int i = 0; scaling_region(i, &info); i++)
        {
            if ((const char*)p >= (const char*)info.base && (const char*)p < (const char*)info.base + info.reserved)
            {
                a.kind = 1;
                a.id = (unsigned long long)i;
                return a;
            }
        }
        const void* start = nullptr;
        unsigned long long size = 0;
        const char* name = nullptr;
        for (unsigned int i = 0; keen::memory::memtestLayoutBlock(i, &start, &size, &name); i++)
        {
            if (start != nullptr && (const char*)p >= (const char*)start && (const char*)p < (const char*)start + size)
            {
                a.kind = 2;
                a.id = i;
                return a;
            }
        }
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0)
        {
            return a;
        }
        a.kind = mbi.Type == MEM_IMAGE ? 3 : 4;
        a.id = (unsigned long long)mbi.AllocationBase;
        return a;
    }

    // Under the far table's lock.
    void note_far_entry(const void* field, const void* target)
    {
        const far_area f = far_area_of(field);
        const far_area t = far_area_of(target);
        for (int i = 0; i < Far_pair_count; i++)
        {
            far_pair& p = Far_pairs[i];
            if (p.field.kind == f.kind && p.field.id == f.id && p.target.kind == t.kind && p.target.id == t.id)
            {
                p.count++;
                return;
            }
        }
        if (Far_pair_count < (int)(sizeof(Far_pairs) / sizeof(Far_pairs[0])))
        {
            far_pair& p = Far_pairs[Far_pair_count++];
            p.field = f;
            p.target = t;
            p.count = 1;
            p.first_field = field;
            p.first_target = target;
            return;
        }
        Far_unsorted++;
    }

    const char* far_area_label(const far_area& a, char* out, size_t size)
    {
        switch (a.kind)
        {
        case 1:
        {
            scaling_region_info info;
            if (scaling_region((int)a.id, &info))
            {
                char label[160];
                const char* name = strstr(info.name, "object_pool<") != nullptr ? pool_label(info.name, label, sizeof(label)) : info.name;
                snprintf(out, size, "region \"%s\" (%.0f MB at %p)", name, mb(info.reserved), info.base);
                return out;
            }
            break;
        }
        case 2:
        {
            const void* start = nullptr;
            unsigned long long bytes = 0;
            const char* name = nullptr;
            if (keen::memory::memtestLayoutBlock((unsigned int)a.id, &start, &bytes, &name))
            {
                snprintf(out, size, "layout block %u %s (%.0f MB at %p)", (unsigned int)a.id, name != nullptr ? name : "", mb(bytes), start);
                return out;
            }
            break;
        }
        case 3:
        {
            char path[MAX_PATH] = "";
            GetModuleFileNameA((HMODULE)a.id, path, sizeof(path));
            const char* file = strrchr(path, '\\');
            snprintf(out, size, "the image of %s (%p)", file != nullptr ? file + 1 : path, (const void*)a.id);
            return out;
        }
        case 4:
        {
            // The allocation's size: its regions while they belong to it.
            unsigned long long bytes = 0;
            const char* p = (const char*)a.id;
            MEMORY_BASIC_INFORMATION mbi;
            while (VirtualQuery(p + bytes, &mbi, sizeof(mbi)) != 0 && mbi.AllocationBase == (void*)a.id && mbi.RegionSize > 0)
            {
                bytes += mbi.RegionSize;
            }
            snprintf(out, size, "an allocation of %.1f MB at %p", mb(bytes), (const void*)a.id);
            return out;
        }
        default:
            break;
        }
        snprintf(out, size, "(nowhere known)");
        return out;
    }

    void write_far_census(FILE* f)
    {
        if (Far_pair_count == 0)
        {
            return;
        }
        // Largest first.
        far_pair sorted[64];
        const int n = Far_pair_count;
        memcpy(sorted, Far_pairs, sizeof(far_pair) * (size_t)n);
        for (int i = 1; i < n; i++)
        {
            for (int j = i; j > 0 && sorted[j].count > sorted[j - 1].count; j--)
            {
                const far_pair swap = sorted[j];
                sorted[j] = sorted[j - 1];
                sorted[j - 1] = swap;
            }
        }
        fprintf(f, "  far entries by where the field and its target lie (the first of each: field, target):\n");
        char a[200];
        char b[200];
        for (int i = 0; i < n; i++)
        {
            fprintf(f, "  %10u  %s\n              -> %s   [%p -> %p]\n", sorted[i].count, far_area_label(sorted[i].field, a, sizeof(a)),
                    far_area_label(sorted[i].target, b, sizeof(b)), sorted[i].first_field, sorted[i].first_target);
        }
        if (Far_unsorted > 0)
        {
            fprintf(f, "  %10u  in other pairs\n", Far_unsorted);
        }
    }

    void write_scaling_summary(FILE* f)
    {
        int pages = 0;
        const long long havok = havok_memory_heap_page_bytes(&pages);
        fprintf(f, "grow-on-demand memory (%s)\n", scaling_enabled() ? "on" : "off: OPENRFG_SCALING=0");
        if (havok > 0 || pages > 0)
        {
            fprintf(f, "  %-40s %10.1f MB in %d pages (at most 2 GB)\n", "Havok heap", mb((unsigned long long)havok), pages);
        }
        scaling_region_info info;
        char label[160];
        for (int i = 0; scaling_region(i, &info); i++)
        {
            if (info.committed >= (1u << 20) && strcmp(info.name, "Havok heap") != 0) // Havok's: the page line above
            {
                const char* name = strstr(info.name, "object_pool<") != nullptr ? pool_label(info.name, label, sizeof(label)) : info.name;
                fprintf(f, "  %-40s %10.1f MB committed of %.1f MB reserved\n", name, mb(info.committed), mb(info.reserved));
            }
        }
        fprintf(f, "  %d regions, %u granules committed on first touch, %u commits refused by the system\n", scaling_region_count(),
                scaling_fault_count(), scaling_commit_failures());
        scaling_pool_info pools[512];
        const int grown = scaling_grown_pools(pools, 512);
        for (int i = 0; i < grown; i++)
        {
            fprintf(f, "  pool grew: %-60s %u -> %u objects\n", pool_label(pools[i].name, label, sizeof(label)), pools[i].initial,
                    pools[i].objects);
        }
        const int exhausted = scaling_exhausted_pools(pools, 512);
        for (int i = 0; i < exhausted; i++)
        {
            fprintf(f, "  pool FULL: %-60s at %u objects\n", pool_label(pools[i].name, label, sizeof(label)), pools[i].objects);
        }
        fprintf(f, "\n");
    }

    // Growth events: a region (or Havok's heap) passing the next 64 MB step, or doubling below 64 MB.
    unsigned long long Scaling_reported[4096];
    long long Havok_pages_reported;

    bool grew(unsigned long long now, unsigned long long& reported)
    {
        const unsigned long long step = 64ull << 20;
        const bool passed = reported < step ? now >= 2 * reported && now >= (1ull << 20) && now > reported : now / step > reported / step;
        if (passed)
        {
            reported = now;
        }
        return passed;
    }

    // "object_pool<struct glass_shard,750,1>" out of a pool's __FUNCSIG__.
    const char* pool_label(const char* name, char* out, size_t size)
    {
        if (name == nullptr)
        {
            return "(a pool without a name)";
        }
        const char* p = strstr(name, "object_pool<");
        const char* e = p != nullptr ? strstr(p, ">::") : nullptr;
        if (p == nullptr || e == nullptr)
        {
            return name;
        }
        const size_t n = (size_t)(e - p + 1) < size - 1 ? (size_t)(e - p + 1) : size - 1;
        memcpy(out, p, n);
        out[n] = '\0';
        return out;
    }

    int Exhausted_reported;
    unsigned int Grown_reported[512];
    const char* Grown_names[512];
    int Grown_names_count;

    void report_pools()
    {
        scaling_pool_info pools[512];
        char label[160];
        const int exhausted = scaling_exhausted_pools(pools, 512);
        for (; Exhausted_reported < exhausted; Exhausted_reported++)
        {
            memtest_event("pool: %s is full (%u objects)", pool_label(pools[Exhausted_reported].name, label, sizeof(label)),
                          pools[Exhausted_reported].objects);
        }
        const int grown = scaling_grown_pools(pools, 512);
        for (int i = 0; i < grown; i++)
        {
            int k = 0;
            while (k < Grown_names_count && Grown_names[k] != pools[i].name)
            {
                k++;
            }
            if (k == Grown_names_count)
            {
                if (Grown_names_count >= 512)
                {
                    continue;
                }
                Grown_names[Grown_names_count] = pools[i].name;
                Grown_reported[Grown_names_count] = pools[i].initial;
                Grown_names_count++;
            }
            if (pools[i].objects >= 2 * Grown_reported[k])
            {
                memtest_event("scaling: pool %s grew %u -> %u objects", pool_label(pools[i].name, label, sizeof(label)), pools[i].initial,
                              pools[i].objects);
                Grown_reported[k] = pools[i].objects;
            }
        }
    }

    void report_scaling()
    {
        report_pools();
        scaling_region_info info;
        for (int i = 0; i < 4096 && scaling_region(i, &info); i++)
        {
            if (grew(info.committed, Scaling_reported[i]))
            {
                memtest_event("scaling: %s grew to %.1f MB committed (of %.1f MB reserved)", info.name, mb(info.committed), mb(info.reserved));
            }
        }
        int pages = 0;
        unsigned long long havok = (unsigned long long)havok_memory_heap_page_bytes(&pages);
        unsigned long long reported = (unsigned long long)Havok_pages_reported;
        if (grew(havok, reported))
        {
            Havok_pages_reported = (long long)reported;
            memtest_event("scaling: Havok's heap grew to %.1f MB (%d page%s)", mb(havok), pages, pages == 1 ? "" : "s");
        }
    }

    void write_summary(const char* reason)
    {
        FILE* f = open_out("memtest_summary.txt", "w");
        if (f == nullptr)
        {
            return;
        }
        fprintf(f, "OpenRFG64 memtest summary (%s) at %.1f s, frame %u\n\n", reason, now_s(), Frame);
        fprintf(f, "process\n");
        fprintf(f, "  peak private bytes      %10.1f MB%s\n", mb(Peak_private), Peak_private > 4ull << 30 ? "  (more than a 32-bit process can address)" : "");
        fprintf(f, "  passed 4 GB at          %10s\n", Over_4gb_at < 0 ? "never" : "");
        if (Over_4gb_at >= 0)
        {
            fprintf(f, "                          %10.1f s\n", Over_4gb_at);
        }
        fprintf(f, "  peak working set        %10.1f MB\n\n", mb(Peak_ws));
        fprintf(f, "game budgets\n");
        fprintf(f, "  RFG_MempoolProvider     %10.1f of %.1f MB at the peak (every game pool is carved from it)\n", mb(Peak_provider), mb(Provider_total));
        fprintf(f, "  Havok heap              %10.1f of %.1f MB at the peak%s\n", mb((unsigned long long)Peak_havok_inuse), mb((unsigned long long)Havok_total),
                Havok_oom ? "  ** OUT OF MEMORY **" : "");
        fprintf(f, "  C runtime heap (x64)    %10.1f MB at the peak (dlmalloc's replacement: unbounded)\n", mb((unsigned long long)Crt_peak));
        fprintf(f, "  level memory (block 6)  %10.1f of %.1f MB at the peak, %ld failed allocations%s%s\n", mb(Peak_level), mb(Level_size),
                Level_failures, Level_first_failure[0] ? "; the first: " : "", Level_first_failure);
        fprintf(f, "  world objects           %10d of %d at the peak (indices are 16-bit: at most 65,535)\n", Peak_objects, Max_objects);
        fprintf(f, "  collision model table   %10d of %d at the peak; indices handed out up to %u, %d at most kept past the chunks' 16 bits\n",
                Peak_cm_models, openrfg_max_collision_models(), Cm_high_water, Peak_cm_side);
        unsigned int first_miss = 0;
        const unsigned int cm_misses = openrfg_cm_misses(&first_miss);
        if (cm_misses != 0)
        {
            fprintf(f, "  ** %u Havok bodies asked for a collision model that is not there (the first: index %u) **\n", cm_misses, first_miss);
        }
        fprintf(f, "  debris (general movers) %10d of %d at the peak (the pool links its elements with 16-bit indices)\n", Peak_movers,
                openrfg_general_movers_capacity());
        fprintf(f, "  zone object handles     %10d of 7280 at the peak in the fullest zone (a fixed bitmap per zone)\n", Peak_zone_handles);
        fprintf(f, "  destroyable render inst.%10d of %d at the peak%s\n\n", Peak_render, Render_max,
                scaling_enabled() ? " (they grow when they run out)" : "");
        fprintf(f, "debris (rfgphys's keep_debris in the source: %s)\n", Debris.keep ? "on" : "off");
        fprintf(f, "  kept from the game's deletion at shatter %u, deleted by the zone budget %u, fades refused %u, vaporizing refused %u\n",
                Debris.kept, Debris.gated, Debris.fades_blocked, Debris.vaporize_refused);
        fprintf(f, "  recycled by the zone budget %u, streaming met a parent that is gone %u, demolition vehicle bombs %u\n",
                Debris.recycled, Debris.stream_skips, Debris.blasts);
        fprintf(f, "  compiles cancelled with the request pool empty %u\n", Debris.recompiles_cancelled);
        fprintf(f, "  Havok: islands trimmed by the watchdog %u (%u bodies destroyed: solver memory), low-memory calls %u\n\n",
                Debris.island_trims, Debris.bodies_trimmed, Debris.havok_low_memory);
        write_scaling_summary(f);
        fprintf(f, "32-bit structures\n");
#ifdef OPENRFG_X64
        fprintf(f, "  far offset table        %10u of %u entries at the peak%s\n", Peak_far, et_far::Max_entries,
                Peak_far >= et_far::Max_entries ? "  ** FULL: offsets are being truncated **" : "");
        write_far_census(f);
#endif
        fprintf(f, "  string table fallbacks  %10ld records outside their arena, %ld tables without an arena\n\n", Strpool_fallbacks, Strpool_unregistered);
        fprintf(f, "failure messages from the game: %ld%s%s\n\n", Failure_messages, Last_failure[0] ? ", the last: " : "", Last_failure);
        // The pools by how full they got.
        pool_entry* sorted[Max_pools];
        int n = 0;
        AcquireSRWLockExclusive(&Pool_lock);
        for (int i = 0; i < Pool_count; i++)
        {
            if (Pools[i].valid && Pools[i].size >= 0x10000 && !Pools[i].dynamic)
            {
                sorted[n++] = &Pools[i];
            }
        }
        qsort(sorted, (size_t)n, sizeof(sorted[0]), compare_pool_fill);
        fprintf(f, "game pools by peak fill (%d pools of 64 KB or more; registry %d%s)\n", n, Pool_count, Pool_registry_overflow ? ", overflowed" : "");
        fprintf(f, "  %-32s %12s %12s %7s  %s\n", "pool", "peak", "size", "fill", "full at");
        for (int i = 0; i < n && i < 40; i++)
        {
            const pool_entry& e = *sorted[i];
            char full[32] = "";
            if (e.full_at >= 0)
            {
                _snprintf_s(full, sizeof(full), _TRUNCATE, "%.1f s", e.full_at);
            }
            else if (e.full_at == -2.0)
            {
                strcpy_s(full, "whole from the start");
            }
            fprintf(f, "  %-32s %12u %12u %6.1f%%  %s\n", e.name, e.peak_seen, e.size, e.size ? 100.0 * e.peak_seen / e.size : 0.0, full);
        }
        ReleaseSRWLockExclusive(&Pool_lock);
        fclose(f);
    }

    // ---- the Wrecking Crew start (memtest_frame): the menu loaded for a second -> options -> match begin.
    int hex_value(char c)
    {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    }

    void wc_drive()
    {
        if (Wc_map == nullptr || Wc_started)
        {
            return;
        }
        if (gameseq_get_state() != GS_WRECKING_CREW_MAIN_MENU || openrfg_wc_menu_state() != 1 /* MMS_WC_LOADED */)
        {
            Wc_loaded_since = 0;
            return;
        }
        const unsigned long long now = GetTickCount64();
        if (Wc_loaded_since == 0)
        {
            Wc_loaded_since = now;
            return;
        }
        if (now - Wc_loaded_since < 1000)
        {
            return;
        }
        Wc_started = true;
        // Total chaos, 1 player, weapon 4 (thermobaric rockets), no time limit: rfgphys's avenue default.
        const char* hex = Wc_options_hex != nullptr ? Wc_options_hex : "030000000100000013000000120000000400000000000000FFFFFFFF010000000500000000010000FFFFFFFF";
        unsigned char bytes[sizeof(wrecking_crew_options)];
        memcpy(bytes, &Wrecking_crew_options, sizeof(bytes));
        for (size_t i = 0; i < sizeof(bytes) && hex[2 * i] != '\0' && hex[2 * i + 1] != '\0'; i++)
        {
            const int hi = hex_value(hex[2 * i]);
            const int lo = hex_value(hex[2 * i + 1]);
            if (hi < 0 || lo < 0)
            {
                break;
            }
            bytes[i] = (unsigned char)(hi << 4 | lo);
        }
        memcpy(&Wrecking_crew_options, bytes, sizeof(bytes));
        const int count = wrecking_crew_get_num_maps();
        int index = -1;
        for (int i = 0; i < count; i++)
        {
            const char* file = wrecking_crew_get_map_file_name(i);
            if (file != nullptr && _stricmp(file, Wc_map) == 0)
            {
                index = i;
            }
        }
        if (index < 0)
        {
            char list[1024] = "";
            for (int i = 0; i < count; i++)
            {
                const char* file = wrecking_crew_get_map_file_name(i);
                strncat_s(list, file ? file : "?", _TRUNCATE);
                strncat_s(list, " ", _TRUNCATE);
            }
            memtest_event("wc: map %s is not in the Wrecking Crew list (%d maps: %s); not starting", Wc_map, count, list);
            return;
        }
        Wrecking_crew_options.map_index = index;
        Wrecking_crew_options.current_challenge_id = -1;
        memtest_event("wc: starting %s (map %d of %d), mode %d, %d player(s), weapon %d, free play %d", Wc_map, index, count,
                      (int)Wrecking_crew_options.mode, Wrecking_crew_options.num_players, Wrecking_crew_options.weapon_index,
                      Wrecking_crew_options.free_play ? 1 : 0);
        Wc_started_ms = now;
        wrecking_crew_match_begin();
    }
}

// ---- lifecycle

void memtest_init()
{
    if (Initialized)
    {
        return;
    }
    Initialized = true;
    Start_ms = GetTickCount64();
    scaling_static_init_done(); // scaling: pools made from now on keep the game's fixed storage
#ifdef OPENRFG_X64
    et_far::On_new_entry = note_far_entry;
#endif
    const unsigned char* base = (const unsigned char*)GetModuleHandleA(nullptr);
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    Image_lo = (uintptr_t)base;
    Image_hi = Image_lo + nt->OptionalHeader.SizeOfImage;

    const char* dir = Out_dir_arg;
    char env_dir[MAX_PATH];
    if (dir == nullptr && GetEnvironmentVariableA("OPENRFG_RUN_DIR", env_dir, sizeof(env_dir)) > 0)
    {
        dir = env_dir;
    }
    if (dir != nullptr)
    {
        strncpy_s(Out_dir, dir, _TRUNCATE);
    }
    char interval[32];
    if (GetEnvironmentVariableA("OPENRFG_MEMTEST_INTERVALMS", interval, sizeof(interval)) > 0 && atoi(interval) >= 100)
    {
        Interval_ms = (unsigned int)atoi(interval);
    }
    Events = open_out("memtest_events.log", "w");
    Native = open_out("memtest_native.log", "w");
    Csv = open_out("memtest.csv", "w");
    Pools_csv = open_out("memtest_pools.csv", "w");
    if (Csv != nullptr)
    {
        fprintf(Csv, "t_s,frame,state,private_mb,private_peak_mb,working_set_mb,va_used_gb,provider_used_mb,provider_peak_mb,provider_total_mb,"
                     "vram_pool_used_mb,vram_pool_total_mb,audio_bank_used_mb,audio_bank_total_mb,pools,pools_used_mb,fullest_pool,fullest_pct,"
                     "havok_inuse_mb,havok_peak_mb,havok_total_mb,crt_heap_mb,crt_blocks,crt_peak_mb,far_entries,far_max,strpool_fallbacks,"
                     "strpool_no_arena,objects,max_objects,failure_messages,overlay_hits,level_used_mb,level_peak_mb,level_size_mb,"
                     "level_largest_free_mb,level_failures,collision_models,collision_models_max,general_movers,general_movers_max,"
                     "zone_handles_used,zones,render_instances,render_instances_max,zone_free,debris_kept,debris_deleted_by_budget,"
                     "fades_blocked,vaporize_refused,debris_recycled,stream_parent_gone,demolish_bombs,havok_system_mb,havok_solver_mb,"
                     "island_trims,bodies_trimmed,havok_low_memory,scaling_committed_mb,havok_pages_mb,scaling_faults\n");
        fflush(Csv);
    }
    if (Pools_csv != nullptr)
    {
        fprintf(Pools_csv, "t_s,pool,used,size,peak,dynamic\n");
        fflush(Pools_csv);
    }
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, sizeof(exe));
    memtest_event("memtest: %s, %s build of %s %s, census every %u ms, output in %s", exe,
#ifdef OPENRFG_X64
                  "64-bit",
#else
                  "32-bit",
#endif
                  __DATE__, __TIME__, Interval_ms, Out_dir);
    memtest_event("memtest: command line: %s", GetCommandLineA());
    if (const char* ini = memtest_config_ini_path())
    {
        memtest_event("memtest: config.ini redirected to %s", ini);
    }
    if (Sledge_dir != nullptr)
    {
        load_sledge(Sledge_dir);
    }
    if (Plugins_dir != nullptr)
    {
        load_plugins(Plugins_dir);
    }
    if (Wc_map != nullptr)
    {
        memtest_event("wc: will start a Wrecking Crew match on %s from the main menu", Wc_map);
    }
    // The summary when the game ends normally (the player quits), from what the census recorded while it ran. No last
    // census: by now main() has ended and the memory system is destroyed, and a pool destroyed inside another one keeps
    // its base class's vtable (in the image, so read_pool's check passes) and a call into it is a pure virtual call:
    // abort(), which no __try catches (the first quit of a session, 2026-11-19).
    atexit([]() {
        if (!Initialized)
        {
            return;
        }
        write_pools();
        write_summary("the game ended");
        memtest_event("memtest: the game ended at frame %u", Frame);
    });
}

void memtest_frame()
{
    if (!Initialized)
    {
        return;
    }
    Frame++;
    const int state = (int)gameseq_get_state();
    if (state != Last_state)
    {
        PROCESS_MEMORY_COUNTERS_EX pmc = {};
        pmc.cb = sizeof(pmc);
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));
        memtest_event("state: %d %s -> %d %s (frame %u, private %.0f MB)", Last_state, state_name(Last_state), state, state_name(state), Frame, mb(pmc.PrivateUsage));
        Last_state = state;
    }
    wc_drive();
    report_scaling();
    const unsigned long long now = GetTickCount64();
    if (now - Last_census_ms >= Interval_ms)
    {
        Last_census_ms = now;
        census();
    }
    if (now - Last_pools_ms >= 10000)
    {
        Last_pools_ms = now;
        write_pools();
    }
    if (now - Last_summary_ms >= 10000)
    {
        Last_summary_ms = now;
        write_summary("running");
    }
    if (Exit_after_s > 0 && Wc_started_ms != 0 && now - Wc_started_ms >= (unsigned long long)Exit_after_s * 1000ull)
    {
        census();
        write_pools();
        write_summary("--exit-after");
        memtest_event("memtest: --exit-after %d s reached: ending the process", Exit_after_s);
        TerminateProcess(GetCurrentProcess(), 0);
    }
}

void memtest_level_event(const char* what)
{
    memtest_event("level: %s", what);
}

// ---- what the recovered code reports

void memtest_pool_register(mempool_base* pool)
{
    AcquireSRWLockExclusive(&Pool_lock);
    for (int i = 0; i < Pool_count; i++)
    {
        if (Pools[i].pool == pool)
        {
            ReleaseSRWLockExclusive(&Pool_lock);
            return;
        }
    }
    if (Pool_count < Max_pools)
    {
        pool_entry& e = Pools[Pool_count++];
        memset(&e, 0, sizeof(e));
        e.pool = pool;
        e.full_at = -1.0;
    }
    else
    {
        Pool_registry_overflow++;
    }
    ReleaseSRWLockExclusive(&Pool_lock);
}

void memtest_pool_unregister(mempool_base* pool)
{
    AcquireSRWLockExclusive(&Pool_lock);
    for (int i = 0; i < Pool_count; i++)
    {
        if (Pools[i].pool == pool)
        {
            Pools[i] = Pools[--Pool_count];
            break;
        }
    }
    ReleaseSRWLockExclusive(&Pool_lock);
}

void memtest_native_print(const char* text)
{
    if (Native == nullptr || text == nullptr)
    {
        return;
    }
    AcquireSRWLockExclusive(&Log_lock);
    fputs(text, Native);
    const size_t n = strlen(text);
    if (n == 0 || text[n - 1] != '\n')
    {
        fputc('\n', Native);
    }
    fflush(Native);
    ReleaseSRWLockExclusive(&Log_lock);
    // Messages that mean something ran out.
    static const char* const patterns[] = {"out of memory", "Out of memory", "overflow", "Overflow", "Cannot alloc", "alloc failed",
                                           "allocation fail", "allocationFailure", "full!", "is full", "exhausted", "not enough memory",
                                           "Too many", "too many", "increase MAX", "Failed to alloc", "failed to alloc"};
    for (const char* p : patterns)
    {
        if (strstr(text, p) != nullptr)
        {
            InterlockedIncrement(&Failure_messages);
            strncpy_s(Last_failure, text, _TRUNCATE);
            for (char* c = Last_failure; *c; c++)
            {
                if (*c == '\n' || *c == '\r')
                {
                    *c = ' ';
                }
            }
            char line[300];
            _snprintf_s(line, sizeof(line), _TRUNCATE, "game: %s", Last_failure);
            log_line(Events, line);
            break;
        }
    }
}

void memtest_event(const char* format, ...)
{
    char text[1024];
    va_list args;
    va_start(args, format);
    _vsnprintf_s(text, sizeof(text), _TRUNCATE, format, args);
    va_end(args);
    log_line(Events, text);
}

void memtest_crt_alloc(size_t bytes)
{
    const long long now = InterlockedAdd64(&Crt_bytes, (long long)bytes);
    InterlockedIncrement64(&Crt_blocks);
    long long peak = Crt_peak;
    while (now > peak)
    {
        const long long seen = InterlockedCompareExchange64(&Crt_peak, now, peak);
        if (seen == peak)
        {
            break;
        }
        peak = seen;
    }
}

void memtest_crt_free(size_t bytes)
{
    InterlockedAdd64(&Crt_bytes, -(long long)bytes);
    InterlockedDecrement64(&Crt_blocks);
}

void memtest_strpool_fallback()
{
    InterlockedIncrement(&Strpool_fallbacks);
}

void memtest_strpool_arena_unregistered()
{
    InterlockedIncrement(&Strpool_unregistered);
}

void memtest_havok_out_of_memory(unsigned int size, bool final_failure)
{
    if (!final_failure)
    {
        memtest_event("havok: cannot allocate %u bytes: the game frees what it can", size);
        return;
    }
    InterlockedIncrement(&Havok_oom);
    memtest_event("havok: OUT OF MEMORY (%u bytes): rfg.exe would hang here; the census is written and the process ends", size);
    census();
    write_pools();
    write_summary("Havok out of memory");
    TerminateProcess(GetCurrentProcess(), 3);
}

void memtest_level_alloc_failed(const void* manager, unsigned int size, const char* name)
{
    const bool level = manager == &Level_memmgr;
    if (level)
    {
        InterlockedIncrement(&Level_failures);
    }
    // Read the manager's figures without its lock: approximate, for the log only.
    const split_memmgr* m = static_cast<const split_memmgr*>(manager);
    split_memmgr* mm = const_cast<split_memmgr*>(m);
    char line[200];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "%s: alloc of %u bytes (%s) FAILED: %u of %u bytes used, largest possible %u",
                level ? "level memory" : "split_memmgr", size, name ? name : "-", mm->space_used(), mm->space_max(),
                mm->largest_allocation_possible());
    if (level && Level_first_failure[0] == '\0')
    {
        strncpy_s(Level_first_failure, line, _TRUNCATE);
    }
    log_line(Events, line);
}

// ---- for the plugins

// The target of a self-relative 32-bit offset field of the game's data (et_ptr_offset). On x64 such a field may hold an
// index into the far table (a target more than 2 GB away, endian_type.inl), which a plugin cannot read itself; the
// converted rfgphys calls this (GetProcAddress on the program). Null-able conventions (0 or -1) are the caller's.
extern "C" __declspec(dllexport) const void* openrfg_et_ptr_target(const void* field)
{
    const int offset = *static_cast<const int*>(field);
#ifdef OPENRFG_X64
    if (offset >= et_far::Base)
    {
        return et_far::far_target(offset);
    }
#endif
    return static_cast<const char*>(field) + offset;
}

// ---- settings

const char* memtest_config_ini_path()
{
    static char path[MAX_PATH];
    static int state = 0;
    if (state == 0)
    {
        state = GetEnvironmentVariableA("OPENRFG_CONFIG_INI", path, sizeof(path)) > 0 ? 1 : 2;
    }
    return state == 1 ? path : nullptr;
}

uint64_t memtest_budget(const char* name, uint64_t default_bytes)
{
    char var[96];
    char value[64];
    _snprintf_s(var, sizeof(var), _TRUNCATE, "OPENRFG_MEMTEST_%s", name);
    if (GetEnvironmentVariableA(var, value, sizeof(value)) == 0)
    {
        return default_bytes;
    }
    char* end = nullptr;
    unsigned long long v = _strtoui64(value, &end, 0);
    if (end != nullptr)
    {
        if (*end == 'k' || *end == 'K') v <<= 10;
        else if (*end == 'm' || *end == 'M') v <<= 20;
        else if (*end == 'g' || *end == 'G') v <<= 30;
    }
    memtest_event("budget: %s = %llu bytes (default %llu)", name, v, (unsigned long long)default_bytes);
    return v;
}

// ---- the Wrecking Crew start

const char* memtest_wc_map()
{
    return Wc_map;
}

// Unattended runs (OPENRFG_MEMTEST_WCCONTINUE=1, set by the run script with -IgnoreFocus): the match's "continue"
// screens are passed after 1.5 s, the first one before the player's turn among them (the match waits there).
bool memtest_wc_continue()
{
    static int on = -1;
    if (on < 0)
    {
        char value[8] = "";
        on = GetEnvironmentVariableA("OPENRFG_MEMTEST_WCCONTINUE", value, sizeof(value)) > 0 && value[0] == '1' ? 1 : 0;
    }
    if (on == 0)
    {
        return false;
    }
    static unsigned long long since = 0;
    const unsigned long long now = GetTickCount64();
    if (since == 0 || now - since > 5000)
    {
        since = now; // a new wait (no call for a while: the last one ended)
        return false;
    }
    if (now - since < 1500)
    {
        return false;
    }
    since = 0;
    memtest_event("wc: continue pressed (unattended run), Wrecking Crew state %d", (int)wrecking_crew_get_state());
    return true;
}

// ---- the Sledge layer

const char* memtest_overlay_mount(int i, const char** mount_name)
{
    if (i < 0 || i >= Mount_count)
    {
        return nullptr;
    }
    *mount_name = Mount_name[i];
    return Mount_native[i];
}

bool memtest_overlay_find(cf_found_info* found_info, const char* filename)
{
    if (Overlay_file_count == 0 || filename == nullptr)
    {
        return false;
    }
    const char* base = filename;
    for (const char* p = filename; *p; p++)
    {
        if (*p == '\\' || *p == '/' || *p == ':')
        {
            base = p + 1;
        }
    }
    for (int i = 0; i < Overlay_file_count; i++)
    {
        const overlay_file& o = Overlay_files[i];
        if (_stricmp(base, o.name) != 0)
        {
            continue;
        }
        strncpy(found_info->m_full_name, o.keen_path, cf_found_info::CFFI_FULL_NAME_ARRAY_SIZE);
        found_info->m_full_name[cf_found_info::CFFI_FULL_NAME_ARRAY_SIZE - 1] = '\0';
        found_info->m_size = (unsigned int)o.size;
        found_info->m_searched_system = CF_SEARCH_STANDARD;
        found_info->m_media_type = CF_IO_MEDIA_HDD;
        found_info->vpe = nullptr;
        if (InterlockedIncrement(&Overlay_hits) <= 50)
        {
            memtest_event("sledge: %s served from %s", filename, o.keen_path);
        }
        return true;
    }
    return false;
}

int memtest_wc_map_additions(const memtest_wc_map_addition** additions)
{
    *additions = Wc_additions;
    return Wc_addition_count;
}
