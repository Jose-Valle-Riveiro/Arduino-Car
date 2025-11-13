/*
 Robot seguidor con CheapStepper, SD y 3 modos (grabar/reproducir/manual)
 Pines y mapeo:
  - SD CS -> D4 (SPI hardware D11/D12/D13)
  - Sensores: Left D2, Right D3
  - Motor Izquierdo (físicamente "reflejado"): IN1 D5, IN2 D6, IN3 D7, IN4 D8
  - Motor Derecho: IN1 D9, IN2 D10, IN3 A0, IN4 A1
*/

#include <SPI.h>
#include <SD.h>
#include <CheapStepper.h>

// ---------- PINS ----------
const int SD_CS = 4;
const int S_LEFT_PIN  = 2;
const int S_RIGHT_PIN = 3;
const int BUZZER_PIN = A2;

int melody[] = {
  660, 660, 0, 660, 0, 520, 660, 0, 770, 0, 380
};

int noteDurations[] = {
  100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100
};

int numNotes = sizeof(melody) / sizeof(melody[0]);

// Variables para control con millis()
unsigned long previousMillis = 0;
int noteIndex = 0;
bool playingMelody = false;
// ---------- CheapStepper ----------
CheapStepper motorDer(5, 6, 7, 8);   // motor izquierdo (reflejado)
CheapStepper motorIzq(9, 10, A0, A1); // motor derecho

// dirección lógica (no se usa para movimientos de corrección explícitos)
bool IzqClockwise = false;
bool DerClockwise = true;



// ---------- Archivos ----------
const char* CONFIG_FILE = "config.txt";
const char* ROUTE_FILE  = "route.txt";

// ---------- Timing / movimiento ----------
const int STEPS_PER_ROUTE_ENTRY = 1;    // pasos por "tick"
const unsigned int STEP_DELAY_US = 800; // microsegundos entre pasos
const unsigned long CORRECTION_STEP_LIMIT = 4096;

const int SENSOR_ACTIVE = HIGH; // cambia si tus TCRT devuelven HIGH en cinta

// ---------- Helpers SD / config ----------
bool initSD() {
  if (!SD.begin(SD_CS)) {
    Serial.println("ERROR: No se pudo inicializar la SD.");
    return false;
  }
  Serial.println("SD inicializada.");
  return true;
}

int readConfigMode() {
  if (!SD.exists(CONFIG_FILE)) {
    Serial.println("config.txt no existe. Usando modo 1 por defecto.");
    return 1;
  }
  File f = SD.open(CONFIG_FILE, FILE_READ);
  if (!f) return 1;
  String s = f.readStringUntil('\n');
  f.close();
  s.trim();
  int m = s.toInt();
  if (m < 1 || m > 3) m = 1;
  return m;
}

int leerSensoresEstado() {
  int readLeft = digitalRead(S_LEFT_PIN);
  int readRight = digitalRead(S_RIGHT_PIN);
  
  if (readLeft == SENSOR_ACTIVE && readRight == SENSOR_ACTIVE){
    return 3; //Recto
  }
  else if (readLeft != SENSOR_ACTIVE && readRight == SENSOR_ACTIVE){
    return 2; //Girar Derecha
  }
  else if (readLeft == SENSOR_ACTIVE && readRight != SENSOR_ACTIVE){
    return 1; //Girar Izquierda
  }
  else {
    return 0; //Ninguno se detecta
  }
}

// mueve x "ticks" sincronizado (1 paso por motor)
void avanzarTicks(unsigned long ticks) {
  for (unsigned long i = 0; i < ticks; i++){
    motorDer.move(DerClockwise, 1);
    motorIzq.move(IzqClockwise, 1);
    delayMicroseconds(STEP_DELAY_US);
  }
}

// Se mueve indefinidamente recto hasta que no se detecte las condiciones necesarias
void irRecto() {
  // Avanza en lotes mientras ambos sensores sigan en estado 3.
  while (true) {
    int estado = leerSensoresEstado();
    if (estado != 3) {
      // ya no estamos sobre la linea, paramos
      break;
    }
    // Avanzamos un pequeño lote de pasos (mantiene velocidad, permite reaccionar rápido)
    avanzarTicks(32);
    // continuar; leerSensoresEstado() será evaluado al inicio del while
  }
}



void girarDerechaTicks(unsigned long ticks) {
  // izquierda adelante, derecha atras
  bool izqDir = false;
  bool derDir = false;
  for (unsigned long i = 0; i < ticks; i++) {
    motorDer.move(derDir, 1);
    motorIzq.move(izqDir, 1);
  }
}

