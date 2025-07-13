@echo off
setlocal

REM Get workspace folder from argument
set workspace=%1

REM Get target source name: udp_client or udp_server
set target=%2

REM Common source files (in src)
set common=%workspace%\src\protocol_frames.c ^
%workspace%\src\mem_pool.c ^
%workspace%\src\sha256.c ^
%workspace%\src\bitmap.c ^
%workspace%\src\queue.c ^
%workspace%\src\hash.c ^
%workspace%\src\checksum.c ^
%workspace%\src\fileio.c

REM Conditional dependency only for udp_server
if "%target%"=="test_server" (
    set private=%workspace%\src\file_handler.c ^
%workspace%\src\message_handler.c ^
%workspace%\src\server_frames.c
) else if "%target%"=="test_client" (
    set private=%workspace%\src\client_frames.c
) else (
    echo [ERROR] Unknown target: %target%
    exit /b 1
)


echo cl.exe /EHsc /favor:AMD64 ^
%common% ^
%private% ^
%workspace%\%target%.c ^
/I"%workspace%" ^
/O2 ^
/Fe"%workspace%\bin\%target%.exe" ^
/Fo"%workspace%\bin\obj\\" ^
/Fd"%workspace%\bin\pdb\\" ^
/link /MACHINE:X64


REM Build with cl.exe
cl.exe /EHsc /favor:AMD64 ^
%common% ^
%private% ^
%workspace%\%target%.c ^
/I"%workspace%" ^
/O2 ^
/Fe"%workspace%\bin\%target%.exe" ^
/Fo"%workspace%\bin\obj\\" ^
/Fd"%workspace%\bin\pdb\\" ^
/link /MACHINE:X64

endlocal