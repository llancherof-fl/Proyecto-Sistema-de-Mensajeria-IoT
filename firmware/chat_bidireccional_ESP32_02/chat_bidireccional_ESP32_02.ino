/**
 * ESP32-S3 + SIM7670G — Chat Bidireccional LTE (VERSIÓN FINAL OPTIMIZADA)
 * ======================================================================
 * Modificaciones respecto a la versión en main:
 *   - Logging verbose de todos los comandos AT en Serial Monitor
 *   - Agregado AT+CPSI  (info completa del canal LTE: banda, EARFCN, RSRP, RSRQ, RSSNR)
 *   - Agregado AT+CNSMOD (modo de red actual: LTE, WCDMA, etc.)
 *   - Función logAT() para enviar cualquier comando AT y mostrar respuesta completa
 *   - Reporte periódico de señal cada 30 s en el loop()
 * 
 * ESTE CÓDIGO ES PARA: ESP32 #2
 */

#define TINY_GSM_MODEM_SIM7600
#define TINY_GSM_RX_BUFFER 2048

#include "display_manager.h"
#include "teclado_manager.h"
#include <TinyGsmClient.h>
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>

// ───── Pines SIM7670 ─────
#define SIM_RX 17
#define SIM_TX 18
#define SIM_DTR 45
#define MODEM_PWRKEY 4

// ───── Red ─────
const char APN[]       = "internet.comcel.com.co";
const char SERVER[]    = "13.58.175.26";
const int  PORT        = 8000;
const char DEVICE_ID[] = "ESP32_02";
const char TARGET_ID[] = "ESP32_01";

#define SSE_CHUNK 256
const unsigned long POLL_SSE_MS     = 100;
const unsigned long SILENCIO_SSE_MS = 45000;

// ───── TinyGSM ─────
HardwareSerial SerialAT(1);
TinyGsm modem(SerialAT);
TinyGsmClient gsmClientTX(modem, 0);
TinyGsmClient gsmClientRX(modem, 1);
HttpClient httpTX(gsmClientTX, SERVER, PORT);

// ───── Estado global ─────
bool lteConectado=false, sseConectado=false;
unsigned long ultimoDatoRX=0, ultimoCheckGPRS=0, ultimoPollSSE=0;
String sseBuffer="";

//══════════════════════════════════════════════════════════════════
// LOGGING DE COMANDOS AT
// Envía un comando AT, espera la respuesta y la imprime completa
// en el monitor serial con formato claro.
//══════════════════════════════════════════════════════════════════

String logAT(const String& cmd, unsigned long timeoutMs = 3000) {
  Serial.println();
  Serial.print(">>> ");
  Serial.println(cmd);

  // Vaciar buffer de recepción antes de enviar
  while (SerialAT.available()) SerialAT.read();

  SerialAT.println(cmd);

  String respuesta = "";
  unsigned long tInicio = millis();

  while (millis() - tInicio < timeoutMs) {
    while (SerialAT.available()) {
      char c = SerialAT.read();
      respuesta += c;
    }
    // Terminamos cuando aparece OK, ERROR o CME ERROR
    if (respuesta.indexOf("OK") != -1      ||
        respuesta.indexOf("ERROR") != -1   ||
        respuesta.indexOf("+CME ERROR") != -1) {
      break;
    }
    delay(10);
  }

  // Imprimir respuesta línea a línea con prefijo "<<< "
  respuesta.trim();
  if (respuesta.length() == 0) {
    Serial.println("<<< (sin respuesta)");
  } else {
    int inicio = 0;
    while (inicio < (int)respuesta.length()) {
      int fin = respuesta.indexOf('\n', inicio);
      if (fin == -1) fin = respuesta.length();
      String linea = respuesta.substring(inicio, fin);
      linea.trim();
      if (linea.length() > 0) {
        Serial.print("<<< ");
        Serial.println(linea);
      }
      inicio = fin + 1;
    }
  }

  return respuesta;
}

//══════════════════════════════════════════════════════════════════
// REPORTE DE SEÑAL LTE
// Ejecuta AT+CPSI y AT+CNSMOD y muestra un resumen interpretado
//══════════════════════════════════════════════════════════════════

