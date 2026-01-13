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
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

#[derive(Debug, Clone)]
struct TestCase {
    name: String,
    file: String,
    description: String,
    requires_server: bool,
}

#[derive(Debug, Clone)]
struct TestCategory {
    name: String,
    description: String,
    tests: Vec<TestCase>,
}

struct TestRunner {
    categories: Vec<TestCategory>,
    selected_category: usize,
    selected_test: usize,
    output_scroll: usize,
    mode: Mode,
    test_output: Vec<String>,
    running_process: Option<Arc<Mutex<Option<Child>>>>,
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
                tests: vec![
                    TestCase {
                        name: "Server Initialization".to_string(),
                        file: "integration_test".to_string(),
                        description: "Basic server startup test".to_string(),
                        requires_server: false,
                    },
                    TestCase {
                        name: "CLI Client Workflow".to_string(),
                        file: "integration_test".to_string(),
                        description: "Complete client workflow: auth, car select, race".to_string(),
                        requires_server: true,
                    },
                    TestCase {
                        name: "Multiplayer Race Session".to_string(),
                        file: "integration_test".to_string(),
                        description: "4 clients racing together".to_string(),
                        requires_server: true,
                    },
                    TestCase {
                        name: "Telemetry Broadcast".to_string(),
                        file: "integration_test".to_string(),
                        description: "Verify clients receive telemetry broadcasts".to_string(),
                        requires_server: true,
                    },
                    TestCase {
                        name: "Tick Rate Stress".to_string(),
                        file: "integration_test".to_string(),
                        description: "Test various tick rates (120Hz-1440Hz)".to_string(),
                        requires_server: false,
                    },
                    TestCase {
                        name: "Multi-Client Load".to_string(),
                        file: "integration_test".to_string(),
                        description: "16 concurrent clients load test".to_string(),
                        requires_server: false,
                    },
                    TestCase {
                        name: "Sandbox Session Workflow".to_string(),
                        file: "integration_test".to_string(),
                        description: "Sandbox mode session workflow".to_string(),
                        requires_server: true,
                    },
                ],
            },
            TestCategory {
                name: "Lobby & Session Tests".to_string(),
                description: "Lobby management and session handling".to_string(),
                tests: vec![
                    TestCase {
                        name: "Create Session".to_string(),
                        file: "lobby_integration_tests".to_string(),
                        description: "Session creation flow".to_string(),
                        requires_server: true,
                    },
                    TestCase {
                        name: "Join Session".to_string(),
                        file: "lobby_integration_tests".to_string(),
                        description: "Join existing session".to_string(),
                        requires_server: true,
                    },
                    TestCase {
                        name: "Leave Session".to_string(),
                        file: "lobby_integration_tests".to_string(),
                        description: "Leave session flow".to_string(),
                        requires_server: true,
                    },
                    TestCase {
                        name: "Session Cleanup on Empty".to_string(),
                        file: "lobby_integration_tests".to_string(),
                        description: "Automatic session cleanup".to_string(),
                        requires_server: true,
                    },
                    TestCase {
                        name: "Max Players Limit".to_string(),
                        file: "lobby_integration_tests".to_string(),
                        description: "Session player limit enforcement".to_string(),
                        requires_server: true,
                    },
                    TestCase {
                        name: "Rapid Join/Leave".to_string(),
                        file: "lobby_integration_tests".to_string(),
                        description: "Rapid join/leave operations".to_string(),
                        requires_server: true,
                    },
                    TestCase {
                        name: "Multiple Sessions".to_string(),
                        file: "lobby_integration_tests".to_string(),
                        description: "Multiple concurrent sessions".to_string(),
                        requires_server: true,
                    },
                    TestCase {
                        name: "Join Nonexistent Session".to_string(),
                        file: "lobby_integration_tests".to_string(),
                        description: "Error handling for invalid session".to_string(),
                        requires_server: true,
                    },
                    TestCase {
                        name: "Lobby State Broadcast".to_string(),
                        file: "lobby_integration_tests".to_string(),
                        description: "Lobby state updates to all clients".to_string(),
                        requires_server: true,
                    },
                    TestCase {
                        name: "Player Returns to Lobby".to_string(),
                        file: "lobby_integration_tests".to_string(),
                        description: "Return to lobby after race".to_string(),
                        requires_server: true,
                    },
                    TestCase {
                        name: "Session Kinds".to_string(),
                        file: "lobby_integration_tests".to_string(),
                        description: "Different session types".to_string(),
                        requires_server: true,
                    },
                    TestCase {
                        name: "Disconnect Cleanup".to_string(),
                        file: "lobby_integration_tests".to_string(),
                        description: "Cleanup on client disconnect".to_string(),
                        requires_server: true,
                    },
                    TestCase {
                        name: "Demo Mode Lap Timing".to_string(),
                        file: "lobby_integration_tests".to_string(),
                        description: "Lap timing in demo mode".to_string(),
                        requires_server: true,
                    },
                ],
            },
            TestCategory {
                name: "Demo Lap Tests".to_string(),
                description: "Demo mode and lap timing validation".to_string(),
                tests: vec![
                    TestCase {
                        name: "Demo Lap Timing".to_string(),
                        file: "demo_lap_tests".to_string(),
                        description: "Demo lap timing accuracy".to_string(),
                        requires_server: false,
                    },
                ],
            },
            TestCategory {
                name: "TLS & Security Tests".to_string(),
                description: "TLS configuration and security validation".to_string(),
                tests: vec![
                    TestCase {
                        name: "TLS Not Required (Starts OK)".to_string(),
                        file: "tls_requirement_test".to_string(),
                        description: "Server starts without TLS when not required".to_string(),
                        requires_server: false,
                    },
                    TestCase {
                        name: "TLS Required (Fails Without Certs)".to_string(),
                        file: "tls_requirement_test".to_string(),
                        description: "Server fails without TLS when required".to_string(),
                        requires_server: false,
                    },
                    TestCase {
                        name: "TLS Required (Starts With Certs)".to_string(),
                        file: "tls_requirement_test".to_string(),
                        description: "Server starts with TLS when certs exist".to_string(),
                        requires_server: false,
                    },
                    TestCase {
                        name: "TLS State Logging".to_string(),
                        file: "tls_requirement_test".to_string(),
                        description: "TLS state logging".to_string(),
                        requires_server: false,
                    },
                ],
            },
            TestCategory {
                name: "Transport & Performance Tests".to_string(),
                description: "Network transport and backpressure handling".to_string(),
                tests: vec![
                    TestCase {
                        name: "Bounded Channels Prevent OOM".to_string(),
                        file: "transport_backpressure_test".to_string(),
                        description: "Backpressure prevents memory issues".to_string(),
                        requires_server: false,
                    },
                    TestCase {
                        name: "Droppable Messages Dropped".to_string(),
                        file: "transport_backpressure_test".to_string(),
                        description: "Message dropping when queue full".to_string(),
                        requires_server: false,
                    },
                    TestCase {
                        name: "Message Priority Classification".to_string(),
                        file: "transport_backpressure_test".to_string(),
                        description: "Message priority handling".to_string(),
                        requires_server: false,
                    },
                    TestCase {
                        name: "Metrics Tracking".to_string(),
                        file: "transport_backpressure_test".to_string(),
                        description: "Metrics collection".to_string(),
                        requires_server: false,
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
        }
    }

    fn draw_category_menu(&self) -> io::Result<()> {
        let (width, height) = terminal::size()?;
        let mut stdout = io::stdout();

        execute!(stdout, Clear(ClearType::All), MoveTo(0, 0))?;

        // Title
        let title_width = width.min(78);
        let title_line = "═".repeat(title_width as usize);
        let title_text = "ApexSim Server - Interactive Integration Tests";

        execute!(
            stdout,
            SetForegroundColor(Color::Cyan),
            Print(format!("╔{}╗\n", title_line)),
            Print(format!("║{:^width$}║\n", title_text, width = title_width as usize)),
            Print(format!("╚{}╝\n", title_line)),
            ResetColor
        )?;

        // Instructions
        let inst = Self::truncate_str("  ↑/↓: Navigate  │  Enter: Select Category  │  Q: Quit", (width as usize).saturating_sub(1));
        execute!(
            stdout,
            Print("\n"),
            SetForegroundColor(Color::DarkGrey),
            Print(&inst),
            Print("\n\n"),
            ResetColor
        )?;

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

            if is_selected {
                execute!(
                    stdout,
                    SetBackgroundColor(Color::DarkBlue),
                    SetForegroundColor(Color::White),
                    Print(&line),
                    ResetColor,
                    Print("\n")
                )?;
            } else {
                execute!(
                    stdout,
                    SetForegroundColor(Color::Cyan),
                    Print(&line),
                    ResetColor,
                    Print("\n")
                )?;
            }

            // Show description for selected category
            if is_selected {
                let desc = Self::truncate_str(&format!("     {}", category.description), (width as usize).saturating_sub(1));
                execute!(
                    stdout,
                    SetForegroundColor(Color::DarkGrey),
                    Print(&desc),
                    Print("\n"),
                    ResetColor
                )?;
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

        // Title
        let title_width = width.min(78);
        let title_line = "═".repeat(title_width as usize);
        let cat_name = Self::truncate_str(&category.name, (title_width as usize).saturating_sub(4));

        execute!(
            stdout,
            SetForegroundColor(Color::Cyan),
            Print(format!("╔{}╗\n", title_line)),
            Print(format!("║ {:<width$} ║\n", cat_name, width = (title_width as usize).saturating_sub(4))),
            Print(format!("╚{}╝\n", title_line)),
            ResetColor
        )?;

        // Instructions
        let inst = Self::truncate_str("  ↑/↓: Navigate  │  Enter: Run Test  │  Backspace: Back  │  Q: Quit", (width as usize).saturating_sub(1));
        execute!(
            stdout,
            Print("\n"),
            SetForegroundColor(Color::DarkGrey),
            Print(&inst),
            Print("\n\n"),
            ResetColor
        )?;

        // Test list
        for (idx, test) in category.tests.iter().enumerate() {
            let is_selected = idx == self.selected_test;

            let server_indicator = if test.requires_server { "[S]" } else { "   " };
            let max_name_width = (width as usize).saturating_sub(12);
            let test_name = Self::truncate_str(&test.name, max_name_width);
            let line = format!(" {} {:2}. {}", server_indicator, idx + 1, test_name);
            let line = Self::truncate_str(&line, (width as usize).saturating_sub(1));

            if is_selected {
                execute!(
                    stdout,
                    SetBackgroundColor(Color::DarkBlue),
                    SetForegroundColor(Color::White),
                    Print(&line),
                    ResetColor,
                    Print("\n")
                )?;
            } else {
                execute!(
                    stdout,
                    Print(&line),
                    Print("\n")
                )?;
            }

            // Show description for selected test
            if is_selected {
                let desc = Self::truncate_str(&format!("     {}", test.description), (width as usize).saturating_sub(1));
                execute!(
                    stdout,
                    SetForegroundColor(Color::DarkGrey),
                    Print(&desc),
                    Print("\n"),
                    ResetColor
                )?;
            }
        }

        // Footer
        let footer = Self::truncate_str("  [S] = Requires running server on 127.0.0.1:9000", (width as usize).saturating_sub(1));
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
        execute!(
            stdout,
            SetForegroundColor(Color::Yellow),
            Print(&title),
            Print("\n"),
            ResetColor,
            SetForegroundColor(Color::DarkGrey),
            Print(&info),
            Print("\n"),
            Print("─".repeat(width as usize)),
            Print("\n"),
            ResetColor
        )?;

        // Output
        let header_lines = 3;
        let footer_lines = 2;
        let available_lines = (height as usize).saturating_sub(header_lines + footer_lines);

        let total_output_lines = self.test_output.len();
        let start_line = if total_output_lines > available_lines {
            total_output_lines - available_lines + self.output_scroll
        } else {
            self.output_scroll
        }
        .min(total_output_lines.saturating_sub(1));

        let end_line = (start_line + available_lines).min(total_output_lines);

        for line in &self.test_output[start_line..end_line] {
            let truncated = Self::truncate_str(line, (width as usize).saturating_sub(1));
            execute!(stdout, Print(&truncated), Print("\n"))?;
        }

        // Footer
        let footer_text = Self::truncate_str("Press 'C' to cancel test  │  Press 'Q' to quit", (width as usize).saturating_sub(1));
        execute!(
            stdout,
            MoveTo(0, height - 2),
            SetForegroundColor(Color::DarkGrey),
            Print("─".repeat(width as usize)),
            ResetColor,
            Print("\n"),
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
        execute!(
            stdout,
            SetForegroundColor(Color::Green),
            Print(&title),
            Print("\n"),
            ResetColor,
            SetForegroundColor(Color::DarkGrey),
            Print("─".repeat(width as usize)),
            Print("\n"),
            ResetColor
        )?;

        // Output with scrolling
        let header_lines = 2;
        let footer_lines = 2;
        let available_lines = (height as usize).saturating_sub(header_lines + footer_lines);

        let start_line = self.output_scroll;
        let end_line = (start_line + available_lines).min(self.test_output.len());

        for line in &self.test_output[start_line..end_line] {
            let truncated = Self::truncate_str(line, (width as usize).saturating_sub(1));
            execute!(stdout, Print(&truncated), Print("\n"))?;
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
            MoveTo(0, height - 2),
            SetForegroundColor(Color::DarkGrey),
            Print("─".repeat(width as usize)),
            ResetColor,
            Print("\n"),
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

        let category = &self.categories[self.selected_category];
        let test = &category.tests[self.selected_test];
        let test_function_name = self.get_test_function_name(&test.name);

        // Build the cargo test command
        let mut cmd = Command::new("cargo");
        cmd.arg("test")
            .arg("--test")
            .arg(&test.file)
            .arg(&test_function_name)
            .arg("--")
            .arg("--ignored")
            .arg("--nocapture")
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
        Ok(())
    }

    fn get_test_function_name(&self, test_name: &str) -> String {
        // Convert display name to function name
        let name = test_name
            .to_lowercase()
            .replace(" ", "_")
            .replace("/", "_")
            .replace("(", "")
            .replace(")", "")
            .replace("-", "_");

        format!("test_{}", name)
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
}

fn main() -> io::Result<()> {
    // Setup terminal
    enable_raw_mode()?;
    let mut stdout = io::stdout();
    execute!(stdout, EnterAlternateScreen, Hide)?;

    let result = {
        let mut runner = TestRunner::new();
        runner.run()
    };

    // Cleanup terminal
    execute!(stdout, Show, LeaveAlternateScreen)?;
    disable_raw_mode()?;

    result
}
