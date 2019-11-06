:<<"::is_bash"
@echo off
goto :is_windows
::is_bash
#
exec $(dirname $0)/src/build-posix.sh #
echo "src/build-posix.sh failed" #
exit 1 #

:is_windows
setlocal ENABLEEXTENSIONS

rem
rem Check for Visual Studio
if not "x%VSINSTALLDIR%" == "x" goto :vs_cmd_ok

echo VSINSTALLDIR not set. Please install Visual Studio (there is a no-charge
echo version) and run this command inside a "Command Prompt for Visual Studio"
echo window.
exit /b 1
goto :eof

:vs_cmd_ok
call vsdevcmd -arch=x64
if errorlevel 1 goto :vsdevcmd_failed

rem
rem Check for git
set startup_dir=%~dp0
set found_git=
for %%a in ("%PATH:;=";"%") do call :find_in_path "%%~a"

if not "x%found_git%" == "x" goto :found_git_ok
echo git not found in PATH. Please install Git:
echo.
echo https://git-scm.com
echo.
exit /b 1
goto :eof

:find_in_path
set str1="%1"
if not "x%str1:git\cmd=%" == "x%str1%" set found_git="%1"
goto :eof

:find_python
rem keys are parsed as "%1"
rem values underneath the current key are parsed as:
rem name="%1" type="%2" value="%3"
if not "x%2" == "x" goto :eof
set keyname="%1"
rem Search for an "InstallPath" subkey
FOR /F "usebackq tokens=1-3" %%A IN (`REG QUERY %keyname%\InstallPath /ve 2^>nul`) DO (
  if not "x%%C" == "x" set PythonDir="%%C"
)
goto :eof

:vsdevcmd_failed
echo.
echo vsdevcmd failed. Please install Visual Studio (there is a no-charge
echo version) and run this command inside a "Command Prompt for Visual Studio"
echo window.
exit /b 1
goto :eof

:subcmd_failed
exit /b 1
goto :eof

:found_git_ok
rem
rem git was found, clone submodules ("init submodules").
cd %startup_dir%
cmd /c git submodule update --init

FOR /F "usebackq tokens=1-3" %%A IN (`REG QUERY HKLM\SOFTWARE\Python\PythonCore 2^>nul`) DO call :find_python %%A %%B %%C
FOR /F "usebackq tokens=1-3" %%A IN (`REG QUERY HKCU\SOFTWARE\Python\PythonCore 2^>nul`) DO call :find_python %%A %%B %%C
FOR /F "usebackq tokens=1-3" %%A IN (`REG QUERY HKLM\SOFTWARE\Wow6432Node\Python\PythonCore 2^>nul`) DO call :find_python %%A %%B %%C
if not "x%PythonDir%" == "x" goto :python_ok

echo Python not found. Please install Python for Windows:
echo.
echo https://python.org
exit /b 1
goto :eof

:python_ok
rem
rem Build volcano
set COPY_VULKAN_DLL=0
if not exist out\Debug\vulkan-1.dll goto :build_volcano
if not exist out\Debug\vulkaninfo.exe.pdb goto :build_volcano
goto :skip_volcano

:build_volcano
cd vendor\volcano
set VOLCANO_NO_OUT=1
cmd /c build.cmd
if errorlevel 1 goto :subcmd_failed
cd ..\..
set COPY_VULKAN_DLL=1
:skip_volcano

rem
rem Fix "symlink" in src/gn
cd src
forfiles /m gn /c "cmd /c if @isdir == TRUE rmdir @file" 2>nul
forfiles /m gn /c "cmd /c if @isdir == FALSE del @file" 2>nul
mklink /j gn ..\vendor\volcano\src\gn
cd ..\vendor
forfiles /m subgn /c "cmd /c if @isdir == TRUE rmdir @file" 2>nul
forfiles /m subgn /c "cmd /c if @isdir == FALSE del @file" 2>nul
mklink /j subgn ..\vendor\volcano\vendor\subgn
cd ..

rem
rem Safely remove quotes in %PythonDir% while adding to PATH
set PATH=%PATH%;%PythonDir:"=%

echo gn gen out/Debug
vendor\volcano\vendor\subgn\gn.exe gen out/Debug
if errorlevel 1 goto :subcmd_failed

if '%COPY_VULKAN_DLL% == '0 goto :skip_copy_vulkan_dll
rem
rem Copy vulkan-1.dll (and .dll.lib, etc.) into volcanosamples
rem After gn gen creates dir but only if dll was rebuilt.
xcopy /y /e vendor\volcano\out\DebugDLL\vulkan-1.* out\Debug
xcopy /y /e vendor\volcano\out\DebugDLL\vulkan out\Debug\vulkan
xcopy /y /e vendor\volcano\out\DebugDLL\vulkan\explicit_layer.d\*.dll out\Debug
:skip_copy_vulkan_dll

echo ninja -C out/Debug
vendor\volcano\vendor\subgn\ninja.exe -C out/Debug
if errorlevel 1 goto :subcmd_failed

echo set VK_LAYER_PATH=%startup_dir%out\Debug\vulkan\explicit_layer.d
echo set VK_INSTANCE_LAYERS=VK_LAYER_LUNARG_standard_validation

:eof
