# 🔊 **Feature: Procedural Engine Synthesis (No Samples)**  
*A deterministic, real‑time engine audio system based on combustion‑pulse synthesis and modular DSP nodes.*

---

# 1. **Overview**

The engine audio subsystem generates all engine‑related sounds procedurally, without relying on prerecorded samples.  
The system models the engine as a periodic sequence of combustion events feeding into resonant intake/exhaust structures, augmented by mechanical and airflow components.

The architecture is fully modular: each processing stage is a node with defined inputs, outputs, and tunable parameters. Engine types (I4, V6, V8, V12, turbocharged, etc.) are implemented by swapping node configurations, not rewriting logic.

---

# 2. **Core Model: Combustion Pulse Synthesis**

## 2.1 Firing Frequency
For an engine with *N* cylinders:

\[
f_\text{fire} = \frac{RPM}{60} \cdot \frac{N}{2}
\]

This frequency determines the timing of combustion pulses.

## 2.2 Combustion Pulse Generator

**Input:**  
- Firing frequency  
- Throttle (0–1)  
- Engine load (0–1)  
- Cylinder phase offsets (per engine type)

**Process:**  
- Generate a short noise‑based pulse (2–10 ms)  
- Shape with a fixed envelope (attack/decay)  
- Apply per‑cylinder phase offsets  
- Introduce micro‑jitter (±0.5–1.5%) to avoid tonal aliasing

**Output:**  
- Raw combustion signal (broadband, unfiltered)

---

# 3. **Harmonic Shaper Bank**

The raw combustion signal is harmonically sparse. This node enriches it.

## 3.1 Additive Harmonics
- 8–20 sine oscillators  
- Fundamental = \( f_0 = \frac{RPM}{60} \)  
- Harmonic amplitudes defined by engine‑type curves  
- Throttle modulates harmonic brightness

## 3.2 Waveshaping
- Sawtooth or pulse wave → nonlinear waveshaper  
- Drive increases with throttle  
- Shape table selectable per engine type

**Output:**  
- Tonal engine body

---

# 4. **Intake Path**

**Input:**  
- Combustion signal  
- Throttle  
- RPM

**Process:**  
1. Bandpass filter  
   - Center frequency increases with RPM  
   - Q increases with throttle  
2. Intake airflow noise  
   - White noise → envelope follower tied to throttle  
   - Mixed post‑filter

**Output:**  
- Intake layer (breathing, hiss, resonance)

---

# 5. **Exhaust Path**

**Input:**  
- Combustion signal  
- Load  
- RPM

**Process:**  
1. Low‑pass filter (muffler simulation)  
2. Bandpass filter (pipe resonance)  
   - Resonant frequency shifts slightly with RPM  
3. Distortion stage  
   - Soft clipper or waveshaper  
   - Drive = function(load)  
4. Exhaust noise  
   - Low‑passed noise  
   - Adds rumble and texture

**Output:**  
- Exhaust layer (character, aggression)

---

# 6. **Mechanical Layer**

Independent of combustion.

**Components:**  
- Valve ticks (short high‑frequency bursts)  
- Gear whine (harmonic series tied to gear ratio)  
- Alternator whine (optional)  
- Turbo spool (filtered noise with rising pitch)  
- Wastegate (short noise burst on throttle lift)

**Output:**  
- Mechanical layer (non‑combustion engine detail)

---

# 7. **Mixer**

All layers are combined with deterministic gain rules.

**Mixing Logic:**  
- Intake gain ∝ throttle  
- Exhaust gain ∝ load  
- Mechanical gain ∝ inverse RPM (stronger at idle)  
- Harmonic bank crossfades based on engine type and RPM  
- Optional limiter at output

**Output:**  
- Final engine signal (pre‑spatialization)

---

# 8. **Spatialization**

Applies world‑space effects.

**Process:**  
- Doppler shift (vehicle velocity vs listener)  
- Distance‑based low‑pass  
- Occlusion (cockpit, walls, terrain)  
- Stereo/5.1/7.1 panning

**Output:**  
- Final rendered engine audio

---

# 9. **Node Graph (Formal)**

```
[Physics Inputs]
    |
    v
[Firing Pulse Generator] ---> [Harmonic Shaper Bank]
    |                                 |
    v                                 v
[Intake Path] ------------------> [Mixer] <--- [Mechanical Layer]
    |                                 |
    v                                 v
[Exhaust Path] ------------------------+
    |
    v
[Spatialization]
    |
    v
[Audio Out]
```

---

# 10. **Why This Architecture Works**

- Deterministic: all audio is derived from physics inputs  
- Modular: engine types = configuration, not code changes  
- Lightweight: suitable for real‑time simulation  
- Scalable: supports simple engines (I4) to complex (V12 twin‑turbo)  
- Extensible: new nodes can be added without breaking the graph  

---

# 📘 **11. Parameter Table (Per DSP Node)**

We need to enrich the cars with these parameters so the audio system can read them. We should have reasonable defaults for most parameters.

