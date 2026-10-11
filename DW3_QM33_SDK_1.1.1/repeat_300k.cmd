@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0uwb_test_firmware\repeat_no_protocol_test.ps1" %*
exit /b %errorlevel%
