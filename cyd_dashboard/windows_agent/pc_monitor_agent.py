"""
CYD PC Dashboard - Windows agent (v3, CPU/RAM/GPU/NET edition)

Reads CPU (overall + per-core + temp), RAM, swap, top processes, GPU
(load + temp via LibreHardwareMonitor), and network throughput, and
sends them as one JSON line per second over USB serial to the ESP32
CYD board.

Setup:
    pip install -r requirements.txt

Usage:
    python pc_monitor_agent.py                  # auto-detect the CYD's COM port
    python pc_monitor_agent.py --port COM5       # force a specific port
    python pc_monitor_agent.py --list            # list available serial ports
    python pc_monitor_agent.py --list-sensors    # diagnostic: dump CPU/GPU sensors
"""

import argparse
import json
import re
import socket
import sys
import time

import psutil
import requests
import serial
import serial.tools.list_ports

# Optional - only used as a fallback for CPU temperature on older
# LibreHardwareMonitor versions. The script works fine without it.
try:
    import wmi
    HAVE_WMI = True
except ImportError:
    HAVE_WMI = False

# LibreHardwareMonitor's REST API (Options > Remote Web Server > Run).
# This is the primary way we read CPU/GPU temp/load/name - it works on
# every version, including 0.9.5+ where LibreHardwareMonitor's WMI output
# broke entirely.
LHM_REST_URL = "http://localhost:8085/data.json"

# LibreHardwareMonitor reports full hardware names like "12th Gen Intel(R)
# Core(TM) i7-12700K" or "NVIDIA GeForce RTX 4060 Ti" - these patterns
# pull out just the short model part so it fits on the small display.
CPU_NAME_PATTERNS = [r"i[3579]-\d{4,5}[A-Z]{0,2}", r"Ryzen\s+\d\s+\d{3,4}[A-Z0-9]*"]
GPU_NAME_PATTERNS = [r"RTX\s?\d{3,4}(?:\s?Ti)?(?:\s?SUPER)?", r"GTX\s?\d{3,4}(?:\s?Ti)?",
                      r"RX\s?\d{3,4}(?:\s?XT)?"]

BAUD_RATE = 115200
SEND_INTERVAL_SEC = 1.0


def list_ports():
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        print("No serial ports found.")
        return
    for p in ports:
        print(f"  {p.device}  -  {p.description}")


def autodetect_port():
    for p in serial.tools.list_ports.comports():
        desc = (p.description or "").upper()
        if "CH340" in desc or "USB-SERIAL" in desc or "USB SERIAL" in desc:
            return p.device
    return None


def get_local_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except OSError:
        return ""


def get_network_rates(prev_counters, prev_time):
    counters = psutil.net_io_counters()
    now = time.time()
    elapsed = max(now - prev_time, 0.001)
    up_kbps = (counters.bytes_sent - prev_counters.bytes_sent) / 1024.0 / elapsed
    down_kbps = (counters.bytes_recv - prev_counters.bytes_recv) / 1024.0 / elapsed
    return max(up_kbps, 0), max(down_kbps, 0), counters, now


def _walk_lhm_tree(node, results):
    """Recursively collect (sensor_id, label, raw_value_string) for every
    sensor leaf in LibreHardwareMonitor's REST JSON tree - covers any
    unit (°C, %, etc), filtered by the caller as needed."""
    value_str = node.get("Value", "")
    sensor_id = node.get("SensorId", "")
    if sensor_id and isinstance(value_str, str):
        results.append((sensor_id, node.get("Text", ""), value_str))
    for child in node.get("Children", []):
        _walk_lhm_tree(child, results)


def _extract_number(value_str, unit_symbol):
    """Pulls the leading number out of a value string like '45.0 °C' or
    '12 %', only if it actually has the given unit - returns None otherwise."""
    if unit_symbol not in value_str:
        return None
    try:
        return float(value_str.split()[0].replace(",", "."))
    except (ValueError, IndexError):
        return None


