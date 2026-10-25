@echo off
setlocal

echo =====================================================================
echo   BUILD SIÊU TỐC BASS_HRTF (x86 va x64) + NÉN PHONON.DLL DƯỚI 10MB
echo =====================================================================

set "WORK_DIR=%~dp0"
cd /d "%WORK_DIR%"

if not exist "release_x64" mkdir "release_x64"
if not exist "release_x86" mkdir "release_x86"

:: 1. Tim vcvarsall.bat cua VS 2022
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"

:: 2. Build bass_hrtf.dll ban 64-bit (Mat 1 giay)
echo.
echo [1/3] Dang build bass_hrtf.dll (64-bit)...
cmd /c "call "%VCVARS%" x64 && cl /nologo /O2 /LD bass_hrtf.c /Fe"release_x64\bass_hrtf.dll""
del /q bass_hrtf.obj release_x64\*.lib release_x64\*.exp 2>nul

:: 3. Build bass_hrtf.dll ban 32-bit (Mat 1 giay)
echo.
echo [2/3] Dang build bass_hrtf.dll (32-bit)...
cmd /c "call "%VCVARS%" x86 && cl /nologo /O2 /LD bass_hrtf.c /Fe"release_x86\bass_hrtf.dll""
del /q bass_hrtf.obj release_x86\*.lib release_x86\*.exp 2>nul

:: 4. Tai UPX + Steam Audio v4.0.2 (Ban chua bi phinh AVX-512) va nen xuong ~4.5 MB
echo.
echo [3/3] Dang tai UPX va nen phonon.dll xuong duoi 10MB...

if not exist "upx.exe" (
    echo [*] Dang tai cong cu nen UPX...
    curl -L -s -o upx.zip "https://github.com/upx/upx/releases/download/v4.2.4/upx-4.2.4-win64.zip"
    tar -xf upx.zip --strip-components=1 "upx-4.2.4-win64/upx.exe"
    del /q upx.zip
)

if not exist "steamaudio_lite.zip" (
    echo [*] Dang tai phonon.dll ban v4.0.2 gọn nhẹ tu GitHub cua Valve...
    curl -L -o steamaudio_lite.zip "https://github.com/ValveSoftware/steam-audio/releases/download/v4.0.2/steamaudio_4.0.2.zip"
)

echo [*] Dang giai nen phonon.dll (x64 va x86)...
tar -xf steamaudio_lite.zip --strip-components=3 -C "release_x64" "steamaudio/lib/windows-vs2017-x64/phonon.dll"
tar -xf steamaudio_lite.zip --strip-components=3 -C "release_x86" "steamaudio/lib/windows-vs2017-x86/phonon.dll"

echo [*] Dang ep dung luong phonon.dll bang UPX...
upx.exe --best --lzma "release_x64\phonon.dll" "release_x86\phonon.dll"

echo.
echo =====================================================================
echo   XONG! KIỂM TRA DUNG LƯỢNG TRONG 2 THƯ MỤC:
echo   - release_x64\ (bass_hrtf.dll + phonon.dll ~4.5 MB)
echo   - release_x86\ (bass_hrtf.dll + phonon.dll ~3.8 MB)
echo =====================================================================
dir /s /b "release_x64\*.dll" "release_x86\*.dll"
pause