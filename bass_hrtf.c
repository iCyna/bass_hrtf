// ============================================================================
// BASS_HRTF Addon - Standalone & Zero-Dependency Wrapper (BASS + Steam Audio)
// ============================================================================

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#ifdef _WIN32
    #include <windows.h>
    #define EXPORT __declspec(dllexport)
    #define B_CALLBACK __stdcall
    #define P_CALL __stdcall
    #define LOAD_LIB(name) LoadLibraryA(name)
    #define GET_SYM(lib, sym) GetProcAddress((HMODULE)(lib), sym)
    #define CLOSE_LIB(lib) FreeLibrary((HMODULE)(lib))
    #define BASS_LIB_NAME "bass.dll"
    #define PHONON_LIB_NAME "phonon.dll"
#else
    #include <dlfcn.h>
    #define EXPORT __attribute__((visibility("default")))
    #define B_CALLBACK
    #define P_CALL
    #define LOAD_LIB(name) dlopen(name, RTLD_NOW | RTLD_GLOBAL)
    #define GET_SYM(lib, sym) dlsym(lib, sym)
    #define CLOSE_LIB(lib) dlclose(lib)
    #define BASS_LIB_NAME "libbass.so"
    #define PHONON_LIB_NAME "libphonon.so"
#endif

// ============================================================================
// 1. ĐỊNH NGHĨA CẤU TRÚC & HẰNG SỐ CỦA BASS (Thay thế bass.h)
// ============================================================================
typedef uint32_t DWORD;
typedef uint64_t QWORD;
typedef int BOOL;
typedef DWORD HSTREAM;
typedef DWORD HSYNC;

#ifndef TRUE
    #define TRUE 1
    #define FALSE 0
#endif

#define BASS_SAMPLE_FLOAT       0x100
#define BASS_STREAM_DECODE      0x200000
#define BASS_STREAMPROC_END     0x80000000
#define BASS_SYNC_FREE          2

typedef struct {
    DWORD freq;
    DWORD chans;
    DWORD flags;
    DWORD ctype;
    DWORD origres;
    DWORD plugin;
    DWORD sample;
    const char *filename;
} BASS_CHANNELINFO;

typedef DWORD (B_CALLBACK *STREAMPROC)(HSTREAM handle, void *buffer, DWORD length, void *user);
typedef void (B_CALLBACK *SYNCPROC)(HSYNC handle, DWORD channel, DWORD data, void *user);

// Con trỏ hàm gọi thẳng vào bass.dll đang có của bạn
static BOOL (B_CALLBACK *fn_BASS_ChannelGetInfo)(DWORD handle, BASS_CHANNELINFO *info) = NULL;
static HSTREAM (B_CALLBACK *fn_BASS_StreamCreate)(DWORD freq, DWORD chans, DWORD flags, STREAMPROC proc, void *user) = NULL;
static DWORD (B_CALLBACK *fn_BASS_ChannelGetData)(DWORD handle, void *buffer, DWORD length) = NULL;
static HSYNC (B_CALLBACK *fn_BASS_ChannelSetSync)(DWORD handle, DWORD type, QWORD param, SYNCPROC proc, void *user) = NULL;

// ============================================================================
// 2. ĐỊNH NGHĨA CẤU TRÚC & HẰNG SỐ CỦA STEAM AUDIO (Thay thế phonon.h)
// ============================================================================
// Mặc định tương thích chuẩn Steam Audio SDK 4.5.x (0x00040500)
// Bạn có thể truyền version bất kỳ lúc gọi BASS_HRTF_InitEx nếu dùng bản khác
#define DEFAULT_STEAMAUDIO_VERSION 0x00040500
#define HRTF_FRAME_SIZE 512

typedef void* IPLContext;
typedef void* IPLHRTF;
typedef void* IPLBinauralEffect;

typedef enum {
    IPL_STATUS_SUCCESS = 0
} IPLerror;

