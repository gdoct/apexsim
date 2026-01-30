use crossterm::{
    cursor::{Hide, MoveTo, Show},
    event::{self, Event, KeyCode, KeyEvent},
    execute,
    style::{Color, Print, ResetColor, SetBackgroundColor, SetForegroundColor},
    terminal::{
        self, disable_raw_mode, enable_raw_mode, Clear, ClearType, EnterAlternateScreen,
        LeaveAlternateScreen,
    },
};
use std::io::{self, Write};
use std::net::TcpStream;
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

/// Server process manager
struct ServerProcess {
    child: Child,
}

impl ServerProcess {
    /// Kill any existing apexsim-server processes
    fn kill_existing() {
        let _ = Command::new("pkill")
            .args(["-f", "apexsim-server"])
            .status();
        // Give processes time to terminate
        thread::sleep(Duration::from_millis(500));
    }

    /// Start the server and wait for it to be ready
    fn start() -> io::Result<Self> {
        // Kill any existing server first
        Self::kill_existing();

        // Start the server process
        let child = Command::new("cargo")
            .args(["run", "--release"])
            .current_dir("/home/guido/apexsim/server")
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()?;

        // Wait for server to be ready by checking TCP port
        let start_time = std::time::Instant::now();
        let timeout = Duration::from_secs(30);

        while start_time.elapsed() < timeout {
            if TcpStream::connect("127.0.0.1:9000").is_ok() {
                // Server is accepting connections
                return Ok(Self { child });
            }
            thread::sleep(Duration::from_millis(200));
        }

        Err(io::Error::new(
            io::ErrorKind::TimedOut,
            "Server failed to start within 30 seconds",
        ))
    }

    /// Stop the server process
    fn stop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

impl Drop for ServerProcess {
    fn drop(&mut self) {
        self.stop();
    }
}

#[derive(Debug, Clone)]
struct TestCase {
    name: String,
    function_name: String,
    file: String,
    description: String,
    requires_server: bool,
}

#[derive(Debug, Clone)]
struct TestCategory {
    name: String,
    description: String,
    tests: Vec<TestCase>,
    /// If true, this category's tests manage their own server processes
    manages_own_server: bool,
}

struct TestRunner {
    pub categories: Vec<TestCategory>,
    selected_category: usize,
    selected_test: usize,
    output_scroll: usize,
    mode: Mode,
    test_output: Vec<String>,
    running_process: Option<Arc<Mutex<Option<Child>>>>,
    server_process: Option<ServerProcess>,
}

#[derive(PartialEq)]
enum Mode {
    CategoryMenu,
    TestMenu,
    Running,
    ViewOutput,
}

impl TestRunner {
    fn truncate_str(s: &str, max_len: usize) -> String {
        if s.chars().count() <= max_len {
            s.to_string()
        } else if max_len <= 3 {
            "...".to_string()
        } else {
            let mut result = String::new();
            let mut char_count = 0;
            for ch in s.chars() {
                if char_count + 3 >= max_len {
                    result.push_str("...");
                    break;
                }
                result.push(ch);
                char_count += 1;
            }
            result
        }
    }

