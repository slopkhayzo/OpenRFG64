// Stand-in for binkw32.dll (bink.h), new code: built instead of linking lib/win32/binkw32.lib when CMake's
// OPENRFG_STUB_BINK is on. Every movie opens as a black 16x16 movie of one frame, so the video player takes its normal
// path: it allocates the frame buffers, creates the Y/cR/cB textures and draws the video's rectangle in black. A video
// played once ends on its first decoded frame (video_player.cpp, bink_do_frame_sync: FrameNum == Frames) and its stop
// callback runs; a looping one stays black. Returning no movie instead (a failed play) left the rectangle undrawn, so
// whatever was under it showed, and skipped the stop callbacks the menus wait on.
//
// Real playback: when bink_helper32.exe (tools/bink_helper, a 32-bit program) is next to the game exe and binkw32.dll is
// found in the game folder, each movie runs in its own helper process which decodes it and plays its sound; the decoded
// YUV planes come back through shared memory. Any failure falls back to the black movie above.
//
// Only the 23 functions rfg.exe imports are provided (bink.h); the DLL's other exports are not used. The program does
// not need binkw32.dll next to it.
#include "library/modules/thirdparty/bink/bink_1_300_7_0/include/bink.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../../../tools/bink_helper/bink_ipc.h"

namespace
{
// The fake movie: 16x16, one frame at 30 frames per second.
constexpr unsigned int Fake_width = 16;
constexpr unsigned int Fake_height = 16;
constexpr unsigned int Fake_frames = 1;
constexpr unsigned int Fake_frame_rate = 30;

// Black in YCbCr: Y 0 with both chroma planes at their midpoint 0x80. Y 0 (not video-range 16) stays black whether the
// bink shader expands video range (the negative result clamps to 0) or reads the planes as full range.
constexpr unsigned char Black_y = 0x00;
constexpr unsigned char Black_chroma = 0x80;

// Magic number: one handle per video tracker (video_player.cpp, Video_tracker_pool holds 8).
constexpr int Max_open_movies = 8;

// Magic number: the read buffer size reported by BinkGetRealtime. The buffer is reported full, so the video player's
// throttle (bink_maybe_throttle) never holds the streaming system's disc processing paused for a movie that reads
// nothing (100 * used / (size + 1) = 99, above its 80% resume mark).
constexpr unsigned long long Fake_read_buffer_size = 0x10000;

BINK Movies[Max_open_movies];
volatile long Movie_in_use[Max_open_movies];

// One helper process per open movie (proc == nullptr: the black stand-in movie).
struct helper_state
{
    HANDLE proc;
    HANDLE to_child;
    HANDLE from_child;
    HANDLE map;
    unsigned char* view;
    unsigned int plane_off[9];
    unsigned int total_sets;
    bool registered;
    BINKPLANE game_y;
    BINKPLANE game_cr;
    BINKPLANE game_cb;
    unsigned int y_height;
    unsigned int c_height;
};
helper_state Helpers[Max_open_movies];

// Set by BinkSetSoundTrack / BinkSetIOSize just before BinkOpen, handed to the helper.
unsigned int Pending_tracks[8];
unsigned int Pending_num_tracks;
unsigned long long Pending_io_size;
volatile long Log_count;

void bink_log(const char* fmt, ...)
{
    if (_InterlockedIncrement(&Log_count) > 400)
    {
        return;
    }
    FILE* f = fopen("bink_proxy.log", "a");
    if (f == nullptr)
    {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

bool read_all(HANDLE h, void* data, DWORD size)
{
    char* p = (char*)data;
    while (size > 0)
    {
        DWORD got = 0;
        if (!ReadFile(h, p, size, &got, nullptr) || got == 0)
        {
            return false;
        }
        p += got;
        size -= got;
    }
    return true;
}

bool write_all(HANDLE h, const void* data, DWORD size)
{
    const char* p = (const char*)data;
    while (size > 0)
    {
        DWORD put = 0;
        if (!WriteFile(h, p, size, &put, nullptr) || put == 0)
        {
            return false;
        }
        p += put;
        size -= put;
    }
    return true;
}

void helper_release(helper_state& h)
{
    if (h.view != nullptr)
    {
        UnmapViewOfFile(h.view);
    }
    if (h.map != nullptr)
    {
        CloseHandle(h.map);
    }
    if (h.to_child != nullptr)
    {
        CloseHandle(h.to_child);
    }
    if (h.from_child != nullptr)
    {
        CloseHandle(h.from_child);
    }
    if (h.proc != nullptr)
    {
        if (WaitForSingleObject(h.proc, 1500) != WAIT_OBJECT_0)
        {
            TerminateProcess(h.proc, 0);
        }
        CloseHandle(h.proc);
    }
    memset(&h, 0, sizeof(h));
}

// A request and its answer; false (and the movie falls back to black) when the helper died.
bool helper_rpc(int index, bink_ipc_msg& m, bink_ipc_reply& r)
{
    helper_state& h = Helpers[index];
    if (h.proc == nullptr)
    {
        return false;
    }
    if (!write_all(h.to_child, &m, sizeof(m)) || !read_all(h.from_child, &r, sizeof(r)))
    {
        bink_log("helper %d lost (cmd %u)", index, m.cmd);
        BINK* bink = &Movies[index];
        helper_release(h);
        bink->FrameNum = bink->Frames; // a video played once ends
        return false;
    }
    return true;
}

void apply_reply(BINK* bink, const bink_ipc_reply& r)
{
    bink->FrameNum = r.frame_num;
    bink->LastFrameNum = r.last_frame_num;
}

// Folder holding binkw32.dll: the environment override, then the movie's folders upward, then the exe's folder.
bool find_dll_dir(const char* movie, char* out, size_t out_size)
{
    char dir[MAX_PATH];
    char probe[MAX_PATH + 16];
    const DWORD env = GetEnvironmentVariableA("OPENRFG_BINKW32_DIR", dir, MAX_PATH);
    if (env > 0 && env < MAX_PATH)
    {
        _snprintf(probe, sizeof(probe), "%s\\binkw32.dll", dir);
        if (GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES)
        {
            strncpy(out, dir, out_size - 1);
            out[out_size - 1] = 0;
            return true;
        }
    }
    strncpy(dir, movie, MAX_PATH - 1);
    dir[MAX_PATH - 1] = 0;
    for (int up = 0; up < 8; up++)
    {
        char* slash = strrchr(dir, '\\');
        char* slash2 = strrchr(dir, '/');
        if (slash2 != nullptr && (slash == nullptr || slash2 > slash))
        {
            slash = slash2;
        }
        if (slash == nullptr)
        {
            break;
        }
        *slash = 0;
        _snprintf(probe, sizeof(probe), "%s\\binkw32.dll", dir);
        if (GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES)
        {
            strncpy(out, dir, out_size - 1);
            out[out_size - 1] = 0;
            return true;
        }
    }
    return false;
}

// Starts bink_helper32.exe (next to the game exe) and opens the movie in it. Fills the BINK head from its answer.
bool helper_open(int index, const char* name, unsigned int flags, BINK* bink)
{
    helper_state& h = Helpers[index];
    memset(&h, 0, sizeof(h));
    char exe[MAX_PATH];
    if (GetModuleFileNameA(nullptr, exe, MAX_PATH) == 0)
    {
        return false;
    }
    char* slash = strrchr(exe, '\\');
    if (slash == nullptr)
    {
        return false;
    }
    strcpy(slash + 1, "bink_helper32.exe");
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES)
    {
        bink_log("no %s (build tools/bink_helper): black video", exe);
        return false;
    }
    char dll_dir[MAX_PATH];
    if (!find_dll_dir(name, dll_dir, sizeof(dll_dir)))
    {
        bink_log("binkw32.dll not found near %s: black video", name);
        return false;
    }

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;
    HANDLE child_in_r = nullptr, child_in_w = nullptr, child_out_r = nullptr, child_out_w = nullptr;
    if (!CreatePipe(&child_in_r, &child_in_w, &sa, 0) || !CreatePipe(&child_out_r, &child_out_w, &sa, 0))
    {
        return false;
    }
    SetHandleInformation(child_in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(child_out_r, HANDLE_FLAG_INHERIT, 0);

    char cmd[2 * MAX_PATH + 8];
    _snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\"", exe, dll_dir);
    cmd[sizeof(cmd) - 1] = 0;
    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = child_in_r;
    si.hStdOutput = child_out_w;
    si.hStdError = child_out_w;
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));
    const BOOL started = CreateProcessA(nullptr, cmd, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(child_in_r);
    CloseHandle(child_out_w);
    if (!started)
    {
        bink_log("CreateProcess failed (%lu): %s", GetLastError(), cmd);
        CloseHandle(child_in_w);
        CloseHandle(child_out_r);
        return false;
    }
    CloseHandle(pi.hThread);
    h.proc = pi.hProcess;
    h.to_child = child_in_w;
    h.from_child = child_out_r;

    bink_ipc_reply ready;
    if (!read_all(h.from_child, &ready, sizeof(ready)) || ready.rc != 0)
    {
        bink_log("helper did not start (rc %d)", ready.rc);
        helper_release(h);
        return false;
    }
    bink_ipc_msg m;
    memset(&m, 0, sizeof(m));
    m.cmd = BINKIPC_OPEN;
    m.a = (int)flags;
    m.b = (int)(unsigned int)(Pending_io_size & 0xFFFFFFFFull);
    m.c = (int)(unsigned int)(Pending_io_size >> 32);
    m.num_tracks = Pending_num_tracks;
    memcpy(m.tracks, Pending_tracks, sizeof(m.tracks));
    strncpy(m.path, name, sizeof(m.path) - 1);
    bink_ipc_reply r;
    memset(&r, 0, sizeof(r));
    if (!helper_rpc(index, m, r) || r.rc != 0 || r.width == 0 || r.height == 0)
    {
        bink_log("helper could not open %s", name);
        helper_release(Helpers[index]);
        return false;
    }
    bink->Width = r.width;
    bink->Height = r.height;
    bink->Frames = r.frames;
    bink->FrameNum = r.frame_num;
    bink->LastFrameNum = r.last_frame_num;
    bink->FrameRate = r.frame_rate;
    bink->FrameRateDiv = r.frame_rate_div;
    bink->NumTracks = r.num_tracks;
    bink_log("playing %s (%ux%u, %u frames)", name, r.width, r.height, r.frames);
    return true;
}

int movie_index(const BINK* bink)
{
    if (bink == nullptr)
    {
        return -1;
    }
    const int index = (int)(bink - Movies);
    return (index >= 0 && index < Max_open_movies) ? index : -1;
}

// Chroma planes are half the size, rounded up (as video_player_play sizes the textures).
unsigned int chroma_size(unsigned int size)
{
    return size / 2 + size % 2;
}

void fill_plane(const BINKPLANE& plane, unsigned int height, unsigned char value)
{
    if (plane.Allocate != 0 && plane.Buffer != nullptr)
    {
        memset(plane.Buffer, value, (size_t)plane.BufferPitch * height);
    }
}
} // namespace

