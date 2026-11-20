// ============================================================================
// BASS_HRTF Addon v5.0 - Zero-<windows.h> Cross-Platform Lock-Free HRTF & SteamAudio FX Engine
// Hỗ trợ: Windows (x86/x64/ARM64), Linux, Android, macOS, iOS
// ============================================================================
// Nâng cấp toàn diện v5.0:
// 1. SỬA TRIỆT ĐỂ LỖI STREAM / DSP KHÔNG NHẢ DỮ LIỆU (VOICE CHAT / DECODE / LOOP):
//    - Sửa hằng số BASS_SYNC_FREE = 8 (v4.1 để nhầm = 2 là BASS_SYNC_END khiến
//      node HRTF bị hủy ngay khi push stream tạm cạn buffer hoặc file phát hết 1 lượt).
//    - Sửa hằng số BASS_ATTRIB_GRANULE = 14 (v4.1 để nhầm = 42 là BASS_CONFIG_UNICODE).
//    - Hỗ trợ đầy đủ STREAMPROC_PUSH (Voice Chat thời gian thực), HRECORD, Decode Stream,
//      HSAMPLE (tự động lấy channel/stream), HMUSIC (MOD/XM/IT/S3M/MO3), Mono & Stereo,
//      Int16 & Float32 (tự động ép BASS_DATA_FLOAT khi đọc decode data).
//    - Không bao giờ trả về BASS_STREAMPROC_END khi nguồn âm là Push/Record/Live
//      đang tạm stalled chờ gói tin thoại mới.
// 2. KHẮC PHỤC TRIỆT ĐỂ TIẾNG KIM LOẠI / CLICK ("CÁCH MỘT CÁI"):
//    - Đảm bảo bất biến FIFO: ringInFrames + ringOutFrames == frameSize trong DSP Mode,
//      không bao giờ chèn khoảng trắng (zero-pad) vào giữa luồng âm thanh khi buffer
//      không chia hết cho frameSize.
//    - Không bypass đột ngột iplBinauralEffectApply khi spatialBlend == 0 (tránh nhảy
//      pha group-delay ~20 samples của HRIR gây tiếng kim loại).
//    - Tự động Reset bộ đệm overlap-add của SteamAudio khi đi vào trạng thái
//      Distance Culling (ngủ) để khi thức dậy không phát lại đuôi âm thanh cũ.
//    - Vòng lặp gom đủ frameSize trong StreamProc thay vì zero-pad nửa khung hình.
// 3. HỖ TRỢ SET FRAME SIZE ĐỘNG (64 .. 2048 samples):
//    - BASS_HRTF_SetFrameSize / BASS_HRTF_GetFrameSize / BASS_HRTF_SetNodeFrameSize
//      tối ưu độ trễ cực thấp (128/256) cho Voice Chat hoặc (512/1024) cho nhạc nền.
// 4. KHAI THÁC TOÀN BỘ FX CỦA STEAMAUDIO (LAZY-INIT & TỐI ƯU CPU):
//    - BinauralEffect, DirectEffect (Distance, AirAbsorption, Directivity Dipole,
//      Occlusion, 3-Band Transmission), ReflectionEffect (Parametric Reverb & Early Taps),
//      PathEffect (Multi-path Ambisonics), VirtualSurroundEffect, Ambisonics Pipeline
//      (Encode + 3D Head Rotation Yaw/Pitch/Roll + Binaural), PanningEffect.
//    - Tách biệt rõ API Thêm/Bật FX (AddSteamFX / Enable*) và API Chỉnh Tham Số (SetFXParam / Set*Params).
// ============================================================================

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// CẤU HÌNH ĐA NỀN TẢNG (ZERO <windows.h>)
// ============================================================================
typedef uint32_t DWORD;
typedef uint64_t QWORD;
typedef int32_t  BOOL;

#ifndef TRUE
    #define TRUE  1
    #define FALSE 0
#endif

#if defined(_WIN32) || defined(_WIN64) || defined(__CYGWIN__)
    #define EXPORT __declspec(dllexport)
    #define B_CALLBACK __stdcall
    #define P_CALL __stdcall

    __declspec(dllimport) void* __stdcall LoadLibraryA(const char* lpLibFileName);
    __declspec(dllimport) void* __stdcall GetProcAddress(void* hModule, const char* lpProcName);

    #define LOAD_LIB(name) LoadLibraryA(name)
    #define GET_SYM(lib, sym) ((void*)GetProcAddress((void*)(lib), (sym)))
    #define BASS_LIB_NAME   "bass.dll"
    #define PHONON_LIB_NAME "phonon.dll"

    #if defined(_MSC_VER) && !defined(__clang__)
        #include <intrin.h>
        #define ATOMIC_INC(ptr)                  _InterlockedIncrement((volatile long*)(ptr))
        #define ATOMIC_DEC(ptr)                  _InterlockedDecrement((volatile long*)(ptr))
        #define ATOMIC_CAS_PTR(dest, exch, comp) (_InterlockedCompareExchangePointer((void* volatile*)(dest), (void*)(exch), (void*)(comp)) == (void*)(comp))
        #define ATOMIC_XCHG_PTR(dest, val)       _InterlockedExchangePointer((void* volatile*)(dest), (void*)(val))
        #define MEMORY_BARRIER()                 _ReadWriteBarrier()
    #else
        #define ATOMIC_INC(ptr)                  __atomic_add_fetch((ptr), 1, __ATOMIC_SEQ_CST)
        #define ATOMIC_DEC(ptr)                  __atomic_sub_fetch((ptr), 1, __ATOMIC_SEQ_CST)
        #define ATOMIC_CAS_PTR(dest, exch, comp) __sync_bool_compare_and_swap((dest), (comp), (exch))
        #define ATOMIC_XCHG_PTR(dest, val)       __atomic_exchange_n((dest), (val), __ATOMIC_SEQ_CST)
        #define MEMORY_BARRIER()                 __atomic_thread_fence(__ATOMIC_SEQ_CST)
    #endif
#else
    #define EXPORT __attribute__((visibility("default")))
    #define B_CALLBACK
    #define P_CALL

    extern void* dlopen(const char* filename, int flag);
    extern void* dlsym(void* handle, const char* symbol);

    #if defined(__APPLE__)
        #define HRTF_RTLD_NOW    0x2
        #define HRTF_RTLD_GLOBAL 0x8
        #define HRTF_RTLD_DEFAULT ((void*)-2)
        #if defined(IS_IOS_PLATFORM) || defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__) || (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE) || (defined(TARGET_OS_SIMULATOR) && TARGET_OS_SIMULATOR)
            #ifndef IS_IOS_PLATFORM
                #define IS_IOS_PLATFORM 1
            #endif
            #define BASS_LIB_NAME   "@rpath/bass.framework/bass"
            #define PHONON_LIB_NAME "@rpath/phonon.framework/phonon"
            #define BASS_LIB_ALT    "libbass.dylib"
            #define PHONON_LIB_ALT  "libphonon.dylib"
        #else
            #define BASS_LIB_NAME   "libbass.dylib"
            #define PHONON_LIB_NAME "libphonon.dylib"
        #endif
    #elif defined(__ANDROID__) && !defined(__LP64__)
        // Android 32-bit (armeabi-v7a, x86) Bionic libc ABI
        #define HRTF_RTLD_NOW     0x00000
        #define HRTF_RTLD_GLOBAL  0x00002
        #define HRTF_RTLD_DEFAULT ((void*)0xffffffff)
        #define BASS_LIB_NAME     "libbass.so"
        #define PHONON_LIB_NAME   "libphonon.so"
    #else
        // Linux (glibc/musl) & Android 64-bit (arm64-v8a, x86_64)
        #define HRTF_RTLD_NOW     0x00002
        #define HRTF_RTLD_GLOBAL  0x00100
        #define HRTF_RTLD_DEFAULT ((void*)0)
        #define BASS_LIB_NAME     "libbass.so"
        #define PHONON_LIB_NAME   "libphonon.so"
    #endif

    #define LOAD_LIB(name) dlopen((name), HRTF_RTLD_NOW | HRTF_RTLD_GLOBAL)
    #define GET_SYM(lib, sym) dlsym((lib), (sym))

    #define ATOMIC_INC(ptr)                  __atomic_add_fetch((ptr), 1, __ATOMIC_SEQ_CST)
    #define ATOMIC_DEC(ptr)                  __atomic_sub_fetch((ptr), 1, __ATOMIC_SEQ_CST)
    #define ATOMIC_CAS_PTR(dest, exch, comp) __sync_bool_compare_and_swap((dest), (comp), (exch))
    #define ATOMIC_XCHG_PTR(dest, val)       __atomic_exchange_n((dest), (val), __ATOMIC_SEQ_CST)
    #define MEMORY_BARRIER()                 __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif

typedef DWORD HSTREAM;
typedef DWORD HSYNC;
typedef DWORD HDSP;
typedef DWORD HSAMPLE;
typedef DWORD HCHANNEL;

// Chuẩn hằng số BASS 2.4 (Đã sửa chính xác BASS_SYNC_FREE=8 và BASS_ATTRIB_GRANULE=14)
#define BASS_SAMPLE_LOOP        4
#define BASS_SAMPLE_FLOAT       0x100
#define BASS_STREAM_AUTOFREE    0x40000
#define BASS_STREAM_DECODE      0x200000
#define BASS_STREAMPROC_END     0x80000000
#define BASS_SYNC_END           2
#define BASS_SYNC_FREE          8
#define BASS_SYNC_SETPOS        11
#define BASS_CONFIG_FLOATDSP    9
#define BASS_ATTRIB_GRANULE     14
#define BASS_DATA_AVAILABLE     0
#define BASS_DATA_FLOAT         0x40000000
#define BASS_ACTIVE_STOPPED     0
#define BASS_ACTIVE_PLAYING     1
#define BASS_ACTIVE_STALLED     2
#define BASS_ACTIVE_PAUSED      3
#define BASS_CTYPE_SAMPLE       1
#define BASS_CTYPE_RECORD       2
#define BASS_CTYPE_STREAM       0x10000
#define BASS_CTYPE_MUSIC_MOD    0x20000
#define BASS_SAMCHAN_NEW        1
#define BASS_SAMCHAN_STREAM     2

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

typedef struct {
    DWORD freq;
    float volume;
    float pan;
    DWORD flags;
    DWORD length;
    DWORD max;
    DWORD origres;
    DWORD chans;
    DWORD mingap;
    DWORD mode3d;
    float mindist;
    float maxdist;
    DWORD iangle;
    DWORD oangle;
    float outvol;
    DWORD vam;
    DWORD priority;
} BASS_SAMPLE_INFO;

typedef DWORD (B_CALLBACK *STREAMPROC)(HSTREAM handle, void *buffer, DWORD length, void *user);
typedef void  (B_CALLBACK *DSPPROC)(HDSP handle, DWORD channel, void *buffer, DWORD length, void *user);
typedef void  (B_CALLBACK *SYNCPROC)(HSYNC handle, DWORD channel, DWORD data, void *user);

static BOOL     (B_CALLBACK *fn_BASS_SetConfig)(DWORD option, DWORD value) = NULL;
static int      (B_CALLBACK *fn_BASS_ErrorGetCode)(void) = NULL;
static BOOL     (B_CALLBACK *fn_BASS_ChannelGetInfo)(DWORD handle, BASS_CHANNELINFO *info) = NULL;
static DWORD    (B_CALLBACK *fn_BASS_ChannelIsActive)(DWORD handle) = NULL;
static DWORD    (B_CALLBACK *fn_BASS_ChannelFlags)(DWORD handle, DWORD flags, DWORD mask) = NULL;
static HSTREAM  (B_CALLBACK *fn_BASS_StreamCreate)(DWORD freq, DWORD chans, DWORD flags, STREAMPROC proc, void *user) = NULL;
static BOOL     (B_CALLBACK *fn_BASS_StreamFree)(HSTREAM handle) = NULL;
static DWORD    (B_CALLBACK *fn_BASS_StreamPutData)(HSTREAM handle, const void *buffer, DWORD length) = NULL;
static DWORD    (B_CALLBACK *fn_BASS_ChannelGetData)(DWORD handle, void *buffer, DWORD length) = NULL;
static HSYNC    (B_CALLBACK *fn_BASS_ChannelSetSync)(DWORD handle, DWORD type, QWORD param, SYNCPROC proc, void *user) = NULL;
static BOOL     (B_CALLBACK *fn_BASS_ChannelRemoveSync)(DWORD handle, HSYNC sync) = NULL;
static HDSP     (B_CALLBACK *fn_BASS_ChannelSetDSP)(DWORD handle, DSPPROC proc, void *user, int priority) = NULL;
static BOOL     (B_CALLBACK *fn_BASS_ChannelRemoveDSP)(DWORD handle, HDSP dsp) = NULL;
static BOOL     (B_CALLBACK *fn_BASS_ChannelSetAttribute)(DWORD handle, DWORD attrib, float value) = NULL;
static BOOL     (B_CALLBACK *fn_BASS_SampleGetInfo)(HSAMPLE handle, BASS_SAMPLE_INFO *info) = NULL;
static HCHANNEL (B_CALLBACK *fn_BASS_SampleGetChannel)(HSAMPLE handle, DWORD flags) = NULL;

// ============================================================================
// STEAMAUDIO (PHONON) DEFINITIONS & TYPES
// ============================================================================
#define DEFAULT_STEAMAUDIO_VERSION 0x00040500
#define DEFAULT_HRTF_FRAME_SIZE    512
#define MIN_HRTF_FRAME_SIZE        64
#define MAX_HRTF_FRAME_SIZE        2048
#define MAX_AMBI_CHANNELS          16  // Hỗ trợ tới bậc 3 ((3+1)^2 = 16 kênh)
#define MAX_SURROUND_CHANNELS      8   // Hỗ trợ tới 7.1 (8 kênh)

#define IPL_STATUS_SUCCESS 0
#define IPL_FALSE 0
#define IPL_TRUE  1

#define IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION (1 << 0)
#define IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION       (1 << 1)
#define IPL_DIRECTEFFECTFLAGS_APPLYDIRECTIVITY         (1 << 2)
#define IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION           (1 << 3)
#define IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION        (1 << 4)
#define IPL_TRANSMISSIONTYPE_FREQINDEPENDENT           0
#define IPL_TRANSMISSIONTYPE_FREQDEPENDENT             1

#define IPL_REFLECTIONEFFECTTYPE_CONVOLUTION           0
#define IPL_REFLECTIONEFFECTTYPE_PARAMETRIC            1
#define IPL_REFLECTIONEFFECTTYPE_HYBRID                2
#define IPL_REFLECTIONEFFECTTYPE_TAN                   3

#define IPL_SPEAKERLAYOUTTYPE_MONO                     0
#define IPL_SPEAKERLAYOUTTYPE_STEREO                   1
#define IPL_SPEAKERLAYOUTTYPE_QUADRAPHONIC             2
#define IPL_SPEAKERLAYOUTTYPE_SURROUND_5_1             3
#define IPL_SPEAKERLAYOUTTYPE_SURROUND_7_1             4
#define IPL_SPEAKERLAYOUTTYPE_CUSTOM                   5

// Chế độ Spatialization (Panning Mode)
#define BASS_HRTF_MODE_BINAURAL                        0
#define BASS_HRTF_MODE_PANNING                         1
#define BASS_HRTF_MODE_AMBISONICS                      2
#define BASS_HRTF_MODE_VIRTUAL_SURROUND                3

// Mã FX của SteamAudio (dùng cho AddSteamFX / RemoveSteamFX / SetFXParam)
#define BASS_HRTF_FX_DIRECT                            1
#define BASS_HRTF_FX_REFLECTION                        2
#define BASS_HRTF_FX_PATHING                           3
#define BASS_HRTF_FX_VIRTUAL_SURROUND                  4
#define BASS_HRTF_FX_AMBISONICS                        5
#define BASS_HRTF_FX_PANNING                           6

// Mã Tham số (Param ID) cho SetFXParam / GetFXParam
#define BASS_HRTF_PARAM_OCCLUSION                      1
#define BASS_HRTF_PARAM_SPATIAL_BLEND                  2
#define BASS_HRTF_PARAM_GAIN                           3
#define BASS_HRTF_PARAM_AIR_ABSORPTION                 4
#define BASS_HRTF_PARAM_DIRECTIVITY                    5
#define BASS_HRTF_PARAM_DIRECTIVITY_WEIGHT             6
#define BASS_HRTF_PARAM_DIRECTIVITY_POWER              7
#define BASS_HRTF_PARAM_SOURCE_RADIUS                  8
#define BASS_HRTF_PARAM_TRANS_LOW                      9
#define BASS_HRTF_PARAM_TRANS_MID                      10
#define BASS_HRTF_PARAM_TRANS_HIGH                     11
#define BASS_HRTF_PARAM_REVERB_LOW                     12
#define BASS_HRTF_PARAM_REVERB_MID                     13
#define BASS_HRTF_PARAM_REVERB_HIGH                    14
#define BASS_HRTF_PARAM_REVERB_EQ_LOW                  15
#define BASS_HRTF_PARAM_REVERB_EQ_MID                  16
#define BASS_HRTF_PARAM_REVERB_EQ_HIGH                 17
#define BASS_HRTF_PARAM_REVERB_DELAY_MS                18
#define BASS_HRTF_PARAM_REVERB_WET                     19
#define BASS_HRTF_PARAM_PATH_EQ_LOW                    20
#define BASS_HRTF_PARAM_PATH_EQ_MID                    21
#define BASS_HRTF_PARAM_PATH_EQ_HIGH                   22
#define BASS_HRTF_PARAM_PATH_WET                       23
#define BASS_HRTF_PARAM_AMBI_ORDER                     24
#define BASS_HRTF_PARAM_SURROUND_LAYOUT                25
#define BASS_HRTF_PARAM_INTERPOLATION                  26
#define BASS_HRTF_PARAM_LOW_LATENCY_FLUSH              27

typedef void* IPLContext;
typedef void* IPLHRTF;
typedef void* IPLBinauralEffect;
typedef void* IPLDirectEffect;
typedef void* IPLReflectionEffect;
typedef void* IPLPathEffect;
typedef void* IPLVirtualSurroundEffect;
typedef void* IPLPanningEffect;
typedef void* IPLAmbisonicsEncodeEffect;
typedef void* IPLAmbisonicsRotationEffect;
typedef void* IPLAmbisonicsBinauralEffect;
typedef void* IPLAmbisonicsPanningEffect;
typedef void* IPLAmbisonicsDecodeEffect;
typedef int32_t IPLerror;

typedef struct { float x, y, z; } IPLVector3;

typedef struct {
    IPLVector3 right;
    IPLVector3 up;
    IPLVector3 ahead;
    IPLVector3 origin;
} IPLCoordinateSpace3;

typedef struct {
    uint32_t version;
    void* logCallback;
    void* allocateCallback;
    void* freeCallback;
    int32_t simdLevel;
    uint32_t flags;
} IPLContextSettings;

typedef struct {
    int32_t samplingRate;
    int32_t frameSize;
} IPLAudioSettings;

typedef struct {
    int32_t type;
    const char* sofaFileName;
    const uint8_t* sofaData;
    int32_t sofaDataSize;
    float volume;
    int32_t normType;
} IPLHRTFSettings;

typedef struct {
    int32_t type;
    int32_t numSpeakers;
    IPLVector3* speakers;
} IPLSpeakerLayout;

typedef struct { IPLHRTF hrtf; } IPLBinauralEffectSettings;
typedef struct { int32_t numChannels; } IPLDirectEffectSettings;

typedef struct {
    IPLVector3 direction;
    int32_t interpolation;
    float spatialBlend;
    IPLHRTF hrtf;
    float* peakDelays;
} IPLBinauralEffectParams;

typedef struct {
    int32_t flags;
    int32_t transmissionType;
    float distanceAttenuation;
    float airAbsorption[3];
    float directivity;
    float occlusion;
    float transmission[3];
} IPLDirectEffectParams;

typedef struct {
    int32_t type;
    int32_t irSize;
    int32_t numChannels;
} IPLReflectionEffectSettings;

typedef struct {
    int32_t type;
    void* ir;
    float reverbTimes[3];
    float eq[3];
    int32_t delay;
    int32_t irSize;
    int32_t numChannels;
    void* tanDevice;
    int32_t tanSlot;
} IPLReflectionEffectParams;

typedef struct {
    int32_t maxOrder;
    int32_t spatialize;
    IPLSpeakerLayout speakerLayout;
    IPLHRTF hrtf;
} IPLPathEffectSettings;

typedef struct {
    float eqCoeffs[3];
    float* shCoeffs;
    int32_t order;
    int32_t binaural;
    IPLHRTF hrtf;
    IPLCoordinateSpace3 listener;
} IPLPathEffectParams;

typedef struct {
    IPLSpeakerLayout speakerLayout;
    IPLHRTF hrtf;
} IPLVirtualSurroundEffectSettings;

typedef struct {
    IPLHRTF hrtf;
} IPLVirtualSurroundEffectParams;

typedef struct {
    IPLSpeakerLayout speakerLayout;
} IPLPanningEffectSettings;

typedef struct {
    IPLVector3 direction;
} IPLPanningEffectParams;

