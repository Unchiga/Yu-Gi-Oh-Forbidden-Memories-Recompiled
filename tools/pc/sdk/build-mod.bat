@echo off
rem build-mod.bat: build a Memories code mod (every .c in its folder) into its
rem objects. Double-click it, or drag the mod's folder onto it, or run it:
rem
rem     build-mod.bat my-mod                  the objects: my-mod\NAME.o, NAME.x86_64-windows.o, NAME.aarch64-translated.o
rem     build-mod.bat my-mod --target i386    anything after the folder goes to tools\build_mod.py
rem     build-mod.bat --setup                 only fetch the tools (below), then stop
rem
rem Needs nothing installed, on Windows 10 version 1803 or newer (64-bit: the
rem downloads are x64 programs). The first run downloads, into the toolchain folder
rem beside this file, a Python (the embeddable package from python.org) and the
rem compiler (llvm-mingw, the toolchain the game is built with: clang, ld.lld,
rem llvm-objcopy), checks each file's SHA-256 against the hash written below,
rem and unpacks it. Later runs find them there and download nothing. Delete the
rem toolchain folder to start over. Nothing else is downloaded, installed or
rem changed on your computer. It uses only the curl, tar and certutil programs
rem that come with Windows 10 and 11.
rem
rem Your own tools instead (nothing is downloaded for what you name):
rem     MEMORIES_SDK_PYTHON      the python to run tools\build_mod.py with
rem     MEMORIES_SDK_TOOLCHAIN   a folder with clang, ld.lld and llvm-objcopy in it,
rem                              or in its bin folder (a llvm-mingw you unpacked yourself)
rem     MEMORIES_SDK_NOPAUSE     1: do not wait for a key at the end
rem The Android object (aarch64-translated) needs a clang of the LLVM major the
rem SDK's guest pass is built against (23): llvm-mingw 20260922's.
setlocal EnableExtensions DisableDelayedExpansion

rem --- What is downloaded: the same values as toolchain-pins.json (a batch file
rem --- cannot read it; tools\pc\test_sdk_launchers.py keeps them the same).
set "PYTHON_VERSION=3.14.8"
set "PYTHON_URL=https://www.python.org/ftp/python/3.14.8/python-3.14.8-embed-amd64.zip"
set "PYTHON_SHA256=a93abe456ab01bd96d7a085b3cdb6566b3063f4241360d114142fbdb07f0a310"
set "PYTHON_MEGABYTES=13"
set "LLVM_RELEASE=20260922"
set "LLVM_URL=https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/llvm-mingw-20260922-ucrt-x86_64.zip"
set "LLVM_SHA256=e3ad77d117a4bea19a7a3b333341824d79a5a371004a10e25b8504e7b3047666"
set "LLVM_MEGABYTES=191"
rem The only protocol curl may use, also after a redirect (the tests of this file
rem name another, for archives made on the spot).
set "ALLOWED=https"

rem --- The programs Windows comes with, by full path: a Git for Windows or MSYS
rem --- tar earlier on PATH cannot unpack a zip, and a clean PATH has none of them.
set "CURL=%SystemRoot%\System32\curl.exe"
set "TAR=%SystemRoot%\System32\tar.exe"
set "CERTUTIL=%SystemRoot%\System32\certutil.exe"

set "SDK=%~dp0"
if "%SDK:~-1%"=="\" set "SDK=%SDK:~0,-1%"
set "TOOLCHAIN=%SDK%\toolchain"
set "EXITCODE=0"

rem --- Opened by a double-click (or a drag onto the file): Explorer starts cmd with
rem --- /c and this file, and the window would close on the last line. (A script
rem --- or a terminal that starts it so gets the same wait: MEMORIES_SDK_NOPAUSE=1.)
set "WAIT="
set "COMMANDLINE=%cmdcmdline:"=%"
set "WITHOUT_C=%COMMANDLINE:/c =%"
if /i not "%WITHOUT_C%"=="%COMMANDLINE%" set "WAIT=1"
if defined MEMORIES_SDK_NOPAUSE set "WAIT="