def _pick_preferred(candidates, keywords):
    """Given a list of (id_or_name, label, value), prefer whichever one
    matches a keyword (checked in priority order), falling back to the
    single highest value if nothing matches."""
    if not candidates:
        return None
    for keyword in keywords:
        for _key, label, value in candidates:
            if keyword in label.lower():
                return value
    return max(value for _key, _label, value in candidates)


def _fetch_lhm_tree():
    """One HTTP call to LibreHardwareMonitor's REST API, returning the
    raw JSON tree, or None if the server isn't reachable."""
    try:
        resp = requests.get(LHM_REST_URL, timeout=1)
        resp.raise_for_status()
        return resp.json()
    except Exception:
        return None


def _find_hardware_name(node, sensor_id_prefix, ancestors=None):
    """Given the LHM tree and a sensor id prefix like '/nvidiagpu/0', find
    the friendly hardware name that owns it (e.g. 'NVIDIA GeForce RTX
    4060 Ti'). LibreHardwareMonitor's tree is Computer > Hardware >
    SensorType-group > Sensor, so once we reach the matching leaf sensor,
    the hardware name is two levels back up the ancestor chain."""
    if ancestors is None:
        ancestors = []
    sid = node.get("SensorId") or ""
    if "Value" in node and sid.startswith(sensor_id_prefix):
        if len(ancestors) >= 2:
            return ancestors[-2]
        if ancestors:
            return ancestors[-1]
        return None
    for child in node.get("Children", []):
        found = _find_hardware_name(child, sensor_id_prefix, ancestors + [node.get("Text", "")])
        if found:
            return found
    return None


def _shorten_name(full_name, patterns):
    """Pull just the short model number out of a full hardware name using
    the given regex patterns; falls back to a truncated full name if none
    of the patterns match (still better than nothing on a small screen)."""
    for pattern in patterns:
        m = re.search(pattern, full_name, re.IGNORECASE)
        if m:
            return m.group(0)
    return full_name[:24]


def _cpu_temp_via_wmi():
    """Fallback for older (pre-0.9.5) LibreHardwareMonitor versions, where
    WMI still works but the REST API might not be running."""
    if not HAVE_WMI:
        return None
    try:
        w = wmi.WMI(namespace="root\\LibreHardwareMonitor")
        candidates = []
        for sensor in w.Sensor():
            if sensor.SensorType != "Temperature":
                continue
            parent = (sensor.Parent or "").lower()
            if "cpu" not in parent:
                continue
            candidates.append((sensor.Name or "", sensor.Name or "", sensor.Value))
        return _pick_preferred(candidates, ("package", "tctl", "tdie", "die"))
    except Exception:
        return None  # LibreHardwareMonitor not running, or WMI not accessible


