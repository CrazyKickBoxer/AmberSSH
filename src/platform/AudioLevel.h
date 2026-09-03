// AudioLevel.h — system audio loudness monitor (WASAPI loopback on the
// default render endpoint). Feeds the audio-reactive particle turbulence.
// Runs on its own thread; Level() is lock-free and safe from the render loop.
#pragma once

#include <atomic>
#include <thread>

namespace amber
{

class AudioLevel
{
public:
    AudioLevel() = default;
    ~AudioLevel();
    AudioLevel(const AudioLevel&) = delete;
    AudioLevel& operator=(const AudioLevel&) = delete;

    void Start();
    void Stop();
    bool Running() const { return m_running.load(); }
    // 0..1: envelope-followed RMS with slow auto-gain, so quiet and loud
    // sources both drive the full range. 0 when nothing is playing.
    float Level() const { return m_level.load(); }

private:
    void ThreadMain();

    std::thread m_thread;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_stop{ false };
    std::atomic<float> m_level{ 0.0f };
};

} // namespace amber