typedef struct { int32_t maxOrder; } IPLAmbisonicsEncodeEffectSettings;
typedef struct {
    IPLVector3 direction;
    int32_t order;
} IPLAmbisonicsEncodeEffectParams;

typedef struct { int32_t maxOrder; } IPLAmbisonicsRotationEffectSettings;
typedef struct {
    IPLCoordinateSpace3 orientation;
    int32_t order;
} IPLAmbisonicsRotationEffectParams;

typedef struct {
    IPLHRTF hrtf;
    int32_t maxOrder;
} IPLAmbisonicsBinauralEffectSettings;

typedef struct {
    IPLHRTF hrtf;
    int32_t order;
} IPLAmbisonicsBinauralEffectParams;

typedef struct {
    int32_t type;
    float minDistance;
    void* callback;
    void* userData;
    int32_t dirty;
} IPLDistanceAttenuationModel;

typedef struct {
    int32_t type;
    float coefficients[3];
    void* callback;
    void* userData;
    int32_t dirty;
} IPLAirAbsorptionModel;

typedef struct {
    float dipoleWeight;
    float dipolePower;
    void* callback;
    void* userData;
} IPLDirectivity;

typedef struct {
    int32_t numChannels;
    int32_t numSamples;
    float** data;
} IPLAudioBuffer;

typedef struct {
    float absorption[3];
    float scattering;
    float transmission[3];
} IPLMaterial;

// Con trỏ hàm SteamAudio cốt lõi
static IPLerror (P_CALL *fn_iplContextCreate)(IPLContextSettings*, IPLContext*) = NULL;
static void     (P_CALL *fn_iplContextRelease)(IPLContext*) = NULL;
static IPLerror (P_CALL *fn_iplHRTFCreate)(IPLContext, IPLAudioSettings*, IPLHRTFSettings*, IPLHRTF*) = NULL;
static void     (P_CALL *fn_iplHRTFRelease)(IPLHRTF*) = NULL;

static IPLerror (P_CALL *fn_iplBinauralEffectCreate)(IPLContext, IPLAudioSettings*, IPLBinauralEffectSettings*, IPLBinauralEffect*) = NULL;
static void     (P_CALL *fn_iplBinauralEffectRelease)(IPLBinauralEffect*) = NULL;
static void     (P_CALL *fn_iplBinauralEffectReset)(IPLBinauralEffect) = NULL;
static int32_t  (P_CALL *fn_iplBinauralEffectApply)(IPLBinauralEffect, IPLBinauralEffectParams*, IPLAudioBuffer*, IPLAudioBuffer*) = NULL;

static IPLerror (P_CALL *fn_iplDirectEffectCreate)(IPLContext, IPLAudioSettings*, IPLDirectEffectSettings*, IPLDirectEffect*) = NULL;
static void     (P_CALL *fn_iplDirectEffectRelease)(IPLDirectEffect*) = NULL;
static void     (P_CALL *fn_iplDirectEffectReset)(IPLDirectEffect) = NULL;
static int32_t  (P_CALL *fn_iplDirectEffectApply)(IPLDirectEffect, IPLDirectEffectParams*, IPLAudioBuffer*, IPLAudioBuffer*) = NULL;

static IPLerror (P_CALL *fn_iplReflectionEffectCreate)(IPLContext, IPLAudioSettings*, IPLReflectionEffectSettings*, IPLReflectionEffect*) = NULL;
static void     (P_CALL *fn_iplReflectionEffectRelease)(IPLReflectionEffect*) = NULL;
static void     (P_CALL *fn_iplReflectionEffectReset)(IPLReflectionEffect) = NULL;
static int32_t  (P_CALL *fn_iplReflectionEffectApply)(IPLReflectionEffect, IPLReflectionEffectParams*, IPLAudioBuffer*, IPLAudioBuffer*, void*) = NULL;

static IPLerror (P_CALL *fn_iplPathEffectCreate)(IPLContext, IPLAudioSettings*, IPLPathEffectSettings*, IPLPathEffect*) = NULL;
static void     (P_CALL *fn_iplPathEffectRelease)(IPLPathEffect*) = NULL;
static void     (P_CALL *fn_iplPathEffectReset)(IPLPathEffect) = NULL;
static int32_t  (P_CALL *fn_iplPathEffectApply)(IPLPathEffect, IPLPathEffectParams*, IPLAudioBuffer*, IPLAudioBuffer*) = NULL;

static IPLerror (P_CALL *fn_iplVirtualSurroundEffectCreate)(IPLContext, IPLAudioSettings*, IPLVirtualSurroundEffectSettings*, IPLVirtualSurroundEffect*) = NULL;
static void     (P_CALL *fn_iplVirtualSurroundEffectRelease)(IPLVirtualSurroundEffect*) = NULL;
static void     (P_CALL *fn_iplVirtualSurroundEffectReset)(IPLVirtualSurroundEffect) = NULL;
static int32_t  (P_CALL *fn_iplVirtualSurroundEffectApply)(IPLVirtualSurroundEffect, IPLVirtualSurroundEffectParams*, IPLAudioBuffer*, IPLAudioBuffer*) = NULL;

static IPLerror (P_CALL *fn_iplPanningEffectCreate)(IPLContext, IPLAudioSettings*, IPLPanningEffectSettings*, IPLPanningEffect*) = NULL;
static void     (P_CALL *fn_iplPanningEffectRelease)(IPLPanningEffect*) = NULL;
static void     (P_CALL *fn_iplPanningEffectReset)(IPLPanningEffect) = NULL;
static int32_t  (P_CALL *fn_iplPanningEffectApply)(IPLPanningEffect, IPLPanningEffectParams*, IPLAudioBuffer*, IPLAudioBuffer*) = NULL;

static IPLerror (P_CALL *fn_iplAmbisonicsEncodeEffectCreate)(IPLContext, IPLAudioSettings*, IPLAmbisonicsEncodeEffectSettings*, IPLAmbisonicsEncodeEffect*) = NULL;
static void     (P_CALL *fn_iplAmbisonicsEncodeEffectRelease)(IPLAmbisonicsEncodeEffect*) = NULL;
static void     (P_CALL *fn_iplAmbisonicsEncodeEffectReset)(IPLAmbisonicsEncodeEffect) = NULL;
static int32_t  (P_CALL *fn_iplAmbisonicsEncodeEffectApply)(IPLAmbisonicsEncodeEffect, IPLAmbisonicsEncodeEffectParams*, IPLAudioBuffer*, IPLAudioBuffer*) = NULL;

static IPLerror (P_CALL *fn_iplAmbisonicsRotationEffectCreate)(IPLContext, IPLAudioSettings*, IPLAmbisonicsRotationEffectSettings*, IPLAmbisonicsRotationEffect*) = NULL;
static void     (P_CALL *fn_iplAmbisonicsRotationEffectRelease)(IPLAmbisonicsRotationEffect*) = NULL;
static void     (P_CALL *fn_iplAmbisonicsRotationEffectReset)(IPLAmbisonicsRotationEffect) = NULL;
static int32_t  (P_CALL *fn_iplAmbisonicsRotationEffectApply)(IPLAmbisonicsRotationEffect, IPLAmbisonicsRotationEffectParams*, IPLAudioBuffer*, IPLAudioBuffer*) = NULL;

static IPLerror (P_CALL *fn_iplAmbisonicsBinauralEffectCreate)(IPLContext, IPLAudioSettings*, IPLAmbisonicsBinauralEffectSettings*, IPLAmbisonicsBinauralEffect*) = NULL;
static void     (P_CALL *fn_iplAmbisonicsBinauralEffectRelease)(IPLAmbisonicsBinauralEffect*) = NULL;
static void     (P_CALL *fn_iplAmbisonicsBinauralEffectReset)(IPLAmbisonicsBinauralEffect) = NULL;
static int32_t  (P_CALL *fn_iplAmbisonicsBinauralEffectApply)(IPLAmbisonicsBinauralEffect, IPLAmbisonicsBinauralEffectParams*, IPLAudioBuffer*, IPLAudioBuffer*) = NULL;

static void       (P_CALL *fn_iplAudioBufferDeinterleave)(IPLContext, float*, IPLAudioBuffer*) = NULL;
static void       (P_CALL *fn_iplAudioBufferInterleave)(IPLContext, IPLAudioBuffer*, float*) = NULL;
static void       (P_CALL *fn_iplAudioBufferDownmix)(IPLContext, IPLAudioBuffer*, IPLAudioBuffer*) = NULL;
static void       (P_CALL *fn_iplAudioBufferMix)(IPLContext, IPLAudioBuffer*, IPLAudioBuffer*) = NULL;
static IPLVector3 (P_CALL *fn_iplCalculateRelativeDirection)(IPLContext, IPLVector3, IPLVector3, IPLVector3, IPLVector3) = NULL;
static float      (P_CALL *fn_iplDistanceAttenuationCalculate)(IPLContext, IPLVector3, IPLVector3, IPLDistanceAttenuationModel*) = NULL;
static void       (P_CALL *fn_iplAirAbsorptionCalculate)(IPLContext, IPLVector3, IPLVector3, IPLAirAbsorptionModel*, float*) = NULL;
static float      (P_CALL *fn_iplDirectivityCalculate)(IPLContext, IPLCoordinateSpace3, IPLVector3, IPLDirectivity*) = NULL;

// ============================================================================
// TRẠNG THÁI TOÀN CỤC & HỆ THỐNG PRESET VẬT LIỆU / REVERB
// ============================================================================
static void* g_BassLib = NULL;
static void* g_PhononLib = NULL;
static IPLContext g_Context = NULL;
static IPLHRTF g_HRTF = NULL;
static int g_SampleRate = 44100;
static volatile int g_FrameSize = DEFAULT_HRTF_FRAME_SIZE;
static char g_SofaPath[512] = {0};

static float g_DefaultMinDist   = 1.0f;
static float g_DefaultMaxDist   = 50.0f;
static float g_DefaultRolloff   = 0.12f;
static float g_DefaultBlendStep = 0.25f;

static volatile float g_ListenerX     = 0.0f;
static volatile float g_ListenerY     = 0.0f;
static volatile float g_ListenerZ     = 0.0f;
static volatile float g_ListenerYaw   = 0.0f;
static volatile float g_ListenerPitch = 0.0f;
static volatile float g_ListenerRoll  = 0.0f;

// 12 Preset Vật liệu chuẩn từ SteamAudio / NVGT sound.cpp
static const IPLMaterial g_PresetMaterials[12] = {
    {{0.00f, 0.00f, 0.00f}, 0.00f, {1.000f, 1.000f, 1.000f}}, // 0: Air
    {{0.10f, 0.20f, 0.30f}, 0.05f, {0.100f, 0.050f, 0.030f}}, // 1: Generic
    {{0.03f, 0.04f, 0.07f}, 0.05f, {0.015f, 0.015f, 0.015f}}, // 2: Brick
    {{0.05f, 0.07f, 0.08f}, 0.05f, {0.015f, 0.002f, 0.001f}}, // 3: Concrete
    {{0.01f, 0.02f, 0.02f}, 0.05f, {0.060f, 0.044f, 0.011f}}, // 4: Ceramic
    {{0.60f, 0.70f, 0.80f}, 0.05f, {0.031f, 0.012f, 0.008f}}, // 5: Gravel
    {{0.24f, 0.69f, 0.73f}, 0.05f, {0.020f, 0.005f, 0.003f}}, // 6: Carpet
    {{0.06f, 0.03f, 0.02f}, 0.05f, {0.060f, 0.044f, 0.011f}}, // 7: Glass
    {{0.12f, 0.06f, 0.04f}, 0.05f, {0.056f, 0.056f, 0.004f}}, // 8: Plaster
    {{0.11f, 0.07f, 0.06f}, 0.05f, {0.070f, 0.014f, 0.005f}}, // 9: Wood
    {{0.20f, 0.07f, 0.06f}, 0.05f, {0.200f, 0.025f, 0.010f}}, // 10: Metal
    {{0.13f, 0.20f, 0.24f}, 0.05f, {0.015f, 0.002f, 0.001f}}  // 11: Rock
};

// 16 Preset Không gian Reverb Vật lý 3 Băng Tần chuẩn SteamAudio
typedef struct {
    float reverbTimes[3]; // Low, Mid, High RT60 (seconds)
    float eq[3];          // Low, Mid, High gain
    float delayMs;        // Early reflection pre-delay (ms)
    float defaultWet;     // Default wet mix
} SteamReverbPreset;

static const SteamReverbPreset g_ReverbPresets[16] = {
    {{0.45f, 0.38f, 0.25f}, {1.00f, 0.95f, 0.82f},  8.0f, 0.22f}, // 0: Small Room
    {{0.85f, 0.72f, 0.48f}, {1.00f, 0.92f, 0.75f}, 14.0f, 0.28f}, // 1: Medium Room
    {{1.45f, 1.25f, 0.85f}, {1.00f, 0.90f, 0.70f}, 24.0f, 0.34f}, // 2: Large Room
    {{2.40f, 2.10f, 1.35f}, {1.00f, 0.94f, 0.72f}, 35.0f, 0.42f}, // 3: Concert Hall
    {{1.90f, 1.75f, 1.15f}, {0.95f, 1.00f, 0.80f}, 18.0f, 0.38f}, // 4: Long Hallway
    {{3.20f, 2.85f, 1.60f}, {1.00f, 0.88f, 0.55f}, 45.0f, 0.52f}, // 5: Cave
    {{3.80f, 3.10f, 1.40f}, {0.92f, 0.85f, 0.48f}, 85.0f, 0.45f}, // 6: Canyon
    {{0.95f, 1.05f, 0.88f}, {0.90f, 1.00f, 0.96f},  6.0f, 0.36f}, // 7: Bathroom
    {{2.60f, 2.20f, 1.30f}, {1.00f, 0.90f, 0.68f}, 32.0f, 0.40f}, // 8: Warehouse
    {{2.20f, 2.00f, 1.45f}, {0.95f, 1.00f, 0.85f}, 22.0f, 0.44f}, // 9: Tunnel / Sewer
    {{1.10f, 0.85f, 0.35f}, {0.90f, 0.82f, 0.45f}, 40.0f, 0.24f}, // 10: Forest
    {{1.35f, 1.10f, 0.65f}, {0.95f, 0.90f, 0.65f}, 50.0f, 0.26f}, // 11: City Street
    {{1.80f, 0.90f, 0.15f}, {1.00f, 0.45f, 0.08f}, 12.0f, 0.58f}, // 12: Underwater
    {{1.65f, 1.50f, 1.10f}, {1.00f, 0.96f, 0.85f}, 16.0f, 0.40f}, // 13: Concrete Bunker
    {{0.22f, 0.18f, 0.12f}, {1.00f, 0.98f, 0.92f},  4.0f, 0.12f}, // 14: Studio Vocal Booth
    {{4.20f, 3.60f, 2.10f}, {1.00f, 0.92f, 0.68f}, 55.0f, 0.50f}  // 15: Cathedral / Arena
};

#define MAX_ENV_BOXES 512
typedef struct {
    int active;
    float minX, maxX, minY, maxY, minZ, maxZ;
    IPLMaterial mat;
} EnvBox;

static EnvBox g_EnvBoxes[MAX_ENV_BOXES];
static volatile int g_EnvBoxCount = 0;
static volatile int g_AutoEnvRaycast = 0;

// Bộ đệm trễ Early Reflections 4-tap cực nhẹ (2048 mẫu stereo ~ 46ms)
#define REVERB_DELAY_LINE_MASK 4095
#define REVERB_DELAY_LINE_SIZE 4096

// ============================================================================
// CẤU TRÚC NODE HRTF (QUẢN LÝ BẰNG ATOMIC REFCOUNT - 100% LOCK-FREE)
// ============================================================================
typedef struct {
    volatile long refCount;
    volatile int isAlive;

    DWORD handleKey;
    DWORD sourceChan;
    DWORD ownedSampleChan;
    HDSP dspHandle;
    HSYNC freeSync;
    HSYNC posSync;
    DWORD sampleRate;
    int sourceChans;
    int isDSPMode;
    int isDecodeStream;
    int isLiveSource;
    volatile int lowLatencyFlush;

    int frameSize;
    volatile int targetFrameSize;

    // Các đối tượng hiệu ứng SteamAudio (Lazy-allocated để tối ưu RAM & CPU)
    IPLBinauralEffect           binauralEffect;
    IPLDirectEffect             directEffect;
    IPLReflectionEffect         reflectionEffect;
    IPLPathEffect               pathEffect;
    IPLVirtualSurroundEffect    virtualSurroundEffect;
    IPLPanningEffect            panningEffect;
    IPLAmbisonicsEncodeEffect   ambiEncodeEffect;
    IPLAmbisonicsRotationEffect ambiRotateEffect;
    IPLAmbisonicsBinauralEffect ambiBinauralEffect;

    volatile int enabled;
    volatile int panningMode;        // 0=Binaural, 1=Panning, 2=Ambisonics, 3=VirtualSurround
    volatile int useDirectFilter;
    volatile int useReflectionEffect;
    volatile int usePathEffect;
    volatile int useVirtualSurround;
    volatile int useAmbisonics;
    volatile int useStereoInput;

    volatile int ambiOrder;          // 1..3 (Mặc định 1 = 4 kênh FOA)
    volatile int surroundLayoutType; // IPL_SPEAKERLAYOUTTYPE_*

    // Tọa độ hướng chuẩn hóa trong hệ SteamAudio
    volatile float dirX, dirY, dirZ;

    // Tọa độ tương đối & tọa độ thế giới
    volatile float relX, relY, relZ;
    volatile float worldX, worldY, worldZ;
    volatile int useWorldPos;

    // Hướng phát của nguồn âm (Dùng cho Directivity Cone / Dipole)
    volatile float srcAheadX, srcAheadY, srcAheadZ;
    volatile float dipoleWeight, dipolePower;
    volatile int useDipoleCalc;

    volatile float spatialBlend;
    volatile float maxSpatialBlend;
    volatile float distAtten;
    volatile float userGain;
    volatile float gainAtten;
    float prevGain;
    volatile float distanceUnits;
    volatile float effectiveDist;
    volatile float occlusion;
    volatile float airAbsorption;
    volatile float airAbsCoeffLow, airAbsCoeffMid, airAbsCoeffHigh;
    volatile float transLow, transMid, transHigh;
    volatile float baseTransLow, baseTransMid, baseTransHigh;
    volatile float directivity;
    volatile float sourceRadius;
    volatile int interpolation;
    volatile int needReset;
    int wasCulled;

    // Tham số Reflection / Parametric Reverb
    volatile float reverbTimes[3];
    volatile float reverbEq[3];
    volatile float reverbDelayMs;
    volatile float reverbWetMix;
    float erDelayBufL[REVERB_DELAY_LINE_SIZE];
    float erDelayBufR[REVERB_DELAY_LINE_SIZE];
    int erWriteIdx;
    float erLpStateL, erLpStateR;

    // Tham số Pathing Effect
    volatile float pathEq[3];
    volatile float pathSh[MAX_AMBI_CHANNELS];
    volatile float pathWetMix;

    float minDist;
    float maxDist;
    float rolloff;
    float blendStep;

    // Slide state tracking
    volatile int slidePosActive;
    float slideStartX, slideStartY, slideStartZ;
    float slideTargetX, slideTargetY, slideTargetZ;
    float slidePosElapsedMs, slidePosTotalMs;

    volatile int slideAttrActive;
    int slideAttrType;
    float slideAttrStart, slideAttrTarget;
    float slideAttrElapsedMs, slideAttrTotalMs;

    // Buffers Deinterleaved chuẩn cho SteamAudio SIMD (Hỗ trợ tới MAX_HRTF_FRAME_SIZE = 2048)
    float inLeftFrame[MAX_HRTF_FRAME_SIZE];
    float inRightFrame[MAX_HRTF_FRAME_SIZE];
    float inMonoFrame[MAX_HRTF_FRAME_SIZE];
    float processedMonoFrame[MAX_HRTF_FRAME_SIZE];
    float reverbMonoFrame[MAX_HRTF_FRAME_SIZE];
    float outLeftFrame[MAX_HRTF_FRAME_SIZE];
    float outRightFrame[MAX_HRTF_FRAME_SIZE];
    float auxLeftFrame[MAX_HRTF_FRAME_SIZE];
    float auxRightFrame[MAX_HRTF_FRAME_SIZE];
    float ambiFrames[MAX_AMBI_CHANNELS][MAX_HRTF_FRAME_SIZE];
    float ambiRotFrames[MAX_AMBI_CHANNELS][MAX_HRTF_FRAME_SIZE];

    float* inStereoPtrs[2];
    float* inMonoPtrs[1];
    float* procPtrs[1];
    float* revPtrs[1];
    float* outPtrs[2];
    float* auxPtrs[2];
    float* ambiPtrs[MAX_AMBI_CHANNELS];
    float* ambiRotPtrs[MAX_AMBI_CHANNELS];

    IPLAudioBuffer inStereoBuf;
    IPLAudioBuffer inMonoBuf;
    IPLAudioBuffer procBuf;
    IPLAudioBuffer revBuf;
    IPLAudioBuffer outBuf;
    IPLAudioBuffer auxBuf;
    IPLAudioBuffer ambiBuf;
    IPLAudioBuffer ambiRotBuf;

    // Ring Buffer O(1) bảo toàn bất biến FIFO: ringInFrames + ringOutFrames == frameSize
    float ringInStereo[MAX_HRTF_FRAME_SIZE * 2 * 4];
    float ringOutStereo[MAX_HRTF_FRAME_SIZE * 2 * 4];
    float streamDecodeTemp[MAX_HRTF_FRAME_SIZE * 8];
    int ringInFrames;
    int ringOutReadPos;
    int ringOutWritePos;
    int dspFifoPrimed;
    int tailFlushed;
} BASS_HRTF_Node;