void girarIzquierdaTicks(unsigned long ticks) {
  // izquierda atras, derecha adelante
  bool izqDir = true;
  bool derDir = true;
  for (unsigned long i = 0; i < ticks; i++) {
    motorIzq.move(izqDir, 1);
    motorDer.move(derDir, 1);
    delayMicroseconds(STEP_DELAY_US);
  }
}

// corrección hasta recuperar ambos sensores==3 o hasta tope de pasos
bool corregirHastaAmbos(File &logFile) {
  unsigned long pasos = 0;

  while (pasos < CORRECTION_STEP_LIMIT) {
    int estado = leerSensoresEstado();

    // escribir el estado actual a la SD (registro de ruta)
    logFile.println(estado);
    logFile.flush();

    if (estado == 3) return true;
    else if (estado == 2) {
      girarDerechaTicks(100);
    } else if (estado == 1){
      girarIzquierdaTicks(100);
    }
    pasos++;
  }
  return false;
}

// ---------- Modos ----------
void modoGrabar() {
  Serial.println("Modo 1: GRABANDO ruta en SD (route.txt). Fin cuando ambos sensores = 0).");

  // eliminar si existe y crear nuevo archivo (reemplaza truncate)
  if (SD.exists(ROUTE_FILE)) {
    SD.remove(ROUTE_FILE);
  }
  File f = SD.open(ROUTE_FILE, FILE_WRITE);
  if (!f) {
    Serial.println("ERROR: no se pudo abrir route.txt para escritura.");
    return;
  }
  
  while (true) {
    reproducirMelodia();
    int estado = leerSensoresEstado();
    f.println(estado);
    f.flush();

    if (estado == 3) {
      avanzarTicks(128);
    } else if (estado == 2) {
      bool ok = corregirHastaAmbos(f);
      if (!ok) Serial.println("Timeout correccionDerecha. Continuando.");
    } else if (estado == 1) {
      bool ok = corregirHastaAmbos(f);
      if (!ok) Serial.println("Timeout correccionIzquierda. Continuando.");
    } else { // 0
      Serial.println("Ninguno detecta -> fin de grabación.");
      break;
    }
    delay(10);
  }

  f.close();
  Serial.println("Ruta guardada en route.txt");
}

void ejecutarEstado(int estado) {
  if (estado == 3) {
    avanzarTicks(128);
  } else if (estado == 2) {
    girarDerechaTicks(100);
  } else if (estado == 1) {
    girarIzquierdaTicks(100);
  } else {
    delay(100);
  }
}

void modoReproducir(bool verbose = true) {
  Serial.println("Modo Reproducir: leyendo route.txt...");
  if (!SD.exists(ROUTE_FILE)) {
    Serial.println("route.txt no existe en SD.");
    return;
  }
  File f = SD.open(ROUTE_FILE, FILE_READ);
  if (!f) {
    Serial.println("No se pudo abrir route.txt");
    return;
  }
  

  while (f.available()) {
    reproducirMelodia();
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    int estado = line.charAt(0) - '0';
    if (estado < 0 || estado > 3) continue;
    if (verbose) {
      Serial.print("Estado leido: ");
    }
    ejecutarEstado(estado);
  }
  f.close();
  Serial.println("Reproducción terminada.");
}

void reproducirMelodia() {
  if (!playingMelody) return; // si no está activa, salir

  unsigned long currentMillis = millis();

  if (noteIndex < numNotes) {
    int noteDuration = noteDurations[noteIndex];

    // Si ya pasó el tiempo de la nota actual
    if (currentMillis - previousMillis >= noteDuration) {
      previousMillis = currentMillis;

      if (melody[noteIndex] == 0) {
        noTone(BUZZER_PIN); // pausa entre notas
      } else {
        tone(BUZZER_PIN, melody[noteIndex]);
      }
      noteIndex++;
    }
  } else {
    // reinicia la canción cuando termina
    noteIndex = 0;
  }
}


// ---------- SETUP y LOOP ----------
void setup() {
  pinMode(BUZZER_PIN, OUTPUT);
  playingMelody = true;
  Serial.begin(115200);
  while (!Serial) {}

  pinMode(S_LEFT_PIN, INPUT);
  pinMode(S_RIGHT_PIN, INPUT);

  motorIzq.setRpm(60);
  motorDer.setRpm(60);

  if (!initSD()) {
    Serial.println("Continuando sin SD (no se podrá grabar/reproducir).");
  }

  int modo = readConfigMode();
  Serial.print("Modo desde config.txt: ");
  Serial.println(modo);

  if (modo == 1) {
    modoGrabar();
  } else if (modo == 2) {
    modoReproducir(true);
  } else if (modo == 3) {
    modoReproducir(false);
  } else {
    Serial.println("Modo inválido.");
  }

  Serial.println("Programa finalizado. Reinicia para ejecutar otra vez.");
}

void loop() {
}