@echo off
REM ===========================================================================
REM build.bat - build the bridge on Windows with MinGW g++ (w64devkit).
REM
REM WARNING (ABI):
REM   This bridge DLL (`bambu_networking.dll`) is built by MSVC.  MinGW g++'s
REM   std::string / std::function layout is NOT compatible with MSVC's, so calls
REM   that pass C++ types across the plugin boundary will fail or crash.
REM   To use the Windows plugin correctly, build with MSVC (cl.exe) instead.
REM   This script builds the bridge but the plugin will only link at runtime if
REM   you rebuild with MSVC.  See MSVC note in README.
REM ===========================================================================
setlocal

set CXX=%1
if "%CXX%"=="" set CXX=g++

if not defined CODEC_PREFIX (
    >&2 echo [build] Set CODEC_PREFIX to the MinGW prefix with the static OpenH264 and libjpeg libraries.
    exit /b 1
)

set OUT=build\bambu-bridge.exe

if not exist build mkdir build

echo [build] %CXX% -std=c++17 -> %OUT%
%CXX% -std=c++17 -O2 -Wall -Wextra ^
    -Iinclude -Isrc -I"%CODEC_PREFIX%\include" ^
    src\main.cpp src\bridge.cpp src\plugin_loader.cpp src\json.cpp ^
    src\rpc_handler.cpp src\stdio_transport.cpp src\http_server.cpp src\camera_stream.cpp ^
    src\http_web_api.cpp src\http_websocket.cpp ^
    src\mcp_stdio.cpp ^
    "%CODEC_PREFIX%\lib\libopenh264.a" "%CODEC_PREFIX%\lib\libjpeg.a" -lws2_32 ^
    -o %OUT%

if errorlevel 1 ( echo [build] FAILED & exit /b 1 )
echo [build] OK: %OUT%
echo [build] MinGW build only. Rebuild with MSVC x64 before using run.bat.

endlocal
