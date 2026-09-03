#include "JumpList.h"

#include <windows.h>
#include <shobjidl.h>      // ICustomDestinationList, IShellLink (pulls propsys)
#include <propvarutil.h>
#include <propkey.h>       // after propsys: PROPERTYKEY operators need it
#include <wrl/client.h>

#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace amber
{

static const wchar_t kAppId[] = L"AmberSystems.AmberSSH";

void InitAppUserModelId()
{
    SetCurrentProcessExplicitAppUserModelID(kAppId);
}

void UpdateJumpList(const std::vector<std::pair<std::wstring, std::string>>& items)
{
    ComPtr<ICustomDestinationList> list;
    if (FAILED(CoCreateInstance(CLSID_DestinationList, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&list))))
        return;
    list->SetAppID(kAppId);
    UINT maxSlots = 0;
    ComPtr<IObjectArray> removed;
    if (FAILED(list->BeginList(&maxSlots, IID_PPV_ARGS(&removed))))
        return;

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);

    ComPtr<IObjectCollection> coll;
    if (FAILED(CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&coll))))
        return;
    size_t n = 0;
    for (const auto& it : items)
    {
        if (n++ >= 12)
            break;
        ComPtr<IShellLinkW> link;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&link))))
            continue;
        link->SetPath(exe);
        std::wstring args = L"--connect ";
        for (char ch : it.second)
            args.push_back(static_cast<wchar_t>(ch));
        link->SetArguments(args.c_str());
        link->SetIconLocation(exe, 0);
        ComPtr<IPropertyStore> ps;
        if (SUCCEEDED(link.As(&ps)))
        {
            PROPVARIANT pv;
            if (SUCCEEDED(InitPropVariantFromString(it.first.c_str(), &pv)))
            {
                ps->SetValue(PKEY_Title, pv);
                ps->Commit();
                PropVariantClear(&pv);
            }
        }
        coll->AddObject(link.Get());
    }
    ComPtr<IObjectArray> arr;
    if (SUCCEEDED(coll.As(&arr)))
        list->AppendCategory(L"Saved Sessions", arr.Get());
    list->CommitList();
}

} // namespace amber