def get_lhm_stats():
    """Best-effort CPU/GPU/RAM stats from LibreHardwareMonitor.

    Returns a dict with whichever of "ctemp", "cpuname", "gpu", "gtemp",
    "gpuname", "rtemp" it could find - missing keys just mean that sensor
    or name wasn't available (e.g. no discrete GPU, RAM temp not exposed
    by the motherboard, or LibreHardwareMonitor isn't running). SensorId
    paths look like "/amdcpu/0/temperature/0" or "/nvidiagpu/0/load/0",
    so we filter on "cpu"/"gpu" plus "/temperature/"/"/load/" in the id
    rather than guessing at label text - this works the same across
    Intel/AMD/NVIDIA without hardcoding vendor-specific sensor names.
    """
    result = {}
    tree = _fetch_lhm_tree()

    if tree is None:
        # REST API not reachable - fall back to WMI for CPU temp only
        # (older LibreHardwareMonitor versions; no GPU/name fallback here).
        temp = _cpu_temp_via_wmi()
        if temp is not None:
            result["ctemp"] = round(temp, 1)
        return result

    sensors = []
    _walk_lhm_tree(tree, sensors)

    cpu_temps = [(s, l, _extract_number(v, "°C")) for s, l, v in sensors
                 if "cpu" in s.lower() and "/temperature/" in s.lower()]
    cpu_temps = [(s, l, v) for s, l, v in cpu_temps if v is not None]
    ctemp = _pick_preferred(cpu_temps, ("package", "tctl", "tdie", "die"))
    if ctemp is not None:
        result["ctemp"] = round(ctemp, 1)

    cpu_sensor_ids = [s for s, _l, _v in sensors if re.match(r"^/(intel|amd)cpu/", s.lower())]
    if cpu_sensor_ids:
        prefix = "/".join(cpu_sensor_ids[0].split("/")[:3])  # e.g. "/intelcpu/0"
        raw_name = _find_hardware_name(tree, prefix)
        if raw_name:
            result["cpuname"] = _shorten_name(raw_name, CPU_NAME_PATTERNS)

    gpu_loads = [(s, l, _extract_number(v, "%")) for s, l, v in sensors
                 if "gpu" in s.lower() and "/load/" in s.lower()]
    gpu_loads = [(s, l, v) for s, l, v in gpu_loads if v is not None]
    gload = _pick_preferred(gpu_loads, ("gpu core", "core"))
    if gload is not None:
        result["gpu"] = round(gload, 1)

    gpu_temps = [(s, l, _extract_number(v, "°C")) for s, l, v in sensors
                 if "gpu" in s.lower() and "/temperature/" in s.lower()]
    gpu_temps = [(s, l, v) for s, l, v in gpu_temps if v is not None]
    gtemp = _pick_preferred(gpu_temps, ("gpu core", "core", "hot spot", "junction"))
    if gtemp is not None:
        result["gtemp"] = round(gtemp, 1)

    gpu_sensor_ids = [s for s, _l, _v in sensors if "gpu" in s.lower()]
    if gpu_sensor_ids:
        prefix = "/".join(gpu_sensor_ids[0].split("/")[:3])  # e.g. "/nvidiagpu/0"
        raw_name = _find_hardware_name(tree, prefix)
        if raw_name:
            result["gpuname"] = _shorten_name(raw_name, GPU_NAME_PATTERNS)

    # RAM temp is best-effort - many boards/DIMMs don't expose this at all
    ram_temps = [(s, l, _extract_number(v, "°C")) for s, l, v in sensors
                 if "ram" in s.lower() or "memory" in l.lower() or "dimm" in l.lower()]
    ram_temps = [(s, l, v) for s, l, v in ram_temps if v is not None]
    rtemp = _pick_preferred(ram_temps, ("memory", "ram", "dimm"))
    if rtemp is not None:
        result["rtemp"] = round(rtemp, 1)

    return result


def get_top_processes(n=3):
    """Top N processes by memory usage (RSS), as [{"n": name, "m": mb}]."""
    procs = []
    for p in psutil.process_iter(["name", "memory_info"]):
        try:
            mem = p.info["memory_info"]
            if mem is None:
                continue
            procs.append((p.info["name"] or "?", mem.rss / (1024 ** 2)))
        except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
            continue
    procs.sort(key=lambda x: x[1], reverse=True)
    return [{"n": name[:16], "m": round(mb, 1)} for name, mb in procs[:n]]


def build_payload(prev_counters, prev_time, local_ip):
    cpu = psutil.cpu_percent(interval=None)
    per_core = psutil.cpu_percent(interval=None, percpu=True)
    vm = psutil.virtual_memory()
    swap = psutil.swap_memory()

    freq = None
    try:
        f = psutil.cpu_freq()
        if f:
            freq = f.current
    except (AttributeError, NotImplementedError):
        pass

    up_kbps, down_kbps, counters, now = get_network_rates(prev_counters, prev_time)

    payload = {
        "cpu": round(cpu, 1),
        "cores": [round(c, 1) for c in per_core],
        "ram": round(vm.percent, 1),
        "ramu": round(vm.used / (1024 ** 3), 2),
        "ramt": round(vm.total / (1024 ** 3), 2),
        "swap": round(swap.percent, 1),
        "procs": get_top_processes(3),
        "up": round(up_kbps, 1),
        "down": round(down_kbps, 1),
        "ip": local_ip,
    }

    if freq:
        payload["freq"] = round(freq, 0)

    payload.update(get_lhm_stats())  # adds ctemp, gpu, gtemp where available

    return payload, counters, now


