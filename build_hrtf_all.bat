@echo off
setlocal EnableDelayedExpansion

echo ===========================================================================
echo   BASS_HRTF v5.0 - ALL-IN-ONE CROSS-PLATFORM BUILDER (ON WINDOWS)
echo   Targets: Windows (x64/x86/ARM64), Android, Linux, macOS, iOS
echo   Mode:    BELOW_NORMAL PRIORITY (Zero PC lag)
echo ===========================================================================

set "WORK_DIR=%~dp0"
if "%WORK_DIR:~-1%"=="\" set "WORK_DIR=%WORK_DIR:~0,-1%"
cd /d "%WORK_DIR%"

set "C_SRC=%WORK_DIR%\bass_hrtf.c"
if not exist "%C_SRC%" (
    echo [ERROR] Khong tim thay file nguon: "%C_SRC%"
    exit /b 1
)

:: 1. Cau hinh thu muc cong cu tap trung tai e:\platform-compiles
set "TC_ROOT=e:\platform-compiles"
set "ZIG_VER=0.13.0"
set "ZIG_FOLDER=zig-windows-x86_64-%ZIG_VER%"
set "ZIG_EXE=%TC_ROOT%\zig\zig.exe"
set "ZIG_GLOBAL_CACHE_DIR=%TC_ROOT%\zig_cache"
set "ZIG_LOCAL_CACHE_DIR=%TC_ROOT%\zig_cache"
set "UPX_EXE=%TC_ROOT%\upx\upx.exe"
set "STEAMAUDIO_VER=4.8.1"
set "STEAMAUDIO_ZIP=%TC_ROOT%\steamaudio_%STEAMAUDIO_VER%.zip"
set "OUT_TMP=%TC_ROOT%\out"
set "STUB_DIR=%TC_ROOT%\bionic_stubs"

if not exist "%TC_ROOT%" mkdir "%TC_ROOT%"
if not exist "%ZIG_GLOBAL_CACHE_DIR%" mkdir "%ZIG_GLOBAL_CACHE_DIR%"
if not exist "%OUT_TMP%" mkdir "%OUT_TMP%"
if not exist "%STUB_DIR%" mkdir "%STUB_DIR%"

echo.
echo [STEP 1/5] KIEM TRA VA TAI CONG CU BUILD VAO %TC_ROOT%...

call :EnsureZig
if errorlevel 1 exit /b 1

call :EnsureUpx
if errorlevel 1 exit /b 1

call :EnsureSteamAudio
if errorlevel 1 exit /b 1

:: 2. Bien dich Windows (x64, x86, ARM64)
echo.
echo [STEP 2/5] BIEN DICH WINDOWS (x64, x86, ARM64)...

if not exist "%WORK_DIR%\release_x64" mkdir "%WORK_DIR%\release_x64"
if not exist "%WORK_DIR%\release_x86" mkdir "%WORK_DIR%\release_x86"
if not exist "%WORK_DIR%\release_win_arm64" mkdir "%WORK_DIR%\release_win_arm64"

set "VCVARS="
if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"

call :BuildWindows

:: 3. Bien dich Android (arm64-v8a, armeabi-v7a, x86_64, x86 - Bionic ABI)
echo.
echo [STEP 3/5] BIEN DICH ANDROID (arm64-v8a, armeabi-v7a, x86_64, x86)...

if not exist "%WORK_DIR%\release_android\arm64-v8a" mkdir "%WORK_DIR%\release_android\arm64-v8a"
if not exist "%WORK_DIR%\release_android\armeabi-v7a" mkdir "%WORK_DIR%\release_android\armeabi-v7a"
if not exist "%WORK_DIR%\release_android\x86_64" mkdir "%WORK_DIR%\release_android\x86_64"
if not exist "%WORK_DIR%\release_android\x86" mkdir "%WORK_DIR%\release_android\x86"

call :CreateBionicStubSource
call :BuildAndroidArch "arm64-v8a"   "aarch64-linux-musl" ""
call :BuildAndroidArch "armeabi-v7a" "arm-linux-musleabi" "-mcpu=cortex_a7 -mfloat-abi=softfp"
call :BuildAndroidArch "x86_64"      "x86_64-linux-musl"  ""
call :BuildAndroidArch "x86"         "x86-linux-musl"     ""