typedef enum {
    IPL_SIMDLEVEL_SSE2 = 0,
    IPL_SIMDLEVEL_SSE4 = 1,
    IPL_SIMDLEVEL_AVX  = 2,
    IPL_SIMDLEVEL_AVX2 = 3
} IPLSIMDLevel;

typedef struct { float x, y, z; } IPLVector3;

typedef struct {
    uint32_t version;
    void* logCallback;
    void* allocateCallback;
    void* freeCallback;
    IPLSIMDLevel simdLevel;
    uint32_t flags;
} IPLContextSettings;

typedef struct {
    int32_t samplingRate;
    int32_t frameSize;
} IPLAudioSettings;

typedef struct {
    int32_t type;           // 0 = IPL_HRTFTYPE_DEFAULT, 1 = IPL_HRTFTYPE_SOFA
    const char* sofaFileName;
    const uint8_t* sofaData;
    int32_t sofaDataSize;
    float volume;
    int32_t normType;       // 0 = IPL_HRTFNORMTYPE_NONE
} IPLHRTFSettings;

typedef struct {
    IPLHRTF hrtf;
} IPLBinauralEffectSettings;

typedef struct {
    IPLVector3 direction;
    int32_t interpolation;  // 0 = NEAREST, 1 = BILINEAR
    float spatialBlend;
    IPLHRTF hrtf;
    float* peakDelays;
} IPLBinauralEffectParams;

typedef struct {
    int32_t numChannels;
    int32_t numSamples;
    float** data;
} IPLAudioBuffer;

// Con trỏ hàm gọi vào phonon.dll
static IPLerror (P_CALL *fn_iplContextCreate)(IPLContextSettings*, IPLContext*) = NULL;
static void (P_CALL *fn_iplContextRelease)(IPLContext*) = NULL;
static IPLerror (P_CALL *fn_iplHRTFCreate)(IPLContext, IPLAudioSettings*, IPLHRTFSettings*, IPLHRTF*) = NULL;
static void (P_CALL *fn_iplHRTFRelease)(IPLHRTF*) = NULL;
static IPLerror (P_CALL *fn_iplBinauralEffectCreate)(IPLContext, IPLAudioSettings*, IPLBinauralEffectSettings*, IPLBinauralEffect*) = NULL;
static void (P_CALL *fn_iplBinauralEffectRelease)(IPLBinauralEffect*) = NULL;
static void (P_CALL *fn_iplBinauralEffectReset)(IPLBinauralEffect) = NULL;
static int32_t (P_CALL *fn_iplBinauralEffectApply)(IPLBinauralEffect, IPLBinauralEffectParams*, IPLAudioBuffer*, IPLAudioBuffer*) = NULL;

// ============================================================================
// 3. QUẢN LÝ TRẠNG THÁI TOÀN CỤC & CẤU TRÚC KÊNH HRTF
// ============================================================================
static void* g_BassLib = NULL;
static void* g_PhononLib = NULL;
static IPLContext g_Context = NULL;
static IPLHRTF g_HRTF = NULL;
static int g_SampleRate = 44100;

// Spinlock siêu nhẹ bảo vệ bảng kênh khi thêm/xóa (Thread-safe 100%)
static volatile long g_TableLock = 0;
static void LockTable(void) {
#ifdef _WIN32
    while (InterlockedExchange(&g_TableLock, 1) == 1) { Sleep(0); }
#else
    while (__sync_lock_test_and_set(&g_TableLock, 1)) {}
#endif
}
static void UnlockTable(void) {
#ifdef _WIN32
    InterlockedExchange(&g_TableLock, 0);
#else
    __sync_lock_release(&g_TableLock);
#endif
}