void reporteSeñal() {
  Serial.println();
  Serial.println("========================================");
  Serial.println("  REPORTE DE SEÑAL LTE");
  Serial.println("========================================");

  // AT+CPSI — información completa del canal LTE
  // Respuesta esperada en LTE:
  // +CPSI: LTE,Online,<MCC>-<MNC>,<TAC>,<SCellID>,<PCellID>,
  //        <Freq Band>,<earfcn>,<dlbw>,<ulbw>,<RSRQ>,<RSRP>,<RSSI>,<RSSNR>
  String respCPSI = logAT("AT+CPSI?");

  // AT+CNSMOD — modo de red (0=sin servicio, 8=LTE, etc.)
  // Respuesta: +CNSMOD: <n>,<stat>
  String respCNSMOD = logAT("AT+CNSMOD?");

  // AT+CSQ — RSSI clásico (para comparar con el informe)
  String respCSQ = logAT("AT+CSQ");

  // ── Interpretación de CPSI ──────────────────────────────────
  // Buscamos la línea con el prefijo +CPSI:
  Serial.println();
  Serial.println("---- Interpretación AT+CPSI ----");

  int idxCPSI = respCPSI.indexOf("+CPSI:");
  if (idxCPSI != -1) {
    String lineaCPSI = respCPSI.substring(idxCPSI);
    int finLinea = lineaCPSI.indexOf('\n');
    if (finLinea != -1) lineaCPSI = lineaCPSI.substring(0, finLinea);
    lineaCPSI.trim();

    // Separar campos por coma (después de "+CPSI: ")
    String datos = lineaCPSI.substring(7); // quitar "+CPSI: "
    Serial.println("Campos: " + datos);

    // Parsear campo a campo
    // Formato LTE: System,Op,MCC-MNC,TAC,SCellID,PCellID,Band,earfcn,dlbw,ulbw,RSRQ,RSRP,RSSI,RSSNR
    int pos = 0;
    String campos[15];
    int numCampos = 0;
    while (pos < (int)datos.length() && numCampos < 15) {
      int coma = datos.indexOf(',', pos);
      if (coma == -1) coma = datos.length();
      campos[numCampos++] = datos.substring(pos, coma);
      pos = coma + 1;
    }

    if (numCampos >= 1) {
      Serial.print("  Modo del sistema : "); Serial.println(campos[0]);
    }
    if (numCampos >= 2) {
      Serial.print("  Estado operación : "); Serial.println(campos[1]);
    }
    if (numCampos >= 3) {
      Serial.print("  MCC-MNC (operador): "); Serial.println(campos[2]);
    }
    if (numCampos >= 7) {
      Serial.print("  Banda LTE        : "); Serial.println(campos[6]);
    }
    if (numCampos >= 8) {
      Serial.print("  EARFCN           : "); Serial.println(campos[7]);
    }
    if (numCampos >= 9) {
      Serial.print("  BW Downlink (dlbw): ");
      // dlbw: 0=1.4MHz,1=3MHz,2=5MHz,3=10MHz,4=15MHz,5=20MHz
      int dlbw = campos[8].toInt();
      String bwStr[] = {"1.4 MHz","3 MHz","5 MHz","10 MHz","15 MHz","20 MHz"};
      Serial.println((dlbw >= 0 && dlbw <= 5) ? bwStr[dlbw] : campos[8] + " (código)");
    }
    if (numCampos >= 10) {
      Serial.print("  BW Uplink  (ulbw): ");
      int ulbw = campos[9].toInt();
      String bwStr[] = {"1.4 MHz","3 MHz","5 MHz","10 MHz","15 MHz","20 MHz"};
      Serial.println((ulbw >= 0 && ulbw <= 5) ? bwStr[ulbw] : campos[9] + " (código)");
    }
    if (numCampos >= 11) {
      Serial.print("  RSRQ (dB)        : "); Serial.println(campos[10]);
    }
    if (numCampos >= 12) {
      Serial.print("  RSRP (dBm)       : "); Serial.println(campos[11]);
      // Interpretación de calidad de señal LTE según RSRP
      int rsrp = campos[11].toInt();
      Serial.print("    → Calidad RSRP : ");
      if      (rsrp >= -80)              Serial.println("Excelente");
      else if (rsrp >= -90)              Serial.println("Buena");
      else if (rsrp >= -100)             Serial.println("Moderada");
      else if (rsrp >= -110)             Serial.println("Débil");
      else                               Serial.println("Muy débil / borde cobertura");
    }
    if (numCampos >= 13) {
      Serial.print("  RSSI (dBm)       : "); Serial.println(campos[12]);
    }
    if (numCampos >= 14) {
      Serial.print("  RSSNR (dB)       : "); Serial.println(campos[13]);
      // Estimación de modulación probable según RSSNR
      int rssnr = campos[13].toInt();
      Serial.print("    → Modulación probable (estimada por SNR): ");
      if      (rssnr >= 22) Serial.println("64-QAM (6 bits/símbolo)");
      else if (rssnr >= 15) Serial.println("16-QAM (4 bits/símbolo)");
      else if (rssnr >= 7)  Serial.println("QPSK   (2 bits/símbolo)");
      else                  Serial.println("BPSK / QPSK (señal débil)");
      Serial.println("    (La red decide la modulación real — esta es una estimación)");
    }
  } else {
    Serial.println("  No se encontró respuesta +CPSI (módulo no en LTE?)");
  }

  // ── Interpretación de CNSMOD ────────────────────────────────
  Serial.println();
  Serial.println("---- Interpretación AT+CNSMOD ----");
  int idxCNSMOD = respCNSMOD.indexOf("+CNSMOD:");
  if (idxCNSMOD != -1) {
    String lineaCNSMOD = respCNSMOD.substring(idxCNSMOD);
    int finLinea = lineaCNSMOD.indexOf('\n');
    if (finLinea != -1) lineaCNSMOD = lineaCNSMOD.substring(0, finLinea);
    lineaCNSMOD.trim();
    // Formato: +CNSMOD: <n>,<stat>
    int coma = lineaCNSMOD.lastIndexOf(',');
    if (coma != -1) {
      int stat = lineaCNSMOD.substring(coma + 1).toInt();
      String modos[] = {"Sin servicio","GSM","GPRS","EGPRS/EDGE","WCDMA",
                        "HSDPA","HSUPA","HSPA","LTE"};
      Serial.print("  Modo de red (stat="); Serial.print(stat); Serial.print("): ");
      Serial.println((stat >= 0 && stat <= 8) ? modos[stat] : "Desconocido");
    }
  } else {
    Serial.println("  No se encontró respuesta +CNSMOD");
  }

  Serial.println("========================================");
  Serial.println();
}

