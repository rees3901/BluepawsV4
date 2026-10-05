"""Strict build-time hardware adapter; never edits the canonical source files."""
from pathlib import Path
Import("env")
root = Path(env.subst("$PROJECT_DIR"))
private = root / ".secrets"
if not (private / "hub_secrets.h").is_file():
    raise RuntimeError("Supply private hub_secrets.h before building")
env.Append(CPPPATH=[str(private)])

def replace_once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError("Upstream adapter contract changed: " + old[:70])
    return text.replace(old, new, 1)

canonical = (root / "../platformio/src/main.cpp").read_text(encoding="utf-8")
pins = (root / "../../diagnostics/t190-radio-monitor/include/pins.h").read_text()
pin_adapter = '#include "t190_pins.h"\n#define HUB_PINS_H\n'
for name in ("NSS", "SCK", "MOSI", "MISO", "RST", "BUSY", "DIO1"):
    pin_adapter += f"#define PIN_LORA_{name} SNIFFER_LORA_{name}\n"
pin_adapter += '#define PIN_LED SNIFFER_HEARTBEAT\n'
canonical = replace_once(canonical, '#include "hub_pins.h"', pin_adapter)
start = canonical.index("static void femInit() {")
end = canonical.index("static bool requestHubMode", start)
canonical = canonical[:start] + '''// T190 has no Tracker V2 KCT8103L front-end.
static void femInit() { Serial.println("[FEM] T190: no external FEM"); }
static void femSetRx() {}
static void femSetTx() {}

''' + canonical[end:]
presence = (root / "../platformio/include/hub_presence_impl.h").read_text(encoding="utf-8")
start = presence.index("    HardwareSerial uart(1);")
end = presence.index("    for (;;) {", start)
presence = presence[:start] + '''    Preferences prefs;
    prefs.begin("bp-hub-self", false);
    Serial.println("[HUB GNSS] T190: absent; hub position unavailable");
''' + presence[end:]
start = presence.index("        // Bound UART work")
end = presence.index("        // Persist settings", start)
presence = presence[:start] + presence[end:]
canonical = replace_once(canonical, '#include "hub_presence_impl.h"', '#include "t190_presence.inc"')
presence = replace_once(presence, '    hubSelfMutex = xSemaphoreCreateMutex();', '''    // User-configured fallback; real GNSS or a provisioned override takes precedence.
    hubSelf.lat = PERSONAL_HOME_LAT;
    hubSelf.lon = PERSONAL_HOME_LON;
    hubSelf.hasFix = true;
    hubSelfMutex = xSemaphoreCreateMutex();''')
presence = replace_once(presence, '        hubSelf.revision = p.getULong64("revision",0);', '''        hubSelf.lat = p.getDouble("homeLat", PERSONAL_HOME_LAT);
        hubSelf.lon = p.getDouble("homeLon", PERSONAL_HOME_LON);
        hubSelf.revision = p.getULong64("revision",0);''')
presence = replace_once(presence, '    uint32_t age=(millis()-s.fixMs)/1000;', '''    // No GNSS on T190: configured position is explicit in the existing contract.
    doc["position_simulated"] = true;
    if (!cloud) doc["position_source"] = "configured_home";
    uint32_t age=0;''')
ui = (root / "../personal-heltec/src/main.cpp").read_text(encoding="utf-8")
ui = replace_once(ui, '#include "../../platformio/src/main.cpp"', '#include "t190_canonical.inc"')
ui = replace_once(ui, '#include <Adafruit_ST7735.h>', '#include <Adafruit_ST7789.h>')
start = ui.index("// Heltec V2:")
end = ui.index("Button button;", start)
ui = ui[:start] + '''constexpr int Cs=SNIFFER_TFT_CS, Dc=SNIFFER_TFT_DC, Reset=SNIFFER_TFT_RST;
constexpr int Clock=SNIFFER_TFT_SCK, Mosi=SNIFFER_TFT_MOSI, Backlight=SNIFFER_TFT_BL;
constexpr int UserButton=SNIFFER_USER_BTN;
constexpr uint8_t PageCount=8;
SPIClass displaySPI(FSPI);
class TrackerDisplay : public Adafruit_ST7789 {
public:
    TrackerDisplay() : Adafruit_ST7789(&displaySPI,Cs,Dc,Reset) {}
    void beginTracker() { init(170,320); setRotation(1); setSPISpeed(16000000); }
} screen;
''' + ui[end:]
ui = replace_once(ui, '    displaySPI.begin(Clock,-1,Mosi,Cs);', '    pinMode(SNIFFER_TFT_POWER,OUTPUT); digitalWrite(SNIFFER_TFT_POWER,LOW);\n    displaySPI.begin(Clock,-1,Mosi,Cs);')
ui = replace_once(ui, 'No hub GPS fix yet', 'Hub GNSS not fitted')
ui = replace_once(ui, 'Needs a clear sky view', 'Collar GPS unaffected')
ui = replace_once(ui, 'Hub fix age: %s', 'Configured home: %s')
ui = replace_once(ui, 'GPS is hub\'s own position', 'Configured, not GNSS')
ui = replace_once(ui, '[DISPLAY] V2 ST7735 160x80 on FSPI; USER GPIO0: tap next, hold home', '[DISPLAY] T190 ST7789 320x170 on FSPI; USER GPIO21: tap next, hold home')
for name, text in [('t190_pins.h',pins),('t190_canonical.inc',canonical),('t190_presence.inc',presence),('t190_ui.inc',ui)]:
    (private/name).write_text(text,encoding="utf-8")
