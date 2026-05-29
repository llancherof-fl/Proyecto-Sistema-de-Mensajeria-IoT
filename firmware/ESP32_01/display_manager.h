#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_SDA       11
#define OLED_SCL       12

#define ZONA_TX_Y   0
#define ZONA_SEP_Y 21
#define ZONA_RX_Y  24

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

static String _txActual = "";
static String _rxRemite = "";
static String _rxTexto  = "";
static bool   _oledOk   = false;

// Dirty flag: displaySetTX/RX solo marcan _dirty=true (microsegundos).
// displayFlush() en el loop() es quien hace la transferencia I2C real,
// máximo una vez cada REDRAW_MS. El teclado nunca espera al display.
static bool          _dirty    = false;
const  unsigned long REDRAW_MS = 80;  // ~12 fps, imperceptible para humanos

void setupDisplay() {
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);  // Fast mode: reduce la transferencia de ~25ms a ~13ms
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("[OLED] Error: pantalla no encontrada"));
    _oledOk = false;
    return;
  }
  _oledOk = true;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.display();
  Serial.println(F("[OLED] OK (I2C 400kHz)"));
}

void _buildFrame() {
  display.clearDisplay();

  // Zona TX
  display.setTextSize(1);
  display.setCursor(0, ZONA_TX_Y);
  display.print(F("Tu: "));
  String txShow = _txActual;
  if (txShow.length() > 18) txShow = txShow.substring(txShow.length() - 18);
  display.print(txShow);
  display.print('|');

  // Separador
  display.drawLine(0, ZONA_SEP_Y, SCREEN_WIDTH - 1, ZONA_SEP_Y, SSD1306_WHITE);

  // Zona RX
  display.setCursor(0, ZONA_RX_Y);
  if (_rxTexto.length() == 0) {
    display.print(F("Esperando msg..."));
  } else {
    display.print(_rxRemite.substring(0, 10));
    display.print(F(": "));
    display.setCursor(0, ZONA_RX_Y + 9);
    String rxTrunc = _rxTexto;
    if (rxTrunc.length() > 80) rxTrunc = rxTrunc.substring(0, 77) + "...";
    display.print(rxTrunc);
  }
}

// Llamar desde loop() — único punto donde se hace I2C real
void displayFlush() {
  if (!_oledOk || !_dirty) return;
  static unsigned long _tFlush = 0;
  if (millis() - _tFlush < REDRAW_MS) return;
  _tFlush = millis();
  _buildFrame();
  display.display();
  _dirty = false;
}

// Solo marca dirty — no bloquea
void displaySetTX(const String& texto) {
  if (texto == _txActual) return;
  _txActual = texto;
  _dirty    = true;
}

// Solo marca dirty — no bloquea
void displaySetRX(const String& remitente, const String& texto) {
  _rxRemite = remitente;
  _rxTexto  = texto;
  _dirty    = true;
}

// Estado de feedback (envio/error) — bloquea brevemente pero ocurre
// solo cuando el usuario acaba de presionar D, no durante escritura
void displayEstado(const String& linea1, const String& linea2 = "") {
  if (!_oledOk) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 20);
  display.println(linea1);
  if (linea2.length() > 0) {
    display.setCursor(0, 32);
    display.println(linea2);
  }
  display.display();
  _dirty = true;  // Forzar redibujado de la vista normal al volver
}

#endif // DISPLAY_MANAGER_H