if /i "%~1"=="--help" goto :usage
if /i "%~1"=="-h" goto :usage
if "%~1"=="/?" goto :usage
set "SETUP_ONLY="
if /i "%~1"=="--setup" set "SETUP_ONLY=1"

rem --- Which mod: the first argument (a folder dragged onto this file arrives
rem --- here), else ask. Every argument goes on to build_mod.py in quotes and with
rem --- its trailing backslashes taken off: Tab completion and a drag add one, and
rem --- "C:\some dir\" would swallow the closing quote on the way to Python.
set "ARGS="
if defined SETUP_ONLY goto :tools
if not "%~1"=="" goto :next_argument
echo.
echo Which mod should be built? Drag its folder into this window and press Enter.
echo (The folder with its mod.json and .c files.)
set "ASKED="
set /p "ASKED=Mod folder: "
if not defined ASKED goto :no_mod
set "ARG=%ASKED:"=%"
goto :strip_backslash
:next_argument
if "%~1"=="" goto :tools
set "ARG=%~1"
shift
:strip_backslash
if not "%ARG:~-1%"=="\" goto :backslash_gone
set "ARG=%ARG:~0,-1%"
goto :strip_backslash
:backslash_gone
rem A drive's root keeps its backslash ("C:" alone is the drive's current folder); two
rem of them before the closing quote make one.
if "%ARG:~-1%"==":" set "ARG=%ARG%\\"
if defined ARGS goto :argument_added
if "%ARG:~0,1%"=="-" goto :argument_added
set "FIRST=%ARG%"
if not exist "%ARG%\" goto :not_a_folder
:argument_added
set ARGS=%ARGS% "%ARG%"
goto :next_argument

:tools
rem --- Python.
if defined MEMORIES_SDK_PYTHON goto :own_python
set "PYTHON=%TOOLCHAIN%\python-%PYTHON_VERSION%\python.exe"
if exist "%PYTHON%" goto :python_ready
call :fetch_python
if errorlevel 1 goto :failed
goto :python_ready
:own_python
set "PYTHON=%MEMORIES_SDK_PYTHON%"
if exist "%PYTHON%" goto :python_ready
where "%PYTHON%" >nul 2>nul
if not errorlevel 1 goto :python_ready
<nul set /p "=build-mod: MEMORIES_SDK_PYTHON is %MEMORIES_SDK_PYTHON%, which was not found."
echo.
goto :failed
:python_ready

rem --- Compiler.
if defined MEMORIES_SDK_TOOLCHAIN goto :own_toolchain
set "BIN=%TOOLCHAIN%\llvm-mingw-%LLVM_RELEASE%\bin"
if exist "%BIN%\clang.exe" goto :toolchain_ready
call :fetch_llvm
if errorlevel 1 goto :failed
goto :toolchain_ready
:own_toolchain
set "BIN=%MEMORIES_SDK_TOOLCHAIN%"
if exist "%BIN%\bin\clang.exe" set "BIN=%BIN%\bin"
if exist "%BIN%\clang.exe" goto :toolchain_ready
<nul set /p "=build-mod: MEMORIES_SDK_TOOLCHAIN is %MEMORIES_SDK_TOOLCHAIN%, which has no clang.exe."
echo.
goto :failed
:toolchain_ready
set "PATH=%BIN%;%PATH%"

rem (Paths are printed with set /p, quoted, and not in parenthesized blocks: an
rem unquoted echo would run what follows an & in a folder's name, and a ")" in
rem it would end a block. No path is a call argument either: call reads % and ^
rem in it a second time.)
if not defined SETUP_ONLY goto :build
echo build-mod: the tools are ready:
<nul set /p "=build-mod:   %PYTHON%"
echo.
<nul set /p "=build-mod:   %BIN%"
echo.
goto :finish

