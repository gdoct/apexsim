# Things to consider for the car data format in a racing simulator game:
## Visual Data (3D Models & Textures)
### Exterior Model:
High-poly mesh for detailed rendering (for gameplay and cinematics).
Lower-poly meshes for LOD (Level of Detail) to optimize performance when cars are further away.
UV mapping for textures.
### Interior Model:
Detailed cockpit model, including dashboard, steering wheel, seats, shifter, pedals. This is crucial for first-person camera views.
Animations for steering wheel rotation, gear shifter movement, pedal depression.

the exterior and interior models can be one single model though.

### Wheel Models: Separate 3D models for wheels and tires, allowing for rotation and suspension movement.
### Damage Models: Different meshes or morph targets to represent various levels of car damage (dents, scratches, broken parts).
### Liveries/Skins: Texture sets that define the car's paint scheme, decals, and sponsor logos. This should be easily swappable for player customization or different teams.
### Lighting Data: Placement and type of headlights, taillights, brake lights, and turn signals.

## Physics Data (Coefficients & Parameters)
This is where the bulk of your realistic simulation will come from.

### Body & Chassis:
* Mass & Inertia: Total vehicle mass and moments of inertia around all axes.
* Center of Mass: XYZ coordinates relative to the car's origin.
* Drag Coefficient (Cd): How much air resistance the car generates.
* Frontal Area: Used in drag calculations.
* Lift Coefficient (Cl): How much aerodynamic lift (or downforce if negative) the car generates at speed.
* Aero Balance: How downforce is distributed between front and rear.
* Chassis Stiffness: For flex, though this is often simplified.

### Suspension:
* Spring Rates: Stiffness of the springs at each wheel.
* Damper Rates: Rebound and bump damping for shock absorbers.
* Anti-Roll Bar Stiffness: How much resistance to body roll.
* Suspension Travel: Maximum compression and extension.
* Wheel Offsets: Distance from chassis to wheel center.
* Camber, Caster, Toe: Alignment angles for each wheel (static values, and potentially dynamic changes with suspension).

### Tires: (This is one of the most complex and critical areas)
* Grip Coefficients: Lateral and longitudinal friction curves (e.g., Pacejka tire model parameters).
* Tire Stiffness: Radial, lateral, and torsional stiffness.
* Thermal Model: How tire temperature affects grip and wear.
* Wear Model: How tires degrade over time/distance.
* Pressure Sensitivity: How tire pressure affects grip and contact patch.
* Contact Patch Size: The area of the tire touching the ground.

### Engine:
* Torque Curve: Engine torque output at different RPMs.
* Horsepower Curve: Derived from torque.
* Idle / Redline / Rev Limiter: RPM thresholds.
* Engine Braking: Negative torque when off throttle.
* Rotational Inertia & Friction: Affects RPM response and losses.
* Fuel Consumption: Base + load-based model (and later, BSFC-based).

### Transmission/Drivetrain:
* Gear Ratios: For each forward gear and reverse.
* Final Drive Ratio.
* Differential Type & Lock: Open, limited slip (LSD), locked, spool. Parameters for lock strength, preload.
* Clutch Engagement/Slip: For manual transmissions.
* Drivetrain Loss: Efficiency of power transfer.

## TOML: Engine & Drivetrain Simulation Schema

This repo currently supports a minimal `[physics]` section in each `car.toml`. For more realistic powertrain simulation, add these sections. Existing cars without these sections continue to load using defaults.

### [engine]

| Key | Type | Unit | Required | Description |
|---|---:|---:|---:|---|
| `max_power_w` | float | W | no | Peak engine power. If omitted, legacy power is derived from `physics.max_engine_force_n * 100`. |
| `max_torque_nm` | float | N·m | no | Peak torque used for legacy fallback. |
| `idle_rpm` | float | rpm | no | Target idle speed. |
| `redline_rpm` | float | rpm | no | Driver-facing redline (used for normalization). |
| `max_rpm` | float | rpm | no | Absolute maximum RPM (clamp for derived RPM). |
| `rev_limiter_rpm` | float | rpm | no | Limiter threshold where torque cut begins. |
| `inertia_kg_m2` | float | kg·m² | no | Approx. crank+flywheel inertia. |
| `friction_torque_nm` | float | N·m | no | Base friction torque opposing rotation (scaled by RPM fraction). |
| `engine_brake_torque_nm` | float | N·m | no | Extra negative torque at closed throttle (scaled by RPM fraction). |
| `idle_control_gain` | float | unitless | no | Simple idle controller gain (reserved for future use). |

