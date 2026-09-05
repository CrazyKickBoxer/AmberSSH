// app_reel.cpp — the demo reel: a timed performance of the client for a
// camera, cut to a beat.
//
// --reel runs a scripted sequence and nothing else: a terminal that types
// itself, every motion style on successive beats, the interface styles and
// appearances, then a real connection to a VNC profile of the user's with
// real input into its desktop, and back out. Every step sits on a beat of
// the track the reel will be cut to (AMBER_REEL_BPM), so a scene change
// lands where the ear expects one. The screen is grabbed from outside by
// FFmpeg; the reel only performs.
//
// Two markers make the cut exact without any clock being shared: the reel
// holds a black screen, then flashes the whole frame light for a tenth of
// a second at beat zero, and again at its last beat. The post-process finds
// both with blackdetect and trims to them, so the audio's first beat is
// laid on the first flash to within a frame.
//
// It borrows the motion style, the skin, the appearance and the fullscreen
// state, and puts all four back. It never saves settings.
//
//   AMBER_REEL_BPM      the track's tempo (default 90.5)
//   AMBER_REEL_PROFILE  the id of the VNC profile to connect to (required
//                       for the desktop act; without it the act is skipped)
//   AMBER_REEL_STRETCH  multiplies every beat position (default 1)
#include "app.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "ui/Chrome.h"
#include "vnc/Keysyms.h"

