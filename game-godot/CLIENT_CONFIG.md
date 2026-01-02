# Client Configuration

The ApexSim game client uses a configuration file system to locate the content directory (cars, tracks, etc.).

## Configuration Files

The client looks for configuration files in the following order:

1. **`client_config.json`** - Base configuration (committed to git)
2. **`client_config.override.json`** - User-specific overrides (gitignored, for local development)

The override file takes precedence over the base config, allowing developers to customize paths without modifying the committed configuration.

## Configuration Format

```json
{
  "ContentDirectory": "../content"
}
```

### Properties

- **ContentDirectory**: Path to the content directory containing cars, tracks, etc.
  - Can be relative (to the executable) or absolute
  - Default: `"../content"`

## Usage

### Default Setup

By default, the content directory is expected to be at `../content` relative to the game executable:

```
apexsim/
├── content/
│   ├── cars/
│   └── tracks/
├── game-godot/
│   ├── client_config.json
│   └── (executable here when built)
└── server/
```

### Custom Content Location

To use a different content location (e.g., for development or testing):

1. Create `client_config.override.json` in the same directory as the executable:

```json
{
  "ContentDirectory": "/path/to/my/content"
}
```

2. Or modify `client_config.json` directly (not recommended for development)

## Car Loading

The client:
1. Scans the `{ContentDirectory}/cars` directory for car folders
2. Reads each `car.toml` file to discover available cars
3. Loads 3D models (`.glb` files) from the car folders
4. Filters cars to only show those also available on the server

## Example: Multiple Content Directories

Development setup with separate content for testing:

```json
{
  "ContentDirectory": "/home/user/apexsim-test-content"
}
```

Production setup:

```json
{
  "ContentDirectory": "./content"
}
```

## Troubleshooting

If you see errors like "Failed to open cars directory":

1. Check that `client_config.json` exists and is valid JSON
2. Verify the `ContentDirectory` path is correct and accessible
3. Ensure the content directory contains a `cars` subdirectory
4. Check file permissions on the content directory

The client will print the resolved content path on startup:
```
Using content directory: /full/path/to/content
Loading cars from: /full/path/to/content/cars
```
