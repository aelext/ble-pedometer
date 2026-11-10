/*
  BLE Pedometer for Bruin Software Engineering (UCLA)

  Samples the onboard accelerometer, counts candidate steps by peak detection
  on the acceleration magnitude, and uses an Edge Impulse model to decide
  whether each sample window is "Walking". Candidate steps are only added to
  the total when the window is classified as walking. The running total is
  published over BLE as a notify characteristic.
*/

#include <ArduinoBLE.h>
#include <Arduino_LSM6DSOX.h>
#include <yalext08-bse-wearable_inferencing.h>


// =====================================================================
//  BLE
// =====================================================================

BLEService stepService("49B10010-E8F2-537E-4F6C-D104768A1214");
BLEUnsignedIntCharacteristic stepChar("49B10011-E8F2-537E-4F6C-D104768A1214", BLERead | BLENotify);

unsigned int steps = 0;      // confirmed step total published over BLE
bool wasConnected = false;   // connection state from the previous check
int pendingSteps;            // candidate steps in the current window, awaiting classification


void setupBLE() {
  if (!BLE.begin()) {
    Serial.println("starting Bluetooth® Low Energy module failed!");
    while (1);
  }

  BLE.setLocalName("Step Tracker");
  BLE.setAdvertisedService(stepService);

  stepService.addCharacteristic(stepChar);
  BLE.addService(stepService);

  stepChar.writeValue(0);

  BLE.advertise();
  Serial.println("BLE Step Tracker Peripheral");
}

// Polled between samples so connect/disconnect events are handled without blocking.
void updateBLE() {
  BLEDevice central = BLE.central();
  bool connected = central && central.connected();

  if (connected && !wasConnected) {
    Serial.print("Connected to central: ");
    Serial.println(central.address());
    stepChar.writeValue(steps);   // send the current total as soon as a central connects
  } else if (!connected && wasConnected) {
    Serial.println("Disconnected from central");
  }

  wasConnected = connected;
}

void publishSteps() {
  stepChar.writeValue(steps);
}


// =====================================================================
//  Step detection
// =====================================================================

// Called once per accelerometer sample (x, y, z in g).
// Counts a step when the acceleration magnitude rises above HIGH_THRESHOLD,
// then waits for it to fall below LOW_THRESHOLD before another step can count
// (hysteresis). A cooldown rejects double counts from a single footfall.
void detectStep(float x, float y, float z) {
  const float HIGH_THRESHOLD = 1.20f;          // g, magnitude that registers a step
  const float LOW_THRESHOLD = 1.10f;           // g, magnitude that re-arms detection
  const unsigned long COOLDOWN_MS = 300;       // minimum time between steps

  static bool aboveThreshold = false;          // true while the current peak is still in progress
  static unsigned long lastStepTime = 0;

  float mag = sqrtf(x * x + y * y + z * z);
  unsigned long now = millis();

  if (mag > HIGH_THRESHOLD && !aboveThreshold && (now - lastStepTime > COOLDOWN_MS)) {
    pendingSteps++;
    aboveThreshold = true;
    lastStepTime = now;
  } else if (mag < LOW_THRESHOLD) {
    aboveThreshold = false;
  }
}


// =====================================================================
//  Edge Impulse classification
//  https://docs.edgeimpulse.com/hardware/deployments/run-cpp-overview
// =====================================================================

static const char* WALKING_LABEL = "Walking";     // must match the Edge Impulse label exactly
static const float CONFIDENCE_THRESHOLD = 0.70f;  // minimum score to accept "Walking"

static unsigned long time_between_samples_us = (unsigned long)(EI_CLASSIFIER_INTERVAL_MS * 1000.0f);

// One window of interleaved x, y, z samples for the classifier
static float features[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE];

// Runs the model on the current window. Returns true if the top label is
// "Walking" with at least CONFIDENCE_THRESHOLD confidence.
bool classifyWindow() {
  ei_impulse_result_t result = { 0 };

  signal_t signal;
  numpy::signal_from_buffer(features, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, &signal);

  EI_IMPULSE_ERROR res = run_classifier(&signal, &result, false);
  if (res != EI_IMPULSE_OK) {
    Serial.print("run_classifier failed: ");
    Serial.println(res);
    return false;
  }

  for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
    Serial.print(result.classification[ix].label);
    Serial.print(": ");
    Serial.print(result.classification[ix].value, 2);
    Serial.print("  ");
  }

  size_t best = 0;
  for (size_t ix = 1; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
    if (result.classification[ix].value > result.classification[best].value) best = ix;
  }

  return strcmp(result.classification[best].label, WALKING_LABEL) == 0 &&
         result.classification[best].value >= CONFIDENCE_THRESHOLD;
}


// =====================================================================
//  Main
// =====================================================================

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000);   // wait up to 3 s for Serial so it still runs on battery

  if (!IMU.begin()) {
    Serial.println("Failed to initialize IMU!");
    while (1);
  }

  setupBLE();
}

void loop() {
  float x, y, z;

  // Fill one window at the model's sampling rate, running step detection on each sample
  for (size_t ix = 0; ix < EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE; ix += EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME) {
    unsigned long next_tick = micros() + time_between_samples_us;
    IMU.readAcceleration(x, y, z);   // g

    features[ix + 0] = x;
    features[ix + 1] = y;
    features[ix + 2] = z;

    detectStep(x, y, z);

    while ((long)(micros() - next_tick) < 0) {
      updateBLE();
    }
  }

  // Keep this window's candidate steps only if the model says the user was walking
  bool walking = classifyWindow();

  if (walking && pendingSteps > 0) {
    steps += pendingSteps;
    publishSteps();
  }
  pendingSteps = 0;

  Serial.print(" steps: ");
  Serial.println(steps);
}
