#pragma once
#include <ETH.h>
#include <Network.h>
#include <ESPmDNS.h>
#include <SPI.h>

// Stabiler Name: Geraet ist immer als http://fan-controller.local/ erreichbar,
// egal welche DHCP-IP es bekommt (mDNS funktioniert NUR ueber den nativen ETH.h-Stack,
// nicht ueber die alte Ethernet.h). Bei mehreren Geraeten im selben LAN hier eindeutig machen.
#define ETH_HOSTNAME "fan-controller"

// Board-Pins (Waveshare ESP32-S3-ETH, W5500) — identisch zu v4.0
#define ETH_SPI_SCK   13
#define ETH_SPI_MISO  12
#define ETH_SPI_MOSI  11
#define ETH_PIN_CS    14
#define ETH_PIN_INT   10
#define ETH_PIN_RST    9
#define ETH_SPI_MHZ   20

// Liveness-Flags, vom async Event-Callback gesetzt.
// ⚠️ §E2: auf Dateiebene static — korrekt nur, solange der Sketch EINE Uebersetzungseinheit
// ist. Bei einer Aufteilung auf mehrere .cpp bekaeme jede ihre eigene Kopie und die
// Netz-Zustaende liefen auseinander. Einzelheiten: concurrency.h, gleicher Hinweis.
static volatile bool g_ethLinkUp = false;
static volatile bool g_ethHasIp  = false;

// §E3: Logger als eigener Header — vorher wurde logFmt() hier vorwaerts deklariert und aus
// dem Hauptsketch erwartet; das Netz-Modul haing also am Sketch statt umgekehrt.
#include "fan_log.h"
#define NETLOGI(t, m) logFmt('I', t, m)
#define NETLOGW(t, m) logFmt('W', t, m)
#define NETLOGE(t, m) logFmt('E', t, m)

static void onEthEvent(arduino_event_id_t event, arduino_event_info_t info) {
  (void)info;
  switch (event) {
    case ARDUINO_EVENT_ETH_START:        NETLOGI("ETH", "Treiber gestartet"); break;
    case ARDUINO_EVENT_ETH_CONNECTED:    g_ethLinkUp = true;  NETLOGI("ETH", "Verbindung steht"); break;
    case ARDUINO_EVENT_ETH_GOT_IP:       g_ethHasIp = true;   NETLOGI("ETH", "IP-Adresse bezogen"); break;
    case ARDUINO_EVENT_ETH_LOST_IP:      g_ethHasIp = false;  NETLOGW("ETH", "IP-Adresse verloren"); break;
    case ARDUINO_EVENT_ETH_DISCONNECTED: g_ethLinkUp = false; g_ethHasIp = false; NETLOGW("ETH", "Verbindung weg"); break;
    default: break;
  }
}

// §F7: Event-Callback EINMALIG registrieren (in setup(), NICHT in ethBegin) — sonst haengt
// jeder ethHardReset() einen weiteren Handler an Network.onEvent an -> Liste waechst.
static void ethRegisterEvents() {
  Network.onEvent(onEthEvent);
}

// Startet SPI + nativen W5500-Treiber. Rückgabe true bei erfolgreichem ETH.begin.
// Overload ETH.h Z.169: (type, phy_addr, cs, irq, rst, SPIClass&, freq_mhz)
static bool ethBegin() {
  ETH.setHostname(ETH_HOSTNAME);   // VOR begin: meldet den Namen auch beim DHCP an
  SPI.begin(ETH_SPI_SCK, ETH_SPI_MISO, ETH_SPI_MOSI, ETH_PIN_CS);
  return ETH.begin(ETH_PHY_W5500, 1, ETH_PIN_CS, ETH_PIN_INT, ETH_PIN_RST, SPI, ETH_SPI_MHZ);
}

// mDNS-Responder starten (einmal je IP-Lease). Macht <ETH_HOSTNAME>.local erreichbar.
static void ethStartMdns() {
  MDNS.end();
  if (MDNS.begin(ETH_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    NETLOGI("MDNS", ETH_HOSTNAME ".local");
  } else {
    NETLOGW("MDNS", "Start fehlgeschlagen");
  }
}

// Harter Treiber-Neustart bei verklemmtem Link (>15 s down). Events setzen die Flags neu.
static void ethHardReset() {
  ETH.end();
  delay(50);
  ethBegin();
}

static inline bool   ethLinkUp()  { return g_ethLinkUp; }
static inline bool   ethHasIp()   { return g_ethHasIp; }
static inline String ethLocalIp() { return ETH.localIP().toString(); }