//══════════════════════════════════════════════════════════════════
// LTE INIT
//══════════════════════════════════════════════════════════════════

void intentarConexionLTE() {
  lteConectado=false;
  displayEstado("Buscando red...", "LTE SIM7670G");

  if (!modem.waitForNetwork(30000L)) {
    displayEstado("Sin señal LTE", "Revisa SIM");
    return;
  }

  if (!modem.gprsConnect(APN,"","")) {
    displayEstado("Error GPRS", APN);
    return;
  }

  lteConectado=true;
  httpTX.setHttpResponseTimeout(15000);
  httpTX.connectionKeepAlive();

  displayEstado("LTE Conectado!", "");
  delay(800);
}

//══════════════════════════════════════════════════════════════════
// SSE
//══════════════════════════════════════════════════════════════════

void conectarSSE() {
  sseConectado=false;
  sseBuffer="";
  if (!gsmClientRX.connect(SERVER, PORT)) return;

  gsmClientRX.print(
    String("GET /subscribe?device_id=") + DEVICE_ID + " HTTP/1.1\r\n" +
    "Host: " + SERVER + "\r\n"
    "Accept: text/event-stream\r\n"
    "Connection: keep-alive\r\n\r\n"
  );

  while(gsmClientRX.available()) {
    if(gsmClientRX.read()=='\n') break;
  }
  sseConectado=true;
  ultimoDatoRX=millis();
}

void desconectarSSE(){ gsmClientRX.stop(); sseConectado=false; }

void procesarLineaSSE(String& linea){
  linea.trim();
  if(!linea.startsWith("data: ")) return;

  StaticJsonDocument<256> doc;
  if(deserializeJson(doc,linea.substring(6))) return;
  if(doc["device_id"]==DEVICE_ID) return;

  displaySetRX(doc["device_id"],doc["payload"]);
}

void leerSSE(){
  if(!sseConectado) return;

  unsigned long ahora=millis();
  if(ahora-ultimoDatoRX>SILENCIO_SSE_MS){ desconectarSSE(); return; }
  if(!gsmClientRX.connected()){ desconectarSSE(); return; }

  int disponibles=gsmClientRX.available();
  if(disponibles<=0) return;

  uint8_t buffer[SSE_CHUNK];
  int leidos=gsmClientRX.read(buffer,min(disponibles,SSE_CHUNK));
  if(leidos<=0) return;

  ultimoDatoRX=ahora;

  for(int i=0;i<leidos;i++){
    char c=(char)buffer[i];
    sseBuffer+=c;
    if(c=='\n'){ procesarLineaSSE(sseBuffer); sseBuffer=""; }
  }
}

//══════════════════════════════════════════════════════════════════
// HTTP POST
//══════════════════════════════════════════════════════════════════

