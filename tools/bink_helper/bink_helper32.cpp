// OpenRFG 64-bit Bink helper (32-bit program). The game's binkw32.dll is 32-bit and cannot load into the 64-bit game, so
// the game starts this program, which loads the DLL, plays the movie (sound through Bink's own DirectSound output) and
// shares the decoded YUV planes through shared memory. Talks to the game over stdin/stdout (bink_ipc.h).
// Usage: bink_helper32.exe <folder holding binkw32.dll>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string.h>
#include <stdio.h>

#define OPENRFG_SIZE_ASSERT(c, m)
#include "../../src/library/modules/thirdparty/bink/bink_1_300_7_0/include/bink.h"
#include "bink_ipc.h"

static HANDLE In_pipe;
static HANDLE Out_pipe;

static bool read_all(void* data, DWORD size)
{
    char* p = (char*)data;
    while (size > 0)
    {
        DWORD got = 0;
        if (!ReadFile(In_pipe, p, size, &got, nullptr) || got == 0)
        {
            return false;
        }
        p += got;
        size -= got;
    }
    return true;
}

static bool write_all(const void* data, DWORD size)
{
    const char* p = (const char*)data;
    while (size > 0)
    {
        DWORD put = 0;
        if (!WriteFile(Out_pipe, p, size, &put, nullptr) || put == 0)
        {
            return false;
        }
        p += put;
        size -= put;
    }
    return true;
}

static BINK* Movie;
static BINKFRAMEBUFFERS Buffers; // registered with Bink, so it must stay valid while the movie is open
static HANDLE Shared_map;
static unsigned char* Shared_view;

static void fill_status(bink_ipc_reply& r)
{
    if (Movie != nullptr)
    {
        r.width = Movie->Width;
        r.height = Movie->Height;
        r.frames = Movie->Frames;
        r.frame_num = Movie->FrameNum;
        r.last_frame_num = Movie->LastFrameNum;
        r.frame_rate = Movie->FrameRate;
        r.frame_rate_div = Movie->FrameRateDiv;
        r.num_tracks = Movie->NumTracks;
    }
    r.cur_frame_set = Buffers.FrameNum;
}