#define MAX_HRTF_CHANNELS 2048
static BASS_HRTF_Node* volatile g_Nodes[MAX_HRTF_CHANNELS] = {0};
static volatile int g_MaxNodeLimit = 0;

// ============================================================================
// QUẢN LÝ VÒNG ĐỜI NODE 100% LOCK-FREE
// ============================================================================
static int SanitizeFrameSize(int fs) {
    if (fs <= 0) return g_FrameSize > 0 ? g_FrameSize : DEFAULT_HRTF_FRAME_SIZE;
    if (fs < MIN_HRTF_FRAME_SIZE) fs = MIN_HRTF_FRAME_SIZE;
    if (fs > MAX_HRTF_FRAME_SIZE) fs = MAX_HRTF_FRAME_SIZE;
    // Làm tròn về bội số của 16 để tối ưu hóa SIMD SSE/AVX/NEON
    fs = (fs + 15) & ~15;
    if (fs > MAX_HRTF_FRAME_SIZE) fs = MAX_HRTF_FRAME_SIZE;
    return fs;
}

static void Node_AddRef(BASS_HRTF_Node *node) {
    if (node) {
        ATOMIC_INC(&node->refCount);
    }
}

static void Node_ReleaseEffects(BASS_HRTF_Node *node) {
    if (!node) return;
    if (node->binauralEffect && fn_iplBinauralEffectRelease) fn_iplBinauralEffectRelease(&node->binauralEffect);
    if (node->directEffect && fn_iplDirectEffectRelease) fn_iplDirectEffectRelease(&node->directEffect);
    if (node->reflectionEffect && fn_iplReflectionEffectRelease) fn_iplReflectionEffectRelease(&node->reflectionEffect);
    if (node->pathEffect && fn_iplPathEffectRelease) fn_iplPathEffectRelease(&node->pathEffect);
    if (node->virtualSurroundEffect && fn_iplVirtualSurroundEffectRelease) fn_iplVirtualSurroundEffectRelease(&node->virtualSurroundEffect);
    if (node->panningEffect && fn_iplPanningEffectRelease) fn_iplPanningEffectRelease(&node->panningEffect);
    if (node->ambiEncodeEffect && fn_iplAmbisonicsEncodeEffectRelease) fn_iplAmbisonicsEncodeEffectRelease(&node->ambiEncodeEffect);
    if (node->ambiRotateEffect && fn_iplAmbisonicsRotationEffectRelease) fn_iplAmbisonicsRotationEffectRelease(&node->ambiRotateEffect);
    if (node->ambiBinauralEffect && fn_iplAmbisonicsBinauralEffectRelease) fn_iplAmbisonicsBinauralEffectRelease(&node->ambiBinauralEffect);
}

static void Node_Release(BASS_HRTF_Node *node) {
    if (!node) return;
    if (ATOMIC_DEC(&node->refCount) == 0) {
        Node_ReleaseEffects(node);
        if (node->ownedSampleChan && fn_BASS_StreamFree) {
            fn_BASS_StreamFree(node->ownedSampleChan);
            node->ownedSampleChan = 0;
        }
        free(node);
    }
}

static BASS_HRTF_Node* AcquireNode(DWORD handle) {
    if (!handle) return NULL;
    int limit = g_MaxNodeLimit;
    if (limit > MAX_HRTF_CHANNELS) limit = MAX_HRTF_CHANNELS;
    for (int i = 0; i < limit; ++i) {
        BASS_HRTF_Node *n = g_Nodes[i];
        if (n && n->isAlive) {
            if (n->handleKey == handle || n->sourceChan == handle || n->dspHandle == handle || n->ownedSampleChan == handle) {
                Node_AddRef(n);
                if (!n->isAlive || g_Nodes[i] != n) {
                    Node_Release(n);
                    return NULL;
                }
                return n;
            }
        }
    }
    return NULL;
}

static BOOL RegisterNodeLockFree(BASS_HRTF_Node *node) {
    node->isAlive = 1;
    for (int i = 0; i < MAX_HRTF_CHANNELS; ++i) {
        if (g_Nodes[i] == NULL) {
            if (ATOMIC_CAS_PTR(&g_Nodes[i], node, NULL)) {
                int curLimit = g_MaxNodeLimit;
                if (i + 1 > curLimit) {
                    g_MaxNodeLimit = i + 1;
                }
                return TRUE;
            }
        }
    }
    node->isAlive = 0;
    return FALSE;
}

static void UnregisterNodeLockFree(BASS_HRTF_Node *node) {
    if (!node) return;
    node->isAlive = 0;
    int limit = g_MaxNodeLimit;
    if (limit > MAX_HRTF_CHANNELS) limit = MAX_HRTF_CHANNELS;
    for (int i = 0; i < limit; ++i) {
        if (g_Nodes[i] == node) {
            if (ATOMIC_CAS_PTR(&g_Nodes[i], NULL, node)) {
                Node_Release(node);
                return;
            }
        }
    }
}

// ============================================================================
// TẢI THƯ VIỆN ĐỘNG ĐA NỀN TẢNG (WINDOWS / LINUX / ANDROID / MACOS / IOS)
// ============================================================================
static BOOL LoadAllSymbols(const char* customBassPath, const char* customPhononPath) {
    if (!g_BassLib) {
        g_BassLib = LOAD_LIB(customBassPath ? customBassPath : BASS_LIB_NAME);
#if defined(IS_IOS_PLATFORM)
        if (!g_BassLib && !customBassPath) g_BassLib = LOAD_LIB(BASS_LIB_ALT);
#endif
#if !defined(_WIN32) && !defined(_WIN64) && !defined(__CYGWIN__)
        if (!g_BassLib && GET_SYM(HRTF_RTLD_DEFAULT, "BASS_StreamCreate")) {
            g_BassLib = HRTF_RTLD_DEFAULT;
        }
#endif
    }
    if (!g_PhononLib) {
        g_PhononLib = LOAD_LIB(customPhononPath ? customPhononPath : PHONON_LIB_NAME);
#if defined(IS_IOS_PLATFORM)
        if (!g_PhononLib && !customPhononPath) g_PhononLib = LOAD_LIB(PHONON_LIB_ALT);
#endif
#if !defined(_WIN32) && !defined(_WIN64) && !defined(__CYGWIN__)
        if (!g_PhononLib && GET_SYM(HRTF_RTLD_DEFAULT, "iplBinauralEffectApply")) {
            g_PhononLib = HRTF_RTLD_DEFAULT;
        }
#endif
    }
    if (!g_BassLib || !g_PhononLib) return FALSE;

    #define BIND_SYM(target, lib, sym) *(void**)(&(target)) = GET_SYM(lib, sym)

    BIND_SYM(fn_BASS_SetConfig,           g_BassLib, "BASS_SetConfig");
    BIND_SYM(fn_BASS_ErrorGetCode,        g_BassLib, "BASS_ErrorGetCode");
    BIND_SYM(fn_BASS_ChannelGetInfo,      g_BassLib, "BASS_ChannelGetInfo");
    BIND_SYM(fn_BASS_ChannelIsActive,     g_BassLib, "BASS_ChannelIsActive");
    BIND_SYM(fn_BASS_ChannelFlags,        g_BassLib, "BASS_ChannelFlags");
    BIND_SYM(fn_BASS_StreamCreate,        g_BassLib, "BASS_StreamCreate");
    BIND_SYM(fn_BASS_StreamFree,          g_BassLib, "BASS_StreamFree");
    BIND_SYM(fn_BASS_StreamPutData,       g_BassLib, "BASS_StreamPutData");
    BIND_SYM(fn_BASS_ChannelGetData,      g_BassLib, "BASS_ChannelGetData");
    BIND_SYM(fn_BASS_ChannelSetSync,      g_BassLib, "BASS_ChannelSetSync");
    BIND_SYM(fn_BASS_ChannelRemoveSync,   g_BassLib, "BASS_ChannelRemoveSync");
    BIND_SYM(fn_BASS_ChannelSetDSP,       g_BassLib, "BASS_ChannelSetDSP");
    BIND_SYM(fn_BASS_ChannelRemoveDSP,    g_BassLib, "BASS_ChannelRemoveDSP");
    BIND_SYM(fn_BASS_ChannelSetAttribute, g_BassLib, "BASS_ChannelSetAttribute");
    BIND_SYM(fn_BASS_SampleGetInfo,       g_BassLib, "BASS_SampleGetInfo");
    BIND_SYM(fn_BASS_SampleGetChannel,    g_BassLib, "BASS_SampleGetChannel");

    BIND_SYM(fn_iplContextCreate,                   g_PhononLib, "iplContextCreate");
    BIND_SYM(fn_iplContextRelease,                  g_PhononLib, "iplContextRelease");
    BIND_SYM(fn_iplHRTFCreate,                      g_PhononLib, "iplHRTFCreate");
    BIND_SYM(fn_iplHRTFRelease,                     g_PhononLib, "iplHRTFRelease");
    BIND_SYM(fn_iplBinauralEffectCreate,            g_PhononLib, "iplBinauralEffectCreate");
    BIND_SYM(fn_iplBinauralEffectRelease,           g_PhononLib, "iplBinauralEffectRelease");
    BIND_SYM(fn_iplBinauralEffectReset,             g_PhononLib, "iplBinauralEffectReset");
    BIND_SYM(fn_iplBinauralEffectApply,             g_PhononLib, "iplBinauralEffectApply");
    BIND_SYM(fn_iplDirectEffectCreate,              g_PhononLib, "iplDirectEffectCreate");
    BIND_SYM(fn_iplDirectEffectRelease,             g_PhononLib, "iplDirectEffectRelease");
    BIND_SYM(fn_iplDirectEffectReset,               g_PhononLib, "iplDirectEffectReset");
    BIND_SYM(fn_iplDirectEffectApply,               g_PhononLib, "iplDirectEffectApply");
    BIND_SYM(fn_iplReflectionEffectCreate,          g_PhononLib, "iplReflectionEffectCreate");
    BIND_SYM(fn_iplReflectionEffectRelease,         g_PhononLib, "iplReflectionEffectRelease");
    BIND_SYM(fn_iplReflectionEffectReset,           g_PhononLib, "iplReflectionEffectReset");
    BIND_SYM(fn_iplReflectionEffectApply,           g_PhononLib, "iplReflectionEffectApply");
    BIND_SYM(fn_iplPathEffectCreate,                g_PhononLib, "iplPathEffectCreate");
    BIND_SYM(fn_iplPathEffectRelease,               g_PhononLib, "iplPathEffectRelease");
    BIND_SYM(fn_iplPathEffectReset,                 g_PhononLib, "iplPathEffectReset");
    BIND_SYM(fn_iplPathEffectApply,                 g_PhononLib, "iplPathEffectApply");
    BIND_SYM(fn_iplVirtualSurroundEffectCreate,     g_PhononLib, "iplVirtualSurroundEffectCreate");
    BIND_SYM(fn_iplVirtualSurroundEffectRelease,    g_PhononLib, "iplVirtualSurroundEffectRelease");
    BIND_SYM(fn_iplVirtualSurroundEffectReset,      g_PhononLib, "iplVirtualSurroundEffectReset");
    BIND_SYM(fn_iplVirtualSurroundEffectApply,      g_PhononLib, "iplVirtualSurroundEffectApply");
    BIND_SYM(fn_iplPanningEffectCreate,             g_PhononLib, "iplPanningEffectCreate");
    BIND_SYM(fn_iplPanningEffectRelease,            g_PhononLib, "iplPanningEffectRelease");
    BIND_SYM(fn_iplPanningEffectReset,              g_PhononLib, "iplPanningEffectReset");
    BIND_SYM(fn_iplPanningEffectApply,              g_PhononLib, "iplPanningEffectApply");
    BIND_SYM(fn_iplAmbisonicsEncodeEffectCreate,    g_PhononLib, "iplAmbisonicsEncodeEffectCreate");
    BIND_SYM(fn_iplAmbisonicsEncodeEffectRelease,   g_PhononLib, "iplAmbisonicsEncodeEffectRelease");
    BIND_SYM(fn_iplAmbisonicsEncodeEffectReset,     g_PhononLib, "iplAmbisonicsEncodeEffectReset");
    BIND_SYM(fn_iplAmbisonicsEncodeEffectApply,     g_PhononLib, "iplAmbisonicsEncodeEffectApply");
    BIND_SYM(fn_iplAmbisonicsRotationEffectCreate,  g_PhononLib, "iplAmbisonicsRotationEffectCreate");
    BIND_SYM(fn_iplAmbisonicsRotationEffectRelease, g_PhononLib, "iplAmbisonicsRotationEffectRelease");
    BIND_SYM(fn_iplAmbisonicsRotationEffectReset,   g_PhononLib, "iplAmbisonicsRotationEffectReset");
    BIND_SYM(fn_iplAmbisonicsRotationEffectApply,   g_PhononLib, "iplAmbisonicsRotationEffectApply");
    BIND_SYM(fn_iplAmbisonicsBinauralEffectCreate,  g_PhononLib, "iplAmbisonicsBinauralEffectCreate");
    BIND_SYM(fn_iplAmbisonicsBinauralEffectRelease, g_PhononLib, "iplAmbisonicsBinauralEffectRelease");
    BIND_SYM(fn_iplAmbisonicsBinauralEffectReset,   g_PhononLib, "iplAmbisonicsBinauralEffectReset");
    BIND_SYM(fn_iplAmbisonicsBinauralEffectApply,   g_PhononLib, "iplAmbisonicsBinauralEffectApply");

    BIND_SYM(fn_iplAudioBufferDeinterleave,         g_PhononLib, "iplAudioBufferDeinterleave");
    BIND_SYM(fn_iplAudioBufferInterleave,           g_PhononLib, "iplAudioBufferInterleave");
    BIND_SYM(fn_iplAudioBufferDownmix,              g_PhononLib, "iplAudioBufferDownmix");
    BIND_SYM(fn_iplAudioBufferMix,                  g_PhononLib, "iplAudioBufferMix");
    BIND_SYM(fn_iplCalculateRelativeDirection,      g_PhononLib, "iplCalculateRelativeDirection");
    BIND_SYM(fn_iplDistanceAttenuationCalculate,    g_PhononLib, "iplDistanceAttenuationCalculate");
    BIND_SYM(fn_iplAirAbsorptionCalculate,          g_PhononLib, "iplAirAbsorptionCalculate");
    BIND_SYM(fn_iplDirectivityCalculate,            g_PhononLib, "iplDirectivityCalculate");

    return (fn_BASS_StreamCreate && fn_BASS_ChannelSetDSP && fn_iplBinauralEffectApply);
}

// ============================================================================
// RAYCAST AABB SIÊU NHANH (Lock-Free Occlusion & Material Transmission)
// ============================================================================
static int RayIntersectsAABB(float ox, float oy, float oz, float dx, float dy, float dz, float maxDist,
                             float minX, float maxX, float minY, float maxY, float minZ, float maxZ) {
    float tmin = 0.0f;
    float tmax = maxDist;
    float invD, t0, t1, tmp;

    if (fabsf(dx) < 1e-6f) {
        if (ox < minX || ox > maxX) return 0;
    } else {
        invD = 1.0f / dx;
        t0 = (minX - ox) * invD;
        t1 = (maxX - ox) * invD;
        if (invD < 0.0f) { tmp = t0; t0 = t1; t1 = tmp; }
        if (t0 > tmin) tmin = t0;
        if (t1 < tmax) tmax = t1;
        if (tmax <= tmin) return 0;
    }
    if (fabsf(dy) < 1e-6f) {
        if (oy < minY || oy > maxY) return 0;
    } else {
        invD = 1.0f / dy;
        t0 = (minY - oy) * invD;
        t1 = (maxY - oy) * invD;
        if (invD < 0.0f) { tmp = t0; t0 = t1; t1 = tmp; }
        if (t0 > tmin) tmin = t0;
        if (t1 < tmax) tmax = t1;
        if (tmax <= tmin) return 0;
    }
    if (fabsf(dz) < 1e-6f) {
        if (oz < minZ || oz > maxZ) return 0;
    } else {
        invD = 1.0f / dz;
        t0 = (minZ - oz) * invD;
        t1 = (maxZ - oz) * invD;
        if (invD < 0.0f) { tmp = t0; t0 = t1; t1 = tmp; }
        if (t0 > tmin) tmin = t0;
        if (t1 < tmax) tmax = t1;
        if (tmax <= tmin) return 0;
    }
    return 1;
}

static void ComputeEnvRaycastForNode(BASS_HRTF_Node *node, float srcX, float srcY, float srcZ, float lstX, float lstY, float lstZ) {
    int count = g_EnvBoxCount;
    if (count <= 0) return;

    float dx = srcX - lstX;
    float dy = srcY - lstY;
    float dz = srcZ - lstZ;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (dist < 0.001f) {
        node->occlusion = 0.0f;
        return;
    }

    float invDist = 1.0f / dist;
    dx *= invDist; dy *= invDist; dz *= invDist;

    int hits = 0;
    float transL = 1.0f, transM = 1.0f, transH = 1.0f;

    for (int i = 0; i < count; ++i) {
        if (!g_EnvBoxes[i].active) continue;
        if (RayIntersectsAABB(lstX, lstY, lstZ, dx, dy, dz, dist,
                              g_EnvBoxes[i].minX, g_EnvBoxes[i].maxX,
                              g_EnvBoxes[i].minY, g_EnvBoxes[i].maxY,
                              g_EnvBoxes[i].minZ, g_EnvBoxes[i].maxZ)) {
            hits++;
            transL *= g_EnvBoxes[i].mat.transmission[0];
            transM *= g_EnvBoxes[i].mat.transmission[1];
            transH *= g_EnvBoxes[i].mat.transmission[2];
        }
    }

    if (hits > 0) {
        node->occlusion = (hits >= 3) ? 1.0f : (0.4f + 0.3f * (float)(hits - 1));
        node->transLow  = transL;
        node->transMid  = transM;
        node->transHigh = transH;
    } else {
        node->occlusion = 0.0f;
        node->transLow  = node->baseTransLow;
        node->transMid  = node->baseTransMid;
        node->transHigh = node->baseTransHigh;
    }
}

// ============================================================================
// CẬP NHẬT TỌA ĐỘ 3D & TÍNH HƯỚNG
// ============================================================================
static void UpdateNode3DCoordinatesEx(BASS_HRTF_Node *node, float relX, float relY, float relZ, float listenerYawDeg, float listenerPitchDeg, int positionChanged) {
    node->relX = relX;
    node->relY = relY;
    node->relZ = relZ;

    float rawDist = sqrtf(relX * relX + relY * relY + relZ * relZ);
    node->distanceUnits = rawDist;

    float effDist = rawDist;
    float sRad = node->sourceRadius;
    if (sRad > 0.0001f) {
        effDist = (rawDist > sRad) ? (rawDist - sRad) : 0.0f;
    }
    node->effectiveDist = effDist;

    float minD = node->minDist;
    float maxD = (node->maxDist > minD + 0.001f) ? node->maxDist : (minD + 1.0f);
    float atten = 1.0f;

    if (effDist >= maxD) {
        node->distAtten = 0.0f;
        node->gainAtten = 0.0f;
        return;
    } else if (effDist > minD) {
        float excess = effDist - minD;
        atten = 1.0f / (1.0f + node->rolloff * excess);
        float linearFade = 1.0f - (excess / (maxD - minD));
        if (linearFade < 0.0f) linearFade = 0.0f;
        atten *= linearFade;
    }
    node->distAtten = atten;
    node->gainAtten = atten * node->userGain;

    float blend = effDist * node->blendStep;
    float maxB  = node->maxSpatialBlend;
    if (blend > maxB) blend = maxB;
    if (blend < 0.0f) blend = 0.0f;
    node->spatialBlend = blend;

    if (rawDist > 0.0001f) {
        float yawRad   = listenerYawDeg * (3.1415926535f / 180.0f);
        float pitchRad = listenerPitchDeg * (3.1415926535f / 180.0f);

        if (fn_iplCalculateRelativeDirection && g_Context) {
            IPLVector3 srcPos = { relX, relZ, -relY };
            IPLVector3 lstPos = { 0.0f, 0.0f, 0.0f };

            float cosP = cosf(pitchRad);
            float sinP = sinf(pitchRad);
            float cosY = cosf(yawRad);
            float sinY = sinf(yawRad);

            IPLVector3 ahead = { sinY * cosP, sinP, -cosY * cosP };
            IPLVector3 up    = { -sinY * sinP, cosP, cosY * sinP };

            IPLVector3 dir = fn_iplCalculateRelativeDirection(g_Context, srcPos, lstPos, ahead, up);
            node->dirX = dir.x;
            node->dirY = dir.y;
            node->dirZ = dir.z;
        } else {
            float rx = relX, ry = relY, rz = relZ;
            if (fabsf(listenerYawDeg) > 0.001f) {
                float cosA = cosf(yawRad), sinA = sinf(yawRad);
                float rotX = cosA * rx - sinA * ry;
                float rotY = sinA * rx + cosA * ry;
                rx = rotX; ry = rotY;
            }
            if (fabsf(listenerPitchDeg) > 0.001f) {
                float cosP = cosf(pitchRad), sinP = sinf(pitchRad);
                float rotY = cosP * ry + sinP * rz;
                float rotZ = -sinP * ry + cosP * rz;
                ry = rotY; rz = rotZ;
            }
            float invRaw = 1.0f / rawDist;
            node->dirX = rx * invRaw;
            node->dirY = rz * invRaw;
            node->dirZ = -ry * invRaw;
        }

        if (node->useDipoleCalc && fn_iplDirectivityCalculate && g_Context) {
            IPLCoordinateSpace3 srcSpace;
            srcSpace.ahead.x  = node->srcAheadX;
            srcSpace.ahead.y  = node->srcAheadZ;
            srcSpace.ahead.z  = -node->srcAheadY;
            srcSpace.up.x     = 0.0f; srcSpace.up.y = 1.0f; srcSpace.up.z = 0.0f;
            srcSpace.right.x  = 1.0f; srcSpace.right.y = 0.0f; srcSpace.right.z = 0.0f;
            srcSpace.origin.x = relX; srcSpace.origin.y = relZ; srcSpace.origin.z = -relY;
            IPLVector3 lstOrigin = { 0.0f, 0.0f, 0.0f };
            IPLDirectivity dirModel = { node->dipoleWeight, node->dipolePower, NULL, NULL };
            node->directivity = fn_iplDirectivityCalculate(g_Context, srcSpace, lstOrigin, &dirModel);
        }
    } else {
        node->dirX = 0.0f;
        node->dirY = 0.0f;
        node->dirZ = -1.0f;
    }

    if (positionChanged && g_AutoEnvRaycast && node->useWorldPos && node->useDirectFilter) {
        ComputeEnvRaycastForNode(node, node->worldX, node->worldY, node->worldZ, g_ListenerX, g_ListenerY, g_ListenerZ);
    }
}

