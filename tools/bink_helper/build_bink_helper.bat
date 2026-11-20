@echo off
rem Builds the 32-bit Bink helper (bink_helper32.exe) with the Visual Studio 32-bit compiler and puts it next to OpenRFG.exe.
setlocal
set "ROOT=%~dp0..\.."
set "OUT=%ROOT%\build\x64\src\openrfg"
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" goto novc
call "%VCVARS%" x86
if errorlevel 1 goto fail
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /O2 /MT /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS "%~dp0bink_helper32.cpp" /Fo"%TEMP%\bink_helper32.obj" /Fe"%OUT%\bink_helper32.exe" /link "%ROOT%\lib\win32\binkw32.lib" delayimp.lib /DELAYLOAD:binkw32.dll /SUBSYSTEM:CONSOLE
if errorlevel 1 goto fail
echo.
echo Built OK: %OUT%\bink_helper32.exe
goto end
:novc
echo Could not find vcvarsall.bat. Edit the VCVARS line in this file.
goto end
:fail
echo BUILD FAILED
:end
pause
endlocal
