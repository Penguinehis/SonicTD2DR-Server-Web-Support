@echo off
setlocal EnableDelayedExpansion

cd /d "%~dp0"

echo.
echo  ============================================================
echo   betterserver-oss  ^|  Auto-Build
echo   Missing tools will be installed automatically.
echo  ============================================================
echo.

set "MSYS2=C:\msys64"
set "PATH=%MSYS2%\ucrt64\bin;%MSYS2%\usr\bin;%PATH%"




set "REPO_ROOT="
set "CHECK=%~dp0"
:search_loop
if exist "!CHECK!CMakeLists.txt" if exist "!CHECK!disasterserver\" (
    set "REPO_ROOT=!CHECK!"
    goto :found_root
)
for %%P in ("!CHECK!..") do set "NEXT=%%~fP\"
if "!NEXT!"=="!CHECK!" goto :root_not_found
set "CHECK=!NEXT!"
goto :search_loop

:root_not_found
echo  [ERROR] Cannot find the repo root.
echo          Make sure build.bat is inside the betterserver-oss folder
echo          (the folder that contains CMakeLists.txt and disasterserver\).
goto :fail

:found_root
echo  Repo   : !REPO_ROOT!
echo.

set "BUILD_DIR=!REPO_ROOT!build"
set "ENET_DIR=!REPO_ROOT!enet"




echo [1/5] MSYS2...
if not exist "%MSYS2%\usr\bin\bash.exe" (
    echo       Not installed. Downloading now ^(one-time, ~100 MB^)...
    echo       Please wait -- this can take a few minutes.
    echo.
    set "MSYS2_EXE=%TEMP%\msys2-installer.exe"
    powershell -NoProfile -Command ^
        "try {" ^
        "  $r = Invoke-RestMethod 'https://api.github.com/repos/msys2/msys2-installer/releases/latest';" ^
        "  $a = $r.assets | Where-Object { $_.name -like '*x86_64*.exe' } | Select-Object -First 1;" ^
        "  Invoke-WebRequest -Uri $a.browser_download_url -OutFile '%MSYS2_EXE%' -UseBasicParsing;" ^
        "  exit 0" ^
        "} catch { exit 1 }"
    if errorlevel 1 (
        echo  [ERROR] Could not download MSYS2 installer.
        echo          Check your internet connection and try again.
        echo          You can also install MSYS2 manually from https://www.msys2.org/
        echo          to C:\msys64, then re-run this script.
        goto :fail
    )
    echo       Running installer silently...
    "%MSYS2_EXE%" in --confirm-command --accept-messages --root "%MSYS2%"
    del /q "%MSYS2_EXE%" 2>nul
    if not exist "%MSYS2%\usr\bin\bash.exe" (
        echo  [ERROR] MSYS2 installation appears to have failed.
        echo          Try installing manually from https://www.msys2.org/ to C:\msys64
        echo          and then re-run this script.
        goto :fail
    )
    echo       MSYS2 installed OK.
) else (
    echo       OK.
)




echo.
echo [2/5] Build tools ^(gcc, cmake, ninja^)...
echo       Syncing packages -- may take a few minutes on first run...
"%MSYS2%\usr\bin\bash.exe" -lc "pacman -Syu --noconfirm 2>&1"
"%MSYS2%\usr\bin\bash.exe" -lc "pacman -S --noconfirm --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja 2>&1"
if errorlevel 1 (
    echo  [ERROR] Could not install build tools.
    echo          Check your internet connection and try again.
    goto :fail
)
where gcc >nul 2>&1
if errorlevel 1 (
    echo  [ERROR] gcc is still not visible after install.
    echo          Close this window, reopen it, and run build.bat again.
    goto :fail
)
echo       Build tools OK.