:build
rem -X utf8: its messages hold folder names, and a Python writing to a pipe or a file would
rem stop at the first letter the Windows code page lacks (a console takes any).
"%PYTHON%" -X utf8 "%SDK%\tools\build_mod.py" %ARGS%
set "EXITCODE=%ERRORLEVEL%"
if "%EXITCODE%"=="0" goto :built
echo.
echo build-mod: tools\build_mod.py stopped with code %EXITCODE%; its message is above.
goto :finish
:built
echo.
echo build-mod: build_mod.py finished. Its last line above says which objects were built and
echo build-mod: which were not, and why; they are next to the mod's files, or in the folder --out names.
goto :finish

:usage
echo Usage: build-mod.bat MOD_FOLDER [build_mod.py options]
echo        build-mod.bat --setup
echo.
echo Builds the code mod in MOD_FOLDER (its mod.json and .c files) for the 32-bit
echo games (NAME.o), the 64-bit Windows game (NAME.x86_64-windows.o) and the
echo Android app (NAME.aarch64-translated.o), next to the mod's files. Options
echo after the folder go to tools\build_mod.py (--target, --out, ...). With no
echo folder it asks, so you can double-click it or drag a folder onto it.
echo.
echo First run: downloads Python %PYTHON_VERSION% (about %PYTHON_MEGABYTES% MB, from python.org) and
echo llvm-mingw %LLVM_RELEASE% (about %LLVM_MEGABYTES% MB, from github.com) into the toolchain folder
echo beside this file, checks their SHA-256 and unpacks them. It needs 64-bit Windows 10
echo version 1803 or newer, or 11, with its curl, tar and certutil. Nothing else is installed
echo or changed.
echo.
echo MEMORIES_SDK_PYTHON      python to use instead of the download
echo MEMORIES_SDK_TOOLCHAIN   folder with clang, ld.lld and llvm-objcopy (or its bin
echo                         folder) to use instead of the download
echo MEMORIES_SDK_NOPAUSE     1: do not wait for a key at the end
goto :finish

:no_mod
echo build-mod: no folder given.
set "EXITCODE=1"
goto :finish

:not_a_folder
<nul set /p "=build-mod: %FIRST% is not a folder."
echo.
set "EXITCODE=1"
goto :finish

:failed
set "EXITCODE=1"

:finish
if defined WAIT (
    echo.
    pause
)
endlocal & exit /b %EXITCODE%


rem ----------------------------------------------------------------------------
rem :fetch_python and :fetch_llvm: download one archive, check its SHA-256, unpack
rem it into toolchain\.unpack-* and rename that to its final folder, so that a
rem folder under its final name is always complete.

:fetch_python
call :check_system
if errorlevel 1 exit /b 1
set "WHAT=Python %PYTHON_VERSION% (the embeddable package, from python.org)"
set "URL=%PYTHON_URL%"
set "WANT=%PYTHON_SHA256%"
set "MEGABYTES=%PYTHON_MEGABYTES%"
for %%f in ("%PYTHON_URL%") do set "ARCHIVE_NAME=%%~nxf"
set "ARCHIVE=%TOOLCHAIN%\downloads\%ARCHIVE_NAME%"
call :get_verified
if errorlevel 1 exit /b 1
set "PART=%TOOLCHAIN%\.unpack-python"
if exist "%PART%" rmdir /s /q "%PART%"
mkdir "%PART%"
set "PART_NAME=.unpack-python"
call :untar
if not errorlevel 1 goto :python_unpacked
rmdir /s /q "%PART%"
call :unpack_failed
exit /b 1
:python_unpacked
if exist "%PART%\python.exe" goto :python_has_exe
rmdir /s /q "%PART%"
<nul set /p "=build-mod: %ARCHIVE% has no python.exe in it."
echo.
exit /b 1
:python_has_exe
rem A folder of that name without python.exe in it (an earlier run cut short) would make move
rem put the new one inside it.
if exist "%TOOLCHAIN%\python-%PYTHON_VERSION%" rmdir /s /q "%TOOLCHAIN%\python-%PYTHON_VERSION%"
move "%PART%" "%TOOLCHAIN%\python-%PYTHON_VERSION%" >nul
if errorlevel 1 exit /b 1
del "%ARCHIVE%"
exit /b 0