int main(int argc, char** argv)
{
    In_pipe = GetStdHandle(STD_INPUT_HANDLE);
    Out_pipe = GetStdHandle(STD_OUTPUT_HANDLE);
    bink_ipc_reply ready;
    memset(&ready, 0, sizeof(ready));
    // Load binkw32.dll from the folder given (the delay-loaded import then finds it already loaded).
    if (argc < 2)
    {
        ready.rc = 1;
    }
    else
    {
        char dll[600];
        _snprintf(dll, sizeof(dll), "%s\\binkw32.dll", argv[1]);
        dll[sizeof(dll) - 1] = 0;
        if (LoadLibraryA(dll) == nullptr)
        {
            ready.rc = 2;
        }
    }
    write_all(&ready, sizeof(ready));
    if (ready.rc != 0)
    {
        return 1;
    }

    for (;;)
    {
        bink_ipc_msg m;
        if (!read_all(&m, sizeof(m)))
        {
            break; // the game closed the pipe (or died)
        }
        bink_ipc_reply r;
        memset(&r, 0, sizeof(r));
        bool quit = false;
        switch (m.cmd)
        {
        case BINKIPC_OPEN:
        {
            m.path[sizeof(m.path) - 1] = 0;
            BinkSetSoundSystem((void*)BinkOpenDirectSound, 0);
            if (m.num_tracks > 0)
            {
                BinkSetSoundTrack(m.num_tracks > 8 ? 8 : m.num_tracks, m.tracks);
            }
            if (((unsigned int)m.a & 0x1000000u) != 0)
            {
                BinkSetIOSize(((unsigned long long)(unsigned int)m.c << 32) | (unsigned long long)(unsigned int)m.b);
            }
            Movie = BinkOpen(m.path, (unsigned int)m.a);
            r.rc = Movie != nullptr ? 0 : 1;
            break;
        }
        case BINKIPC_REGISTER:
        {
            if (Movie == nullptr)
            {
                r.rc = 1;
                break;
            }
            memset(&Buffers, 0, sizeof(Buffers));
            BinkGetFrameBuffersInfo(Movie, &Buffers);
            const unsigned int ya_pitch = (unsigned int)m.a;
            const unsigned int c_pitch = (unsigned int)m.b;
            unsigned int total = 0;
            unsigned int offsets[3][5]; // Y, cR, cB, A, H offsets
            for (int i = 0; i < Buffers.TotalFrames && i < 3; i++)
            {
                BINKFRAMEPLANESET& f = Buffers.Frames[i];
                BINKPLANE* planes[5] = { &f.YPlane, &f.cRPlane, &f.cBPlane, &f.APlane, &f.HPlane };
                for (int p = 0; p < 5; p++)
                {
                    offsets[i][p] = 0;
                    if (planes[p]->Allocate == 0)
                    {
                        continue;
                    }
                    const unsigned int pitch = (p == 1 || p == 2) ? c_pitch : ya_pitch;
                    const unsigned int height = (p == 1 || p == 2) ? Buffers.cRcBBufferHeight : Buffers.YABufferHeight;
                    planes[p]->BufferPitch = pitch;
                    offsets[i][p] = total;
                    total += pitch * height;
                    total = (total + 15u) & ~15u;
                }
            }
            if (Shared_view != nullptr)
            {
                UnmapViewOfFile(Shared_view);
                Shared_view = nullptr;
            }
            if (Shared_map != nullptr)
            {
                CloseHandle(Shared_map);
                Shared_map = nullptr;
            }
            m.path[sizeof(m.path) - 1] = 0;
            Shared_map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, total + 16, m.path);
            if (Shared_map == nullptr)
            {
                r.rc = 2;
                break;
            }
            Shared_view = (unsigned char*)MapViewOfFile(Shared_map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
            if (Shared_view == nullptr)
            {
                r.rc = 3;
                break;
            }
            for (int i = 0; i < Buffers.TotalFrames && i < 3; i++)
            {
                BINKFRAMEPLANESET& f = Buffers.Frames[i];
                BINKPLANE* planes[5] = { &f.YPlane, &f.cRPlane, &f.cBPlane, &f.APlane, &f.HPlane };
                for (int p = 0; p < 5; p++)
                {
                    if (planes[p]->Allocate != 0)
                    {
                        planes[p]->Buffer = Shared_view + offsets[i][p];
                    }
                }
                r.plane_offset[i * 3 + 0] = offsets[i][0];
                r.plane_offset[i * 3 + 1] = offsets[i][1];
                r.plane_offset[i * 3 + 2] = offsets[i][2];
            }
            BinkRegisterFrameBuffers(Movie, &Buffers);
            r.shared_size = total + 16;
            r.total_sets = (unsigned int)Buffers.TotalFrames;
            break;
        }
        case BINKIPC_DOFRAME:
            r.rc = Movie != nullptr ? BinkDoFrame(Movie) : 0;
            break;
        case BINKIPC_NEXTFRAME:
            if (Movie != nullptr)
            {
                BinkNextFrame(Movie);
            }
            break;
        case BINKIPC_WAIT:
            r.rc = Movie != nullptr ? BinkWait(Movie) : 0;
            break;
        case BINKIPC_SHOULDSKIP:
            r.rc = Movie != nullptr ? BinkShouldSkip(Movie) : 0;
            break;
        case BINKIPC_PAUSE:
            r.rc = Movie != nullptr ? BinkPause(Movie, m.a) : 0;
            break;
        case BINKIPC_GOTO:
            if (Movie != nullptr)
            {
                BinkGoto(Movie, (unsigned int)m.a, m.b);
            }
            break;
        case BINKIPC_GETKEYFRAME:
            r.rc = Movie != nullptr ? (int)BinkGetKeyFrame(Movie, (unsigned int)m.a, m.b) : 1;
            break;
        case BINKIPC_SETVOLUME:
            if (Movie != nullptr)
            {
                BinkSetVolume(Movie, (unsigned int)m.a, m.b);
            }
            break;
        case BINKIPC_CLOSE:
            if (Movie != nullptr)
            {
                BinkClose(Movie);
                Movie = nullptr;
            }
            quit = true;
            break;
        default:
            r.rc = -1;
            break;
        }
        fill_status(r);
        if (!write_all(&r, sizeof(r)) || quit)
        {
            break;
        }
    }
    if (Movie != nullptr)
    {
        BinkClose(Movie);
    }
    return 0;
}