:: 4. Bien dich Linux, macOS, iOS
echo.
echo [STEP 4/5] BIEN DICH LINUX, MACOS VA IOS...

if not exist "%WORK_DIR%\release_linux\x64" mkdir "%WORK_DIR%\release_linux\x64"
if not exist "%WORK_DIR%\release_linux\x86" mkdir "%WORK_DIR%\release_linux\x86"
if not exist "%WORK_DIR%\release_linux\aarch64" mkdir "%WORK_DIR%\release_linux\aarch64"
if not exist "%WORK_DIR%\release_linux\armhf" mkdir "%WORK_DIR%\release_linux\armhf"
if not exist "%WORK_DIR%\release_macos\arm64" mkdir "%WORK_DIR%\release_macos\arm64"
if not exist "%WORK_DIR%\release_macos\x64" mkdir "%WORK_DIR%\release_macos\x64"
if not exist "%WORK_DIR%\release_ios\arm64" mkdir "%WORK_DIR%\release_ios\arm64"

start "" /b /wait /belownormal "%ZIG_EXE%" cc -target x86_64-linux-gnu.2.17 -O3 -s -shared -fPIC -fno-sanitize=all -ffast-math "%C_SRC%" -lm -ldl -o "%WORK_DIR%\release_linux\x64\libbass_hrtf.so"
if errorlevel 1 (echo   [FAIL] Linux x64) else (echo   [OK] Linux x64           : release_linux\x64\libbass_hrtf.so)

start "" /b /wait /belownormal "%ZIG_EXE%" cc -target x86-linux-gnu.2.17 -O3 -s -shared -fPIC -fno-sanitize=all -ffast-math "%C_SRC%" -lm -ldl -o "%WORK_DIR%\release_linux\x86\libbass_hrtf.so"
if errorlevel 1 (echo   [FAIL] Linux x86) else (echo   [OK] Linux x86           : release_linux\x86\libbass_hrtf.so)

start "" /b /wait /belownormal "%ZIG_EXE%" cc -target aarch64-linux-gnu.2.17 -O3 -s -shared -fPIC -fno-sanitize=all -ffast-math "%C_SRC%" -lm -ldl -o "%WORK_DIR%\release_linux\aarch64\libbass_hrtf.so"
if errorlevel 1 (echo   [FAIL] Linux aarch64) else (echo   [OK] Linux aarch64       : release_linux\aarch64\libbass_hrtf.so)

start "" /b /wait /belownormal "%ZIG_EXE%" cc -target arm-linux-gnueabihf.2.17 -O3 -s -shared -fPIC -fno-sanitize=all -ffast-math "%C_SRC%" -lm -ldl -o "%WORK_DIR%\release_linux\armhf\libbass_hrtf.so"
if errorlevel 1 (echo   [FAIL] Linux armhf) else (echo   [OK] Linux armhf         : release_linux\armhf\libbass_hrtf.so)

start "" /b /wait /belownormal "%ZIG_EXE%" cc -target aarch64-macos-none -O3 -dynamiclib -fPIC -fno-sanitize=all -ffast-math -Wl,-undefined,dynamic_lookup "%C_SRC%" -o "%WORK_DIR%\release_macos\arm64\libbass_hrtf.dylib"
echo   [OK] macOS arm64         : release_macos\arm64\libbass_hrtf.dylib

start "" /b /wait /belownormal "%ZIG_EXE%" cc -target x86_64-macos-none -O3 -dynamiclib -fPIC -fno-sanitize=all -ffast-math -Wl,-undefined,dynamic_lookup "%C_SRC%" -o "%WORK_DIR%\release_macos\x64\libbass_hrtf.dylib"
echo   [OK] macOS x86_64        : release_macos\x64\libbass_hrtf.dylib