def list_sensors():
    """Diagnostic: dump every CPU/GPU temperature and load sensor we can
    find via LibreHardwareMonitor's REST API, plus what get_lhm_stats()
    actually derived from them - useful for confirming sensor names."""
    print("--- Checking LibreHardwareMonitor REST API (localhost:8085) ---")
    tree = _fetch_lhm_tree()
    if tree is None:
        print("  Not reachable.")
        print("  Enable it in LibreHardwareMonitor: Options > Remote Web Server > Run")
    else:
        sensors = []
        _walk_lhm_tree(tree, sensors)
        relevant = [(s, l, v) for s, l, v in sensors
                    if ("cpu" in s.lower() or "gpu" in s.lower() or "ram" in s.lower())
                    and ("/temperature/" in s.lower() or "/load/" in s.lower())]
        if relevant:
            for sid, label, val in relevant:
                print(f"  id={sid!r:38}  name={label!r:25}  value={val}")
        else:
            print("  Connected, but no CPU/GPU/RAM temperature or load sensors found.")

    print("--- Checking LibreHardwareMonitor WMI (fallback, older versions) ---")
    if not HAVE_WMI:
        print("  'wmi' package not installed - run: pip install -r requirements.txt")
    else:
        try:
            w = wmi.WMI(namespace="root\\LibreHardwareMonitor")
            found = False
            for sensor in w.Sensor():
                if sensor.SensorType == "Temperature":
                    found = True
                    print(f"  name={sensor.Name!r:30}  parent={sensor.Parent!r:20}  value={sensor.Value}")
            if not found:
                print("  No temperature sensors found via WMI.")
        except Exception as e:
            print(f"  Not reachable: {e}")

    print("--- Derived values (what actually gets sent to the CYD) ---")
    stats = get_lhm_stats()
    if stats:
        for key, val in stats.items():
            print(f"  {key} = {val}")
    else:
        print("  Nothing derived - none of the sources above returned usable data.")


def main():
    parser = argparse.ArgumentParser(description="CYD PC Dashboard agent")
    parser.add_argument("--port", help="Serial port, e.g. COM5")
    parser.add_argument("--list", action="store_true", help="List serial ports and exit")
    parser.add_argument("--list-sensors", action="store_true",
                         help="List all temperature sensors seen via LibreHardwareMonitor and exit")
    args = parser.parse_args()

    if args.list:
        list_ports()
        return

    if args.list_sensors:
        list_sensors()
        return

    port = args.port or autodetect_port()
    if not port:
        print("Could not auto-detect the CYD's COM port.")
        print("Run with --list to see available ports, then pass --port COMx")
        sys.exit(1)

    print(f"Connecting to {port} at {BAUD_RATE} baud...")
    try:
        ser = serial.Serial(port, BAUD_RATE, timeout=1)
    except serial.SerialException as e:
        print(f"Failed to open {port}: {e}")
        sys.exit(1)

    time.sleep(2)  # let the ESP32 finish resetting after the port opens
    print("Connected. Sending stats every second. Ctrl+C to stop.")

    psutil.cpu_percent(interval=None)              # prime the counter
    psutil.cpu_percent(interval=None, percpu=True)  # prime per-core too
    prev_counters = psutil.net_io_counters()
    prev_time = time.time()
    local_ip = get_local_ip()

    try:
        while True:
            payload, prev_counters, prev_time = build_payload(prev_counters, prev_time, local_ip)
            line = json.dumps(payload) + "\n"
            ser.write(line.encode("utf-8"))
            print(line.strip())
            time.sleep(SEND_INTERVAL_SEC)
    except KeyboardInterrupt:
        print("\nStopping.")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
