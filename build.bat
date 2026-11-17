@echo off
setlocal
rem builds fastdl.exe with MSVC (Visual Studio 2017+ with the "Desktop development with C++" workload)

set "VCVARS="
for %%Y in (18 2022 2019 2017) do (
    for %%E in (Community Professional Enterprise BuildTools) do (
        if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\%%Y\%%E\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\%%Y\%%E\VC\Auxiliary\Build\vcvars64.bat"
        if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\%%Y\%%E\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\%%Y\%%E\VC\Auxiliary\Build\vcvars64.bat"
    )
)
if not defined VCVARS goto novs

call "%VCVARS%" >nul
if errorlevel 1 exit /b 1

cl /nologo /O2 /GL /MT /EHsc /std:c++17 /W4 /DUNICODE /D_UNICODE fastdl.cpp /Fe:fastdl.exe /link /LTCG winhttp.lib
if errorlevel 1 exit /b 1
del /q fastdl.obj >nul 2>nul
echo Built fastdl.exe
exit /b 0

:novs
echo Visual Studio with the C++ tools was not found.
exit /b 1
