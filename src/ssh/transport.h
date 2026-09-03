// transport.h — the TCP connect path shared by every network protocol:
// IP-version preference, connect timeout, TCP_NODELAY / SO_KEEPALIVE, and
// SOCKS4 / SOCKS4a / SOCKS5 / HTTP CONNECT proxies with an exclusion list.
#pragma once

#include <winsock2.h>

#include <atomic>
#include <string>

struct SshConfig;

// Resolves and connects to cfg.host:cfg.port, directly or through the
// configured proxy (proxy handshake included). `abortSlot` receives the
// in-progress socket so Disconnect() can abort a blocking connect; `stop`
// is polled. On failure returns INVALID_SOCKET with `err` filled in.
// `status` receives progress text ("resolving...", "proxy handshake...").
SOCKET ConnectTransport(const SshConfig& cfg, std::atomic<uintptr_t>& abortSlot,
                        std::atomic<bool>& stop, std::string& err,
                        std::string& status);

// True when cfg.host should bypass the proxy (exclusion list / localhost).
bool ProxyExcluded(const SshConfig& cfg);

// Simple '*' / '?' glob match, case-insensitive (proxy exclusion patterns).
bool WildcardMatch(const std::string& pattern, const std::string& text);
