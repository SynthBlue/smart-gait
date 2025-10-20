#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <I2Cdev.h>
#include <MPU6050.h>
#include <EEPROM.h>
#include <math.h>
#include <WiFiManager.h>

WiFiManager wm;             // objeto WiFiManager

// ======= CONEXIÓN AL ACCESS POINT (opcionales - no se usan si WiFiManager provee credenciales) =====
const char* ssid     = "XXXX";
const char* password = "XXXX";

// ======== CONFIGURACIÓN MQTT =========
const char* mqtt_server = "XXXX";
const int   mqtt_port   = 123456789;
const char* mqtt_user   = "XXXX";
const char* mqtt_pass   = "XXXX";
const char* mqtt_topic  = "XXXX";

// ======== OBJETOS =========
WiFiClient espClient;
PubSubClient client(espClient);
MPU6050 mpu;

// ======== CONSTANTES DE ESCALA (default; se recalcula en setup()) ========
float accel_sensitivity = 4096.0; // se actualizará según getFullScaleAccelRange()
const float GYRO_SENSITIVITY = 131.0;

// ======== EEPROM ADDRESSES (coincidir con calibrador) =========
const int ADDR_AX = 0;
const int ADDR_AY = 4;
const int ADDR_AZ = 8;
const int ADDR_MAGIC = 12;
const uint8_t MAGIC = 0x42;

float bias_ax = 0.0, bias_ay = 0.0, bias_az = 0.0;

// ======== ESTRUCTURA PARA FEATURES (float) =========
struct FeatureStats {
  long count;
  float sum;
  float mean;
  float M2;
  float variance;
  float stdv;
  float minVal;
  float maxVal;
  bool isFirst;
};

FeatureStats fs_ax, fs_ay, fs_az;
FeatureStats fs_gx, fs_gy, fs_gz;

// ======== PARÁMETROS =========
int sample_count = 0;
const int limit_refresh = 100; // ventana: número de muestras por envío
String sensor_name = "sensor_01";

// ======== FUNCIONES AUXILIARES =========
void initFeature(FeatureStats &fs) {
  fs.count = 0;
  fs.sum = 0.0;
  fs.mean = 0.0;
  fs.M2 = 0.0;
  fs.variance = 0.0;
  fs.stdv = 0.0;
  fs.minVal = 0.0;
  fs.maxVal = 0.0;
  fs.isFirst = true;
}

void update_features(FeatureStats &fs, float value) {
  fs.count++;
  fs.sum += value;
  if (fs.isFirst) {
    fs.minVal = value;
    fs.maxVal = value;
    fs.isFirst = false;
  } else {
    if (value < fs.minVal) fs.minVal = value;
    if (value > fs.maxVal) fs.maxVal = value;
  }
  float delta = value - fs.mean;
  fs.mean += delta / fs.count;
  float delta2 = value - fs.mean;
  fs.M2 += delta * delta2;
  if (fs.count > 1) {
    fs.variance = fs.M2 / (fs.count - 1);
    fs.stdv = sqrt(fs.variance);
  } else {
    fs.variance = 0.0;
    fs.stdv = 0.0;
  }
}

void reset_features(FeatureStats &fs) {
  initFeature(fs);
}

// ======== EEPROM helper (lectura float) =========
float eepromReadFloat(int addr) {
  float v = 0.0;
  uint8_t *p = (uint8_t*)&v;
  for (int i=0;i<4;i++) p[i] = EEPROM.read(addr + i);
  return v;
}

void loadBiasFromEeprom() {
  EEPROM.begin(64);
  if (EEPROM.read(ADDR_MAGIC) == MAGIC) {
    bias_ax = eepromReadFloat(ADDR_AX);
    bias_ay = eepromReadFloat(ADDR_AY);
    bias_az = eepromReadFloat(ADDR_AZ);
    Serial.print("bias_ax: "); Serial.println(bias_ax,6);
    Serial.print("bias_ay: "); Serial.println(bias_ay,6);
    Serial.print("bias_az: "); Serial.println(bias_az,6);
  } else {
    Serial.println("No bias en EEPROM -> usando 0.0");
    bias_ax = bias_ay = bias_az = 0.0;
  }
}

