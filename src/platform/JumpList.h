// JumpList.h — taskbar jump list of saved sessions: right-click the
// AmberSSH taskbar button and connect to any profile in one click. Each
// entry relaunches the exe with "--connect <profile-id>".
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace amber
{

// Must run before the main window is created so the taskbar groups the
// window with the jump list.
void InitAppUserModelId();

// items: (display title, profile id). Replaces the previous list.
void UpdateJumpList(const std::vector<std::pair<std::wstring, std::string>>& items);

} // namespace amber
