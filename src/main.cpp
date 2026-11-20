#include <Arduino.h>
#include "esp_sleep.h"

// ---------- pins ----------
const int SOIL_PIN = 34;
const int SOIL_PWR = 25;
const int PUMP_PIN = 26;
const int VBAT_PIN = 35;
const int CLEAR_PIN = 27; // button to GND: hold at wake/boot to clear fault

// ---------- thresholds: PROVISIONAL ----------
const int START_RAW = 3000; // irrigate when raw >= this
const int STOP_RAW = 1000;  // stop when raw <= this

// ---------- sleep ----------
const uint64_t SLEEP_INTERVAL_S = 30; // demo: 30 s. final: 15*60
const int WAKES_PER_DAY = 2880;       // 86400 / SLEEP_INTERVAL_S (final: 96)

// ---------- battery policy (off while powered from USB) ----------
const bool USE_BATTERY_POLICY = false;
const float BAT_RATIO = 2.0f;
const float BAT_MIN_RUN = 3.2f;
const float BAT_MIN_IRRIGATE = 3.4f;

// ---------- irrigation timing ----------
const unsigned long PUMP_ON_MS = 5000;
const unsigned long SOAK_MS = 30000; // final: 60-120 s
const unsigned long CONFIRM_GAP_MS = 1000;

// ---------- safety limits ----------
const int CONFIRM_READS = 3;
const int MAX_PULSES_PER_CYCLE = 6;
const unsigned long MAX_PUMP_MS_PER_DAY = 60000;
const int MIN_CHANGE = 100;
const int NO_RESPONSE_LIMIT = 3;

// ---------- state kept through deep sleep (RTC memory) ----------
RTC_DATA_ATTR unsigned long rtcPumpMsToday = 0;
RTC_DATA_ATTR int rtcWakeCount = 0;
RTC_DATA_ATTR bool rtcFault = false;

const int SAMPLES = 15;

void sortArr(int *v, int n)
{
  for (int i = 0; i < n - 1; i++)
    for (int j = i + 1; j < n; j++)
      if (v[j] < v[i])
      {
        int t = v[i];
        v[i] = v[j];
        v[j] = t;
      }
}

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
  sortArr(v, SAMPLES);
  return v[SAMPLES / 2];
}

float readVbat()
{
  int v[SAMPLES];
  for (int i = 0; i < SAMPLES; i++)
  {
    v[i] = analogReadMilliVolts(VBAT_PIN);
    delay(5);
  }
  sortArr(v, SAMPLES);
  return v[SAMPLES / 2] / 1000.0f * BAT_RATIO;
}

void runPump(unsigned long ms)
{
  Serial.printf("  pump ON %lu ms\n", ms);
  digitalWrite(PUMP_PIN, HIGH);
  delay(ms);
  digitalWrite(PUMP_PIN, LOW);
  rtcPumpMsToday += ms;
  Serial.println("  pump OFF");
}

bool confirmedDry()
{
  for (int i = 0; i < CONFIRM_READS; i++)
  {
    int r = readSoilRaw();
    Serial.printf("  confirm read %d: %d\n", i + 1, r);
    if (r < START_RAW)
      return false;
    delay(CONFIRM_GAP_MS);
  }
  return true;
}

void irrigate()
{
  int noResponse = 0;
  Serial.println("[irr] cycle started");

  for (int pulse = 1; pulse <= MAX_PULSES_PER_CYCLE; pulse++)
  {
    if (rtcPumpMsToday + PUMP_ON_MS > MAX_PUMP_MS_PER_DAY)
    {
      Serial.println("[irr] daily pump limit reached, stopping");
      return;
    }
    int before = readSoilRaw();
    Serial.printf("[irr] pulse %d: raw before = %d\n", pulse, before);

    runPump(PUMP_ON_MS);
    delay(SOAK_MS);

    int after = readSoilRaw();
    Serial.printf("[irr]   raw after = %d\n", after);

    if (after <= STOP_RAW)
    {
      Serial.println("[irr] soil wet enough, cycle done");
      return;
    }
    if (after > before - MIN_CHANGE)
    {
      noResponse++;
      Serial.printf("[irr]   no response (%d/%d)\n", noResponse, NO_RESPONSE_LIMIT);
      if (noResponse >= NO_RESPONSE_LIMIT)
      {
        rtcFault = true;
        Serial.println("[irr] FAULT: soil reading does not respond. Irrigation disabled.");
        return;
      }
    }
    else
    {
      noResponse = 0;
    }
  }
  Serial.println("[irr] max pulses per cycle reached, stopping");
}

void goToSleep()
{
  digitalWrite(PUMP_PIN, LOW);
  digitalWrite(SOIL_PWR, LOW);
  Serial.printf("sleeping %llu s (wake #%d, pumpToday=%lus, fault=%d)\n",
                SLEEP_INTERVAL_S, rtcWakeCount, rtcPumpMsToday / 1000, rtcFault);
  Serial.flush();
  esp_sleep_enable_timer_wakeup(SLEEP_INTERVAL_S * 1000000ULL);
  esp_deep_sleep_start();
}

void setup()
{
  pinMode(PUMP_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, LOW); // pump OFF first, always
  pinMode(SOIL_PWR, OUTPUT);
  digitalWrite(SOIL_PWR, LOW);

  Serial.begin(115200);
  delay(500);
  analogReadResolution(12);
  analogSetPinAttenuation(SOIL_PIN, ADC_11db);
  analogSetPinAttenuation(VBAT_PIN, ADC_11db);

  // clear-fault button: hold it while the board wakes/boots
  pinMode(CLEAR_PIN, INPUT_PULLUP);
  if (digitalRead(CLEAR_PIN) == LOW)
  {
    rtcFault = false;
    rtcPumpMsToday = 0;
    rtcWakeCount = 0;
    Serial.println("*** fault cleared by button ***");
  }

  rtcWakeCount++;
  if (rtcWakeCount >= WAKES_PER_DAY)
  { // new day: reset the pump counter
    rtcWakeCount = 0;
    rtcPumpMsToday = 0;
  }

  Serial.printf("\n=== wake #%d, wake cause: %d ===\n",
                rtcWakeCount, (int)esp_sleep_get_wakeup_cause());

  if (rtcFault)
  {
    Serial.println("FAULT flag set: irrigation disabled (hold button to clear)");
    goToSleep();
  }

  if (USE_BATTERY_POLICY)
  {
    float vbat = readVbat();
    Serial.printf("Vbat=%.2f V\n", vbat);
    if (vbat < BAT_MIN_RUN)
    {
      Serial.println("battery too low, sleeping");
      goToSleep();
    }
  }

  int raw = readSoilRaw();
  Serial.printf("soil raw = %d\n", raw);

  if (raw >= START_RAW && confirmedDry())
  {
    bool batOk = true;
    if (USE_BATTERY_POLICY)
      batOk = readVbat() >= BAT_MIN_IRRIGATE;
    if (batOk)
      irrigate();
    else
      Serial.println("battery too low to irrigate");
  }
  else
  {
    Serial.println("soil is OK, no irrigation");
  }

  goToSleep();
}

void loop() {}