#include "Vitals.h"
#include "SftpClient.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <sstream>

namespace amber
{

VitalsMonitor::~VitalsMonitor()
{
    Stop();
}

void VitalsMonitor::Start(const ConnectionProfile& p, std::string password,
                          std::string passphrase)
{
    Stop();
    m_stop.store(false);
    m_running.store(true);
    m_profileId = p.id;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_hist.clear();
        m_status = "vitals: connecting...";
    }
    m_thread = std::thread(&VitalsMonitor::ThreadMain, this, p, std::move(password),
                           std::move(passphrase));
}

void VitalsMonitor::Stop()
{
    m_stop.store(true);
    if (m_thread.joinable())
        m_thread.join();
    m_running.store(false);
}

std::vector<VitalsMonitor::Sample> VitalsMonitor::History() const
{
    std::lock_guard<std::mutex> lk(m_mx);
    return m_hist;
}

std::string VitalsMonitor::Status() const
{
    std::lock_guard<std::mutex> lk(m_mx);
    return m_status;
}

void VitalsMonitor::ThreadMain(ConnectionProfile p, std::string pw, std::string pp)
{
    SftpClient client;
    std::string err;
    bool ok = client.Connect(p, pw, pp, err);
    if (!pw.empty()) SecureZeroMemory(&pw[0], pw.size());
    if (!pp.empty()) SecureZeroMemory(&pp[0], pp.size());
    if (!ok)
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_status = "vitals: " + err;
        m_running.store(false);
        return;
    }

    // One round trip per sample: everything the parser needs, delimited.
    const std::string cmd =
        "head -1 /proc/stat; echo ==; grep -E '^(MemTotal|MemAvailable)' /proc/meminfo; "
        "echo ==; cat /proc/net/dev; echo ==; cat /proc/loadavg";
    unsigned long long prevIdle = 0, prevTotal = 0, prevNet = 0;
    ULONGLONG prevTick = 0;
    while (!m_stop.load())
    {
        std::string out;
        if (!client.Exec(cmd, out, err))
        {
            std::lock_guard<std::mutex> lk(m_mx);
            m_status = "vitals: " + err;
            break;
        }
        // Split on "==" lines.
        std::vector<std::string> parts(1);
        {
            std::istringstream in(out);
            std::string line;
            while (std::getline(in, line))
            {
                if (line == "==") { parts.emplace_back(); continue; }
                parts.back() += line + "\n";
            }
        }
        Sample s;
        // CPU: "cpu user nice system idle iowait irq softirq steal ..."
        if (parts.size() > 0)
        {
            unsigned long long v[10] = {};
            int n = sscanf_s(parts[0].c_str(), "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                             &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
            if (n >= 4)
            {
                unsigned long long idle = v[3] + v[4], total = 0;
                for (int i = 0; i < 8; ++i) total += v[i];
                if (prevTotal && total > prevTotal)
                    s.cpu = 1.0f - static_cast<float>(idle - prevIdle) /
                                       static_cast<float>(total - prevTotal);
                prevIdle = idle;
                prevTotal = total;
            }
        }
        // Memory.
        if (parts.size() > 1)
        {
            unsigned long long total = 0, avail = 0;
            const char* t = strstr(parts[1].c_str(), "MemTotal:");
            const char* a = strstr(parts[1].c_str(), "MemAvailable:");
            if (t) total = strtoull(t + 9, nullptr, 10);
            if (a) avail = strtoull(a + 13, nullptr, 10);
            if (total)
                s.mem = static_cast<float>(total - avail) / static_cast<float>(total);
        }
        // Network: sum rx+tx bytes of every interface except lo.
        if (parts.size() > 2)
        {
            unsigned long long bytes = 0;
            std::istringstream in(parts[2]);
            std::string line;
            while (std::getline(in, line))
            {
                size_t colon = line.find(':');
                if (colon == std::string::npos) continue;
                std::string name = line.substr(0, colon);
                name.erase(0, name.find_first_not_of(' '));
                if (name == "lo") continue;
                unsigned long long f[16] = {};
                int n = sscanf_s(line.c_str() + colon + 1,
                                 "%llu %llu %llu %llu %llu %llu %llu %llu %llu",
                                 &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6], &f[7], &f[8]);
                if (n >= 9) bytes += f[0] + f[8];
            }
            ULONGLONG tick = GetTickCount64();
            if (prevTick && bytes >= prevNet && tick > prevTick)
                s.netKBs = static_cast<float>(bytes - prevNet) / 1024.0f /
                           (static_cast<float>(tick - prevTick) / 1000.0f);
            prevNet = bytes;
            prevTick = tick;
        }
        if (parts.size() > 3)
            s.load1 = static_cast<float>(atof(parts[3].c_str()));

        {
            std::lock_guard<std::mutex> lk(m_mx);
            m_hist.push_back(s);
            if (m_hist.size() > 60)
                m_hist.erase(m_hist.begin());
            char buf[128];
            snprintf(buf, sizeof(buf), "cpu %3.0f%%  mem %3.0f%%  net %5.0f KB/s  load %.2f",
                     s.cpu * 100.0f, s.mem * 100.0f, s.netKBs, s.load1);
            m_status = buf;
        }
        for (int i = 0; i < 20 && !m_stop.load(); ++i)
            Sleep(100);
    }
    client.Close();
    m_running.store(false);
}

} // namespace amber