    fn new() -> Self {
        let categories = vec![
            TestCategory {
                name: "Integration Tests".to_string(),
                description: "Core server integration tests".to_string(),
                manages_own_server: false,
                tests: vec![
                    TestCase {
                        name: "Server Initialization".to_string(),
                        function_name: "test_server_initialization".to_string(),
                        file: "integration_test".to_string(),
                        description: "Basic server startup test".to_string(),
                        requires_server: false,
                    },
                    TestCase {
                        name: "CLI Client Workflow".to_string(),
                        function_name: "test_cli_client_workflow".to_string(),
                        file: "integration_test".to_string(),
                        description: "Complete client workflow: auth, car select, race".to_string(),
                        requires_server: true,
                    },
                    TestCase {
                        name: "Multiplayer Race Session".to_string(),
                        function_name: "test_multiplayer_race_session".to_string(),
                        file: "integration_test".to_string(),
                        description: "4 clients racing together".to_string(),
                        requires_server: true,
                    },
                    TestCase {
                        name: "Telemetry Broadcast".to_string(),
                        function_name: "test_telemetry_broadcast".to_string(),
                        file: "integration_test".to_string(),
                        description: "Verify clients receive telemetry broadcasts".to_string(),
                        requires_server: true,
                    },
                    TestCase {
                        name: "Sandbox Session Workflow".to_string(),
                        function_name: "test_sandbox_session_workflow".to_string(),
                        file: "integration_test".to_string(),
                        description: "Sandbox mode session workflow".to_string(),
                        requires_server: true,
                    },
                ],
            },
            TestCategory {
                name: "Lobby & Session Tests".to_string(),
                description: "Lobby management and session handling".to_string(),
                manages_own_server: false,
                tests: vec![
                    TestCase {
                        name: "Create Session".to_string(),
                        function_name: "test_create_session".to_string(),
                        file: "lobby_integration_tests".to_string(),
                        description: "Session creation flow".to_string(),
                        requires_server: true,
                    },
                    TestCase {
                        name: "Join Session".to_string(),
                        function_name: "test_join_session".to_string(),
                        file: "lobby_integration_tests".to_string(),
                        description: "Join existing session".to_string(),
                        requires_server: true,
                    },
                    TestCase {
                        name: "Leave Session".to_string(),
                        function_name: "test_leave_session".to_string(),
                        file: "lobby_integration_tests".to_string(),
                        description: "Leave session flow".to_string(),
                        requires_server: true,
                    },
                    TestCase {
                        name: "Session Cleanup on Empty".to_string(),
                        function_name: "test_session_cleanup_on_empty".to_string(),
                        file: "lobby_integration_tests".to_string(),
                        description: "Automatic session cleanup".to_string(),
                        requires_server: true,
                    },
                    TestCase {
                        name: "Max Players Limit".to_string(),
                        function_name: "test_max_players_limit".to_string(),
                        file: "lobby_integration_tests".to_string(),
                        description: "Session player limit enforcement".to_string(),
                        requires_server: true,
                    },
                    TestCase {
                        name: "Rapid Join/Leave".to_string(),
                        function_name: "test_rapid_join_leave".to_string(),
                        file: "lobby_integration_tests".to_string(),
                        description: "Rapid join/leave operations".to_string(),
                        requires_server: true,
                    },
                    TestCase {
                        name: "Multiple Sessions".to_string(),
                        function_name: "test_multiple_sessions".to_string(),
                        file: "lobby_integration_tests".to_string(),
                        description: "Multiple concurrent sessions".to_string(),
                        requires_server: true,
                    },
                    TestCase {
                        name: "Join Nonexistent Session".to_string(),
                        function_name: "test_join_nonexistent_session".to_string(),
                        file: "lobby_integration_tests".to_string(),
                        description: "Error handling for invalid session".to_string(),
                        requires_server: true,
                    },
                    TestCase {
                        name: "Lobby State Broadcast".to_string(),
                        function_name: "test_lobby_state_broadcast".to_string(),
                        file: "lobby_integration_tests".to_string(),
                        description: "Lobby state updates to all clients".to_string(),
                        requires_server: true,
                    },
                    TestCase {
                        name: "Player Returns to Lobby".to_string(),
                        function_name: "test_player_returns_to_lobby".to_string(),
                        file: "lobby_integration_tests".to_string(),
                        description: "Return to lobby after race".to_string(),
                        requires_server: true,
                    },
                    TestCase {
                        name: "Session Kinds".to_string(),
                        function_name: "test_session_kinds".to_string(),
                        file: "lobby_integration_tests".to_string(),
                        description: "Different session types".to_string(),
                        requires_server: true,
                    },
                    TestCase {
                        name: "Disconnect Cleanup".to_string(),
                        function_name: "test_disconnect_cleanup".to_string(),
                        file: "lobby_integration_tests".to_string(),
                        description: "Cleanup on client disconnect".to_string(),
                        requires_server: true,
                    },
                    TestCase {
                        name: "Demo Mode Lap Timing".to_string(),
                        function_name: "test_demo_mode_lap_timing".to_string(),
                        file: "lobby_integration_tests".to_string(),
                        description: "Lap timing in demo mode".to_string(),
                        requires_server: true,
                    },
                ],
            },
            TestCategory {
                name: "Demo Lap Tests".to_string(),
                description: "Demo mode and lap timing validation".to_string(),
                manages_own_server: false,
                tests: vec![
                    TestCase {
                        name: "Demo Lap Timing".to_string(),
                        function_name: "test_demo_lap_timing".to_string(),
                        file: "demo_lap_tests".to_string(),
                        description: "Demo lap timing accuracy".to_string(),
                        requires_server: false,
                    },
                ],
            },
            TestCategory {
                name: "TLS & Security Tests".to_string(),
                description: "TLS configuration and security validation".to_string(),
                manages_own_server: false,
                tests: vec![
                    TestCase {
                        name: "TLS Not Required (Starts OK)".to_string(),
                        function_name: "test_server_starts_without_tls_when_not_required".to_string(),
                        file: "tls_requirement_test".to_string(),
                        description: "Server starts without TLS when not required".to_string(),
                        requires_server: false,
                    },
                    TestCase {
                        name: "TLS Required (Fails Without Certs)".to_string(),
                        function_name: "test_server_fails_without_tls_when_required".to_string(),
                        file: "tls_requirement_test".to_string(),
                        description: "Server fails without TLS when required".to_string(),
                        requires_server: false,
                    },
                    TestCase {
                        name: "TLS Required (Starts With Certs)".to_string(),
                        function_name: "test_server_starts_with_tls_when_required_and_certs_exist".to_string(),
                        file: "tls_requirement_test".to_string(),
                        description: "Server starts with TLS when certs exist".to_string(),
                        requires_server: false,
                    },
                    TestCase {
                        name: "TLS State Logging".to_string(),
                        function_name: "test_tls_state_logging".to_string(),
                        file: "tls_requirement_test".to_string(),
                        description: "TLS state logging".to_string(),
                        requires_server: false,
                    },
                ],
            },
            TestCategory {
                name: "Transport & Performance Tests".to_string(),
                description: "Network transport and backpressure handling".to_string(),
                manages_own_server: false,
                tests: vec![
                    TestCase {
                        name: "Bounded Channels Prevent OOM".to_string(),
                        function_name: "test_bounded_channels_prevent_oom".to_string(),
                        file: "transport_backpressure_test".to_string(),
                        description: "Backpressure prevents memory issues".to_string(),
                        requires_server: false,
                    },
                    TestCase {
                        name: "Droppable Messages Dropped".to_string(),
                        function_name: "test_droppable_messages_are_dropped_when_queue_full".to_string(),
                        file: "transport_backpressure_test".to_string(),
                        description: "Message dropping when queue full".to_string(),
                        requires_server: false,
                    },
                    TestCase {
                        name: "Message Priority Classification".to_string(),
                        function_name: "test_message_priority_classification".to_string(),
                        file: "transport_backpressure_test".to_string(),
                        description: "Message priority handling".to_string(),
                        requires_server: false,
                    },
                    TestCase {
                        name: "Metrics Tracking".to_string(),
                        function_name: "test_metrics_tracking".to_string(),
                        file: "transport_backpressure_test".to_string(),
                        description: "Metrics collection".to_string(),
                        requires_server: false,
                    },
                ],
            },
            TestCategory {
                name: "Stress Tests".to_string(),
                description: "Performance benchmarks and load testing".to_string(),
                manages_own_server: true,  // Stress tests manage their own server processes
                tests: vec![
                    TestCase {
                        name: "Tick Rate Stress".to_string(),
                        function_name: "test_tick_rate_stress".to_string(),
                        file: "stress_tests".to_string(),
                        description: "Test various tick rates (120Hz-1440Hz)".to_string(),
                        requires_server: true,
                    },
                    TestCase {
                        name: "Multi-Client Load".to_string(),
                        function_name: "test_multi_client_load".to_string(),
                        file: "stress_tests".to_string(),
                        description: "16 concurrent clients load test".to_string(),
                        requires_server: true,
                    },
                ],
            },
        ];

        Self {
            categories,
            selected_category: 0,
            selected_test: 0,
            output_scroll: 0,
            mode: Mode::CategoryMenu,
            test_output: Vec::new(),
            running_process: None,
            server_process: None,
        }
    }

