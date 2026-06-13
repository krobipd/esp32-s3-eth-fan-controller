# Flash-Checkliste v4.0 (NUR mit ausdrücklicher Freigabe von krobi!)

> Gerät: Waveshare ESP32-S3-ETH @ **10.47.88.239** · PoE, **kein USB**. Bricking inakzeptabel.
> Der Erst-Flash läuft per HTTP-POST `/ota` auf die ALTE Firmware — die hat **kein**
> Health-Window, dieser eine Schritt ist ungeschützt. Image-Init (SPI/W5500/PCNT/LEDC/NVS)
> ist 1:1 aus dem laufenden v3.3-Code übernommen.

## Vorher
- [ ] ioBroker: neue, flache Topics vorbereitet — `<prefix>/<deviceId>/<fan>/speed` (Ist %),
      `…/set` (Befehl %), `…/rpm`, `…/status` (online/offline). Altes Schema wird abgelöst.
- [ ] Frischer Build:
      `tools/build_ui.sh && "<arduino-cli>" compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc --output-dir /tmp/out-v4 fan_controller_v4.0`
      → EXIT 0, Image < 1,25 MB (App-Slot). **PSRAM bleibt disabled** (CLAUDE.md §4).
- [ ] Host-Tests grün (`c++ -std=c++17 tests/host/test_logic.cpp -o /tmp/t && /tmp/t` → `OK`).
- [ ] **Freigabe von krobi liegt vor.**

## Flash (Risiko-Fenster!)
```sh
curl -X POST --data-binary @/tmp/out-v4/fan_controller_v4.0.ino.bin \
  -H 'Content-Type: application/octet-stream' http://10.47.88.239/ota
```

## Nachher — Reihenfolge einhalten
- [ ] **In den ersten ~90 s NICHT power-cyclen / stromlos machen.** Ein unkommandierter
      Reset (PoE-/Power-Blip, Brownout) im Health-Window rollt auf die alte FW zurück.
      (Ein *bewusster* Reboot via UI/zweites OTA ist ok — `commitIfPending` markiert vorher valid.)
- [ ] Boot < 10 s, `ping 10.47.88.239` ok, `http://10.47.88.239/` lädt das neue Control-Room-UI.
- [ ] `/api/status` prüfen: `wdt:true`, nach 90 s `ota_pending:false`, `crash_streak:0`,
      `largest_block` plausibel (~190k+).
- [ ] Alle 4 Lüfter drehen mit den restaurierten Duties, RPM plausibel.
- [ ] MQTT verbunden; ioBroker auf neue Topics umgestellt; `…/set`-Roundtrip wirkt.
- [ ] UI-Funktionstest: Slider, Lüfter bearbeiten (Name/Pins/**Invert bleibt erhalten!**),
      Kalibrieren, MQTT speichern, Logs im System-Tab.

## OTA-im-Fenster-Roundtrip (Build-unabhängig, muss am Gerät geprüft werden)
- [ ] Ein zweites (gültiges) OTA **innerhalb** der ersten 90 s nach dem ersten Flash
      durchführen → muss sauber durchlaufen und ins neue Image booten (testet, dass
      `commitIfPending` vor `set_boot_partition` greift).

## ROLLBACK-TEST (Spec §6.4 — die Anti-Brick-Garantie real beweisen)
- [ ] Ein **absichtlich defektes** Test-Image flashen, das **autonom und früh** crasht
      (z.B. `abort()` am Anfang von `setup()` oder in den ersten Loops) — **NICHT** über
      einen Pfad, der `commitIfPending` erreicht (kein Reboot/OTA-Handler-Aufruf, kein
      Erreichen der 90 s). Erwartung: WDT/Panic → Reboot → Bootloader rollt automatisch
      auf v4.0 zurück. Gerät kommt mit v4.0 wieder hoch.
- [ ] Falls der Rollback NICHT feuert: dokumentieren! Dann hat der Bootloader des Geräts
      evtl. kein `APP_ROLLBACK_ENABLE` aktiv → Health-Window ist wirkungslos, Updates
      bleiben „Hoffnung + Test". (Core 3.3.8 hat es per sdkconfig; am Gerät bestätigen.)

## 24-h-Beobachtung
- [ ] `largest_block` über 24 h: darf **nicht monoton fallen** (Heap-Fragmentierung).
- [ ] `boot_count` stabil (keine unerwarteten Reboots), `reset_reason` = SW/POWERON.
