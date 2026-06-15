# MQTT-Anbindung

Das Gerät verbindet sich als Client zu einem MQTT-Broker (Konfiguration im Web-UI unter
**Einstellungen → MQTT**). Die `deviceId` wird aus der MAC abgeleitet und sieht so aus:
`ws-s3eth-XXXXXX`.

## Topic-Schema

Flach: Geräte-Info unter `info/`, pro Lüfter drei Datenpunkte unter dem (klein geschriebenen) Lüfternamen:

```
<prefix>/<deviceId>/info/status        online / offline        (retained, Last-Will)
<prefix>/<deviceId>/info/version       Firmware-Version (z. B. 5.2.0)   (retained)
<prefix>/<deviceId>/info/ip            aktuelle IP-Adresse     (retained)
<prefix>/<deviceId>/<fan>/speed        Ist-Stellwert 0–100 %   (retained)
<prefix>/<deviceId>/<fan>/set          Befehl 0–100            (kein Retain)
<prefix>/<deviceId>/<fan>/rpm          Drehzahl
```

`status`, `sys` und `info` sind als Lüfternamen reserviert (kollidieren nicht mit `info/`).

Beispiel (prefix `esp`, Lüfter `nas`):

| Topic | Richtung | Wert |
|---|---|---|
| `esp/ws-s3eth-1A2B3C/info/status` | Gerät → Broker | `online` (retained), `offline` (Last-Will) |
| `esp/ws-s3eth-1A2B3C/info/version` | Gerät → Broker | z. B. `5.2.0` (retained) |
| `esp/ws-s3eth-1A2B3C/info/ip` | Gerät → Broker | z. B. `10.0.0.42` (retained) |
| `esp/ws-s3eth-1A2B3C/nas/speed` | Gerät → Broker | z. B. `58` (Ist-%, retained) |
| `esp/ws-s3eth-1A2B3C/nas/rpm` | Gerät → Broker | z. B. `864` |
| `esp/ws-s3eth-1A2B3C/nas/set` | Broker → Gerät | `0`–`100` (Sollwert in %) |

Wichtig:
- **Steuern** ausschließlich über `…/<fan>/set` (0–100). `…/speed` ist der **Ist-Wert** und
  wird vom Gerät retained gepublisht — schreibe NICHT auf `speed` (Ist und Soll auf einem
  Topic würde eine Rückkopplung erzeugen, deshalb der getrennte `set`-Datenpunkt).
- `set`-Payloads ohne Ziffer werden ignoriert (Schutz gegen Fehl-Publishes / geleerte
  retained Topics) — der Lüfter fällt also nicht versehentlich auf 0 %.
- Beim Umbenennen/Löschen eines Lüfters räumt das Gerät das alte retained `…/speed`-Topic auf.

## Beispiel: ioBroker

Der MQTT-Adapter (Broker- oder Client-Modus) legt eingehende Topics **automatisch** als
Objekte an — für `info/*` und `speed`/`rpm` ist also nichts zu konfigurieren. Aus
`esp/ws-s3eth-1A2B3C/nas/speed` wird das Objekt:

```
mqtt.0.esp.ws-s3eth-1A2B3C.nas.speed
```

Zum **Steuern** schreibst du den Sollwert (0–100) auf den `set`-Datenpunkt mit `ack:false`:

```
mqtt.0.esp.ws-s3eth-1A2B3C.nas.set  =  60   (ack:false)
```

Damit der Adapter diesen Wert auch wirklich an den Broker published, muss er für eigene
States auf Änderung publishen (Standard im Broker-Modus). Schnelltest, dass der Befehl
ankommt: das Gerät loggt jeden empfangenen Befehl (Web-UI → System → Log:
`MQTT: set nas -> 60%`).

> Wechselt man von einem älteren, anders strukturierten Schema, muss die eigene Logik
> (Skripte/Visualisierung) auf die neuen Objekt-IDs und die 0–100-Skala umgezogen werden —
> das ist Sache der jeweiligen Home-Automation, nicht der Firmware.
