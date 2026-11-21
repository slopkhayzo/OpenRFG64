// Shared by bink_helper32.exe (32-bit) and the 64-bit stand-in src/stubs/thirdparty/bink.cpp: the messages the stand-in
// sends over the helper's stdin pipe and the answers read back from its stdout pipe. Both sides use fixed sizes with only
// 32-bit fields, so the layout is identical in 32 and 64 bit.
#pragma once

enum bink_ipc_cmd : unsigned int
{
    BINKIPC_OPEN = 1, // path, a = open flags, b = io size (low), c = io size (high), tracks/num_tracks
    BINKIPC_CLOSE = 2,
    BINKIPC_DOFRAME = 3,
    BINKIPC_NEXTFRAME = 4,
    BINKIPC_WAIT = 5,
    BINKIPC_SHOULDSKIP = 6,
    BINKIPC_PAUSE = 7, // a = pause
    BINKIPC_GOTO = 8, // a = frame, b = flags
    BINKIPC_GETKEYFRAME = 9, // a = frame, b = flags
    BINKIPC_SETVOLUME = 10, // a = track, b = volume
    BINKIPC_REGISTER = 11, // a = Y pitch, b = chroma pitch; creates the shared memory named in path
};

struct bink_ipc_msg
{
    unsigned int cmd;
    int a;
    int b;
    int c;
    unsigned int num_tracks;
    unsigned int tracks[8];
    char path[520];
};

struct bink_ipc_reply
{
    int rc; // the function's result (OPEN: 0 = opened, non-zero = failed)
    unsigned int width;
    unsigned int height;
    unsigned int frames;
    unsigned int frame_num;
    unsigned int last_frame_num;
    unsigned int frame_rate;
    unsigned int frame_rate_div;
    int num_tracks;
    unsigned int cur_frame_set; // after DOFRAME: which registered frame set holds the decoded picture
    unsigned int shared_size; // after REGISTER: bytes of the shared memory
    unsigned int plane_offset[9]; // after REGISTER: shared memory offset of [set * 3 + (0 Y, 1 cR, 2 cB)]
    unsigned int total_sets;
    unsigned int pad;
};
