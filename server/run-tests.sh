#!/bin/bash
# Interactive Integration Test Runner for ApexSim Server
#
# This script launches an interactive terminal UI for running integration tests.
# You can navigate through available tests, run them individually, view output,
# and cancel running tests.

cd "$(dirname "$0")"

echo "Building test runner..."
cargo build --bin test-runner

if [ $? -eq 0 ]; then
    echo ""
    echo "Starting interactive test runner..."
    echo ""
    sleep 1
    cargo run --bin test-runner
else
    echo "Failed to build test runner. Please check the error messages above."
    exit 1
fi
