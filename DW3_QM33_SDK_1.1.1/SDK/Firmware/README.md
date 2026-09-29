# Important Warning

**On Windows**, when extracting the zipped package inside the `Firmware` directory, it is recommended to enable the Long Path support. Otherwise, the following error may occur: "Error 0x80010135: Path too long".

To resolve this issue, you can either:

- Run the following PowerShell command from a terminal window with elevated privileges:

  ```powershell
  New-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem" -Name "LongPathsEnabled" -Value 1 -PropertyType DWORD -Force
  ```

- Change the following registry key:
  `Computer\HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\FileSystem\LongPathsEnabled`
  in the Registry Editor opened with elevated privileges.

Alternatively, you can extract the zipped directory at the root directory instead of inside the `Firmware` directory.
