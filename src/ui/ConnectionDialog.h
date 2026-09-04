// ConnectionDialog.h — native Win32 connection manager with a PuTTY-style
// category tree on the left and a settings panel on the right. Every PuTTY
// category is present (Session / Logging / Terminal / Keyboard / Bell /
// Features / Window / Appearance / Behaviour / Translation / Selection /
// Colours / Connection / Data / Proxy / SSH / Auth / X11 / Tunnels / Host
// keys / Serial / Telnet / Rlogin) plus AmberSSH's Effects page.
//
// Built with raw Win32 controls (no ImGui, no resource-script dialog template)
// so the layout can be DPI-scaled at runtime and so the terminal viewport keeps
// exclusive ownership of the DirectX swap chain. Pages are described by a
// field table (see the .cpp), so load / save / collect are table-driven.
#pragma once

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

#include "../profiles/ProfileStore.h"
#include "../utility/SecureString.h"

namespace amber
{

// Everything the caller needs to open a session. Secrets live here only for as
// long as it takes to hand them to the SSH worker.
struct ConnectionRequest
{
    ConnectionProfile profile;
    SecureString password;
    SecureString passphrase;
    SecureString proxyPassword;
};

class ConnectionDialog
{
public:
    // Runs a modal dialog. Returns true when the user chose Open, in which case
    // requestOut is filled. Profiles are loaded from and saved to `store`.
    static bool Show(HWND owner, ProfileStore& store, ConnectionRequest& requestOut);

    enum class Page
    {
        Session, Logging,
        Terminal, Keyboard, Bell, Features,
        Window, Appearance, Behaviour, Translation, Selection, Colours,
        Connection, Guardian, Reattach, Data, Proxy, Ssh, SshAuth, SshX11,
        SshTunnels, SshHostKeys, RemoteGui,
        Serial, Telnet, Rlogin, Local,
        Effects,
        Count
    };

    enum class Kind { Edit, Number, Password, Check, Combo, ComboEdit, RadioRow, Multi, Note };

    // One configurable field: its page, control kind, label and the profile
    // accessor pair. Values travel as strings (numbers / indices formatted).
    struct Field
    {
        Page page;
        Kind kind;
        int id;
        std::wstring label;
        std::vector<std::wstring> options;     // Combo / ComboEdit / RadioRow
        std::function<std::wstring(const ConnectionProfile&)> get;
        std::function<void(ConnectionProfile&, const std::wstring&)> set;
        int width = -1;                        // 96-dpi px, -1 = full width
        HWND label_ = nullptr;                 // created controls
        std::vector<HWND> ctrls;
    };

private:
    ConnectionDialog(ProfileStore& store, ConnectionRequest& out)
        : m_store(store), m_out(out) {}

    static INT_PTR CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT Proc(HWND, UINT, WPARAM, LPARAM);

    void DefineFields();
    void BuildControls(HWND parent);
    void BuildTree(HWND parent);
    void Layout();
    void ShowPage(Page page);
    void RefreshSessionList();
    void LoadSelectedProfile();
    void SaveCurrentProfile();
    void DeleteSelectedProfile();
    bool CollectRequest();
    void ReadFields(ConnectionProfile& p);            // controls → profile
    void WriteFields(const ConnectionProfile& p);     // profile → controls
    void SyncAuthEnabled();
    void SyncGuardianEnabled();   // reattach fields follow the reattach mode
    void SyncProtocol();
    void BrowseForKey();
    void SetStatus(const std::wstring& text);
    std::wstring FieldValue(const Field& f) const;
    void SetFieldValue(Field& f, const std::wstring& v);
    Field* FindField(int id);

    // Amber chrome: banner, field outlines, owner-drawn buttons and list rows.
    void PaintChrome(HDC dc);
    void DrawThemedButton(const DRAWITEMSTRUCT& dis);
    void DrawSessionRow(const DRAWITEMSTRUCT& dis);

    ProfileStore& m_store;
    ConnectionRequest& m_out;

    HWND m_dlg = nullptr;
    HWND m_tree = nullptr;
    HWND m_status = nullptr;
    HFONT m_font = nullptr;
    HFONT m_monoFont = nullptr;
    HFONT m_headerFont = nullptr;
    // Small condensed face for the LCARS sidebar block tags.
    HFONT m_tagFont = nullptr;
    HBRUSH m_bgBrush = nullptr;
    HBRUSH m_fieldBrush = nullptr;
    UINT m_dpi = 96;
    Page m_page = Page::Session;
    bool m_accepted = false;

    std::vector<Field> m_fields;
    std::vector<HWND> m_sessionExtra;          // list + name + Load/Save/Delete
    std::string m_selectedProfileId;
    int m_lastProtocol = 0;
};

} // namespace amber
