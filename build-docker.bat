@echo off
REM One-command cross build via Docker Desktop. Nothing needs to be installed
REM locally except Docker.
REM
REM   build-docker.bat               -- windows-x64 + linux-arm64 (common pair)
REM   build-docker.bat all           -- every target below
REM   build-docker.bat linux-arm64   -- just one
REM
REM Targets: windows-x64 windows-x86 windows-arm64 windows-arm
REM          linux-x64   linux-x86   linux-arm64   linux-arm
REM
REM Artifacts land in .\dist
setlocal
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "IMAGE=betterserver-build"
if not defined BUILD_TYPE set "BUILD_TYPE=Release"
if not defined DIST_DIR set "DIST_DIR=dist"

where docker >nul 2>nul
if errorlevel 1 (
    echo [!] Docker was not found on PATH. Install Docker Desktop and retry.
    exit /b 1
)

docker info >nul 2>nul
if errorlevel 1 (
    echo [!] Docker is installed but the engine is not running. Start Docker Desktop and retry.
    exit /b 1
)

echo [*] Building the toolchain image ^(cached after the first run^)...
docker build -t %IMAGE% -f "%ROOT%\docker\Dockerfile" "%ROOT%\docker"
if errorlevel 1 exit /b 1

if not exist "%ROOT%\%DIST_DIR%" mkdir "%ROOT%\%DIST_DIR%"

echo [*] Cross-compiling...
docker run --rm -v "%ROOT%:/src:ro" -v "%ROOT%\%DIST_DIR%:/out" -e BUILD_TYPE=%BUILD_TYPE% %IMAGE% bash /src/docker/build.sh %*
if errorlevel 1 exit /b 1

echo.
echo [*] Artifacts:
dir /b "%ROOT%\%DIST_DIR%"
endlocal