## **11.1 Physics Input Layer**
| Parameter | Type | Range | Description |
|----------|------|--------|-------------|
| `rpm` | float | 0–12000 | Current engine RPM |
| `throttle` | float | 0–1 | Driver throttle input |
| `load` | float | 0–1 | Engine load (torque %) |
| `gear_ratio` | float | >0 | Current gear ratio |
| `vehicle_speed` | float | m/s | For Doppler |
| `listener_pos` | Vector3 | — | Spatialization input |
| `engine_pos` | Vector3 | — | Spatialization input |

Derived:
| Derived Parameter | Formula |
|------------------|---------|
| `f0` | `rpm / 60` |
| `f_fire` | `f0 * (cylinders / 2)` |

---

## **11.2 Firing Pulse Generator**
| Parameter | Type | Range | Description |
|----------|------|--------|-------------|
| `pulse_width` | ms | 2–10 | Duration of combustion pulse |
| `pulse_env_attack` | ms | 0.1–1 | Envelope attack |
| `pulse_env_decay` | ms | 1–5 | Envelope decay |
| `pulse_noise_amount` | float | 0–1 | Broadband noise content |
| `phase_offsets[]` | float array | 0–1 | Per‑cylinder phase |
| `jitter_amount` | float | 0–0.02 | Random pitch jitter |

Output: raw combustion signal.

---

## **11.3 Harmonic Shaper Bank**
| Parameter | Type | Range | Description |
|----------|------|--------|-------------|
| `harmonic_count` | int | 8–20 | Number of harmonics |
| `harmonic_gain[n]` | float | 0–1 | Per‑harmonic amplitude |
| `waveshaper_drive` | float | 0–1 | Nonlinear shaping amount |
| `waveshaper_curve` | enum | soft, hard, custom | Distortion curve |

Output: tonal engine body.

---

## **11.4 Intake Path**
| Parameter | Type | Range | Description |
|----------|------|--------|-------------|
| `bp_center_freq` | Hz | 200–3000 | Intake resonance |
| `bp_q` | float | 0.5–5 | Resonance sharpness |
| `intake_noise_gain` | float | 0–1 | Throttle‑dependent noise |

---

## **11.5 Exhaust Path**
| Parameter | Type | Range | Description |
|----------|------|--------|-------------|
| `lp_cutoff` | Hz | 200–8000 | Muffler filtering |
| `bp_center_freq` | Hz | 80–400 | Pipe resonance |
| `bp_q` | float | 0.5–3 | Resonance sharpness |
| `distortion_drive` | float | 0–1 | Load‑dependent distortion |
| `exhaust_noise_gain` | float | 0–1 | Broadband rumble |

---

## **11.6 Mechanical Layer**
| Component | Parameters |
|----------|------------|
| Valve ticks | tick_rate, tick_gain |
| Gear whine | base_freq, harmonic_count, gain |
| Turbo spool | noise_gain, pitch_curve, attack/decay |
| Wastegate | burst_length, burst_gain |

---

## **11.7 Mixer**
| Parameter | Type | Description |
|----------|------|-------------|
| `intake_gain_curve` | curve | throttle → gain |
| `exhaust_gain_curve` | curve | load → gain |
| `mechanical_gain_curve` | curve | rpm → gain |
| `limiter_threshold` | float | Output limiter |

---

## **11.8 Spatialization**
| Parameter | Type | Description |
|----------|------|-------------|
| `doppler_factor` | float | Speed‑based pitch shift |
| `distance_lpf_curve` | curve | Distance → LPF cutoff |
| `occlusion_amount` | float | Cockpit/walls |

---

# 🧩 **12. Runtime Update Loop (Godot‑Style Pseudocode)**

This is written in a Godot‑friendly style (GDScript‑like), but engine‑agnostic.

```gdscript
func _process(delta):
    # 1. Read physics
    rpm = engine.get_rpm()
    throttle = input.get_throttle()
    load = engine.get_load()
    gear_ratio = transmission.get_gear_ratio()
    vehicle_speed = car.get_speed()

    # 2. Derived frequencies
    f0 = rpm / 60.0
    f_fire = f0 * (cylinders / 2.0)

    # 3. Update firing pulse generator
    pulse_signal = firing_pulse.update(
        f_fire,
        throttle,
        load,
        delta
    )

    # 4. Harmonic shaper
    harmonic_signal = harmonic_bank.update(
        f0,
        throttle,
        load,
        delta
    )

    # 5. Intake path
    intake_signal = intake_path.process(
        pulse_signal,
        throttle,
        rpm,
        delta
    )

    # 6. Exhaust path
    exhaust_signal = exhaust_path.process(
        pulse_signal,
        load,
        rpm,
        delta
    )

    # 7. Mechanical layer
    mechanical_signal = mechanical_layer.update(
        rpm,
        throttle,
        gear_ratio,
        delta
    )

    # 8. Mix
    engine_signal = mixer.mix(
        harmonic_signal,
        intake_signal,
        exhaust_signal,
        mechanical_signal,
        rpm,
        throttle,
        load
    )

    # 9. Spatialization
    final_signal = spatializer.process(
        engine_signal,
        vehicle_speed,
        listener_pos,
        engine_pos
    )

    # 10. Output
    audio_server.push(final_signal)
```

