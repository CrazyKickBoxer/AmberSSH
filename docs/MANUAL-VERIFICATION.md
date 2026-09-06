# Manual verification

Everything in this list needs a person and a screen. It exists because the
security work in AUDIT.md was verified by compiling, by unit tests on the pure
parts, and by reading the call sites — and only one of it has been watched
running.

Work top to bottom. The first section is the security gates, and those are the
ones where "it compiles" is worth least.

Paths assume `build\Release\AmberSSH.exe`.

---

## 1. Security gates

### 1.1 A remote host cannot silently take the clipboard (S3)

The fixture replays a recording that carries an OSC 52 clipboard write. It
needs no server: playback binds the same sinks a live session does.

```
build\Release\AmberSSH.exe --play tests\fixtures\osc52-clipboard-write.cast
```

Set the clipboard to something recognisable first, so you can tell whether it
moved:

```powershell
Set-Clipboard -Value 'ORIGINAL'
```

- [ ] A consent box appears naming the size of the transfer, about half a
      second in.
- [ ] Declining leaves the clipboard as `ORIGINAL` and the status line says
      the write was refused.
- [ ] Accepting sets the clipboard to `clipboard hijack attempt`.
- [ ] Connection settings, Features page, has **Remote clipboard (OSC 52)**
      with Disabled / Ask each time / Always allow, defaulting to Ask.
- [ ] Set it to Disabled, replay: no box, clipboard unchanged.

**If no box appears and the clipboard changes anyway,** the gate is not wired
and this is the finding that matters most in the whole list.

### 1.2 A pasted escape cannot break out of bracketed paste (S4)

```powershell
Set-Clipboard -Value ([char]27 + "[201~echo INJECTED")
```

Open any session, press Ctrl+V.

- [ ] The paste preview appears even though the text has no line break.
- [ ] The status line says an escape character was removed.
- [ ] The shell receives `[201~echo INJECTED` as literal text on the command
      line, not as a command that runs.

**The old behaviour** was no preview at all and `echo INJECTED` sitting on the
command line ready for your Enter.

### 1.3 A recording does not leak what the screen masked (S2)

Turn the Privacy Cloak on, start a recording (File, Record Session), then in
the session produce something that looks like a secret:

```
echo "export AWS_SECRET_ACCESS_KEY=wJalrXUtnFEMIK7MDENGbPxRfiCYEXAMPLEKEY"
```

Stop the recording and open the `.cast` in a text editor.

- [ ] The key does not appear anywhere in the file.
- [ ] `[redacted]` does appear.
- [ ] Replaying the file still renders correctly — colours and cursor moves
      intact, not a stream of garbage.

The third one matters as much as the first: masking a recording has to leave
its escape sequences alone or the replay is ruined.

### 1.4 SFTP refuses hostile filenames (S1, S9)

Needs a server you control and can create odd filenames on. On the Fedora VM:

```bash
mkdir -p /tmp/hostile && cd /tmp/hostile
touch 'ok.txt' && touch $'..\\..\\..\\evil.exe' && touch 'CON' && touch 'a.txt '
printf 'x' > $'evil‮gnp.exe'
ln -s / to-root
```

Open the SFTP browser, navigate there, download the whole directory.

- [ ] `ok.txt` arrives.
- [ ] Nothing is written outside the folder you chose. Check the parent, and
      check `shell:startup`.
- [ ] The status line reports entries refused.
- [ ] `to-root` is not descended and the transfer does not run away.

### 1.5 known_hosts failures are reported (Q5)

Back up `%USERPROFILE%\.ssh\known_hosts` first.

Corrupt it (write a line of nonsense), then connect to a host already in it:

- [ ] A status line says known_hosts could not be read, rather than the host
      silently looking unknown.

Restore it, make it read-only, then connect to a host **not** in it and accept
the key:

- [ ] A status line says the key was accepted for this session only.
- [ ] Connecting again prompts again, which is the honest consequence.

### 1.6 A rejected cipher list fails rather than falling back (S5)

Connection settings, SSH page, set **Cipher preference** to `nonsense-cipher`.

- [ ] The connection fails with a message quoting the list back.
- [ ] It does **not** connect using the defaults.

Clear the field and confirm it connects normally again.

---

## 2. Appearance and accessibility

### 2.1 Faint text on the three themes that were changed (U4)

Faint text was below WCAG AA on Violet Haze, Blood Cell and Brass Gaslight and
is now floored per theme. The maths is tested; how it looks is not.

Produce dim text: `printf '\033[2mfaint text\033[0m normal text\n'`

- [ ] On Violet Haze, Blood Cell and Brass Gaslight: faint is still visibly
      dimmer than normal, but readable.
- [ ] On Amber Nixie, Emerald CRT, Ice Cathode and Paper White: unchanged from
      before. These four were untouched deliberately.

**If faint and normal now look identical** on the three changed themes, the
floor is too aggressive and should be argued down with a lower target.

### 2.2 Keyboard-only navigation

Unplug the mouse, or just do not touch it. Open Connection settings.

- [ ] Tab reaches every control on a page.
- [ ] The focused control is visibly focused. The dialogs are owner-drawn and
      that is exactly where focus rectangles get lost.
- [ ] Ctrl+Tab or the page list moves between the 29 pages.
- [ ] Escape and Enter do the obvious things.

### 2.3 Screen reader

Turn on Narrator, focus the terminal.

- [ ] Expect **nothing**. The grid is GPU particles with no text layer and
      there is no UI Automation provider anywhere in the source.

This is a confirmation, not a test. It is here so the gap is seen rather than
assumed, and so someone decides whether it matters.

### 2.4 Readability on long output

`find / -type f 2>/dev/null | head -5000`

- [ ] Text stays legible while it scrolls, not just when it settles.
- [ ] If it does not, note which motion style and density: that is the useful
      half of the report.

---

## 3. First run

Rename `%LOCALAPPDATA%\AmberSSH` aside, then launch.

- [ ] Count the steps from launch to a live session. Write the number down.
- [ ] Nothing dangerous is on by default.
- [ ] Reduced motion follows the Windows animation setting without being
      asked for.

Put the folder back afterwards.

---

## 4. Release

### 4.1 Signature on a machine that is not this one

The development certificate is self-signed, so this is about confirming what
a stranger sees.

- [ ] Right-click `AmberSSH.exe`, Properties, Digital Signatures: the signer
      reads `AmberSSH Development` and there is a timestamp.
- [ ] On another machine, SmartScreen warns and the publisher shows as
      unknown. That is correct for a self-signed certificate and is the
      argument for buying a CA-issued one.

### 4.2 The old certificate

`CN=JulieRoseManorDevelopment` is still installed in four stores including
`LocalMachine\TrustedPeople`, which trusts it for every user on this machine.
The commands to remove it are in the conversation; uninstall the
`Julie.RoseManor` package first.

- [ ] Decided: removed, or deliberately kept.

---

## What is already verified, and does not need you

Listed so this checklist is not longer than it has to be.

- The crash handler. Confirmed by building a binary with a deliberate null
  write, running it, and reading the report.
- Every pure gate: filename checks, path containment, the host key decision,
  the download planner against a hostile tree, contrast maths, paste escape
  stripping, terminal-aware masking. 504 test cases.
- The parsers under fuzzing: 810,000 mutated inputs across six seeds, and
  360,000 of those under AddressSanitizer.
- Both build configurations, and packaging end to end.
