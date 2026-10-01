"""Keyboard control for LiteRadio: W thrust, A/D steer, Space lift.

Adapted from betahovercontrol/src/zorro_control/keyboard.py. Targets, slew rates,
40 ms update period, auto-repeat handling and focus-loss behavior are preserved.
"""
import argparse
import time
import tkinter as tk

from pc_hid import LiteRadioClient


SAFE = (-1.0, 0.0, -1.0)  # CH3, CH4, CH6 as fractions of full mixer range
PERIOD_MS = 40


def approach(current: float, target: float, maximum_step: float) -> float:
    return current + max(-maximum_step, min(maximum_step, target - current))


def targets(pressed: set[str], lift_enabled: bool = False) -> tuple[float, float, float]:
    # Preserve the example's actual target: -40% thrust; idle is -100%.
    thrust = -0.4 if "w" in pressed else -1.0
    # Opposing turn keys cancel each other.
    steer = 0.5 * (int("d" in pressed) - int("a" in pressed))
    # Space toggles lift between -30% and its idle value of -100%.
    lift = -0.3 if lift_enabled else -1.0
    return thrust, steer, lift


def send_state(radio, values):
    thrust, steer, lift = values
    radio.send_control(thrust=thrust*100, steer=steer*100, lift=lift*100)


class TelemetryBar:
    """A labeled numeric bar; fill is a display scale, not a capacity estimate."""
    def __init__(self, parent, title):
        self.canvas = tk.Canvas(parent, width=560, height=62, bg="#182334",
                                highlightthickness=0)
        self.canvas.pack(pady=5)
        self.canvas.create_text(12, 13, text=title, anchor="w", fill="#dce6f3",
                                font=("Segoe UI", 10, "bold"))
        self.value = self.canvas.create_text(548, 13, text="Waiting for telemetry",
                                            anchor="e", fill="#a6b3c5", font=("Segoe UI", 10))
        self.canvas.create_rectangle(12, 34, 548, 50, fill="#304056", outline="")
        self.fill = self.canvas.create_rectangle(12, 34, 12, 50, fill="#66758a", outline="")

    def set(self, fraction, text, color=None):
        fraction = max(0.0, min(1.0, fraction))
        if color is None:
            color = "#ef6461" if fraction < .3 else "#f5b942" if fraction < .7 else "#39cc91"
        self.canvas.coords(self.fill, 12, 34, 12 + 536*fraction, 50)
        self.canvas.itemconfigure(self.fill, fill=color)
        self.canvas.itemconfigure(self.value, text=text, fill=color)


