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
constexpr uint32_t kReturn = 0xFF0D, kEscape = 0xFF1B, kF1 = 0xFFBE, kF2 = 0xFFBF, kF4 = 0xFFC1, kF7 = 0xFFC4;
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

// The same, for the other directories the reel lists. Each screenful the
// motion styles perform on is a real directory off that machine, so the
// showcase is nine different screens rather than one reprinted nine times.
const char* const kEtcNames[] = {
    "abrt", "adjtime", "aliases", "alsa", "alternatives", "anaconda", "anacrontab", "asound.conf",
    "at.deny", "audit", "authselect", "autofs.conf", "auto.master", "auto.master.d", "auto.misc", "auto.net",
    "auto.smb", "avahi", "bashrc", "binfmt.d", "bluetooth", "brlapi.key", "brltty", "brltty.conf",
    "certmonger", "chrony.conf", "cifs-utils", "cockpit", "containers", "credstore", "cron.d", "cron.daily",
    "cron.deny", "cron.hourly", "cron.monthly", "crontab", "cron.weekly", "crypto-policies", "crypttab", "csh.cshrc",
    "csh.login", "cups", "cupshelpers", "dbus-1", "dconf", "debuginfod", "default", "depmod.d",
    "dhcp", "DIR_COLORS", "dnf", "dnfdragora", "dnsmasq.conf", "dnsmasq.d", "dracut.conf", "dracut.conf.d",
    "eac", "egl", "environment", "ethertypes", "exports", "exports.d", "favicon.png", "fedora-release",
    "filesystems", "firefox", "firewalld", "flatpak", "fonts", "foomatic", "fprintd.conf", "fstab",
    "fuse.conf", "fwupd", "gcrypt", "gdbinit", "gdbinit.d", "geoclue", "glvnd", "gnupg",
    "GREP_COLORS", "groff", "group", "group-", "grub2.cfg", "grub.d", "gshadow", "gshadow-",
    "gss", "gssproxy", "host.conf", "hostname", "hosts", "hp", "idmapd.conf", "ImageMagick-7",
    "initial-setup", "inittab", "inputrc", "intel_lpmd", "ipa", "ipp-usb", "iproute2", "ipsec.conf",
    "ipsec.d", "ipsec.secrets", "iscsi", "issue", "issue.d", "issue.net", "kernel", "keys",
    "keyutils", "krb5.conf", "krb5.conf.d", "ld.so.cache", "ld.so.conf", "ld.so.conf.d", "libaudit.conf", "libblockdev",
    "libibverbs.d", "libnl", "libreport", "libssh", "libuser.conf", "lightdm", "locale.conf", "localtime",
    "login.defs", "logrotate.conf", "logrotate.d", "lvm", "machine-id", "magic", "mailcap", "man_db.conf",
    "mcelog", "mime.types", "mke2fs.conf", "modprobe.d", "modules-load.d", "motd", "motd.d", "mtab",
    "multipath", "netconfig", "NetworkManager", "networks", "nfs.conf", "nfsmount.conf", "nfsmount.conf.d", "nftables",
    "nsswitch.conf", "nvme", "oddjob", "oddjobd.conf", "oddjobd.conf.d", "openal", "openfortivpn", "openldap",
    "opensc.conf", "openvpn", "opt", "os-release", "ostree", "PackageKit", "pam.d", "paperspecs",
    "passwd", "passwd-", "passwdqc.conf", "pinforc", "pkcs11", "pkgconfig", "pki", "plymouth",
    "pm", "polkit-1", "popt.d", "ppp", "printcap", "profile", "profile.d", "protocols",
    "pulse", "purple", "qemu-ga", "rc.d", "rdma", "reader.conf.d", "redhat-release", "request-key.d",
    "resolv.conf", "rpc", "rpm", "rsyncd.conf", "rsyslog.conf", "rsyslog.d", "rwtab.d", "samba",
    "sane.d", "sasl2", "security", "selinux", "sensors3.conf", "sensors.d", "services", "sestatus.conf",
    "setroubleshoot", "sgml", "shadow", "shadow-", "shells", "skel", "smartmontools", "sos",
    "ssh", "ssl", "sssd", "statetab.d", "strongswan", "subgid", "subgid-", "subuid",
    "subuid-", "sudo.conf", "sudoers", "sudoers.d", "swid", "sysconfig", "sysctl.conf", "sysctl.d",
    "systemd", "system-release", "terminfo", "tigervnc", "tmpfiles.d", "tpm2-tss", "trusted-key.key", "ts.conf",
    "udev", "udisks2", "unbound", "updatedb.conf", "UPower", "userdb", "vconsole.conf", "vimrc",
    "virc", "vmware-tools", "vpl", "vpnc", "vulkan", "whois.conf", "wireplumber", "wpa_supplicant",
    "X11", "xattr.conf", "xdg", "xml", "yum.repos.d",
};
const char* const kLib64Names[] = {
    "alsa-lib", "atril", "autofs", "bfd-plugins", "bpf", "cifs-utils", "cmake", "colord-sensors",
    "device-mapper", "dri", "dri-nonfree", "enchant", "engines-3", "fipscheck", "freerdp3", "games",
    "gbm", "gdk-pixbuf-2.0", "gio", "grilo-0.3", "gstreamer-1.0", "gtk-3.0", "gutenprint", "hmaccalc",
    "krb5", "liba52.so.0.0.0", "libacl.so.1", "libaio.so.1.0.0", "libaml.so.0", "libanl.so.1", "libao.so.4", "libaribb24.so.0",
    "libasm.so.1", "libassimp.so.6", "libass.so.9.4.1", "libasyncns.so.0", "libatm.so.1", "libatomic.so.1", "libattr.so.1", "libaugeas.so.0",
    "libautofs.so", "libavif.so.16", "libb2.so.1", "libbd_dm.so.3", "libbd_loop.so.3", "libbd_nvme.so.3", "libbd_swap.so.3", "libbluray.so.3",
    "libbpf.so.1.6.3", "libburn.so.4", "libbz2.so.1.0.8", "libcaca.so.0", "libcamera", "libcap.so.2", "libcares.so.2", "libcddb.so.2",
    "libcdio.so.19", "libcdt.so.6.0.3", "libchewing.so.3", "libclastfm.so.0", "libcom_err.so.2", "libcrack.so.2", "libcrypt.so.2", "libctf-nobfd.so",
    "libctf.so.0", "libcue.so.2", "libcups.so.2", "libdaemon.so.0", "libdav1d.so.7", "libdb-5.so", "libdc1394.so.26", "libdconf.so.1",
    "libdecor-0.so.0", "libdhash.so.1", "libdnf", "libdnf5.so.2", "libdotconf.so.0", "libdrm.so.2", "libdvdread.so.8", "libdw.so.1",
    "libe2p.so.2.3", "libeac.so.3.1.0", "libeconf.so.0", "libefa.so.1", "libEGL.so.1.1.0", "libei.so.1.6.0", "libelf.so.1", "libepoxy.so.0",
    "libetpan.so.20", "libev.so.4", "libexempi.so.8", "libexiv2.so.28", "libexpat.so.1", "libext2fs.so.2", "libfa.so.1", "libfdisk.so.1",
    "libffi.so.8", "libfftw3f.so.3", "libfido2.so.1", "libFLAC.so.14", "libflite.so.1", "libfmt.so.11", "libform.so.6", "libformw.so.6",
    "libfreebl3.chk", "libfribidi.so.0", "libfuse3.so.4", "libfyaml.so.0", "libgbm.so.1", "libgcc_s.so.1", "libgck-2.so.2", "libgcrypt.so.20",
    "libgdk-3.so.0", "libgd.so.3.0.11", "libgeany.so.0", "libgif.so.7.1.0", "libGLESv2.so.2", "libGL.so.1.7.0", "libGLU.so.1.3.1", "libGLX.so.0",
    "libgme.so.0", "libgmp.so.10", "libgomp.so.1", "libgpgme.so.45", "libgphoto2_port", "libgpm.so.2", "libgsf-1.so.114", "libgs.so.10",
    "libgssrpc.so.4", "libgtk-3.so.0", "libgusb.so.2", "libgvc.so.7.0.8", "libgxps.so.2", "libhandy-1.so.0", "libheif", "libhgfs.so.0",
    "libhistory.so.8", "libhogweed.so.6", "libhpip.so.0", "libhwy.so.1", "libhyphen.so.0", "libibverbs.so.1", "libicalss.so.3", "libICE.so.6.3.0",
    "libicutu.so.77", "libidn2.so.0", "libigdgmm.so.12", "libilbc.so.3", "libinih.so.0", "libionic.so.1", "libipt.so.2.1.2", "libisns.so.0",
    "libjansson.so.4", "libjbig.so.2.1", "libjose.so.0", "libjq.so.1", "libjson-c.so.5", "libkadm5clnt.so", "libkcapi.so.1", "libkdb5.so.10.0",
    "libkmod.so.2", "libkrad.so.0.0", "libkrb5.so.3.3", "liblber.so.2", "liblcms2.so.2", "libldb.so.2", "libLerc.so.4", "libLLVM-22.so",
    "liblockdev.so.1", "liblqr-1.so.0", "libLTO.so", "liblua-5.4.so", "liblzma.so.5", "libm17n.so.0", "libmana.so.1", "libmenu.so.6.6",
    "libmenuw.so.6.6", "libmfxhw64.so.1", "libmfx.so.1.35", "libmlx4.so.1", "libmm-glib.so.0", "libmng.so.2.0.2", "libmnl.so.0.2.0", "libmount.so.1",
    "libmp3lame.so.0", "libmpc.so.3", "libmpdec.so.4", "libmpfr.so.6", "libm.so.6", "libmtdev.so.1", "libmtp.so.9.4.0", "libmvec.so.1",
    "libnatpmp.so.1", "libndp.so.0", "libndr-nbt.so.0", "libndr.so.6.1.0", "libnetapi.so.1", "libnewt.so.0.52", "libnftnl.so.11", "libngtcp2.so.16",
    "libnl", "libnma.so.0", "libnm.so.0", "libnotify.so.4", "libnsl.so.3", "libnspr4.so", "libnssckbi.so", "libnss_sss.so.2",
    "libnuma.so.1", "libnvme.so.1", "liboauth.so.0", "libogg.so.0", "libonig.so.5", "libOpenGL.so.0", "libopenmpt.so.0", "libopusenc.so.0",
    "libopusurl.so.0", "libout123.so.0", "libpamc.so.0", "libpanel.so.6", "libpanelw.so.6", "libparted.so.2", "libpcap.so.1", "libpci.so.3",
    "libpcre2-8.so.0", "libperl.so.5.42", "libpinyin", "libpisock.so.9", "libpkgconf.so.7", "libplds4.so", "libply.so.5.0.0", "libpopt.so.0",
    "libppd.so.2.0.0", "libproxy", "libpskc.so.0", "libpsl.so.5.3.5", "libpsx.so.2.77", "libpthread.so.0", "libpulse.so.0", "libpython3.so",
    "libQt6Core.so.6", "libQt6Gui.so.6", "libQt6Nfc.so.6", "libQt6Qml.so.6", "libQt6Svg.so.6", "libQt6Xml.so.6", "librav1e.so.0", "libraw.so.25",
    "libre2.so.11", "libreport.so.2", "libresolv.so.2", "librom1394.so.0", "librpm.so.10", "librt.so.1", "libsane.so.1", "libsatyr.so.4",
    "libsbc.so.1.3.1", "libseat.so.1", "libselinux.so.1", "libsepol.so.2", "libsframe.so", "libshout.so.3", "libslapi.so.2", "libsmbldap.so.2",
    "libSM.so.6", "libsnappy.so.1", "libsodium.so.26", "libsoftokn3.so", "libsolv.so.1", "libsoxr.so.0", "libspectre.so.1", "libspeex.so.1",
    "libsrtp2.so.1", "libsrt.so.1.5.6", "libssl3.so", "libssl.so.3.5.5", "libss.so.2", "libsss_sudo.so", "libstemmer.so.0", "libsubid.so.5",
    "libswscale.so.9", "libsynctex.so.2", "libtag_c.so.2", "libtag.so.2.3.0", "libtasn1.so.6", "libtdb.so.1", "libtevent.so.0", "libtheora.so.0",
    "libtic.so.6.6", "libtiffxx.so.6", "libtinfo.so.6.6", "libtommath.so.1", "libtss2-rc.so.0", "libts.so.0.10.5", "libudev.so.1", "libudf.so.0",
    "libudisks2.so.0", "libunbound.so.8", "liburcu-bp.so.8", "liburcu.so.8",
};
const char* const kShareNames[] = {
    "abrt", "accountsservice", "aclocal", "adobe", "alsa", "anaconda", "anthy-unicode", "antiword",
    "appdata", "applications", "appstream", "at", "atril", "audit-rules", "augeas", "authselect",
    "avahi", "awk", "backgrounds", "bash-completion", "blivet-gui", "blueman", "catfish", "cmake",
    "cockpit", "color", "colord", "config.kcfg", "containers", "cracklib", "crypto-policies", "cups",
    "dbus-1", "defaults", "dict", "dnf5", "dnfdragora", "dnsmasq", "dns-root-data", "doc",
    "drirc.d", "egl", "emacs", "emoticons", "empty", "empty.sshd", "enchant", "enchant-2-2",
    "espeak-ng-data", "etc", "factory", "farstream", "fedora-logos", "ffmpeg", "file", "firewalld",
    "fish", "flatpak", "fontconfig", "fonts", "foomatic", "FreeRDP", "fwupd", "galculator",
    "games", "gawk", "gcc-16", "GConf", "gdb", "gdm", "geany", "gettext",
    "ghostscript", "gir-1.0", "glib-2.0", "glvnd", "glycin-loaders", "gnome", "gnome-abrt", "gnome-shell",
    "gnupg", "grilo-0.3", "groff", "grub", "gstreamer-1.0", "gtk-2.0", "gtk-3.0", "gtk-4.0",
    "gtk-engines", "gtksourceview-4", "gtksourceview-5", "gutenprint", "gvfs", "help", "hplip", "hunspell",
    "hwdata", "hyphen", "i18n", "ibus", "ibus-anthy", "ibus-chewing", "ibus-hangul", "ibus-libpinyin",
    "ibus-m17n", "ibus-table", "icewm", "icons", "idl", "ima", "ImageMagick-7", "imchooseui",
    "info", "ipa", "ipp-usb", "iproute2", "iso-codes", "javascript", "kconf_update", "kde4",
    "keyutils", "kf6", "kio", "knsrcfiles", "kpackage", "kservices6", "kservicetypes6", "libcamera",
    "libchewing", "libdrm", "libgpg-error", "libgphoto2", "libhangul", "libinput", "liblouis", "liblouisutdml",
    "libreport", "libthai", "libwacom", "licenses", "lightdm", "locale", "localsearch3", "lua",
    "m17n", "magic", "man", "mdadm", "metainfo", "mfx", "mime", "mime-info",
    "misc", "ModemManager", "modulefiles", "mozilla", "mpage", "myspell", "nfs-utils", "omf",
    "openal", "openfortivpn", "opensc", "open-vm-tools", "osinfo", "os-prober", "p11-kit", "PackageKit",
    "pam.d", "paps", "parole", "perl5", "pipewire", "pixmaps", "pkgconfig", "pki",
    "plymouth", "polkit-1", "poppler", "ppd", "publicsuffix", "python-meh", "python-wheels", "qemu",
    "qt6", "rhel", "rootfiles", "sane", "seahorse", "selinux", "setroubleshoot", "sgml",
    "smartmontools", "snmp", "solid", "sounds", "spa-0.2", "spandsp", "sssd", "sssd-kcm",
    "strongswan", "swcatalog", "systemd", "systemtap", "tabset", "tcl9", "tcl9.0", "templates",
    "terminfo", "tesseract", "texlive", "themes", "thumbnailers", "Thunar", "transmission", "udica",
    "unicode", "usb_modeswitch", "vala", "vim", "vulkan", "wallpapers", "web-assets", "weston",
    "WinPR", "wireplumber", "X11", "xdg-terminals", "xfburn", "xfce4", "xfsprogs", "xfwm4",
    "xgreeters", "xml", "xsessions", "xwayland", "zoneinfo", "zsh",
};