// ============================================================================
// LAZY INITIALIZATION CHO CÁC HIỆU ỨNG STEAMAUDIO
// ============================================================================
static void UpdateNodeBufferDescriptors(BASS_HRTF_Node *node, int fs) {
    node->inStereoBuf.numChannels = 2;
    node->inStereoBuf.numSamples  = fs;
    node->inMonoBuf.numChannels   = 1;
    node->inMonoBuf.numSamples    = fs;
    node->procBuf.numChannels     = 1;
    node->procBuf.numSamples      = fs;
    node->revBuf.numChannels      = 1;
    node->revBuf.numSamples       = fs;
    node->outBuf.numChannels      = 2;
    node->outBuf.numSamples       = fs;
    node->auxBuf.numChannels      = 2;
    node->auxBuf.numSamples       = fs;
    int ambiChans = (node->ambiOrder + 1) * (node->ambiOrder + 1);
    if (ambiChans > MAX_AMBI_CHANNELS) ambiChans = MAX_AMBI_CHANNELS;
    node->ambiBuf.numChannels     = ambiChans;
    node->ambiBuf.numSamples      = fs;
    node->ambiRotBuf.numChannels  = ambiChans;
    node->ambiRotBuf.numSamples   = fs;
}

static BOOL EnsureDirectEffectCreated(BASS_HRTF_Node *node) {
    if (node->directEffect) return TRUE;
    if (!fn_iplDirectEffectCreate || !g_Context) return FALSE;
    IPLAudioSettings audioSettings = { (int32_t)g_SampleRate, node->frameSize };
    IPLDirectEffectSettings dirSettings = { 1 };
    IPLDirectEffect newEffect = NULL;
    if (fn_iplDirectEffectCreate(g_Context, &audioSettings, &dirSettings, &newEffect) != IPL_STATUS_SUCCESS) {
        return FALSE;
    }
    if (!ATOMIC_CAS_PTR(&node->directEffect, newEffect, NULL)) {
        if (fn_iplDirectEffectRelease) fn_iplDirectEffectRelease(&newEffect);
    }
    return TRUE;
}

static BOOL EnsureReflectionEffectCreated(BASS_HRTF_Node *node) {
    if (node->reflectionEffect) return TRUE;
    if (!fn_iplReflectionEffectCreate || !g_Context) return FALSE;
    IPLAudioSettings audioSettings = { (int32_t)g_SampleRate, node->frameSize };
    IPLReflectionEffectSettings refSettings;
    memset(&refSettings, 0, sizeof(refSettings));
    refSettings.type        = IPL_REFLECTIONEFFECTTYPE_PARAMETRIC;
    refSettings.irSize      = g_SampleRate * 2;
    refSettings.numChannels = 1;
    IPLReflectionEffect newEffect = NULL;
    if (fn_iplReflectionEffectCreate(g_Context, &audioSettings, &refSettings, &newEffect) != IPL_STATUS_SUCCESS) {
        return FALSE;
    }
    if (!ATOMIC_CAS_PTR(&node->reflectionEffect, newEffect, NULL)) {
        if (fn_iplReflectionEffectRelease) fn_iplReflectionEffectRelease(&newEffect);
    }
    return TRUE;
}

static BOOL EnsureAmbisonicsCreated(BASS_HRTF_Node *node) {
    if (node->ambiEncodeEffect && node->ambiRotateEffect && node->ambiBinauralEffect) return TRUE;
    if (!fn_iplAmbisonicsEncodeEffectCreate || !fn_iplAmbisonicsRotationEffectCreate || !fn_iplAmbisonicsBinauralEffectCreate || !g_Context) return FALSE;
    IPLAudioSettings audioSettings = { (int32_t)g_SampleRate, node->frameSize };
    int maxOrder = 3;

    if (!node->ambiEncodeEffect) {
        IPLAmbisonicsEncodeEffectSettings encSet = { maxOrder };
        IPLAmbisonicsEncodeEffect eff = NULL;
        if (fn_iplAmbisonicsEncodeEffectCreate(g_Context, &audioSettings, &encSet, &eff) == IPL_STATUS_SUCCESS) {
            if (!ATOMIC_CAS_PTR(&node->ambiEncodeEffect, eff, NULL) && fn_iplAmbisonicsEncodeEffectRelease) {
                fn_iplAmbisonicsEncodeEffectRelease(&eff);
            }
        }
    }
    if (!node->ambiRotateEffect) {
        IPLAmbisonicsRotationEffectSettings rotSet = { maxOrder };
        IPLAmbisonicsRotationEffect eff = NULL;
        if (fn_iplAmbisonicsRotationEffectCreate(g_Context, &audioSettings, &rotSet, &eff) == IPL_STATUS_SUCCESS) {
            if (!ATOMIC_CAS_PTR(&node->ambiRotateEffect, eff, NULL) && fn_iplAmbisonicsRotationEffectRelease) {
                fn_iplAmbisonicsRotationEffectRelease(&eff);
            }
        }
    }
    if (!node->ambiBinauralEffect) {
        IPLAmbisonicsBinauralEffectSettings binSet = { g_HRTF, maxOrder };
        IPLAmbisonicsBinauralEffect eff = NULL;
        if (fn_iplAmbisonicsBinauralEffectCreate(g_Context, &audioSettings, &binSet, &eff) == IPL_STATUS_SUCCESS) {
            if (!ATOMIC_CAS_PTR(&node->ambiBinauralEffect, eff, NULL) && fn_iplAmbisonicsBinauralEffectRelease) {
                fn_iplAmbisonicsBinauralEffectRelease(&eff);
            }
        }
    }
    return (node->ambiEncodeEffect && node->ambiRotateEffect && node->ambiBinauralEffect);
}

static BOOL EnsurePathEffectCreated(BASS_HRTF_Node *node) {
    if (node->pathEffect && EnsureAmbisonicsCreated(node)) return TRUE;
    if (!fn_iplPathEffectCreate || !g_Context) return FALSE;
    EnsureAmbisonicsCreated(node);
    IPLAudioSettings audioSettings = { (int32_t)g_SampleRate, node->frameSize };
    IPLPathEffectSettings pathSet;
    memset(&pathSet, 0, sizeof(pathSet));
    pathSet.maxOrder = 1;
    pathSet.spatialize = 0;
    pathSet.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
    pathSet.hrtf = g_HRTF;
    IPLPathEffect newEffect = NULL;
    if (fn_iplPathEffectCreate(g_Context, &audioSettings, &pathSet, &newEffect) != IPL_STATUS_SUCCESS) {
        return FALSE;
    }
    if (!ATOMIC_CAS_PTR(&node->pathEffect, newEffect, NULL)) {
        if (fn_iplPathEffectRelease) fn_iplPathEffectRelease(&newEffect);
    }
    return TRUE;
}

static BOOL EnsureVirtualSurroundCreated(BASS_HRTF_Node *node) {
    if (node->virtualSurroundEffect) return TRUE;
    if (!fn_iplVirtualSurroundEffectCreate || !g_Context) return FALSE;
    IPLAudioSettings audioSettings = { (int32_t)g_SampleRate, node->frameSize };
    IPLVirtualSurroundEffectSettings vsSet;
    memset(&vsSet, 0, sizeof(vsSet));
    vsSet.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
    vsSet.hrtf = g_HRTF;
    IPLVirtualSurroundEffect newEffect = NULL;
    if (fn_iplVirtualSurroundEffectCreate(g_Context, &audioSettings, &vsSet, &newEffect) != IPL_STATUS_SUCCESS) {
        return FALSE;
    }
    if (!ATOMIC_CAS_PTR(&node->virtualSurroundEffect, newEffect, NULL)) {
        if (fn_iplVirtualSurroundEffectRelease) fn_iplVirtualSurroundEffectRelease(&newEffect);
    }
    return TRUE;
}

static BOOL EnsurePanningEffectCreated(BASS_HRTF_Node *node) {
    if (node->panningEffect) return TRUE;
    if (!fn_iplPanningEffectCreate || !g_Context) return FALSE;
    IPLAudioSettings audioSettings = { (int32_t)g_SampleRate, node->frameSize };
    IPLPanningEffectSettings panSet;
    memset(&panSet, 0, sizeof(panSet));
    panSet.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
    IPLPanningEffect newEffect = NULL;
    if (fn_iplPanningEffectCreate(g_Context, &audioSettings, &panSet, &newEffect) != IPL_STATUS_SUCCESS) {
        return FALSE;
    }
    if (!ATOMIC_CAS_PTR(&node->panningEffect, newEffect, NULL)) {
        if (fn_iplPanningEffectRelease) fn_iplPanningEffectRelease(&newEffect);
    }
    return TRUE;
}

static void ResetAllNodeEffects(BASS_HRTF_Node *node) {
    if (!node) return;
    if (node->binauralEffect && fn_iplBinauralEffectReset) fn_iplBinauralEffectReset(node->binauralEffect);
    if (node->directEffect && fn_iplDirectEffectReset) fn_iplDirectEffectReset(node->directEffect);
    if (node->reflectionEffect && fn_iplReflectionEffectReset) fn_iplReflectionEffectReset(node->reflectionEffect);
    if (node->pathEffect && fn_iplPathEffectReset) fn_iplPathEffectReset(node->pathEffect);
    if (node->virtualSurroundEffect && fn_iplVirtualSurroundEffectReset) fn_iplVirtualSurroundEffectReset(node->virtualSurroundEffect);
    if (node->panningEffect && fn_iplPanningEffectReset) fn_iplPanningEffectReset(node->panningEffect);
    if (node->ambiEncodeEffect && fn_iplAmbisonicsEncodeEffectReset) fn_iplAmbisonicsEncodeEffectReset(node->ambiEncodeEffect);
    if (node->ambiRotateEffect && fn_iplAmbisonicsRotationEffectReset) fn_iplAmbisonicsRotationEffectReset(node->ambiRotateEffect);
    if (node->ambiBinauralEffect && fn_iplAmbisonicsBinauralEffectReset) fn_iplAmbisonicsBinauralEffectReset(node->ambiBinauralEffect);
    memset(node->erDelayBufL, 0, sizeof(node->erDelayBufL));
    memset(node->erDelayBufR, 0, sizeof(node->erDelayBufR));
    node->erLpStateL = 0.0f;
    node->erLpStateR = 0.0f;
    node->erWriteIdx = 0;
}

static void RebuildNodeForFrameSize(BASS_HRTF_Node *node, int newFs) {
    newFs = SanitizeFrameSize(newFs);
    if (node->frameSize == newFs && node->binauralEffect) return;

    Node_ReleaseEffects(node);
    node->frameSize = newFs;
    UpdateNodeBufferDescriptors(node, newFs);

    IPLAudioSettings audioSettings = { (int32_t)g_SampleRate, newFs };
    IPLBinauralEffectSettings binSettings = { g_HRTF };
    fn_iplBinauralEffectCreate(g_Context, &audioSettings, &binSettings, &node->binauralEffect);

    if (node->useDirectFilter) EnsureDirectEffectCreated(node);
    if (node->useReflectionEffect) EnsureReflectionEffectCreated(node);
    if (node->usePathEffect) EnsurePathEffectCreated(node);
    if (node->useVirtualSurround || node->panningMode == BASS_HRTF_MODE_VIRTUAL_SURROUND) EnsureVirtualSurroundCreated(node);
    if (node->panningMode == BASS_HRTF_MODE_PANNING) EnsurePanningEffectCreated(node);
    if (node->useAmbisonics || node->panningMode == BASS_HRTF_MODE_AMBISONICS) EnsureAmbisonicsCreated(node);

    node->ringInFrames = 0;
    node->ringOutReadPos = 0;
    node->ringOutWritePos = 0;
    node->dspFifoPrimed = 0;
}

// ============================================================================
// CẬP NHẬT HIỆU ỨNG TRƯỢT (SLIDE ANIMATIONS)
// ============================================================================
static void UpdateSlideAnimations(BASS_HRTF_Node *node) {
    float dtMs = (float)node->frameSize * 1000.0f / (float)(node->sampleRate > 0 ? node->sampleRate : 44100);

    if (node->slidePosActive) {
        node->slidePosElapsedMs += dtMs;
        float t = (node->slidePosTotalMs > 0.0f) ? (node->slidePosElapsedMs / node->slidePosTotalMs) : 1.0f;
        if (t >= 1.0f) {
            t = 1.0f;
            node->slidePosActive = 0;
        }
        float curX = node->slideStartX + (node->slideTargetX - node->slideStartX) * t;
        float curY = node->slideStartY + (node->slideTargetY - node->slideStartY) * t;
        float curZ = node->slideStartZ + (node->slideTargetZ - node->slideStartZ) * t;

        if (node->useWorldPos) {
            node->worldX = curX;
            node->worldY = curY;
            node->worldZ = curZ;
            UpdateNode3DCoordinatesEx(node, curX - g_ListenerX, curY - g_ListenerY, curZ - g_ListenerZ, g_ListenerYaw, g_ListenerPitch, 1);
        } else {
            UpdateNode3DCoordinatesEx(node, curX, curY, curZ, g_ListenerYaw, g_ListenerPitch, 1);
        }
    }

    if (node->slideAttrActive) {
        node->slideAttrElapsedMs += dtMs;
        float t = (node->slideAttrTotalMs > 0.0f) ? (node->slideAttrElapsedMs / node->slideAttrTotalMs) : 1.0f;
        if (t >= 1.0f) {
            t = 1.0f;
            node->slideAttrActive = 0;
        }
        float curVal = node->slideAttrStart + (node->slideAttrTarget - node->slideAttrStart) * t;

        if (node->slideAttrType == 1) {
            node->occlusion = (curVal < 0.0f) ? 0.0f : ((curVal > 1.0f) ? 1.0f : curVal);
        } else if (node->slideAttrType == 2) {
            float maxB = (curVal < 0.0f) ? 0.0f : ((curVal > 1.0f) ? 1.0f : curVal);
            node->maxSpatialBlend = maxB;
            float blend = node->effectiveDist * node->blendStep;
            if (blend > maxB) blend = maxB;
            if (blend < 0.0f) blend = 0.0f;
            node->spatialBlend = blend;
        } else if (node->slideAttrType == 3) {
            node->userGain = (curVal < 0.0f) ? 0.0f : curVal;
            node->gainAtten = node->distAtten * node->userGain;
        } else if (node->slideAttrType == 4) {
            node->airAbsorption = (curVal < 0.0f) ? 0.0f : curVal;
        } else if (node->slideAttrType == 5) {
            node->reverbWetMix = (curVal < 0.0f) ? 0.0f : ((curVal > 1.0f) ? 1.0f : curVal);
        }
    }
}

// ============================================================================
// CORE PROCESSING PIPELINE (DYNAMIC FRAME SIZE + FULL STEAMAUDIO FX)
// ============================================================================
static inline float FastSoftClip(float x) {
    if (x > 0.95f) {
        float d = (x - 0.95f) * 20.0f;
        return 0.95f + 0.05f * (d / (1.0f + d));
    } else if (x < -0.95f) {
        float d = (-x - 0.95f) * 20.0f;
        return -0.95f - 0.05f * (d / (1.0f + d));
    }
    return x;
}

static void ApplyEarlyReflectionsAndReverb(BASS_HRTF_Node *node, const float *monoIn, float *outL, float *outR, int fs) {
    float wet = node->reverbWetMix;
    if (wet <= 0.001f) return;

    float rtLow  = node->reverbTimes[0];
    float rtMid  = node->reverbTimes[1];
    float rtHigh = node->reverbTimes[2];
    float eqLow  = node->reverbEq[0];
    float eqMid  = node->reverbEq[1];
    float eqHigh = node->reverbEq[2];
    int delaySamples = (int)(node->reverbDelayMs * 0.001f * (float)g_SampleRate);
    if (delaySamples < 16) delaySamples = 16;
    if (delaySamples > 1200) delaySamples = 1200;

    // 1. Gọi SteamAudio Parametric ReflectionEffect (nếu có)
    int hasSteamParametric = 0;
    if (node->reflectionEffect && fn_iplReflectionEffectApply) {
        IPLReflectionEffectParams refParams;
        memset(&refParams, 0, sizeof(refParams));
        refParams.type           = IPL_REFLECTIONEFFECTTYPE_PARAMETRIC;
        refParams.reverbTimes[0] = rtLow;
        refParams.reverbTimes[1] = rtMid;
        refParams.reverbTimes[2] = rtHigh;
        refParams.eq[0]          = eqLow;
        refParams.eq[1]          = eqMid;
        refParams.eq[2]          = eqHigh;
        refParams.delay          = delaySamples;
        refParams.irSize         = g_SampleRate * 2;
        refParams.numChannels    = 1;

        IPLAudioBuffer inB = { 1, fs, (float**)&monoIn };
        fn_iplReflectionEffectApply(node->reflectionEffect, &refParams, &inB, &node->revBuf, NULL);
        hasSteamParametric = 1;
    }

    // 2. Kết hợp Stereo Early Reflections & FDN Tail cực nhẹ (O(1) per sample, 0% lag)
    int tap1L = (delaySamples) & REVERB_DELAY_LINE_MASK;
    int tap2R = (int)(delaySamples * 1.37f) & REVERB_DELAY_LINE_MASK;
    int tap3L = (int)(delaySamples * 1.93f) & REVERB_DELAY_LINE_MASK;
    int tap4R = (int)(delaySamples * 2.51f) & REVERB_DELAY_LINE_MASK;

    float decay = 0.25f + 0.30f * (rtMid / (1.0f + rtMid));
    if (decay > 0.68f) decay = 0.68f;
    float damp = 0.35f + 0.45f * (eqHigh / (0.5f + eqLow));
    if (damp > 0.85f) damp = 0.85f;
    if (damp < 0.10f) damp = 0.10f;

    float toneGain = (eqLow * 0.35f + eqMid * 0.45f + eqHigh * 0.20f) * wet * 0.45f;
    int wIdx = node->erWriteIdx;
    float lpL = node->erLpStateL;
    float lpR = node->erLpStateR;

    for (int i = 0; i < fs; ++i) {
        float s = monoIn[i] + (hasSteamParametric ? node->reverbMonoFrame[i] * 0.5f : 0.0f);
        float e1 = node->erDelayBufL[(wIdx - tap1L) & REVERB_DELAY_LINE_MASK];
        float e2 = node->erDelayBufR[(wIdx - tap2R) & REVERB_DELAY_LINE_MASK];
        float e3 = node->erDelayBufL[(wIdx - tap3L) & REVERB_DELAY_LINE_MASK];
        float e4 = node->erDelayBufR[(wIdx - tap4R) & REVERB_DELAY_LINE_MASK];

        float revL = e1 * 0.65f + e3 * 0.35f;
        float revR = e2 * 0.65f + e4 * 0.35f;

        lpL = lpL + damp * (revL - lpL);
        lpR = lpR + damp * (revR - lpR);

        node->erDelayBufL[wIdx] = s + lpR * decay;
        node->erDelayBufR[wIdx] = s + lpL * decay;
        wIdx = (wIdx + 1) & REVERB_DELAY_LINE_MASK;

        outL[i] += lpL * toneGain;
        outR[i] += lpR * toneGain;
    }

    node->erWriteIdx = wIdx;
    node->erLpStateL = lpL;
    node->erLpStateR = lpR;
}