def update_telemetry(bars, radio, now, battery_min, battery_max):
    report = radio.status
    stamp = radio.status_received_at
    fresh = report is not None and stamp is not None and now-stamp <= 1.0
    linked = fresh and report['radio_powered'] and report['link_state'] == 2
    if linked:
        rssi = report['rssi_dbm']
        bars['signal'].set((rssi+120)/80, f"{rssi} dBm")
        lq = max(0, min(100, report['lq']))
        color = "#39cc91" if lq >= 90 else "#f5b942" if lq >= 50 else "#ef6461"
        bars['link'].set(lq/100, f"Connected | LQ {lq}%", color)
    else:
        text = ("Waiting for USB status" if report is None else "USB status stale" if not fresh
                else "Radio powered off" if not report['radio_powered'] else "RF disconnected")
        bars['signal'].set(0, "No live RX signal", "#93a0b2")
        bars['link'].set(0, text, "#93a0b2" if not fresh else "#ef6461")
    arm = report.get('arm_command') if fresh and report['radio_powered'] else None
    if arm is None:
        bars['arm'].set(0, "Unknown / unavailable", "#93a0b2")
    elif arm:
        bars['arm'].set(1, "ARM requested", "#ef6461")
    else:
        bars['arm'].set(0, "DISARM requested", "#39cc91")
    battery = radio.battery
    stamp = radio.battery_received_at
    if battery is None or stamp is None:
        bars['battery'].set(0, "No vehicle battery telemetry", "#93a0b2")
    elif not linked or now-stamp > 5.0:
        bars['battery'].set(0, f"{battery['voltage_v']:.2f} V | stale ({max(0,now-stamp):.0f}s)", "#93a0b2")
    else:
        voltage = battery['voltage_v']
        bars['battery'].set((voltage-battery_min)/(battery_max-battery_min),
                            f"{voltage:.2f} V | {battery['consumed_mah']} mAh used")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", help="USB serial number if multiple radios are attached")
    parser.add_argument("--battery-min", type=float, default=3.0, help="voltage bar lower bound (default 1S: 3.0 V)")
    parser.add_argument("--battery-max", type=float, default=4.35, help="voltage bar upper bound (default 1S: 4.35 V)")
    args = parser.parse_args()
    if not 0 < args.battery_min < args.battery_max < 100:
        parser.error("Require 0 < battery-min < battery-max < 100 V")

    radio = LiteRadioClient(serial=args.serial)
    root = tk.Tk()
    root.title("LiteRadio HID keyboard control")
    root.geometry("600x510")
    root.configure(bg="#101927")
    root.resizable(False, False)
    instructions = tk.Label(root, text="W: forward thrust   A/D: turn   Space: toggle lift\nRelease W/A/D to return to idle. Esc: quit.",
                            font=("Segoe UI", 11), justify="center", bg="#101927", fg="#e8eef7")
    instructions.pack(pady=15)
    status = tk.StringVar(value="Click this window to focus. SC high enables PC control. SB still controls CH5.")
    tk.Label(root, textvariable=status, wraplength=560, justify="center",
             bg="#101927", fg="#cbd8e9", height=3).pack(pady=4)
    bars = {key: TelemetryBar(root, title) for key, title in (
        ('signal', 'RX signal (uplink RSSI)'),
        ('battery', 'Vehicle battery (RX/FC)'),
        ('link', 'RF connection / link quality'),
        ('arm', 'Arm command (SB / CH5)'))}
    tk.Label(root, text=f"Voltage scale: {args.battery_min:g}-{args.battery_max:g} V (not charge %)\n"
                       "TX handset battery is not reported. Arm command is not FC confirmation.",
             bg="#101927", fg="#93a0b2", font=("Segoe UI", 9)).pack(pady=8)

    pressed: set[str] = set()
    pending_release: dict[str, str] = {}
    state = list(SAFE)
    last_tick = time.monotonic()
    next_tick = last_tick
    running = True
    lift_enabled = False

    def on_press(event: tk.Event) -> None:
        nonlocal lift_enabled
        key = event.keysym.lower()
        if key == "escape":
            close()
            return
        if key in ("w", "a", "d", "space"):
            if key in pending_release:
                root.after_cancel(pending_release.pop(key))
            if key == "space" and key not in pressed:
                lift_enabled = not lift_enabled
            pressed.add(key)

    def on_release(event: tk.Event) -> None:
        key = event.keysym.lower()
        if key in ("w", "a", "d", "space"):
            # Ignore synthetic release events generated by keyboard auto-repeat.
            pending_release[key] = root.after(35, lambda k=key: release(k))

    def release(key: str) -> None:
        pending_release.pop(key, None)
        pressed.discard(key)

    def on_focus_out(_event: tk.Event) -> None:
        nonlocal lift_enabled
        pressed.clear()
        for timer in pending_release.values():
            root.after_cancel(timer)
        pending_release.clear()
        lift_enabled = False
        state[:] = SAFE
        try:
            send_state(radio, SAFE)
        except OSError:
            close()

    def close() -> None:
        nonlocal running
        if not running:
            return
        running = False
        try:
            send_state(radio, SAFE)
        except OSError:
            pass
        finally:
            try:
                radio.close()
            except OSError:
                pass
            finally:
                root.destroy()

    def tick() -> None:
        nonlocal last_tick, next_tick
        if not running:
            return
        now = time.monotonic()
        dt = min(0.1, max(0.0, now - last_tick))
        last_tick = now
        desired = targets(pressed, lift_enabled)
        # Slew limits filter keyboard steps: thrust 120%/s, turn 225%/s, lift 150%/s.
        rates = (1.2, 2.25, 1.5)
        for index in range(3):
            state[index] = approach(state[index], desired[index], rates[index] * dt)
        try:
            send_state(radio, state)
            report = radio.read_telemetry()
        except OSError as exc:
            status.set(f"USB connection lost: {exc}")
            close()
            return
        line = (f"Thrust {state[0]*100:+.0f}%  Steer {state[1]*100:+.0f}%  Lift {state[2]*100:+.0f}%")
        if report:
            line += f"\nControl: {report['state']} | PC permitted: {report['permitted']}"
            if report['locked']:
                line += "\nCycle SC out of high and back to enable PC control."
        update_telemetry(bars, radio, now, args.battery_min, args.battery_max)
        status.set(line)
        next_tick += PERIOD_MS / 1000
        if next_tick < time.monotonic():
            next_tick = time.monotonic()
        root.after(max(0, round((next_tick - time.monotonic()) * 1000)), tick)

    root.bind("<KeyPress>", on_press)
    root.bind("<KeyRelease>", on_release)
    root.bind("<FocusOut>", on_focus_out)
    root.protocol("WM_DELETE_WINDOW", close)
    root.after(0, tick)
    root.mainloop()


if __name__ == "__main__":
    main()
