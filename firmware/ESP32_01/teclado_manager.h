#ifndef TECLADO_MANAGER_H
#define TECLADO_MANAGER_H

#include <Keypad.h>

// ─────────────────────────────────────────────────────────────────────────────
// Mapa de teclas 4x4
// ─────────────────────────────────────────────────────────────────────────────
const byte ROWS = 4;
const byte COLS = 4;

char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

byte rowPins[ROWS] = {15, 6, 5, 14}; // R1, R2, R3, R4
byte colPins[COLS] = {16, 7, 8, 9}; // C1, C2, C3, C4

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ─────────────────────────────────────────────────────────────────────────────
// Tabla multi-tap estilo T9
// Índice según tecla:
//   '1'→0  '2'→1  '3'→2  '4'→3  '5'→4  '6'→5
//   '7'→6  '8'→7  '9'→8  '*'→9  '0'→10 '#'→11
// ─────────────────────────────────────────────────────────────────────────────
const char* layout[] = {
  ".,?!1",    // 1
  "ABCabc2",  // 2
  "DEFdef3",  // 3
  "GHIghi4",  // 4
  "JKLjkl5",  // 5
  "MNOmno6",  // 6
  "PQRSpqrs7",// 7
  "TUVtuv8",  // 8
  "WXYZwxyz9",// 9
  "*_-+",    // * → espacio y símbolos
  "0",        // 0 → solo cero
  "#/=$"         // # → solo #  (pero será capturado antes como "borrar todo")
};

// ─────────────────────────────────────────────────────────────────────────────
// Estado multi-tap
// ─────────────────────────────────────────────────────────────────────────────
static String  _mensajeEnCurso  = "";   // Texto acumulado confirmado
static char    _letraEnCurso    = 0;    // Carácter siendo ciclado ahora
static int     _indiceCiclo     = 0;    // Índice dentro del layout de esa tecla
static char    _ultimaTecla     = NO_KEY;
static unsigned long _tUltima   = 0;
const  unsigned long MULTITAP_MS = 900; // ms para confirmar el carácter

// Callback que el .ino debe implementar para notificar cambios al display
// Se declara como puntero de función para no depender de display_manager aquí
typedef void (*OnTextoChange)(const String&);
typedef void (*OnMensajeEnviado)(const String&);

static OnTextoChange    _cbTexto   = nullptr;
static OnMensajeEnviado _cbEnviar  = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
void tecladoSetCallbacks(OnTextoChange cbTexto, OnMensajeEnviado cbEnviar) {
  _cbTexto  = cbTexto;
  _cbEnviar = cbEnviar;
}

// ─────────────────────────────────────────────────────────────────────────────
// Retorna el índice en layout[] para una tecla dada
int _teclaAIndice(char t) {
  if (t >= '1' && t <= '9') return t - '1';       // 0..8
  if (t == '*') return 9;
  if (t == '0') return 10;
  if (t == '#') return 11;
  return -1;  // A, B, C, D u otras → se manejan aparte
}

// ─────────────────────────────────────────────────────────────────────────────
// Confirmar el carácter en ciclo actual y añadirlo al mensaje
void _confirmarLetra() {
  if (_letraEnCurso != 0) {
    _mensajeEnCurso += _letraEnCurso;
    _letraEnCurso = 0;
    _indiceCiclo  = 0;
    _ultimaTecla  = NO_KEY;
    if (_cbTexto) _cbTexto(_mensajeEnCurso);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Procesar una tecla leída del keypad (llamar desde loop())
// Retorna true si se debe enviar el mensaje (tecla D pulsada)
bool tecladoProcesar() {
  unsigned long ahora = millis();

  // Confirmar letra si pasó el timeout sin nueva pulsación
  if (_letraEnCurso != 0 && (ahora - _tUltima) > MULTITAP_MS) {
    _confirmarLetra();
  }

  char tecla = keypad.getKey();
  if (tecla == NO_KEY) return false;

  // ── Tecla D: ENVIAR ───────────────────────────────────────────────────────
  if (tecla == 'D') {
    _confirmarLetra();  // Confirmar cualquier letra pendiente
    if (_mensajeEnCurso.length() > 0) {
      String paraEnviar = _mensajeEnCurso;
      _mensajeEnCurso = "";
      _letraEnCurso   = 0;
      if (_cbTexto)   _cbTexto(_mensajeEnCurso);
      if (_cbEnviar)  _cbEnviar(paraEnviar);
    }
    return true;
  }

// ── Tecla A: BACKSPACE (Borrar último carácter) ──────────────────────────
  if (tecla == 'A') {
    if (_letraEnCurso != 0) {
      // Si hay una letra en ciclo (parpadeando), la cancelamos
      _letraEnCurso = 0;
      _indiceCiclo  = 0;
      _ultimaTecla  = NO_KEY;
    } else if (_mensajeEnCurso.length() > 0) {
      // Si no hay letra en ciclo, borramos el último del mensaje confirmado
      _mensajeEnCurso.remove(_mensajeEnCurso.length() - 1);
    }
    if (_cbTexto) _cbTexto(_mensajeEnCurso);
    return false;
  }

  // ── Tecla B: ESPACIO ──────────────────────────────────────────────────────
  if (tecla == 'B') {
    _confirmarLetra(); // Primero fijamos lo que se estaba escribiendo
    _mensajeEnCurso += " "; // Añadimos el espacio
    if (_cbTexto) _cbTexto(_mensajeEnCurso);
    return false;
  }

  // ── Tecla C: BORRAR TODO (Clear) ──────────────────────────────────────────
  if (tecla == 'C') {
    _mensajeEnCurso = "";
    _letraEnCurso   = 0;
    _indiceCiclo    = 0;
    _ultimaTecla    = NO_KEY;
    if (_cbTexto) _cbTexto(_mensajeEnCurso);
    return false;
  }

  // ── Teclas 0-9, * y #: multi-tap ─────────────────────────────────────────────────
  int idx = _teclaAIndice(tecla);
  if (idx < 0) return false;

  const char* opciones = layout[idx];
  int numOpciones = strlen(opciones);

  if (tecla == _ultimaTecla) {
    // Misma tecla → avanzar en el ciclo
    _indiceCiclo = (_indiceCiclo + 1) % numOpciones;
  } else {
    // Tecla diferente → confirmar la anterior y empezar nueva
    _confirmarLetra();
    _indiceCiclo = 0;
  }

  _letraEnCurso = opciones[_indiceCiclo];
  _ultimaTecla  = tecla;
  _tUltima      = ahora;

  // Mostrar preview (texto confirmado + letra en ciclo)
  if (_cbTexto) _cbTexto(_mensajeEnCurso + _letraEnCurso);

  return false;
}

#endif // TECLADO_MANAGER_H
