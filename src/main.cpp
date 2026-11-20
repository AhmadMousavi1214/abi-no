#include <Arduino.h>

// ---------- pins ----------
const int SOIL_PIN = 34; // sensor A0
const int SOIL_PWR = 25; // sensor VCC (on only while reading)
const int PUMP_PIN = 26; // MOSFET gate

// ---------- calibration (measured) ----------
const int DRY_RAW = 4095;
const int WET_RAW = 535;

// ---------- irrigation thresholds: PROVISIONAL ----------
const int START_RAW = 3000; // irrigate when raw >= this (dry side)
const int STOP_RAW = 1000;  // stop when raw <= this (wet side)

// ---------- timing (short values for testing) ----------
const unsigned long CHECK_INTERVAL_MS = 10000; // final: 15 min
const unsigned long PUMP_ON_MS = 5000;         // one pulse
const unsigned long SOAK_MS = 30000;           // wait after pulse; final: 3-5 min

// ---------- safety limits ----------
const int CONFIRM_READS = 3; // consecutive dry reads needed
const int MAX_PULSES_PER_CYCLE = 6;
const unsigned long MAX_PUMP_MS_PER_DAY = 60000;
const int MIN_CHANGE = 100;      // raw must drop at least this after a pulse
const int NO_RESPONSE_LIMIT = 3; // pulses with no change before a fault

// ---------- state ----------
unsigned long pumpMsToday = 0;
unsigned long dayStartMs = 0;
bool faultLocked = false;

const int SAMPLES = 15;

int readSoilRaw()
{
  digitalWrite(SOIL_PWR, HIGH);
  delay(100);
  int v[SAMPLES];
  for (int i = 0; i < SAMPLES; i++)
  {
    v[i] = analogRead(SOIL_PIN);
    delay(5);
  }
  digitalWrite(SOIL_PWR, LOW);

  for (int i = 0; i < SAMPLES - 1; i++)
    for (int j = i + 1; j < SAMPLES; j++)
      if (v[j] < v[i])
      {
        int t = v[i];
        v[i] = v[j];
        v[j] = t;
      }
  return v[SAMPLES / 2]; // median
}

void runPump(unsigned long ms)
{
  Serial.printf("  pump ON %lu ms\n", ms);
  digitalWrite(PUMP_PIN, HIGH);
  delay(ms);
  digitalWrite(PUMP_PIN, LOW);
  pumpMsToday += ms;
  Serial.println("  pump OFF");
}

// true only if several reads in a row are all on the dry side
bool confirmedDry()
{
  for (int i = 0; i < CONFIRM_READS; i++)
  {
    if (readSoilRaw() < START_RAW)
      return false;
    delay(1000);
  }
  return true;
}

void irrigate()
{
  int noResponse = 0;
  Serial.println("irrigation cycle started");

  for (int pulse = 1; pulse <= MAX_PULSES_PER_CYCLE; pulse++)
  {
    if (pumpMsToday + PUMP_ON_MS > MAX_PUMP_MS_PER_DAY)
    {
      Serial.println("daily pump limit reached, stopping");
      return;
    }

    int before = readSoilRaw();
    Serial.printf("pulse %d: raw before = %d\n", pulse, before);
    runPump(PUMP_ON_MS);
    delay(SOAK_MS); // let the water soak in
    int after = readSoilRaw();
    Serial.printf("  raw after = %d\n", after);

    if (after <= STOP_RAW)
    {
      Serial.println("soil is wet enough, cycle done");
      return;
    }

    if (after > before - MIN_CHANGE)
    { // reading barely changed
      noResponse++;
      Serial.printf("  no response (%d/%d)\n", noResponse, NO_RESPONSE_LIMIT);
      if (noResponse >= NO_RESPONSE_LIMIT)
      {
        faultLocked = true;
        Serial.println("FAULT: soil reading does not respond. Irrigation disabled.");
        return;
      }
    }
    else
    {
      noResponse = 0;
    }
  }
  Serial.println("max pulses per cycle reached, stopping");
}

void setup()
{
  Serial.begin(115200);
  pinMode(PUMP_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, LOW); // pump OFF first, always
  pinMode(SOIL_PWR, OUTPUT);
  digitalWrite(SOIL_PWR, LOW);
  analogReadResolution(12);
  analogSetPinAttenuation(SOIL_PIN, ADC_11db);
  dayStartMs = millis();
  delay(2000);
  Serial.println("irrigation logic started");
}

void loop()
{
  if (faultLocked)
  {
    Serial.println("FAULT: irrigation disabled (reset the board to retry)");
    delay(10000);
    return;
  }

  // reset the daily pump counter every 24 hours
  if (millis() - dayStartMs >= 86400000UL)
  {
    dayStartMs = millis();
    pumpMsToday = 0;
  }

  int raw = readSoilRaw();
  Serial.printf("t=%lus raw=%d pumpToday=%lus\n", millis() / 1000, raw, pumpMsToday / 1000);

  if (raw >= START_RAW && confirmedDry())
  {
    irrigate();
  }
  delay(CHECK_INTERVAL_MS);
}