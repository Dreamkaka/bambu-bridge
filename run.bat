@echo off
setlocal
if exist "%~dp0bambu-bridge.exe" (
    "%~dp0bambu-bridge.exe" --plugin-dir "%~dp0plugins" --http 8080 --no-stdio %*
    exit /b
)

set "BRIDGE=%~dp0build\bambu-bridge.exe"
set "PLUGIN_DIR=%~dp0open_bambu_networking_plugin\bin\win_x64"

if not exist "%BRIDGE%" (
    >&2 echo [bridge] Build build\bambu-bridge.exe with MSVC x64 first. See README.md.
    exit /b 1
)
if not exist "%PLUGIN_DIR%\bambu_networking.dll" (
    >&2 echo [bridge] Open Bambu networking plugin not found: "%PLUGIN_DIR%"
    exit /b 1
)

"%BRIDGE%" --plugin-dir "%PLUGIN_DIR%" --http 8080 --no-stdio --web-api %*
exit /b %errorlevel%
