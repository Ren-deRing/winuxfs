REM SPDX-License-Identifier: GPL-2.0-only
@echo off
rem Scripted equivalent of the Explorer check (docs/vm.md, "M1 acceptance
rem observations"). Run in the VM from the directory holding winuxfs.exe and
rem winuxfs_backend_memfs.dll. Everything happens in this one logon session,
rem because a drive letter exists only in the session of the mounting process.
cd /d %~dp0
start "" /b winuxfs.exe mount Y: none --type memfs --plugin-dir %~dp0 > mount.log 2>&1
set n=0
:wait
if exist Y:\ goto up
set /a n+=1
if %n% gtr 30 (echo mount did not appear & type mount.log & exit /b 1)
ping -n 2 127.0.0.1 >nul
goto wait
:up
echo --- vol
vol Y:
echo --- mkdir/create
mkdir Y:\docs
mkdir Y:\docs\sub
echo hello from winuxfs> Y:\docs\a.txt
echo second file> Y:\docs\sub\b.txt
echo --- dir
dir Y:\ /s
echo --- read back
type Y:\docs\a.txt
type Y:\docs\sub\b.txt
echo --- rename file and folder
ren Y:\docs\a.txt renamed.txt
ren Y:\docs\sub inner
dir Y:\docs
type Y:\docs\renamed.txt
type Y:\docs\inner\b.txt
echo --- move across folders
move Y:\docs\renamed.txt Y:\moved.txt
dir Y:\
echo --- delete
del Y:\moved.txt
rmdir /s /q Y:\docs\inner
rmdir Y:\docs
dir Y:\
if exist Y:\moved.txt (echo FAIL moved.txt still there)
echo --- unmount
winuxfs.exe unmount Y:
ping -n 3 127.0.0.1 >nul
if exist Y:\ (echo FAIL Y: still present) else (echo Y: gone)
type mount.log
