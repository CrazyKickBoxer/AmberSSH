// Vitals.h — remote system monitor: a second SSH connection samples the
// host's CPU, memory, network and load every couple of seconds (via
// /proc), keeping a short history the title bar renders as sparklines.
#pragma once

#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

#include "../profiles/ConnectionProfile.h"

namespace amber
{

class VitalsMonitor
{
public:
    struct Sample
    {
        float cpu = 0.0f;      // 0..1 busy fraction
        float mem = 0.0f;      // 0..1 used fraction (excluding cache)
        float netKBs = 0.0f;   // rx+tx KB/s across non-loopback interfaces
        float load1 = 0.0f;    // 1-minute load average
    };

    ~VitalsMonitor();
    void Start(const ConnectionProfile& p, std::string password, std::string passphrase);
    void Stop();
    bool Running() const { return m_running.load(); }
    std::vector<Sample> History() const;   // oldest → newest, up to 60
    std::string Status() const;
    const std::string& ProfileId() const { return m_profileId; }

private:
    void ThreadMain(ConnectionProfile p, std::string pw, std::string pp);

    std::thread m_thread;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_stop{ false };
    mutable std::mutex m_mx;
    std::vector<Sample> m_hist;
    std::string m_status;
    std::string m_profileId;
};

} // namespace amber