:fetch_llvm
call :check_system
if errorlevel 1 exit /b 1
set "WHAT=llvm-mingw %LLVM_RELEASE% (clang, ld.lld and llvm-objcopy, from github.com)"
set "URL=%LLVM_URL%"
set "WANT=%LLVM_SHA256%"
set "MEGABYTES=%LLVM_MEGABYTES%"
for %%f in ("%LLVM_URL%") do set "ARCHIVE_NAME=%%~nxf"
set "ARCHIVE=%TOOLCHAIN%\downloads\%ARCHIVE_NAME%"
call :get_verified
if errorlevel 1 exit /b 1
set "PART=%TOOLCHAIN%\.unpack-llvm-mingw"
if exist "%PART%" rmdir /s /q "%PART%"
mkdir "%PART%"
set "PART_NAME=.unpack-llvm-mingw"
echo build-mod: unpacking (a minute or two)
call :untar
if not errorlevel 1 goto :llvm_unpacked
rmdir /s /q "%PART%"
call :unpack_failed
exit /b 1
:llvm_unpacked
set "TOP="
set "COUNT=0"
for /d %%d in ("%PART%\*") do set "TOP=%%d" & set /a COUNT+=1
if "%COUNT%"=="1" goto :llvm_single
rmdir /s /q "%PART%"
<nul set /p "=build-mod: %ARCHIVE% does not hold a single folder."
echo.
exit /b 1
:llvm_single
if exist "%TOP%\bin\clang.exe" goto :llvm_has_clang
rmdir /s /q "%PART%"
<nul set /p "=build-mod: %ARCHIVE% has no bin\clang.exe in it."
echo.
exit /b 1
:llvm_has_clang
rem A folder of that name without clang in it (an earlier run cut short) would make move put
rem the new one inside it.
if exist "%TOOLCHAIN%\llvm-mingw-%LLVM_RELEASE%" rmdir /s /q "%TOOLCHAIN%\llvm-mingw-%LLVM_RELEASE%"
move "%TOP%" "%TOOLCHAIN%\llvm-mingw-%LLVM_RELEASE%" >nul
if errorlevel 1 exit /b 1
rmdir /s /q "%PART%"
del "%ARCHIVE%"
exit /b 0

rem :untar: ARCHIVE_NAME from toolchain\downloads into toolchain\PART_NAME, with tar started in
rem the toolchain folder and given plain relative names. tar.exe reads its arguments in the
rem Windows code page: a path with letters outside it ("Jose" with an accent is fine, a Cyrillic or
rem Japanese folder name or an emoji is not) reaches it with question marks and it cannot open
rem the file. The folder itself is entered by cmd, which handles any name.
:untar
pushd "%TOOLCHAIN%"
"%TAR%" -xf "downloads\%ARCHIVE_NAME%" -C "%PART_NAME%"
set "TAR_RESULT=%ERRORLEVEL%"
popd
exit /b %TAR_RESULT%

:unpack_failed
<nul set /p "=build-mod: could not unpack %ARCHIVE%."
echo.
echo build-mod: The file was checked, so it is not damaged. tar.exe may be stopped by the SDK
echo build-mod: folder's path: very long (Windows stops at 260 characters), or not on a local
echo build-mod: drive. Move the whole SDK folder to a short plain one, such as C:\yfm-sdk,
echo build-mod: and run this again.
exit /b 0

