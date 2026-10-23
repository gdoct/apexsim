"""Render the radio's voice lines: audio/radio/<id>.wav from audio/radio/lines.yml.

    python tools/generators/radio/build_radio_lines.py              # every line
    python tools/generators/radio/build_radio_lines.py car_left p1  # a few (ids)

Run from apexsim-content/ on Windows. The speech is the local Windows voice
(System.Speech through PowerShell, offline: no service, no model download),
one voice per channel as lines.yml names them. Each take then goes through a
radio: trimmed, band-passed to a 350 Hz - 3 kHz voice channel, compressed and
lightly driven, with a faint hiss under it and a squelch burst as the key
opens and closes. Written 16 kHz mono 16-bit, about 30 KB a second.

The hiss and squelch are seeded from the line's id, so the same voice renders
the same bytes every run. The client plays the files by id
(apexsim/game-unreal ApexRadio, apexsim/docs/game/audio.md "Radio").
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
import wave
import zlib
from pathlib import Path

import numpy as np
import yaml
from scipy import signal

ROOT = Path(__file__).resolve().parents[3]
RADIO_DIR = ROOT / "audio" / "radio"
LINES_FILE = RADIO_DIR / "lines.yml"

SAMPLE_RATE = 16000
# SAPI speaking rate (-10..10): the spotter talks fast, the engineer a little less so.
SPEECH_RATE = {"spotter": 2, "engineer": 1}

BAND_HZ = (350.0, 3000.0)
SILENCE_DB = -45.0
PAD_S = 0.03
COMP_THRESHOLD_DB = -18.0
COMP_RATIO = 4.0
DRIVE = 1.8
HISS_DB = -42.0
SQUELCH_OPEN_S = 0.05
SQUELCH_CLOSE_S = 0.09
SQUELCH_DB = -20.0
PEAK_DB = -1.0


def db(value: float) -> float:
    return 10.0 ** (value / 20.0)


def speak_all(lines: dict, voices: dict, out_dir: Path) -> None:
    """One PowerShell run renders every take to <id>.raw.wav in out_dir."""
    script = [
        "Add-Type -AssemblyName System.Speech",
        "$s = New-Object System.Speech.Synthesis.SpeechSynthesizer",
        "$fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo("
        f"{SAMPLE_RATE}, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, "
        "[System.Speech.AudioFormat.AudioChannel]::Mono)",
    ]
    for line_id, line in lines.items():
        channel = line["channel"]
        text = line["text"].replace("'", "''")
        path = str(out_dir / f"{line_id}.raw.wav").replace("'", "''")
        script += [
            f"$s.SelectVoice('{voices[channel]}')",
            f"$s.Rate = {SPEECH_RATE[channel]}",
            f"$s.SetOutputToWaveFile('{path}', $fmt)",
            f"$s.Speak('{text}')",
            "$s.SetOutputToNull()",
        ]
    script_path = out_dir / "speak.ps1"
    script_path.write_text("\n".join(script), encoding="utf-8-sig")
    subprocess.run(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script_path)],
        check=True,
    )


def read_wav(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as f:
        assert f.getsampwidth() == 2 and f.getnchannels() == 1, path
        assert f.getframerate() == SAMPLE_RATE, path
        data = np.frombuffer(f.readframes(f.getnframes()), dtype="<i2")
    return data.astype(np.float64) / 32768.0


def write_wav(path: Path, samples: np.ndarray) -> None:
    pcm = np.clip(np.round(samples * 32767.0), -32768, 32767).astype("<i2")
    with wave.open(str(path), "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(SAMPLE_RATE)
        f.writeframes(pcm.tobytes())


def trim(x: np.ndarray) -> np.ndarray:
    loud = np.flatnonzero(np.abs(x) > db(SILENCE_DB))
    if loud.size == 0:
        return x
    pad = int(PAD_S * SAMPLE_RATE)
    return x[max(0, loud[0] - pad): min(x.size, loud[-1] + pad)]


def compress(x: np.ndarray) -> np.ndarray:
    """A feed-forward compressor on a peak envelope (5 ms attack, 80 ms release)."""
    attack = np.exp(-1.0 / (0.005 * SAMPLE_RATE))
    release = np.exp(-1.0 / (0.080 * SAMPLE_RATE))
    env = np.empty_like(x)
    level = 0.0
    for i, v in enumerate(np.abs(x)):
        coeff = attack if v > level else release
        level = coeff * level + (1.0 - coeff) * v
        env[i] = level
    threshold = db(COMP_THRESHOLD_DB)
    gain = np.ones_like(x)
    over = env > threshold
    gain[over] = (env[over] / threshold) ** (1.0 / COMP_RATIO - 1.0)
    return x * gain


def radio(x: np.ndarray, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    band = signal.butter(4, BAND_HZ, btype="bandpass", fs=SAMPLE_RATE, output="sos")

    voice = trim(x)
    voice = voice / max(1e-9, np.max(np.abs(voice)))
    voice = signal.sosfilt(band, voice)
    voice = compress(voice / max(1e-9, np.max(np.abs(voice))))
    voice = np.tanh(DRIVE * voice / max(1e-9, np.max(np.abs(voice)))) / np.tanh(DRIVE)

    open_n = int(SQUELCH_OPEN_S * SAMPLE_RATE)
    close_n = int(SQUELCH_CLOSE_S * SAMPLE_RATE)
    total = open_n + voice.size + close_n
    out = np.zeros(total)
    out[open_n: open_n + voice.size] = voice

    hiss = signal.sosfilt(band, rng.standard_normal(total))
    hiss *= db(HISS_DB) / max(1e-9, np.sqrt(np.mean(hiss ** 2)))
    out += hiss

    # The key: a burst of band-limited noise as it opens, a longer one
    # falling away as it closes.
    burst = signal.sosfilt(band, rng.standard_normal(open_n + close_n))
    burst *= db(SQUELCH_DB) / max(1e-9, np.sqrt(np.mean(burst ** 2)))
    out[:open_n] += burst[:open_n] * np.linspace(1.0, 0.3, open_n)
    out[-close_n:] += burst[open_n:] * np.linspace(1.0, 0.0, close_n) ** 2

    # No click at either end.
    fade = int(0.003 * SAMPLE_RATE)
    out[:fade] *= np.linspace(0.0, 1.0, fade)
    out[-fade:] *= np.linspace(1.0, 0.0, fade)
    return out * (db(PEAK_DB) / max(1e-9, np.max(np.abs(out))))


def main(argv: list[str]) -> int:
    spec = yaml.safe_load(LINES_FILE.read_text(encoding="utf-8"))
    voices = spec["voices"]
    lines = spec["lines"]
    if argv:
        unknown = [a for a in argv if a not in lines]
        if unknown:
            print(f"unknown line id(s): {', '.join(unknown)}", file=sys.stderr)
            return 1
        lines = {k: v for k, v in lines.items() if k in argv}

    with tempfile.TemporaryDirectory(prefix="apex_radio_") as tmp:
        tmp_dir = Path(tmp)
        speak_all(lines, voices, tmp_dir)
        total_bytes = 0
        for line_id in lines:
            raw = read_wav(tmp_dir / f"{line_id}.raw.wav")
            out_path = RADIO_DIR / f"{line_id}.wav"
            write_wav(out_path, radio(raw, zlib.crc32(line_id.encode("utf-8"))))
            total_bytes += out_path.stat().st_size
            print(f"{out_path.relative_to(ROOT)}  {out_path.stat().st_size // 1024} KB")
    print(f"{len(lines)} line(s), {total_bytes // 1024} KB")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
