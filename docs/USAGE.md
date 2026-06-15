# Bedienung & Konfiguration

Nach dem ersten Boot holt sich das Gerät per DHCP eine IP (Ethernet, kein WLAN). Die
IP steht im Serial-Log; alternativ im Router nachsehen (Hostname/MAC) oder per MQTT
(`<prefix>/<deviceId>/info/status` wird „online", `…/info/ip` zeigt die aktuelle IP).
Die laufende Firmware-Version steht oben in der Web-UI und unter `…/info/version`.

Die Weboberfläche erreichst du unter **`http://<geräte-ip>/`**.

## Die Weboberfläche

Vier Tabs:

### Dashboard
Live-Übersicht aller konfigurierten Lüfter: Name, aktuelle Drehzahl (RPM), ein
Schieberegler für die Leistung (0–100 %) und eine Status-LED. Der Regler wirkt sofort
(entprellt). Status-Anzeige: ETH / MQTT / WDT (Watchdog) und ein Safe-Mode-Banner, falls
aktiv.

### System
Gesundheit des Geräts für den Dauerbetrieb: freier/kleinster Heap und größter
zusammenhängender Block (Fragmentierung), Uptime, Boot-Zähler, letzter Reset-Grund,
Crash-Streak, Watchdog- und OTA-Status. Dazu ein Log-Viewer (aktueller + vorheriger
Boot) und Buttons für Reboot und Safe-Mode-Reset.

### Einstellungen
- **Lüfter** anlegen / bearbeiten / löschen. Pro Lüfter: Name, PWM-Pin, Tacho-Pin,
  „PWM invertieren". Belegte/ungeeignete Pins werden ausgeblendet.
- **Kalibrierung**: Mindest-Start in % (manche Lüfter laufen erst ab z. B. 20 % an) und
  eine Notiz.
- **MQTT**: aktiv, Host, Port, User, Passwort, Prefix.

### Firmware
OTA-Upload mit Fortschrittsanzeige und Reboot-Überwachung. Zeigt den Validierungs-Status
(`PENDING_VERIFY` während des 90-s-Health-Windows, danach „validiert").

## Einen Lüfter einrichten (Schritt für Schritt)

1. **Einstellungen → „+ Neuer Lüfter"** — legt einen leeren Slot an, der Editor öffnet sich.
2. **Name** vergeben (Kleinbuchstaben/Ziffern/`_`/`-`, max. 19 Zeichen; reserviert: `status`, `sys`).
   Der Name bildet das MQTT-Topic.
3. **PWM-Pin** und **Tacho-Pin** aus der Liste wählen (welche GPIOs erlaubt sind →
   [HARDWARE.md](HARDWARE.md), wie verdrahten → [WIRING.md](WIRING.md)).
4. **Speichern.** Der Lüfter erscheint im Dashboard.
5. Optional **Kalibrierung**: am Live-Regler austesten, ab welcher % der Lüfter zuverlässig
   anläuft, und diesen Wert als „Mindest-Start" eintragen.

Hinweise:
- **Invertieren** nur, wenn dein PWM-Pegel invertiert ist (z. B. über einen
  Pegelwandler-Transistor) — sonst läuft der Lüfter „andersherum" (100 % = Stopp).
- Drehzahl wird mit **2 Pulsen/Umdrehung** gerechnet (Standard bei PC-Lüftern). Zeigt ein
  Lüfter trotz Drehung keine RPM, stimmt meist die Tacho-Verdrahtung/der Pull-up nicht
  (→ WIRING.md).

## Robustheit im Dauerbetrieb (was das Gerät selbst tut)

- **Watchdog** (8 s): hängt der Controller, gibt es einen automatischen Reboot.
- **Safe-Mode**: nach 3 Crash-Boots in Folge laufen alle Lüfter auf festen **70 %**
  Failsafe, MQTT wird abgeschaltet, Web-UI/OTA bleiben für die Rettung erreichbar. Ein
  Banner weist darauf hin; „Safe-Mode-Reset" im System-Tab hebt ihn (nach Reboot) auf.
- **Duty-Wiederherstellung**: die zuletzt gesetzten Leistungen werden gespeichert und
  nach einem Reboot **vor** der Netz-Initialisierung wieder angelegt — die Lüfter drehen
  also sofort wieder, auch wenn das Netz noch fehlt.
- **Storm-Shield**: unplausibel viele Tacho-Pulse (Störung) pausieren die Messung
  kurzzeitig statt das Gerät zu belasten.

## Status-Codes (Fault) der Lüfter

| Code | Bedeutung |
|---|---|
| 0 | OK |
| 1 | Pin nicht erlaubt |
| 2 | Pin nicht interrupt-fähig (Tacho) |
| 3 | Lüfter dreht (Duty > 0), aber keine Tacho-Pulse → Verdrahtung prüfen |
| 4 | Puls-Sturm (Störung) → Messung pausiert, Cooldown |

Siehe auch: [MQTT.md](MQTT.md) für die Anbindung an ioBroker/Home-Automation,
[API.md](API.md) für die HTTP-Schnittstelle.
