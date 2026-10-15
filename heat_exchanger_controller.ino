#include <Arduino.h>
#include "src/HeatExchanger.h"

// Task scheduling timestamps
static uint32_t lastFastTick = 0;   // 100ms (10 Hz)
static uint32_t lastSlowTick = 0;   // 1000ms (1 Hz)
static uint32_t lastLcdTick = 0;    // 250ms (4 Hz)
static bool ledState = false;

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n==================================================");
    Serial.println("  ESP32 HEAT EXCHANGER CONTROL SYSTEM STARTING   ");
    Serial.println("==================================================");

    pinMode(PIN_STATUS_LED, OUTPUT);
    digitalWrite(PIN_STATUS_LED, HIGH);

    // 1. Initialize Subsystems
    Safety.init();
    Flow.init();
    Pump.init();
    Heater.init();
    Sensors.init();
    Display.init();
    Logger.init();
    Network.init();
    Gemini.init();
    Web.init();

    digitalWrite(PIN_STATUS_LED, LOW);
    Serial.println("[System] Initialization complete. Entering control loop.");
}

void loop() {
    uint32_t now = millis();

    // 1. Handle incoming HTTP requests
    Web.handleClient();

    // 2. High-frequency pump motor ramping
    Pump.update();

    // 3. Network background state
    Network.update();

    // 4. Fast Loop (10 Hz / 100 ms): Flow calculations & Safety interlocks
    if (now - lastFastTick >= 100) {
        lastFastTick = now;

        Flow.update();

        float thIn = Sensors.getHotIn();
        float thOut = Sensors.getHotOut();
        float tcIn = Sensors.getColdIn();
        float tcOut = Sensors.getColdOut();
        float flowLPM = Flow.getFlowRateLPM();

        bool heaterWanted = (Heater.getMode() != HEATER_MODE_OFF);
        Safety.update(flowLPM, thIn, thOut, tcIn, tcOut, heaterWanted);

        // Control temperature for thermostatic mode: Hot side outlet (Th,out) or Hot inlet
        float controlTemp = (thOut > -50.0f) ? thOut : thIn;
        Heater.update(controlTemp);
    }

    // 5. Display Refresh Loop (4 Hz / 250 ms)
    if (now - lastLcdTick >= 250) {
        lastLcdTick = now;
        String ipStr = Network.getActiveIP();
        Display.update(Thermo.getData(), Heater.isRelayEnergized(), Pump.getSpeedPercent(), ipStr.c_str());
    }

    // 6. Slow Loop (1 Hz / 1000 ms): Thermodynamic calculations, logging & AI
    if (now - lastSlowTick >= 1000) {
        lastSlowTick = now;

        // Retrieve latest Dallas conversions
        Sensors.updateReadings();

        float thIn = Sensors.getHotIn();
        float thOut = Sensors.getHotOut();
        float tcIn = Sensors.getColdIn();
        float tcOut = Sensors.getColdOut();
        float flowLPM = Flow.getFlowRateLPM();

        // Compute thermodynamic state (Q, LMTD, effectiveness)
        Thermo.compute(thIn, thOut, tcIn, tcOut, flowLPM);

        // Record telemetry into circular log buffer
        Logger.record(Thermo.getData(), Pump.getSpeedPercent(), Heater.isRelayEnergized());

        // Heartbeat LED (1 Hz if normal, 5 Hz fast flash if safety tripped)
        if (Safety.isTripped()) {
            ledState = !ledState;
            digitalWrite(PIN_STATUS_LED, ledState ? HIGH : LOW);
        } else {
            ledState = !ledState;
            digitalWrite(PIN_STATUS_LED, ledState ? HIGH : LOW);
        }

        // Trigger next asynchronous temperature conversion
        Sensors.requestReadings();

        // Gemini AI Periodic Auto-Analysis
        Gemini.update(Thermo.getData(), Pump.getSpeedPercent(), Heater.isRelayEnergized(), Heater.getTargetTemp());

        // Autonomous Closed-Loop AI Control (if enabled by user in Web UI)
        if (Gemini.isAutonomousControlEnabled()) {
            const GeminiRecommendation& rec = Gemini.getLastRecommendation();
            if (rec.hasValidRecommendation && (now / 1000 - rec.lastAnalysisTimeSec < 60)) {
                // Apply AI recommendations safely
                Pump.setSpeedPercent(rec.recommendedPumpPwm);
                if (rec.recommendedHeaterDuty > 0) {
                    Heater.setMode(HEATER_MODE_TIME_PWM);
                    Heater.setDutyPercent(rec.recommendedHeaterDuty);
                }
            }
        }
    }
}
