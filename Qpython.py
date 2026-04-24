"""
Qpython.py — Pure 2FA Biometric EVM Bridge
Runs natively on the Arduino UNO Q Linux environment.
Handles SQLite DB for voter tracking and TCP routing to the Ballot Unit.
"""
import sys, os, time, threading, sqlite3, queue
from arduino.app_utils import Bridge, App
import serial

sys.stdout.reconfigure(line_buffering=True)

USER_DB = "EVM_Database"
os.makedirs(USER_DB, exist_ok=True)
DB_FILE = os.path.join(USER_DB, "system_ledger.db")

# ── R3 Connection (via TCP Socket) ──
UNO_R3_PORT = 'socket://172.17.0.1:9000'
r3_serial = None
r3_response_q = queue.Queue()
r3_lock = threading.Lock()
_ballot_mode_active = False

def _init_db():
    with sqlite3.connect(DB_FILE) as conn:
        conn.execute("CREATE TABLE IF NOT EXISTS users (uid TEXT PRIMARY KEY, finger_name TEXT, voted INTEGER DEFAULT 0)")
        conn.execute("CREATE TABLE IF NOT EXISTS config (key TEXT PRIMARY KEY, value TEXT)")
_init_db()

def _get_db_conn():
    conn = sqlite3.connect(DB_FILE, timeout=5.0)
    conn.execute("PRAGMA journal_mode=WAL")
    return conn

# ── R3 Connection Watchdog ──
def _try_connect_r3():
    global r3_serial
    os.system("killall socat 2>/dev/null")
    time.sleep(0.3)
    # Automatically bind the first connected USB/ACM device
    os.system("socat TCP-LISTEN:9000,fork,reuseaddr FILE:$(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -n 1),b115200,raw,echo=0,nonblock 2>/dev/null &")
    try:
        time.sleep(1.5)
        new_serial = serial.serial_for_url(UNO_R3_PORT, baudrate=115200, timeout=2)
        with r3_lock:
            r3_serial = new_serial
        print("[BRIDGE] R3 Connected via socat.")
        return True
    except:
        return False

def _r3_send(command):
    global r3_serial
    with r3_lock: local_serial = r3_serial
    if local_serial:
        try:
            local_serial.reset_input_buffer()
            local_serial.write((command + "\n").encode('utf-8'))
            local_serial.flush()
            print(f"[TX] {command}")
            return "OK"
        except:
            with r3_lock: r3_serial = None
    return "TIMEOUT"

def _r3_listener_loop():
    global r3_serial
    while True:
        with r3_lock: local_serial = r3_serial
        if local_serial and local_serial.in_waiting > 0:
            try:
                line = local_serial.readline().decode('utf-8', errors='ignore').strip()
                if line and line != "PONG":
                    r3_response_q.put(line)
                    print(f"[RX] {line}")
            except: pass
        time.sleep(0.02)
threading.Thread(target=_r3_listener_loop, daemon=True).start()

# ── Bridge Functions ──
def get_r3_status():
    with r3_lock:
        return "ONLINE" if r3_serial is not None else "OFFLINE"

def begin_ballot_mode():
    global _ballot_mode_active
    _ballot_mode_active = True
    if not _try_connect_r3():
        threading.Thread(target=lambda: _try_connect_r3(), daemon=True).start()
    return "OK"

def end_ballot_mode():
    global _ballot_mode_active, r3_serial
    _ballot_mode_active = False
    with r3_lock:
        if r3_serial: r3_serial.close()
        r3_serial = None
    os.system("killall socat 2>/dev/null")
    return "OK"

def check_user_exists(uid):
    with _get_db_conn() as conn:
        res = conn.execute("SELECT 1 FROM users WHERE uid=?", (str(uid),)).fetchone()
        return "YES" if res else "NO"

def add_user(data):
    parts = data.split("|")
    if len(parts) == 2:
        with _get_db_conn() as conn:
            conn.execute("INSERT OR REPLACE INTO users (uid, finger_name, voted) VALUES (?, ?, 0)", (parts[0], parts[1]))
        return "OK"
    return "ERROR"

def get_voter_finger(uid):
    with _get_db_conn() as conn:
        res = conn.execute("SELECT finger_name FROM users WHERE uid=?", (str(uid),)).fetchone()
        return res[0] if res else ""

def check_has_voted(uid):
    with _get_db_conn() as conn:
        res = conn.execute("SELECT voted FROM users WHERE uid=?", (str(uid),)).fetchone()
        return "YES" if res and res[0] == 1 else "NO"

def mark_as_voted(uid):
    with _get_db_conn() as conn:
        conn.execute("UPDATE users SET voted=1 WHERE uid=?", (str(uid),))
    return "OK"

def wipe_all_auth_data():
    with _get_db_conn() as conn:
        conn.execute("DELETE FROM users")
        conn.execute("DELETE FROM config")
    return "OK"

def get_user_count():
    with _get_db_conn() as conn:
        res = conn.execute("SELECT COUNT(*) FROM users WHERE uid != '1'").fetchone()
        return str(res[0]) if res else "0"

def run_ballot_session(uid):
    # Sends vote command to R3 and waits for physical 2FA and ballot cast
    if _r3_send(f"VOTE:{uid}") != "OK": return "ERROR"
    deadline = time.time() + 60.0
    while time.time() < deadline:
        try:
            resp = r3_response_q.get(timeout=0.2)
            if resp == "CAST:SUCCESS":
                mark_as_voted(uid)
                return "SUCCESS"
            elif resp in ["FAIL", "TIMEOUT", "ERROR"]:
                return "FAILED"
        except queue.Empty: pass
    _r3_send("LOCK")
    return "TIMEOUT"

def extract_template(uid):
    if _r3_send(f"START:{uid}") == "OK": return "EXTRACT"
    return "ERROR"

def send_template_chunk(hex_data):
    if _r3_send(f"D:{hex_data}") == "OK": return "OK"
    return "ERROR"

def finish_template_transfer(uid):
    if _r3_send(f"FINISH:0") == "OK": return "OK"
    return "ERROR"

def set_admin_finger(name):
    with _get_db_conn() as conn:
        conn.execute("INSERT OR REPLACE INTO config (key, value) VALUES ('admin_finger', ?)", (name,))
    return "OK"

def get_admin_finger():
    with _get_db_conn() as conn:
        res = conn.execute("SELECT value FROM config WHERE key='admin_finger'").fetchone()
        return res[0] if res else "Enrolled Finger"

Bridge.provide("get_r3_status", get_r3_status)
Bridge.provide("begin_ballot_mode", begin_ballot_mode)
Bridge.provide("end_ballot_mode", end_ballot_mode)
Bridge.provide("check_user_exists", check_user_exists)
Bridge.provide("add_user", add_user)
Bridge.provide("get_voter_finger", get_voter_finger)
Bridge.provide("check_has_voted", check_has_voted)
Bridge.provide("wipe_all_auth_data", wipe_all_auth_data)
Bridge.provide("get_user_count", get_user_count)
Bridge.provide("run_ballot_session", run_ballot_session)
Bridge.provide("extract_template", extract_template)
Bridge.provide("send_template_chunk", send_template_chunk)
Bridge.provide("finish_template_transfer", finish_template_transfer)
Bridge.provide("set_admin_finger", set_admin_finger)
Bridge.provide("get_admin_finger", get_admin_finger)

def loop(): time.sleep(0.1)
App.run(user_loop=loop)