    /// Check if any test in the given category requires a server
    fn category_needs_server(&self, category_idx: usize) -> bool {
        let category = &self.categories[category_idx];
        // If category manages its own server, we don't start one
        if category.manages_own_server {
            return false;
        }
        category.tests.iter().any(|t| t.requires_server)
    }

    /// Start the server if needed for the current test/category
    fn ensure_server_running(&mut self) -> io::Result<bool> {
        let category = &self.categories[self.selected_category];

        // If category manages its own server, don't start one
        if category.manages_own_server {
            return Ok(false);
        }

        // Check if current test requires server
        let test = &category.tests[self.selected_test];
        if !test.requires_server {
            return Ok(false);
        }

        // Check if server is already running
        if self.server_process.is_some() {
            return Ok(true);
        }

        // Start the server
        self.test_output.push("Starting server...".to_string());
        match ServerProcess::start() {
            Ok(server) => {
                self.server_process = Some(server);
                self.test_output.push("Server started successfully.".to_string());
                self.test_output.push(String::new());
                Ok(true)
            }
            Err(e) => {
                self.test_output.push(format!("Failed to start server: {}", e));
                Err(e)
            }
        }
    }

    /// Stop the server if running
    fn stop_server(&mut self) {
        if let Some(mut server) = self.server_process.take() {
            server.stop();
        }
    }

    fn draw_category_menu(&self) -> io::Result<()> {
        let (width, height) = terminal::size()?;
        let mut stdout = io::stdout();

        execute!(stdout, Clear(ClearType::All), MoveTo(0, 0))?;

        // Title - use actual terminal width minus 2 for the box borders
        let title_width = (width as usize).saturating_sub(2).min(78);
        let title_line = "═".repeat(title_width);
        let title_text = "ApexSim Server - Interactive Integration Tests";
        let title_text = Self::truncate_str(title_text, title_width);

        execute!(
            stdout,
            SetForegroundColor(Color::Cyan),
            Print(format!("╔{}╗", title_line)),
            MoveTo(0, 1),
            Print(format!("║{:^width$}║", title_text, width = title_width)),
            MoveTo(0, 2),
            Print(format!("╚{}╝", title_line)),
            MoveTo(0, 3),
            ResetColor
        )?;

        // Instructions
        let inst = Self::truncate_str("  ↑/↓: Navigate  │  Enter: Select  │  A: Run All in Category  │  Q: Quit", (width as usize).saturating_sub(1));
        let mut current_row = 4u16;
        execute!(
            stdout,
            MoveTo(0, current_row),
            SetForegroundColor(Color::DarkGrey),
            Print(&inst),
            ResetColor
        )?;
        current_row += 2;

        // Count total tests
        let total_tests: usize = self.categories.iter().map(|c| c.tests.len()).sum();

        // Category list
        for (idx, category) in self.categories.iter().enumerate() {
            let is_selected = idx == self.selected_category;

            // Build the line
            let test_count_str = format!("({} tests)", category.tests.len());
            let available_for_name = (width as usize).saturating_sub(test_count_str.len() + 10);
            let name = Self::truncate_str(&category.name, available_for_name);

            let line = format!(" {:2}. {}  {}", idx + 1, name, test_count_str);
            let line = Self::truncate_str(&line, (width as usize).saturating_sub(1));

            execute!(stdout, MoveTo(0, current_row))?;

            if is_selected {
                execute!(
                    stdout,
                    SetBackgroundColor(Color::DarkBlue),
                    SetForegroundColor(Color::White),
                    Print(&line),
                    ResetColor
                )?;
            } else {
                execute!(
                    stdout,
                    SetForegroundColor(Color::Cyan),
                    Print(&line),
                    ResetColor
                )?;
            }
            current_row += 1;

            // Show description for selected category
            if is_selected {
                let desc = Self::truncate_str(&format!("     {}", category.description), (width as usize).saturating_sub(1));
                execute!(
                    stdout,
                    MoveTo(0, current_row),
                    SetForegroundColor(Color::DarkGrey),
                    Print(&desc),
                    ResetColor
                )?;
                current_row += 1;
            }
        }

        // Footer
        let footer = Self::truncate_str(
            &format!("  Total: {} tests across {} categories", total_tests, self.categories.len()),
            (width as usize).saturating_sub(1)
        );
        execute!(
            stdout,
            MoveTo(0, height - 2),
            SetForegroundColor(Color::DarkGrey),
            Print(&footer),
            ResetColor
        )?;

        stdout.flush()?;
        Ok(())
    }

