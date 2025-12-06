/**
  * Version: DFPlayerMini_BLE_C3_DualMode
  * Modified for ESP32-C3 support (Pin changes & Serial1)
**/

#include <Arduino.h>
#include "DFRobotDFPlayerMini.h"

// ==========================================
//      CONFIGURACIÓN DE MODO
// ==========================================
// DESCOMENTA para simular en Wokwi
// COMENTA para usar en ESP32-C3 físico con BLE real
// #define WOKWI_SIMULATION 

// ==========================================
//        DEFINICIONES Y LIBRERÍAS
// ==========================================

DFRobotDFPlayerMini myDFPlayer;

// --- CAMBIOS PARA ESP32-C3 ---
// El ESP32-C3 tiene menos pines. 16 y 17 NO existen.
// Usaremos UART1. 
// Pines sugeridos para C3: RX=20, TX=21 (Estándar) o RX=4, TX=5 (Común en dev boards)
// Verifica el pinout de tu placa específica (SuperMini, Xiao, DevKitM)

#define RX_PIN 20   // Conectar al TX del DFPlayer
#define TX_PIN 21   // Conectar al RX del DFPlayer
#define BUSY_PIN 3  // GPIO 19 no existe en C3, usamos GPIO 3

// Creamos una instancia manual de HardwareSerial para el puerto 1
HardwareSerial mySoftwareSerial(1); 

// Variables globales
int currentVolume = 15;
int currentEQ = 0;
int currentDevice = 2; // 1-USB, 2-SD, 3-Sleep, 4-Serial
int currentTrack = 0; 
int currentFolder = 1;
int isLoopActivated = -1;
bool busyActivated = false;
bool next_song = true;
bool play_state = false;
unsigned long previousMillis = 0;  
const long interval = 500;  

// --- BLOQUE BLE (Solo se compila si NO estamos en simulación) ---
#ifndef WOKWI_SIMULATION
  #include <BLEDevice.h>
  #include <BLEServer.h>
  #include <BLEUtils.h>
  #include <BLE2902.h>

  BLEServer* pServer = NULL;
  BLECharacteristic* pCharacteristic = NULL;
  bool deviceConnected = false;
  bool oldDeviceConnected = false;

  // UUIDs para el servicio UART-like
  #define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E" 
  #define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
  #define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

  class MyServerCallbacks: public BLEServerCallbacks {
      void onConnect(BLEServer* pServer) {
        deviceConnected = true;
      };
      void onDisconnect(BLEServer* pServer) {
        deviceConnected = false;
      }
  };
#endif

// Prototipos de funciones
String processCommand(String cmd);
String ClearString(String input);
int WaitForModuleResponse(uint8_t option);
void sendResponse(String msg);

// ==========================================
//       LÓGICA DE COMUNICACIÓN (DUAL)
// ==========================================

// Función centralizada para enviar respuestas (Serial o BLE)
void sendResponse(String msg) {
  if (msg == "" || msg == NULL) return;

  #ifdef WOKWI_SIMULATION
    Serial.print("[TX Simulado]: ");
    Serial.println(msg);
  #else
    if (deviceConnected) {
      // BLE necesita array de chars, no String directo
      pCharacteristic->setValue(msg.c_str());
      pCharacteristic->notify();
    }
  #endif
}

#ifndef WOKWI_SIMULATION
// Callback para recibir datos por BLE
class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pChar) {
      String rxValue = pChar->getValue(); 

      if (rxValue.length() > 0) {
        String cmd = ClearString(rxValue);
        Serial.print("BLE RX: "); Serial.println(cmd); // Debug en puerto serie
        
        // Procesar y responder
        String respuesta = processCommand(cmd);
        sendResponse(respuesta);
      }
    }
};
#endif