echo.
echo [3/5] ENet library...
if not exist "!ENET_DIR!\CMakeLists.txt" (
    echo       Not found. Downloading enet from GitHub...
    set "ENET_ZIP=!REPO_ROOT!enet.zip"
    set "ENET_TMP=!REPO_ROOT!enet-tmp"
    powershell -NoProfile -Command ^
        "try {" ^
        "  Invoke-WebRequest -Uri 'https://github.com/lsalzman/enet/archive/refs/heads/master.zip'" ^
        "    -OutFile '!ENET_ZIP!' -UseBasicParsing; exit 0" ^
        "} catch { exit 1 }"
    if errorlevel 1 (
        echo  [ERROR] Could not download enet.
        echo          Check your internet connection, or manually clone:
        echo            git clone https://github.com/lsalzman/enet.git
        echo          into the !REPO_ROOT!enet folder and re-run.
        goto :fail
    )
    if exist "!ENET_TMP!" rmdir /s /q "!ENET_TMP!"
    if exist "!ENET_DIR!"  rmdir /s /q "!ENET_DIR!"
    mkdir "!ENET_TMP!"
    powershell -NoProfile -Command ^
        "try { Expand-Archive -Path '!ENET_ZIP!' -DestinationPath '!ENET_TMP!' -Force; exit 0 } catch { exit 1 }"
    if errorlevel 1 (
        echo  [ERROR] Could not extract enet.zip.
        goto :fail
    )
    for /d %%D in ("!ENET_TMP!\*") do move "%%D" "!REPO_ROOT!enet" >nul 2>&1
    rmdir /s /q "!ENET_TMP!" 2>nul
    del  /q     "!ENET_ZIP!"  2>nul
    if not exist "!ENET_DIR!\CMakeLists.txt" (
        echo  [ERROR] enet extraction failed -- CMakeLists.txt not found inside enet\.
        goto :fail
    )
    echo       ENet downloaded and extracted OK.
) else (
    echo       OK.
)




echo.
echo [4/5] Compiling ^(may take a minute or two on first build^)...
if not exist "!BUILD_DIR!" mkdir "!BUILD_DIR!"

set "SRC=!REPO_ROOT!"
if "!SRC:~-1!"=="\" set "SRC=!SRC:~0,-1!"
set "BLD=!BUILD_DIR!"
if "!BLD:~-1!"=="\" set "BLD=!BLD:~0,-1!"

cmake -S "!SRC!" -B "!BLD!" -G "Ninja" ^
      -DCMAKE_BUILD_TYPE=Release ^
      -DBUILD_UI=OFF ^
      -DCMAKE_C_COMPILER=gcc ^
      -DCMAKE_CXX_COMPILER=g++ ^
      -DCMAKE_EXE_LINKER_FLAGS="-static -static-libgcc -static-libstdc++"
if errorlevel 1 (
    echo  [ERROR] CMake configuration failed.
    echo          Review the messages above for details.
    goto :fail
)

cmake --build "!BLD!" --config Release
if errorlevel 1 (
    echo  [ERROR] Compilation failed.
    echo          Review the messages above for details.
    goto :fail
)




echo.
echo [5/5] Finalising...
set "EXE_FOUND=!BUILD_DIR!\disasterserver\DisasterServer.exe"
if not exist "!EXE_FOUND!" (
    echo  [ERROR] DisasterServer.exe was not found at:
    echo          !EXE_FOUND!
    echo          The build may have failed silently -- check output above.
    goto :fail
)
set "OUTPUT_EXE=!REPO_ROOT!server.exe"
copy /y "!EXE_FOUND!" "!OUTPUT_EXE!" >nul
if errorlevel 1 (
    echo  [ERROR] Could not copy server.exe to the repo root.
    echo          Check that the folder is not read-only.
    goto :fail
)

echo.
echo  ============================================================
echo   BUILD SUCCESSFUL
echo.
echo   server.exe is ready at:
echo   !OUTPUT_EXE!
echo.
echo   Double-click server.exe to launch the server + Admin CLI.
echo   Type help in the console for management commands.
echo   Config.json will be created on first launch.
echo   See BUILD_INSTRUCTIONS.txt for configuration details.
echo  ============================================================
echo.
pause
endlocal
exit /b 0

:fail
echo.
echo  ============================================================
echo   BUILD FAILED
echo   Screenshot this window and include it when asking for help.
echo   See BUILD_INSTRUCTIONS.txt for manual build steps.
echo  ============================================================
echo.
pause
endlocal
exit /b 1
