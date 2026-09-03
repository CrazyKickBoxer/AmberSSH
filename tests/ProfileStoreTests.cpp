// ProfileStoreTests.cpp — persistence, schema tolerance, and the guarantee
// that no secret is ever written to profiles.json.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "profiles/ProfileStore.h"

using namespace amber;

namespace
{

std::filesystem::path TempFile(const char* stem)
{
    auto dir = std::filesystem::temp_directory_path() / "amberssh-tests";
    std::filesystem::create_directories(dir);
    return dir / (std::string(stem) + ".json");
}

std::string ReadAll(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

ConnectionProfile Sample()
{
    ConnectionProfile p;
    p.id = MakeUuid();
    p.name = "prod";
    p.host = "example.com";
    p.port = 2222;
    p.username = "deploy";
    p.auth = AuthMethod::PublicKey;
    p.privateKeyPath = R"(C:\keys\id_ed25519)";
    p.rememberPassphrase = true;
    return p;
}

} // namespace

TEST_CASE("uuids are well formed and unique", "[profiles][uuid]")
{
    std::string a = MakeUuid();
    std::string b = MakeUuid();
    REQUIRE(a.size() == 36);
    REQUIRE(a[8] == '-');
    REQUIRE(a[13] == '-');
    REQUIRE(a[18] == '-');
    REQUIRE(a[23] == '-');
    REQUIRE(a[14] == '4');              // version 4
    REQUIRE(a != b);
}

TEST_CASE("save then load round-trips every field", "[profiles]")
{
    auto file = TempFile("roundtrip");
    std::filesystem::remove(file);

    ConnectionProfile p = Sample();
    ProfileStore out;
    out.Upsert(p);
    std::string err;
    REQUIRE(out.SaveTo(file, &err));
    REQUIRE(err.empty());

    ProfileStore in;
    ProfileStore::LoadReport report;
    REQUIRE(in.LoadFrom(file, &report));
    REQUIRE(report.fileError.empty());
    REQUIRE(report.skipped == 0);
    REQUIRE(in.Count() == 1);

    const ConnectionProfile* got = in.Find(p.id);
    REQUIRE(got != nullptr);
    REQUIRE(got->name == p.name);
    REQUIRE(got->host == p.host);
    REQUIRE(got->port == p.port);
    REQUIRE(got->username == p.username);
    REQUIRE(got->auth == AuthMethod::PublicKey);
    REQUIRE(got->privateKeyPath == p.privateKeyPath);
    REQUIRE(got->rememberPassphrase);
}

TEST_CASE("every PuTTY-page option survives a save/load round trip", "[profiles][putty]")
{
    auto file = TempFile("putty-roundtrip");
    std::filesystem::remove(file);

    ConnectionProfile p = Sample();
    p.protocol = Protocol::Telnet;
    p.closeOnExit = CloseOnExit::Never;
    p.logMode = LogMode::All;
    p.logFile = "&H-&Y.log";
    p.logAppend = false;
    p.autoWrap = false;
    p.implicitCr = true;
    p.localEcho = TriState::On;
    p.localLineEdit = TriState::Off;
    p.answerback = "hello";
    p.backspaceIsDel = false;
    p.homeEnd = HomeEndMode::Rxvt;
    p.fnKeys = FnKeysMode::Sco;
    p.appKeypadInitial = true;
    p.bell = BellStyle::Both;
    p.bellTaskbar = false;
    p.allowAltScreen = false;
    p.allowMouse = false;
    p.cols = 132;
    p.rows = 50;
    p.resizeAction = ResizeAction::Font;
    p.scrollbackLines = 12345;
    p.scrollOnOutput = true;
    p.cursor = CursorShape::Bar;
    p.cursorBlink = false;
    p.fontFamily = "JetBrains Mono";
    p.fontSize = 18.0f;
    p.gapPx = 3;
    p.windowTitle = "prod box";
    p.warnOnClose = false;
    p.altF4Closes = false;
    p.charset = Charset::Cp437;
    p.poorMansLineDrawing = true;
    p.mouseButtons = MouseButtons::Xterm;
    p.rectSelectDefault = true;
    p.autoCopy = false;
    p.palette = 1;
    p.themeId = 3;
    p.allowAnsiColours = false;
    p.boldStyle = BoldStyle::Both;
    p.tcpNoDelay = false;
    p.tcpKeepalive = true;
    p.ipVersion = 2;
    p.logicalHost = "prod.internal";
    p.termType = "vt220";
    p.termSpeed = "9600,9600";
    p.envVars = "FOO=bar\nBAZ=1";
    p.proxyType = ProxyType::Socks5;
    p.proxyHost = "proxy";
    p.proxyPort = 1081;
    p.proxyUser = "pu";
    p.proxyExclude = "*.local";
    p.proxyDns = false;
    p.remoteCommand = "tmux a";
    p.compression = true;
    p.cipherPref = "aes256-ctr";
    p.agentForward = true;
    p.x11Forward = true;
    p.x11Display = "localhost:1";
    p.manualHostKeys = "SHA256:abc\nSHA256:def";
    p.serialPort = "COM7";
    p.serialBaud = 115200;
    p.serialDataBits = 7;
    p.serialStopBits = 2;
    p.serialParity = SerialParity::Even;
    p.serialFlow = SerialFlow::RtsCts;
    p.telnetPassive = true;
    p.telnetKeyboard = true;
    p.telnetNewline = true;
    p.rloginLocalUser = "me";
    p.effectPreset = "Miami Night";
    p.densityPpc = 96;

    ProfileStore out;
    out.Upsert(p);
    REQUIRE(out.SaveTo(file));
    ProfileStore in;
    REQUIRE(in.LoadFrom(file));
    const ConnectionProfile* g = in.Find(p.id);
    REQUIRE(g != nullptr);
    REQUIRE(g->protocol == Protocol::Telnet);
    REQUIRE(g->closeOnExit == CloseOnExit::Never);
    REQUIRE(g->logMode == LogMode::All);
    REQUIRE(g->logFile == "&H-&Y.log");
    REQUIRE_FALSE(g->logAppend);
    REQUIRE_FALSE(g->autoWrap);
    REQUIRE(g->implicitCr);
    REQUIRE(g->localEcho == TriState::On);
    REQUIRE(g->localLineEdit == TriState::Off);
    REQUIRE(g->answerback == "hello");
    REQUIRE_FALSE(g->backspaceIsDel);
    REQUIRE(g->homeEnd == HomeEndMode::Rxvt);
    REQUIRE(g->fnKeys == FnKeysMode::Sco);
    REQUIRE(g->appKeypadInitial);
    REQUIRE(g->bell == BellStyle::Both);
    REQUIRE_FALSE(g->bellTaskbar);
    REQUIRE_FALSE(g->allowAltScreen);
    REQUIRE_FALSE(g->allowMouse);
    REQUIRE(g->cols == 132);
    REQUIRE(g->rows == 50);
    REQUIRE(g->resizeAction == ResizeAction::Font);
    REQUIRE(g->scrollbackLines == 12345);
    REQUIRE(g->scrollOnOutput);
    REQUIRE(g->cursor == CursorShape::Bar);
    REQUIRE_FALSE(g->cursorBlink);
    REQUIRE(g->fontFamily == "JetBrains Mono");
    REQUIRE(g->fontSize == 18.0f);
    REQUIRE(g->gapPx == 3);
    REQUIRE(g->windowTitle == "prod box");
    REQUIRE_FALSE(g->warnOnClose);
    REQUIRE_FALSE(g->altF4Closes);
    REQUIRE(g->charset == Charset::Cp437);
    REQUIRE(g->poorMansLineDrawing);
    REQUIRE(g->mouseButtons == MouseButtons::Xterm);
    REQUIRE(g->rectSelectDefault);
    REQUIRE_FALSE(g->autoCopy);
    REQUIRE(g->palette == 1);
    REQUIRE(g->themeId == 3);
    REQUIRE_FALSE(g->allowAnsiColours);
    REQUIRE(g->boldStyle == BoldStyle::Both);
    REQUIRE_FALSE(g->tcpNoDelay);
    REQUIRE(g->tcpKeepalive);
    REQUIRE(g->ipVersion == 2);
    REQUIRE(g->logicalHost == "prod.internal");
    REQUIRE(g->termType == "vt220");
    REQUIRE(g->termSpeed == "9600,9600");
    REQUIRE(g->envVars == "FOO=bar\nBAZ=1");
    REQUIRE(g->proxyType == ProxyType::Socks5);
    REQUIRE(g->proxyHost == "proxy");
    REQUIRE(g->proxyPort == 1081);
    REQUIRE(g->proxyUser == "pu");
    REQUIRE(g->proxyExclude == "*.local");
    REQUIRE_FALSE(g->proxyDns);
    REQUIRE(g->remoteCommand == "tmux a");
    REQUIRE(g->compression);
    REQUIRE(g->cipherPref == "aes256-ctr");
    REQUIRE(g->agentForward);
    REQUIRE(g->x11Forward);
    REQUIRE(g->x11Display == "localhost:1");
    REQUIRE(g->manualHostKeys == "SHA256:abc\nSHA256:def");
    REQUIRE(g->serialPort == "COM7");
    REQUIRE(g->serialBaud == 115200);
    REQUIRE(g->serialDataBits == 7);
    REQUIRE(g->serialStopBits == 2);
    REQUIRE(g->serialParity == SerialParity::Even);
    REQUIRE(g->serialFlow == SerialFlow::RtsCts);
    REQUIRE(g->telnetPassive);
    REQUIRE(g->telnetKeyboard);
    REQUIRE(g->telnetNewline);
    REQUIRE(g->rloginLocalUser == "me");
    REQUIRE(g->effectPreset == "Miami Night");
    REQUIRE(g->densityPpc == 96);

    // The file never carries a proxy password either.
    std::string text = ReadAll(file);
    REQUIRE(text.find("proxyPass\"") == std::string::npos);
    REQUIRE(text.find("\"password\"") == std::string::npos);
}

TEST_CASE("guardian settings survive a round trip", "[profiles][guardian]")
{
    auto file = TempFile("guardian-roundtrip");
    std::filesystem::remove(file);

    ConnectionProfile p = Sample();
    p.reconnectMode = ReconnectMode::Ask;
    p.reconnectMaxAttempts = 0;
    p.reconnectJitterPercent = 35;
    p.reconnectNotify = false;
    p.reconnectBanner = false;
    p.reattachMode = ReattachMode::Screen;
    p.reattachSession = "build-2";
    p.reattachCommand = "zellij attach main";
    p.restoreCwd = true;
    p.restoreForwards = false;
    {
        ProfileStore s;
        s.Upsert(p);
        std::string err;
        REQUIRE(s.SaveTo(file, &err));
    }
    ProfileStore r;
    REQUIRE(r.LoadFrom(file));
    const ConnectionProfile* g = r.Find(p.id);
    REQUIRE(g != nullptr);
    REQUIRE(g->reconnectMode == ReconnectMode::Ask);
    REQUIRE(g->reconnectMaxAttempts == 0);
    REQUIRE(g->reconnectJitterPercent == 35);
    REQUIRE_FALSE(g->reconnectNotify);
    REQUIRE_FALSE(g->reconnectBanner);
    REQUIRE(g->reattachMode == ReattachMode::Screen);
    REQUIRE(g->reattachSession == "build-2");
    REQUIRE(g->reattachCommand == "zellij attach main");
    REQUIRE(g->restoreCwd);
    REQUIRE_FALSE(g->restoreForwards);

    SECTION("the legacy autoReconnect key is still written for older builds")
    {
        const std::string text = ReadAll(file);
        REQUIRE(text.find("\"autoReconnect\": true") != std::string::npos);
    }
    SECTION("out-of-range guardian values are repaired on load")
    {
        ConnectionProfile bad = Sample();
        bad.reconnectJitterPercent = 900;
        bad.reconnectMaxAttempts = -4;
        auto f2 = TempFile("guardian-clamp");
        std::filesystem::remove(f2);
        ProfileStore s;
        s.Upsert(bad);
        REQUIRE(s.SaveTo(f2, nullptr));
        ProfileStore r2;
        REQUIRE(r2.LoadFrom(f2));
        const ConnectionProfile* c = r2.Find(bad.id);
        REQUIRE(c != nullptr);
        REQUIRE(c->reconnectJitterPercent <= 50);
        REQUIRE(c->reconnectMaxAttempts >= 0);
        std::filesystem::remove(f2);
    }
    std::filesystem::remove(file);
}

TEST_CASE("a profile written before Stage 2 keeps its reconnect behaviour",
          "[profiles][guardian][compat]")
{
    // The Stage 1 file had one boolean. It has to keep meaning what it meant.
    auto file = TempFile("guardian-legacy");
    std::filesystem::remove(file);
    {
        std::ofstream out(file, std::ios::binary);
        out << R"({"version":2,"profiles":[
          {"id":"11111111-1111-4111-8111-111111111111","name":"old on",
           "host":"a.example","port":22,"autoReconnect":true},
          {"id":"22222222-2222-4222-8222-222222222222","name":"old off",
           "host":"b.example","port":22,"autoReconnect":false}]})";
    }
    ProfileStore r;
    REQUIRE(r.LoadFrom(file));
    const ConnectionProfile* on = r.Find("11111111-1111-4111-8111-111111111111");
    const ConnectionProfile* off = r.Find("22222222-2222-4222-8222-222222222222");
    REQUIRE(on != nullptr);
    REQUIRE(off != nullptr);
    REQUIRE(on->reconnectMode == ReconnectMode::Automatic);
    REQUIRE(off->reconnectMode == ReconnectMode::Off);
    // And the Stage 2 defaults are what a file that says nothing gets.
    REQUIRE(on->reattachMode == ReattachMode::None);
    REQUIRE_FALSE(on->restoreCwd);
    REQUIRE(on->reconnectMaxAttempts == 6);
    std::filesystem::remove(file);
}