extern "C"
{
    // A black movie of one frame for any name, nullptr only once all 8 handles are open.
    BINK* __stdcall BinkOpen(const char* name, unsigned int flags)
    {
        for (int i = 0; i < Max_open_movies; i++)
        {
            if (_InterlockedCompareExchange(&Movie_in_use[i], 1, 0) == 0)
            {
                BINK* bink = &Movies[i];
                memset(bink, 0, sizeof(*bink));
                if (name != nullptr && helper_open(i, name, flags, bink))
                {
                    bink->OpenFlags = flags;
                    return bink;
                }
                bink->Width = Fake_width;
                bink->Height = Fake_height;
                bink->Frames = Fake_frames;
                // Bink numbers frames from 1; FrameNum is the frame the next BinkDoFrame decodes.
                bink->FrameNum = 1;
                bink->FrameRate = Fake_frame_rate;
                bink->FrameRateDiv = 1;
                bink->OpenFlags = flags;
                return bink;
            }
        }
        return nullptr;
    }

    void __stdcall BinkClose(BINK* bink)
    {
        const int index = movie_index(bink);
        if (index >= 0)
        {
            if (Helpers[index].proc != nullptr)
            {
                bink_ipc_msg m;
                bink_ipc_reply r;
                memset(&m, 0, sizeof(m));
                m.cmd = BINKIPC_CLOSE;
                helper_rpc(index, m, r);
                helper_release(Helpers[index]);
            }
            _InterlockedExchange(&Movie_in_use[index], 0);
        }
    }

    // 0: the frame was decoded (the planes were filled black when registered and never change).
    int __stdcall BinkDoFrame(BINK* bink)
    {
        const int index = movie_index(bink);
        if (index >= 0 && Helpers[index].proc != nullptr)
        {
            bink_ipc_msg m;
            bink_ipc_reply r;
            memset(&m, 0, sizeof(m));
            m.cmd = BINKIPC_DOFRAME;
            if (helper_rpc(index, m, r))
            {
                apply_reply(bink, r);
                helper_state& h = Helpers[index];
                if (h.registered && h.view != nullptr && r.cur_frame_set < h.total_sets)
                {
                    const unsigned int set = r.cur_frame_set;
                    if (h.game_y.Buffer != nullptr)
                    {
                        memcpy(h.game_y.Buffer, h.view + h.plane_off[set * 3 + 0], (size_t)h.game_y.BufferPitch * h.y_height);
                    }
                    if (h.game_cr.Buffer != nullptr)
                    {
                        memcpy(h.game_cr.Buffer, h.view + h.plane_off[set * 3 + 1], (size_t)h.game_cr.BufferPitch * h.c_height);
                    }
                    if (h.game_cb.Buffer != nullptr)
                    {
                        memcpy(h.game_cb.Buffer, h.view + h.plane_off[set * 3 + 2], (size_t)h.game_cb.BufferPitch * h.c_height);
                    }
                }
                return r.rc;
            }
            return 0;
        }
        if (bink != nullptr)
        {
            bink->LastFrameNum = bink->FrameNum;
        }
        return 0;
    }

    int __stdcall BinkDoFrameAsync(BINK* bink, unsigned int yplane_thread_index, unsigned int other_work_thread_index)
    {
        (void)yplane_thread_index;
        (void)other_work_thread_index;
        return BinkDoFrame(bink);
    }

    // 1: the asynchronous work is done, so a caller never waits on it.
    int __stdcall BinkDoFrameAsyncWait(BINK* bink, int us)
    {
        (void)bink;
        (void)us;
        return 1;
    }

    // Past the last frame the movie wraps to the first (a looping video).
    void __stdcall BinkNextFrame(BINK* bink)
    {
        const int index = movie_index(bink);
        if (index >= 0 && Helpers[index].proc != nullptr)
        {
            bink_ipc_msg m;
            bink_ipc_reply r;
            memset(&m, 0, sizeof(m));
            m.cmd = BINKIPC_NEXTFRAME;
            if (helper_rpc(index, m, r))
            {
                apply_reply(bink, r);
            }
            return;
        }
        if (bink != nullptr)
        {
            bink->FrameNum = bink->FrameNum >= bink->Frames ? 1 : bink->FrameNum + 1;
        }
    }

    // 0: no need to wait for the next frame.
    int __stdcall BinkWait(BINK* bink)
    {
        const int index = movie_index(bink);
        if (index >= 0 && Helpers[index].proc != nullptr)
        {
            bink_ipc_msg m;
            bink_ipc_reply r;
            memset(&m, 0, sizeof(m));
            m.cmd = BINKIPC_WAIT;
            if (helper_rpc(index, m, r))
            {
                return r.rc;
            }
        }
        return 0;
    }

    // 0: no frame to skip (the video player loops while this is non-zero).
    int __stdcall BinkShouldSkip(BINK* bink)
    {
        const int index = movie_index(bink);
        if (index >= 0 && Helpers[index].proc != nullptr)
        {
            bink_ipc_msg m;
            bink_ipc_reply r;
            memset(&m, 0, sizeof(m));
            m.cmd = BINKIPC_SHOULDSKIP;
            if (helper_rpc(index, m, r))
            {
                return r.rc;
            }
        }
        return 0;
    }

    int __stdcall BinkPause(BINK* bink, int pause)
    {
        const int index = movie_index(bink);
        if (index >= 0 && Helpers[index].proc != nullptr)
        {
            bink_ipc_msg m;
            bink_ipc_reply r;
            memset(&m, 0, sizeof(m));
            m.cmd = BINKIPC_PAUSE;
            m.a = pause;
            if (helper_rpc(index, m, r))
            {
                return r.rc;
            }
        }
        return pause;
    }

    void __stdcall BinkGoto(BINK* bink, unsigned int frame, int flags)
    {
        const int index = movie_index(bink);
        if (index >= 0 && Helpers[index].proc != nullptr)
        {
            bink_ipc_msg m;
            bink_ipc_reply r;
            memset(&m, 0, sizeof(m));
            m.cmd = BINKIPC_GOTO;
            m.a = (int)frame;
            m.b = flags;
            if (helper_rpc(index, m, r))
            {
                apply_reply(bink, r);
            }
            return;
        }
        if (bink != nullptr)
        {
            bink->FrameNum = (frame < 1 || frame > bink->Frames) ? 1 : frame;
        }
    }

    // Every frame is a key frame.
    unsigned int __stdcall BinkGetKeyFrame(BINK* bink, unsigned int frame, int flags)
    {
        const int index = movie_index(bink);
        if (index >= 0 && Helpers[index].proc != nullptr)
        {
            bink_ipc_msg m;
            bink_ipc_reply r;
            memset(&m, 0, sizeof(m));
            m.cmd = BINKIPC_GETKEYFRAME;
            m.a = (int)frame;
            m.b = flags;
            if (helper_rpc(index, m, r))
            {
                return (unsigned int)r.rc;
            }
        }
        if (bink == nullptr || frame < 1 || frame > bink->Frames)
        {
            return 1;
        }
        return frame;
    }

    // One set of Y, cR and cB planes (no alpha); the video player allocates them and sets their pitches.
    void __stdcall BinkGetFrameBuffersInfo(BINK* bink, BINKFRAMEBUFFERS* buffers)
    {
        if (buffers == nullptr)
        {
            return;
        }
        memset(buffers, 0, sizeof(*buffers));
        if (bink == nullptr)
        {
            return;
        }
        buffers->TotalFrames = 1;
        buffers->YABufferWidth = bink->Width;
        buffers->YABufferHeight = bink->Height;
        buffers->cRcBBufferWidth = chroma_size(bink->Width);
        buffers->cRcBBufferHeight = chroma_size(bink->Height);
        buffers->FrameNum = 0;
        buffers->Frames[0].YPlane.Allocate = 1;
        buffers->Frames[0].cRPlane.Allocate = 1;
        buffers->Frames[0].cBPlane.Allocate = 1;
    }

    // The planes are filled black once: the video player uploads the buffers it registered (frame set FrameNum 0) to
    // its textures, and nothing writes them again.
    void __stdcall BinkRegisterFrameBuffers(BINK* bink, BINKFRAMEBUFFERS* buffers)
    {
        if (buffers == nullptr)
        {
            return;
        }
        for (int i = 0; i < buffers->TotalFrames && i < 3; i++)
        {
            const BINKFRAMEPLANESET& frame = buffers->Frames[i];
            fill_plane(frame.YPlane, buffers->YABufferHeight, Black_y);
            fill_plane(frame.cRPlane, buffers->cRcBBufferHeight, Black_chroma);
            fill_plane(frame.cBPlane, buffers->cRcBBufferHeight, Black_chroma);
        }
        const int index = movie_index(bink);
        if (index >= 0 && Helpers[index].proc != nullptr && buffers->TotalFrames >= 1)
        {
            helper_state& h = Helpers[index];
            const BINKFRAMEPLANESET& frame = buffers->Frames[0];
            bink_ipc_msg m;
            bink_ipc_reply r;
            memset(&m, 0, sizeof(m));
            m.cmd = BINKIPC_REGISTER;
            m.a = (int)frame.YPlane.BufferPitch;
            m.b = (int)frame.cRPlane.BufferPitch;
            static volatile long Map_counter;
            _snprintf(m.path, sizeof(m.path), "Local\\OpenRFG_bink_%lu_%ld", GetCurrentProcessId(), _InterlockedIncrement(&Map_counter));
            if (helper_rpc(index, m, r) && r.rc == 0)
            {
                h.map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, m.path);
                if (h.map != nullptr)
                {
                    h.view = (unsigned char*)MapViewOfFile(h.map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
                }
                if (h.view != nullptr)
                {
                    memcpy(h.plane_off, r.plane_offset, sizeof(h.plane_off));
                    h.total_sets = r.total_sets;
                    h.game_y = frame.YPlane;
                    h.game_cr = frame.cRPlane;
                    h.game_cb = frame.cBPlane;
                    h.y_height = buffers->YABufferHeight;
                    h.c_height = buffers->cRcBBufferHeight;
                    h.registered = true;
                }
                else
                {
                    bink_log("shared memory %s failed (%lu)", m.path, GetLastError());
                }
            }
            else
            {
                bink_log("helper register failed");
            }
        }
    }

    void __stdcall BinkGetSummary(BINK* bink, BINKSUMMARY* summary)
    {
        if (summary == nullptr)
        {
            return;
        }
        memset(summary, 0, sizeof(*summary));
        if (bink == nullptr)
        {
            return;
        }
        summary->Width = bink->Width;
        summary->Height = bink->Height;
        summary->FileFrameRate = bink->FrameRate;
        summary->FileFrameRateDiv = bink->FrameRateDiv;
        summary->FrameRate = bink->FrameRate;
        summary->FrameRateDiv = bink->FrameRateDiv;
        summary->TotalFrames = bink->Frames;
    }

    // The read buffer is reported full (see Fake_read_buffer_size).
    void __stdcall BinkGetRealtime(BINK* bink, BINKREALTIME* realtime, unsigned int frames)
    {
        (void)frames;
        if (realtime == nullptr)
        {
            return;
        }
        memset(realtime, 0, sizeof(*realtime));
        realtime->ReadBufferSize = Fake_read_buffer_size;
        realtime->ReadBufferUsed = Fake_read_buffer_size;
        if (bink != nullptr)
        {
            realtime->FrameNum = bink->FrameNum;
            realtime->FrameRate = bink->FrameRate;
            realtime->FrameRateDiv = bink->FrameRateDiv;
        }
    }

    unsigned int __stdcall BinkControlBackgroundIO(BINK* bink, unsigned int control)
    {
        (void)bink;
        (void)control;
        return 0;
    }

    // The movie has no sound.
    void __stdcall BinkSetVolume(BINK* bink, unsigned int track, int volume)
    {
        const int index = movie_index(bink);
        if (index >= 0 && Helpers[index].proc != nullptr)
        {
            bink_ipc_msg m;
            bink_ipc_reply r;
            memset(&m, 0, sizeof(m));
            m.cmd = BINKIPC_SETVOLUME;
            m.a = (int)track;
            m.b = volume;
            helper_rpc(index, m, r);
        }
    }

    void __stdcall BinkSetIOSize(unsigned long long size)
    {
        Pending_io_size = size;
    }

    void __stdcall BinkSetSoundTrack(unsigned int total_tracks, const unsigned int* tracks)
    {
        Pending_num_tracks = (tracks == nullptr || total_tracks > 8) ? 0 : total_tracks;
        for (unsigned int i = 0; i < Pending_num_tracks; i++)
        {
            Pending_tracks[i] = tracks[i];
        }
    }

    // 0: no sound system installed (the video player ignores the result).
    int __stdcall BinkSetSoundSystem(void* open, unsigned int param)
    {
        (void)open;
        (void)param;
        return 0;
    }

    // Only passed to BinkSetSoundSystem, never called here.
    void* __stdcall BinkOpenDirectSound(unsigned int param)
    {
        (void)param;
        return nullptr;
    }

    // 1: no asynchronous thread runs, so stopping one succeeds at once.
    int __stdcall BinkRequestStopAsyncThread(unsigned int thread_index)
    {
        (void)thread_index;
        return 1;
    }

    int __stdcall BinkWaitStopAsyncThread(unsigned int thread_index)
    {
        (void)thread_index;
        return 1;
    }
}