// A directory the reel can list: its name pool and the command that prints it.
struct DirListing
{
    const char*        cmd;
    const char* const* names;
    int                count;
};
const DirListing kDirs[] = {
    { "ls /usr/bin",   kBinNames,   static_cast<int>(sizeof(kBinNames)   / sizeof(kBinNames[0]))   },
    { "ls /etc",       kEtcNames,   static_cast<int>(sizeof(kEtcNames)   / sizeof(kEtcNames[0]))   },
    { "ls /usr/lib64", kLib64Names, static_cast<int>(sizeof(kLib64Names) / sizeof(kLib64Names[0])) },
    { "ls /usr/share", kShareNames, static_cast<int>(sizeof(kShareNames) / sizeof(kShareNames[0])) },
};
constexpr int kDirCount = static_cast<int>(sizeof(kDirs) / sizeof(kDirs[0]));

// /var/log as ls -lhA printed it, for the long-format screen.
struct LogEntry { const char* mode; const char* size; const char* date; const char* name; };
const LogEntry kVarLog[] = {
    { "drwxr-xr-x.", "175", "Sep 3 16:04", "anaconda" },
    { "drwx------.", "23", "Sep 4 02:06", "audit" },
    { "drwxr-xr-x.", "6", "Jan 15 16:00", "blivet-gui" },
    { "-rw-------.", "316K", "Sep 5 12:12", "boot.log" },
    { "-rw-rw----.", "1.5K", "Sep 4 02:30", "btmp" },
    { "drwxr-x---.", "6", "Sep 4 02:06", "chrony" },
    { "-rw-------.", "41K", "Sep 5 13:01", "cron" },
    { "drwxr-xr-x.", "6", "May 25 17:00", "cups" },
    { "-rw-r--r--.", "778K", "Sep 5 12:34", "dnf5.log" },
    { "-rw-r--r--.", "1.0M", "Sep 5 05:30", "dnf5.log.1" },
    { "-rw-r--r--.", "1.0M", "Sep 5 05:29", "dnf5.log.2" },
    { "-rw-r--r--.", "1.0M", "Sep 4 22:26", "dnf5.log.3" },
    { "-rw-r--r--.", "1.0M", "Sep 4 15:18", "dnf5.log.4" },
    { "-rw-r-----.", "0", "Sep 4 02:06", "firewalld" },
    { "drwxr-sr-x+", "46", "Sep 4 02:06", "journal" },
    { "-rw-rw-r--.", "286K", "Sep 5 12:07", "lastlog" },
    { "drwxr-xr-x.", "6", "Aug 16 17:00", "lightdm" },
    { "-rw-------.", "0", "Sep 3 15:59", "maillog" },
    { "-rw-------.", "8.9M", "Sep 5 13:35", "messages" },
    { "drwx------.", "6", "Jan 16 16:00", "ppp" },
    { "drwx------.", "6", "Sep 3 15:55", "private" },
    { "drwxr-xr-x.", "6", "Mar 22 17:00", "qemu-ga" },
    { "lrwxrwxrwx.", "39", "Sep 3 15:55", "README" },
    { "drwx------.", "17", "Aug 13 17:00", "samba" },
    { "-rw-------.", "150K", "Sep 5 13:35", "secure" },
    { "drwx------.", "6", "Jan 16 16:00", "speech-dispatcher" },
    { "-rw-------.", "0", "Sep 3 15:59", "spooler" },
    { "drwxrwx---.", "6", "Mar 10 17:00", "sssd" },
    { "-rw-rw-r--.", "45K", "Sep 5 12:12", "wtmp" },
};
constexpr int kVarLogCount = static_cast<int>(sizeof(kVarLog) / sizeof(kVarLog[0]));