typedef struct {
    DWORD sourceChan;
    IPLBinauralEffect effect;
    
    // Thông số 3D cập nhật Lock-free từ luồng Game/Python
    volatile float dirX;
    volatile float dirY;
    volatile float dirZ;
    volatile float distanceAtten;
    volatile float spatialBlend; // 0.0 (2D) -> 1.0 (Full 3D HRTF)
    volatile int interpolation;  // 0: Nearest, 1: Bilinear
    volatile int needReset;      // Cờ báo xóa bộ đệm khi tua nhạc

    // Bộ đệm tĩnh (Zero-Allocation trong quá trình phát)
    float inMonoFrame[HRTF_FRAME_SIZE];
    float outLeftFrame[HRTF_FRAME_SIZE];
    float outRightFrame[HRTF_FRAME_SIZE];
    float* inPtrs[1];
    float* outPtrs[2];
    IPLAudioBuffer inBuf;
    IPLAudioBuffer outBuf;

    // Ring Buffer (FIFO) nối nhịp giữa BASS và Steam Audio
    float ringBuffer[HRTF_FRAME_SIZE * 2 * 4];
    int ringCount;
} BASS_HRTF_Node;

#define MAX_HRTF_CHANNELS 1024
static BASS_HRTF_Node* g_Nodes[MAX_HRTF_CHANNELS] = {0};
static HSTREAM g_Streams[MAX_HRTF_CHANNELS] = {0};

// ============================================================================
// 4. HÀM NẠP ĐỘNG DLL/SO (Tự tìm trong thư mục hiện tại)
// ============================================================================
static BOOL LoadAllSymbols(const char* customBassPath, const char* customPhononPath) {
    if (!g_BassLib) {
        g_BassLib = LOAD_LIB(customBassPath ? customBassPath : BASS_LIB_NAME);
        if (!g_BassLib) return FALSE;
    }
    if (!g_PhononLib) {
        g_PhononLib = LOAD_LIB(customPhononPath ? customPhononPath : PHONON_LIB_NAME);
        if (!g_PhononLib) return FALSE;
    }

    fn_BASS_ChannelGetInfo = GET_SYM(g_BassLib, "BASS_ChannelGetInfo");
    fn_BASS_StreamCreate   = GET_SYM(g_BassLib, "BASS_StreamCreate");
    fn_BASS_ChannelGetData = GET_SYM(g_BassLib, "BASS_ChannelGetData");
    fn_BASS_ChannelSetSync = GET_SYM(g_BassLib, "BASS_ChannelSetSync");

    fn_iplContextCreate         = GET_SYM(g_PhononLib, "iplContextCreate");
    fn_iplContextRelease        = GET_SYM(g_PhononLib, "iplContextRelease");
    fn_iplHRTFCreate            = GET_SYM(g_PhononLib, "iplHRTFCreate");
    fn_iplHRTFRelease           = GET_SYM(g_PhononLib, "iplHRTFRelease");
    fn_iplBinauralEffectCreate  = GET_SYM(g_PhononLib, "iplBinauralEffectCreate");
    fn_iplBinauralEffectRelease = GET_SYM(g_PhononLib, "iplBinauralEffectRelease");
    fn_iplBinauralEffectReset   = GET_SYM(g_PhononLib, "iplBinauralEffectReset");
    fn_iplBinauralEffectApply   = GET_SYM(g_PhononLib, "iplBinauralEffectApply");

    if (!fn_BASS_ChannelGetInfo || !fn_BASS_StreamCreate || !fn_BASS_ChannelGetData || !fn_BASS_ChannelSetSync ||
        !fn_iplContextCreate || !fn_iplHRTFCreate || !fn_iplBinauralEffectCreate || !fn_iplBinauralEffectApply) {
        return FALSE;
    }
    return TRUE;
}

