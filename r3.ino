#include <Adafruit_Fingerprint.h>
#include <EEPROM.h>
#include <LiquidCrystal_I2C.h>
#include <SoftwareSerial.h>

SoftwareSerial fingerSerial(3, 2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Hardware Pins
#define PIN_ADMIN 4     // Show Results
#define PIN_DELETE 5    // Wipe R3 DB
#define PIN_BUZZER 6
#define PIN_PARTY_A 8
#define LED_PARTY_A 9
#define LED_PARTY_B 10
#define LED_PARTY_C 11
#define PIN_PARTY_B 12
#define PIN_PARTY_C 13

// EEPROM Config
#define ADDR_VOTES_A 0
#define ADDR_VOTES_B 4
#define ADDR_VOTES_C 8
#define ADDR_VOTED_BITMAP 100 

uint8_t templateBuffer[512];
int byteIdx = 0;
int pendingTargetID = -1;

enum BallotLedState { BLED_LOCKED, BLED_READY, BLED_SELECTED_A, BLED_SELECTED_B, BLED_SELECTED_C, BLED_SUCCESS };
BallotLedState bLedState = BLED_LOCKED;

void updateBallotLEDs() {
  unsigned long ms = millis();
  if (bLedState == BLED_LOCKED) {
    digitalWrite(LED_PARTY_A, HIGH); digitalWrite(LED_PARTY_B, HIGH); digitalWrite(LED_PARTY_C, HIGH);
  } else if (bLedState == BLED_READY) {
    int step = (ms % 1200) / 400;
    digitalWrite(LED_PARTY_A, step == 0); digitalWrite(LED_PARTY_B, step == 1); digitalWrite(LED_PARTY_C, step == 2);
  } else if (bLedState == BLED_SELECTED_A || bLedState == BLED_SELECTED_B || bLedState == BLED_SELECTED_C) {
    bool on = (ms / 200) % 2 == 0;
    digitalWrite(LED_PARTY_A, bLedState == BLED_SELECTED_A && on);
    digitalWrite(LED_PARTY_B, bLedState == BLED_SELECTED_B && on);
    digitalWrite(LED_PARTY_C, bLedState == BLED_SELECTED_C && on);
  } else if (bLedState == BLED_SUCCESS) {
    digitalWrite(LED_PARTY_A, HIGH); digitalWrite(LED_PARTY_B, HIGH); digitalWrite(LED_PARTY_C, HIGH);
  }
}

void smartDelay(unsigned long ms) { unsigned long s = millis(); while (millis() - s < ms) updateBallotLEDs(); }
void beepShort() { digitalWrite(PIN_BUZZER, HIGH); smartDelay(100); digitalWrite(PIN_BUZZER, LOW); }
void beepLong() { digitalWrite(PIN_BUZZER, HIGH); smartDelay(600); digitalWrite(PIN_BUZZER, LOW); }

void setup() {
  Serial.begin(115200); lcd.init(); lcd.backlight(); fingerSerial.begin(57600);
  
  pinMode(PIN_ADMIN, INPUT_PULLUP); pinMode(PIN_DELETE, INPUT_PULLUP);
  pinMode(PIN_PARTY_A, INPUT_PULLUP); pinMode(PIN_PARTY_B, INPUT_PULLUP); pinMode(PIN_PARTY_C, INPUT_PULLUP);
  pinMode(LED_PARTY_A, OUTPUT); pinMode(LED_PARTY_B, OUTPUT); pinMode(LED_PARTY_C, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);

  if (EEPROM.read(510) != 0xAA) {
    unsigned long zero = 0;
    EEPROM.put(ADDR_VOTES_A, zero); EEPROM.put(ADDR_VOTES_B, zero); EEPROM.put(ADDR_VOTES_C, zero);
    for (int i = 0; i < 128; i++) EEPROM.update(ADDR_VOTED_BITMAP + i, 0);
    EEPROM.write(510, 0xAA);
  }
  lockUI();
}

void loop() {
  updateBallotLEDs();
  
  // Physical Admin Overrides
  if (digitalRead(PIN_ADMIN) == LOW) showResultsStandalone();
  if (digitalRead(PIN_DELETE) == LOW) wipeR3Standalone();

  if (Serial.available() > 0) {
    char p = Serial.peek();
    if (p == 'D') {
      Serial.read(); while (Serial.available() == 0);
      if (Serial.read() == ':') {
        for (int i = 0; i < 128; i++) {
          while (Serial.available() < 2);
          char h = Serial.read(); char l = Serial.read();
          if (byteIdx < 512) templateBuffer[byteIdx++] = (hexToNibble(h) << 4) | hexToNibble(l);
        }
        Serial.println("OK");
      }
    } else {
      String cmd = Serial.readStringUntil('\n'); cmd.trim();
      if (cmd.startsWith("START:")) {
        pendingTargetID = cmd.substring(6).toInt(); byteIdx = 0;
        lcd.clear(); lcd.print("Syncing Data..."); Serial.println("OK");
      } else if (cmd.startsWith("FINISH:")) {
        injectTemplate(pendingTargetID);
      } else if (cmd.startsWith("VOTE:")) {
        processUnlockAndVote(cmd.substring(5).toInt());
      } else if (cmd == "LOCK") {
        lockUI();
      }
    }
  }
}

uint8_t hexToNibble(char c) {
  c = toupper(c); if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10; return 0;
}

void injectTemplate(int id) {
  uint8_t downChar[] = {0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x04, 0x09, 0x01, 0x00, 0x0F};
  fingerSerial.write(downChar, 13);
  unsigned long ack = millis(); while (fingerSerial.available() < 12 && millis() - ack < 500);
  while (fingerSerial.available()) fingerSerial.read();

  for (int p = 0; p < 4; p++) {
    uint8_t pid = (p == 3) ? 0x08 : 0x02; uint16_t sum = pid + 0x82;
    uint8_t head[] = {0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, pid, 0x00, 0x82};
    fingerSerial.write(head, 9);
    for (int i = 0; i < 128; i++) { fingerSerial.write(templateBuffer[p * 128 + i]); sum += templateBuffer[p * 128 + i]; }
    fingerSerial.write(sum >> 8); fingerSerial.write(sum & 0xFF);
  }
  if (finger.storeModel(id) == FINGERPRINT_OK) { lcd.clear(); lcd.print("Sync OK"); Serial.println("SAVED"); }
  else Serial.println("ERROR");
}

void processUnlockAndVote(int targetID) {
  lcd.clear(); lcd.print("2FA REQUIRED"); lcd.setCursor(0, 1); lcd.print("Place Finger...");
  unsigned long start = millis(); bool auth = false;
  
  while (millis() - start < 15000) {
    if (finger.getImage() == FINGERPRINT_OK) {
      finger.image2Tz();
      if (finger.fingerFastSearch() == FINGERPRINT_OK && finger.fingerID == targetID) { auth = true; break; }
      else { lcd.clear(); lcd.print("DENIED"); beepLong(); smartDelay(1500); lcd.clear(); lcd.print("Try Again..."); }
    }
  }

  if (!auth) { Serial.println("FAIL"); lockUI(); return; }

  lcd.clear(); lcd.print("Select Party"); setBallotState(BLED_READY); beepShort();
  String chosen = ""; unsigned long vTimer = millis();

  while (millis() - vTimer < 20000) {
    if (digitalRead(PIN_PARTY_A) == LOW) chosen = "A";
    else if (digitalRead(PIN_PARTY_B) == LOW) chosen = "B";
    else if (digitalRead(PIN_PARTY_C) == LOW) chosen = "C";

    if (chosen != "") {
      if (chosen == "A") setBallotState(BLED_SELECTED_A);
      if (chosen == "B") setBallotState(BLED_SELECTED_B);
      if (chosen == "C") setBallotState(BLED_SELECTED_C);
      lcd.clear(); lcd.print("Selected: " + chosen); lcd.setCursor(0,1); lcd.print("Sealing Vote...");
      smartDelay(3000); break;
    }
  }

  if (chosen == "") { Serial.println("TIMEOUT"); lockUI(); return; }

  int addr = -1;
  if (chosen == "A") addr = ADDR_VOTES_A; else if (chosen == "B") addr = ADDR_VOTES_B; else if (chosen == "C") addr = ADDR_VOTES_C;
  unsigned long c = 0; EEPROM.get(addr, c); EEPROM.put(addr, c + 1);
  
  // Wipe JIT Template
  if(targetID != 1) finger.deleteModel(targetID);

  setBallotState(BLED_SUCCESS); beepLong(); lcd.clear(); lcd.print("VOTE CAST!");
  Serial.println("CAST:SUCCESS"); smartDelay(3000); lockUI();
}

void showResultsStandalone() {
  lcd.clear(); lcd.print("Officer Auth"); lcd.setCursor(0,1); lcd.print("Scan ID 1...");
  unsigned long t = millis(); bool auth = false;
  while(millis() - t < 10000) {
    if(finger.getImage() == FINGERPRINT_OK) {
      finger.image2Tz(); if(finger.fingerFastSearch() == FINGERPRINT_OK && finger.fingerID == 1) { auth = true; break; }
    }
  }
  if(auth) {
    unsigned long a=0,b=0,c=0; EEPROM.get(ADDR_VOTES_A, a); EEPROM.get(ADDR_VOTES_B, b); EEPROM.get(ADDR_VOTES_C, c);
    lcd.clear(); lcd.print("A:"); lcd.print(a); lcd.print(" B:"); lcd.print(b); lcd.setCursor(0,1); lcd.print("C:"); lcd.print(c);
    smartDelay(6000);
  } else { lcd.clear(); lcd.print("Access Denied"); smartDelay(2000); }
  lockUI();
}

void wipeR3Standalone() {
  lcd.clear(); lcd.print("Wipe Auth"); lcd.setCursor(0,1); lcd.print("Scan ID 1...");
  unsigned long t = millis(); bool auth = false;
  while(millis() - t < 10000) {
    if(finger.getImage() == FINGERPRINT_OK) {
      finger.image2Tz(); if(finger.fingerFastSearch() == FINGERPRINT_OK && finger.fingerID == 1) { auth = true; break; }
    }
  }
  if(auth) {
    unsigned long z=0; EEPROM.put(ADDR_VOTES_A, z); EEPROM.put(ADDR_VOTES_B, z); EEPROM.put(ADDR_VOTES_C, z);
    finger.emptyDatabase(); lcd.clear(); lcd.print("WIPED OK"); smartDelay(2000);
  } else { lcd.clear(); lcd.print("Access Denied"); smartDelay(2000); }
  lockUI();
}

void lockUI() { setBallotState(BLED_LOCKED); lcd.clear(); lcd.print("Ballot Locked"); }
