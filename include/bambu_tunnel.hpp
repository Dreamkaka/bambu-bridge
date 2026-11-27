#ifndef BAMBU_BRIDGE_BAMBU_TUNNEL_HPP
#define BAMBU_BRIDGE_BAMBU_TUNNEL_HPP

#ifdef _WIN32
#  ifdef __cplusplus
typedef wchar_t bambu_tchar;
#  else
typedef unsigned short bambu_tchar;
#  endif
#else
typedef char bambu_tchar;
#endif

typedef void* Bambu_Tunnel;
typedef void (*Bambu_Logger)(void* context, int level, bambu_tchar const* msg);

enum Bambu_StreamType { VIDE = 0, AUDI = 1 };
enum Bambu_VideoSubType { AVC1 = 0, MJPG = 1 };
enum Bambu_Error {
    Bambu_success = 0,
    Bambu_stream_end = 1,
    Bambu_would_block = 2,
    Bambu_buffer_limit = 3
};

struct Bambu_StreamInfo {
    Bambu_StreamType type;
    int sub_type;
    union {
        struct { int width; int height; int frame_rate; } video;
        struct { int sample_rate; int channel_count; int sample_size; } audio;
    } format;
    int format_type;
    int format_size;
    int max_frame_size;
    unsigned char const* format_buffer;
};

struct Bambu_Sample {
    int itrack;
    int size;
    int flags;
    unsigned char const* buffer;
    unsigned long long decode_time;
};

typedef int (*fn_Bambu_Create)(Bambu_Tunnel* tunnel, char const* path);
typedef void (*fn_Bambu_SetLogger)(Bambu_Tunnel tunnel, Bambu_Logger logger, void* context);
typedef int (*fn_Bambu_Open)(Bambu_Tunnel tunnel);
typedef int (*fn_Bambu_StartStream)(Bambu_Tunnel tunnel, bool video);
typedef int (*fn_Bambu_StartStreamEx)(Bambu_Tunnel tunnel, int type);
typedef int (*fn_Bambu_GetStreamCount)(Bambu_Tunnel tunnel);
typedef int (*fn_Bambu_GetStreamInfo)(Bambu_Tunnel tunnel, int index, Bambu_StreamInfo* info);
typedef int (*fn_Bambu_ReadSample)(Bambu_Tunnel tunnel, Bambu_Sample* sample);
typedef int (*fn_Bambu_SendMessage)(Bambu_Tunnel tunnel, int type, const char* data, int len);
typedef void (*fn_Bambu_Close)(Bambu_Tunnel tunnel);
typedef void (*fn_Bambu_Destroy)(Bambu_Tunnel tunnel);
typedef int (*fn_Bambu_Init)();
typedef void (*fn_Bambu_Deinit)();
typedef char const* (*fn_Bambu_GetLastErrorMsg)();
typedef void (*fn_Bambu_FreeLogMsg)(bambu_tchar const* msg);

#endif
