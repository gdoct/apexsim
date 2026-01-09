# Procedural World Generation Specification  
**Version:** 1.0  
**Author:** Guido  
**Purpose:** Add procedural terrain, environment dressing, and trackside decals to tracks defined by YAML spline files.

---

# 1. Overview  
This feature adds a **procedurally generated world** around each track. The system takes:

- A centerline spline with widths  
- A raceline  
- A new `environment_type` field (desert, forest, city, etc.)

…and produces:

- Sculpted terrain with elevation  
- Track corridor blended into terrain  
- Environment‑specific vegetation, rocks, buildings  
- Trackside decals (kerbs, skid marks, rubber buildup, grass edges, gravel traps)  
- Optional export to Blender for manual sculpting

The system must be deterministic given a seed.

---

# 2. Required Additions to Track Metadata  
Extend your YAML track format with the following fields:

```yaml
metadata:
  environment_type: "desert"     # desert | forest | city | mountains | plains
  terrain_seed: 12345            # optional, overrides global seed
  terrain_scale: 1.0             # global multiplier for terrain height
  terrain_detail: 0.5            # noise frequency multiplier
  terrain_blend_width: 20.0      # meters to blend track into terrain
  object_density: 0.8            # 0–1, controls vegetation/building density
  decal_profile: "default"       # which decal set to use
```

### Optional per‑node overrides  
Nodes may optionally include:

```yaml
- x: ...
  y: ...
  z: 0.0
  banking: 0.0
  elevation_override: null       # float, forces terrain height under track
  terrain_mask: null             # "no_trees", "no_rocks", "no_buildings"
```

---

# 3. System Architecture  
The world generator consists of **five modules**, each independent and testable.

## 3.1 Terrain Generator  
Responsible for generating a heightmap or mesh around the track.

### Inputs  
- Track nodes  
- `environment_type`  
- Noise parameters  
- Blend width  
- Seed  

### Steps  
1. Generate base terrain using layered noise:  
   - Low‑frequency base shape  
   - Mid‑frequency detail  
   - High‑frequency micro‑detail  
2. Apply environment presets:  
   - Desert → dunes, wind ripples  
   - Forest → rolling hills  
   - City → mostly flat with noise suppressed  
3. Carve track corridor:  
   - Flatten terrain under track  
   - Smooth falloff using `terrain_blend_width`  
4. Apply elevation overrides (if any)

### Output  
- Terrain heightmap or mesh  
- Terrain normal map (optional)

---

## 3.2 Track Sculpting  
This module modifies the track mesh itself.

### Responsibilities  
- Apply elevation from terrain to track nodes  
- Smooth elevation transitions  
- Apply banking  
- Generate cross‑section mesh  
- Generate shoulders (grass, gravel, asphalt)

### Output  
- Final track mesh  
- Track shoulder meshes

---

## 3.3 Environment Object Placement  
Places vegetation, rocks, buildings, props.

### Inputs  
- Terrain mesh  
- Track corridor mask  
- `object_density`  
- `environment_type`

### Rules  
- No objects within X meters of track edge  
- Use biome‑specific object sets  
- Use Poisson disk sampling for natural spacing  
- Align objects to terrain normals  
- Apply per‑node `terrain_mask` overrides

### Examples  
**Desert:**  
- Rocks, dunes, dry bushes, sand patches  

**Forest:**  
- Trees, shrubs, grass, logs, stumps  

**City:**  
- Buildings, fences, streetlights, barriers  

---

## 3.4 Trackside Decal System  
Adds visual detail along the track.

### Decal Types  
- Kerbs (based on curvature)  
- Skid marks (based on raceline curvature + braking zones)  
- Rubber buildup (racing line)  
- Grass edge decals  
- Gravel trap decals  
- Painted lines  
- Start/finish grid  

### Inputs  
- Raceline  
- Track curvature  
- Surface type  
- `decal_profile`

### Outputs  
- Decal mesh instances  
- Decal UVs  
- Decal material assignments

---

## 3.5 Export/Import Pipeline (Optional)  
Allows exporting terrain + track to Blender for sculpting.

### Export  
- Track mesh  
- Terrain mesh  
- Object placement markers  

### Import  
- Modified terrain mesh  
- Modified object placements  

---

# 4. Code Modules to Implement  

## 4.1 `TerrainGenerator`  
```cpp
class TerrainGenerator {
public:
    TerrainGenerator(const TrackData& track, const TerrainSettings& settings);
    TerrainMesh generate();
private:
    float sampleNoise(float x, float y);
    void carveTrackCorridor(TerrainMesh& mesh);
};
```

---

## 4.2 `TrackSculptor`  
```cpp
class TrackSculptor {
public:
    TrackMesh sculpt(const TrackData& track, const TerrainMesh& terrain);
private:
    void applyElevation(TrackMesh& mesh, const TerrainMesh& terrain);
    void applyBanking(TrackMesh& mesh);
};
```

---

## 4.3 `EnvironmentSpawner`  
```cpp
class EnvironmentSpawner {
public:
    std::vector<WorldObject> spawn(const TerrainMesh& terrain,
                                   const TrackMesh& track,
                                   const EnvironmentSettings& env);
private:
    void spawnVegetation();
    void spawnRocks();
    void spawnBuildings();
};
```

---

## 4.4 `DecalGenerator`  
```cpp
class DecalGenerator {
public:
    std::vector<Decal> generate(const TrackMesh& track,
                                const RaceLine& raceline,
                                const DecalSettings& settings);
private:
    void generateKerbs();
    void generateSkidMarks();
    void generateRubber();
};
```

---

## 4.5 `WorldAssembler`  
```cpp
class WorldAssembler {
public:
    World build(const TrackData& track);
private:
    TerrainGenerator terrainGen;
    TrackSculptor sculptor;
    EnvironmentSpawner spawner;
    DecalGenerator decals;
};
```

---

# 5. Environment Preset Definitions  
Each environment type defines:

```yaml
environment_presets:
  desert:
    base_noise_freq: 0.2
    detail_noise_freq: 1.0
    max_height: 15
    object_density: 0.3
    allowed_objects: ["rock_small", "rock_large", "bush_dry"]
  forest:
    base_noise_freq: 0.1
    detail_noise_freq: 0.5
    max_height: 40
    object_density: 0.8
    allowed_objects: ["tree_pine", "tree_oak", "bush_green", "grass"]
  city:
    base_noise_freq: 0.05
    detail_noise_freq: 0.1
    max_height: 5
    object_density: 0.6
    allowed_objects: ["building_small", "building_large", "lamp_post", "fence"]
```

---

# 6. Determinism  
All procedural steps must use:

- Track seed  
- Global seed  
- Environment seed  

Combined into a single RNG state.

---

# 7. Performance Considerations  
- Generate terrain at lower resolution, then subdivide near track  
- Use instancing for vegetation  
- Cache noise samples  
- Multi‑thread object placement  

---

# 8. Use in the game


🧩 Implementation Architecture (Short Version)
Track Load
- Parse YAML
- Generate terrain
- Sculpt track
- Place environment objects
- Bake static decals (kerbs, painted lines)
- Cache result in memory

Session Start
- Apply weather overlays
- Generate dynamic decals (rubber, skid marks)
- Spawn session‑specific props (marshals, cones, cameras)