:check_system
if /i not "%PROCESSOR_ARCHITECTURE%"=="x86" goto :system_64
if defined PROCESSOR_ARCHITEW6432 goto :system_64
echo build-mod: this is 32-bit Windows, and the Python and the compiler it downloads are 64-bit
echo build-mod: (x64) programs. Set MEMORIES_SDK_PYTHON and MEMORIES_SDK_TOOLCHAIN to tools of
echo build-mod: your own, which need not be downloaded.
exit /b 1
:system_64
if exist "%CURL%" if exist "%TAR%" if exist "%CERTUTIL%" exit /b 0
echo build-mod: curl.exe, tar.exe and certutil.exe are not in the Windows System32 folder. They
echo build-mod: come with Windows 10 version 1803 (April 2018) and newer. Use a newer Windows, or
echo build-mod: set MEMORIES_SDK_PYTHON and MEMORIES_SDK_TOOLCHAIN to tools of your own.
exit /b 1

rem :get_verified: %ARCHIVE% is there with the SHA-256 %WANT%, downloaded from
rem %URL% if need be. A file that is there with another hash is deleted and
rem fetched again; a download with another hash is deleted and refused.
:get_verified
if not exist "%TOOLCHAIN%\downloads" mkdir "%TOOLCHAIN%\downloads"
if not exist "%ARCHIVE%" goto :download
set "FILE=%ARCHIVE%"
call :sha256
if /i "%GOT%"=="%WANT%" exit /b 0
del "%ARCHIVE%"
:download
echo build-mod: downloading %WHAT%, about %MEGABYTES% MB
echo build-mod:   from %URL%
"%CURL%" --fail --location --retry 3 --proto =%ALLOWED% --proto-redir =%ALLOWED% --output "%ARCHIVE%.part" "%URL%"
if not errorlevel 1 goto :downloaded
if exist "%ARCHIVE%.part" del "%ARCHIVE%.part"
echo.
echo build-mod: the download failed. Check the internet connection. Behind a company proxy or
echo build-mod: firewall that inspects HTTPS, curl can stop at its certificate checks (the message
echo build-mod: above may say "revocation" or "certificate"): this script does not switch those
echo build-mod: checks off. Ask whoever runs the network, or download the file from the address
echo build-mod: above in a browser and put it in the downloads folder of the toolchain folder
<nul set /p "=build-mod: (%TOOLCHAIN%\downloads) under its own name; the next run checks it and uses it."
echo.
exit /b 1
:downloaded
move /y "%ARCHIVE%.part" "%ARCHIVE%" >nul
set "FILE=%ARCHIVE%"
call :sha256
if /i not "%GOT%"=="%WANT%" goto :wrong_hash
echo build-mod: SHA-256 matches.
exit /b 0
:wrong_hash
del "%ARCHIVE%"
echo.
echo build-mod: the SHA-256 of the download is wrong, so nothing was unpacked and the file
echo build-mod: was deleted.
echo build-mod:   expected %WANT%
echo build-mod:   got      %GOT%
echo build-mod:   from     %URL%
echo build-mod: Try again. If it repeats, a proxy or a mirror may be changing the file (some
echo build-mod: companies' proxies do): do not use it. Download it from the address above in a
<nul set /p "=build-mod: browser and put it in %TOOLCHAIN%\downloads, or set MEMORIES_SDK_PYTHON and"
echo.
echo build-mod: MEMORIES_SDK_TOOLCHAIN to tools of your own.
exit /b 1

rem :sha256: the SHA-256 of the file FILE in GOT. certutil prints a heading, the
rem hash (in older Windows with spaces between the bytes) and a closing line;
rem it is read from a file, since a ")" in the path would end a for /f command.
rem The file is named in a variable, not an argument of call: call would read the
rem % and ^ of a path a second time.
:sha256
set "GOT="
"%CERTUTIL%" -hashfile "%FILE%" SHA256 > "%FILE%.sha256" 2>nul
for /f "usebackq skip=1 tokens=* delims=" %%h in ("%FILE%.sha256") do if not defined GOT set "GOT=%%h"
del "%FILE%.sha256"
set "GOT=%GOT: =%"
exit /b 0