// ============================================================================
// 5. CALLBACK STREAMPROC (Xử lý thời gian thực tốc độ C thuần)
// ============================================================================
static DWORD B_CALLBACK HRTF_StreamProc(HSTREAM handle, void *buffer, DWORD length, void *user) {
    BASS_HRTF_Node *node = (BASS_HRTF_Node*)user;
    float *outStream = (float*)buffer;
    int floatsNeeded = length / sizeof(float);
    int floatsWritten = 0;
    BOOL sourceEnded = FALSE;

    // Nếu người dùng gọi Reset (khi tua file âm thanh gốc)
    if (node->needReset) {
        node->ringCount = 0;
        if (fn_iplBinauralEffectReset) fn_iplBinauralEffectReset(node->effect);
        node->needReset = 0;
    }

    while (floatsWritten < floatsNeeded) {
        // 1. Rút dữ liệu Stereo có sẵn từ Ring Buffer trả cho BASS
        if (node->ringCount > 0) {
            int toCopy = floatsNeeded - floatsWritten;
            if (toCopy > node->ringCount) toCopy = node->ringCount;

            memcpy(outStream + floatsWritten, node->ringBuffer, toCopy * sizeof(float));
            floatsWritten += toCopy;
            node->ringCount -= toCopy;

            if (node->ringCount > 0) {
                memmove(node->ringBuffer, node->ringBuffer + toCopy, node->ringCount * sizeof(float));
            }
            continue;
        }

        if (sourceEnded) break;

        // 2. Kéo đúng 512 mẫu Mono Float từ kênh nguồn BASS
        DWORD bytesRead = fn_BASS_ChannelGetData(node->sourceChan, node->inMonoFrame, HRTF_FRAME_SIZE * sizeof(float));
        if (bytesRead == (DWORD)-1 || bytesRead == 0) {
            sourceEnded = TRUE;
            break;
        }

        int samplesRead = bytesRead / sizeof(float);
        if (samplesRead < HRTF_FRAME_SIZE) {
            memset(node->inMonoFrame + samplesRead, 0, (HRTF_FRAME_SIZE - samplesRead) * sizeof(float));
            sourceEnded = TRUE;
        }

        // 3. Chạy bộ lọc HRTF của Steam Audio
        IPLBinauralEffectParams params;
        memset(&params, 0, sizeof(params));
        params.direction.x   = node->dirX;
        params.direction.y   = node->dirY;
        params.direction.z   = node->dirZ;
        params.interpolation = node->interpolation ? 1 : 0;
        params.spatialBlend  = node->spatialBlend;
        params.hrtf          = g_HRTF;
        params.peakDelays    = NULL;

        fn_iplBinauralEffectApply(node->effect, &params, &node->inBuf, &node->outBuf);

        // 4. Trộn kênh Left/Right thành Interleaved Stereo + Nhân âm lượng khoảng cách
        float atten = node->distanceAtten;
        int idx = 0;
        for (int i = 0; i < HRTF_FRAME_SIZE; ++i) {
            node->ringBuffer[idx++] = node->outLeftFrame[i] * atten;
            node->ringBuffer[idx++] = node->outRightFrame[i] * atten;
        }
        node->ringCount = HRTF_FRAME_SIZE * 2;
    }

    DWORD bytesTotal = floatsWritten * sizeof(float);
    if (sourceEnded && node->ringCount == 0) {
        return bytesTotal | BASS_STREAMPROC_END;
    }
    return bytesTotal;
}

// Tự động giải phóng node khi kênh BASS bị đóng (BASS_StreamFree)
static void B_CALLBACK HRTF_FreeSync(HSYNC handle, DWORD channel, DWORD data, void *user) {
    BASS_HRTF_Node *node = (BASS_HRTF_Node*)user;
    LockTable();
    for (int i = 0; i < MAX_HRTF_CHANNELS; ++i) {
        if (g_Nodes[i] == node) {
            g_Streams[i] = 0;
            g_Nodes[i] = NULL;
            break;
        }
    }
    UnlockTable();

    if (node) {
        if (node->effect && fn_iplBinauralEffectRelease) {
            fn_iplBinauralEffectRelease(&node->effect);
        }
        free(node);
    }
}

// ============================================================================
// 6. CÁC HÀM API PUBLIC (GỌI TỪ PYTHON / C++)
// ============================================================================