// Real files on that machine, with the sizes ls reports, for the transfer
// screen: the reel pulls them over sftp and shows each one's progress.
struct Xfer { const char* name; double mb; };
const Xfer kXfers[] = {
    { "/usr/lib64/libQt6WebEngineCore.so.6.11.2", 204.50 },
    { "/usr/lib64/firefox/libxul.so", 167.85 },
    { "/usr/lib64/libLLVM.so.22.1", 140.27 },
    { "/usr/lib64/libwebkit2gtk-4.1.so.0.21.9", 89.03 },
    { "/usr/share/fonts/google-noto-serif-cjk-vf-fonts/NotoSerifCJK-VF.ttc", 54.83 },
    { "/usr/lib64/libgallium-26.1.8.so", 52.04 },
    { "/usr/lib64/firefox/omni.ja", 42.44 },
    { "/usr/lib64/libjavascriptcoregtk-4.1.so.0.10.13", 32.15 },
    { "/usr/share/fonts/google-noto-sans-cjk-vf-fonts/NotoSansCJK-VF.ttc", 31.17 },
    { "/usr/share/fonts/google-noto-sans-mono-cjk-vf-fonts/NotoSansMonoCJK-VF.ttc", 30.43 },
    { "/usr/lib64/libicudata.so.77.1", 30.43 },
    { "/usr/lib64/libvulkan_intel.so", 23.63 },
    { "/usr/lib64/libQt6Pdf.so.6.11.2", 23.55 },
    { "/usr/lib64/libgs.so.10.06", 22.34 },
    { "/usr/lib64/libvulkan_intel_hasvk.so", 18.65 },
    { "/usr/lib64/libvulkan_radeon.so", 17.71 },
    { "/usr/lib64/libvulkan_panfrost.so", 15.98 },
    { "/usr/lib64/libvulkan_nouveau.so", 14.82 },
    { "/usr/lib64/liblpcnetfreedv.so.0.5", 14.81 },
    { "/usr/lib64/libvulkan_freedreno.so", 14.14 },
    { "/usr/lib64/libvulkan_lvp.so", 14.06 },
    { "/usr/lib64/libvulkan_asahi.so", 13.92 },
    { "/usr/lib64/libmfxhw64.so.1.35", 13.73 },
    { "/usr/lib64/dri/iHD_drv_video.so", 11.82 },
    { "/usr/lib64/libvulkan_powervr_mesa.so", 11.77 },
    { "/usr/lib64/libgtk-4.so.1.2200.4", 11.51 },
    { "/usr/lib64/libvulkan_broadcom.so", 11.46 },
    { "/usr/lib64/libvulkan_dzn.so", 11.39 },
};
constexpr int kXferCount = static_cast<int>(sizeof(kXfers) / sizeof(kXfers[0]));

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
    auto fillScreen = [this, &r](int dirIdx) {
        const DirListing& dir = kDirs[std::clamp(dirIdx, 0, kDirCount - 1)];
        const int colW = 19;
        const int cols = std::max(1, (static_cast<int>(m_gm.cols) - 2) / colW);
        // Every row under the prompt, right down to the one above the status
        // bar. The last row is deliberately left without its newline so a
        // full screen does not scroll itself by one line.
        const int rows = std::max(4, static_cast<int>(m_gm.rows) - 2);
        const int shown = std::min(dir.count, cols * rows);
        std::string d = "\x1b[2J\x1b[H" + Prompt() + dir.cmd + "\r\n";
        for (int i = 0; i < shown; ++i)
        {
            if (i % cols == 0)
                d += " ";
            const char* colour = (i % 9 == 0)   ? "\x1b[38;5;114m"
                                 : (i % 5 == 0) ? "\x1b[38;5;180m"
                                 : (i % 7 == 0) ? "\x1b[38;5;75m"
                                                : "\x1b[38;5;252m";
            char cell[96];
            snprintf(cell, sizeof cell, "%s%s %-*s\x1b[0m", colour, kNfCode, colW - 4, dir.names[i]);
            d += cell;
            if (i % cols == cols - 1 && i != shown - 1)
                d += "\r\n";
        }
        r.screen = d;
        if (amber::Session* t = ReelTerminal(m_sessions))
            t->localPending += d;
    };
    // The long form: /var/log the way ls -lhA prints it, in two columns so
    // the mode strings and sizes fill the width the wide listing fills with
    // names. Different shape, same machine.
    auto longScreen = [this, &r]() {
        std::string d = "\x1b[2J\x1b[H" + Prompt() + "ls -lhA /var/log\r\n"
                        "\x1b[38;5;244mtotal 12M\x1b[0m\r\n";
        const int half = (kVarLogCount + 1) / 2;
        for (int i = 0; i < half; ++i)
        {
            for (int c = 0; c < 2; ++c)
            {
                const int k = i + c * half;
                if (k >= kVarLogCount)
                    break;
                const LogEntry& e = kVarLog[k];
                const bool isDir = e.mode[0] == 'd';
                char cell[220];
                snprintf(cell, sizeof cell,
                         " \x1b[38;5;244m%-11s\x1b[0m \x1b[38;5;180m%5s\x1b[0m "
                         "\x1b[38;5;108m%-12s\x1b[0m %s%s  %-18s\x1b[0m",
                         e.mode, e.size, e.date,
                         isDir ? "\x1b[38;5;75m" : "\x1b[38;5;252m",
                         isDir ? kNfFolder : kNfFile, e.name);
                d += cell;
            }
            d += "\r\n";
        }
        r.screen = d;
        if (amber::Session* t = ReelTerminal(m_sessions))
            t->localPending += d;
    };
    // A transfer: every file on its own row with its own bar, all of them
    // moving at once. t is how far through the batch we are, so the whole
    // block can be reprinted a few times a beat and the screen is never
    // still. Each row's own progress is staggered off the batch's.
    auto transferScreen = [this, &r](double t, bool secure) {
        char head[220];
        snprintf(head, sizeof head,
                 "\x1b[2J\x1b[H%s%s\r\n\x1b[38;5;244m%s\x1b[0m\r\n\r\n",
                 Prompt().c_str(),
                 secure ? "sftp -P 2222 josh@fedora:/usr/lib64/ ." : "rsync -az --info=progress2 fedora:/usr/share/ .",
                 secure ? "Connected. 28 files queued." : "receiving incremental file list");
        std::string d = head;
        double doneMb = 0.0, totalMb = 0.0;
        for (int i = 0; i < kXferCount; ++i)
        {
            const double lead = static_cast<double>(i) / (kXferCount * 1.6);
            const double p = std::clamp((t - lead) / 0.42, 0.0, 1.0);
            const int width = 30;
            const int filled = static_cast<int>(p * width + 0.5);
            std::string bar;
            for (int k = 0; k < width; ++k)
                bar += k < filled ? "\xe2\x96\x88" : "\xe2\x96\x91";
            const bool complete = p >= 1.0;
            char row[512];
            snprintf(row, sizeof row,
                     "  %s%s\x1b[0m %-74s \x1b[38;5;%dm%s\x1b[0m %3d%%  \x1b[38;5;180m%6.1f\x1b[0m/%6.1f MB  %s\r\n",
                     complete ? "\x1b[38;5;114m" : "\x1b[38;5;244m",
                     complete ? kNfCheck : kNfDown,
                     kXfers[i].name,
                     complete ? 114 : 214, bar.c_str(),
                     static_cast<int>(p * 100.0 + 0.5), p * kXfers[i].mb, kXfers[i].mb,
                     complete ? "\x1b[38;5;114mdone\x1b[0m" : "");
            d += row;
            doneMb += p * kXfers[i].mb;
            totalMb += kXfers[i].mb;
        }
        char foot[220];
        snprintf(foot, sizeof foot,
                 "\r\n  \x1b[38;5;244m%s\x1b[0m  \x1b[1m%5.2f\x1b[0m of %.2f MB   %s  \x1b[38;5;108m%.1f MB/s\x1b[0m\r\n",
                 kNfLock, doneMb, totalMb, kNfClock, 4.2 + 1.6 * t);
        d += foot;
        r.screen = d;
        if (amber::Session* t2 = ReelTerminal(m_sessions))
            t2->localPending += d;
    };
    // One style, and the same screenful reprinted under it.
    auto styleDemo = [this, &r](int i) {
        m_motionStyle = std::clamp(i, 0, kMotionStyleCount - 1);
        if (amber::Session* t = ReelTerminal(m_sessions))
            t->localPending += r.screen;
        SetStatus(std::string("Motion: ") + MotionStyleAt(static_cast<uint32_t>(m_motionStyle)).name, 2.2);
    };
    // A style change and a brand new screen under it: the showcase slot.
    auto styleDir = [this, fillScreen](int styleIdx, int dirIdx) {
        m_motionStyle = std::clamp(styleIdx, 0, kMotionStyleCount - 1);
        fillScreen(dirIdx);
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
    at(1.20, [redraw] { redraw(5); });                                  // light speed
    at(1.80, [shock] { shock(0, 0.70f, 0.60f); });                      // ring

    // The applications menu, opened by the desktop's own Alt+F1, and then
    // walked. Every arrow is a real key on the far side and every highlight
    // is a real repaint coming back over RFB: the whole menu redraws in
    // particles five times a second.
    at(2.40, [chord] { chord(amber::vnc::XK_Alt_L, kF1); });
    for (int i = 0; i < 11; ++i)
        at(2.80 + i * 0.28, [tap] { tap(kDown); });
    at(6.10, [tap] { tap(kRight); });                                   // into a submenu
    for (int i = 0; i < 4; ++i)
        at(6.40 + i * 0.28, [tap] { tap(kDown); });
    at(7.70, [tap] { tap(kEscape); });
    at(7.90, [tap] { tap(kEscape); });
    at(8.30, [shock] { shock(1, 0.45f, 0.50f); });                      // water drop

    // The file manager, opened with Super+E, put through its three views and
    // sent somewhere else, then closed. A window opening and closing is the
    // hardest thing to draw in particles and the best thing to watch.
    at(8.50, [chord] { chord(amber::vnc::XK_Super_L, amber::vnc::KeysymFromCodePoint(U'e')); });
    at(11.6, [chord] { chord(amber::vnc::XK_Control_L, amber::vnc::KeysymFromCodePoint(U'2')); });
    for (int i = 0; i < 5; ++i)
        at(12.0 + i * 0.28, [tap] { tap(kDown); });
    at(13.5, [chord] { chord(amber::vnc::XK_Control_L, amber::vnc::KeysymFromCodePoint(U'3')); });
    for (int i = 0; i < 4; ++i)
        at(13.9 + i * 0.28, [tap] { tap(kRight); });
    at(15.1, [chord] { chord(amber::vnc::XK_Control_L, amber::vnc::KeysymFromCodePoint(U'1')); });
    for (int i = 0; i < 5; ++i)
        at(15.5 + i * 0.28, [tap] { tap(kRight); });
    // Alt+F4, not the application's own quit: it goes to the window manager,
    // so it lands wherever the keyboard focus happens to be inside the window.
    // Ctrl+W, the window's own close. Neither Alt+F4 nor Ctrl+Q reaches it
    // through this server — tried both, and only this one lands.
    at(17.0, [chord] { chord(amber::vnc::XK_Control_L, amber::vnc::KeysymFromCodePoint(U'w')); });
    at(17.8, [shock] { shock(2, 0.75f, 0.42f); });                      // splash

    // A terminal on the far side, opened with the desktop's own Ctrl+Alt+T,
    // then real commands on the real machine two beats apart, each one
    // rematerialising in a style of its own.
    at(18.4, [key] {
        key(amber::vnc::XK_Control_L, true);
        key(amber::vnc::XK_Alt_L, true);
        key(amber::vnc::KeysymFromCodePoint(U't'), true);
        key(amber::vnc::KeysymFromCodePoint(U't'), false);
        key(amber::vnc::XK_Alt_L, false);
        key(amber::vnc::XK_Control_L, false);
    });
    at(20.4, [redraw, type] { redraw(7);  type("uname -srm"); });                             // iris
    at(22.6, [redraw, type] { redraw(8);  type("free -h"); });                                // sonic boom
    at(24.8, [redraw, type] { redraw(9);  type("ls -la /etc | head -18"); });                 // shatter
    at(27.0, [redraw, type] { redraw(10); type("df -h /"); });                                // odometer
    at(29.2, [redraw, type] { redraw(6);  type("systemctl is-active sshd vncserver@:1"); });  // shear plates
    at(31.4, [redraw, type] { redraw(5);  type("ip -br a"); });                               // light speed
    at(33.6, [redraw, type] { redraw(8);  type("top -b -n1 | head -12"); });                  // sonic boom
    at(35.8, [redraw, type] { redraw(1);  type("exit"); });                                   // burn
    at(36.9, [shock] { shock(1, 0.50f, 0.50f); });
    at(37.7, [shock] { shock(3, 0.50f, 0.50f); });                      // vortex

    // ---- the terminal: the cut is a bang, not a fade --------------------------------
    // Beat 43 lands on the downbeat: the picture goes white for a sixth of a
    // beat, and what comes back is the other half of the application.
    at(38.5, [this, &r, showTerminal, appearance] {
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
    at(38.66, [appearance] { appearance(0); });

    // The whole machine's /usr/bin, edge to edge, and then six fields in
    // three beats: a montage on the half-beat before the paced demonstration
    // starts. Every one of these is two thousand glyphs re-forming at once.
    at(39.5, [fillScreen] { fillScreen(0); });
    {
        const int kRush[] = { 5, 20, 8, 16, 2, 12 };
        for (int i = 0; i < 6; ++i)
            at(40.25 + i * 0.5, [styleDemo, kRush, i] { styleDemo(kRush[i]); });
    }
    // one more flash to close the montage and open the paced act
    at(43.2, [appearance] { appearance(1); });
    at(43.35, [appearance] { appearance(0); });

    // The showcase: nine slots of two and a half beats, and every one of
    // them is a different screen off the same machine arriving under a
    // different motion field. Four wide listings, two long ones, two
    // transfers with every file's bar moving at once, and the big one last.
    at(44.00, [styleDir]   { styleDir(2, 1); });                        // digital rain — /etc
    at(46.65, [styleDir]   { styleDir(4, 2); });                        // sonic boom — /usr/lib64
    at(49.30, [this, longScreen] { m_motionStyle = 5; longScreen();     // magnetic assemble — ls -lhA
                                   SetStatus("Motion: " + std::string(MotionStyleAt(5).name), 2.2); });
    // The first transfer: the block is reprinted six times a beat, so the
    // bars are never in the same place two frames running.
    at(51.95, [this] { m_motionStyle = 6;
                       SetStatus("Motion: " + std::string(MotionStyleAt(6).name), 2.2); });
    for (int i = 0; i <= 13; ++i)
        at(51.95 + i * 0.18, [transferScreen, i] { transferScreen(i / 13.0, true); });
    at(54.60, [styleDir]   { styleDir(8, 3); });                        // glitch — /usr/share
    at(57.25, [styleDir]   { styleDir(12, 0); });                       // starwake — /usr/bin
    at(59.90, [this] { m_motionStyle = 16;
                       SetStatus("Motion: " + std::string(MotionStyleAt(16).name), 2.2); });
    for (int i = 0; i <= 13; ++i)
        at(59.90 + i * 0.18, [transferScreen, i] { transferScreen(i / 13.0, false); });
    at(62.55, [this, longScreen] { m_motionStyle = 19; longScreen();    // murmuration — ls -lhA
                                   SetStatus("Motion: " + std::string(MotionStyleAt(19).name), 2.2); });
    at(65.20, [styleDir]   { styleDir(20, 2); });                       // hammer — /usr/lib64
    at(67.85, [styleDemo] { styleDemo(0); });

    // ---- the interface styles, over the same screen -------------------------------
    // Fifteen of them, a little over half a beat each: fast enough to read
    // as one run through the set rather than fifteen separate looks.
    for (int i = 0; i < amber::kChromeCount; ++i)
        at(68.5 + i * 0.55, [skin, i] { skin(i); });
    at(68.5 + amber::kChromeCount * 0.55, [skin, &r] { skin(r.chrome); });

    // ---- the appearances ----------------------------------------------------------
    at(77.5, [appearance] { appearance(1); });
    at(78.3, [appearance] { appearance(2); });
    at(79.1, [appearance] { appearance(3); });
    at(79.9, [appearance] { appearance(0); });

    // ---- out ----------------------------------------------------------------------
    at(82.0, [this, &r] {
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
    at(90.0, [appearance] { appearance(1); });   // the end marker
    at(90.15, [this] {
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
            // Driving a file manager selects things, and a selection on the
            // far side is a clipboard offer coming back. The consent box is
            // right for a person and wrong for a camera, so this connection
            // takes no clipboard at all. It is the session's own copy of the
            // profile: the saved one is untouched.
            if (amber::Session* d = ReelDesktop(m_sessions, r.profileId))
                d->profile.vncClipboard = 0;
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