// ======= Sensitivity mapping =======
float accelSensitivityFromRange(uint8_t range) {
  switch(range) {
    case 0: return 16384.0; // ±2g
    case 1: return 8192.0;  // ±4g
    case 2: return 4096.0;  // ±8g
    case 3: return 2048.0;  // ±16g
    default: return 4096.0;
  }
}

// ======== WIFI / MQTT =========
void setup_wifi() {
  Serial.println();
  Serial.println("== setup_wifi: iniciando ==");

  // Asegurar modo STA+AP (mejor compatibilidad)
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP); // opcional

  // Habilitar debug de WiFiManager (muestra más info en serial)
  wm.setDebugOutput(true);

  // Fijar timeout para el portal de configuración (segundos)
  wm.setConfigPortalTimeout(60); // si el portal está abierto, cierra después de 60s si no configuran

  // Intentar conectar usando credenciales guardadas; si no, abrirá portal (hasta timeout)
  bool res = wm.autoConnect("ESP8266-Config");
  if (res) {
    Serial.println("AutoConnect: conectado a WiFi (o credenciales ya guardadas).");
  } else {
    Serial.println("AutoConnect no conectó (timeout o error) — el portal habrá cerrado o nunca se abrió.");
    // Si quieres FORZAR siempre el portal para pruebas, descomenta:
    // wm.startConfigPortal("ESP8266-Config");
  }

  // Esperar conexión (pequeño loop)
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi conectado!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("No conectado a WiFi (posible portal activo o timeout).");
    Serial.print("Modo WiFi: "); Serial.println(WiFi.getMode());
  }
  Serial.println("== setup_wifi: fin ==");
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Conectando al broker MQTT...");
    String clientId = "ESP8266Client-";
    clientId += String(ESP.getChipId());
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      Serial.println("Conectado!");
    } else {
      Serial.print("Error, rc=");
      Serial.print(client.state());
      Serial.println(" - Reintentando en 5s...");
      delay(5000);
    }
  }
}

// ======== SETUP =========
void setup() {
  Serial.begin(115200);
  delay(10);

  // init feature structs
  initFeature(fs_ax); initFeature(fs_ay); initFeature(fs_az);
  initFeature(fs_gx); initFeature(fs_gy); initFeature(fs_gz);

  // WIFI + MQTT
  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setBufferSize(1024);

  // I2C + MPU
  Wire.begin(); // si necesitas pins explícitos: Wire.begin(D2,D1);
  Serial.println("Iniciando MPU6050...");

  mpu.initialize();
  mpu.setFullScaleAccelRange(MPU6050_ACCEL_FS_8);   // ±8 g (coincide con calibrador)
  mpu.setFullScaleGyroRange(MPU6050_GYRO_FS_250);   // ±250 °/s

  uint8_t range = mpu.getFullScaleAccelRange();
  accel_sensitivity = accelSensitivityFromRange(range);
  Serial.print("Accel FS code: "); Serial.print(range);
  Serial.print("  sens: "); Serial.println(accel_sensitivity);

  if (mpu.testConnection()) {
    Serial.println("MPU6050: Conexión satisfactoria");
  } else {
    Serial.println("MPU6050: Problemas en la conexión - revisa wiring");
    while (1) delay(1000);
  }

  // Cargar biases guardados (desde calibrador)
  loadBiasFromEeprom();
}