    fn draw_test_menu(&self) -> io::Result<()> {
        let (width, height) = terminal::size()?;
        let mut stdout = io::stdout();

        execute!(stdout, Clear(ClearType::All), MoveTo(0, 0))?;

        let category = &self.categories[self.selected_category];

        // Title - use actual terminal width minus 2 for the box borders
        let title_width = (width as usize).saturating_sub(2).min(78);
        let title_line = "═".repeat(title_width);
        let cat_name = Self::truncate_str(&category.name, title_width.saturating_sub(2));

        execute!(
            stdout,
            SetForegroundColor(Color::Cyan),
            Print(format!("╔{}╗", title_line)),
            MoveTo(0, 1),
            Print(format!("║ {:<width$}║", cat_name, width = title_width.saturating_sub(1))),
            MoveTo(0, 2),
            Print(format!("╚{}╝", title_line)),
            MoveTo(0, 3),
            ResetColor
        )?;

        // Instructions
        let inst = Self::truncate_str("  ↑/↓: Navigate  │  Enter: Run Test  │  A: Run All  │  Backspace: Back  │  Q: Quit", (width as usize).saturating_sub(1));
        let mut current_row = 4u16;
        execute!(
            stdout,
            MoveTo(0, current_row),
            SetForegroundColor(Color::DarkGrey),
            Print(&inst),
            ResetColor
        )?;
        current_row += 2;

        // Test list
        for (idx, test) in category.tests.iter().enumerate() {
            let is_selected = idx == self.selected_test;

            let server_indicator = if test.requires_server { "[S]" } else { "   " };
            let max_name_width = (width as usize).saturating_sub(12);
            let test_name = Self::truncate_str(&test.name, max_name_width);
            let line = format!(" {} {:2}. {}", server_indicator, idx + 1, test_name);
            let line = Self::truncate_str(&line, (width as usize).saturating_sub(1));

            execute!(stdout, MoveTo(0, current_row))?;

            if is_selected {
                execute!(
                    stdout,
                    SetBackgroundColor(Color::DarkBlue),
                    SetForegroundColor(Color::White),
                    Print(&line),
                    ResetColor
                )?;
            } else {
                execute!(
                    stdout,
                    Print(&line)
                )?;
            }
            current_row += 1;

            // Show description for selected test
            if is_selected {
                let desc = Self::truncate_str(&format!("     {}", test.description), (width as usize).saturating_sub(1));
                execute!(
                    stdout,
                    MoveTo(0, current_row),
                    SetForegroundColor(Color::DarkGrey),
                    Print(&desc),
                    ResetColor
                )?;
                current_row += 1;
            }
        }

        // Footer
        let footer = Self::truncate_str("  [S] = Requires server (automatically managed)", (width as usize).saturating_sub(1));
        execute!(
            stdout,
            MoveTo(0, height - 1),
            SetForegroundColor(Color::DarkGrey),
            Print(&footer),
            ResetColor
        )?;

        stdout.flush()?;
        Ok(())
    }

    fn draw_running(&self) -> io::Result<()> {
        let (width, height) = terminal::size()?;
        let mut stdout = io::stdout();

        execute!(stdout, Clear(ClearType::All), MoveTo(0, 0))?;

        let category = &self.categories[self.selected_category];
        let test = &category.tests[self.selected_test];

        // Title
        let title = Self::truncate_str(&format!("Running: {}", test.name), (width as usize).saturating_sub(1));
        let info = Self::truncate_str(
            &format!("Category: {} │ File: {}", category.name, test.file),
            (width as usize).saturating_sub(1)
        );
        let separator = "─".repeat((width as usize).min(200));
        execute!(
            stdout,
            MoveTo(0, 0),
            SetForegroundColor(Color::Yellow),
            Print(&title),
            MoveTo(0, 1),
            ResetColor,
            SetForegroundColor(Color::DarkGrey),
            Print(&info),
            MoveTo(0, 2),
            Print(&separator),
            ResetColor
        )?;

        // Output
        let header_lines = 3u16;
        let footer_lines = 2u16;
        let available_lines = (height.saturating_sub(header_lines + footer_lines)) as usize;

        let total_output_lines = self.test_output.len();
        let start_line = if total_output_lines > available_lines {
            total_output_lines - available_lines + self.output_scroll
        } else {
            self.output_scroll
        }
        .min(total_output_lines.saturating_sub(1));

        let end_line = (start_line + available_lines).min(total_output_lines);

        let mut current_row = header_lines;
        for line in &self.test_output[start_line..end_line] {
            let truncated = Self::truncate_str(line, (width as usize).saturating_sub(1));
            execute!(stdout, MoveTo(0, current_row), Print(&truncated))?;
            current_row += 1;
        }

        // Footer
        let footer_text = Self::truncate_str("Press 'C' to cancel test  │  Press 'Q' to quit", (width as usize).saturating_sub(1));
        execute!(
            stdout,
            MoveTo(0, height.saturating_sub(2)),
            SetForegroundColor(Color::DarkGrey),
            Print(&separator),
            MoveTo(0, height.saturating_sub(1)),
            ResetColor,
            SetForegroundColor(Color::Red),
            Print(&footer_text),
            ResetColor
        )?;

        stdout.flush()?;
        Ok(())
    }