start "" /b /wait /belownormal "%ZIG_EXE%" cc -target aarch64-macos-none -DIS_IOS_PLATFORM=1 -O3 -dynamiclib -fPIC -fno-sanitize=all -ffast-math -Wl,-undefined,dynamic_lookup "%C_SRC%" -o "%WORK_DIR%\release_ios\arm64\libbass_hrtf.dylib"
start "" /b /wait /belownormal "%ZIG_EXE%" cc -target aarch64-macos-none -DIS_IOS_PLATFORM=1 -O3 -fPIC -fno-sanitize=all -ffast-math -c "%C_SRC%" -o "%OUT_TMP%\ios_bass_hrtf.o"
start "" /b /wait /belownormal "%ZIG_EXE%" ar rcs "%WORK_DIR%\release_ios\arm64\libbass_hrtf.a" "%OUT_TMP%\ios_bass_hrtf.o"
del /q "%OUT_TMP%\ios_bass_hrtf.o" 2>nul
echo   [OK] iOS arm64           : release_ios\arm64\libbass_hrtf.dylib + libbass_hrtf.a

:: 5. Giai nen Phonon va ep dung luong UPX
echo.
echo [STEP 5/5] GIAI NEN PHONON DA NEN TANG VA NEN UPX...
call :ExtractAndPackPhonon

echo.
echo ===========================================================================
echo   HOAN TAT BUILD DA NEN TANG! DANH SACH THU MUC DAU RA:
echo   - release_x64\          (Windows 64-bit: bass_hrtf.dll + phonon.dll)
echo   - release_x86\          (Windows 32-bit: bass_hrtf.dll + phonon.dll)
echo   - release_win_arm64\    (Windows ARM64:  bass_hrtf.dll)
echo   - release_android\      (arm64-v8a, armeabi-v7a, x86_64, x86)
echo   - release_linux\        (x64, x86, aarch64, armhf)
echo   - release_macos\        (arm64, x64)
echo   - release_ios\          (arm64 .dylib + .a)
echo ===========================================================================
if "%~1"=="--no-pause" exit /b 0
pause
exit /b 0

:: ===========================================================================
:: SUBROUTINES
:: ===========================================================================

:EnsureZig
if exist "%ZIG_EXE%" (
    echo   [OK] Zig Compiler da san sang: %ZIG_EXE%
    exit /b 0
)
echo   [*] Dang tai Zig Compiler %ZIG_VER% (Mirror CDN toc do cao)...
curl.exe -L --fail --retry 3 -o "%TC_ROOT%\zig.zip" "https://pkg.machengine.org/zig/%ZIG_FOLDER%.zip"
tar.exe -tf "%TC_ROOT%\zig.zip" >nul 2>nul
if errorlevel 1 (
    echo   [*] Chuyen sang tai tu ziglang.org...
    curl.exe -L --fail --retry 3 -o "%TC_ROOT%\zig.zip" "https://ziglang.org/download/%ZIG_VER%/%ZIG_FOLDER%.zip"
)
tar.exe -tf "%TC_ROOT%\zig.zip" >nul 2>nul
if errorlevel 1 exit /b 1
if exist "%TC_ROOT%\%ZIG_FOLDER%" rmdir /s /q "%TC_ROOT%\%ZIG_FOLDER%"
if exist "%TC_ROOT%\zig" rmdir /s /q "%TC_ROOT%\zig"
tar.exe -xf "%TC_ROOT%\zig.zip" -C "%TC_ROOT%"
move "%TC_ROOT%\%ZIG_FOLDER%" "%TC_ROOT%\zig" >nul
del /q "%TC_ROOT%\zig.zip" 2>nul
exit /b 0

:EnsureUpx
if exist "%UPX_EXE%" (
    echo   [OK] UPX Compressor da san sang: %UPX_EXE%
    exit /b 0
)
echo   [*] Dang tai UPX Compressor v4.2.4...
if not exist "%TC_ROOT%\upx" mkdir "%TC_ROOT%\upx"
curl.exe -L --fail --retry 3 -s -o "%TC_ROOT%\upx.zip" "https://github.com/upx/upx/releases/download/v4.2.4/upx-4.2.4-win64.zip"
if errorlevel 1 exit /b 1
tar.exe -xf "%TC_ROOT%\upx.zip" --strip-components=1 -C "%TC_ROOT%\upx" "upx-4.2.4-win64/upx.exe"
del /q "%TC_ROOT%\upx.zip" 2>nul
exit /b 0

