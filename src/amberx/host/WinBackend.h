// WinBackend.h — what wmain needs from the Windows half of AmberXHost.
// The rest of the backend is the C surface declared in server/amberwin.h.
#pragma once

#include <cstdint>
#include <string>

namespace amber::amberx
{

class FramedPipe;

// Registers the display window and the queues. The pipe must already be
// handshaken; the backend owns nothing about its security, only its bytes.
bool BackendInit(FramedPipe& pipe, int width, int height, uint32_t display,
                 const std::string& keymapPath, std::string& err);

// Starts the pipe and server threads, pumps the window until the server has
// returned, and returns the server's exit code.
int BackendRun();

} // namespace amber::amberx