    fn draw_output_view(&self) -> io::Result<()> {
        let (width, height) = terminal::size()?;
        let mut stdout = io::stdout();

        execute!(stdout, Clear(ClearType::All), MoveTo(0, 0))?;

        let category = &self.categories[self.selected_category];
        let test = &category.tests[self.selected_test];

        // Title
        let title = Self::truncate_str(&format!("Test Completed: {}", test.name), (width as usize).saturating_sub(1));
        let separator = "─".repeat((width as usize).min(200));
        execute!(
            stdout,
            MoveTo(0, 0),
            SetForegroundColor(Color::Green),
            Print(&title),
            MoveTo(0, 1),
            ResetColor,
            SetForegroundColor(Color::DarkGrey),
            Print(&separator),
            ResetColor
        )?;

        // Output with scrolling
        let header_lines = 2u16;
        let footer_lines = 2u16;
        let available_lines = (height.saturating_sub(header_lines + footer_lines)) as usize;

        let start_line = self.output_scroll;
        let end_line = (start_line + available_lines).min(self.test_output.len());

        let mut current_row = header_lines;
        for line in &self.test_output[start_line..end_line] {
            let truncated = Self::truncate_str(line, (width as usize).saturating_sub(1));
            execute!(stdout, MoveTo(0, current_row), Print(&truncated))?;
            current_row += 1;
        }

        // Footer
        let footer_text = Self::truncate_str(
            &format!(
                "↑/↓: Scroll │ Lines {}-{}/{} │ Backspace: Back to Menu │ Q: Quit",
                start_line + 1,
                end_line,
                self.test_output.len()
            ),
            (width as usize).saturating_sub(1)
        );
        execute!(
            stdout,
            MoveTo(0, height.saturating_sub(2)),
            SetForegroundColor(Color::DarkGrey),
            Print(&separator),
            MoveTo(0, height.saturating_sub(1)),
            ResetColor,
            SetForegroundColor(Color::Cyan),
            Print(&footer_text),
            ResetColor
        )?;

        stdout.flush()?;
        Ok(())
    }

    fn run_test(&mut self) -> io::Result<()> {
        self.mode = Mode::Running;
        self.test_output.clear();
        self.output_scroll = 0;

        // Extract needed values to avoid borrow conflicts
        let category = &self.categories[self.selected_category];
        let test = &category.tests[self.selected_test];
        let requires_server = test.requires_server;
        let manages_own_server = category.manages_own_server;
        let test_file = test.file.clone();
        let test_function = test.function_name.clone();

        // Start server if needed (and category doesn't manage its own)
        let server_started = if requires_server && !manages_own_server {
            self.draw_running()?;
            match self.ensure_server_running() {
                Ok(started) => {
                    if started {
                        self.draw_running()?;
                    }
                    started
                }
                Err(e) => {
                    self.test_output.push(format!("ERROR: Failed to start server: {}", e));
                    self.mode = Mode::ViewOutput;
                    return Ok(());
                }
            }
        } else {
            false
        };

        // Build the cargo test command
        let mut cmd = Command::new("cargo");
        cmd.arg("test")
            .arg("--test")
            .arg(&test_file)
            .arg(&test_function)
            .arg("--");

        // Only pass --ignored for tests marked as requiring server (which have #[ignore])
        if requires_server {
            cmd.arg("--ignored");
        }

        cmd.arg("--nocapture")
            .arg("--test-threads=1")
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .current_dir("/home/guido/apexsim/server");

        let process_handle = Arc::new(Mutex::new(None::<Child>));
        self.running_process = Some(process_handle.clone());

        // Spawn the process
        let mut child = cmd.spawn()?;
        let stdout = child.stdout.take();
        let stderr = child.stderr.take();

        *process_handle.lock().unwrap() = Some(child);

        // Read output in a separate thread
        let output_lines = Arc::new(Mutex::new(Vec::new()));
        let output_lines_clone = output_lines.clone();

        thread::spawn(move || {
            use std::io::BufRead;

            if let Some(stdout) = stdout {
                let reader = std::io::BufReader::new(stdout);
                for line in reader.lines() {
                    if let Ok(line) = line {
                        output_lines_clone.lock().unwrap().push(line);
                    }
                }
            }
        });

        let output_lines_clone = output_lines.clone();
        thread::spawn(move || {
            use std::io::BufRead;

            if let Some(stderr) = stderr {
                let reader = std::io::BufReader::new(stderr);
                for line in reader.lines() {
                    if let Ok(line) = line {
                        output_lines_clone.lock().unwrap().push(line);
                    }
                }
            }
        });

        // Draw initial screen
        self.draw_running()?;

        // Poll for output and redraw
        loop {
            // Check if process is still running
            let mut process_guard = process_handle.lock().unwrap();
            let still_running = if let Some(ref mut child) = *process_guard {
                match child.try_wait() {
                    Ok(Some(_)) => false,
                    Ok(None) => true,
                    Err(_) => false,
                }
            } else {
                false
            };
            drop(process_guard);

            // Update output
            {
                let mut output_guard = output_lines.lock().unwrap();
                if !output_guard.is_empty() {
                    self.test_output.append(&mut output_guard);
                    self.draw_running()?;
                }
            }

            // Check for user input
            if event::poll(Duration::from_millis(100))? {
                if let Event::Key(key) = event::read()? {
                    match key.code {
                        KeyCode::Char('c') | KeyCode::Char('C') => {
                            // Cancel the test
                            let mut process_guard = process_handle.lock().unwrap();
                            if let Some(ref mut child) = *process_guard {
                                let _ = child.kill();
                            }
                            self.test_output.push("\n=== TEST CANCELLED BY USER ===".to_string());
                            self.mode = Mode::ViewOutput;
                            break;
                        }
                        KeyCode::Char('q') | KeyCode::Char('Q') => {
                            let mut process_guard = process_handle.lock().unwrap();
                            if let Some(ref mut child) = *process_guard {
                                let _ = child.kill();
                            }
                            return Ok(());
                        }
                        _ => {}
                    }
                }
            }

            if !still_running {
                self.mode = Mode::ViewOutput;

                // Collect any remaining output
                thread::sleep(Duration::from_millis(200));
                let mut output_guard = output_lines.lock().unwrap();
                self.test_output.append(&mut output_guard);

                break;
            }
        }

        self.running_process = None;

        // Stop server after test completes
        if server_started {
            self.test_output.push(String::new());
            self.test_output.push("Stopping server...".to_string());
            self.stop_server();
        }

        Ok(())
    }

