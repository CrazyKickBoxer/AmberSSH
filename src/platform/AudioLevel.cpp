#include "AudioLevel.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <ksmedia.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace amber
{

AudioLevel::~AudioLevel()
{
    Stop();
}

void AudioLevel::Start()
{
    if (m_running.load() || m_thread.joinable())
        return;
    m_stop.store(false);
    m_running.store(true);
    m_thread = std::thread(&AudioLevel::ThreadMain, this);
}

void AudioLevel::Stop()
{
    m_stop.store(true);
    if (m_thread.joinable())
        m_thread.join();
    m_running.store(false);
    m_level.store(0.0f);
}

void AudioLevel::ThreadMain()
{
    // Own apartment: COM/WASAPI never touch the render thread (see the emoji
    // worker for why that matters with DXGI).
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDevice> dev;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> cap;
    WAVEFORMATEX* fmt = nullptr;

    bool ok =
        SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                   CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
        SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) &&
        SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(client.GetAddressOf()))) &&
        SUCCEEDED(client->GetMixFormat(&fmt)) &&
        SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_LOOPBACK,
                                     10000000 /* 1 s buffer, 100 ns units */,
                                     0, fmt, nullptr)) &&
        SUCCEEDED(client->GetService(IID_PPV_ARGS(&cap))) &&
        SUCCEEDED(client->Start());

    bool isFloat = false;
    if (fmt)
    {
        if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
            isFloat = true;
        else if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        {
            auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(fmt);
            isFloat = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
        }
    }

    float env = 0.0f;        // fast-attack / slow-release envelope
    float peak = 0.05f;      // slow auto-gain reference
    while (ok && !m_stop.load())
    {
        UINT32 packet = 0;
        if (FAILED(cap->GetNextPacketSize(&packet)))
            break;
        if (packet == 0)
        {
            // Loopback delivers nothing while the mix is silent: decay so
            // the field settles instead of freezing at the last level.
            env *= 0.90f;
            m_level.store(env / peak * (env / peak));
            Sleep(8);
            continue;
        }
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
            break;
        double sum = 0.0;
        size_t n = 0;
        if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data && fmt)
        {
            const size_t count = static_cast<size_t>(frames) * fmt->nChannels;
            if (isFloat)
            {
                const float* s = reinterpret_cast<const float*>(data);
                for (size_t i = 0; i < count; ++i)
                    sum += static_cast<double>(s[i]) * s[i];
                n = count;
            }
            else if (fmt->wBitsPerSample == 16)
            {
                const int16_t* s = reinterpret_cast<const int16_t*>(data);
                for (size_t i = 0; i < count; ++i)
                {
                    double v = s[i] / 32768.0;
                    sum += v * v;
                }
                n = count;
            }
        }
        cap->ReleaseBuffer(frames);

        float rms = n ? static_cast<float>(std::sqrt(sum / static_cast<double>(n)))
                      : 0.0f;
        env = std::max(rms, env * 0.85f);
        peak = std::max(std::max(env, peak * 0.9995f), 0.02f);
        float lvl = std::clamp(env / peak, 0.0f, 1.0f);
        m_level.store(lvl * lvl);
    }

    if (client)
        client->Stop();
    if (fmt)
        CoTaskMemFree(fmt);
    cap.Reset();
    client.Reset();
    dev.Reset();
    en.Reset();
    if (SUCCEEDED(coHr))
        CoUninitialize();
    m_level.store(0.0f);
    m_running.store(false);
}

} // namespace amber