namespace
{

// keysyms the desktop act needs beyond Keysyms.h's set
constexpr uint32_t kReturn = 0xFF0D, kEscape = 0xFF1B, kF2 = 0xFFBF, kF4 = 0xFFC1, kF7 = 0xFFC4;
constexpr uint32_t kLeft = 0xFF51, kUp = 0xFF52, kRight = 0xFF53, kDown = 0xFF54;

std::string Env(const char* name)
{
    char buf[512] = {};
    const DWORD n = GetEnvironmentVariableA(name, buf, sizeof buf);
    return (n == 0 || n >= sizeof buf) ? std::string() : std::string(buf, n);
}

// Nerd Font glyphs, from the Private Use Area. AmberSSH's fallback chain
// resolves them out of %LOCALAPPDATA%\AmberSSH\fonts (SymbolsNerdFontMono),
// which is what the prompt-icon auto-fetch puts there. Written as escapes
// rather than literal bytes so the source's own encoding cannot matter;
// each is its own literal because a C++ hex escape is greedy.
constexpr const char* kNfSep     = "\xEE\x82\xB0";   // U+E0B0 powerline separator
constexpr const char* kNfBranch  = "\xEE\x82\xA0";   // U+E0A0 git branch
constexpr const char* kNfFolder  = "\xEF\x81\xBB";   // U+F07B folder
constexpr const char* kNfFile    = "\xEF\x85\x9B";   // U+F15B file
constexpr const char* kNfFedora  = "\xEF\x85\xBC";   // U+F17C linux (U+F30A, fedora, is not in the symbols font)
constexpr const char* kNfDisk    = "\xEF\x82\xA0";   // U+F0A0 disk
constexpr const char* kNfChip    = "\xEF\x8B\x9B";   // U+F2DB chip
constexpr const char* kNfCheck   = "\xEF\x80\x8C";   // U+F00C check
constexpr const char* kNfCross   = "\xEF\x80\x8D";   // U+F00D cross
constexpr const char* kNfDown    = "\xEF\x80\x99";   // U+F019 download
constexpr const char* kNfLock    = "\xEF\x80\xA3";   // U+F023 lock
constexpr const char* kNfGear    = "\xEF\x80\x93";   // U+F013 cog
constexpr const char* kNfArchive = "\xEF\x87\x86";   // U+F1C6 archive
constexpr const char* kNfCode    = "\xEF\x84\xA1";   // U+F121 code
constexpr const char* kNfKey     = "\xEF\x82\x84";   // U+F084 key
constexpr const char* kNfClock   = "\xEF\x80\x97";   // U+F017 clock

// Real names out of /usr/bin on the Fedora box this was shot against,
// sampled across the whole directory so the screen reads like the machine
// rather than like a word list.
const char* const kBinNames[] = {
    "abrtd", "abrt-watch-log", "adcli", "agetty", "alsamixer", "anaconda", "aplay", "apropos",
    "arecordmidi", "arptables", "aseqdump", "asunder", "atril-previewer", "audit2why", "aulastlog", "automount",
    "avcstat", "b43-fwcutter", "basenc", "bc", "blkdeactivate", "blockdev", "bluemoon", "brltty",
    "brltty-genkey", "brltty-mkuser", "brltty-ttb", "bs2bstream", "btrfsck", "bunzip2", "bzdiff", "bzip2recover",
    "cache_repair", "canberra-boot", "cardos-tool", "cdda-player", "cd-info", "certmonger", "chage", "chcat",
    "check-regexp", "chmem", "chronyc", "chvt", "cisco-decrypt", "clockdiff", "colcrt", "comm",
    "composite", "convert", "cpio", "cracklib-packer", "crontab", "ctags", "cups-calibrate", "cupsfilter",
    "curve_keygen", "date", "dbus-send", "dc", "deallocvt", "depmod", "dig", "dirname",
    "dmfilemapd", "dnf", "dnie-tool", "dos2unix", "driverless", "dumpe2fs", "dump-utmp", "e2image",
    "e4defrag", "echo", "eidenv", "enchant-2", "envsubst", "era_invalidate", "ethtool", "eu-elfcompress",
    "eu-ranlib", "eu-stacktrace", "ex", "exiv2", "factor", "fancontrol", "fc-cat", "fc-query",
    "fedfs-map-nfs4", "fgconsole", "fincore", "firewall-cmd", "flatpak", "fmt", "fprintd-delete", "fribidi",
    "fsck.exfat", "fsck.minix", "fsfreeze", "fuse-overlayfs", "gaim", "gcr-viewer", "gdbus", "genl-ctrl-list",
    "getconf", "getkeycodes", "getpolicyload", "ghostscript", "glxinfo64", "gnome-keyring-3", "gpg", "gpgme-json",
    "gr2fonttest", "grops", "groupmod", "grub2-editenv", "grub2-mkrelpath", "gs", "gslj", "gst-inspect-1.0",
    "gtf", "gunzip", "hangul", "hexdump", "hostnamectl", "hp-firmware", "hp-plugin", "hp-setup",
    "hv_kvp_daemon", "ibus", "iconvconfig", "ifconfig", "imsettings-info", "insmod", "iodine", "ip6tables-save",
    "ipa-join", "ipmaddr", "iptables", "irqbalance", "iscsiadm", "iso-info", "jcat-tool", "json_reformat",
    "kbdinfo", "kernel-install", "kinit", "kpartx", "ktutil", "lastcomm", "ld", "ld.so",
    "lesspipe.sh", "libinput", "linux64", "load_policy", "local-getcert", "login", "look", "lpasswd",
    "lpmove", "lpr.cups", "ls", "lsfd", "lslogins", "lspci", "lusermod", "lvextend",
    "lvmdump", "lvmsar", "lvs", "magick-script", "manpath", "mbim-network", "mdmon", "mii-diag",
    "mkdir", "mkfontscale", "mkfs.ext2", "mkfs.msdos", "mknod", "mmcli", "modprobe", "more",
    "mount.fuse3", "mount.ntfs-fuse", "mp3rtp", "mpris-proxy", "multipathd", "namei", "ndptool", "NetworkManager",
    "nf-ct-add", "nf-exp-list", "nfsdcld", "nft", "nl", "nl-class-delete", "nl-fib-lookup", "nl-link-stats",
    "nl-neigh-delete", "nl-qdisc-list", "nl-rule-list", "nmblookup", "normalizer", "nsec3hash", "ntfs-3g", "ntfscluster",
    "ntfsfix", "ntfsmount", "ntfstruncate", "objcopy", "ogg123", "oomctl", "opensc-asn1", "openvt",
    "orca", "os-prober", "pactl", "pango-list", "parecord", "passt", "pasta.avx2", "pax11publish",
    "pdf2dsc", "pdfimages", "pdftohtml", "peekfd", "pfbtopfa", "pidof", "pinfo", "pipewire",
    "pivot_root", "pkcon", "pkexec", "pkmon", "plocate", "pod2man", "poweroff", "ppdpo",
    "pppstats", "pre-grohtml", "printf_ngettext", "ps2ascii", "ps2pdf14", "psfgettable", "pstree.x11", "pvck",
    "pvresize", "pw-cli", "pw-dot", "pw-jack", "pw-metadata", "pw-midirecord", "pwqfilter", "pw-sysex",
    "pydoc3.14", "qemu-arm-static", "qr", "quota", "quotasync", "rdma", "readtags", "regdiff",
    "renice", "repquota", "resizepart", "rfkill", "route", "rpc.idmapd", "rpm", "rpmkeys",
    "rsync-ssl", "rtmon", "runscript", "rx", "satyr", "sdiff", "secret-tool", "selabel_compare",
    "selinuxenabled", "semodule_link", "serdi", "setcifsacl", "setfont", "setpgid", "setroubleshootd", "setxkbmap",
    "sha1hmac", "sha256sum", "sharesec", "shred", "skdump", "sleep", "smbclient", "smbprint",
    "smp_discover", "smp_write_gpio", "soelim.groff", "sotruss", "spa-monitor", "speaker-test", "split", "ssh-add",
    "ssh-keyscan", "sssd", "sstpc", "stdbuf", "stty", "sudoedit", "swaplabel", "symlinks",
    "systemd-cat", "systemd-delta", "systemd-mount", "systemd-run", "tabs", "tbl", "tcptraceroute", "tee",
    "thin_delta", "thin_restore", "thunar-settings", "timeout", "top", "tpm2_changeeps", "tpm2_create", "tpm2_ecdhzgen",
    "tpm2_hash", "tpm2_nvdefine", "tpm2_nvundefine", "tpm2_pcrreset", "tpm2_quote", "tpm2_rsaencrypt", "tpm2_sign", "tpm2_unseal",
    "tracepath6", "true", "tset", "ts_print_mt", "tss2_createnv", "tss2_exportkey", "tss2_list", "tss2_pcrextend",
    "tss2_sign", "tty", "uchardet", "ulimit", "umount.nfs4", "uncompface", "unix2dos", "unsetfiles",
    "unzstd", "upnpc", "usbhid-dump", "userdbctl", "utmpdump", "varlinkctl", "VBoxService", "vgcfgbackup",
    "vgcreate", "vgimportclone", "vgremove", "vi", "vimdiff", "vmcore-dmesg", "vmware-checkvm", "vncconfig",
    "vpddecode", "wait", "wc", "weston-debug", "whatis", "who", "wipefs", "wpa_passphrase",
    "wsdd", "X", "xauth", "xdg-email", "xdg-user-dir", "xfce4-appfinder", "xfce4-session", "xfrun4",
    "xfs_estimate", "xfs_info", "xfs_mkfile", "xfs_rtcp", "xinit", "xmlsec1", "xqmstats", "xtables-monitor",
    "xz", "xzegrep", "yes", "zdiff", "zgrep", "zipinfo", "znew", "zstdgrep",
};
constexpr int kBinCount = static_cast<int>(sizeof(kBinNames) / sizeof(kBinNames[0]));

// A powerline prompt: user segment, path segment, and the separators that
// make them read as one ribbon.
std::string Prompt()
{
    return std::string("\x1b[48;5;24m\x1b[38;5;255m ") + kNfKey + " josh@fedora \x1b[0m" +
           "\x1b[38;5;24m\x1b[48;5;238m" + kNfSep + "\x1b[0m" +
           "\x1b[48;5;238m\x1b[38;5;252m ~ \x1b[0m\x1b[38;5;238m" + kNfSep + "\x1b[0m ";
}

// One row of a listing, the way eza --icons draws it: mode, owner, size,
// date, then the type's glyph and the name in the colour ls would use.
std::string Row(const char* mode, const char* size, const char* date, const char* icon, const char* colour,
                const char* name)
{
    return std::string("\x1b[38;5;244m") + mode + "\x1b[0m josh josh \x1b[38;5;180m" + size +
           "\x1b[0m \x1b[38;5;108m" + date + "\x1b[0m  " + colour + icon + "  " + name + "\x1b[0m\r\n";
}

std::string Bar(double t)
{
    const int width = 30;
    const int filled = std::clamp(static_cast<int>(t * width + 0.5), 0, width);
    std::string bar;
    for (int i = 0; i < width; ++i)
        bar += i < filled ? "\xe2\x96\x88" : "\xe2\x96\x91";
    char line[200];
    snprintf(line, sizeof line, "\r  \x1b[38;5;214m%s\x1b[0m %3d%%  %5.1f MB", bar.c_str(),
             static_cast<int>(t * 100.0 + 0.5), t * 18.4);
    return line;
}

} // namespace

