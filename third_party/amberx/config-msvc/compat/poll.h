/* poll.h — PROBE SHIM. include/xserver_poll.h wants the POSIX poll
 * interface. Windows has WSAPoll with the same struct shape; this declares
 * the POSIX names so the core compiles, with no definition — the AmberX
 * host does not poll sockets at all (it has no listener), so the real port
 * removes the dependency rather than implementing it. Original AmberSSH
 * file. */
#ifndef AMBERX_PROBE_POLL_H
#define AMBERX_PROBE_POLL_H
struct pollfd { int fd; short events; short revents; };
typedef unsigned long nfds_t;
#define POLLIN   0x0001
#define POLLPRI  0x0002
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020
int poll(struct pollfd*, nfds_t, int);
#endif
