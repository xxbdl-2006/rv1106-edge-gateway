@echo off
rem ---------------------------------------------------------------------------
rem Run scripts/verify-osd.sh from cmd or PowerShell.
rem
rem Why this exists: the verification script is bash, and on this machine
rem D:\Git\bin is NOT on PATH (only D:\Git\cmd is), so `bash script.sh` does not
rem resolve in PowerShell. That is a second wall after the first one, which is
rem that `MSYS_NO_PATHCONV=1 ./script.sh` is POSIX syntax and PowerShell rejects
rem it with "not recognized as the name of a cmdlet".
rem
rem Neither wall has anything to do with verifying the overlay, so this removes
rem both: it finds a bash, and it forwards the arguments unchanged.
rem
rem Usage (PowerShell or cmd, from the repo root):
rem     .\scripts\verify-osd.cmd --verify-only
rem     .\scripts\verify-osd.cmd --build-only
rem
rem A .cmd rather than a .ps1 on purpose: PowerShell will not run a .ps1 unless
rem the execution policy allows it, and asking the user to change an execution
rem policy to run a test is not a reasonable prerequisite.
rem ---------------------------------------------------------------------------

setlocal

set "REPO_DIR=%~dp0.."
set "SCRIPT=%REPO_DIR%\scripts\verify-osd.sh"

if not exist "%SCRIPT%" (
    echo ERROR: cannot find "%SCRIPT%" >&2
    exit /b 1
)

rem Candidates in order of preference. The PortableGit one is the copy that
rem ships with this tooling; the others are ordinary Git for Windows installs.
set "BASH_EXE="
for %%C in (
    "%ProgramFiles%\Git\bin\bash.exe"
    "%ProgramFiles(x86)%\Git\bin\bash.exe"
    "D:\Git\bin\bash.exe"
    "C:\Users\adms\.workbuddy\binaries\PortableGit\versions\1.2.0\bin\bash.exe"
) do (
    if exist "%%~C" if not defined BASH_EXE set "BASH_EXE=%%~C"
)

if not defined BASH_EXE (
    echo ERROR: could not find bash.exe. >&2
    echo   Looked in: >&2
    echo     %%ProgramFiles%%\Git\bin\bash.exe >&2
    echo     %%ProgramFiles^(x86^)%%\Git\bin\bash.exe >&2
    echo     D:\Git\bin\bash.exe >&2
    echo     C:\Users\adms\.workbuddy\binaries\PortableGit\...\bin\bash.exe >&2
    echo   Install Git for Windows, or edit this file to add your path. >&2
    exit /b 1
)

rem Pushd so the script's own repo detection is consistent, and so a relative
rem argument from the caller still means what they think it means.
pushd "%REPO_DIR%" || exit /b 1
"%BASH_EXE%" "%SCRIPT%" %*
set "RC=%ERRORLEVEL%"
popd

endlocal & exit /b %RC%