bool enviarHTTPPost(const String& payload) {
  displayEstado("Enviando...", "LTE Network");

  StaticJsonDocument<256> doc;
  doc["device_id"] = DEVICE_ID;
  doc["target_id"] = TARGET_ID;
  doc["payload"]   = payload;
  String body;
  serializeJson(doc, body);

  httpTX.setHttpResponseTimeout(500);
  httpTX.beginRequest();
  httpTX.post("/publish");
  httpTX.sendHeader("Content-Type", "application/json");
  httpTX.sendHeader("Content-Length", body.length());
  httpTX.endRequest();
  httpTX.print(body);

  int statusCode = httpTX.responseStatusCode();
  String response = httpTX.responseBody();

  Serial.println("Enviado: " + body + "  |  Status: " + statusCode);

  if (statusCode == 200 || statusCode == 202) {
    displayEstado("Enviado OK", "");
    delay(200);
    return true;
  } else {
    displayEstado("Error Envío", String(statusCode));
    Serial.println("Error envío: " + response + " code " + statusCode);
    return false;
  }
}

//══════════════════════════════════════════════════════════════════
// CALLBACKS TECLADO
//══════════════════════════════════════════════════════════════════

void onTextoChange(const String& txt){ displaySetTX(txt); }

void onMensajeEnviado(const String& txt){
  if(!lteConectado){ intentarConexionLTE(); return; }
  enviarHTTPPost(txt);
  displaySetTX("");
}

//══════════════════════════════════════════════════════════════════
// SETUP
//══════════════════════════════════════════════════════════════════

void setup(){
  Serial.begin(115200);
  setupDisplay();
  displayEstado("Iniciando...","SIM7670G");

  tecladoSetCallbacks(onTextoChange,onMensajeEnviado);

  pinMode(SIM_DTR,OUTPUT);
  digitalWrite(SIM_DTR,LOW);

  SerialAT.begin(115200,SERIAL_8N1,SIM_RX,SIM_TX);
  delay(5000);

  // ── Secuencia AT con logging verbose ──────────────────────────
  Serial.println();
  Serial.println("========================================");
  Serial.println("  INICIALIZACIÓN SIM7670G — AT Commands");
  Serial.println("========================================");

  logAT("AT");                                          // test comunicación
  logAT("ATE0");                                        // echo off
  logAT("AT+CPIN?");                                    // estado SIM
  logAT("AT+CSQ");                                      // señal inicial
  logAT("AT+CREG?");                                    // registro red circuitos
  logAT("AT+CGREG?");                                   // registro red paquetes
  logAT("AT+COPS?");                                    // operador y tecnología (sufijo 7 = LTE)
  logAT("AT+CNSMOD?");                                  // modo red (8 = LTE)
  logAT("AT+CGDCONT=1,\"IP\",\"" + String(APN) + "\""); // configurar APN
  logAT("AT+CGATT=1", 10000);                           // activar adjunto GPRS
  logAT("AT+CGACT=1,1", 10000);                         // activar contexto de datos
  logAT("AT+CGPADDR");                                  // verificar IP asignada

  Serial.println();
  Serial.println("========================================");
  Serial.println("  INFORMACIÓN DEL CANAL LTE (post-conexión)");
  Serial.println("========================================");

  // Primer reporte de señal completo al arranque
  reporteSeñal();

  // ── Conexión LTE y SSE ────────────────────────────────────────
  intentarConexionLTE();
  if(lteConectado) conectarSSE();

  displaySetRX("","");
  displaySetTX("");
}

//══════════════════════════════════════════════════════════════════
// LOOP
//══════════════════════════════════════════════════════════════════

void loop(){
  tecladoProcesar();
  displayFlush();

  // --- Lectura SSE cada 5 s ---
  static unsigned long cronometroRed = 0;
  if (millis() - cronometroRed > 5000) {
    displayEstado("Leyendo mensajes", "");
    unsigned long tInicio = millis();

    leerSSE();

    unsigned long tFin = millis();
    Serial.print("leerSSE tardó: ");
    Serial.print(tFin - tInicio);
    Serial.println(" ms");

    cronometroRed = millis();
    displayFlush();
  }

  // --- Reporte de señal LTE cada 30 s ---
  static unsigned long cronometroSenal = 0;
  if (millis() - cronometroSenal > 30000) {
    cronometroSenal = millis();
    reporteSeñal();
  }

  // --- Reconexión SSE silenciosa ---
  if (lteConectado && !sseConectado) {
    static unsigned long tRecon = 0;
    if (millis() - tRecon > 8000) {
      tRecon = millis();
      conectarSSE();
    }
  }
}