// Khởi tạo nâng cao (Cho phép chỉ định đường dẫn DLL/SO và file .SOFA tùy chỉnh)
EXPORT BOOL B_CALLBACK BASS_HRTF_InitEx(int sampleRate, const char* bassPath, const char* phononPath, const char* sofaPath, uint32_t steamAudioVersion) {
    if (g_Context != NULL) return TRUE;
    if (!LoadAllSymbols(bassPath, phononPath)) return FALSE;

    g_SampleRate = (sampleRate > 0) ? sampleRate : 44100;

    IPLContextSettings ctxSettings;
    memset(&ctxSettings, 0, sizeof(ctxSettings));
    ctxSettings.version = (steamAudioVersion > 0) ? steamAudioVersion : DEFAULT_STEAMAUDIO_VERSION;
    ctxSettings.simdLevel = IPL_SIMDLEVEL_AVX2;

    // Nếu người dùng truyền sai version của phonon.dll, tự động dò các version Steam Audio 4.x phổ biến
    if (fn_iplContextCreate(&ctxSettings, &g_Context) != IPL_STATUS_SUCCESS) {
        uint32_t fallbacks[] = { 0x00040503, 0x00040502, 0x00040501, 0x00040500, 0x00040400, 0x00040300, 0x00040200, 0x00040100, 0x00040000 };
        BOOL ok = FALSE;
        for (int i = 0; i < 9; ++i) {
            ctxSettings.version = fallbacks[i];
            if (fn_iplContextCreate(&ctxSettings, &g_Context) == IPL_STATUS_SUCCESS) {
                ok = TRUE;
                break;
            }
        }
        if (!ok) return FALSE;
    }

    IPLAudioSettings audioSettings = { g_SampleRate, HRTF_FRAME_SIZE };
    IPLHRTFSettings hrtfSettings;
    memset(&hrtfSettings, 0, sizeof(hrtfSettings));
    if (sofaPath && sofaPath[0] != '\0') {
        hrtfSettings.type = 1; // IPL_HRTFTYPE_SOFA
        hrtfSettings.sofaFileName = sofaPath;
    } else {
        hrtfSettings.type = 0; // IPL_HRTFTYPE_DEFAULT
    }
    hrtfSettings.volume = 1.0f;
    hrtfSettings.normType = 0;

    if (fn_iplHRTFCreate(g_Context, &audioSettings, &hrtfSettings, &g_HRTF) != IPL_STATUS_SUCCESS) {
        fn_iplContextRelease(&g_Context);
        g_Context = NULL;
        return FALSE;
    }
    return TRUE;
}

// Khởi tạo nhanh mặc định (Tự tìm bass.dll và phonon.dll cùng thư mục)
EXPORT BOOL B_CALLBACK BASS_HRTF_Init(int sampleRate) {
    return BASS_HRTF_InitEx(sampleRate, NULL, NULL, NULL, 0);
}

// Giải phóng toàn bộ hệ thống HRTF
EXPORT void B_CALLBACK BASS_HRTF_Free(void) {
    if (g_HRTF && fn_iplHRTFRelease) { fn_iplHRTFRelease(&g_HRTF); g_HRTF = NULL; }
    if (g_Context && fn_iplContextRelease) { fn_iplContextRelease(&g_Context); g_Context = NULL; }
}

