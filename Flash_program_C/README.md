# Flash_program_C

Win32 C version of `Flash_program/Flash_program_V1.py`.

## Build

Using Visual Studio Developer Command Prompt:

```bat
build_msvc.bat
```

Using MinGW-w64:

```bat
build_mingw.bat
```

## Protocol

- Packet size: 1024 bytes
- Header: command, firmware version `0x67`, size `1024`
- Payload: 1016 bytes
- CRC: STM32-style CRC32 over header + payload
- Data packet pad byte: `0xFF`
- `0x6A` from STM32 triggers `WAIT (0x21)`, then the app waits for `READY (0x4A)`
- After all `.bin` data is sent, the app sends `FINISHED (0x9A)` and waits for `0x56`