static void ProcessOneFrame_StereoIn(BASS_HRTF_Node *node, float *interleavedStereoInOut) {
    int targetFs = node->targetFrameSize > 0 ? node->targetFrameSize : g_FrameSize;
    if (targetFs != node->frameSize) {
        RebuildNodeForFrameSize(node, targetFs);
    }
    int fs = node->frameSize;
    UpdateNodeBufferDescriptors(node, fs);
    UpdateSlideAnimations(node);

    int mode = node->panningMode;
    int useDirect = (node->useDirectFilter && node->directEffect && fn_iplDirectEffectApply) ? 1 : 0;
    int needMonoDownmix = (!node->useStereoInput || useDirect || mode == BASS_HRTF_MODE_PANNING ||
                           mode == BASS_HRTF_MODE_AMBISONICS || node->useAmbisonics ||
                           node->useReflectionEffect || node->usePathEffect) ? 1 : 0;

    // 1. Deinterleave & Downmix bằng SIMD của SteamAudio
    if (interleavedStereoInOut) {
        if (fn_iplAudioBufferDeinterleave && g_Context) {
            fn_iplAudioBufferDeinterleave(g_Context, interleavedStereoInOut, &node->inStereoBuf);
            if (needMonoDownmix && fn_iplAudioBufferDownmix) {
                fn_iplAudioBufferDownmix(g_Context, &node->inStereoBuf, &node->inMonoBuf);
            }
        } else {
            for (int i = 0; i < fs; ++i) {
                float L = interleavedStereoInOut[i * 2];
                float R = interleavedStereoInOut[i * 2 + 1];
                node->inLeftFrame[i]  = L;
                node->inRightFrame[i] = R;
                node->inMonoFrame[i]  = (L + R) * 0.5f;
            }
        }
    }

    IPLAudioBuffer *currentMonoBuf = &node->inMonoBuf;
    IPLAudioBuffer *inputForBinaural = (!needMonoDownmix && interleavedStereoInOut)
                                       ? &node->inStereoBuf
                                       : &node->inMonoBuf;

    // 2. Bộ lọc Vật lý Trực tiếp (Air Absorption, Directivity, Occlusion, 3-Band Transmission)
    if (useDirect) {
        IPLDirectEffectParams directParams;
        memset(&directParams, 0, sizeof(directParams));
        directParams.flags = IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION |
                             IPL_DIRECTEFFECTFLAGS_APPLYDIRECTIVITY |
                             IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION |
                             IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION;
        directParams.transmissionType    = IPL_TRANSMISSIONTYPE_FREQDEPENDENT;
        directParams.distanceAttenuation = 1.0f;
        directParams.directivity         = node->directivity;

        float dist = node->distanceUnits;
        float airFactor = node->airAbsorption;
        if (fn_iplAirAbsorptionCalculate && g_Context) {
            IPLVector3 srcV = { dist * airFactor, 0.0f, 0.0f };
            IPLVector3 lstV = { 0.0f, 0.0f, 0.0f };
            IPLAirAbsorptionModel airModel;
            memset(&airModel, 0, sizeof(airModel));
            airModel.type = 1; // Exponential
            airModel.coefficients[0] = node->airAbsCoeffLow;
            airModel.coefficients[1] = node->airAbsCoeffMid;
            airModel.coefficients[2] = node->airAbsCoeffHigh;
            fn_iplAirAbsorptionCalculate(g_Context, srcV, lstV, &airModel, directParams.airAbsorption);
        } else {
            directParams.airAbsorption[0] = expf(-node->airAbsCoeffLow  * dist * airFactor);
            directParams.airAbsorption[1] = expf(-node->airAbsCoeffMid  * dist * airFactor);
            directParams.airAbsorption[2] = expf(-node->airAbsCoeffHigh * dist * airFactor);
        }

        directParams.occlusion       = node->occlusion;
        directParams.transmission[0] = node->transLow;
        directParams.transmission[1] = node->transMid;
        directParams.transmission[2] = node->transHigh;

        fn_iplDirectEffectApply(node->directEffect, &directParams, &node->inMonoBuf, &node->procBuf);
        currentMonoBuf   = &node->procBuf;
        inputForBinaural = &node->procBuf;
    }

    // 3. Spatialization theo chế độ: Binaural / Panning / Ambisonics / VirtualSurround
    if ((mode == BASS_HRTF_MODE_VIRTUAL_SURROUND || node->useVirtualSurround) &&
        node->virtualSurroundEffect && fn_iplVirtualSurroundEffectApply) {
        IPLAudioBuffer *vsIn = interleavedStereoInOut ? &node->inStereoBuf : currentMonoBuf;
        if (vsIn->numChannels == 1) {
            memcpy(node->inLeftFrame, currentMonoBuf->data[0], (size_t)fs * sizeof(float));
            memcpy(node->inRightFrame, currentMonoBuf->data[0], (size_t)fs * sizeof(float));
            vsIn = &node->inStereoBuf;
        }
        IPLVirtualSurroundEffectParams vsParams = { g_HRTF };
        fn_iplVirtualSurroundEffectApply(node->virtualSurroundEffect, &vsParams, vsIn, &node->outBuf);
    } else if (mode == BASS_HRTF_MODE_PANNING && node->panningEffect && fn_iplPanningEffectApply) {
        IPLPanningEffectParams panParams;
        panParams.direction.x = node->dirX;
        panParams.direction.y = node->dirY;
        panParams.direction.z = node->dirZ;
        fn_iplPanningEffectApply(node->panningEffect, &panParams, currentMonoBuf, &node->outBuf);
    } else if ((mode == BASS_HRTF_MODE_AMBISONICS || node->useAmbisonics) &&
               node->ambiEncodeEffect && node->ambiRotateEffect && node->ambiBinauralEffect) {
        int order = node->ambiOrder;
        if (order < 1) order = 1;
        if (order > 3) order = 3;
        UpdateNodeBufferDescriptors(node, fs);

        IPLAmbisonicsEncodeEffectParams encParams;
        encParams.direction.x = node->dirX;
        encParams.direction.y = node->dirY;
        encParams.direction.z = node->dirZ;
        encParams.order       = order;
        fn_iplAmbisonicsEncodeEffectApply(node->ambiEncodeEffect, &encParams, currentMonoBuf, &node->ambiBuf);

        float yawRad   = g_ListenerYaw   * (3.1415926535f / 180.0f);
        float pitchRad = g_ListenerPitch * (3.1415926535f / 180.0f);
        float cosP = cosf(pitchRad), sinP = sinf(pitchRad);
        float cosY = cosf(yawRad),   sinY = sinf(yawRad);

        IPLAmbisonicsRotationEffectParams rotParams;
        memset(&rotParams, 0, sizeof(rotParams));
        rotParams.orientation.ahead.x = sinY * cosP;
        rotParams.orientation.ahead.y = sinP;
        rotParams.orientation.ahead.z = -cosY * cosP;
        rotParams.orientation.up.x    = -sinY * sinP;
        rotParams.orientation.up.y    = cosP;
        rotParams.orientation.up.z    = cosY * sinP;
        rotParams.orientation.right.x = cosY;
        rotParams.orientation.right.y = 0.0f;
        rotParams.orientation.right.z = sinY;
        rotParams.order = order;

        fn_iplAmbisonicsRotationEffectApply(node->ambiRotateEffect, &rotParams, &node->ambiBuf, &node->ambiRotBuf);

        IPLAmbisonicsBinauralEffectParams binParams = { g_HRTF, order };
        fn_iplAmbisonicsBinauralEffectApply(node->ambiBinauralEffect, &binParams, &node->ambiRotBuf, &node->outBuf);
    } else {
        // Mặc định: Pure Binaural HRTF (Không bao giờ bypass đột ngột để tránh click pha!)
        IPLBinauralEffectParams hrtfParams;
        memset(&hrtfParams, 0, sizeof(hrtfParams));
        hrtfParams.direction.x   = node->dirX;
        hrtfParams.direction.y   = node->dirY;
        hrtfParams.direction.z   = node->dirZ;
        hrtfParams.interpolation = node->interpolation ? 1 : 0;
        hrtfParams.spatialBlend  = node->spatialBlend;
        hrtfParams.hrtf          = g_HRTF;

        fn_iplBinauralEffectApply(node->binauralEffect, &hrtfParams, inputForBinaural, &node->outBuf);
    }

    // 4. Áp dụng Pathing Effect (Multi-path propagation quanh góc tường) nếu được bật
    if (node->usePathEffect && node->pathWetMix > 0.001f &&
        node->pathEffect && node->ambiBinauralEffect &&
        fn_iplPathEffectApply && fn_iplAmbisonicsBinauralEffectApply) {
        float sh[4] = {
            node->pathSh[0],
            node->pathSh[1] * node->dirY,
            node->pathSh[2] * node->dirZ,
            node->pathSh[3] * node->dirX
        };
        IPLPathEffectParams pathParams;
        memset(&pathParams, 0, sizeof(pathParams));
        pathParams.eqCoeffs[0] = node->pathEq[0];
        pathParams.eqCoeffs[1] = node->pathEq[1];
        pathParams.eqCoeffs[2] = node->pathEq[2];
        pathParams.shCoeffs    = sh;
        pathParams.order       = 1;
        pathParams.binaural    = 1;
        pathParams.hrtf        = g_HRTF;

        int savedChans = node->ambiBuf.numChannels;
        node->ambiBuf.numChannels = 4;
        fn_iplPathEffectApply(node->pathEffect, &pathParams, currentMonoBuf, &node->ambiBuf);
        IPLAmbisonicsBinauralEffectParams abParams = { g_HRTF, 1 };
        fn_iplAmbisonicsBinauralEffectApply(node->ambiBinauralEffect, &abParams, &node->ambiBuf, &node->auxBuf);
        node->ambiBuf.numChannels = savedChans;

        float pWet = node->pathWetMix;
        for (int i = 0; i < fs; ++i) {
            node->outLeftFrame[i]  += node->auxLeftFrame[i]  * pWet;
            node->outRightFrame[i] += node->auxRightFrame[i] * pWet;
        }
    }

    // 5. Áp dụng Reflection / Parametric Reverb nếu được bật
    if (node->useReflectionEffect && node->reverbWetMix > 0.001f) {
        ApplyEarlyReflectionsAndReverb(node, currentMonoBuf->data[0], node->outLeftFrame, node->outRightFrame, fs);
    }

    // 6. Unified Gain Ramp mượt mà + Fast Soft-Clipping chuẩn SIMD
    float targetGain  = node->gainAtten;
    float currentGain = node->prevGain;
    float step = (targetGain - currentGain) * (1.0f / (float)fs);

    float * __restrict outL = node->outLeftFrame;
    float * __restrict outR = node->outRightFrame;

    for (int i = 0; i < fs; ++i) {
        currentGain += step;
        outL[i] = FastSoftClip(outL[i] * currentGain);
        outR[i] = FastSoftClip(outR[i] * currentGain);
    }
    node->prevGain = targetGain;

    // 7. Interleave ra buffer Stereo
    if (interleavedStereoInOut) {
        if (fn_iplAudioBufferInterleave && g_Context) {
            fn_iplAudioBufferInterleave(g_Context, &node->outBuf, interleavedStereoInOut);
        } else {
            for (int i = 0; i < fs; ++i) {
                interleavedStereoInOut[i * 2]     = outL[i];
                interleavedStereoInOut[i * 2 + 1] = outR[i];
            }
        }
    }
}

// ============================================================================
// DSP CALLBACK MODE (BẤT BIẾN FIFO O(1) - KHÔNG BAO GIỜ ĐỨT QUÃNG / CLICK)
// ============================================================================
static void B_CALLBACK HRTF_DSPProc(HDSP handle, DWORD channel, void *buffer, DWORD length, void *user) {
    BASS_HRTF_Node *node = (BASS_HRTF_Node*)user;
    if (!node || !node->isAlive || !node->enabled || !buffer || length == 0) return;

    Node_AddRef(node);
    if (!node->isAlive) {
        Node_Release(node);
        return;
    }

    int targetFs = node->targetFrameSize > 0 ? node->targetFrameSize : g_FrameSize;
    if (targetFs != node->frameSize) {
        RebuildNodeForFrameSize(node, targetFs);
    }
    int fs = node->frameSize;

    if (node->needReset) {
        node->ringInFrames = 0;
        node->ringOutReadPos = 0;
        node->ringOutWritePos = 0;
        node->dspFifoPrimed = 0;
        node->prevGain = node->gainAtten;
        ResetAllNodeEffects(node);
        node->needReset = 0;
    }

    // DISTANCE CULLING (AUTO-SLEEP & CLEAN WAKEUP): 0% CPU khi ngoài vùng nghe
    if (node->gainAtten <= 0.0001f && node->prevGain <= 0.0001f &&
        !node->slidePosActive && !node->slideAttrActive) {
        if (!node->wasCulled) {
            ResetAllNodeEffects(node);
            node->ringInFrames = 0;
            node->ringOutReadPos = 0;
            node->ringOutWritePos = 0;
            node->dspFifoPrimed = 0;
            node->wasCulled = 1;
        }
        memset(buffer, 0, length);
        Node_Release(node);
        return;
    }
    node->wasCulled = 0;

    // HỖ TRỢ KÊNH MONO (chans == 1) TRONG DSP MODE (In-place fallback)
    if (node->sourceChans == 1) {
        float *ioMono = (float*)buffer;
        int totalFrames = (int)(length / sizeof(float));
        int offset = 0;
        while (offset < totalFrames) {
            int chunk = totalFrames - offset;
            if (chunk > fs) chunk = fs;
            for (int i = 0; i < chunk; ++i) {
                float s = ioMono[offset + i];
                node->ringInStereo[i * 2]     = s;
                node->ringInStereo[i * 2 + 1] = s;
            }
            if (chunk < fs) {
                memset(node->ringInStereo + (chunk * 2), 0, (size_t)(fs - chunk) * 2 * sizeof(float));
            }
            ProcessOneFrame_StereoIn(node, node->ringInStereo);
            for (int i = 0; i < chunk; ++i) {
                ioMono[offset + i] = (node->ringInStereo[i * 2] + node->ringInStereo[i * 2 + 1]) * 0.5f;
            }
            offset += chunk;
        }
        Node_Release(node);
        return;
    }

    float *ioStereo = (float*)buffer;
    int totalFrames = (int)(length / (sizeof(float) * 2));

    // FAST PATH: Chỉ dùng khi FIFO chưa kích hoạt và kích thước buffer chia hết cho frameSize
    if (!node->dspFifoPrimed && node->ringInFrames == 0 && (totalFrames % fs == 0)) {
        for (int offset = 0; offset < totalFrames; offset += fs) {
            ProcessOneFrame_StereoIn(node, ioStereo + (offset * 2));
        }
        Node_Release(node);
        return;
    }

    // FIFO PATH BẤT BIẾN: ringInFrames + (ringOutAvail / 2) == fs
    // Khởi tạo pre-fill 1 frame đầu tiên để đảm bảo không bao giờ thiếu mẫu ở giữa luồng!
    if (!node->dspFifoPrimed) {
        node->ringInFrames = 0;
        node->ringOutReadPos = 0;
        node->ringOutWritePos = fs * 2;
        memset(node->ringOutStereo, 0, (size_t)(fs * 2) * sizeof(float));
        node->dspFifoPrimed = 1;
    }

    int framesProcessed = 0;
    const int maxRingFloats = MAX_HRTF_FRAME_SIZE * 2 * 4;

    while (framesProcessed < totalFrames) {
        int spaceInFrame = fs - node->ringInFrames;
        int remain = totalFrames - framesProcessed;
        int toTake = (remain < spaceInFrame) ? remain : spaceInFrame;

        // 1. Nạp toTake frames từ ioStereo vào ringInStereo
        memcpy(node->ringInStereo + (node->ringInFrames * 2),
               ioStereo + (framesProcessed * 2),
               (size_t)(toTake * 2) * sizeof(float));
        node->ringInFrames += toTake;

        // 2. Lấy đúng toTake frames từ ringOutStereo ghi ra ioStereo
        int floatsNeeded = toTake * 2;
        int availFloats = node->ringOutWritePos - node->ringOutReadPos;
        int floatsToCopy = (availFloats < floatsNeeded) ? availFloats : floatsNeeded;
        float *dst = ioStereo + (framesProcessed * 2);

        if (floatsToCopy > 0) {
            memcpy(dst, node->ringOutStereo + node->ringOutReadPos, (size_t)floatsToCopy * sizeof(float));
            node->ringOutReadPos += floatsToCopy;
            if (node->ringOutReadPos == node->ringOutWritePos) {
                node->ringOutReadPos = 0;
                node->ringOutWritePos = 0;
            }
        }
        if (floatsToCopy < floatsNeeded) {
            memset(dst + floatsToCopy, 0, (size_t)(floatsNeeded - floatsToCopy) * sizeof(float));
        }

        // 3. Khi ringInStereo đủ 1 frame (fs), xử lý HRTF và đẩy vào ringOutStereo
        if (node->ringInFrames == fs) {
            node->ringInFrames = 0;
            ProcessOneFrame_StereoIn(node, node->ringInStereo);

            if (node->ringOutReadPos == node->ringOutWritePos) {
                node->ringOutReadPos = 0;
                node->ringOutWritePos = 0;
            } else if (node->ringOutWritePos + (fs * 2) > maxRingFloats) {
                int avail = node->ringOutWritePos - node->ringOutReadPos;
                memmove(node->ringOutStereo, node->ringOutStereo + node->ringOutReadPos, (size_t)avail * sizeof(float));
                node->ringOutReadPos = 0;
                node->ringOutWritePos = avail;
            }

            if (node->ringOutWritePos + (fs * 2) <= maxRingFloats) {
                memcpy(node->ringOutStereo + node->ringOutWritePos,
                       node->ringInStereo,
                       (size_t)(fs * 2) * sizeof(float));
                node->ringOutWritePos += fs * 2;
            }
        }

        framesProcessed += toTake;
    }

    Node_Release(node);
}

