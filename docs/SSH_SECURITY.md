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
