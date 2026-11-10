#include <Arduino.h>
#include "src/HeatExchanger.h"

// Task scheduling timestamps
static uint32_t lastFastTick = 0;   // 100 ms (10 Hz): flow, simulator, safety, heater
static uint32_t lastLcdTick = 0;    // 250 ms (4 Hz)
static uint32_t lastSlowTick = 0;   // 1000 ms (1 Hz): thermodynamics, logging
static bool ledState = false;

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n==================================================");
    Serial.println("  ESP32 HEAT EXCHANGER CONTROL SYSTEM STARTING   ");
    Serial.println("==================================================");

    pinMode(PIN_STATUS_LED, OUTPUT);
    digitalWrite(PIN_STATUS_LED, HIGH);

    // Actuators first so their pins are driven to a safe state immediately.
    Heater.init();
    Pump.init();
    ColdPump.init();
    Safety.init();
    FlowHot.init();
    FlowCold.init();
    Sensors.init();
    Settings.load();

    // Real hardware or virtual plant (AUTO: simulate when probes are missing/faulty).
    Commands::applySimMode(Sim.getMode());

    Display.init();
    Logger.init();
    NetManager.init();
    Gemini.init();
    Web.init();

    digitalWrite(PIN_STATUS_LED, LOW);
    Serial.printf("[System] Ready (%s). Free heap: %u bytes\n",
                  Sim.isActive() ? "SIMULATION" : "hardware", ESP.getFreeHeap());
}

void loop() {
    uint32_t now = millis();

    Web.handleClient();
    Sensors.update();       // DS18B20 conversion state machine (non-blocking)
    Pump.update();          // Soft ramp + PWM output stage (hot loop)
    ColdPump.update();      // Soft ramp + PWM output stage (cold loop)
    NetManager.update();

    // ---- Fast loop: interlocks and actuator control ---------------------
    if (now - lastFastTick >= 100) {
        lastFastTick = now;

        FlowHot.update();
        FlowCold.update();
        Sim.update(Pump.getSpeedPercent(), ColdPump.getSpeedPercent(), Heater.isRelayEnergized());

        float flowLPM = PV::flowLPM();
        float flowColdLPM = PV::flowColdLPM();
        Pump.updateFlowLoop(flowLPM);
        ColdPump.updateFlowLoop(flowColdLPM);

        SafetyInputs in;
        in.flowLPM = flowLPM;
        in.coldFlowLPM = flowColdLPM;
        in.probeFaultMask = PV::faultMask();
        in.maxValidTempC = PV::maxValidTemp();
        in.heaterDemand = Heater.isDemanding();
        in.relayEnergized = Heater.isRelayEnergized();
        in.pumpRunning = Pump.isRunning();
        Safety.update(in);

        Heater.update(PV::temp((ProbeIndex)Heater.getControlProbe()), Safety.isHeaterAllowed());

        // 5 Hz alarm flash while tripped
        if (Safety.isTripped()) {
            ledState = !ledState;
            digitalWrite(PIN_STATUS_LED, ledState);
        }
    }

    // ---- Display -------------------------------------------------------
    if (now - lastLcdTick >= 250) {
        lastLcdTick = now;
        String ip = NetManager.getActiveIP();
        if (Sim.isActive()) ip = "SIM " + ip;
        LcdStatus st{ Heater.isRelayEnergized(), Pump.getSpeedPercent(), ColdPump.getSpeedPercent(),
                      Heater.getModeShort(), Heater.getTargetTemp(), ip.c_str() };
        Display.update(Thermo.getData(), st);
    }

    // ---- Slow loop: thermodynamics and logging --------------------------
    if (now - lastSlowTick >= 1000) {
        lastSlowTick = now;

        // AUTO enters the virtual plant on any hardware fault and returns to
        // real hardware automatically once the fault clears.
        Commands::updateSimSupervisor();

        Thermo.compute(PV::temp(PROBE_HOT_IN), PV::temp(PROBE_HOT_OUT),
                       PV::temp(PROBE_COLD_IN), PV::temp(PROBE_COLD_OUT),
                       PV::flowLPM(), PV::flowColdLPM());
        Logger.record(Thermo.getData(), Pump.getSpeedPercent(), ColdPump.getSpeedPercent(),
                      Heater.isRelayEnergized(), (uint8_t)Heater.getDutyPercent(), Sim.isActive());

        if (!Safety.isTripped()) {
            ledState = !ledState;   // 1 Hz heartbeat
            digitalWrite(PIN_STATUS_LED, ledState);
        }
    }

    // ---- Gemini: periodic analysis, chat replies, AI closed loop -------
    Chat.update();
}