// ==========================================
//                  SETUP
// ==========================================
void setup()
{
  // El C3 usa USB CDC para Serial, asegúrate de tener "USB CDC On Boot" activado en IDE
  Serial.begin(115200);
  
  // --- CAMBIO CLAVE PARA C3 ---
  // Inicializamos UART1 en los pines definidos para el C3
  mySoftwareSerial.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);

  pinMode(BUSY_PIN, INPUT_PULLUP);
  
  Serial.println();
  Serial.println(F("DFRobot DFPlayer Mini - ESP32-C3 Version"));
  Serial.println(F("Initializing DFPlayer ..."));

  // Pasamos 'mySoftwareSerial' en lugar de 'Serial2'
  if (!myDFPlayer.begin(mySoftwareSerial, /*isACK = */true, /*doReset = */true)) {
    Serial.println(F("Unable to begin DFPlayer: Check connections/SD card"));
  } else {
    Serial.println(F("DFPlayer Mini online."));
  }
  
  myDFPlayer.setTimeOut(500);
  myDFPlayer.volume(15); 
  myDFPlayer.EQ(DFPLAYER_EQ_NORMAL);
  myDFPlayer.outputDevice(DFPLAYER_DEVICE_SD);
  myDFPlayer.enableLoop();

  // --- INICIALIZACIÓN DE COMUNICACIÓN ---
  #ifdef WOKWI_SIMULATION
    Serial.println("--- MODO SIMULACIÓN WOKWI ACTIVADO ---");
    Serial.println("Escribe comandos (ej: 'pp1') en el monitor serial.");
  #else
    // Configuración BLE
    BLEDevice::init("ESP32_C3_MP3"); // Nombre cambiado para identificar C3
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    BLEService *pService = pServer->createService(SERVICE_UUID);

    // Característica de NOTIFICACIÓN (TX - Enviar al cel)
    pCharacteristic = pService->createCharacteristic(
                            CHARACTERISTIC_UUID_TX,
                            BLECharacteristic::PROPERTY_NOTIFY
                          );
    pCharacteristic->addDescriptor(new BLE2902());

    // Característica de ESCRITURA (RX - Recibir del cel)
    BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
                                             CHARACTERISTIC_UUID_RX,
                                             BLECharacteristic::PROPERTY_WRITE
                                           );
    pRxCharacteristic->setCallbacks(new MyCallbacks());

    pService->start();
    
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(false);
    pAdvertising->setMinPreferred(0x0);
    BLEDevice::startAdvertising();
    Serial.println("--- MODO BLE REAL: Esperando conexión... ---");
  #endif
}

// ==========================================
//                  LOOP
// ==========================================
void loop()
{
  // 1. Lógica automática (Siguiente canción)
  // CAMBIO: Usamos BUSY_PIN
  busyActivated = digitalRead(BUSY_PIN);
  unsigned long currentMillis = millis();     
  if (currentMillis - previousMillis >= interval) 
  {
      previousMillis = currentMillis;
      // Nota: El pin BUSY es LOW cuando está tocando, HIGH cuando termina
      if(busyActivated == HIGH && next_song && play_state)
      {
        String autoResponse = processCommand("pn");
        sendResponse(autoResponse);
        delay(10);
      }
  }

  // 2. Lógica de Recepción de Comandos
  #ifdef WOKWI_SIMULATION
    if (Serial.available()) {
      String cmd = Serial.readStringUntil('\n');
      cmd.trim(); 
      cmd = ClearString(cmd);
      
      if (cmd.length() > 0) {
        String respuesta = processCommand(cmd);
        sendResponse(respuesta);
      }
    }
  #else
    if (!deviceConnected && oldDeviceConnected) {
        delay(500); 
        pServer->startAdvertising(); 
        Serial.println("BLE Desconectado. Reiniciando advertising...");
        oldDeviceConnected = deviceConnected;
    }
    if (deviceConnected && !oldDeviceConnected) {
        oldDeviceConnected = deviceConnected;
        Serial.println("BLE Conectado.");
    }
  #endif
}

// ==========================================
//          PROCESAMIENTO DE COMANDOS
// ==========================================

