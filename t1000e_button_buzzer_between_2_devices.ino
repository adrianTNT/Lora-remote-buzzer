/*
 * VolkNet - two T1000-E units: hold a button on one, the other buzzes
 * Each received heartbeat = one short 50ms beep. Tap = single beep.
 * Hold = pulsed beeps (50ms on / ~250ms off). Fail-safe: beeps self-terminate.
 */

#include <RadioLib.h>
#include <Tracker_T1000_E_LoRaWAN_Examples.h>
#include <nrfx_timer.h>

// --- LR1110 radio pins (verified from Meshtastic T1000-E variant) ---
#define LR_NSS    (0 + 12)    // P0.12
#define LR_DIO1   (32 + 1)    // P1.01
#define LR_BUSY   (0 + 7)     // P0.07
#define LR_RESET  (32 + 10)   // P1.10
#define LR_TCXO_V 1.6

LR1110 radio = new Module(LR_NSS, LR_DIO1, LR_RESET, LR_BUSY);

// --- RF switch table (verified from Meshtastic rfswitch.h) - REQUIRED for RX ---
static const uint32_t rfswitch_dio_pins[Module::RFSWITCH_MAX_PINS] = {
  RADIOLIB_LR11X0_DIO5, RADIOLIB_LR11X0_DIO6,
  RADIOLIB_LR11X0_DIO7, RADIOLIB_LR11X0_DIO8, RADIOLIB_NC
};
static const Module::RfSwitchMode_t rfswitch_table[] = {
  { LR11x0::MODE_STBY,  { LOW,  LOW,  LOW,  LOW  } },
  { LR11x0::MODE_RX,    { HIGH, LOW,  LOW,  HIGH } },
  { LR11x0::MODE_TX,    { HIGH, HIGH, LOW,  HIGH } },
  { LR11x0::MODE_TX_HP, { LOW,  HIGH, LOW,  HIGH } },
  { LR11x0::MODE_TX_HF, { LOW,  LOW,  LOW,  LOW  } },
  { LR11x0::MODE_GNSS,  { LOW,  LOW,  HIGH, LOW  } },
  { LR11x0::MODE_WIFI,  { LOW,  LOW,  LOW,  LOW  } },
  END_OF_MODE_TABLE,
};

// --- LoRa params (must match on both units) ---
#define FREQ_MHZ   868.0
#define BW_KHZ     125.0
#define SF         9
#define CR         7
#define SYNC_WORD  RADIOLIB_LR11X0_LORA_SYNC_WORD_PRIVATE
#define TX_DBM     14
#define PREAMBLE   8

// --- buzzer (EN + hardware-timer PWM, per Seeed example) ---
const nrfx_timer_t pwm_timer = NRFX_TIMER_INSTANCE(2);
#define BUZZ_FREQ_HZ 3500
#define BEEP_MS      150          // <-- buzz length per heartbeat; change freely

static void pwm_toggle(nrf_timer_event_t, void*) {
  static bool s = LOW; s = !s; digitalWrite(PIN_BUZZER_PWM, s);
}
static void pwm_set_frequency(uint32_t f) {
  uint32_t period_us = 1e6 / f;
  nrfx_timer_disable(&pwm_timer);
  nrfx_timer_clear(&pwm_timer);
  nrfx_timer_extended_compare(&pwm_timer, NRF_TIMER_CC_CHANNEL0,
      period_us / 2, NRF_TIMER_SHORT_COMPARE0_CLEAR_MASK, true);
  nrfx_timer_enable(&pwm_timer);
}
bool buzzerOn = false;
void buzzStart() {
  if (buzzerOn) return;
  pwm_set_frequency(BUZZ_FREQ_HZ);
  digitalWrite(PIN_BUZZER_EN, HIGH);
  buzzerOn = true;
  digitalWrite(LED_GREEN, HIGH);  
}
void buzzStop() {
  if (!buzzerOn) return;
  digitalWrite(PIN_BUZZER_EN, LOW);
  nrfx_timer_disable(&pwm_timer);
  digitalWrite(PIN_BUZZER_PWM, LOW);
  buzzerOn = false;
  digitalWrite(LED_GREEN, LOW); 
}