    fn run_all_tests_in_category(&mut self) -> io::Result<()> {
        let category = &self.categories[self.selected_category];
        let total_tests = category.tests.len();
        let manages_own_server = category.manages_own_server;
        let needs_server = self.category_needs_server(self.selected_category);

        self.mode = Mode::Running;
        self.test_output.clear();
        self.output_scroll = 0;

        self.test_output.push(format!(
            "Running all {} tests in category: {}",
            total_tests, category.name
        ));
        self.test_output.push("═".repeat(60));
        self.test_output.push(String::new());

        self.draw_running()?;

        // Start server once for the entire category if needed
        let server_started = if needs_server && !manages_own_server {
            self.test_output.push("Starting server for test category...".to_string());
            self.draw_running()?;
            match ServerProcess::start() {
                Ok(server) => {
                    self.server_process = Some(server);
                    self.test_output.push("Server started successfully.".to_string());
                    self.test_output.push(String::new());
                    self.draw_running()?;
                    true
                }
                Err(e) => {
                    self.test_output.push(format!("ERROR: Failed to start server: {}", e));
                    self.test_output.push("Aborting test run.".to_string());
                    self.mode = Mode::ViewOutput;
                    return Ok(());
                }
            }
        } else {
            false
        };

        let mut passed = 0;
        let mut failed = 0;
        let mut failed_tests: Vec<String> = Vec::new();

        for test_idx in 0..total_tests {
            let category = &self.categories[self.selected_category];
            let test = &category.tests[test_idx];

            self.test_output.push(format!(
                "[{}/{}] Running: {}",
                test_idx + 1,
                total_tests,
                test.name
            ));
            self.draw_running()?;

            // Build the cargo test command
            let mut cmd = Command::new("cargo");
            cmd.arg("test")
                .arg("--test")
                .arg(&test.file)
                .arg(&test.function_name)
                .arg("--");

            if test.requires_server {
                cmd.arg("--ignored");
            }

            cmd.arg("--nocapture")
                .arg("--test-threads=1")
                .stdout(Stdio::piped())
                .stderr(Stdio::piped())
                .current_dir("/home/guido/apexsim/server");

            let output = cmd.output()?;
            let stdout_str = String::from_utf8_lossy(&output.stdout);
            let stderr_str = String::from_utf8_lossy(&output.stderr);

            if output.status.success() {
                passed += 1;
                self.test_output.push("       ✓ PASSED".to_string());
            } else {
                failed += 1;
                failed_tests.push(test.name.clone());
                self.test_output.push("       ✗ FAILED".to_string());
                // Add last few lines of output for failed tests
                for line in stdout_str.lines().take(10) {
                    self.test_output.push(format!("         {}", line));
                }
                for line in stderr_str.lines().take(5) {
                    self.test_output.push(format!("         {}", line));
                }
            }
            self.test_output.push(String::new());
            self.draw_running()?;

            // Check for user cancellation
            if event::poll(Duration::from_millis(10))? {
                if let Event::Key(key) = event::read()? {
                    match key.code {
                        KeyCode::Char('c') | KeyCode::Char('C') => {
                            self.test_output
                                .push("\n=== BATCH RUN CANCELLED BY USER ===".to_string());
                            if server_started {
                                self.test_output.push("Stopping server...".to_string());
                                self.stop_server();
                            }
                            self.mode = Mode::ViewOutput;
                            return Ok(());
                        }
                        KeyCode::Char('q') | KeyCode::Char('Q') => {
                            if server_started {
                                self.stop_server();
                            }
                            return Ok(());
                        }
                        _ => {}
                    }
                }
            }

            // Delay between tests for server cleanup
            thread::sleep(Duration::from_millis(3000));
        }

        self.test_output.push("═".repeat(60));
        self.test_output
            .push(format!("Results: {} passed, {} failed", passed, failed));

        if !failed_tests.is_empty() {
            self.test_output.push(String::new());
            self.test_output.push("Failed tests:".to_string());
            for name in &failed_tests {
                self.test_output.push(format!("  - {}", name));
            }
        }

        // Stop server after all tests complete
        if server_started {
            self.test_output.push(String::new());
            self.test_output.push("Stopping server...".to_string());
            self.stop_server();
        }

        self.mode = Mode::ViewOutput;
        Ok(())
    }

