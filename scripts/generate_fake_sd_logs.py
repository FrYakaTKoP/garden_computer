"""Create plausible, CRC-protected SD log fixtures for browser history testing."""

from __future__ import annotations

import json
import math
import random
import zlib
from datetime import datetime, timedelta
from pathlib import Path


OUTPUT_DIRECTORY = Path(__file__).resolve().parents[1] / "test-data" / "fake-sd-logs"
DATES = (datetime(2026, 8, 5), datetime(2026, 8, 6), datetime(2026, 8, 7))
INTERVAL = timedelta(minutes=10)


def cloud_factor(hour: float, clouds: tuple[tuple[float, float, float], ...]) -> float:
    factor = 1.0
    for center, width, depth in clouds:
        distance = abs(hour - center)
        if distance < width:
            factor *= 1.0 - depth * (1.0 - distance / width)
    return factor


def make_record(moment: datetime, day_index: int, energy_wh: float, monthly_wh: float) -> tuple[dict, float]:
    randomizer = random.Random(f"{moment:%Y%m%d%H%M}")
    hour = moment.hour + moment.minute / 60
    clouds_by_day = (
        ((11.7, 0.28, 0.62), (14.35, 0.50, 0.44), (16.15, 0.22, 0.70)),
        ((10.9, 0.42, 0.35), (13.10, 0.18, 0.76), (15.45, 0.65, 0.30)),
        ((12.25, 0.32, 0.56), (13.85, 0.23, 0.80), (16.70, 0.40, 0.40)),
    )
    daylight = max(0.0, math.sin(math.pi * (hour - 6.0) / 14.0))
    solar_w = min(300.0, 300.0 * daylight ** 1.45 * cloud_factor(hour, clouds_by_day[day_index]))
    solar_w = min(300.0, max(0.0, solar_w + randomizer.uniform(-4.0, 4.0)))
    load_w = 7.0 + randomizer.uniform(-1.2, 1.2)
    if 6.0 <= hour < 6.5 or 19.0 <= hour < 19.5:
        load_w += 25.0

    pv_voltage = 0.0 if solar_w < 1 else 20.5 + solar_w / 38.0 + randomizer.uniform(-0.25, 0.25)
    pv_current = 0.0 if pv_voltage == 0 else solar_w / pv_voltage
    charge_progress = max(0.0, min(1.0, (hour - 7.0) / 8.0))
    battery_v = 12.25 + 2.0 * charge_progress + (0.12 if solar_w > 35 else 0.0) + randomizer.uniform(-0.04, 0.04)
    if hour > 18:
        battery_v = 12.85 - 0.04 * (hour - 18) + randomizer.uniform(-0.04, 0.04)
    battery_current = (solar_w - load_w) / max(12.0, battery_v)
    battery_soc = max(55, min(100, round(60 + charge_progress * 38 - max(0, hour - 19) * 1.6)))
    load_voltage = battery_v - 0.05
    load_current = load_w / load_voltage
    energy_wh += solar_w / 6.0

    record = {
        "v": 1,
        "t": int(moment.strftime("%Y%m%d%H%M%S")),
        "tv": 1,
        "p": [round(pv_voltage * 100), round(pv_current * 100), round(solar_w * 100)],
        "b": [round(battery_v * 100), round(battery_current * 100), round(battery_v * battery_current * 100), battery_soc, round((23 + daylight * 7) * 10)],
        "l": [round(load_voltage * 100), round(load_current * 100), round(load_w * 100)],
        "k": 0,
        "u": 0,
        "w": 1,
    }
    if moment.hour == 23 and moment.minute == 50:
        record["e"] = [round(energy_wh * 10), round((monthly_wh + energy_wh) * 10), round((452100 + monthly_wh + energy_wh) * 10)]
    return record, energy_wh


def write_day(day: datetime, day_index: int, monthly_wh: float) -> float:
    energy_wh = 0.0
    lines: list[str] = []
    moment = day
    for _ in range(144):
        record, energy_wh = make_record(moment, day_index, energy_wh, monthly_wh)
        body = json.dumps(record, separators=(",", ":"))
        record["crc"] = f"{zlib.crc32(body.encode()):08X}"
        lines.append(json.dumps(record, separators=(",", ":")))
        moment += INTERVAL
    OUTPUT_DIRECTORY.mkdir(parents=True, exist_ok=True)
    (OUTPUT_DIRECTORY / f"{day:%Y%m%d}.ndjson").write_text("\n".join(lines) + "\n", encoding="ascii")
    return energy_wh


def main() -> None:
    monthly_wh = 18320.0
    for index, day in enumerate(DATES):
        monthly_wh += write_day(day, index, monthly_wh)
    print(f"Wrote {len(DATES)} daily logs to {OUTPUT_DIRECTORY}")


if __name__ == "__main__":
    main()