TEST_CASE("a serial profile is valid without a host and keeps its port type",
          "[profiles][putty]")
{
    ConnectionProfile p;
    p.id = MakeUuid();
    p.protocol = Protocol::Serial;
    p.serialPort = "COM3";
    REQUIRE(p.Valid());
    p.protocol = Protocol::Ssh;
    REQUIRE_FALSE(p.Valid());        // an SSH profile needs a host
    REQUIRE(ProtocolDefaultPort(Protocol::Telnet) == 23);
    REQUIRE(ProtocolDefaultPort(Protocol::Rlogin) == 513);
    REQUIRE(ProtocolFromName(ProtocolName(Protocol::Rlogin)) == Protocol::Rlogin);
}

TEST_CASE("the saved file contains no secret fields", "[profiles][security]")
{
    auto file = TempFile("nosecrets");
    std::filesystem::remove(file);

    ConnectionProfile p = Sample();
    p.rememberPassword = true;
    ProfileStore store;
    store.Upsert(p);
    REQUIRE(store.SaveTo(file));

    std::string text = ReadAll(file);
    REQUIRE(text.find("password\"") == std::string::npos);
    REQUIRE(text.find("passphrase\"") == std::string::npos);
    REQUIRE(text.find("BEGIN OPENSSH PRIVATE KEY") == std::string::npos);
    // The remember-* flags are booleans and are expected to be present.
    REQUIRE(text.find("rememberPassword") != std::string::npos);
}

