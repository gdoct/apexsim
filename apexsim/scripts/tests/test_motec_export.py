"""Lap traces to MoTeC: python -m unittest discover -s scripts/tests -p "test_motec_export.py" """

import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import motec_export  # noqa: E402

HEADER = "distance_m,time_s,speed_kph,throttle,brake,steering,gear,rpm,tyre_fl_c,tyre_fr_c,tyre_rl_c,tyre_rr_c,x,y"


def lap_csv(folder: Path, lap: int, lap_ms: int) -> Path:
    """A lap of a straight 2 km at a steady 200 km/h, a sample every 2 m,
    in fourth gear for the first half and fifth after."""
    lines = [
        f"# apexsim lap trace 1; track=Monzarella; car=posh-gt3rs; lap={lap}; lap_ms={lap_ms}; valid=1; recorded=2026-10-18T12:34:56.000Z",
        HEADER,
    ]
    seconds = lap_ms / 1000.0
    for i in range(1001):
        d = 2.0 * i
        t = seconds * d / 2000.0
        gear = 4 if d < 1000 else 5
        lines.append(f"{d:.1f},{t:.3f},200.0,1.000,0.000,-0.250,{gear},7000,85.0,86.0,90.0,91.0,{d:.2f},0.00")
    path = folder / f"lap{lap}.csv"
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path


class MotecExportTest(unittest.TestCase):
    def test_two_laps_round_trip_with_their_beacons(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            laps = [motec_export.read_lap_csv(lap_csv(folder, n, ms)) for n, ms in ((3, 36_000), (4, 35_000))]
            out = folder / "stint.ld"
            ends = motec_export.write_ld(out, laps, rate=20, driver="Test Driver")
            self.assertAlmostEqual(ends[0], 36.0, places=3)
            self.assertAlmostEqual(ends[1], 71.0, places=3)

            log = motec_export.read_ld(out)
            self.assertEqual((log["venue"], log["vehicle"], log["driver"]), ("Monzarella", "posh-gt3rs", "Test Driver"))
            self.assertEqual((log["date"], log["time"]), ("18/10/2026", "12:34:56"))
            chans = log["channels"]
            self.assertEqual(len(chans), len(motec_export.CHANNELS) + 1)
            speed = chans["Ground Speed"]
            self.assertEqual((speed["freq"], speed["unit"]), (20, "km/h"))
            self.assertEqual(len(speed["data"]), 71 * 20)
            np.testing.assert_allclose(speed["data"], 200.0)
            np.testing.assert_allclose(chans["Throttle Pos"]["data"], 100.0)
            np.testing.assert_allclose(chans["Steering"]["data"], -25.0)
            # Gear is held, never blended: 4 then 5 in each lap.
            self.assertEqual(set(np.unique(chans["Gear"]["data"]).tolist()), {4.0, 5.0})
            # The second lap starts again from the line.
            lap_no = chans["Lap Number"]["data"]
            dist = chans["Lap Distance"]["data"]
            first = int(np.argmax(lap_no == 4))
            self.assertEqual(first, 36 * 20)
            self.assertLess(dist[first], 1.0)
            self.assertGreater(dist[first - 1], 1990.0)

            ldx = out.with_suffix(".ldx").read_text(encoding="utf-8")
            self.assertIn('Time="36000000.00"', ldx)
            self.assertIn('Time="71000000.00"', ldx)
            self.assertIn('<String Id="Fastest Time" Value="0:35.000"/>', ldx)
            self.assertIn('<String Id="Fastest Lap" Value="2"/>', ldx)

    def test_a_file_that_is_not_a_trace_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / "x.csv"
            bad.write_text("a,b\n1,2\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                motec_export.read_lap_csv(bad)


if __name__ == "__main__":
    unittest.main()
