// VncTab.h — what a VNC tab carries that a terminal tab does not.
//
// A VNC tab IS a Session (sessions/Session.h): that is what lets the tab
// strip, panes, workspaces, quake mode and every other tab-lifecycle path
// keep working without a second tab model in app.cpp. Its grid is simply
// unused. What it has instead lives here: the network/decode worker, the
// particle desktop that draws it, how the framebuffer is placed on screen,
// the input state that must be released on focus loss or disconnect, and
// the numbers the overlay shows.
//
// Ownership follows the threads. `session` is the worker's; the render
// thread only ever calls its thread-safe surface (events, damage, input
// queue). `desk` is the render thread's and is never touched by the worker.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../render/desktop.h"
#include "../vnc/VncSession.h"

namespace amber
{

struct VncTab
{
    std::unique_ptr<vnc::VncSession> session;
    std::unique_ptr<DesktopParticles> desk;

    // The profile id of the SSH session tunnelled through, "" for direct.
    std::string viaProfileId;
    // The forward spec asked of that session, so its ForwardUp can be
    // recognised; "" for direct.
    std::string tunnelSpec;

    // ---- placement: the framebuffer on screen -----------------------------
    // Fit inside the content area, letterboxed, integer scale when it fits
    // at 1:1 (faithful mode needs native scale), otherwise the largest
    // uniform scale that fits. Recomputed each frame from the window size.
    float dstX = 0, dstY = 0, scale = 1;
    uint32_t fbW = 0, fbH = 0;
    bool nativeScale = false;      // scale == 1: the faithful contract holds

    // ---- input ----------------------------------------------------------
    // Everything held, so it can be released on focus loss or disconnect:
    // a stuck Shift on the far side is the classic VNC failure.
    std::vector<uint32_t> heldKeysyms;
    uint8_t heldButtons = 0;
    int lastFbX = -1, lastFbY = -1;   // last pointer position sent, fb px
    bool pointerInside = false;
    wchar_t pendingHighSurrogate = 0; // WM_CHAR delivers a pair as two messages

    // ---- the click shockwave (effect) ------------------------------------
    // Set on a button press inside the picture; the sim fades it in a second.
    float shockX = 0.0f, shockY = 0.0f;
    double shockTime = -1.0;
    float shockAmp = 1.0f;            // +1 a click pushes out, -0.5 a right click pulls in

    // ---- clipboard loop guard --------------------------------------------
    // Text we last put on the Windows clipboard from the server, and the
    // text we last sent the server from Windows: a change that equals either
    // is an echo, not news, and is not sent back.
    std::string lastFromServer;
    std::string lastToServer;
    // Set for the duration of a Ctrl+V: after the text is sent, V is pressed
    // so the far side pastes what it just received.
    bool pasteThenType = false;

    // ---- diagnostics -----------------------------------------------------
    double lastDamageAt = 0.0;
    uint32_t damageRectsLastFrame = 0;
    uint64_t damageBytesLastFrame = 0;
    // Input-to-picture, APPROXIMATE: the time from the last pointer event
    // sent to the next framebuffer update completed. It measures the round
    // trip to a server that reacts to the pointer (a moving cursor, a hover
    // highlight); on a static desktop it measures nothing and reads as 0.
    double lastInputSentAt = -1.0;
    float approxInputToPictureMs = 0.0f;
    uint32_t updatesSeen = 0;
    std::string desktopName;
    // network throughput, over one-second windows
    uint64_t bytesInPrev = 0;
    double bytesAt = 0.0;
    float bytesPerSec = 0.0f;
};

} // namespace amber
