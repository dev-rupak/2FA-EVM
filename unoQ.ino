#include <Adafruit_Fingerprint.h>
#include <Arduino_RouterBridge.h>
#include <Wire.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>

Adafruit_Fingerprint finger = Adafruit_Fingerprint(&Serial);
hd44780_I2Cexp lcd;

uint8_t templateBuffer[512];
int rowPins[4] = {2, 3, 4, 5};
int colPins[4] = {A2, A3, A4, A5};
char keys[4][4] = {{'1', '2', '3', 'A'}, {'4', '5', '6', 'B'}, {'7', '8', '9', 'C'}, {'*', '0', '#', 'D'}};

const int LED_GREEN = 8;
const int LED_RED   = 6;

enum LedState { LED_IDLE, LED_SCANNING, LED_FP_ENROLL, LED_TRANSFER, LED_PROCESSING, LED_SUCCESS, LED_ERROR };
LedState masterLedState = LED_IDLE;

void setMasterState(LedState state) {
  masterLedState = state;
  if (state == LED_SUCCESS) { digitalWrite(LED_GREEN, HIGH); digitalWrite(LED_RED, LOW); }
  else if (state == LED_ERROR) { digitalWrite(LED_RED, HIGH); digitalWrite(LED_GREEN, LOW); }
  else if (state == LED_IDLE) { digitalWrite(LED_GREEN, LOW); digitalWrite(LED_RED, LOW); }
}

void updateMasterLEDs() {
  unsigned long ms = millis();
  if (masterLedState == LED_IDLE) {
    digitalWrite(LED_GREEN, (ms % 1500 < 400) ? HIGH : LOW); digitalWrite(LED_RED, LOW);
  } else if (masterLedState == LED_SCANNING) {
    digitalWrite(LED_GREEN, ((ms / 500) % 2 == 0) ? HIGH : LOW); digitalWrite(LED_RED, LOW);
  } else if (masterLedState == LED_FP_ENROLL) {
    bool t = (ms / 80) % 2 == 0; digitalWrite(LED_GREEN, t ? HIGH : LOW); digitalWrite(LED_RED, t ? LOW : HIGH);
  } else if (masterLedState == LED_TRANSFER) {
    unsigned long p = ms % 3200;
    digitalWrite(LED_GREEN, (p < 2800) ? HIGH : LOW); digitalWrite(LED_RED, (p >= 1000 && p < 2800) ? HIGH : LOW);
  } else if (masterLedState == LED_PROCESSING) {
    bool t = (ms / 200) % 2 == 0; digitalWrite(LED_GREEN, t ? HIGH : LOW); digitalWrite(LED_RED, t ? LOW : HIGH);
  }
}

void smartDelay(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) { updateMasterLEDs(); }
}

void lcdPrint(const char *l1, const char *l2) {
  lcd.clear(); lcd.setCursor(0, 0); lcd.print(l1); lcd.setCursor(0, 1); lcd.print(l2);
}

void setup() {
  pinMode(LED_GREEN, OUTPUT); pinMode(LED_RED, OUTPUT); setMasterState(LED_IDLE);
  Bridge.begin();
  lcd.begin(16, 2);
  for (int i = 0; i < 4; i++) { pinMode(colPins[i], INPUT_PULLUP); pinMode(rowPins[i], OUTPUT); digitalWrite(rowPins[i], HIGH); }

  lcdPrint("2FA System", "Booting...");
  finger.begin(57600); smartDelay(1200);

  while (finger.loadModel(1) != FINGERPRINT_OK) {
    lcdPrint("No Officer Found", "Setup Required");
    smartDelay(2000);
    enrollAdminFlow();
  }
  showMainMenu();
}

void loop() {
  char key = readKeypad();
  if (!key) { smartDelay(20); return; }

  if (key == 'A' && verifyAdmin()) enrollFlow();
  else if (key == 'B' && verifyAdmin()) recognizeFlow();
  else if (key == 'C') countFlow();
  else if (key == 'D' && verifyAdmin()) deleteFlow();
  showMainMenu();
}