TEST_CASE("a malformed entry is skipped, valid ones survive", "[profiles][recovery]")
{
    auto file = TempFile("malformed");
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << R"({
          "schemaVersion": 1,
          "profiles": [
            { "id": "aaa", "name": "good", "host": "h1.example", "port": 22 },
            { "id": "bbb", "name": "no host" },
            12345,
            { "id": "ccc", "name": "also good", "host": "h2.example", "port": "not-a-number" }
          ]
        })";
    }

    ProfileStore store;
    ProfileStore::LoadReport report;
    REQUIRE(store.LoadFrom(file, &report));
    REQUIRE(report.fileError.empty());
    REQUIRE(report.skipped == 2);           // missing host, and the bare number
    REQUIRE(store.Count() == 2);
    REQUIRE(store.FindByName("good") != nullptr);
    REQUIRE(store.FindByName("also good") != nullptr);
    // A bad port type falls back to the default rather than rejecting the row.
    REQUIRE(store.FindByName("also good")->port == 22);
}

TEST_CASE("a corrupt file reports an error and does not throw", "[profiles][recovery]")
{
    auto file = TempFile("corrupt");
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "{ this is not json";
    }
    ProfileStore store;
    ProfileStore::LoadReport report;
    REQUIRE_FALSE(store.LoadFrom(file, &report));
    REQUIRE_FALSE(report.fileError.empty());
    REQUIRE(store.Count() == 0);
}

