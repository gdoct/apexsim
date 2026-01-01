# ApexSim CLI Game Client

A command-line client for testing and interacting with the ApexSim Racing Server without needing the Unreal Engine client.

## Features

- Connect to the ApexSim server via TCP
- Authenticate with player name and token
- View lobby state (players, sessions, cars, tracks)
- Select a car
- Create new racing sessions
- Join existing sessions
- Start sessions and receive telemetry
- Configurable via command-line args or config file

## Building

```bash
cd cli-game
cargo build
```

For release builds:
```bash
cargo build --release
```

## Usage

### Basic usage (uses default config)
```bash
cargo run
```

### With custom player name
```bash
cargo run -- -n "MyPlayerName"
```

### With custom server address
```bash
cargo run -- -s "192.168.1.100:9000"
```

### With all options
```bash
cargo run -- -c ./client.toml -s "127.0.0.1:9000" -n "TestPlayer" -t "my-token" -l debug
```

## Configuration

Create a `client.toml` file:

```toml
[server]
tcp_address = "127.0.0.1:9000"
udp_address = "127.0.0.1:9001"

[player]
name = "CLI-Player"
token = "dev-token"
```

## Command-Line Options

| Option | Short | Description | Default |
|--------|-------|-------------|---------|
| `--config` | `-c` | Path to config file | `./client.toml` |
| `--server` | `-s` | Server TCP address | From config |
| `--name` | `-n` | Player name | From config |
| `--token` | `-t` | Auth token | From config |
| `--log-level` | `-l` | Log level (trace/debug/info/warn/error) | `info` |

## VS Code Tasks

The project includes VS Code tasks for easy development:

- **Server: Build** - Build the server
- **Server: Run** - Run the server in release mode
- **Server: Run (Debug)** - Run the server with debug logging
- **Server: Stop** - Stop all running server processes
- **CLI Game: Build** - Build the CLI client
- **CLI Game: Run** - Run the CLI client
- **CLI Game: Run (Custom Name)** - Run with a custom player name
- **Build All** - Build both server and client

Use `Ctrl+Shift+P` → "Tasks: Run Task" to access these.

## Interactive Menu

Once connected, you'll see an interactive menu:

```
What would you like to do?
> Refresh lobby state
  Select car
  Create new session
  Join session
  Send heartbeat
  Quit
```

Use arrow keys to navigate and Enter to select.

## Protocol

The client uses the same binary protocol as the Unreal Engine client:
- TCP for reliable messages (auth, lobby, session management)
- Messages are length-prefixed and serialized with bincode
- Compatible with server version 0.1.0