// Bọc kênh Mono Decode của BASS thành kênh Stereo 3D HRTF
EXPORT HSTREAM B_CALLBACK BASS_HRTF_StreamCreate(DWORD sourceChan, DWORD flags) {
    if (!g_Context || !g_HRTF) return 0;

    BASS_CHANNELINFO info;
    if (!fn_BASS_ChannelGetInfo(sourceChan, &info)) return 0;

    // Bắt buộc nguồn phải là 1 kênh (Mono), 32-bit Float và bật cờ Decode
    if (info.chans != 1 || !(info.flags & BASS_SAMPLE_FLOAT) || !(info.flags & BASS_STREAM_DECODE)) {
        return 0;
    }

    BASS_HRTF_Node *node = (BASS_HRTF_Node*)calloc(1, sizeof(BASS_HRTF_Node));
    if (!node) return 0;

    node->sourceChan    = sourceChan;
    node->dirX          = 0.0f;
    node->dirY          = 0.0f;
    node->dirZ          = -1.0f; // Trước mặt
    node->distanceAtten = 1.0f;
    node->spatialBlend  = 1.0f;
    node->interpolation = 0;

    IPLAudioSettings audioSettings = { (int32_t)info.freq, HRTF_FRAME_SIZE };
    IPLBinauralEffectSettings effectSettings = { g_HRTF };

    if (fn_iplBinauralEffectCreate(g_Context, &audioSettings, &effectSettings, &node->effect) != IPL_STATUS_SUCCESS) {
        free(node);
        return 0;
    }

    node->inPtrs[0]  = node->inMonoFrame;
    node->inBuf.numChannels = 1;
    node->inBuf.numSamples  = HRTF_FRAME_SIZE;
    node->inBuf.data        = node->inPtrs;

    node->outPtrs[0] = node->outLeftFrame;
    node->outPtrs[1] = node->outRightFrame;
    node->outBuf.numChannels = 2;
    node->outBuf.numSamples  = HRTF_FRAME_SIZE;
    node->outBuf.data        = node->outPtrs;

    HSTREAM hrtfStream = fn_BASS_StreamCreate(info.freq, 2, flags | BASS_SAMPLE_FLOAT, HRTF_StreamProc, node);
    if (!hrtfStream) {
        fn_iplBinauralEffectRelease(&node->effect);
        free(node);
        return 0;
    }

    LockTable();
    for (int i = 0; i < MAX_HRTF_CHANNELS; ++i) {
        if (g_Streams[i] == 0) {
            g_Streams[i] = hrtfStream;
            g_Nodes[i]   = node;
            break;
        }
    }
    UnlockTable();

    fn_BASS_ChannelSetSync(hrtfStream, BASS_SYNC_FREE, 0, HRTF_FreeSync, node);
    return hrtfStream;
}

// Cập nhật tọa độ 3D tương đối (Source - Listener) và độ suy giảm âm lượng
EXPORT BOOL B_CALLBACK BASS_HRTF_Set3D(HSTREAM hrtfStream, float relX, float relY, float relZ, float attenuation, int bilinear) {
    BASS_HRTF_Node *node = NULL;
    for (int i = 0; i < MAX_HRTF_CHANNELS; ++i) {
        if (g_Streams[i] == hrtfStream) {
            node = g_Nodes[i];
            break;
        }
    }
    if (!node) return FALSE;

    // Tự động chuẩn hóa vector hướng về độ dài bằng 1.0
    float len = sqrtf(relX * relX + relY * relY + relZ * relZ);
    if (len > 0.0001f) {
        node->dirX = relX / len;
        node->dirY = relY / len;
        node->dirZ = relZ / len;
    } else {
        node->dirX = 0.0f;
        node->dirY = 0.0f;
        node->dirZ = -1.0f;
    }

    if (attenuation < 0.0f) attenuation = 0.0f;
    node->distanceAtten = attenuation;
    node->interpolation = bilinear ? 1 : 0;
    return TRUE;
}

// Tự động tính cả Hướng + Suy giảm khoảng cách tuyến tính/logarit ngay trong C
EXPORT BOOL B_CALLBACK BASS_HRTF_SetPositionAuto(HSTREAM hrtfStream, float relX, float relY, float relZ, float minDist, float maxDist, int bilinear) {
    float dist = sqrtf(relX * relX + relY * relY + relZ * relZ);
    float atten = 1.0f;
    if (dist > minDist) {
        if (dist >= maxDist && maxDist > minDist) {
            atten = 0.0f;
        } else if (maxDist > minDist) {
            // Công thức suy giảm Inverse Distance Clamped chuẩn game audio
            atten = minDist / dist;
        }
    }
    return BASS_HRTF_Set3D(hrtfStream, relX, relY, relZ, atten, bilinear);
}

// Xóa bộ đệm HRTF khi tua (Seek) kênh gốc
EXPORT BOOL B_CALLBACK BASS_HRTF_Reset(HSTREAM hrtfStream) {
    for (int i = 0; i < MAX_HRTF_CHANNELS; ++i) {
        if (g_Streams[i] == hrtfStream && g_Nodes[i] != NULL) {
            g_Nodes[i]->needReset = 1;
            return TRUE;
        }
    }
    return FALSE;
}