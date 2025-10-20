#include <Wire.h>
#include <I2Cdev.h>
#include <MPU6050.h>
#include <EEPROM.h>

MPU6050 mpu;

const int CALIB_SAMPLES = 1000;

// EEPROM addresses
const int ADDR_AX = 0;   // 4 bytes
const int ADDR_AY = 4;
const int ADDR_AZ = 8;
const int ADDR_MAGIC = 12;
const uint8_t MAGIC = 0x42;

float accelSensitivityFromRange(uint8_t range) {
  switch(range) {
    case 0: return 16384.0; // ±2g
    case 1: return 8192.0;  // ±4g
    case 2: return 4096.0;  // ±8g
    case 3: return 2048.0;  // ±16g
    default: return 4096.0;
  }
}

void eepromWriteFloat(int addr, float value) {
  uint8_t *p = (uint8_t*)&value;
  for (int i=0;i<4;i++) EEPROM.write(addr + i, p[i]);
}

float eepromReadFloat(int addr) {
  float v = 0.0;
  uint8_t *p = (uint8_t*)&v;
  for (int i=0;i<4;i++) p[i] = EEPROM.read(addr + i);
  return v;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("--- MPU6050 CALIBRATION ---");
  Wire.begin();          // si necesitas pins explícitos: Wire.begin(D2,D1);

  mpu.initialize();
  if (!mpu.testConnection()) {
    Serial.println("MPU6050 NO CONECTA - revisa wiring");
    while(1) delay(1000);
  }

  uint8_t arange = mpu.getFullScaleAccelRange();
  float sens = accelSensitivityFromRange(arange);
  Serial.print("Accel FS code: "); Serial.print(arange);
  Serial.print("  sens: "); Serial.println(sens);

  Serial.println("Coloca el sensor en reposo, Z hacia arriba. Calibrando...");
  long sum_ax=0, sum_ay=0, sum_az=0;
  for (int i=0;i<CALIB_SAMPLES;i++) {
    int16_t rax,ray,raz,rgx,rgy,rgz;
    mpu.getMotion6(&rax,&ray,&raz,&rgx,&rgy,&rgz);
    sum_ax += (long)rax;
    sum_ay += (long)ray;
    sum_az += (long)raz;
    delay(3); // ~3000ms total for 1000 muestras
  }

  float avg_ax_g = (float)sum_ax / (float)CALIB_SAMPLES / sens;
  float avg_ay_g = (float)sum_ay / (float)CALIB_SAMPLES / sens;
  float avg_az_g = (float)sum_az / (float)CALIB_SAMPLES / sens;

  // Asumimos Z hacia arriba => valor esperado en Z = +1.0 g
  float bias_ax = avg_ax_g - 0.0;
  float bias_ay = avg_ay_g - 0.0;
  float bias_az = avg_az_g - 1.0;

  Serial.println("Calibración terminada. Resultados (g):");
  Serial.print("avg_ax_g: "); Serial.println(avg_ax_g,6);
  Serial.print("avg_ay_g: "); Serial.println(avg_ay_g,6);
  Serial.print("avg_az_g: "); Serial.println(avg_az_g,6);
  Serial.println("--- Bias to store (g) ---");
  Serial.print("bias_ax: "); Serial.println(bias_ax,6);
  Serial.print("bias_ay: "); Serial.println(bias_ay,6);
  Serial.print("bias_az: "); Serial.println(bias_az,6);

  // Guardar en EEPROM
  EEPROM.begin(64);
  eepromWriteFloat(ADDR_AX, bias_ax);
  eepromWriteFloat(ADDR_AY, bias_ay);
  eepromWriteFloat(ADDR_AZ, bias_az);
  EEPROM.write(ADDR_MAGIC, MAGIC);
  EEPROM.commit();
  Serial.println("Bias guardados en EEPROM.");
  Serial.println("Reinicia tu sketch principal que lea estos valores.");
}

void loop() {
  // nada
  delay(1000);
}
