/**
 * ESP32-S3 + SIM7670G — Chat Bidireccional LTE (VERSIÓN FINAL OPTIMIZADA)
 * ======================================================================
 * Optimización clave:
 * - Lectura SSE por BLOQUES (no byte a byte)
 * - Poll LTE relajado (120 ms)
 * - Yield al RTOS → teclado fluido
 * * ESTE CÓDIGO ES PARA: ESP32 #1
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
const int  PORT        = 8000;              // ← CAMBIADO DE 8080 A 8000
const char DEVICE_ID[] = "ESP32_02";        // ← ID DE ESTE ESP32
const char TARGET_ID[] = "ESP32_01";        // ← A QUIÉN LE ENVÍA MENSAJES

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
volatile int longitudTextoTeclado = 0; 
unsigned long ultimaPulsacionTeclado = 0; 
const unsigned long TIEMPO_ESPERA_TECLADO = 4000; // ← MODIFICADO: 4 segundos completos de espera tras teclear

//══════════════════════════════════════════════════════════════════
// LTE INIT (TU SECUENCIA ORIGINAL)
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
    String("GET /subscribe?device_id=") + DEVICE_ID + " HTTP/1.1\r\n" +  // ← AGREGADO device_id en query
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
  if(ahora-ultimoDatoRX>SILENCIO_SSE_MS){

    desconectarSSE();return;

    }

  if(!gsmClientRX.connected()){

    desconectarSSE();return;

  }


  int disponibles=gsmClientRX.available();
  if(disponibles<=0) return;

  uint8_t buffer[SSE_CHUNK];


  int leidos=gsmClientRX.read(buffer,min(disponibles,SSE_CHUNK));

  
  if(leidos<=0) return;

  ultimoDatoRX=ahora;

  for(int i=0;i<leidos;i++){
    char c=(char)buffer[i];
    sseBuffer+=c;
    if(c=='\n'){procesarLineaSSE(sseBuffer); sseBuffer="";}
  }
}

//══════════════════════════════════════════════════════════════════
// HTTP POST
//══════════════════════════════════════════════════════════════════

bool enviarHTTPPost(const String& payload) {
  // 1. Mostrar feedback inmediato al usuario
  displayEstado("Enviando...", "LTE Network");

  StaticJsonDocument<256> doc;
  doc["device_id"] = DEVICE_ID;
  doc["target_id"] = TARGET_ID;    // ← AGREGADO: A quién va dirigido
  doc["payload"] = payload;
  String body; 
  serializeJson(doc, body);

  // 2. Timeout agresivo: Si el servidor no responde en 5 seg, abortamos
  // Esto evita que el teclado se quede muerto si la red está lenta
  httpTX.setHttpResponseTimeout(500); 

  httpTX.beginRequest();
  httpTX.post("/publish");
  httpTX.sendHeader("Content-Type", "application/json");
  httpTX.sendHeader("Content-Length", body.length());
  httpTX.endRequest();
  httpTX.print(body);
  

  int statusCode = httpTX.responseStatusCode();
  String response = httpTX.responseBody();

  Serial.println("Enviado "+ body+"    Status code "+ statusCode);
  
  if (statusCode == 200 or statusCode == 202) {
    displayEstado("Enviado OK", "");
    Serial.println("Enviado");
    delay(200); // Pequeño feedback visual
    return true;
  } else {
    displayEstado("Error Envío", String(statusCode));
    Serial.println("Error envio"+response+" code"+statusCode);
    return false;
  }
}

//══════════════════════════════════════════════════════════════════
// CALLBACKS TECLADO
//══════════════════════════════════════════════════════════════════

void onTextoChange(const String& txt){ 
  longitudTextoTeclado = txt.length(); 
  ultimaPulsacionTeclado = millis(); // Guarda el instante exacto de la última tecla
  displaySetTX(txt); 
}

void onMensajeEnviado(const String& txt){
  if(!lteConectado){ intentarConexionLTE(); return; }
  enviarHTTPPost(txt);
  longitudTextoTeclado = 0; 
  displaySetTX("");
}

//══════════════════════════════════════════════════════════════════
// SETUP (TU ORIGINAL COMPLETO)
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

  modem.sendAT("AT"); modem.waitResponse();
  modem.sendAT("ATE0"); modem.waitResponse();
  modem.sendAT("AT+CPIN?"); modem.waitResponse();
  modem.sendAT("AT+CGDCONT=1,\"IP\",\""+String(APN)+"\"");
  modem.waitResponse();
  modem.sendAT("AT+CGATT=1"); modem.waitResponse(10000L);

  intentarConexionLTE();
  if(lteConectado) conectarSSE();

  displaySetRX("","");
  displaySetTX("");
}

//══════════════════════════════════════════════════════════════════
// LOOP OPTIMIZADO
//══════════════════════════════════════════════════════════════════

void loop(){
  tecladoProcesar();

  displayFlush();

  // --- PRIORIDAD 3: RED (CONTROLADA) ---
  static unsigned long cronometroRed = 0;
  unsigned long ahora = millis();
  
  // Condición estricta: Solo evalúa red si pasaron 5 segundos desde la última lectura
  // Y ADEMÁS, no hay texto en pantalla O ya pasaron más de 4 segundos completos de total silencio en el teclado.
  if ((ahora - cronometroRed > 5000) && 
      (longitudTextoTeclado == 0 || (ahora - ultimaPulsacionTeclado > TIEMPO_ESPERA_TECLADO))) {
    
    // ← NUEVO: Delay solicitado de 1.5s previo a la alerta visual para asegurar estabilidad en la pantalla
    delay(1500); 
    
    displayEstado("Recibiendo mensajes", ""); 
    unsigned long tInicio = millis();   
  
    leerSSE();                          
    
    unsigned long tFin = millis();      
    Serial.print("leerSSE tardo: ");
    Serial.print(tFin - tInicio);
    Serial.println(" ms");

    cronometroRed = millis();
    displayFlush();
  }

  // Gestión de reconexión silenciosa (También respeta estrictamente los 4 segundos de silencio del teclado)
  if (lteConectado && !sseConectado && 
      (longitudTextoTeclado == 0 || (millis() - ultimaPulsacionTeclado > TIEMPO_ESPERA_TECLADO))) {
    static unsigned long tRecon = 0;
    if (millis() - tRecon > 8000) { 
      tRecon = millis();
      conectarSSE();
    }
  }
}