#include "Hello.h"

#include <atomic>
#include <thread>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Security.Credentials.UI.h>

#pragma comment(lib, "windowsapp.lib")

namespace amber
{

bool VerifyUserPresence(HWND owner, const std::wstring& message)
{
    using namespace winrt::Windows::Security::Credentials::UI;

    // -1 pending, 0 denied, 1 verified, 2 unavailable (treated as verified).
    std::atomic<int> result{ -1 };
    std::thread worker([&]() {
        try
        {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            auto avail = UserConsentVerifier::CheckAvailabilityAsync().get();
            if (avail != UserConsentVerifierAvailability::Available)
                result.store(2);
            else
            {
                auto r = UserConsentVerifier::RequestVerificationAsync(
                             winrt::hstring(message.c_str()))
                             .get();
                result.store(r == UserConsentVerificationResult::Verified ? 1 : 0);
            }
            winrt::uninit_apartment();
        }
        catch (...)
        {
            result.store(2);
        }
    });

    // Keep the owner responsive (and repainting) while Hello's system UI is up.
    while (result.load() < 0)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(15);
    }
    worker.join();
    if (owner)
        SetForegroundWindow(owner);
    return result.load() != 0;
}

} // namespace amber