// ============================================================================
// STREAM CALLBACK MODE (HỖ TRỢ VOICE CHAT THỜI GIAN THỰC, DECODE, MONO/STEREO)
// ============================================================================
static DWORD B_CALLBACK HRTF_StreamProc(HSTREAM handle, void *buffer, DWORD length, void *user) {
    BASS_HRTF_Node *node = (BASS_HRTF_Node*)user;
    if (!node || !node->isAlive) return BASS_STREAMPROC_END;

    Node_AddRef(node);
    if (!node->isAlive) {
        Node_Release(node);
        return BASS_STREAMPROC_END;
    }

    int targetFs = node->targetFrameSize > 0 ? node->targetFrameSize : g_FrameSize;
    if (targetFs != node->frameSize) {
        RebuildNodeForFrameSize(node, targetFs);
    }
    int fs = node->frameSize;

    float *outStream = (float*)buffer;
    int floatsNeeded = (int)(length / sizeof(float));
    int floatsWritten = 0;
    BOOL trueEof = FALSE;

    if (node->needReset) {
        node->ringInFrames = 0;
        node->ringOutReadPos = 0;
        node->ringOutWritePos = 0;
        node->tailFlushed = 0;
        node->prevGain = node->gainAtten;
        ResetAllNodeEffects(node);
        node->needReset = 0;
    }

    int srcChans = (node->sourceChans > 0) ? node->sourceChans : 1;

    while (floatsWritten < floatsNeeded) {
        int availFloats = node->ringOutWritePos - node->ringOutReadPos;
        if (availFloats > 0) {
            int toCopy = floatsNeeded - floatsWritten;
            if (toCopy > availFloats) toCopy = availFloats;
            memcpy(outStream + floatsWritten, node->ringOutStereo + node->ringOutReadPos, (size_t)toCopy * sizeof(float));
            floatsWritten += toCopy;
            node->ringOutReadPos += toCopy;
            if (node->ringOutReadPos == node->ringOutWritePos) {
                node->ringOutReadPos = 0;
                node->ringOutWritePos = 0;
            }
            continue;
        }

        // Gom đủ fs frames từ sourceChan (tránh lỗi zero-pad khi đọc lẻ mẫu ở điểm loop hoặc voice packet)
        BOOL stalledNow = FALSE;
        while (node->ringInFrames < fs) {
            int framesMissing = fs - node->ringInFrames;
            DWORD bytesToAsk = (DWORD)(framesMissing * srcChans * (int)sizeof(float));
            DWORD bytesRead = fn_BASS_ChannelGetData(node->sourceChan, node->streamDecodeTemp, bytesToAsk | BASS_DATA_FLOAT);

            if (bytesRead == (DWORD)-1 || bytesRead == 0) {
                DWORD active = fn_BASS_ChannelIsActive ? fn_BASS_ChannelIsActive(node->sourceChan) : BASS_ACTIVE_STOPPED;
                DWORD flags  = fn_BASS_ChannelFlags ? fn_BASS_ChannelFlags(node->sourceChan, 0, 0) : 0;
                BOOL isLooped = (flags & BASS_SAMPLE_LOOP) != 0;

                if (node->isLiveSource || isLooped || active == BASS_ACTIVE_PLAYING || active == BASS_ACTIVE_STALLED || active == BASS_ACTIVE_PAUSED) {
                    stalledNow = TRUE;
                } else {
                    trueEof = TRUE;
                }
                break;
            }

            int framesGot = (int)(bytesRead / ((DWORD)srcChans * sizeof(float)));
            if (framesGot <= 0) {
                stalledNow = TRUE;
                break;
            }

            if (srcChans == 1) {
                for (int i = 0; i < framesGot; ++i) {
                    float s = node->streamDecodeTemp[i];
                    int dstIdx = node->ringInFrames + i;
                    node->inMonoFrame[dstIdx]       = s;
                    node->ringInStereo[dstIdx * 2]     = s;
                    node->ringInStereo[dstIdx * 2 + 1] = s;
                }
            } else if (srcChans == 2) {
                for (int i = 0; i < framesGot; ++i) {
                    float L = node->streamDecodeTemp[i * 2];
                    float R = node->streamDecodeTemp[i * 2 + 1];
                    int dstIdx = node->ringInFrames + i;
                    node->inMonoFrame[dstIdx]       = (L + R) * 0.5f;
                    node->ringInStereo[dstIdx * 2]     = L;
                    node->ringInStereo[dstIdx * 2 + 1] = R;
                }
            } else {
                for (int i = 0; i < framesGot; ++i) {
                    float L = node->streamDecodeTemp[i * srcChans];
                    float R = node->streamDecodeTemp[i * srcChans + 1];
                    int dstIdx = node->ringInFrames + i;
                    node->inMonoFrame[dstIdx]       = (L + R) * 0.5f;
                    node->ringInStereo[dstIdx * 2]     = L;
                    node->ringInStereo[dstIdx * 2 + 1] = R;
                }
            }
            node->ringInFrames += framesGot;
            node->tailFlushed = 0;
        }

        // Xử lý khi chưa gom đủ 1 frame (fs)
        if (node->ringInFrames < fs) {
            if (stalledNow) {
                // Nếu là luồng Voice Chat / Decode trực tiếp cần nhả ngay khung âm thanh cuối
                if (node->ringInFrames > 0 && node->lowLatencyFlush && floatsWritten == 0) {
                    int valid = node->ringInFrames;
                    float lastM = node->inMonoFrame[valid - 1];
                    float lastL = node->ringInStereo[(valid - 1) * 2];
                    float lastR = node->ringInStereo[(valid - 1) * 2 + 1];
                    // Mượt hóa đuôi về 0 để không bao giờ tạo tiếng click kim loại
                    int padCount = fs - valid;
                    for (int i = 0; i < padCount; ++i) {
                        float fade = 1.0f - ((float)(i + 1) / (float)(padCount + 1));
                        int dstIdx = valid + i;
                        node->inMonoFrame[dstIdx]          = lastM * fade;
                        node->ringInStereo[dstIdx * 2]     = lastL * fade;
                        node->ringInStereo[dstIdx * 2 + 1] = lastR * fade;
                    }
                    node->ringInFrames = 0;

                    ProcessOneFrame_StereoIn(node, (srcChans >= 2 && node->useStereoInput) ? node->ringInStereo : NULL);
                    if (fn_iplAudioBufferInterleave && g_Context) {
                        fn_iplAudioBufferInterleave(g_Context, &node->outBuf, node->ringOutStereo);
                    } else {
                        for (int i = 0; i < fs; ++i) {
                            node->ringOutStereo[i * 2]     = node->outLeftFrame[i];
                            node->ringOutStereo[i * 2 + 1] = node->outRightFrame[i];
                        }
                    }
                    node->ringOutReadPos = 0;
                    node->ringOutWritePos = valid * 2; // Chỉ xuất đúng số mẫu thực tế đã nhận!
                    continue;
                }
                // Tạm dừng vòng lặp, giữ nguyên ringInFrames chờ gói dữ liệu tiếp theo, KHÔNG kết thúc stream!
                break;
            }

            if (trueEof) {
                if (node->ringInFrames > 0) {
                    int valid = node->ringInFrames;
                    memset(node->inMonoFrame + valid, 0, (size_t)(fs - valid) * sizeof(float));
                    memset(node->ringInStereo + (valid * 2), 0, (size_t)(fs - valid) * 2 * sizeof(float));
                    node->ringInFrames = fs;
                    node->tailFlushed = 1;
                } else if (!node->tailFlushed) {
                    memset(node->inMonoFrame, 0, (size_t)fs * sizeof(float));
                    memset(node->ringInStereo, 0, (size_t)(fs * 2) * sizeof(float));
                    node->ringInFrames = fs;
                    node->tailFlushed = 1;
                } else {
                    break;
                }
            }
        }

        if (node->ringInFrames == fs) {
            node->ringInFrames = 0;
            if (node->gainAtten <= 0.0001f && node->prevGain <= 0.0001f &&
                !node->slidePosActive && !node->slideAttrActive) {
                if (!node->wasCulled) {
                    ResetAllNodeEffects(node);
                    node->wasCulled = 1;
                }
                memset(node->ringOutStereo, 0, (size_t)(fs * 2) * sizeof(float));
            } else {
                node->wasCulled = 0;
                ProcessOneFrame_StereoIn(node, (srcChans >= 2 && node->useStereoInput) ? node->ringInStereo : NULL);
                if (fn_iplAudioBufferInterleave && g_Context) {
                    fn_iplAudioBufferInterleave(g_Context, &node->outBuf, node->ringOutStereo);
                } else {
                    for (int i = 0; i < fs; ++i) {
                        node->ringOutStereo[i * 2]     = node->outLeftFrame[i];
                        node->ringOutStereo[i * 2 + 1] = node->outRightFrame[i];
                    }
                }
            }
            node->ringOutReadPos = 0;
            node->ringOutWritePos = fs * 2;
        }
    }

    DWORD bytesTotal = (DWORD)(floatsWritten * sizeof(float));
    BOOL isEnd = (!node->isLiveSource && trueEof && node->tailFlushed && (node->ringOutWritePos == node->ringOutReadPos));
    Node_Release(node);

    return isEnd ? (bytesTotal | BASS_STREAMPROC_END) : bytesTotal;
}

static void B_CALLBACK HRTF_FreeSync(HSYNC handle, DWORD channel, DWORD data, void *user) {
    UnregisterNodeLockFree((BASS_HRTF_Node*)user);
}

static void B_CALLBACK HRTF_PosSync(HSYNC handle, DWORD channel, DWORD data, void *user) {
    BASS_HRTF_Node *node = (BASS_HRTF_Node*)user;
    if (node && node->isAlive) {
        node->needReset = 1;
    }
}

static BASS_HRTF_Node* CreateNodeBase(DWORD sourceChan, DWORD freq, int chans) {
    BASS_HRTF_Node *node = (BASS_HRTF_Node*)calloc(1, sizeof(BASS_HRTF_Node));
    if (!node) return NULL;

    DWORD effectiveRate = (DWORD)g_SampleRate;
    int fs = SanitizeFrameSize(g_FrameSize);

    node->refCount           = 1;
    node->isAlive            = 1;
    node->sourceChan         = sourceChan;
    node->sampleRate         = (freq > 0) ? freq : effectiveRate;
    node->sourceChans        = (chans > 0) ? chans : 1;
    node->frameSize          = fs;
    node->targetFrameSize    = fs;
    node->enabled            = 1;
    node->panningMode        = BASS_HRTF_MODE_BINAURAL;
    node->useDirectFilter    = 0;
    node->useReflectionEffect= 0;
    node->usePathEffect      = 0;
    node->useVirtualSurround = 0;
    node->useAmbisonics      = 0;
    node->useStereoInput     = 0;
    node->ambiOrder          = 1;
    node->surroundLayoutType = IPL_SPEAKERLAYOUTTYPE_STEREO;
    node->dirX               = 0.0f;
    node->dirY               = 0.0f;
    node->dirZ               = -1.0f;
    node->srcAheadX          = 0.0f;
    node->srcAheadY          = 1.0f;
    node->srcAheadZ          = 0.0f;
    node->dipoleWeight       = 0.0f;
    node->dipolePower        = 1.0f;
    node->useDipoleCalc      = 0;
    node->spatialBlend       = 1.0f;
    node->maxSpatialBlend    = 1.0f;
    node->distAtten          = 1.0f;
    node->userGain           = 1.0f;
    node->gainAtten          = 1.0f;
    node->prevGain           = 1.0f;
    node->distanceUnits      = 0.0f;
    node->effectiveDist      = 0.0f;
    node->occlusion          = 0.0f;
    node->airAbsorption      = 1.0f;
    node->airAbsCoeffLow     = 0.0002f;
    node->airAbsCoeffMid     = 0.0017f;
    node->airAbsCoeffHigh    = 0.0182f;
    node->transLow           = 0.35f;
    node->transMid           = 0.12f;
    node->transHigh          = 0.02f;
    node->baseTransLow       = 0.35f;
    node->baseTransMid       = 0.12f;
    node->baseTransHigh      = 0.02f;
    node->directivity        = 1.0f;
    node->sourceRadius       = 0.0f;
    node->interpolation      = 1;

    node->reverbTimes[0]     = 1.2f;
    node->reverbTimes[1]     = 1.0f;
    node->reverbTimes[2]     = 0.7f;
    node->reverbEq[0]        = 1.0f;
    node->reverbEq[1]        = 0.9f;
    node->reverbEq[2]        = 0.7f;
    node->reverbDelayMs      = 20.0f;
    node->reverbWetMix       = 0.25f;

    node->pathEq[0]          = 0.9f;
    node->pathEq[1]          = 0.7f;
    node->pathEq[2]          = 0.45f;
    node->pathSh[0]          = 1.0f;
    node->pathSh[1]          = 0.5f;
    node->pathSh[2]          = 0.5f;
    node->pathSh[3]          = 0.5f;
    node->pathWetMix         = 0.25f;

    node->minDist            = g_DefaultMinDist;
    node->maxDist            = g_DefaultMaxDist;
    node->rolloff            = g_DefaultRolloff;
    node->blendStep          = g_DefaultBlendStep;

    node->inStereoPtrs[0]    = node->inLeftFrame;
    node->inStereoPtrs[1]    = node->inRightFrame;
    node->inStereoBuf.data   = node->inStereoPtrs;

    node->inMonoPtrs[0]      = node->inMonoFrame;
    node->inMonoBuf.data     = node->inMonoPtrs;

    node->procPtrs[0]        = node->processedMonoFrame;
    node->procBuf.data       = node->procPtrs;

    node->revPtrs[0]         = node->reverbMonoFrame;
    node->revBuf.data        = node->revPtrs;

    node->outPtrs[0]         = node->outLeftFrame;
    node->outPtrs[1]         = node->outRightFrame;
    node->outBuf.data        = node->outPtrs;

    node->auxPtrs[0]         = node->auxLeftFrame;
    node->auxPtrs[1]         = node->auxRightFrame;
    node->auxBuf.data        = node->auxPtrs;

    for (int c = 0; c < MAX_AMBI_CHANNELS; ++c) {
        node->ambiPtrs[c]    = node->ambiFrames[c];
        node->ambiRotPtrs[c] = node->ambiRotFrames[c];
    }
    node->ambiBuf.data       = node->ambiPtrs;
    node->ambiRotBuf.data    = node->ambiRotPtrs;

    UpdateNodeBufferDescriptors(node, fs);

    IPLAudioSettings audioSettings = { (int32_t)effectiveRate, fs };
    IPLBinauralEffectSettings binSettings = { g_HRTF };

    if (fn_iplBinauralEffectCreate(g_Context, &audioSettings, &binSettings, &node->binauralEffect) != IPL_STATUS_SUCCESS) {
        free(node);
        return NULL;
    }

    return node;
}

// Hỗ trợ tự động giải quyết HSAMPLE sang HSTREAM/HCHANNEL
static DWORD ResolveChannelOrSample(DWORD handle, BOOL forDecodeStream, DWORD *outOwnedChan, BASS_CHANNELINFO *outInfo) {
    if (outOwnedChan) *outOwnedChan = 0;
    if (!handle) return 0;

    if (fn_BASS_ChannelGetInfo && fn_BASS_ChannelGetInfo(handle, outInfo)) {
        return handle;
    }
    // Nếu handle là HSAMPLE (không phải HCHANNEL/HSTREAM/HMUSIC), tự động tạo sample stream!
    if (fn_BASS_SampleGetInfo && fn_BASS_SampleGetChannel) {
        BASS_SAMPLE_INFO sInfo;
        if (fn_BASS_SampleGetInfo(handle, &sInfo)) {
            DWORD flags = BASS_SAMCHAN_STREAM | (forDecodeStream ? BASS_STREAM_DECODE : 0);
            DWORD ch = fn_BASS_SampleGetChannel(handle, flags);
            if (!ch && !forDecodeStream) {
                ch = fn_BASS_SampleGetChannel(handle, 0);
            }
            if (ch && fn_BASS_ChannelGetInfo(ch, outInfo)) {
                if (outOwnedChan && (flags & BASS_SAMCHAN_STREAM)) *outOwnedChan = ch;
                return ch;
            }
        }
    }
    return 0;
}

// ============================================================================
// EXPORTED API FUNCTIONS (100% LOCK-FREE)
// ============================================================================

