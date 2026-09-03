# AmberSSH — Security Model

## Credentials

Passwords and key passphrases are **never written to disk by AmberSSH**.

- Remembered secrets go to the **Windows Credential Manager** via `CredWriteW`,
  under target names `AmberSSH/{profile-uuid}/password` and
  `AmberSSH/{profile-uuid}/key-passphrase`.
- `profiles.json` stores only *booleans* recording that a secret exists. A unit
  test (`ProfileStoreTests.cpp`, "the saved file contains no secret fields")
  asserts the serialized JSON contains no password, passphrase, or key material.
- Unremembered secrets live only in `amber::SecureString`, a heap buffer that is
  `SecureZeroMemory`'d on destruction and on every reassignment. `ScrubString`
  does the same for a `std::string` that has to cross an API boundary.
- On read-back, the buffer the Credential Manager hands us is zeroed before
  `CredFree`.
- Deleting a profile also erases both of its credential entries.

This narrows, but does not eliminate, secret exposure: a secret must exist in
plaintext in process memory to be handed to libssh2, and the OS may page that
memory. AmberSSH does not lock pages.

## Host keys

`SshSession` retrieves the server host key after the handshake and requires an
explicit user decision before proceeding on first contact.

**Current limitation, stated plainly:** the first-use prompt and the
changed-key path have **not been exercised against a live server** in this
build. Until they are, treat host-key verification as implemented-but-unproven.
The intended behaviour is:

- First contact: show host, port, key algorithm, SHA-256 fingerprint; offer
  accept-once / accept-and-save / cancel; default to cancel.
- Changed key: never continue silently; show expected and received
  fingerprints; explain that this may be a rebuilt server *or* an interception
  attempt; require a separate explicit action to replace the stored key.

### Host sigils

A fingerprint is 43 characters of base64 and nobody compares one properly. The
host-key box therefore draws a **sigil** derived from the key
(`src/security/HostSigil.h`): a small figure whose node count, filled/hollow
nodes, chords, corner cuts and border rhythm all come from the fingerprint.

- It is a **recognition aid, never the evidence.** The fingerprint text is shown
  next to it and remains the thing being verified.
- The same key always produces the same figure, on every machine and every
  build (FNV-1a plus splitmix64, chosen for being identical everywhere).
- Colour is never the only difference: any two hosts that share a hue differ in
  geometry, so the figure works in monochrome and for a colour-vision
  difference. `SigilDescribe` states the shape in words for a screen reader.
- The four-character mnemonic is a shorthand for saying aloud on a call.
  Mnemonics **do** collide — a test asserts it — so one must never be presented
  as proof of identity.

## Blast radius

Before anything leaves `App::SendToShell` the command is analysed by a
hand-written parser (`src/security/BlastRadius.h`) and, above the configured
policy threshold, confirmed.

- **No language model is consulted.** The answer must be the same every time
  and it must be explainable, so every rule is code.
- The parser never rewrites the command; the confirmation shows the exact bytes
  that will be sent.
- What it cannot read, it says: a pipeline is truncated at the first separator
  and reported as partly examined; substitution and unterminated quotes make the
  report explicitly incomplete; a finding resting on a variable or a glob is
  marked "unable to determine".
- Typed-hostname friction is reserved for Critical. Requiring typing for every
  warning is how people learn to type without reading.
- Without shell integration (OSC 133) the command is recovered from the screen
  and the prompt boundary is a guess. Several readings are produced and the
  worst is taken; the dialog says the text came from the screen.

## Privacy cloak

Ctrl+Shift+M masks likely secrets at draw time (`src/security/PrivacyCloak.h`).
The grid is never modified — search, selection and copy still see the real
characters — and the cover is applied where compose reads a cell.

The wording is fixed in one function, `CloakStatusText()`, and a test enforces
it:

> AmberSSH says "potential secrets are being masked".
> AmberSSH must **never** say "this session is safe to share".

Detection is pattern matching and will miss things. On a fixed grid the covered
run is as long as the secret; the copy path (`MaskLine`) uses a fixed-width
`[redacted]` marker, which does hide length. IP addresses and home-directory
names are opt-in, because masking them by default makes the terminal useless
for the work it is usually doing.

## Logging

The logger must never receive secret material. Permitted: profile UUID, host,
port, connection state, error codes, host-key algorithm and fingerprint,
renderer state. Forbidden: passwords, passphrases, private-key bytes,
authentication packets.

## Terminal input hardening

Terminal output is untrusted data from a remote host. The parser therefore:

- bounds its parameter array (16 entries) and clamps parameter values to 65535;
- bounds OSC payloads at 4096 bytes (`kMaxOscLen`) — a regression test covers a
  200 KB title attempt;
- recovers from malformed and truncated sequences without corrupting state;
- never executes an operating-system command from an escape sequence;
- decodes UTF-8 defensively, including sequences split across socket reads.

## Not implemented

- No SSH agent support.
- No `.ppk` support. The dialog says so explicitly rather than failing obscurely.
- No telemetry, no automatic log upload, no automatic host-key acceptance.
