# Elektron Net – Windows Release erstellen

Gedankenstütze für den Release-Prozess via GitHub Actions.

---

## Voraussetzungen

- `CMakeLists.txt` enthält die aktuelle Versionsnummer
- `.github/workflows/build.yaml` ist im Repo
- `share/setup.nsi.in` ist im Repo

---

## Versionsnummer prüfen / setzen

In `CMakeLists.txt` ganz oben:

```cmake
set(CLIENT_NAME "Elektron Net")
set(CLIENT_VERSION_MAJOR 4)
set(CLIENT_VERSION_MINOR 0)
set(CLIENT_VERSION_BUILD 1)
set(CLIENT_VERSION_RC 0)
set(CLIENT_VERSION_IS_RELEASE "true")
set(COPYRIGHT_YEAR "2026")
```

**Für einen normalen Release:**
- `CLIENT_VERSION_IS_RELEASE` → `"true"`
- `CLIENT_VERSION_RC` → `0`
- Ergibt Dateiname: `elektron-net-windows-v4.0.1_setup.exe`

**Für einen Release Candidate:**
- `CLIENT_VERSION_IS_RELEASE` → `"false"`
- `CLIENT_VERSION_RC` → z.B. `2`
- Ergibt Dateiname: `elektron-net-windows-v4.0.1-rc2_setup.exe`
- Wird auf GitHub automatisch als **Pre-Release** markiert

---

## Release erstellen – mit GitHub Desktop

### Schritt 1 – Commit in GitHub Desktop

1. GitHub Desktop öffnen
2. Links alle geänderten Dateien prüfen (Haken setzen)
3. Unten links:
   - **Summary:** z.B. `Release v4.0.1`
   - Button **"Commit to main"** klicken

### Schritt 2 – Push in GitHub Desktop

- Oben rechts **"Push origin"** klicken

### Schritt 3 – Tag setzen via Git Bash

In GitHub Desktop: Menü **Repository → Open in Git Bash**

```bash
git tag v4.0.1
git push origin v4.0.1
```

> ⚠️ Tag-Name muss mit `v` beginnen, sonst startet der Workflow nicht!

### Schritt 4 – Build verfolgen

Auf `github.com/Kutlusoy/elektron-net`:
- Tab **"Actions"** → laufenden Workflow beobachten
- Dauer: ~5 Min. (mit Cache) / ~60 Min. (erster Build ohne Cache)

### Schritt 5 – Release prüfen

- Tab **"Releases"** → `Elektron Net v4.0.1`
- `.exe` steht als Download-Asset bereit

---

## Release erstellen – nur mit Git (Command Line)

```bash
# 1. Dateien stagen und committen
git add .
git commit -m "Release v4.0.1"

# 2. Push zu GitHub
git push origin main

# 3. Tag setzen und pushen
git tag v4.0.1
git push origin v4.0.1
```

Oder Schritt 2 und 3 kombiniert:
```bash
git push origin main --tags
```

---

## Schnellreferenz

| Aktion | GitHub Desktop | Git Bash |
|---|---|---|
| Dateien committen | ✅ | `git commit -m "..."` |
| Push zu main | ✅ | `git push origin main` |
| Tag erstellen | ❌ nicht möglich | `git tag v4.0.1` |
| Tag pushen | ❌ nicht möglich | `git push origin v4.0.1` |

---

## Troubleshooting

**Workflow startet nicht:**
- Tag beginnt nicht mit `v` → `git tag v4.0.1` (nicht `4.0.1`)
- Tag wurde nicht gepusht → `git push origin v4.0.1`

**Build schlägt fehl bei "Extract version":**
- Variablennamen in `CMakeLists.txt` prüfen:
  `CLIENT_VERSION_MAJOR`, `CLIENT_VERSION_MINOR`, `CLIENT_VERSION_BUILD`

**Kein `.exe` im Release:**
- Unter Actions → den fehlgeschlagenen Job aufklappen
- Step **"Rename installer"** zeigt was gefunden wurde
- Step **"Build NSIS installer"** zeigt ob NSIS-Fehler vorlagen

**Tag löschen und neu setzen (falls Fehler):**
```bash
# Lokal löschen
git tag -d v4.0.1
# Auf GitHub löschen
git push origin --delete v4.0.1
# Neu setzen und pushen
git tag v4.0.1
git push origin v4.0.1
```

---

## Dateistruktur

```
.github/
├── workflows/
│   └── build.yaml          ← GitHub Actions Workflow
└── RELEASE_HOWTO.md        ← diese Datei

share/
└── setup.nsi.in            ← NSIS Installer-Skript

CMakeLists.txt              ← Versionsnummern hier pflegen
```
