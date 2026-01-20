# AmberSSH

AmberSSH is a Qt 6-based SSH client with a modular core, structured logging, and a terminal emulator
skeleton for building a full-featured desktop SSH experience.

## Highlights

- Qt 6 widgets UI with connection form and status panel.
- libssh-based transport for encrypted SSH sessions.
- Structured logging via Qt categories and file-backed log output.
- Terminal emulator buffer that can be extended for full VT/ANSI handling.

## Documentation

- [Technical Documentation](docs/TECHNICAL_DOCUMENTATION.md)

## Quick Start

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
./AmberSSH
```