struct App::Reel
{
    struct Step
    {
        double beat;
        std::function<void()> act;
    };
    std::vector<Step> steps;
    size_t next = 0;
    double t0 = 0.0;          // beat zero, absolute
    double beatSecs = 60.0 / 90.5;
    bool started = false;
    // what was borrowed
    int motion = 0, chrome = 0, theme = 0, appearance = 0;
    bool wasFullscreen = false;
    std::string profileId;
    bool done = false;
    // Everything the terminal has been told, so the whole screen can be put
    // back in one frame: the particle choreography fires when a cell's glyph
    // changes, so a redraw of every cell at once is what makes the text
    // reform under a new motion style on the beat.
    std::string screen;
    bool cascade = true;      // the pacing setting, borrowed for the motion act
    bool cascadeBorrowed = false;
};

void App::ReelStop()
{
    if (!m_reel)
        return;
    Reel& r = *m_reel;
    if (r.started)
    {
        m_motionStyle = r.motion;
        m_chromeId = r.chrome;
        amber::SetChrome(m_chromeId);
        m_themeId = r.theme;
        m_appearance = r.appearance;
        ApplyAppearance();
        ApplyTheme();
        if (r.cascadeBorrowed)
            m_fxCascade = r.cascade;
        UpdateMenuChecks();
    }
    delete m_reel;
    m_reel = nullptr;
}