// --- protocol + timing ---
const char* HEARTBEAT = "VKBZ";
#define HEARTBEAT_TX_MS  150
#define RX_TIMEOUT_MS    1000
#define MAX_HOLD_MS      30000

// --- button (T1000-E: P0.06, active-HIGH with pull-down) ---
#define BTN_PIN (0 + 6)

// --- radio state machine (non-blocking) ---
volatile bool operationDone = false;
void onRadioIrq() { operationDone = true; }
enum RadioMode { MODE_RX, MODE_TX };
RadioMode radioMode = MODE_RX;
bool radioOk = false;

bool     btnHeld = false;
uint32_t lastTxTime = 0;
uint32_t buzzStartedAt = 0;

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 1500)) { }

  // buzzer pins + timer
  pinMode(PIN_BUZZER_PWM, OUTPUT);
  pinMode(PIN_BUZZER_EN, OUTPUT);
  digitalWrite(PIN_BUZZER_PWM, LOW);
  digitalWrite(PIN_BUZZER_EN, LOW);
  nrfx_timer_config_t tc = NRFX_TIMER_DEFAULT_CONFIG;
  tc.frequency = NRF_TIMER_FREQ_1MHz;
  nrfx_timer_init(&pwm_timer, &tc, pwm_toggle);

  // button
  pinMode(BTN_PIN, INPUT_PULLDOWN);

  // boot beep proves the sketch runs
  Serial.println(F("=== BOOT ==="));
  buzzStart(); delay(BEEP_MS); buzzStop();

  // radio
  Serial.print(F("radio.begin()... "));
  int st = radio.begin(FREQ_MHZ, BW_KHZ, SF, CR, SYNC_WORD, TX_DBM, PREAMBLE, LR_TCXO_V);
  Serial.print(F("code ")); Serial.println(st);
  if (st == RADIOLIB_ERR_NONE) {
    radio.setRfSwitchTable(rfswitch_dio_pins, rfswitch_table);
    Serial.println(F("RF switch set"));
    radio.setPacketReceivedAction(onRadioIrq);
    radio.startReceive();
    radioMode = MODE_RX;
    radioOk = true;
    Serial.println(F("RADIO OK"));
  } else {
    Serial.println(F("RADIO FAILED"));
  }

  Serial.println(F("Ready - hold the button to buzz the other unit"));
}

void loop() {
  uint32_t now = millis();

  // alive print every 1s
  static uint32_t lastAlive = 0;
  if (now - lastAlive >= 1000) {
    lastAlive = now;
    Serial.print(F("alive radioOk=")); Serial.print(radioOk);
    Serial.print(F(" btn=")); Serial.println(digitalRead(BTN_PIN));
  }

  // button: active-HIGH
  btnHeld = (digitalRead(BTN_PIN) == HIGH);

  if (radioOk) {
    // handle radio interrupt (TX done or RX received)
    if (operationDone) {
      operationDone = false;
      if (radioMode == MODE_TX) {
        radio.finishTransmit();
        radio.startReceive();
        radioMode = MODE_RX;
      } else {
        size_t plen = radio.getPacketLength();
        uint8_t buf[16];
        int state = radio.readData(buf, plen);
        if (state == RADIOLIB_ERR_NONE && plen >= 4 &&
            memcmp(buf, HEARTBEAT, 4) == 0) {
          Serial.println(F("RX heartbeat -> beep"));
          buzzStart(); buzzStartedAt = now;   // (re)start/refresh the buzz
        }
        radio.startReceive();
        radioMode = MODE_RX;
      }
    }

    // sender: while held, non-blocking transmit every 300ms
    if (btnHeld && radioMode == MODE_RX && (now - lastTxTime >= HEARTBEAT_TX_MS)) {
      lastTxTime = now;
      int st = radio.startTransmit((uint8_t*)HEARTBEAT, 4);
      if (st == RADIOLIB_ERR_NONE) {
        radioMode = MODE_TX;
        Serial.println(F("TX heartbeat"));
      } else {
        Serial.print(F("TX err ")); Serial.println(st);
        radio.startReceive();
        radioMode = MODE_RX;
      }
    }
  }

  // each beep self-terminates after BEEP_MS (inherent fail-safe)
  if (buzzerOn && (now - buzzStartedAt >= BEEP_MS)) {
    buzzStop();
  }

  delay(5);
}