This loop is deterministic and runs per audio frame or per physics frame with interpolation.

---

# 🏎️ **13. Engine‑Type Preset Sheet**

These presets define the *character* of each engine type.  
we might need to extend the car toml system to include engine type so the audio system can pick the right preset.
---

## **13.1 Inline‑4 (Naturally Aspirated)**

**Combustion**
- pulse_width: 3 ms  
- jitter: 0.5%  
- noise_amount: medium  

**Harmonics**
- harmonic_count: 12  
- harmonic_gain: strong 2nd/4th, weak odd harmonics  
- waveshaper_drive: low  

**Intake**
- bp_center_freq: 800 → 2200 Hz (RPM‑mapped)  
- intake_noise_gain: high at >60% throttle  

**Exhaust**
- lp_cutoff: 3000 Hz  
- bp_center_freq: 120 Hz  
- distortion_drive: low  

**Mechanical**
- gear whine: medium  
- valve ticks: audible at idle  

---

## **13.2 V6 (Sport)**

**Combustion**
- pulse_width: 4 ms  
- jitter: 1%  
- noise_amount: medium‑high  

**Harmonics**
- harmonic_count: 16  
- strong odd harmonics  
- waveshaper_drive: medium  

**Intake**
- bp_center_freq: 600 → 2600 Hz  
- intake_noise_gain: medium  

**Exhaust**
- lp_cutoff: 4500 Hz  
- bp_center_freq: 150 Hz  
- distortion_drive: medium  

**Mechanical**
- gear whine: low  
- valve ticks: subtle  

---

## **13.3 V8 (Muscle / GT)**

**Combustion**
- pulse_width: 5 ms  
- jitter: 1.2%  
- noise_amount: high  

**Harmonics**
- harmonic_count: 20  
- strong 1st, 2nd, 4th, 8th  
- waveshaper_drive: high  

**Intake**
- bp_center_freq: 500 → 2000 Hz  
- intake_noise_gain: medium  

**Exhaust**
- lp_cutoff: 2500 Hz  
- bp_center_freq: 90 Hz  
- distortion_drive: high  

**Mechanical**
- gear whine: low  
- valve ticks: low  

---

## **13.4 V12 (Exotic)**

**Combustion**
- pulse_width: 3 ms  
- jitter: 0.3%  
- noise_amount: low  

**Harmonics**
- harmonic_count: 20  
- very strong even harmonics  
- waveshaper_drive: low  

**Intake**
- bp_center_freq: 1000 → 3500 Hz  
- intake_noise_gain: low  

**Exhaust**
- lp_cutoff: 6000 Hz  
- bp_center_freq: 180 Hz  
- distortion_drive: low  

**Mechanical**
- gear whine: medium  
- valve ticks: minimal  

---

## **13.5 Turbocharged I4**

**Combustion**
- pulse_width: 3 ms  
- jitter: 0.8%  
- noise_amount: medium  

**Harmonics**
- harmonic_count: 12  
- waveshaper_drive: medium  

**Intake**
- bp_center_freq: 700 → 2000 Hz  
- intake_noise_gain: medium  

**Exhaust**
- lp_cutoff: 3500 Hz  
- bp_center_freq: 110 Hz  
- distortion_drive: medium  

**Mechanical**
- turbo spool: high  
- wastegate: strong  
- gear whine: medium  

---

# Mulripe cars and audio suorces
Each car runs its own engine‑sound DSP graph, and the output of each graph is then spatialized and mixed into the world audio.

Think of it like this:

```Code
Car A → Engine DSP → Spatializer → World Mixer
Car B → Engine DSP → Spatializer → World Mixer
Car C → Engine DSP → Spatializer → World Mixer
```
Every car is its own audio source with its own physics inputs.

🎮 How It Works in Practice (Step‑by‑Step)
1. Each car has its own audio node
In Godot terms:

Each car instance has:

- EngineAudioProcessor (the DSP graph)
- AudioStreamPlayer3D (or custom audio bus)
- A transform in world space

So if there are 20 cars, we have 20 independent DSP graphs.

2. Each DSP graph receives that car’s physics
For each car:
- RPM
- Throttle
- Load
- Gear
- Turbo state
- Position
- Velocity

These feed into the DSP graph only for that car.

3. Each car produces its own mono engine signal
The DSP graph outputs a mono audio stream.
This is important — spatialization works best with mono sources.

4. The mono signal is fed into a 3D audio node
Godot’s AudioStreamPlayer3D handles:
- Distance attenuation
- Doppler shift
- Panning
- Occlusion
- Reverb sends

We already have the spatialization node in our DSP graph.