// ======== LOOP PRINCIPAL =========
void loop() {
  if (!client.connected()) reconnect();
  client.loop();

  int16_t raw_ax, raw_ay, raw_az, raw_gx, raw_gy, raw_gz;
  mpu.getMotion6(&raw_ax, &raw_ay, &raw_az, &raw_gx, &raw_gy, &raw_gz);

  // Convertir RAW -> unidades físicas usando sensibilidad detectada
  float ax_g = ((float)raw_ax) / accel_sensitivity;
  float ay_g = ((float)raw_ay) / accel_sensitivity;
  float az_g = ((float)raw_az) / accel_sensitivity;

  float gx_dps = ((float)raw_gx) / GYRO_SENSITIVITY;
  float gy_dps = ((float)raw_gy) / GYRO_SENSITIVITY;
  float gz_dps = ((float)raw_gz) / GYRO_SENSITIVITY;

  // --- APLICAR BIAS (corrección) justo después de convertir RAW->g ---
  ax_g -= bias_ax;
  ay_g -= bias_ay;
  az_g -= bias_az;

  // actualizar las 6 métricas (usar valores corregidos)
  update_features(fs_ax, ax_g);
  update_features(fs_ay, ay_g);
  update_features(fs_az, az_g);
  update_features(fs_gx, gx_dps);
  update_features(fs_gy, gy_dps);
  update_features(fs_gz, gz_dps);

  sample_count++;

  // DEBUG: mostrar la última lectura y el mean de AX (opcional)
  Serial.print("AX g="); Serial.print(ax_g, 3);
  Serial.print(" mean_ax="); Serial.print(fs_ax.mean, 3);
  Serial.print(" std_ax="); Serial.println(fs_ax.stdv, 3);

  if (sample_count >= limit_refresh) {
    unsigned long event_date_reg = millis();

    // Construir JSON con todas las métricas (valores en g y °/s)
    String json = "{";
    json += "\"sensor_name\":\"" + sensor_name + "\",";
    json += "\"event_date_reg\":" + String(event_date_reg) + ",";

    // Acelerómetro (g)
    json += "\"avg_ax\":" + String(fs_ax.mean, 3) + ",";
    json += "\"avg_ay\":" + String(fs_ay.mean, 3) + ",";
    json += "\"avg_az\":" + String(fs_az.mean, 3) + ",";

    json += "\"min_ax\":" + String(fs_ax.minVal, 3) + ",";
    json += "\"min_ay\":" + String(fs_ax.minVal, 3) + ",";
    json += "\"min_az\":" + String(fs_ax.minVal, 3) + ",";

    json += "\"max_ax\":" + String(fs_ax.maxVal, 3) + ",";
    json += "\"max_ay\":" + String(fs_ax.maxVal, 3) + ",";
    json += "\"max_az\":" + String(fs_ax.maxVal, 3) + ",";

    json += "\"std_ax\":" + String(fs_ax.stdv, 3) + ",";
    json += "\"std_ay\":" + String(fs_ay.stdv, 3) + ",";
    json += "\"std_az\":" + String(fs_az.stdv, 3) + ",";

    // Giroscopio (°/s)
    json += "\"avg_gx\":" + String(fs_gx.mean, 3) + ",";
    json += "\"avg_gy\":" + String(fs_gy.mean, 3) + ",";
    json += "\"avg_gz\":" + String(fs_gz.mean, 3) + ",";

    json += "\"min_gx\":" + String(fs_gx.minVal, 3) + ",";
    json += "\"min_gy\":" + String(fs_gy.minVal, 3) + ",";
    json += "\"min_gz\":" + String(fs_gz.minVal, 3) + ",";

    json += "\"max_gx\":" + String(fs_gx.maxVal, 3) + ",";
    json += "\"max_gy\":" + String(fs_gx.maxVal, 3) + ",";
    json += "\"max_gz\":" + String(fs_gx.maxVal, 3) + ",";

    json += "\"std_gx\":" + String(fs_gx.stdv, 3) + ",";
    json += "\"std_gy\":" + String(fs_gy.stdv, 3) + ",";
    json += "\"std_gz\":" + String(fs_gz.stdv, 3);

    json += "}";

    // DEBUG: longitud y contenido
    Serial.println("----- ENVIANDO JSON -----");
    Serial.println(json);
    Serial.print("Long json: ");
    Serial.println(json.length());

    // Publicar en MQTT (reintenta si falla)
    bool ok = client.publish(mqtt_topic, json.c_str());
    Serial.print("publish ok? ");
    Serial.println(ok ? "YES" : "NO");
    if (!ok) {
      Serial.print("client.state() = ");
      Serial.println(client.state());
      reconnect();
      delay(100);
      bool ok2 = client.publish(mqtt_topic, json.c_str());
      Serial.print("publish reintento? ");
      Serial.println(ok2 ? "YES" : "NO");
    }

    // reset features
    reset_features(fs_ax); reset_features(fs_ay); reset_features(fs_az);
    reset_features(fs_gx); reset_features(fs_gy); reset_features(fs_gz);

    sample_count = 0;
  }

  delay(20); // tasa ~5 Hz; ajustar según necesidad
}