EXPORT BOOL B_CALLBACK BASS_HRTF_InitEx(int sampleRate, const char* bassPath, const char* phononPath, const char* sofaPath, uint32_t steamAudioVersion) {
    if (g_Context != NULL) return TRUE;
    if (!LoadAllSymbols(bassPath, phononPath)) return FALSE;

    if (fn_BASS_SetConfig) {
        fn_BASS_SetConfig(BASS_CONFIG_FLOATDSP, TRUE);
    }

    g_SampleRate = (sampleRate > 0) ? sampleRate : 44100;
    g_FrameSize  = SanitizeFrameSize(g_FrameSize);
    if (sofaPath && sofaPath[0] != '\0') {
        strncpy(g_SofaPath, sofaPath, sizeof(g_SofaPath) - 1);
        g_SofaPath[sizeof(g_SofaPath) - 1] = '\0';
    } else {
        g_SofaPath[0] = '\0';
    }

    IPLContextSettings ctxSettings;
    memset(&ctxSettings, 0, sizeof(ctxSettings));
    ctxSettings.version = (steamAudioVersion > 0) ? steamAudioVersion : DEFAULT_STEAMAUDIO_VERSION;
    ctxSettings.simdLevel = 3;

    if (fn_iplContextCreate(&ctxSettings, &g_Context) != IPL_STATUS_SUCCESS) {
        uint32_t fallbacks[] = {
            0x00040503, 0x00040502, 0x00040501, 0x00040500,
            0x00040400, 0x00040300, 0x00040200, 0x00040100, 0x00040000
        };
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

    IPLAudioSettings audioSettings = { g_SampleRate, g_FrameSize };
    IPLHRTFSettings hrtfSettings;
    memset(&hrtfSettings, 0, sizeof(hrtfSettings));
    if (g_SofaPath[0] != '\0') {
        hrtfSettings.type = 1;
        hrtfSettings.sofaFileName = g_SofaPath;
    } else {
        hrtfSettings.type = 0;
    }
    hrtfSettings.normType = 1;
    hrtfSettings.volume   = 0.90f;

    if (fn_iplHRTFCreate(g_Context, &audioSettings, &hrtfSettings, &g_HRTF) != IPL_STATUS_SUCCESS) {
        fn_iplContextRelease(&g_Context);
        g_Context = NULL;
        return FALSE;
    }

    return TRUE;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_Init(int sampleRate) {
    return BASS_HRTF_InitEx(sampleRate, NULL, NULL, NULL, 0);
}

EXPORT void B_CALLBACK BASS_HRTF_Free(void) {
    for (int i = 0; i < MAX_HRTF_CHANNELS; ++i) {
        BASS_HRTF_Node *node = (BASS_HRTF_Node*)ATOMIC_XCHG_PTR(&g_Nodes[i], NULL);
        if (node) {
            node->isAlive = 0;
            DWORD syncChan = node->isDSPMode ? node->sourceChan : node->handleKey;
            if (node->freeSync && fn_BASS_ChannelRemoveSync) {
                fn_BASS_ChannelRemoveSync(syncChan, node->freeSync);
                node->freeSync = 0;
            }
            if (node->posSync && fn_BASS_ChannelRemoveSync) {
                fn_BASS_ChannelRemoveSync(syncChan, node->posSync);
                node->posSync = 0;
            }
            if (node->isDSPMode && node->dspHandle && fn_BASS_ChannelRemoveDSP) {
                fn_BASS_ChannelRemoveDSP(node->sourceChan, node->dspHandle);
                node->dspHandle = 0;
            }
            Node_Release(node);
        }
    }
    g_MaxNodeLimit = 0;

    for (int i = 0; i < MAX_ENV_BOXES; ++i) {
        g_EnvBoxes[i].active = 0;
    }
    g_EnvBoxCount = 0;
    g_AutoEnvRaycast = 0;

    if (g_HRTF && fn_iplHRTFRelease) { fn_iplHRTFRelease(&g_HRTF); g_HRTF = NULL; }
    if (g_Context && fn_iplContextRelease) { fn_iplContextRelease(&g_Context); g_Context = NULL; }
}

// ============================================================================
// API SET FRAME SIZE (TOÀN CỤC & TỪNG NODE)
// ============================================================================
EXPORT BOOL B_CALLBACK BASS_HRTF_SetFrameSize(int frameSize) {
    if (frameSize < MIN_HRTF_FRAME_SIZE || frameSize > MAX_HRTF_FRAME_SIZE) return FALSE;
    int newFs = SanitizeFrameSize(frameSize);
    g_FrameSize = newFs;

    if (g_Context && fn_iplHRTFCreate) {
        IPLAudioSettings audioSettings = { g_SampleRate, newFs };
        IPLHRTFSettings hrtfSettings;
        memset(&hrtfSettings, 0, sizeof(hrtfSettings));
        if (g_SofaPath[0] != '\0') {
            hrtfSettings.type = 1;
            hrtfSettings.sofaFileName = g_SofaPath;
        } else {
            hrtfSettings.type = 0;
        }
        hrtfSettings.normType = 1;
        hrtfSettings.volume   = 0.90f;

        IPLHRTF newHrtf = NULL;
        if (fn_iplHRTFCreate(g_Context, &audioSettings, &hrtfSettings, &newHrtf) == IPL_STATUS_SUCCESS && newHrtf) {
            g_HRTF = newHrtf;
        }
    }

    int limit = g_MaxNodeLimit;
    if (limit > MAX_HRTF_CHANNELS) limit = MAX_HRTF_CHANNELS;
    for (int i = 0; i < limit; ++i) {
        BASS_HRTF_Node *node = g_Nodes[i];
        if (node && node->isAlive) {
            Node_AddRef(node);
            if (node->isAlive) {
                node->targetFrameSize = newFs;
                if (node->isDSPMode && fn_BASS_ChannelSetAttribute) {
                    fn_BASS_ChannelSetAttribute(node->sourceChan, BASS_ATTRIB_GRANULE, (float)newFs);
                }
            }
            Node_Release(node);
        }
    }
    return TRUE;
}

EXPORT int B_CALLBACK BASS_HRTF_GetFrameSize(void) {
    return g_FrameSize;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_SetNodeFrameSize(DWORD handle, int frameSize) {
    if (frameSize < MIN_HRTF_FRAME_SIZE || frameSize > MAX_HRTF_FRAME_SIZE) return FALSE;
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;
    int newFs = SanitizeFrameSize(frameSize);
    node->targetFrameSize = newFs;
    if (node->isDSPMode && fn_BASS_ChannelSetAttribute) {
        fn_BASS_ChannelSetAttribute(node->sourceChan, BASS_ATTRIB_GRANULE, (float)newFs);
    }
    Node_Release(node);
    return TRUE;
}

EXPORT int B_CALLBACK BASS_HRTF_GetNodeFrameSize(DWORD handle) {
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return 0;
    int fs = node->targetFrameSize > 0 ? node->targetFrameSize : node->frameSize;
    Node_Release(node);
    return fs;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_SetLiveStreamMode(DWORD handle, BOOL isLive, BOOL lowLatencyFlush) {
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;
    node->isLiveSource = isLive ? 1 : 0;
    node->lowLatencyFlush = lowLatencyFlush ? 1 : 0;
    Node_Release(node);
    return TRUE;
}

// ============================================================================
// API MÔI TRƯỜNG VẬT LÝ & TƯỜNG CHẮN 3D (LOCK-FREE RAYCAST AABB)
// ============================================================================
EXPORT int B_CALLBACK BASS_HRTF_EnvAddBoxPreset(int materialPresetId, float minX, float maxX, float minY, float maxY, float minZ, float maxZ) {
    if (materialPresetId < 0 || materialPresetId > 11) materialPresetId = 1;
    int idx = g_EnvBoxCount;
    if (idx >= MAX_ENV_BOXES) return -1;

    EnvBox *b = &g_EnvBoxes[idx];
    b->minX = (minX < maxX) ? minX : maxX;
    b->maxX = (minX < maxX) ? maxX : minX;
    b->minY = (minY < maxY) ? minY : maxY;
    b->maxY = (minY < maxY) ? maxY : minY;
    b->minZ = (minZ < maxZ) ? minZ : maxZ;
    b->maxZ = (minZ < maxZ) ? maxZ : minZ;
    b->mat  = g_PresetMaterials[materialPresetId];
    MEMORY_BARRIER();
    b->active = 1;
    g_EnvBoxCount = idx + 1;
    g_AutoEnvRaycast = 1;
    return idx;
}

EXPORT void B_CALLBACK BASS_HRTF_EnvClearBoxes(void) {
    g_AutoEnvRaycast = 0;
    int count = g_EnvBoxCount;
    g_EnvBoxCount = 0;
    for (int i = 0; i < count; ++i) {
        g_EnvBoxes[i].active = 0;
    }
}

EXPORT void B_CALLBACK BASS_HRTF_EnvSetAutoRaycast(BOOL enable) {
    g_AutoEnvRaycast = enable ? 1 : 0;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_SetNodeMaterialPreset(DWORD handle, int materialPresetId) {
    if (materialPresetId < 0 || materialPresetId > 11) return FALSE;
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;
    float tL = g_PresetMaterials[materialPresetId].transmission[0];
    float tM = g_PresetMaterials[materialPresetId].transmission[1];
    float tH = g_PresetMaterials[materialPresetId].transmission[2];
    node->baseTransLow  = tL;
    node->baseTransMid  = tM;
    node->baseTransHigh = tH;
    node->transLow      = tL;
    node->transMid      = tM;
    node->transHigh     = tH;
    Node_Release(node);
    return TRUE;
}

// ============================================================================
// API TĂNG CƯỜNG FX STEAMAUDIO vs SET PARAM (TÁCH BIỆT RÕ RÀNG)
// ============================================================================

// Thêm / Kích hoạt یک FX của SteamAudio trên Node (Lazy-allocate khi thêm)
EXPORT BOOL B_CALLBACK BASS_HRTF_AddSteamFX(DWORD handle, int fxType, int presetId) {
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;
    BOOL ok = FALSE;

    if (fxType == BASS_HRTF_FX_DIRECT) {
        ok = EnsureDirectEffectCreated(node);
        if (ok) {
            if (presetId >= 0 && presetId <= 11) {
                node->baseTransLow  = g_PresetMaterials[presetId].transmission[0];
                node->baseTransMid  = g_PresetMaterials[presetId].transmission[1];
                node->baseTransHigh = g_PresetMaterials[presetId].transmission[2];
                node->transLow      = node->baseTransLow;
                node->transMid      = node->baseTransMid;
                node->transHigh     = node->baseTransHigh;
                if (presetId > 0 && node->occlusion < 0.35f) node->occlusion = 0.65f;
            }
            node->useDirectFilter = 1;
        }
    } else if (fxType == BASS_HRTF_FX_REFLECTION) {
        EnsureReflectionEffectCreated(node);
        if (presetId >= 0 && presetId < 16) {
            node->reverbTimes[0] = g_ReverbPresets[presetId].reverbTimes[0];
            node->reverbTimes[1] = g_ReverbPresets[presetId].reverbTimes[1];
            node->reverbTimes[2] = g_ReverbPresets[presetId].reverbTimes[2];
            node->reverbEq[0]    = g_ReverbPresets[presetId].eq[0];
            node->reverbEq[1]    = g_ReverbPresets[presetId].eq[1];
            node->reverbEq[2]    = g_ReverbPresets[presetId].eq[2];
            node->reverbDelayMs  = g_ReverbPresets[presetId].delayMs;
            node->reverbWetMix   = g_ReverbPresets[presetId].defaultWet;
        }
        node->useReflectionEffect = 1;
        ok = TRUE;
    } else if (fxType == BASS_HRTF_FX_PATHING) {
        ok = EnsurePathEffectCreated(node);
        if (ok) node->usePathEffect = 1;
    } else if (fxType == BASS_HRTF_FX_VIRTUAL_SURROUND) {
        ok = EnsureVirtualSurroundCreated(node);
        if (ok) {
            if (presetId >= 0 && presetId <= 4) node->surroundLayoutType = presetId;
            node->useVirtualSurround = 1;
        }
    } else if (fxType == BASS_HRTF_FX_AMBISONICS) {
        ok = EnsureAmbisonicsCreated(node);
        if (ok) {
            if (presetId >= 1 && presetId <= 3) node->ambiOrder = presetId;
            node->useAmbisonics = 1;
        }
    } else if (fxType == BASS_HRTF_FX_PANNING) {
        ok = EnsurePanningEffectCreated(node);
        if (ok) node->panningMode = BASS_HRTF_MODE_PANNING;
    }

    Node_Release(node);
    return ok;
}

// Gỡ / Tắt một FX của SteamAudio khỏi Node
EXPORT BOOL B_CALLBACK BASS_HRTF_RemoveSteamFX(DWORD handle, int fxType) {
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;

    if (fxType == BASS_HRTF_FX_DIRECT) {
        node->useDirectFilter = 0;
        if (node->directEffect && fn_iplDirectEffectReset) fn_iplDirectEffectReset(node->directEffect);
    } else if (fxType == BASS_HRTF_FX_REFLECTION) {
        node->useReflectionEffect = 0;
        if (node->reflectionEffect && fn_iplReflectionEffectReset) fn_iplReflectionEffectReset(node->reflectionEffect);
        memset(node->erDelayBufL, 0, sizeof(node->erDelayBufL));
        memset(node->erDelayBufR, 0, sizeof(node->erDelayBufR));
    } else if (fxType == BASS_HRTF_FX_PATHING) {
        node->usePathEffect = 0;
        if (node->pathEffect && fn_iplPathEffectReset) fn_iplPathEffectReset(node->pathEffect);
    } else if (fxType == BASS_HRTF_FX_VIRTUAL_SURROUND) {
        node->useVirtualSurround = 0;
        if (node->panningMode == BASS_HRTF_MODE_VIRTUAL_SURROUND) node->panningMode = BASS_HRTF_MODE_BINAURAL;
    } else if (fxType == BASS_HRTF_FX_AMBISONICS) {
        node->useAmbisonics = 0;
        if (node->panningMode == BASS_HRTF_MODE_AMBISONICS) node->panningMode = BASS_HRTF_MODE_BINAURAL;
    } else if (fxType == BASS_HRTF_FX_PANNING) {
        if (node->panningMode == BASS_HRTF_MODE_PANNING) node->panningMode = BASS_HRTF_MODE_BINAURAL;
    }

    Node_Release(node);
    return TRUE;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_ClearSteamFX(DWORD handle) {
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;
    node->useDirectFilter     = 0;
    node->useReflectionEffect = 0;
    node->usePathEffect       = 0;
    node->useVirtualSurround  = 0;
    node->useAmbisonics       = 0;
    node->panningMode         = BASS_HRTF_MODE_BINAURAL;
    ResetAllNodeEffects(node);
    Node_Release(node);
    return TRUE;
}

// Chỉnh tham số (SetParam) tách biệt với việc thêm FX (không khởi tạo lại hay ngắt hiệu ứng)
EXPORT BOOL B_CALLBACK BASS_HRTF_SetFXParam(DWORD handle, int paramId, float value) {
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;
    BOOL ok = TRUE;

    switch (paramId) {
        case BASS_HRTF_PARAM_OCCLUSION:
            node->occlusion = (value < 0.0f) ? 0.0f : ((value > 1.0f) ? 1.0f : value);
            break;
        case BASS_HRTF_PARAM_SPATIAL_BLEND:
            node->maxSpatialBlend = (value < 0.0f) ? 0.0f : ((value > 1.0f) ? 1.0f : value);
            {
                float b = node->effectiveDist * node->blendStep;
                node->spatialBlend = (b > node->maxSpatialBlend) ? node->maxSpatialBlend : b;
            }
            break;
        case BASS_HRTF_PARAM_GAIN:
            node->userGain = (value < 0.0f) ? 0.0f : value;
            node->gainAtten = node->distAtten * node->userGain;
            break;
        case BASS_HRTF_PARAM_AIR_ABSORPTION:
            node->airAbsorption = (value < 0.0f) ? 0.0f : value;
            break;
        case BASS_HRTF_PARAM_DIRECTIVITY:
            node->directivity = (value < 0.0f) ? 0.0f : ((value > 1.0f) ? 1.0f : value);
            break;
        case BASS_HRTF_PARAM_DIRECTIVITY_WEIGHT:
            node->dipoleWeight = (value < 0.0f) ? 0.0f : ((value > 1.0f) ? 1.0f : value);
            node->useDipoleCalc = 1;
            break;
        case BASS_HRTF_PARAM_DIRECTIVITY_POWER:
            node->dipolePower = (value < 0.0f) ? 0.0f : value;
            node->useDipoleCalc = 1;
            break;
        case BASS_HRTF_PARAM_SOURCE_RADIUS:
            node->sourceRadius = (value < 0.0f) ? 0.0f : value;
            break;
        case BASS_HRTF_PARAM_TRANS_LOW:
            node->transLow = node->baseTransLow = (value < 0.0f) ? 0.0f : ((value > 1.0f) ? 1.0f : value);
            break;
        case BASS_HRTF_PARAM_TRANS_MID:
            node->transMid = node->baseTransMid = (value < 0.0f) ? 0.0f : ((value > 1.0f) ? 1.0f : value);
            break;
        case BASS_HRTF_PARAM_TRANS_HIGH:
            node->transHigh = node->baseTransHigh = (value < 0.0f) ? 0.0f : ((value > 1.0f) ? 1.0f : value);
            break;
        case BASS_HRTF_PARAM_REVERB_LOW:
            node->reverbTimes[0] = (value < 0.05f) ? 0.05f : ((value > 10.0f) ? 10.0f : value);
            break;
        case BASS_HRTF_PARAM_REVERB_MID:
            node->reverbTimes[1] = (value < 0.05f) ? 0.05f : ((value > 10.0f) ? 10.0f : value);
            break;
        case BASS_HRTF_PARAM_REVERB_HIGH:
            node->reverbTimes[2] = (value < 0.05f) ? 0.05f : ((value > 10.0f) ? 10.0f : value);
            break;
        case BASS_HRTF_PARAM_REVERB_EQ_LOW:
            node->reverbEq[0] = (value < 0.0f) ? 0.0f : ((value > 2.0f) ? 2.0f : value);
            break;
        case BASS_HRTF_PARAM_REVERB_EQ_MID:
            node->reverbEq[1] = (value < 0.0f) ? 0.0f : ((value > 2.0f) ? 2.0f : value);
            break;
        case BASS_HRTF_PARAM_REVERB_EQ_HIGH:
            node->reverbEq[2] = (value < 0.0f) ? 0.0f : ((value > 2.0f) ? 2.0f : value);
            break;
        case BASS_HRTF_PARAM_REVERB_DELAY_MS:
            node->reverbDelayMs = (value < 1.0f) ? 1.0f : ((value > 250.0f) ? 250.0f : value);
            break;
        case BASS_HRTF_PARAM_REVERB_WET:
            node->reverbWetMix = (value < 0.0f) ? 0.0f : ((value > 1.0f) ? 1.0f : value);
            break;
        case BASS_HRTF_PARAM_PATH_EQ_LOW:
            node->pathEq[0] = (value < 0.0f) ? 0.0f : ((value > 2.0f) ? 2.0f : value);
            break;
        case BASS_HRTF_PARAM_PATH_EQ_MID:
            node->pathEq[1] = (value < 0.0f) ? 0.0f : ((value > 2.0f) ? 2.0f : value);
            break;
        case BASS_HRTF_PARAM_PATH_EQ_HIGH:
            node->pathEq[2] = (value < 0.0f) ? 0.0f : ((value > 2.0f) ? 2.0f : value);
            break;
        case BASS_HRTF_PARAM_PATH_WET:
            node->pathWetMix = (value < 0.0f) ? 0.0f : ((value > 1.0f) ? 1.0f : value);
            break;
        case BASS_HRTF_PARAM_AMBI_ORDER:
            node->ambiOrder = ((int)value < 1) ? 1 : (((int)value > 3) ? 3 : (int)value);
            break;
        case BASS_HRTF_PARAM_SURROUND_LAYOUT:
            node->surroundLayoutType = ((int)value < 0) ? 0 : (((int)value > 4) ? 4 : (int)value);
            break;
        case BASS_HRTF_PARAM_INTERPOLATION:
            node->interpolation = ((int)value != 0) ? 1 : 0;
            break;
        case BASS_HRTF_PARAM_LOW_LATENCY_FLUSH:
            node->lowLatencyFlush = ((int)value != 0) ? 1 : 0;
            break;
        default:
            ok = FALSE;
            break;
    }

    Node_Release(node);
    return ok;
}

EXPORT float B_CALLBACK BASS_HRTF_GetFXParam(DWORD handle, int paramId) {
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return 0.0f;
    float val = 0.0f;

    switch (paramId) {
        case BASS_HRTF_PARAM_OCCLUSION:          val = node->occlusion; break;
        case BASS_HRTF_PARAM_SPATIAL_BLEND:      val = node->maxSpatialBlend; break;
        case BASS_HRTF_PARAM_GAIN:               val = node->userGain; break;
        case BASS_HRTF_PARAM_AIR_ABSORPTION:     val = node->airAbsorption; break;
        case BASS_HRTF_PARAM_DIRECTIVITY:        val = node->directivity; break;
        case BASS_HRTF_PARAM_DIRECTIVITY_WEIGHT: val = node->dipoleWeight; break;
        case BASS_HRTF_PARAM_DIRECTIVITY_POWER:  val = node->dipolePower; break;
        case BASS_HRTF_PARAM_SOURCE_RADIUS:      val = node->sourceRadius; break;
        case BASS_HRTF_PARAM_TRANS_LOW:          val = node->transLow; break;
        case BASS_HRTF_PARAM_TRANS_MID:          val = node->transMid; break;
        case BASS_HRTF_PARAM_TRANS_HIGH:         val = node->transHigh; break;
        case BASS_HRTF_PARAM_REVERB_LOW:         val = node->reverbTimes[0]; break;
        case BASS_HRTF_PARAM_REVERB_MID:         val = node->reverbTimes[1]; break;
        case BASS_HRTF_PARAM_REVERB_HIGH:        val = node->reverbTimes[2]; break;
        case BASS_HRTF_PARAM_REVERB_EQ_LOW:      val = node->reverbEq[0]; break;
        case BASS_HRTF_PARAM_REVERB_EQ_MID:      val = node->reverbEq[1]; break;
        case BASS_HRTF_PARAM_REVERB_EQ_HIGH:     val = node->reverbEq[2]; break;
        case BASS_HRTF_PARAM_REVERB_DELAY_MS:    val = node->reverbDelayMs; break;
        case BASS_HRTF_PARAM_REVERB_WET:         val = node->reverbWetMix; break;
        case BASS_HRTF_PARAM_PATH_EQ_LOW:        val = node->pathEq[0]; break;
        case BASS_HRTF_PARAM_PATH_EQ_MID:        val = node->pathEq[1]; break;
        case BASS_HRTF_PARAM_PATH_EQ_HIGH:       val = node->pathEq[2]; break;
        case BASS_HRTF_PARAM_PATH_WET:           val = node->pathWetMix; break;
        case BASS_HRTF_PARAM_AMBI_ORDER:         val = (float)node->ambiOrder; break;
        case BASS_HRTF_PARAM_SURROUND_LAYOUT:    val = (float)node->surroundLayoutType; break;
        case BASS_HRTF_PARAM_INTERPOLATION:      val = (float)node->interpolation; break;
        case BASS_HRTF_PARAM_LOW_LATENCY_FLUSH:  val = (float)node->lowLatencyFlush; break;
        default: break;
    }

    Node_Release(node);
    return val;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_EnableReflectionEffect(DWORD handle, BOOL enable) {
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;
    if (enable) {
        EnsureReflectionEffectCreated(node);
        node->useReflectionEffect = 1;
    } else {
        node->useReflectionEffect = 0;
    }
    Node_Release(node);
    return TRUE;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_SetReflectionParams(DWORD handle, float reverbLow, float reverbMid, float reverbHigh, float eqLow, float eqMid, float eqHigh, float delayMs, float wetMix) {
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;
    node->reverbTimes[0] = (reverbLow  < 0.05f) ? 0.05f : reverbLow;
    node->reverbTimes[1] = (reverbMid  < 0.05f) ? 0.05f : reverbMid;
    node->reverbTimes[2] = (reverbHigh < 0.05f) ? 0.05f : reverbHigh;
    node->reverbEq[0]    = (eqLow  < 0.0f) ? 0.0f : eqLow;
    node->reverbEq[1]    = (eqMid  < 0.0f) ? 0.0f : eqMid;
    node->reverbEq[2]    = (eqHigh < 0.0f) ? 0.0f : eqHigh;
    node->reverbDelayMs  = (delayMs < 1.0f) ? 1.0f : ((delayMs > 250.0f) ? 250.0f : delayMs);
    node->reverbWetMix   = (wetMix < 0.0f) ? 0.0f : ((wetMix > 1.0f) ? 1.0f : wetMix);
    Node_Release(node);
    return TRUE;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_SetReflectionPreset(DWORD handle, int reverbPresetId, float wetMix) {
    if (reverbPresetId < 0 || reverbPresetId >= 16) return FALSE;
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;
    EnsureReflectionEffectCreated(node);
    node->reverbTimes[0] = g_ReverbPresets[reverbPresetId].reverbTimes[0];
    node->reverbTimes[1] = g_ReverbPresets[reverbPresetId].reverbTimes[1];
    node->reverbTimes[2] = g_ReverbPresets[reverbPresetId].reverbTimes[2];
    node->reverbEq[0]    = g_ReverbPresets[reverbPresetId].eq[0];
    node->reverbEq[1]    = g_ReverbPresets[reverbPresetId].eq[1];
    node->reverbEq[2]    = g_ReverbPresets[reverbPresetId].eq[2];
    node->reverbDelayMs  = g_ReverbPresets[reverbPresetId].delayMs;
    node->reverbWetMix   = (wetMix >= 0.0f) ? ((wetMix > 1.0f) ? 1.0f : wetMix) : g_ReverbPresets[reverbPresetId].defaultWet;
    node->useReflectionEffect = 1;
    Node_Release(node);
    return TRUE;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_EnablePathEffect(DWORD handle, BOOL enable) {
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;
    if (enable) {
        if (!EnsurePathEffectCreated(node)) {
            Node_Release(node);
            return FALSE;
        }
        node->usePathEffect = 1;
    } else {
        node->usePathEffect = 0;
    }
    Node_Release(node);
    return TRUE;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_SetPathParams(DWORD handle, float eqLow, float eqMid, float eqHigh, float sh0, float sh1, float sh2, float sh3, float wetMix) {
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;
    node->pathEq[0]  = (eqLow  < 0.0f) ? 0.0f : eqLow;
    node->pathEq[1]  = (eqMid  < 0.0f) ? 0.0f : eqMid;
    node->pathEq[2]  = (eqHigh < 0.0f) ? 0.0f : eqHigh;
    node->pathSh[0]  = sh0;
    node->pathSh[1]  = sh1;
    node->pathSh[2]  = sh2;
    node->pathSh[3]  = sh3;
    node->pathWetMix = (wetMix < 0.0f) ? 0.0f : ((wetMix > 1.0f) ? 1.0f : wetMix);
    Node_Release(node);
    return TRUE;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_EnableVirtualSurround(DWORD handle, BOOL enable, int speakerLayoutType) {
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;
    if (enable) {
        if (!EnsureVirtualSurroundCreated(node)) {
            Node_Release(node);
            return FALSE;
        }
        if (speakerLayoutType >= 0 && speakerLayoutType <= 4) {
            node->surroundLayoutType = speakerLayoutType;
        }
        node->useVirtualSurround = 1;
    } else {
        node->useVirtualSurround = 0;
    }
    Node_Release(node);
    return TRUE;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_EnableAmbisonics(DWORD handle, BOOL enable, int order) {
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;
    if (enable) {
        if (!EnsureAmbisonicsCreated(node)) {
            Node_Release(node);
            return FALSE;
        }
        if (order >= 1 && order <= 3) node->ambiOrder = order;
        node->useAmbisonics = 1;
    } else {
        node->useAmbisonics = 0;
    }
    Node_Release(node);
    return TRUE;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_SetPanningMode(DWORD handle, int mode) {
    if (mode < 0 || mode > 3) return FALSE;
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;
    if (mode == BASS_HRTF_MODE_PANNING) EnsurePanningEffectCreated(node);
    else if (mode == BASS_HRTF_MODE_AMBISONICS) EnsureAmbisonicsCreated(node);
    else if (mode == BASS_HRTF_MODE_VIRTUAL_SURROUND) EnsureVirtualSurroundCreated(node);
    node->panningMode = mode;
    Node_Release(node);
    return TRUE;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_SetDirectivityEx(DWORD handle, float dipoleWeight, float dipolePower, float aheadX, float aheadY, float aheadZ) {
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;
    node->dipoleWeight  = (dipoleWeight < 0.0f) ? 0.0f : ((dipoleWeight > 1.0f) ? 1.0f : dipoleWeight);
    node->dipolePower   = (dipolePower  < 0.0f) ? 0.0f : dipolePower;
    node->srcAheadX     = aheadX;
    node->srcAheadY     = aheadY;
    node->srcAheadZ     = aheadZ;
    node->useDipoleCalc = 1;
    UpdateNode3DCoordinatesEx(node, node->relX, node->relY, node->relZ, g_ListenerYaw, g_ListenerPitch, 0);
    Node_Release(node);
    return TRUE;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_SetAirAbsorptionBands(DWORD handle, float lowCoeff, float midCoeff, float highCoeff) {
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;
    node->airAbsCoeffLow  = (lowCoeff  < 0.0f) ? 0.0f : lowCoeff;
    node->airAbsCoeffMid  = (midCoeff  < 0.0f) ? 0.0f : midCoeff;
    node->airAbsCoeffHigh = (highCoeff < 0.0f) ? 0.0f : highCoeff;
    Node_Release(node);
    return TRUE;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_SetStereoInput(DWORD handle, BOOL enable) {
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;
    node->useStereoInput = enable ? 1 : 0;
    Node_Release(node);
    return TRUE;
}

EXPORT void B_CALLBACK BASS_HRTF_SetDistanceModel(float minDist, float maxDist, float rolloffFactor, float blendStep) {
    g_DefaultMinDist   = (minDist >= 0.0f) ? minDist : 1.0f;
    g_DefaultMaxDist   = (maxDist > g_DefaultMinDist) ? maxDist : (g_DefaultMinDist + 50.0f);
    g_DefaultRolloff   = (rolloffFactor >= 0.0f) ? rolloffFactor : 0.12f;
    g_DefaultBlendStep = (blendStep > 0.0f) ? blendStep : 0.25f;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_SetNodeDistanceLimits(DWORD handle, float minDist, float maxDist, float rolloffFactor, float blendStep) {
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;
    node->minDist   = (minDist >= 0.0f) ? minDist : g_DefaultMinDist;
    node->maxDist   = (maxDist > node->minDist) ? maxDist : (node->minDist + 50.0f);
    node->rolloff   = (rolloffFactor >= 0.0f) ? rolloffFactor : g_DefaultRolloff;
    node->blendStep = (blendStep > 0.0f) ? blendStep : g_DefaultBlendStep;
    Node_Release(node);
    return TRUE;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_SetSourceRadius(DWORD handle, float radius) {
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;
    node->sourceRadius = (radius < 0.0f) ? 0.0f : radius;
    Node_Release(node);
    return TRUE;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_SetDirectivity(DWORD handle, float directivity) {
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;
    if (directivity < 0.0f) directivity = 0.0f;
    if (directivity > 1.0f) directivity = 1.0f;
    node->directivity = directivity;
    node->useDipoleCalc = 0;
    Node_Release(node);
    return TRUE;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_SlidePosition(DWORD handle, float targetX, float targetY, float targetZ, float timeMs) {
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;
    if (timeMs <= 0.0f) {
        Node_Release(node);
        return FALSE;
    }

    node->slideStartX = node->useWorldPos ? node->worldX : node->relX;
    node->slideStartY = node->useWorldPos ? node->worldY : node->relY;
    node->slideStartZ = node->useWorldPos ? node->worldZ : node->relZ;
    node->slideTargetX = targetX;
    node->slideTargetY = targetY;
    node->slideTargetZ = targetZ;
    node->slidePosTotalMs = timeMs;
    node->slidePosElapsedMs = 0.0f;
    MEMORY_BARRIER();
    node->slidePosActive = 1;

    Node_Release(node);
    return TRUE;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_SlideAttribute(DWORD handle, int attrType, float targetValue, float timeMs) {
    if (timeMs <= 0.0f || attrType < 1 || attrType > 5) return FALSE;
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;

    float startVal = 0.0f;
    if (attrType == 1) startVal = node->occlusion;
    else if (attrType == 2) startVal = node->maxSpatialBlend;
    else if (attrType == 3) startVal = node->userGain;
    else if (attrType == 4) startVal = node->airAbsorption;
    else if (attrType == 5) startVal = node->reverbWetMix;

    node->slideAttrType = attrType;
    node->slideAttrStart = startVal;
    node->slideAttrTarget = targetValue;
    node->slideAttrTotalMs = timeMs;
    node->slideAttrElapsedMs = 0.0f;
    MEMORY_BARRIER();
    node->slideAttrActive = 1;

    Node_Release(node);
    return TRUE;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_SetSpatialBlend(DWORD handle, float maxBlend) {
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;
    if (maxBlend < 0.0f) maxBlend = 0.0f;
    if (maxBlend > 1.0f) maxBlend = 1.0f;
    node->maxSpatialBlend = maxBlend;
    float blend = node->effectiveDist * node->blendStep;
    node->spatialBlend = (blend > maxBlend) ? maxBlend : blend;
    Node_Release(node);
    return TRUE;
}

// ============================================================================
// ATTACH DSP & STREAM CREATE (HỖ TRỢ MỌI LOẠI KÊNH: STREAM, SAMPLE, MUSIC, RECORD)
// ============================================================================
EXPORT HDSP B_CALLBACK BASS_HRTF_AttachDSP(DWORD channel, int priority) {
    if (!g_Context || !g_HRTF || !channel) return 0;

    BASS_HRTF_Node *existing = AcquireNode(channel);
    if (existing) {
        HDSP h = existing->isDSPMode ? existing->dspHandle : 0;
        Node_Release(existing);
        if (h) return h;
    }

    BASS_CHANNELINFO info;
    DWORD ownedChan = 0;
    DWORD resolvedChan = ResolveChannelOrSample(channel, FALSE, &ownedChan, &info);
    if (!resolvedChan) return 0;
    if (info.chans < 1 || info.chans > 2) return 0;

    BASS_HRTF_Node *node = CreateNodeBase(resolvedChan, info.freq, (int)info.chans);
    if (!node) return 0;
    node->isDSPMode       = 1;
    node->handleKey       = channel;
    node->ownedSampleChan = ownedChan;
    node->isDecodeStream  = (info.flags & BASS_STREAM_DECODE) ? 1 : 0;

    if (fn_BASS_ChannelSetAttribute) {
        fn_BASS_ChannelSetAttribute(resolvedChan, BASS_ATTRIB_GRANULE, (float)node->frameSize);
    }

    if (!RegisterNodeLockFree(node)) {
        Node_Release(node);
        return 0;
    }

    HDSP dsp = fn_BASS_ChannelSetDSP(resolvedChan, HRTF_DSPProc, node, priority);
    if (!dsp) {
        UnregisterNodeLockFree(node);
        return 0;
    }

    node->dspHandle = dsp;
    node->freeSync  = fn_BASS_ChannelSetSync(resolvedChan, BASS_SYNC_FREE, 0, HRTF_FreeSync, node);
    node->posSync   = fn_BASS_ChannelSetSync(resolvedChan, BASS_SYNC_SETPOS, 0, HRTF_PosSync, node);
    return dsp;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_DetachDSP(DWORD channelOrDsp) {
    BASS_HRTF_Node *node = AcquireNode(channelOrDsp);
    if (!node) return FALSE;
    if (!node->isDSPMode) {
        Node_Release(node);
        return FALSE;
    }

    node->isAlive = 0;
    if (node->freeSync && fn_BASS_ChannelRemoveSync) {
        fn_BASS_ChannelRemoveSync(node->sourceChan, node->freeSync);
        node->freeSync = 0;
    }
    if (node->posSync && fn_BASS_ChannelRemoveSync) {
        fn_BASS_ChannelRemoveSync(node->sourceChan, node->posSync);
        node->posSync = 0;
    }
    if (node->dspHandle && fn_BASS_ChannelRemoveDSP) {
        fn_BASS_ChannelRemoveDSP(node->sourceChan, node->dspHandle);
        node->dspHandle = 0;
    }
    if (fn_BASS_ChannelSetAttribute) {
        fn_BASS_ChannelSetAttribute(node->sourceChan, BASS_ATTRIB_GRANULE, 0.0f);
    }

    UnregisterNodeLockFree(node);
    Node_Release(node);
    return TRUE;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_SetEnabled(DWORD handle, BOOL enable) {
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;
    if (!enable && node->enabled) node->needReset = 1;
    node->enabled = enable ? 1 : 0;
    Node_Release(node);
    return TRUE;
}

EXPORT HSTREAM B_CALLBACK BASS_HRTF_StreamCreate(DWORD sourceChan, DWORD flags) {
    if (!g_Context || !g_HRTF || !sourceChan) return 0;

    BASS_CHANNELINFO info;
    DWORD ownedChan = 0;
    DWORD resolvedChan = ResolveChannelOrSample(sourceChan, TRUE, &ownedChan, &info);
    if (!resolvedChan) return 0;
    if (info.chans < 1) return 0;

    BASS_HRTF_Node *node = CreateNodeBase(resolvedChan, info.freq, (int)info.chans);
    if (!node) {
        if (ownedChan && fn_BASS_StreamFree) fn_BASS_StreamFree(ownedChan);
        return 0;
    }
    node->isDSPMode       = 0;
    node->ownedSampleChan = ownedChan;
    node->isDecodeStream  = (flags & BASS_STREAM_DECODE) ? 1 : 0;

    // Nhận diện nguồn âm trực tiếp (Push Stream / Record / Live Voice Chat)
    BOOL isPushOrRecord = (info.ctype == BASS_CTYPE_RECORD || info.ctype == BASS_CTYPE_STREAM);
    if (info.ctype == BASS_CTYPE_STREAM && info.filename == NULL) {
        // Push stream hoặc user stream (filename == NULL)
        isPushOrRecord = TRUE;
    }
    if (info.ctype == BASS_CTYPE_RECORD || (info.ctype == BASS_CTYPE_STREAM && info.filename == NULL)) {
        node->isLiveSource    = 1;
        node->lowLatencyFlush = 1;
    }

    if (!RegisterNodeLockFree(node)) {
        Node_Release(node);
        return 0;
    }

    HSTREAM hrtfStream = fn_BASS_StreamCreate(node->sampleRate, 2, flags | BASS_SAMPLE_FLOAT, HRTF_StreamProc, node);
    if (!hrtfStream) {
        UnregisterNodeLockFree(node);
        return 0;
    }

    node->handleKey = hrtfStream;
    node->freeSync  = fn_BASS_ChannelSetSync(hrtfStream, BASS_SYNC_FREE, 0, HRTF_FreeSync, node);
    node->posSync   = fn_BASS_ChannelSetSync(resolvedChan, BASS_SYNC_SETPOS, 0, HRTF_PosSync, node);
    return hrtfStream;
}

// ============================================================================
// API TỌA ĐỘ 3D & LISTENER
// ============================================================================
EXPORT void B_CALLBACK BASS_HRTF_UpdateListenerEx(float listenerX, float listenerY, float listenerZ, float yawDeg, float pitchDeg, float rollDeg) {
    int posChanged = (fabsf(listenerX - g_ListenerX) > 1e-4f ||
                      fabsf(listenerY - g_ListenerY) > 1e-4f ||
                      fabsf(listenerZ - g_ListenerZ) > 1e-4f) ? 1 : 0;

    g_ListenerX     = listenerX;
    g_ListenerY     = listenerY;
    g_ListenerZ     = listenerZ;
    g_ListenerYaw   = yawDeg;
    g_ListenerPitch = pitchDeg;
    g_ListenerRoll  = rollDeg;

    int limit = g_MaxNodeLimit;
    if (limit > MAX_HRTF_CHANNELS) limit = MAX_HRTF_CHANNELS;

    for (int i = 0; i < limit; ++i) {
        BASS_HRTF_Node *node = g_Nodes[i];
        if (node && node->isAlive) {
            Node_AddRef(node);
            if (node->isAlive && node->useWorldPos && !node->slidePosActive) {
                UpdateNode3DCoordinatesEx(node,
                                          node->worldX - listenerX,
                                          node->worldY - listenerY,
                                          node->worldZ - listenerZ,
                                          yawDeg,
                                          pitchDeg,
                                          posChanged);
            }
            Node_Release(node);
        }
    }
}

EXPORT void B_CALLBACK BASS_HRTF_UpdateListener(float listenerX, float listenerY, float listenerZ, float yawDeg, float pitchDeg) {
    BASS_HRTF_UpdateListenerEx(listenerX, listenerY, listenerZ, yawDeg, pitchDeg, 0.0f);
}

EXPORT BOOL B_CALLBACK BASS_HRTF_SetWorldPosition(DWORD handle, float worldX, float worldY, float worldZ, int bilinear) {
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;

    node->slidePosActive = 0;
    node->worldX = worldX;
    node->worldY = worldY;
    node->worldZ = worldZ;
    node->useWorldPos = 1;
    node->interpolation = bilinear ? 1 : 0;

    UpdateNode3DCoordinatesEx(node,
                              worldX - g_ListenerX,
                              worldY - g_ListenerY,
                              worldZ - g_ListenerZ,
                              g_ListenerYaw,
                              g_ListenerPitch,
                              1);
    Node_Release(node);
    return TRUE;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_SetUser3D(DWORD handle, float userX, float userY, float userZ, float listenerYawDeg, int bilinear) {
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;

    node->slidePosActive = 0;
    node->useWorldPos = 0;
    node->interpolation = bilinear ? 1 : 0;
    UpdateNode3DCoordinatesEx(node, userX, userY, userZ, listenerYawDeg, 0.0f, 1);

    Node_Release(node);
    return TRUE;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_SetPosition3D(DWORD handle, float listenerX, float listenerY, float listenerZ, float sourceX, float sourceY, float sourceZ, float listenerYawDeg, int bilinear) {
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;

    node->slidePosActive = 0;
    node->worldX = sourceX;
    node->worldY = sourceY;
    node->worldZ = sourceZ;
    node->useWorldPos = 1;
    node->interpolation = bilinear ? 1 : 0;

    UpdateNode3DCoordinatesEx(node, sourceX - listenerX, sourceY - listenerY, sourceZ - listenerZ, listenerYawDeg, 0.0f, 1);

    Node_Release(node);
    return TRUE;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_Set3D(DWORD handle, float userX, float userY, float userZ, float attenuation, int bilinear) {
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;

    node->slidePosActive = 0;
    node->useWorldPos = 0;
    if (attenuation >= 0.0f) {
        node->userGain = attenuation;
    }
    UpdateNode3DCoordinatesEx(node, userX, userY, userZ, 0.0f, 0.0f, 1);
    if (attenuation >= 0.0f) {
        node->gainAtten = attenuation;
    }
    node->interpolation = bilinear ? 1 : 0;

    Node_Release(node);
    return TRUE;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_SetPositionAuto(DWORD handle, float userX, float userY, float userZ, float minDist, float maxDist, int bilinear) {
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;

    node->slidePosActive = 0;
    node->useWorldPos = 0;
    node->minDist = (minDist >= 0.0f) ? minDist : g_DefaultMinDist;
    node->maxDist = (maxDist > node->minDist) ? maxDist : (node->minDist + 50.0f);

    UpdateNode3DCoordinatesEx(node, userX, userY, userZ, 0.0f, 0.0f, 1);
    node->interpolation = bilinear ? 1 : 0;

    Node_Release(node);
    return TRUE;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_EnablePhysicalFilter(DWORD handle, BOOL enable) {
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;

    if (enable) {
        if (!EnsureDirectEffectCreated(node)) {
            Node_Release(node);
            return FALSE;
        }
        node->useDirectFilter = 1;
    } else {
        node->useDirectFilter = 0;
    }

    Node_Release(node);
    return TRUE;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_SetPhysicalParams(DWORD handle, float userX, float userY, float userZ, float occlusion, float airAbsorption) {
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;

    if (!EnsureDirectEffectCreated(node)) {
        Node_Release(node);
        return FALSE;
    }

    node->slidePosActive = 0;
    node->useWorldPos = 0;
    UpdateNode3DCoordinatesEx(node, userX, userY, userZ, 0.0f, 0.0f, 1);

    if (occlusion < 0.0f) occlusion = 0.0f;
    if (occlusion > 1.0f) occlusion = 1.0f;
    node->occlusion = occlusion;
    node->airAbsorption = (airAbsorption < 0.0f) ? 0.0f : airAbsorption;
    node->useDirectFilter = 1;

    Node_Release(node);
    return TRUE;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_SetTransmissionBands(DWORD handle, float lowBass, float mid, float highTreble) {
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;
    float tL = (lowBass < 0.0f) ? 0.0f : ((lowBass > 1.0f) ? 1.0f : lowBass);
    float tM = (mid < 0.0f) ? 0.0f : ((mid > 1.0f) ? 1.0f : mid);
    float tH = (highTreble < 0.0f) ? 0.0f : ((highTreble > 1.0f) ? 1.0f : highTreble);
    node->baseTransLow  = tL;
    node->baseTransMid  = tM;
    node->baseTransHigh = tH;
    node->transLow      = tL;
    node->transMid      = tM;
    node->transHigh     = tH;
    Node_Release(node);
    return TRUE;
}

EXPORT BOOL B_CALLBACK BASS_HRTF_Reset(DWORD handle) {
    BASS_HRTF_Node *node = AcquireNode(handle);
    if (!node) return FALSE;
    node->needReset = 1;
    node->slidePosActive = 0;
    node->slideAttrActive = 0;
    Node_Release(node);
    return TRUE;
}

// Đảm bảo MSVC 32-bit (x86) xuất tên hàm sạch không bị mangle (_Func@N), đồng nhất 100% với x64 và ctypes
#if defined(_MSC_VER) && defined(_M_IX86)
    #pragma comment(linker, "/EXPORT:BASS_HRTF_InitEx=_BASS_HRTF_InitEx@20")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_Init=_BASS_HRTF_Init@4")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_Free=_BASS_HRTF_Free@0")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_SetFrameSize=_BASS_HRTF_SetFrameSize@4")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_GetFrameSize=_BASS_HRTF_GetFrameSize@0")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_SetNodeFrameSize=_BASS_HRTF_SetNodeFrameSize@8")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_GetNodeFrameSize=_BASS_HRTF_GetNodeFrameSize@4")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_SetLiveStreamMode=_BASS_HRTF_SetLiveStreamMode@12")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_EnvAddBoxPreset=_BASS_HRTF_EnvAddBoxPreset@28")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_EnvClearBoxes=_BASS_HRTF_EnvClearBoxes@0")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_EnvSetAutoRaycast=_BASS_HRTF_EnvSetAutoRaycast@4")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_SetNodeMaterialPreset=_BASS_HRTF_SetNodeMaterialPreset@8")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_AddSteamFX=_BASS_HRTF_AddSteamFX@12")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_RemoveSteamFX=_BASS_HRTF_RemoveSteamFX@8")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_ClearSteamFX=_BASS_HRTF_ClearSteamFX@4")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_SetFXParam=_BASS_HRTF_SetFXParam@12")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_GetFXParam=_BASS_HRTF_GetFXParam@8")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_EnableReflectionEffect=_BASS_HRTF_EnableReflectionEffect@8")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_SetReflectionParams=_BASS_HRTF_SetReflectionParams@36")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_SetReflectionPreset=_BASS_HRTF_SetReflectionPreset@12")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_EnablePathEffect=_BASS_HRTF_EnablePathEffect@8")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_SetPathParams=_BASS_HRTF_SetPathParams@36")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_EnableVirtualSurround=_BASS_HRTF_EnableVirtualSurround@12")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_EnableAmbisonics=_BASS_HRTF_EnableAmbisonics@12")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_SetPanningMode=_BASS_HRTF_SetPanningMode@8")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_SetDirectivityEx=_BASS_HRTF_SetDirectivityEx@24")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_SetAirAbsorptionBands=_BASS_HRTF_SetAirAbsorptionBands@16")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_SetStereoInput=_BASS_HRTF_SetStereoInput@8")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_SetDistanceModel=_BASS_HRTF_SetDistanceModel@16")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_SetNodeDistanceLimits=_BASS_HRTF_SetNodeDistanceLimits@20")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_SetSourceRadius=_BASS_HRTF_SetSourceRadius@8")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_SetDirectivity=_BASS_HRTF_SetDirectivity@8")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_SlidePosition=_BASS_HRTF_SlidePosition@20")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_SlideAttribute=_BASS_HRTF_SlideAttribute@16")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_SetSpatialBlend=_BASS_HRTF_SetSpatialBlend@8")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_AttachDSP=_BASS_HRTF_AttachDSP@8")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_DetachDSP=_BASS_HRTF_DetachDSP@4")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_SetEnabled=_BASS_HRTF_SetEnabled@8")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_StreamCreate=_BASS_HRTF_StreamCreate@8")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_UpdateListenerEx=_BASS_HRTF_UpdateListenerEx@24")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_UpdateListener=_BASS_HRTF_UpdateListener@20")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_SetWorldPosition=_BASS_HRTF_SetWorldPosition@20")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_SetUser3D=_BASS_HRTF_SetUser3D@24")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_SetPosition3D=_BASS_HRTF_SetPosition3D@36")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_Set3D=_BASS_HRTF_Set3D@24")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_SetPositionAuto=_BASS_HRTF_SetPositionAuto@28")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_EnablePhysicalFilter=_BASS_HRTF_EnablePhysicalFilter@8")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_SetPhysicalParams=_BASS_HRTF_SetPhysicalParams@24")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_SetTransmissionBands=_BASS_HRTF_SetTransmissionBands@16")
    #pragma comment(linker, "/EXPORT:BASS_HRTF_Reset=_BASS_HRTF_Reset@4")
#endif

#ifdef __cplusplus
}
#endif