    fn handle_category_input(&mut self, key: KeyEvent) -> io::Result<bool> {
        match key.code {
            KeyCode::Up => {
                if self.selected_category > 0 {
                    self.selected_category -= 1;
                }
            }
            KeyCode::Down => {
                if self.selected_category < self.categories.len() - 1 {
                    self.selected_category += 1;
                }
            }
            KeyCode::Enter => {
                self.selected_test = 0;
                self.mode = Mode::TestMenu;
            }
            KeyCode::Char('a') | KeyCode::Char('A') => {
                self.run_all_tests_in_category()?;
            }
            KeyCode::Char('q') | KeyCode::Char('Q') => {
                return Ok(true);
            }
            _ => {}
        }
        Ok(false)
    }

    fn handle_test_menu_input(&mut self, key: KeyEvent) -> io::Result<bool> {
        let category = &self.categories[self.selected_category];

        match key.code {
            KeyCode::Up => {
                if self.selected_test > 0 {
                    self.selected_test -= 1;
                }
            }
            KeyCode::Down => {
                if self.selected_test < category.tests.len() - 1 {
                    self.selected_test += 1;
                }
            }
            KeyCode::Enter => {
                self.run_test()?;
            }
            KeyCode::Char('a') | KeyCode::Char('A') => {
                self.run_all_tests_in_category()?;
            }
            KeyCode::Backspace => {
                self.mode = Mode::CategoryMenu;
            }
            KeyCode::Char('q') | KeyCode::Char('Q') => {
                return Ok(true);
            }
            _ => {}
        }
        Ok(false)
    }

    fn handle_output_input(&mut self, key: KeyEvent) -> io::Result<bool> {
        let (_, height) = terminal::size()?;
        let available_lines = (height as usize).saturating_sub(4);

        match key.code {
            KeyCode::Up => {
                if self.output_scroll > 0 {
                    self.output_scroll -= 1;
                }
            }
            KeyCode::Down => {
                let max_scroll = self
                    .test_output
                    .len()
                    .saturating_sub(available_lines);
                if self.output_scroll < max_scroll {
                    self.output_scroll += 1;
                }
            }
            KeyCode::PageUp => {
                self.output_scroll = self.output_scroll.saturating_sub(available_lines);
            }
            KeyCode::PageDown => {
                let max_scroll = self
                    .test_output
                    .len()
                    .saturating_sub(available_lines);
                self.output_scroll = (self.output_scroll + available_lines).min(max_scroll);
            }
            KeyCode::Backspace => {
                self.mode = Mode::TestMenu;
                self.output_scroll = 0;
            }
            KeyCode::Char('q') | KeyCode::Char('Q') => {
                return Ok(true);
            }
            _ => {}
        }
        Ok(false)
    }

    fn run(&mut self) -> io::Result<()> {
        loop {
            match self.mode {
                Mode::CategoryMenu => {
                    self.draw_category_menu()?;
                    if let Event::Key(key) = event::read()? {
                        if self.handle_category_input(key)? {
                            break;
                        }
                    }
                }
                Mode::TestMenu => {
                    self.draw_test_menu()?;
                    if let Event::Key(key) = event::read()? {
                        if self.handle_test_menu_input(key)? {
                            break;
                        }
                    }
                }
                Mode::ViewOutput => {
                    self.draw_output_view()?;
                    if let Event::Key(key) = event::read()? {
                        if self.handle_output_input(key)? {
                            break;
                        }
                    }
                }
                Mode::Running => {
                    // Running mode is handled in run_test
                }
            }
        }
        Ok(())
    }