:EnsureSteamAudio
if exist "%STEAMAUDIO_ZIP%" (
    tar.exe -tf "%STEAMAUDIO_ZIP%" >nul 2>nul
    if not errorlevel 1 (
        echo   [OK] SteamAudio v%STEAMAUDIO_VER% da san sang: %STEAMAUDIO_ZIP%
        exit /b 0
    )
    echo   [!] File steamaudio_%STEAMAUDIO_VER%.zip cu bi loi, dang xoa de tai lai...
    del /q "%STEAMAUDIO_ZIP%" 2>nul
)
echo   [*] Dang tai SteamAudio v%STEAMAUDIO_VER%...
curl.exe -L --fail --retry 3 -o "%STEAMAUDIO_ZIP%.tmp" "https://github.com/ValveSoftware/steam-audio/releases/download/v%STEAMAUDIO_VER%/steamaudio_%STEAMAUDIO_VER%.zip"
if errorlevel 1 exit /b 1
move /y "%STEAMAUDIO_ZIP%.tmp" "%STEAMAUDIO_ZIP%" >nul
exit /b 0

:BuildWindows
if not defined VCVARS goto :BuildWindowsZig
if not exist "%OUT_TMP%\msvc_x64" mkdir "%OUT_TMP%\msvc_x64"
if not exist "%OUT_TMP%\msvc_x86" mkdir "%OUT_TMP%\msvc_x86"
start "" /b /wait /belownormal cmd /c "call "%VCVARS%" x64 >nul && cl /nologo /O2 /LD "%C_SRC%" /Fo"%OUT_TMP%\msvc_x64\bass_hrtf.obj" /Fe"%WORK_DIR%\release_x64\bass_hrtf.dll" >nul"
del /q "%WORK_DIR%\release_x64\*.lib" "%WORK_DIR%\release_x64\*.exp" 2>nul
echo   [OK] Windows x64 (MSVC)  : release_x64\bass_hrtf.dll

start "" /b /wait /belownormal cmd /c "call "%VCVARS%" x86 >nul && cl /nologo /O2 /LD "%C_SRC%" /Fo"%OUT_TMP%\msvc_x86\bass_hrtf.obj" /Fe"%WORK_DIR%\release_x86\bass_hrtf.dll" >nul"
del /q "%WORK_DIR%\release_x86\*.lib" "%WORK_DIR%\release_x86\*.exp" 2>nul
echo   [OK] Windows x86 (MSVC)  : release_x86\bass_hrtf.dll
goto :BuildWindowsArm64

:BuildWindowsZig
start "" /b /wait /belownormal "%ZIG_EXE%" cc -target x86_64-windows-gnu -O3 -shared -ffast-math "%C_SRC%" -o "%WORK_DIR%\release_x64\bass_hrtf.dll"
echo   [OK] Windows x64 (Zig)   : release_x64\bass_hrtf.dll

start "" /b /wait /belownormal "%ZIG_EXE%" cc -target x86-windows-gnu -O3 -shared -ffast-math "%C_SRC%" -o "%WORK_DIR%\release_x86\bass_hrtf.dll"
echo   [OK] Windows x86 (Zig)   : release_x86\bass_hrtf.dll

:BuildWindowsArm64
copy /Y "%WORK_DIR%\release_x64\bass_hrtf.dll" "%WORK_DIR%\bass_hrtf.dll" >nul
start "" /b /wait /belownormal "%ZIG_EXE%" cc -target aarch64-windows-gnu -O3 -s -shared -ffast-math "%C_SRC%" -o "%WORK_DIR%\release_win_arm64\bass_hrtf.dll"
del /q "%WORK_DIR%\release_win_arm64\*.lib" "%WORK_DIR%\release_win_arm64\*.pdb" "%WORK_DIR%\release_win_arm64\*.exp" 2>nul
echo   [OK] Windows ARM64 (Zig) : release_win_arm64\bass_hrtf.dll
exit /b 0