void enrollAdminFlow() {
  lcdPrint("Enroll Officer", "(ID: 1)"); smartDelay(1500);
  String fName = askWhichFinger();
  if (fName == "") return;
  
  if (enrollFinger(1)) {
    Bridge.call("set_admin_finger", fName.c_str());
    lcdPrint("* ADMIN SAVED *", fName.c_str()); smartDelay(2000);
  }
}

void enrollFlow() {
  String uid = getIdFromKeypad("Enroll Voter", "Voter ID:");
  if (uid == "") return;
  int fpId = uid.toInt();
  if (fpId < 2 || fpId > 1000) { lcdPrint("ID 2-1000 only", ""); smartDelay(2000); return; }

  String exists = ""; Bridge.call("check_user_exists", uid.c_str()).result(exists);
  if (exists == "YES") { lcdPrint("ID Already Used", ""); smartDelay(2000); return; }

  String fName = askWhichFinger();
  if (fName == "") return;

  if (enrollFinger(fpId)) {
    String payload = uid + "|" + fName;
    Bridge.call("add_user", payload.c_str());
    lcdPrint("* ENROLLED *", ("ID: " + uid).c_str()); smartDelay(2000);
  }
}

void recognizeFlow() {
  Bridge.call("begin_ballot_mode"); smartDelay(1000);
  String uid = getIdFromKeypad("Vote Mode", "Voter ID:");
  if (uid == "") { Bridge.call("end_ballot_mode"); return; }

  String voted = ""; Bridge.call("check_has_voted", uid.c_str()).result(voted);
  if (voted == "YES") { lcdPrint("Already Voted!", "Access Denied"); smartDelay(2000); Bridge.call("end_ballot_mode"); return; }

  String vFin = ""; Bridge.call("get_voter_finger", uid.c_str()).result(vFin);
  lcdPrint("CU Verification", vFin.c_str()); smartDelay(1200);

  if (!scanFinger(uid.toInt())) {
    lcdPrint("Access Denied", "Finger Mismatch"); smartDelay(2000);
    Bridge.call("end_ballot_mode"); return;
  }

  lcdPrint("Syncing to R3...", uid.c_str()); setMasterState(LED_TRANSFER);
  Bridge.call("extract_template", uid.c_str());
  if (!extractAndSendTemplate(uid.toInt())) { Bridge.call("end_ballot_mode"); return; }
  Bridge.call("finish_template_transfer", uid.c_str());

  lcdPrint("Proceed to BU", "Awaiting Cast..");
  String res = "";
  Bridge.call("run_ballot_session", uid.c_str()).result(res);
  
  if (res == "SUCCESS") { setMasterState(LED_SUCCESS); lcdPrint("VOTE CAST!", "Secure & Saved"); smartDelay(3000); }
  else { setMasterState(LED_ERROR); lcdPrint("BU TIMEOUT", "Vote Cancelled"); smartDelay(3000); }
  
  Bridge.call("end_ballot_mode");
}

// --- HARDWARE HELPERS ---
bool enrollFinger(int id) {
  setMasterState(LED_SCANNING); lcdPrint("Place Finger", "");
  while (finger.getImage() != FINGERPRINT_OK); finger.image2Tz(1);
  lcdPrint("Remove Finger", ""); smartDelay(2000);
  lcdPrint("Place Same Angle", "");
  while (finger.getImage() != FINGERPRINT_OK); finger.image2Tz(2);
  setMasterState(LED_PROCESSING);
  if (finger.createModel() == FINGERPRINT_OK && finger.storeModel(id) == FINGERPRINT_OK) {
    setMasterState(LED_SUCCESS); return true;
  }
  setMasterState(LED_ERROR); lcdPrint("Enroll Failed", "Try Again"); smartDelay(1500); return false;
}