TEST_CASE("an absent file is an empty store, not a failure", "[profiles]")
{
    auto file = TempFile("absent");
    std::filesystem::remove(file);
    ProfileStore store;
    ProfileStore::LoadReport report;
    REQUIRE(store.LoadFrom(file, &report));
    REQUIRE(report.fileError.empty());
    REQUIRE(store.Count() == 0);
}

TEST_CASE("upsert replaces by id and remove deletes", "[profiles]")
{
    ProfileStore store;
    ConnectionProfile p = Sample();
    std::string id = store.Upsert(p);
    REQUIRE(store.Count() == 1);

    p.name = "renamed";
    store.Upsert(p);
    REQUIRE(store.Count() == 1);
    REQUIRE(store.Find(id)->name == "renamed");

    REQUIRE(store.Remove(id));
    REQUIRE(store.Count() == 0);
    REQUIRE_FALSE(store.Remove(id));
}

TEST_CASE("upsert without an id assigns one", "[profiles]")
{
    ProfileStore store;
    ConnectionProfile p;
    p.host = "h.example";
    std::string id = store.Upsert(p);
    REQUIRE(id.size() == 36);
    REQUIRE(store.Find(id) != nullptr);
}

TEST_CASE("out-of-range values are repaired on load", "[profiles][validation]")
{
    auto file = TempFile("ranges");
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << R"({"profiles":[{"id":"x","host":"h","port":99999,"cols":1,"rows":9999}]})";
    }
    ProfileStore store;
    REQUIRE(store.LoadFrom(file));
    REQUIRE(store.Count() == 1);
    const auto& p = store.All()[0];
    REQUIRE(p.port == 22);
    REQUIRE(p.cols == 80);
    REQUIRE(p.rows == 25);
}

TEST_CASE("saving is atomic: no .tmp file is left behind", "[profiles][atomic]")
{
    auto file = TempFile("atomic");
    std::filesystem::remove(file);
    ProfileStore store;
    store.Upsert(Sample());
    REQUIRE(store.SaveTo(file));

    auto temp = file;
    temp += L".tmp";
    REQUIRE_FALSE(std::filesystem::exists(temp));
    REQUIRE(std::filesystem::exists(file));
}

TEST_CASE("overwriting an existing file succeeds", "[profiles][atomic]")
{
    auto file = TempFile("overwrite");
    ProfileStore first;
    first.Upsert(Sample());
    REQUIRE(first.SaveTo(file));

    ProfileStore second;
    ConnectionProfile p = Sample();
    p.name = "second";
    second.Upsert(p);
    REQUIRE(second.SaveTo(file));

    ProfileStore reload;
    REQUIRE(reload.LoadFrom(file));
    REQUIRE(reload.Count() == 1);
    REQUIRE(reload.All()[0].name == "second");
}
