# Building the manual

`manual.html` is the source. `render_hero.ps1` draws the cover banner
(`manual_hero.png`) by sampling `assets/logo.png` and a rendered wordmark into
particles, the way the app draws glyphs. Headless Edge prints the PDF:

```powershell
.\render_hero.ps1
& "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe" --headless=new --disable-gpu `
  --no-pdf-header-footer --print-to-pdf="..\AmberSSH-User-Manual.pdf" "file:///$PWD\manual.html"
```

Typefaces: Bahnschrift, Georgia and Cascadia Mono, all shipped with Windows 11.
