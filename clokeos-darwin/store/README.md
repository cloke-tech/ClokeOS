# Cloke Store

App Store für ClokeOS. Aufbau und Bedienung folgen dem Apple App Store (Entdecken, Kategorien, Suche,
Detailseite, „Laden“-Knopf mit Fortschrittsring, Updates), die Optik der ClokeOS-Oberfläche (Violett, Papercut-Schichten, Liquid-Glass-Leisten).

Standardquelle ist [cloke-tech/ClokeOS-Apps](https://github.com/cloke-tech/ClokeOS-Apps). Weitere Quellen lassen sich unter **Quellen** hinzufügen.

## Inhalt

```
ClokeStore.app/                  .app-Bundle im Aufbau von macOS, kommt nach /Applications oder ~/Applications
  Contents/Info.plist            com.clokeos.store
  Contents/MacOS/ClokeStore      Startskript: ruft clokestore.py auf
  Contents/Resources/index.html  die ganze Oberfläche (eine Datei, kein Build)
  Contents/Resources/clokestore.py  lokaler Dienst (nur Python-Standardbibliothek)
  Contents/Resources/AppIcon.svg
tests/test_clokestore.py         33 Tests für Dienst, Archiv-Sicherheit und Zugriffsschutz
tests/test_ui.py                 20 Ende-zu-Ende-Tests der Oberfläche (Playwright)
```

## Starten

In ClokeOS: `ClokeStore.app` nach `/Applications` kopieren und aus Launchpad starten. Zum Ausprobieren auf jedem Rechner:

```
python3 ClokeStore.app/Contents/Resources/clokestore.py
```

Der Dienst öffnet `http://127.0.0.1:8737/` im Browser und beendet sich fünf Minuten nach dem Schließen der Seite.
Öffnet man `index.html` direkt, ohne Dienst, zeigt der Store nur den Katalog; die Schaltfläche „Laden“ zeigt dann den Befehl zum Kopieren.

## Quellen

Eine Quelle ist eine `index.json` im Format von ClokeOS-Apps (`"format": 1`). Hinzufügen geht mit

- `nutzer/repo` oder einer GitHub-Adresse (der Store sucht `index.json` auf `main`, dann `master`),
- der Adresse einer `index.json` (nur https; http nur für 127.0.0.1 und localhost).

Pflichtfelder je Programm: `id`, `name`, `package.url` und `package.sha256`. Alles Weitere ist optional und wird in der Oberfläche gezeigt,
wenn die Quelle es liefert: `name_de`, `summary`, `summary_de`, `description`, `description_de`, `developer`, `icon`, `screenshots`, `homepage`, `license`, `category`, `version`, `recommends`.
Die Standardquelle kennt diese Zusatzfelder noch nicht; für ihre 13 Einträge bringt der Store deutsche und englische Beschreibungen selbst mit.

Der Store merkt sich Quellen und Sprache im Browser (`localStorage`), den Katalog als Zwischenspeicher. Ist die Quelle nicht erreichbar, erscheint der letzte Stand,
bei der Standardquelle zuletzt eine eingebaute Kopie von `index.json` Version 2.8.

## Wie installiert wird

| Quelle | Vorgang |
|---|---|
| Standardquelle | `cloke-app install <kennung>` (aus ClokeOS-Apps, `tools/cloke-app`) |
| Eigene Quelle | Paket laden, SHA-256 prüfen, sicher entpacken, `.app` nach `~/Applications` legen |

Systemprogramme und Pakete (etwa `webkit`) kommen nur aus der Standardquelle. Programme aus eigenen Quellen landen immer in `~/Applications`.
Der Store entfernt nur, was er selbst nach `~/Applications` gelegt hat. Sein Protokoll liegt unter `~/Library/Application Support/ClokeStore/installed.json`.

## Sicherheit

- Der Dienst hört nur auf 127.0.0.1, prüft den Host-Header (gegen DNS-Rebinding) und die Herkunft, und verlangt ein Token, das nur in der ausgelieferten Seite steht.
- Die Seite hat eine strenge Content-Security-Policy: Skripte laufen nur mit dem Hash der eigenen Datei.
- Archive werden vor dem Entpacken geprüft: keine absoluten Pfade, kein `..`, keine Geräte oder Hardlinks, keine Links aus dem Ordner heraus; setuid-Bits fallen weg; Größenlimits.
- Die Prüfsumme schützt vor beschädigten Downloads, **nicht** vor einer bösartigen Quelle. Eine Quelle bestimmt Paket und Prüfsumme selbst. Der Dialog weist darauf hin.

## Tests

```
python3 tests/test_clokestore.py
python3 tests/test_ui.py [/pfad/zu/chrome]
```

## Noch offen

- Ungetestet gegen das echte GitHub: In der Entwicklungsumgebung war nur ein Zugriff auf die Seiten, nicht auf `raw.githubusercontent.com` möglich. Geprüft ist alles gegen eine lokale Quelle mit gleichem Format.
- Ungetestet in ClokeOS selbst (VM): Start über `ClokeStore.app`, `cloke-app`. Letzteres ist laut ClokeOS-Apps selbst noch nicht in einer VM getestet, ebenso die Release-Dateien `apps-2.8`.
- Auf der Standardquelle gibt es weder Symbole noch Vorschaubilder; der Store zeichnet Symbole selbst. Sobald die Quelle `icon` und `screenshots` liefert, erscheinen sie.
- Signaturen: wie in ClokeOS-Apps bisher nur SHA-256.
