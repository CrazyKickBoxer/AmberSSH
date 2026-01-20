# AmberSSH Technical Documentation

## 1. Overview

AmberSSH is a Qt 6-based SSH client focused on a clean UI, a modular core, and robust logging. The
project is designed to scale to a full-featured terminal client with extensible session management,
structured diagnostics, and hardened cryptography handled by libssh.

## 2. Feature Summary

- **Encrypted SSH transport** using libssh with SSH-2 protocol support.
- **Qt 6 desktop UI** with connection management and session status output.
- **Terminal emulator core** that maintains a screen buffer and provides a foundation for ANSI/VT100
  parsing.
- **Structured debugging** with per-subsystem log categories and file-based log persistence.
- **Extensible architecture** to add features like tabs, key management, and host verification.

## 3. System Architecture

### 3.1 Component Map

| Component | Responsibility |
| --- | --- |
| `MainWindow` | UI layout, user input, and status updates. |
| `SshClient` | SSH session lifecycle, encryption, and channel management. |
| `TerminalEmulator` | Screen buffer and terminal control processing. |
| `Logging` | Log categories, handlers, and on-disk log persistence. |

### 3.2 Data Flow

1. User enters connection data in the Qt UI.
2. `MainWindow` asks `SshClient` to connect.
3. `SshClient` opens an SSH session with libssh (encrypted transport).
4. Data is intended to flow into `TerminalEmulator` for rendering and screen updates.

## 4. Encryption and Security

AmberSSH relies on libssh to negotiate SSH-2 encryption, including key exchange, host key
verification, and ciphers negotiated with the server. All sensitive transport security is handled by
libssh, which exposes an API for configuration and diagnostic output. Future expansion points include:

- Host key verification prompts and known_hosts management.
- Agent forwarding and key-based authentication.
- Per-session cipher constraints for compliance requirements.

## 5. Debugging and Diagnostics

The logging layer defines separate log categories per subsystem (`amber.app`, `amber.ssh`,
`amber.terminal`). All logs are formatted with timestamps and routed to a file under the user data
folder. Diagnostics should be added with Qt logging macros to keep output structured.

## 6. Terminal Emulator

The emulator keeps a fixed-size text buffer, handles cursor movement, and provides a hook point for
ANSI/VT sequences. It should be extended with:

- Full ANSI CSI parsing (cursor moves, colors, line erase).
- UTF-8 decoding and wide glyph support.
- Scrollback management and copy/paste features.

## 7. UI/UX Roadmap

- Session tabs and profile management.
- Host key prompts and connection history.
- Split panes and theming support.
- Secure credential storage integration.

## 8. Build & Run

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
./AmberSSH
```

## 9. Project Layout

```
AmberSSH/
├── CMakeLists.txt
├── docs/
│   └── TECHNICAL_DOCUMENTATION.md
├── include/
│   ├── Logging.h
│   ├── MainWindow.h
│   ├── SshClient.h
│   └── TerminalEmulator.h
└── src/
    ├── Logging.cpp
    ├── MainWindow.cpp
    ├── SshClient.cpp
    ├── TerminalEmulator.cpp
    └── main.cpp
```