#### Torque curve

If `engine.torque_curve` is provided, physics uses it instead of the legacy parabola.

```toml
[[engine.torque_curve]]
rpm = 1000.0
torque_nm = 240.0
```

### [transmission]

| Key | Type | Unit | Required | Description |
|---|---:|---:|---:|---|
| `transmission_type` | string | - | no | One of: `Manual`, `DCT`, `Sequential`, `Automatic`, `CVT`. |
| `gear_ratios` | array(float) | ratio | no | Hand-typed ladder: reverse first (negative), then forward gears counting down. Use this *or* `ratio_curve`, not both. |
| `final_drive_ratio` | float | ratio | no | Final drive ratio. |
| `shift_time_s` | float | s | no | Shift latency used by higher-level logic (reserved). |
| `efficiency` | float | 0-1 | no | Drivetrain efficiency multiplier applied to wheel torque. |

#### [transmission.ratio_curve]

The preferred way to describe a gearbox: give the two ends and the shape, and
the server generates every ratio in between. Put this table *after* the
`[transmission]` keys — TOML assigns every key below a table header to that
table, so `efficiency = 0.96` written under `[transmission.ratio_curve]` is
silently not a transmission setting.

| Key | Type | Unit | Required | Description |
|---|---:|---:|---:|---|
| `gears` | int | - | yes | Forward gears, 1-12. |
| `first` | float | ratio | yes | First gear ratio (before the final drive). |
| `top` | float | ratio | yes | Top gear ratio; must be lower than `first`. |
| `shape` | string | - | no | `geometric` (default): every shift drops the same fraction of the revs. `progressive`: a long first, steps closing up towards top gear, the way a racing box is laid out. |
| `reverse` | float | ratio | no | Reverse ratio, either sign. Defaults to `first`. |

Pick the two ends from road speeds. At the redline a gear reaches

    speed_kmh = redline_rpm / 60 * 2 * pi * wheel_radius_m * 3.6 / (ratio * final_drive_ratio)

so solve for `first` from how fast first gear should run, and for `top` from
the speed the car can actually reach. Gear top gear a little past that
drag-limited speed, so the engine pulls close to the limiter on a straight.
Much further and every gear is too long: the server warns at load when top
gear runs more than 35% past what the car's power can hold against its drag
(`P = 1/2 * rho * Cd * A * v^3`, hybrid assist included).

```toml
[transmission]
transmission_type = "Sequential"
final_drive_ratio = 3.15
efficiency = 0.96

[transmission.ratio_curve]
gears = 8
first = 4.94    # 120 km/h at 15 000 rpm on 0.33 m wheels
top = 1.80      # 329 km/h; the car drags itself to ~315
shape = "progressive"
```

#### Shift points

Nothing to configure: the automatic gearbox and the AI drivers shift where
the car's `[[engine.torque_curve]]` says to. A gear is kept while it puts more
torque on the road than the next one would at the same road speed, so a
peaky engine runs to the limiter and a torquey one shifts well short of it.
98% of `redline_rpm` is the backstop for an engine whose torque never falls
away. Change the torque curve and the shift points follow.

### [drivetrain]

| Key | Type | Unit | Required | Description |
|---|---:|---:|---:|---|
| `layout` | string | - | no | One of: `RWD`, `FWD`, `AWD`. |

### [differential]

| Key | Type | Unit | Required | Description |
|---|---:|---:|---:|---|
| `differential_type` | string | - | no | One of: `Open`, `Locked`, `ClutchLSD`, `ViscousLSD`, `Torsen`. |
| `preload_nm` | float | N·m | no | Preload for clutch LSD. |
| `lock_power` | float | 0-1 | no | Lock factor on power. |
| `lock_coast` | float | 0-1 | no | Lock factor on coast. |

### [fuel]

