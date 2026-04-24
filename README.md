# 🗳️ Pure 2FA Biometric Electronic Voting Machine (Industry-Grade)

This repository contains the firmware and networking daemon for an industry-grade, Dual-Node 2FA Electronic Voting Machine (EVM). 

To ensure absolute election integrity, this architecture physically isolates voter verification from the actual casting of the ballot. It utilizes a Master/Slave hardware configuration, a lightweight Python TCP network bridge, Just-In-Time (JIT) biometric template synchronization, and secure edge-node EEPROM memory for tamper-proof vote tallying. This version is highly optimized for pure fingerprint-based Two-Factor Authentication (2FA).

## 🏗️ Dual-Node Architecture

### 1. Control Unit (CU) - Voter Verification & Server
* **Hardware:** Arduino UNO Q (SBC with Linux + Real-time MCU)
* **Role:** Acts as the powerful central server managed by the Presiding Officer (PO). It leverages its hybrid architecture to handle both real-time hardware scanning and high-level Linux networking/databases.
* **Function:** Enrolls and verifies a voter's identity using the local R307S module. Upon a successful match, it extracts their raw fingerprint template, encodes it into HEX, and streams it over a TCP socket to the Ballot Unit to authorize a single voting session.
* **Storage:** Utilizes an SQLite database (`system_ledger.db`) natively on the UNO Q's Linux environment to track registered voters, their designated scanning finger (e.g., "Right Index"), and whether they have voted.
* **Firmware:** `unoQ.ino` (Runs on the STM32 MCU side)
* **Networking:** Runs `Qpython.py` natively on the Linux OS to route serial commands and manage the database.

### 2. Ballot Unit (BU) - Vote Casting Edge Terminal
* **Hardware:** Arduino Uno R3
* **Role:** Placed in the private voting booth. It waits in a completely locked, stateless, memory-wiped state.
* **Function:** Once the CU pushes a voter's biometric template, the BU demands a **2nd-Factor physical scan (2FA)** to prevent "ghost voting" by an official. If the scan matches the temporarily injected template, the BU unlocks the physical candidate buttons.
* **Storage:** Votes are tallied locally and securely on the Arduino R3's non-volatile EEPROM memory, physically isolating the final ballot counts from the networked CU.
* **Firmware:** `r3.ino`

## 🔐 Core Security Features
* **Anti-Ghosting 2FA:** A vote cannot be cast simply because the Presiding Officer activated the machine. The voter must physically walk to the BU and scan their enrolled finger a second time.
* **Stateless Terminal Security:** If the BU is tampered with before or after a vote, no voter identity data can be stolen, as the local biometric cache is wiped immediately after every transaction.
* **Air-gapped Tallying:** The database that tracks *who* voted (SQLite on the CU) is entirely separate from the memory that tracks *who they voted for* (EEPROM on the BU).
* **EEPROM Hardware Locks:** Vote counts are stored securely on the BU. Results can only be viewed or wiped by pressing physical override buttons on the BU's circuitry while simultaneously providing the Presiding Officer's master fingerprint.

## 🔌 Hardware Pin Configurations

### 1. Control Unit (Arduino UNO Q)
* **Fingerprint Sensor (R307S):** Hardware Serial Pins `0 (RX)` and `1 (TX)`.
  * *Important: Disconnect these pins when uploading firmware via USB to avoid serial conflicts.*
* **Matrix Keypad (Rows):** Digital Pins `2, 3, 4, 5`
* **Matrix Keypad (Columns):** Analog Pins `A2, A3, A4, A5`
* **Status LEDs:** Green on `8`, Red on `6` (Indicates processing, scanning, and network transfer states).
* **16x2 LCD (I2C):** SDA, SCL

### 2. Ballot Unit (Arduino Uno R3)
* **Fingerprint Sensor (R307S):** SoftwareSerial on `2` (RX) and `3` (TX).
* **Party Input Buttons (Active LOW):** Party A (`8`), Party B (`12`), Party C (`13`). *(Wired to Ground)*
* **Party Status LEDs (PWM Capable):** Party A (`9`), Party B (`10`), Party C (`11`).
* **Admin Physical Overrides:** Show Results Button (`4`), Wipe Memory Button (`5`). *(Wired to Ground)*
* **Audio Feedback:** 5V Active Buzzer on `6`.
* **16x2 LCD (I2C):** SDA (`A4`), SCL (`A5`).

*(Note: Both R307S modules operate on 3.3V logic. Always use a voltage divider or logic level shifter on the Arduino's 5V TX line before connecting to the sensor's RX line).*

## 🚀 Setup & Deployment

1. **Hardware Assembly:** * Connect the Keypad, LEDs, and LCD to the UNO Q. 
   * Wire the 5 buttons, 3 LEDs, Buzzer, and LCD to the Uno R3. 
   * Connect an R307S fingerprint sensor to both boards using logic level shifters.
2. **Flash Firmware:** * Upload `unoQ.ino` to the UNO Q.
   * Upload `r3.ino` to the Uno R3.
3. **Network Bridge Initialization:** * Connect the Uno R3 to the UNO Q via USB (it will typically mount as `/dev/ttyACM0`).
   * **Establish the Serial-to-TCP Bridge:** You must route the USB serial data to a local TCP port so the Python daemon can communicate with the Ballot Unit. Open the UNO Q's Linux terminal and run:
     ```bash
     socat TCP-LISTEN:9000,fork,reuseaddr FILE:/dev/ttyACM0,b115200,raw,echo=0
     ```
     *(Note: If your Arduino R3 mounts on a different port like `/dev/ttyUSB0`, adjust the command accordingly).*
   * Once `socat` is running, execute `python3 Qpython.py` natively on the UNO Q's Linux OS to start the central database and routing daemon.
4. **First Boot (PO Enrollment):** * On power-up, the system will detect an empty database and mandate the enrollment of the Presiding Officer (ID #1). This master print is strictly required to authorize the voting phase, view results, or wipe the machine.