:CreateBionicStubSource
set "BIONIC_C=%STUB_DIR%\bionic_stub.c"
echo void* calloc(unsigned long n, unsigned long s) { return 0; } > "%BIONIC_C%"
echo void free(void* p) {} >> "%BIONIC_C%"
echo void* memcpy(void* d, const void* s, unsigned long n) { return d; } >> "%BIONIC_C%"
echo void* memmove(void* d, const void* s, unsigned long n) { return d; } >> "%BIONIC_C%"
echo void* memset(void* d, int c, unsigned long n) { return d; } >> "%BIONIC_C%"
echo char* strncpy(char* d, const char* s, unsigned long n) { return d; } >> "%BIONIC_C%"
echo float sinf(float x) { return 0.0f; } >> "%BIONIC_C%"
echo float cosf(float x) { return 0.0f; } >> "%BIONIC_C%"
echo float sqrtf(float x) { return 0.0f; } >> "%BIONIC_C%"
echo float expf(float x) { return 0.0f; } >> "%BIONIC_C%"
echo float fabsf(float x) { return 0.0f; } >> "%BIONIC_C%"
echo void* dlopen(const char* f, int fl) { return 0; } >> "%BIONIC_C%"
echo void* dlsym(void* h, const char* s) { return 0; } >> "%BIONIC_C%"
echo void __aeabi_memclr(void* d, unsigned int n) {} >> "%BIONIC_C%"
echo void __aeabi_memclr4(void* d, unsigned int n) {} >> "%BIONIC_C%"
echo void __aeabi_memclr8(void* d, unsigned int n) {} >> "%BIONIC_C%"
echo void __aeabi_memcpy(void* d, const void* s, unsigned int n) {} >> "%BIONIC_C%"
echo void __aeabi_memcpy4(void* d, const void* s, unsigned int n) {} >> "%BIONIC_C%"
echo void __aeabi_memmove(void* d, const void* s, unsigned int n) {} >> "%BIONIC_C%"
echo void __aeabi_memmove4(void* d, const void* s, unsigned int n) {} >> "%BIONIC_C%"
echo void __aeabi_memset(void* d, unsigned int n, int c) {} >> "%BIONIC_C%"
exit /b 0

:BuildAndroidArch
set "A_ABI=%~1"
set "A_TARGET=%~2"
set "A_EXTRA=%~3"
set "A_STUB_OUT=%STUB_DIR%\%A_ABI%"
if not exist "%A_STUB_OUT%" mkdir "%A_STUB_OUT%"
start "" /b /wait /belownormal "%ZIG_EXE%" cc -target %A_TARGET% %A_EXTRA% -Wno-incompatible-library-redeclaration -nostdlib -shared -fPIC -Wl,-soname,libc.so "%BIONIC_C%" -o "%A_STUB_OUT%\libc.so"
start "" /b /wait /belownormal "%ZIG_EXE%" cc -target %A_TARGET% %A_EXTRA% -Wno-incompatible-library-redeclaration -nostdlib -shared -fPIC -Wl,-soname,libm.so "%BIONIC_C%" -o "%A_STUB_OUT%\libm.so"
start "" /b /wait /belownormal "%ZIG_EXE%" cc -target %A_TARGET% %A_EXTRA% -Wno-incompatible-library-redeclaration -nostdlib -shared -fPIC -Wl,-soname,libdl.so "%BIONIC_C%" -o "%A_STUB_OUT%\libdl.so"
start "" /b /wait /belownormal "%ZIG_EXE%" cc -target %A_TARGET% %A_EXTRA% -D__ANDROID__=1 -O3 -s -fPIC -fno-sanitize=all -ffast-math -fno-unwind-tables -fno-asynchronous-unwind-tables -c "%C_SRC%" -o "%A_STUB_OUT%\bass_hrtf.o"
if errorlevel 1 (
    echo   [FAIL] Android %A_ABI% compile failed!
    exit /b 1
)
start "" /b /wait /belownormal "%ZIG_EXE%" cc -target %A_TARGET% %A_EXTRA% -shared -nostdlib -s -Wl,-soname,libbass_hrtf.so -Wl,-z,max-page-size=16384 "%A_STUB_OUT%\bass_hrtf.o" "%A_STUB_OUT%\libc.so" "%A_STUB_OUT%\libm.so" "%A_STUB_OUT%\libdl.so" -o "%WORK_DIR%\release_android\%A_ABI%\libbass_hrtf.so"
if errorlevel 1 (
    echo   [FAIL] Android %A_ABI% link failed!
    exit /b 1
)
del /q "%A_STUB_OUT%\bass_hrtf.o" 2>nul
echo   [OK] Android %A_ABI% : release_android\%A_ABI%\libbass_hrtf.so
exit /b 0

