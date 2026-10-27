@echo off
rem winuxfs test VM provisioning. dockur/windows copies the /oem folder to
rem C:\OEM and runs this file once, in the final step of the unattended
rem install. Log: C:\OEM\install.log. See docs/vm.md.
setlocal EnableDelayedExpansion
set OEM=%~dp0
set LOG=C:\OEM\install.log
echo [winuxfs] install.bat start %DATE% %TIME% > "%LOG%"

rem ---- (1) OpenSSH Server ------------------------------------------------
rem Use the built-in capability when it is already present; otherwise the
rem bundled Win32-OpenSSH MSI (offline, works on Insider builds whose
rem features-on-demand may not be downloadable); last resort: download the
rem capability from Windows Update.
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$c = Get-WindowsCapability -Online -Name 'OpenSSH.Server*' -ErrorAction SilentlyContinue;" ^
  "if ($c -and $c.State -eq 'Installed') { 'capability already installed'; exit 0 }" ^
  "if (Test-Path '%OEM%openssh.msi') { exit 10 }" ^
  "Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0 | Out-String; exit 0" >> "%LOG%" 2>&1
if "%ERRORLEVEL%"=="10" (
  echo [winuxfs] installing bundled openssh.msi >> "%LOG%"
  start "" /wait msiexec /i "%OEM%openssh.msi" /qn /norestart /l*v C:\OEM\openssh-install.log
  echo [winuxfs] openssh msiexec exit !ERRORLEVEL! >> "%LOG%"
)

if not exist "%ProgramData%\ssh" mkdir "%ProgramData%\ssh"
copy /y "%OEM%authorized_keys" "%ProgramData%\ssh\administrators_authorized_keys" >> "%LOG%" 2>&1
rem Administrators (S-1-5-32-544) and SYSTEM (S-1-5-18) only, or sshd ignores the file.
icacls "%ProgramData%\ssh\administrators_authorized_keys" /inheritance:r /grant "*S-1-5-32-544:F" /grant "*S-1-5-18:F" >> "%LOG%" 2>&1

netsh advfirewall firewall add rule name="winuxfs-sshd" dir=in action=allow protocol=TCP localport=22 >> "%LOG%" 2>&1
sc config sshd start= auto >> "%LOG%" 2>&1
net start sshd >> "%LOG%" 2>&1

rem ---- (2) WinFsp --------------------------------------------------------
echo [winuxfs] installing winfsp.msi >> "%LOG%"
start "" /wait msiexec /i "%OEM%winfsp.msi" /qn /norestart /l*v C:\OEM\winfsp-install.log
echo [winuxfs] winfsp msiexec exit %ERRORLEVEL% >> "%LOG%"
sc query WinFsp >> "%LOG%" 2>&1
sc query sshd >> "%LOG%" 2>&1

echo [winuxfs] install.bat done %DATE% %TIME% >> "%LOG%"
rem Best effort: make the log visible on the host in vm/shared/.
copy /y "%LOG%" \\host.lan\Data\oem-install.log >nul 2>&1
endlocal
exit /b 0
