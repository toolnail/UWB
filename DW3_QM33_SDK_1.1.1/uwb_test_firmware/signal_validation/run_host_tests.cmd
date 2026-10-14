@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
pushd "%~dp0"
for %%I in (0 1 1000) do (
  cl /nologo /std:c11 /DWIN32 /D_CRT_SECURE_NO_WARNINGS /DTEST_SIGNAL_LOG_EVERY=%%I /I"..\..\Drivers\API\Shared\dwt_uwb_driver" /I"..\..\Drivers\API\Shared\dwt_uwb_driver\lib\qmath\include" /FI"%~dp0host_compat.h" test_rx_signal.c "..\..\Drivers\API\Shared\dwt_uwb_driver\deca_rsl.c" "..\..\Drivers\API\Shared\dwt_uwb_driver\lib\qmath\src\qmath.c" /Fe:test_rx_signal.exe
  if errorlevel 1 exit /b 1
  test_rx_signal.exe
  if errorlevel 1 exit /b 1
)
cl /nologo /std:c11 /D_CRT_SECURE_NO_WARNINGS /I"..\..\Drivers\API\Build_Platforms\nRF52840-DK" test_rssi_stream.c /Fe:test_rssi_stream.exe
if errorlevel 1 exit /b 1
test_rssi_stream.exe
if errorlevel 1 exit /b 1
python test_rssi_capture.py
if errorlevel 1 exit /b 1
popd