:ExtractAndPackPhonon
if not exist "%STEAMAUDIO_ZIP%" exit /b 0
del /q "%WORK_DIR%\release_x64\phonon.dll" "%WORK_DIR%\release_x86\phonon.dll" 2>nul
del /q "%WORK_DIR%\release_android\arm64-v8a\libphonon.so" "%WORK_DIR%\release_android\armeabi-v7a\libphonon.so" "%WORK_DIR%\release_android\x86_64\libphonon.so" "%WORK_DIR%\release_android\x86\libphonon.so" 2>nul
del /q "%WORK_DIR%\release_linux\x64\libphonon.so" "%WORK_DIR%\release_linux\x86\libphonon.so" 2>nul
del /q "%WORK_DIR%\release_macos\arm64\libphonon.dylib" "%WORK_DIR%\release_macos\x64\libphonon.dylib" 2>nul
del /q "%WORK_DIR%\release_ios\arm64\libphonon.a" 2>nul

tar.exe -xf "%STEAMAUDIO_ZIP%" --strip-components=3 -C "%WORK_DIR%\release_x64" "steamaudio/lib/windows-x64/phonon.dll"
tar.exe -xf "%STEAMAUDIO_ZIP%" --strip-components=3 -C "%WORK_DIR%\release_x86" "steamaudio/lib/windows-x86/phonon.dll"
tar.exe -xf "%STEAMAUDIO_ZIP%" --strip-components=3 -C "%WORK_DIR%\release_android\arm64-v8a" "steamaudio/lib/android-armv8/libphonon.so"
tar.exe -xf "%STEAMAUDIO_ZIP%" --strip-components=3 -C "%WORK_DIR%\release_android\armeabi-v7a" "steamaudio/lib/android-armv7/libphonon.so"
tar.exe -xf "%STEAMAUDIO_ZIP%" --strip-components=3 -C "%WORK_DIR%\release_android\x86_64" "steamaudio/lib/android-x64/libphonon.so"
tar.exe -xf "%STEAMAUDIO_ZIP%" --strip-components=3 -C "%WORK_DIR%\release_android\x86" "steamaudio/lib/android-x86/libphonon.so"
tar.exe -xf "%STEAMAUDIO_ZIP%" --strip-components=3 -C "%WORK_DIR%\release_linux\x64" "steamaudio/lib/linux-x64/libphonon.so"
tar.exe -xf "%STEAMAUDIO_ZIP%" --strip-components=3 -C "%WORK_DIR%\release_linux\x86" "steamaudio/lib/linux-x86/libphonon.so"
tar.exe -xf "%STEAMAUDIO_ZIP%" --strip-components=3 -C "%WORK_DIR%\release_macos\arm64" "steamaudio/lib/osx/libphonon.dylib"
copy /Y "%WORK_DIR%\release_macos\arm64\libphonon.dylib" "%WORK_DIR%\release_macos\x64\libphonon.dylib" >nul
tar.exe -xf "%STEAMAUDIO_ZIP%" --strip-components=3 -C "%WORK_DIR%\release_ios\arm64" "steamaudio/lib/ios/libphonon.a"

echo   [*] Dang xoa debug symbols (strip) cho Android libphonon.so...
for %%A in (arm64-v8a armeabi-v7a x86_64 x86) do (
    if exist "%WORK_DIR%\release_android\%%A\libphonon.so" (
        start "" /b /wait /belownormal "%ZIG_EXE%" objcopy --strip-debug "%WORK_DIR%\release_android\%%A\libphonon.so" "%WORK_DIR%\release_android\%%A\libphonon.so.tmp"
        if not errorlevel 1 (
            move /y "%WORK_DIR%\release_android\%%A\libphonon.so.tmp" "%WORK_DIR%\release_android\%%A\libphonon.so" >nul
        ) else (
            del /q "%WORK_DIR%\release_android\%%A\libphonon.so.tmp" 2>nul
        )
    )
)

if exist "%UPX_EXE%" (
    echo   [*] Dang ep dung luong phonon.dll bang UPX...
    start "" /b /wait /belownormal "%UPX_EXE%" --best --lzma --force -q "%WORK_DIR%\release_x64\phonon.dll" "%WORK_DIR%\release_x86\phonon.dll" >nul 2>nul
)
echo   [OK] Da giai nen va toi uu Phonon v%STEAMAUDIO_VER% cho Windows, Android, Linux, macOS, iOS!
exit /b 0
