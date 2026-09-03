# Bundled font licences

These faces are redistributed with AmberSSH and loaded from `exe\fonts` into a
private DirectWrite collection. Each is under a licence that permits
redistribution as part of a larger work, and each requires that its licence
accompany the font — which is what this file is for.

None of these licences is copyleft over AmberSSH itself. The SIL Open Font
Licence reaches only the font files and anything derived from them.

| Font | Copyright | Licence |
|---|---|---|
| **JetBrains Mono** | JetBrains s.r.o. | SIL Open Font License 1.1 |
| **Fira Code** | The Fira Code Project Authors | SIL Open Font License 1.1 |
| **Hack** | Source Foundry Authors; Bitstream Inc.; Tavmjong Bah | Hack Open Font License (MIT-style) + Bitstream Vera License |
| **IBM Plex Mono** | IBM Corp. | SIL Open Font License 1.1 |
| **Source Code Pro** | Adobe Systems Incorporated | SIL Open Font License 1.1 |
| **Ubuntu Mono** | Canonical Ltd. | Ubuntu Font Licence 1.0 |
| **Space Mono** | The Space Mono Project Authors | SIL Open Font License 1.1 |
| **Share Tech Mono** | The Share Tech Mono Project Authors | SIL Open Font License 1.1 |
| **Syne Mono** | The Syne Mono Project Authors | SIL Open Font License 1.1 |
| **Orbitron** | The Orbitron Project Authors | SIL Open Font License 1.1 |
| **Michroma** | The Michroma Project Authors | SIL Open Font License 1.1 |

## Obligations these place on AmberSSH

All of them are met by shipping this file alongside the fonts:

- **The licence travels with the font.** This file does that; it is deployed to
  `exe\fonts` with them.
- **The fonts are not sold on their own.** They are a component of AmberSSH,
  not the product.
- **The reserved font names are not used for modified versions.** AmberSSH does
  not modify any of these files; it loads them as shipped.

## Full licence texts

**SIL Open Font License 1.1** — <https://openfontlicense.org>
The canonical text also ships inside each OFL font's `OFL.txt` where the
upstream project provides one.

**Ubuntu Font Licence 1.0** — <https://ubuntu.com/legal/font-licence>

**Hack** — the Hack Open Font License (an MIT-style permissive licence) for the
Hack-specific work, and the Bitstream Vera License for the portions derived
from Bitstream Vera Sans Mono. Both permit redistribution with attribution.
See <https://github.com/source-foundry/Hack/blob/master/LICENSE.md>.

## Fonts AmberSSH does *not* ship

Faces offered in the font picker that are not in this directory are either
already installed on the system (Cascadia Mono, Consolas) or fetched on demand
from their upstream project when the user selects them. Nothing downloaded that
way is redistributed by AmberSSH.