// The reel's own tabs: the terminal it types into, and the desktop.
static amber::Session* ReelTerminal(std::vector<std::unique_ptr<amber::Session>>& sessions)
{
    for (auto& sp : sessions)
        if (sp->label == "reel")
            return sp.get();
    return nullptr;
}
static amber::Session* ReelDesktop(std::vector<std::unique_ptr<amber::Session>>& sessions, const std::string& id)
{
    for (auto& sp : sessions)
        if (sp->vnc && sp->profile.id == id)
            return sp.get();
    return nullptr;
}
static int IndexOf(std::vector<std::unique_ptr<amber::Session>>& sessions, const amber::Session* s)
{
    for (size_t i = 0; i < sessions.size(); ++i)
        if (sessions[i].get() == s)
            return static_cast<int>(i);
    return -1;
}

void App::ReelBuild()
{
    Reel& r = *m_reel;
    auto at = [&](double beat, std::function<void()> act) { r.steps.push_back({ beat, std::move(act) }); };
    auto say = [this, &r](const std::string& text) {
        if (amber::Session* t = ReelTerminal(m_sessions))
            t->localPending += text;
        r.screen += text;
    };
    // A motion style, and the whole screen redrawn in one frame under it:
    // every cell changes at once, so every particle takes the new field's
    // choreography together, on the beat. Needs the cascade pacing off for
    // the act (put back after), or the redraw would type itself instead.
    auto style = [this, &r, say](int i) {
        m_motionStyle = std::clamp(i, 0, kMotionStyleCount - 1);
        const std::string name = MotionStyleAt(static_cast<uint32_t>(m_motionStyle)).name;
        char line[96];
        snprintf(line, sizeof line, "  \x1b[38;5;214m%02d\x1b[0m  %s\r\n", i + 1, name.c_str());
        const std::string whole = "\x1b[2J\x1b[H" + r.screen + line;
        if (amber::Session* t = ReelTerminal(m_sessions))
            t->localPending += whole;
        r.screen += line;
        SetStatus("Motion: " + name, 1.0);
    };
    // The screen the motion styles perform on: a real listing of /usr/bin,
    // in as many columns as the grid takes, filling it edge to edge. It is
    // kept whole in r.screen so a style change can reprint every cell in
    // one frame — which is what makes the entire screenful take the new
    // field's choreography at once, on the beat.
    auto fillScreen = [this, &r]() {
        const int colW = 19;
        const int cols = std::max(1, (static_cast<int>(m_gm.cols) - 2) / colW);
        // Every row under the prompt, right down to the one above the status
        // bar. The last row is deliberately left without its newline so a
        // full screen does not scroll itself by one line.
        const int rows = std::max(4, static_cast<int>(m_gm.rows) - 2);
        const int shown = std::min(kBinCount, cols * rows);
        std::string d = "\x1b[2J\x1b[H" + Prompt() + "ls /usr/bin\r\n";
        for (int i = 0; i < shown; ++i)
        {
            if (i % cols == 0)
                d += " ";
            const char* colour = (i % 9 == 0)   ? "\x1b[38;5;114m"
                                 : (i % 5 == 0) ? "\x1b[38;5;180m"
                                 : (i % 7 == 0) ? "\x1b[38;5;75m"
                                                : "\x1b[38;5;252m";
            char cell[96];
            snprintf(cell, sizeof cell, "%s%s %-*s\x1b[0m", colour, kNfCode, colW - 4, kBinNames[i]);
            d += cell;
            if (i % cols == cols - 1 && i != shown - 1)
                d += "\r\n";
        }
        r.screen = d;
        if (amber::Session* t = ReelTerminal(m_sessions))
            t->localPending += d;
    };
    // One style, and the same screenful reprinted under it.
    auto styleDemo = [this, &r](int i) {
        m_motionStyle = std::clamp(i, 0, kMotionStyleCount - 1);
        if (amber::Session* t = ReelTerminal(m_sessions))
            t->localPending += r.screen;
        SetStatus(std::string("Motion: ") + MotionStyleAt(static_cast<uint32_t>(m_motionStyle)).name, 2.2);
    };
    // The skin changes the chrome, not the text: the listing stays put so
    // the tab strip, the status bar and the frame are what visibly change.
    auto skin = [this](int i) {
        m_chromeId = std::clamp(i, 0, amber::kChromeCount - 1);
        amber::SetChrome(m_chromeId);
        ApplyTheme();
        UpdateMenuChecks();
        SetStatus(std::string("Interface: ") + amber::Chrome().name, 1.2);
    };
    auto appearance = [this](int i) {
        m_appearance = i;
        ApplyAppearance();
    };
    auto desk = [this, &r]() -> amber::Session* { return ReelDesktop(m_sessions, r.profileId); };
    auto key = [desk](uint32_t ks, bool down) {
        if (amber::Session* d = desk())
            if (d->vnc && d->vnc->session)
                d->vnc->session->SendKey(down, ks);
    };
    auto tap = [key](uint32_t ks) { key(ks, true); key(ks, false); };
    auto chord = [key](uint32_t mod, uint32_t ks) { key(mod, true); key(ks, true); key(ks, false); key(mod, false); };
    auto type = [tap](const std::string& s) {
        for (unsigned char c : s)
            tap(amber::vnc::KeysymFromCodePoint(c));
        tap(kReturn);
    };
    auto redraw = [desk](int styleIdx) {
        if (amber::Session* d = desk())
            d->profile.vncTransition = styleIdx;
    };
    auto shock = [this, desk](int shockStyle, float fx, float fy) {
        amber::Session* d = desk();
        if (!d || !d->vnc || !d->vnc->session)
            return;
        amber::VncTab& v = *d->vnc;
        d->profile.vncShockStyle = shockStyle;
        // a real click on the desktop's background, and the wave from it
        const uint16_t px = static_cast<uint16_t>(fx * v.fbW), py = static_cast<uint16_t>(fy * v.fbH);
        v.session->SendPointer(1, px, py);
        v.session->SendPointer(0, px, py);
        v.shockX = v.dstX + px * v.scale;
        v.shockY = v.dstY + py * v.scale;
        v.shockTime = m_time;
        v.shockAmp = 1.0f;
    };
    auto showTerminal = [this]() {
        if (amber::Session* t = ReelTerminal(m_sessions))
        {
            const int i = IndexOf(m_sessions, t);
            if (i >= 0 && i != m_active)
                SelectTab(i, -1);
        }
    };
    auto showDesktop = [this, desk]() {
        if (amber::Session* d = desk())
        {
            const int i = IndexOf(m_sessions, d);
            if (i >= 0 && i != m_active)
                SelectTab(i, -1);
        }
    };

    // The desktop opens the reel. It is already connected before beat zero,
    // but the first beat lands on the black terminal so the flash marker the
    // cut is found by is unmistakable — and so the desktop arrives as a cut,
    // not as a connection.
    at(0.00, [appearance] { appearance(1); });   // the flash: beat zero
    at(0.15, [appearance] { appearance(0); });
    at(0.40, [showDesktop] { showDesktop(); });
    at(1.50, [redraw] { redraw(5); });                                  // light speed
    at(3.00, [shock] { shock(0, 0.70f, 0.60f); });                      // ring
    at(5.00, [shock] { shock(1, 0.45f, 0.50f); });                      // water drop
    at(7.00, [shock] { shock(2, 0.75f, 0.42f); });                      // splash
    at(9.00, [shock] { shock(3, 0.55f, 0.62f); });                      // vortex
    // A terminal on the far side, opened with the desktop's own Ctrl+Alt+T,
    // then real commands on the real machine, each redrawn in its own style.
    at(12.0, [key] {
        key(amber::vnc::XK_Control_L, true);
        key(amber::vnc::XK_Alt_L, true);
        key(amber::vnc::KeysymFromCodePoint(U't'), true);
        key(amber::vnc::KeysymFromCodePoint(U't'), false);
        key(amber::vnc::XK_Alt_L, false);
        key(amber::vnc::XK_Control_L, false);
    });
    at(15.0, [redraw, type] { redraw(7);  type("uname -srm"); });                             // iris
    at(18.0, [redraw, type] { redraw(8);  type("free -h"); });                                // sonic boom
    at(21.0, [redraw, type] { redraw(9);  type("ls -la ~"); });                               // shatter
    at(25.0, [redraw, type] { redraw(10); type("df -h /"); });                                // odometer
    at(28.0, [redraw, type] { redraw(6);  type("systemctl is-active sshd vncserver@:1"); });  // shear plates
    at(32.0, [redraw, type] { redraw(5);  type("top -b -n1 | head -14"); });                  // light speed
    at(37.0, [redraw, type] { redraw(1);  type("exit"); });                                   // burn
    at(40.0, [shock] { shock(1, 0.50f, 0.50f); });
    at(41.5, [shock] { shock(0, 0.50f, 0.50f); });

    // ---- the terminal: the cut is a bang, not a fade --------------------------------
    // Beat 43 lands on the downbeat: the picture goes white for a sixth of a
    // beat, and what comes back is the other half of the application.
    at(43.0, [this, &r, showTerminal, appearance] {
        appearance(1);               // the flash
        showTerminal();
        r.cascade = m_fxCascade;
        r.cascadeBorrowed = true;
        m_fxCascade = false;         // every redraw lands whole, in one frame
        m_motionStyle = 19;          // the wordmark arrives under murmuration
        if (amber::Session* t = ReelTerminal(m_sessions))
            t->localPending +=
                "\x1b[2J\x1b[H\x1b[38;5;214m"
                "    _              _               ___ ___ _  _\r\n"
                "   /_\\  _ __  _ _ | |__  ___ _ _  / __/ __| || |\r\n"
                "  / _ \\| '  \\| '_>| '_ \\/ -_) '_| \\__ \\__ \\ __ |\r\n"
                " /_/ \\_\\_|_|_|_.__|_.__/\\___|_|   |___/___/_||_|\r\n"
                "\x1b[0m\r\n\x1b[1ma particle terminal\x1b[0m"
                "   \x1b[2m(recorded; the figures are this machine's own)\x1b[0m\r\n";
    });
    at(43.16, [appearance] { appearance(0); });

    // The whole machine's /usr/bin, edge to edge, and then six fields in
    // three beats: a montage on the half-beat before the paced demonstration
    // starts. Every one of these is two thousand glyphs re-forming at once.
    at(44.0, [fillScreen] { fillScreen(); });
    {
        const int kRush[] = { 5, 20, 8, 16, 2, 12 };
        for (int i = 0; i < 6; ++i)
            at(44.75 + i * 0.5, [styleDemo, kRush, i] { styleDemo(kRush[i]); });
    }
    // one more flash to close the montage and open the paced act
    at(47.7, [appearance] { appearance(1); });
    at(47.85, [appearance] { appearance(0); });

    // Every one of these is the whole screen taking a different field at
    // once. The listing is real and it never changes: what changes is how
    // two thousand glyphs' worth of particles get back to their cells.
    {
        const int kShow[] = { 2, 4, 5, 6, 8, 12, 16, 19, 20 };   // the ones with the most to look at
        for (int i = 0; i < 9; ++i)
            at(48.5 + i * 2.5, [styleDemo, kShow, i] { styleDemo(kShow[i]); });
    }
    at(71.0, [styleDemo] { styleDemo(0); });

    // ---- the interface styles, over the same screen -------------------------------
    for (int i = 0; i < amber::kChromeCount; ++i)
        at(73 + i, [skin, i] { skin(i); });
    at(73 + amber::kChromeCount, [skin, &r] { skin(r.chrome); });

    // ---- the appearances ----------------------------------------------------------
    at(89, [appearance] { appearance(1); });
    at(90, [appearance] { appearance(2); });
    at(91, [appearance] { appearance(3); });
    at(92, [appearance] { appearance(0); });

    // ---- out ----------------------------------------------------------------------
    at(94, [this, &r] {
        if (r.cascadeBorrowed)
        {
            m_fxCascade = r.cascade;
            r.cascadeBorrowed = false;
        }
        // The screen clears to nothing first: two thousand glyphs leave under
        // the motion field, and the mark is the only thing left standing.
        m_motionStyle = 1;
        if (amber::Session* t = ReelTerminal(m_sessions))
            t->localPending +=
                "\x1b[2J\x1b[H\r\n\r\n\r\n\x1b[38;5;214m"
                "    _              _               ___ ___ _  _\r\n"
                "   /_\\  _ __  _ _ | |__  ___ _ _  / __/ __| || |\r\n"
                "  / _ \\| '  \\| '_>| '_ \\/ -_) '_| \\__ \\__ \\ __ |\r\n"
                " /_/ \\_\\_|_|_|_.__|_.__/\\___|_|   |___/___/_||_|\r\n"
                "\x1b[0m\r\n"
                "  \x1b[1mthe particle terminal\x1b[0m\r\n"
                "  \x1b[2mssh   telnet   rlogin   raw   serial   local   vnc\x1b[0m\r\n";
        r.screen.clear();
    });
    at(101, [appearance] { appearance(1); });   // the end marker
    at(101.15, [this] {
        ApplyAppearance();
        m_reel->done = true;
    });
}