String processCommand(String cmd) 
{
  cmd.trim();
  // Comandos de Reproducción
  if (cmd.startsWith("pp")) // PLAY
  {
    Serial.println("CMD: PLAY");
    play_state = true;
    if (cmd.length() > 2) {
       currentTrack = cmd.substring(2).toInt();
       myDFPlayer.play(currentTrack);
       return "pp" + String(currentTrack);
    } else {
       myDFPlayer.start(); 
       return "pp";
    }
  } 
  else if (cmd.startsWith("px")) // PAUSE
  {
    play_state = false;
    myDFPlayer.pause();
    return "px";
  } 
  else if (cmd.startsWith("py")) // Resume
  {
    play_state = true;
    myDFPlayer.start();
    return "py";
  }
  else if (cmd.startsWith("ps")) // STOP
  {
    play_state = false;
    myDFPlayer.stop();
    return "ps";
  } 
  else if (cmd.startsWith("pn")) // NEXT
  {
    myDFPlayer.next();
    currentTrack += 1; 
    return "pn" + (String)currentTrack;
  } 
  else if (cmd.startsWith("pb")) // PREVIOUS
  { 
    myDFPlayer.previous();
    if (currentTrack > 0) currentTrack -= 1;
    return "pb" + (String)currentTrack;
  } 
  // Equalización
  else if (cmd.startsWith("e")) // EQUALIZER
  {
    currentEQ = cmd.substring(1).toInt();
    myDFPlayer.EQ(currentEQ);
    return "e" + String(currentEQ);
  } 
  // Volumen
  else if (cmd.startsWith("v")) // VOLUME
  {
    currentVolume = cmd.substring(1).toInt();
    myDFPlayer.volume(currentVolume);
    return "v" + String(currentVolume);
  } 
  // Configuración
  else if (cmd.startsWith("d")) // CURRENT DEVICE
  {
    currentDevice = cmd.substring(1).toInt();
    myDFPlayer.outputDevice(currentDevice);
    return "d" + String(currentDevice);  
  }
  // Consultas de Estado 
  else if (cmd.startsWith("r")) // READ
  {
    int type = cmd.substring(1).toInt();
    int response = WaitForModuleResponse(type);
    return "r" + String(type) + String(response);
  }  
  // Opciones
  else if (cmd.startsWith("o")) // OPTIONS
  {
    int subCmd = cmd.substring(1).toInt();
    switch (subCmd)
    {
      case 0: myDFPlayer.enableDAC(); return "o0";
      case 1: myDFPlayer.disableDAC(); return "o1";
      case 2: myDFPlayer.sleep(); return "o2";
      case 3: myDFPlayer.reset(); return "o3";
      default: return "error_o";
    }    
  } 
  // Loop
  else if (cmd.startsWith("l")) // LOOP
  {
    isLoopActivated = cmd.substring(1).toInt();
    if (isLoopActivated == 0) myDFPlayer.enableLoop();
    else if (isLoopActivated == 1) myDFPlayer.disableLoop();
    else if (isLoopActivated == 2) myDFPlayer.enableLoopAll();
    else if (isLoopActivated == 3) myDFPlayer.disableLoopAll();
    return "l" + (String)isLoopActivated;  
  }
  // Random
  else if (cmd.startsWith("u")) 
  {
    myDFPlayer.randomAll();
    return "u";
  }
  
  return ""; 
}

String ClearString(String input) {
  String output = "";
  for (int i = 0; i < input.length(); i++) {
    char c = input.charAt(i);
    if (c >= 32 && c <= 126) { 
      output += c;
    }
  }
  return output;
}

int WaitForModuleResponse(uint8_t option)
{ 
  int counter = 0;
  int response = -1;
  while (response == -1 && counter < 5)
  {
    delay(100);
    switch (option) 
    {
      case 0: response = myDFPlayer.readState(); break;
      case 1: response = myDFPlayer.readVolume(); break;
      case 2: response = myDFPlayer.readEQ(); break;
      case 3: response = myDFPlayer.readFileCounts(); break; 
      case 4: response = myDFPlayer.readCurrentFileNumber(); break; 
      case 5: response = myDFPlayer.readFileCountsInFolder(currentFolder); break;
    }
    counter++;
  }
  return response;
}