| Key | Type | Unit | Required | Description |
|---|---:|---:|---:|---|
| `capacity_liters` | float | L | no | Fuel tank capacity. Synced into telemetry each tick. |
| `idle_consumption_lps` | float | L/s | no | Base consumption at idle. |
| `load_consumption_scale` | float | (L/s) | no | Consumption scaling with throttle and RPM fraction. |

### [hybrid]

Hybrid is currently configuration-only (reserved for future power/regen integration).

| Key | Type | Unit | Required | Description |
|---|---:|---:|---:|---|
| `enabled` | bool | - | no | Enables hybrid system. |
| `battery_capacity_kwh` | float | kWh | no | Battery capacity. |
| `battery_max_discharge_kw` | float | kW | no | Max discharge power. |
| `battery_max_charge_kw` | float | kW | no | Max charge power (regen/charging). |
| `motor_max_torque_nm` | float | N·m | no | Max motor torque. |
| `motor_max_power_kw` | float | kW | no | Max motor power. |
| `regen_max_power_kw` | float | kW | no | Max regen power. |

### Brakes:
* Brake Torque Curve: How much braking force at different pedal pressures/temperatures.
* Brake Bias: Distribution of braking force front to rear.
* Fade Model: How brake performance degrades with heat.
* ABS (Anti-lock Braking System) Parameters: If applicable.

### Steering:
* Steering Ratio: How many degrees the wheel turns for X degrees of road wheel turn.
* Steering Lock: Maximum steering angle.
* Force Feedback Parameters: For realistic force feedback wheel simulation.

### Damage Model Parameters:
* Impact thresholds for different components (suspension, engine, bodywork).
* How damage affects physics (e.g., bent suspension altering alignment, reduced engine power).

## Audio Data
* Engine Sounds: Multiple layers (idle, low RPM, high RPM, redline, rev limiter, turbo whine, blow-off valve).
* Gear Shift Sounds.
* Tire Sounds: Squealing, scrubbing, skidding, flat-spotting).
* Brake Sounds.
* Impact Sounds: For collisions.
* Wind Noise.
* Miscellaneous: Interior rattles, rain on windshield, etc.
## Gameplay & AI Data
### AI Parameters:
  * Aggression levels.
  * Racing lines.
  * Braking points.
  * Reaction times.
  * Skill levels.
### UI/HUD Data:
* Car name, manufacturer, class.
* Performance metrics (top speed, 0-60 time).
* Thumbnail images for car selection.
* Unlock Conditions/Cost: If applicable in a career mode.
* Upgrade Paths: What parts can be upgraded and their effects on physics.

## Data Structure Recommendation
For organizing this, move beyond a single JSON file for all parameters.
Central JSON/XML File per Car: This would hold all the numeric parameters (physics, engine, suspension, etc.). It's text-based and easy to edit.
Asset References: The JSON should contain paths or IDs to refer to external assets:
model_exterior_path: "cars/mycar/exterior.fbx"
model_interior_path: "cars/mycar/interior.fbx"
texture_livery_default: "cars/mycar/livery_default.png"
sound_engine_path: "audio/engines/v8_race.ogg"
Folder Structure: Organize  assets logically:
code
Code
GameRoot/
├── content/
│   ├── cars/
│   │   ├── MyCar/
│   │   │   ├── mycar_data.json
│   │   │   ├── exterior.fbx
│   │   │   ├── interior.fbx
│   │   │   ├── wheel_front.fbx
│   │   │   ├── wheel_rear.fbx
│   │   │   ├── livery_red.png
│   │   │   └── livery_blue.png
│   │   ├── AnotherCar/
│   │   │   └── ...
│   ├── tracks/
│   │   ├── MyTrack/
│   │   │   ├── mytrack_spline.json
│   │   │   ├── overlay.fbx
│   │   │   └── textures/
│   │   └── ...
│   └── audio/
│       ├── engines/
│       │   ├── v8_race.ogg
│       │   └── ...
│       ├── tires/
│       │   ├── skid_high.ogg
│       │   └── ...
│       └── ...
└── data/
    └── global_settings.json
This structure keeps things modular, easy to manage, and scalable as you add more cars and assets to your racing simulator.