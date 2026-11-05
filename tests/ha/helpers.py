"""Shared helpers for the HA package tests."""

import asyncio


async def settle(hass):
    """block_till_done, plus the loop.call_soon hop the event trigger platform uses."""
    for _ in range(3):
        await hass.async_block_till_done()
        await asyncio.sleep(0)


def make_config(swapped=False):
    zones = []
    for n in range(1, 17):
        out = f"G{n}"
        if n == 1:
            out = "B" if swapped else "A"
        if n == 2:
            out = "A" if swapped else "B"
        zones.append({"zone": n, "name": f"Zone {n}", "enabled": True, "head": 1,
                      "precip": 25.5, "soil": 2, "slope": 3, "sun": 1, "plant": 2,
                      "area": 12.25, "max_cycle_min": 10, "soak_min": 5,
                      "baseline": 8.5, "high_pct": 150, "low_pct": 50, "action": 1,
                      "zone_output": out})
    programs = []
    for n in range(8):
        programs.append({"program": n, "name": f"Prog {n}", "enabled": n % 2 == 0,
                         "mode": 0, "weekday_mask": 127, "interval_days": 2,
                         "days_until_first": 0, "starts": [360, 0, 0, 0],
                         "zone_minutes": [n + 1] * 16, "seasonal_pct": 100,
                         "cycle_soak": True, "weather_adjust": False})
    settings = {"zone_count": 16, "controller_enabled": True, "monthly_pct": [100] * 12,
                "pulses_per_litre": 450.0, "leak_lpm": 1.5, "settle_s": 20,
                "rain_type": 0, "mm_per_tip": 0.2, "rain_skip_mm": 3.0,
                "rain_stops": True, "hard_cap_min": 60, "start_short_fn": 1,
                "start_long_fn": 2, "stop_short_fn": 3, "stop_long_fn": 4,
                "button_program": 0, "master_output": "M1", "relay_active_high": False}
    return {"programs": programs, "zones": zones, "settings": settings}