bool scanFinger(int expectedId) {
  setMasterState(LED_SCANNING);
  unsigned long t = millis();
  while (millis() - t < 10000) {
    if (finger.getImage() == FINGERPRINT_OK) {
      finger.image2Tz();
      if (finger.fingerFastSearch() == FINGERPRINT_OK && finger.fingerID == expectedId) {
        setMasterState(LED_SUCCESS); return true;
      }
      setMasterState(LED_ERROR); lcdPrint("No Match", ""); smartDelay(1000); return false;
    }
  }
  setMasterState(LED_ERROR); return false;
}

bool extractAndSendTemplate(int id) {
  finger.loadModel(id);
  uint8_t upChar[] = {0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x04, 0x08, 0x01, 0x00, 0x0E};
  Serial.write(upChar, 13);
  long t = millis(); while (Serial.available() < 12 && millis() - t < 1000); for (int i = 0; i < 12; i++) Serial.read();

  int idx = 0;
  for (int p = 0; p < 4; p++) {
    while (Serial.available() < 9); for (int i = 0; i < 9; i++) Serial.read();
    for (int i = 0; i < 128; i++) { while (!Serial.available()); templateBuffer[idx++] = Serial.read(); }
    while (Serial.available() < 2); Serial.read(); Serial.read();
  }

  for (int chunk = 0; chunk < 4; chunk++) {
    String hex = ""; hex.reserve(260);
    for (int i = 0; i < 128; i++) {
      uint8_t b = templateBuffer[chunk * 128 + i];
      if (b < 0x10) hex += "0"; hex += String(b, HEX);
    }
    hex.toUpperCase();
    Bridge.call("send_template_chunk", hex.c_str());
  }
  return true;
}

String askWhichFinger() {
  lcdPrint("Hand: 1=R 2=L", "");
  char h = 0; while (h != '1' && h != '2') h = readKeypad();
  String hand = (h == '1') ? "R-" : "L-";
  
  lcdPrint("1Th 2Ix 3Md 4Rn", "5Pi");
  char f = 0; while (f < '1' || f > '5') f = readKeypad();
  if (f == '1') return hand + "Thumb"; if (f == '2') return hand + "Index";
  if (f == '3') return hand + "Middle"; if (f == '4') return hand + "Ring";
  return hand + "Pinky";
}

String getIdFromKeypad(const char *title, const char *prefix) {
  String id = "";
  while (true) {
    lcdPrint(title, (String(prefix) + " " + id + "_").c_str());
    char k = readKeypad();
    if (k >= '0' && k <= '9' && id.length() < 5) id += k;
    else if (k == '#') { if (id.length() > 0) id.remove(id.length() - 1); }
    else if (k == 'B' && id.length() > 0) return id;
    else if (k == '*') return "";
  }
}

bool verifyAdmin() { return scanFinger(1); }
void countFlow() { String c=""; Bridge.call("get_user_count").result(c); lcdPrint("DB Size:", (c + " Voters").c_str()); smartDelay(3000); }
void deleteFlow() { finger.emptyDatabase(); Bridge.call("wipe_all_auth_data"); lcdPrint("DB WIPED", ""); smartDelay(2000); enrollAdminFlow(); }
void showMainMenu() { setMasterState(LED_IDLE); lcdPrint("A:Reg  B:Vote", "C:Count D:Wipe"); }

char readKeypad() {
  for (int r = 0; r < 4; r++) {
    digitalWrite(rowPins[r], LOW);
    for (int c = 0; c < 4; c++) {
      if (digitalRead(colPins[c]) == LOW) { smartDelay(50); while (digitalRead(colPins[c]) == LOW); digitalWrite(rowPins[r], HIGH); return keys[r][c]; }
    } digitalWrite(rowPins[r], HIGH);
  } return 0;
}