void App::ReelTick()
{
    if (m_reelRequested && !m_reel)
    {
        m_reelRequested = false;
        m_reel = new Reel();
        Reel& r = *m_reel;
        const std::string bpm = Env("AMBER_REEL_BPM");
        if (!bpm.empty())
            r.beatSecs = 60.0 / std::max(30.0, atof(bpm.c_str()));
        const std::string stretch = Env("AMBER_REEL_STRETCH");
        const double k = stretch.empty() ? 1.0 : std::max(0.25, atof(stretch.c_str()));
        r.beatSecs *= k;
        r.profileId = Env("AMBER_REEL_PROFILE");
        r.motion = m_motionStyle;
        r.chrome = m_chromeId;
        r.theme = m_themeId;
        r.appearance = m_appearance;
        r.wasFullscreen = m_fullscreen;
        r.started = true;
        // The stage. The terminal tab is made first and left showing, so
        // beat zero lands on black and the flash marker is unmistakable; the
        // desktop is connected straight away behind it, so that when the
        // reel cuts to it a moment later it is already live. Beat zero is
        // held back far enough for that connection to have arrived.
        StartDiagSession();
        m_sessions.back()->label = "reel";
        m_appearance = 0;
        ApplyAppearance();
        m_motionStyle = 0;
        if (!r.profileId.empty())
        {
            ConnectProfileById(r.profileId);
            if (amber::Session* t = ReelTerminal(m_sessions))
            {
                const int i = IndexOf(m_sessions, t);
                if (i >= 0)
                    SelectTab(i, -1);
            }
        }
        // the window is already maximized (App::Tick); the reel performs in
        // it as the user would see it, title bar and tab strip included
        r.t0 = m_time + (r.profileId.empty() ? 2.0 : 5.0);   // time for the desktop to arrive
        ReelBuild();
        std::sort(r.steps.begin(), r.steps.end(), [](const Reel::Step& a, const Reel::Step& b) { return a.beat < b.beat; });
        return;
    }
    if (!m_reel)
        return;
    Reel& r = *m_reel;
    if (r.done)
    {
        ReelStop();
        return;
    }
    if (!ReelTerminal(m_sessions))
    {
        ReelStop();   // the stage was closed
        return;
    }
    const double beat = (m_time - r.t0) / r.beatSecs;
    while (r.next < r.steps.size() && r.steps[r.next].beat <= beat)
    {
        r.steps[r.next].act();
        ++r.next;
        if (!m_reel)
            return;   // an act may have ended it
    }
}