    /// Run all tests in a category (non-interactive, for CLI use)
    fn run_category(&mut self, category_idx: usize) -> io::Result<()> {
        let category = &self.categories[category_idx];
        let total_tests = category.tests.len();
        let manages_own_server = category.manages_own_server;

        println!("Running category: {} ({} tests)", category.name, total_tests);
        println!("{}", "═".repeat(60));
        println!();

        // Check if category needs server
        let needs_server = self.category_needs_server(category_idx);

        // Start server if needed
        let server_started = if needs_server && !manages_own_server {
            println!("Starting server...");
            match ServerProcess::start() {
                Ok(server) => {
                    self.server_process = Some(server);
                    println!("Server started successfully.");
                    println!();
                    true
                }
                Err(e) => {
                    eprintln!("ERROR: Failed to start server: {}", e);
                    std::process::exit(1);
                }
            }
        } else {
            false
        };

        let mut passed = 0;
        let mut failed = 0;
        let mut failed_tests = Vec::new();

        for (idx, test) in category.tests.iter().enumerate() {
            println!(
                "[{}/{}] Running: {}",
                idx + 1,
                total_tests,
                test.name
            );

            // Build the cargo test command
            let mut cmd = Command::new("cargo");
            cmd.arg("test")
                .arg("--test")
                .arg(&test.file)
                .arg(&test.function_name)
                .arg("--");

            if test.requires_server {
                cmd.arg("--ignored");
            }

            cmd.arg("--nocapture")
                .arg("--test-threads=1")
                .current_dir("/home/guido/apexsim/server");

            let output = cmd.output()?;
            let stdout = String::from_utf8_lossy(&output.stdout);
            let stderr = String::from_utf8_lossy(&output.stderr);

            if output.status.success() {
                passed += 1;
                println!("       ✓ PASSED");
            } else {
                failed += 1;
                failed_tests.push((test.name.clone(), stdout.to_string(), stderr.to_string()));
                println!("       ✗ FAILED");
            }
            println!();

            // Delay between tests to allow server to cleanup sessions
            // 3 seconds is needed for the synthetic Disconnect to be fully processed
            // and for the main loop to run cleanup cycles
            std::thread::sleep(Duration::from_millis(3000));
        }

        println!("{}", "═".repeat(60));
        println!("Results: {} passed, {} failed", passed, failed);

        // Stop server after all tests
        if server_started {
            println!();
            println!("Stopping server...");
            self.stop_server();
        }

        if !failed_tests.is_empty() {
            println!();
            println!("Failed tests:");
            for (name, stdout, stderr) in &failed_tests {
                println!("  - {}", name);
                if !stdout.is_empty() {
                    // Show last 20 lines of stdout
                    let lines: Vec<&str> = stdout.lines().collect();
                    let start = if lines.len() > 20 { lines.len() - 20 } else { 0 };
                    for line in &lines[start..] {
                        println!("    stdout: {}", line);
                    }
                }
                if !stderr.is_empty() {
                    // Show last 10 lines of stderr
                    let lines: Vec<&str> = stderr.lines().collect();
                    let start = if lines.len() > 10 { lines.len() - 10 } else { 0 };
                    for line in &lines[start..] {
                        println!("    stderr: {}", line);
                    }
                }
            }
            std::process::exit(1);
        }

        Ok(())
    }
}

fn print_usage() {
    println!("ApexSim Server - Interactive Integration Tests");
    println!();
    println!("Usage: test-runner [OPTIONS]");
    println!();
    println!("Options:");
    println!("  -c, --category <N>  Run all tests in category N (1-indexed)");
    println!("  -l, --list          List all categories and their tests");
    println!("  -h, --help          Show this help message");
    println!();
    println!("Without options, launches the interactive TUI.");
}

fn list_categories(runner: &TestRunner) {
    println!("ApexSim Server - Test Categories");
    println!("=================================");
    println!();
    for (idx, category) in runner.categories.iter().enumerate() {
        println!("{}. {} ({} tests)", idx + 1, category.name, category.tests.len());
        println!("   {}", category.description);
        for (tidx, test) in category.tests.iter().enumerate() {
            let server_marker = if test.requires_server { "[S]" } else { "   " };
            println!("      {} {}.{} {}", server_marker, idx + 1, tidx + 1, test.name);
        }
        println!();
    }
    println!("[S] = Requires server (automatically managed by test runner)");
}

fn main() -> io::Result<()> {
    let args: Vec<String> = std::env::args().collect();

    // Kill any existing server processes on startup
    println!("Terminating any existing apexsim-server processes...");
    ServerProcess::kill_existing();

    let mut runner = TestRunner::new();

    // Parse command line arguments
    if args.len() > 1 {
        let arg = &args[1];
        match arg.as_str() {
            "-h" | "--help" => {
                print_usage();
                return Ok(());
            }
            "-l" | "--list" => {
                list_categories(&runner);
                return Ok(());
            }
            "-c" | "--category" => {
                if args.len() < 3 {
                    eprintln!("Error: --category requires a number argument");
                    std::process::exit(1);
                }
                let cat_num: usize = args[2].parse().unwrap_or_else(|_| {
                    eprintln!("Error: Invalid category number '{}'", args[2]);
                    std::process::exit(1);
                });
                if cat_num == 0 || cat_num > runner.categories.len() {
                    eprintln!(
                        "Error: Category {} does not exist. Valid range: 1-{}",
                        cat_num,
                        runner.categories.len()
                    );
                    std::process::exit(1);
                }
                return runner.run_category(cat_num - 1);
            }
            _ => {
                // Check if it's just a number (shorthand for -c)
                if let Ok(cat_num) = arg.parse::<usize>() {
                    if cat_num > 0 && cat_num <= runner.categories.len() {
                        return runner.run_category(cat_num - 1);
                    }
                }
                eprintln!("Error: Unknown argument '{}'", arg);
                print_usage();
                std::process::exit(1);
            }
        }
    }

    // Interactive mode
    enable_raw_mode()?;
    let mut stdout = io::stdout();
    execute!(stdout, EnterAlternateScreen, Hide)?;

    let result = runner.run();

    // Cleanup terminal
    execute!(stdout, Show, LeaveAlternateScreen)?;
    disable_raw_mode()?;

    result
}
