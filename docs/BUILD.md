# Bauen & Flashen

## Voraussetzungen

- **Arduino IDE 2.x** oder **arduino-cli**
- **ESP32 Arduino Core 3.3.x** (Boardverwalter-URL von Espressif)
- Libraries: **`ESP32MQTTClient`** (über den Library-Manager) — Ethernet läuft über den
  nativen `ETH.h`-Treiber (im Core enthalten), MQTT über esp-mqtt (Core) via ESP32MQTTClient.
  `Update`, `Preferences`, `SPI`, `ESPmDNS` kommen mit dem Core.
- Zum Erzeugen des UI-Assets: `gzip` + `xxd` (auf macOS/Linux vorhanden), `bash`.

## Schritt 1 — Web-UI ins Image einbetten

Die Oberfläche lebt als eine Datei `ui/index.html`. Ein kleines Script komprimiert sie
(gzip) und schreibt sie als C-Header `fan_controller/ui_asset.h`, der in die Firmware
einkompiliert wird:

```sh
tools/build_ui.sh
# -> "ui_asset.h erzeugt: NNNN Bytes gzip"
```

Diesen Schritt nur wiederholen, wenn du `ui/index.html` änderst. Der erzeugte Header
ist im Repo eingecheckt, ein frisches Klonen kann also auch direkt kompilieren.

## Schritt 2 — Firmware kompilieren

### Mit arduino-cli
```sh
arduino-cli compile \
  --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc \
  --output-dir dist \
  fan_controller
# Ergebnis: dist/fan_controller.ino.bin
```

### Mit der Arduino IDE
Sketch `fan_controller/fan_controller.ino` öffnen und im **Tools**-Menü einstellen
(Details in [HARDWARE.md](HARDWARE.md)):

| Einstellung | Wert |
|---|---|
| Board | ESP32S3 Dev Module |
| USB CDC On Boot | **Enabled** |
| **PSRAM** | **Disabled** (Pflicht — sonst sterben Tacho-Pins, siehe HARDWARE.md) |
| Flash Size | 16MB (128Mb) |
| Partition Scheme | ein OTA-fähiges (zwei App-Slots) |

Das Image ist ~0,48 MB groß und passt locker in einen 1,25-MB-App-Slot.

## Schritt 3 — Aufs Gerät flashen (Netzwerk-OTA)

Das Gerät ist headless am LAN (kein USB nötig). Geflasht wird per HTTP-POST an die
laufende Firmware:

```sh
curl -X POST --data-binary @dist/fan_controller.ino.bin \
  -H 'Content-Type: application/octet-stream' \
  http://<geräte-ip>/ota
```

Alternativ über die Web-UI: Tab **Firmware** → `.bin` auswählen → Upload & Flash.

> ### ⚠️ Anti-Brick: das 90-Sekunden-Health-Window
> Nach dem Flash bootet das neue Image in den Zustand `PENDING_VERIFY`. Läuft es **90 s
> am Stück gesund**, markiert es sich selbst als gültig. Crasht oder hängt es vorher
> (Watchdog), rollt der Bootloader **automatisch** auf die vorherige Firmware zurück.
> Deshalb:
> - In den ersten ~90 s nach dem Flash das Gerät **nicht stromlos machen / power-cyclen**.
> - Ein *bewusster* Reboot oder ein zweites OTA aus dem laufenden Image ist ok — es markiert
>   sich vorher selbst gültig.
> - Ein fehlgeschlagener *Upload* kann nicht bricken: die alte App läuft bis zum Reboot weiter.

## Schritt 4 — Testkette (ohne Hardware, Pflicht vor jedem Release)

Die komplette gerätelose Prüfung läuft über **einen** Befehl — dieselbe Kette fährt die CI
bei jedem Push und Pull-Request:

```sh
bash tools/run_tests.sh     # Host-Tests + Syntax der Oberfläche und der Werkzeuge
bash tools/test_mock.sh     # Verhaltenstests gegen den Geräteersatz
```

Was dabei geprüft wird:

| Prüfung | Warum |
|---|---|
| Host-Tests mit **jedem** gefundenen Compiler, `-Wall -Wextra -Werror`, ASan + UBSan | clang und gcc warnen unterschiedlich; die Logik rechnet mit rohen Puffern und Zeit-Arithmetik über den 32-bit-Wrap |
| `node --check` auf das inline-JavaScript aus `ui/index.html` | `build_ui.sh` gzippt die Datei zu einem Byte-Array — der Compiler sieht nur noch Zahlen, ein JS-Syntaxfehler übersetzt **fehlerfrei** und würde geflasht |
| Syntax der Python- und Shell-Werkzeuge | sie laufen sonst erst im Release-Moment das erste Mal |
| `ui_asset.h` == `ui/index.html` | sonst läuft die geflashte Oberfläche der Quelle hinterher |
| 63 Verhaltenstests gegen `tools/mock_api.py` | halten fest, dass der Geräteersatz **dieselben** Routen, Prüfungen und Statuscodes hat wie die Firmware |

Die Extraktion des JavaScripts (`tools/extract_ui_js.py`) ist **zeilentreu**: eine Fehlermeldung
von `node` zeigt auf die Zeile in `ui/index.html`, nicht auf eine Position in einem Extrakt.

## UI ohne Gerät entwickeln

Ein Mock-Server liefert `ui/index.html` plus Fake-`/api/*`-Antworten, sodass die
Oberfläche im Browser ohne ESP getestet werden kann:

```sh
python3 tools/mock_api.py        # -> http://127.0.0.1:8077
```

Der Mock ist ein **vollständiger Geräteersatz**: dieselben Routen (und nur die), dieselbe
Eingabeprüfung, dieselben Fehlertexte und Statuscodes, dieselbe CSRF-Prüfung. Das ist Absicht —
ein Ersatz, der weniger verlangt als das Original, verschiebt Fehler genau dorthin, wo sie am
teuersten sind: auf ein Gerät ohne USB-Rückweg. `tools/test_mock.sh` hält das fest.
