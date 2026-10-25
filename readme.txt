================================================================================
BASS_HRTF Addon (BASS + Steam Audio Binaural & Direct Sound Wrapper)
================================================================================

OVERVIEW:
---------
BASS_HRTF is a lightweight, high-performance C/C++ wrapper library that bridges 
Un4seen BASS with Valve's Steam Audio SDK (phonon.dll / libphonon.so). It provides 
true HRTF 3D spatialization, distance attenuation, air absorption (frequency-dependent 
treble loss over distance), and occlusion/transmission (muffling behind walls) with 
near-zero CPU overhead and zero memory allocation on the real-time audio thread.

FEATURES:
---------
- True Head-Related Transfer Function (HRTF) supporting 360-degree positioning 
  (including elevation/height: top-to-bottom and front-to-back).
- Direct Sound Physics: Real-time air absorption and wall occlusion (muffling).
- Un4seen Ecosystem Compatible: Wraps any Mono Decode BASS stream into a standard 
  HSTREAM that can be fed directly into bass_fx or bassmix.
- Standalone / Dynamic Linking: Does not require header compilation locks or .lib 
  files during build; automatically resolves function symbols at runtime.
- Cross-Platform Support: Windows (x86/x64) and Android (ARMv7/ARM64).


BUILD INSTRUCTIONS:
-------------------
1. Windows (Visual Studio Build Tools):
   Open 'x64 Native Tools Command Prompt for VS 2022' (or x86 for 32-bit):
   
   cl /O2 /LD bass_hrtf.c /Febass_hrtf.dll

2. Android (NDK Clang):
   aarch64-linux-android21-clang -shared -fPIC -O3 bass_hrtf.c -o libbass_hrtf.so -lm -ldl


API REFERENCE & PARAMETERS:
---------------------------
1. BASS_HRTF_Init(int sampleRate)
   - Description: Initializes the global Steam Audio context and default HRTF.
   - Parameters:
     * sampleRate (int): Audio device sampling rate (e.g., 44100, 48000).
   - Returns: BOOL (TRUE/FALSE)

2. BASS_HRTF_InitEx(int sampleRate, const char* bassPath, const char* phononPath, const char* sofaPath, uint32_t version)
   - Description: Advanced initialization with custom DLL paths and .SOFA profile support.
   - Parameters:
     * sampleRate (int): Sampling rate.
     * bassPath (const char*): Path to bass.dll / libbass.so (or NULL for default).
     * phononPath (const char*): Path to phonon.dll / libphonon.so (or NULL).
     * sofaPath (const char*): Path to custom .SOFA HRTF profile file (or NULL for default).
     * version (uint32_t): Steam Audio SDK version integer (or 0 for auto-detect).
   - Returns: BOOL

3. BASS_HRTF_StreamCreate(DWORD sourceChan, DWORD flags)
   - Description: Wraps a BASS decode stream into an HRTF-processed stereo stream.
   - Parameters:
     * sourceChan (DWORD): BASS handle of a MONO, 32-bit FLOAT, DECODE stream.
     * flags (DWORD): Standard BASS stream flags (e.g., BASS_SAMPLE_LOOP).
   - Returns: HSTREAM (New stereo HRTF stream handle, or 0 on failure).

4. BASS_HRTF_Set3D(HSTREAM hrtfStream, float relX, float relY, float relZ, float attenuation, int bilinear)
   - Description: Instantly updates the 3D relative position and distance volume.
   - Parameters:
     * hrtfStream (HSTREAM): The HRTF stream handle.
     * relX (float): Relative X coordinate (+Right, -Left).
     * relY (float): Relative Y coordinate (+Up, -Down / Elevation).
     * relZ (float): Relative Z coordinate (-Front, +Back in OpenGL/SteamAudio coords).
     * attenuation (float): Volume multiplier / distance attenuation factor (0.0 to 1.0+).
     * bilinear (int): HRTF interpolation mode (0 = Nearest for low CPU, 1 = Bilinear for smooth rotation).
   - Returns: BOOL

5. BASS_HRTF_SetPositionAuto(HSTREAM hrtfStream, float relX, float relY, float relZ, float minDist, float maxDist, int bilinear)
   - Description: Updates 3D position and automatically calculates inverse-distance attenuation.
   - Parameters:
     * hrtfStream (HSTREAM): HRTF stream handle.
     * relX, relY, relZ (float): Relative position coordinates.
     * minDist (float): Distance below which volume is at 100%.
     * maxDist (float): Distance beyond which volume drops to 0%.
     * bilinear (int): 0 for Nearest, 1 for Bilinear interpolation.
   - Returns: BOOL

6. BASS_HRTF_SetPhysicalParams(HSTREAM hrtfStream, float relX, float relY, float relZ, float occlusion, float airAbsorption)
   - Description: Comprehensive physics update including wall occlusion and atmospheric air absorption.
   - Parameters:
     * hrtfStream (HSTREAM): HRTF stream handle.
     * relX, relY, relZ (float): Relative 3D position vectors.
     * occlusion (float): Occlusion/blocking factor (0.0 = clear, 1.0 = fully blocked behind thick wall).
     * airAbsorption (float): Environmental absorption factor (0.0 = disabled, 1.0 = standard air, >1.0 = heavy fog/muffle).
   - Returns: BOOL

7. BASS_HRTF_Reset(HSTREAM hrtfStream)
   - Description: Resets internal effect buffers (Call this when seeking/rewinding the source stream).
   - Parameters:
     * hrtfStream (HSTREAM): HRTF stream handle.
   - Returns: BOOL

8. BASS_HRTF_Free(void)
   - Description: Releases global Steam Audio resources and contexts.
   - Parameters: None.
   - Returns: Void


PYTHON INTEGRATION EXAMPLE:
-------------------------
import ctypes

bass = ctypes.WinDLL("./bass.dll")
hrtf = ctypes.WinDLL("./bass_hrtf.dll")

# Initialize
bass.BASS_Init(-1, 44100, 0, 0, None)
hrtf.BASS_HRTF_Init(44100)

# Create decode source stream (Must be MONO, FLOAT, DECODE)
source = bass.BASS_StreamCreateFile(False, b"sound.wav", 0, 0, 0x100 | 0x2 | 0x200000)

# Create HRTF stream
hrtf_stream = hrtf.BASS_HRTF_StreamCreate(source, 0)
bass.BASS_ChannelPlay(hrtf_stream, False)

# Update position and physical parameters in real-time loop
# X=2.0 (Right), Y=0.0 (Ear level), Z=-3.0 (In front), Occlusion=0.5 (Behind partial wall)
hrtf.BASS_HRTF_SetPhysicalParams(hrtf_stream, 2.0, 0.0, -3.0, 0.5, 1.0)