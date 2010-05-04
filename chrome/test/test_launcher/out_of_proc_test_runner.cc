// Copyright (c) 2009 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>

#include "base/command_line.h"
#include "base/logging.h"
#include "base/process_util.h"

#include "chrome/test/test_launcher/test_runner.h"
#include "chrome/test/unit/chrome_test_suite.h"

// This version of the test launcher forks a new process for each test it runs.

namespace {

const char kGTestListTestsFlag[] = "gtest_list_tests";
const char kGTestHelpFlag[]   = "gtest_help";
const char kSingleProcessFlag[]   = "single-process";
const char kSingleProcessAltFlag[]   = "single_process";
const char kTestTerminateTimeoutFlag[] = "test-terminate-timeout";
// The following is kept for historical reasons (so people that are used to
// using it don't get surprised).
const char kChildProcessFlag[]   = "child";
const char kHelpFlag[]   = "help";

const int64 kDefaultTestTimeoutMs = 30000;

class OutOfProcTestRunner : public tests::TestRunner {
 public:
  OutOfProcTestRunner() {
  }

  virtual ~OutOfProcTestRunner() {
  }

  bool Init() {
    return true;
  }

  // Returns true if the test succeeded, false if it failed.
  bool RunTest(const std::string& test_name) {
    const CommandLine* cmd_line = CommandLine::ForCurrentProcess();
#if defined(OS_WIN)
    CommandLine new_cmd_line =
        CommandLine::FromString(cmd_line->command_line_string());
#else
    CommandLine new_cmd_line(cmd_line->argv());
#endif

    // Always enable disabled tests.  This method is not called with disabled
    // tests unless this flag was specified to the browser test executable.
    new_cmd_line.AppendSwitch("gtest_also_run_disabled_tests");
    new_cmd_line.AppendSwitchWithValue("gtest_filter", test_name);
    new_cmd_line.AppendSwitch(kChildProcessFlag);

    base::ProcessHandle process_handle;
    if (!base::LaunchApp(new_cmd_line, false, false, &process_handle))
      return false;

    int test_terminate_timeout_ms = kDefaultTestTimeoutMs;
    if (cmd_line->HasSwitch(kTestTerminateTimeoutFlag)) {
      std::wstring timeout_str(
          cmd_line->GetSwitchValue(kTestTerminateTimeoutFlag));
      int timeout = StringToInt(WideToUTF16Hack(timeout_str));
      test_terminate_timeout_ms = std::max(test_terminate_timeout_ms, timeout);
    }

    int exit_code = 0;
    if (!base::WaitForExitCodeWithTimeout(process_handle, &exit_code,
                                          test_terminate_timeout_ms)) {
      LOG(ERROR) << "Test timeout (" << test_terminate_timeout_ms
                 << " ms) exceeded!";

      exit_code = -1;  // Set a non-zero exit code to signal a failure.

      // Ensure that the process terminates.
      base::KillProcess(process_handle, -1, true);
    }

    return exit_code == 0;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(OutOfProcTestRunner);
};

class OutOfProcTestRunnerFactory : public tests::TestRunnerFactory {
 public:
  OutOfProcTestRunnerFactory() { }

  virtual tests::TestRunner* CreateTestRunner() const {
    return new OutOfProcTestRunner();
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(OutOfProcTestRunnerFactory);
};

void PrintUsage() {
  fprintf(stdout, "Runs tests using the gtest framework, each test being run in"
      " its own process.\nAny gtest flags can be specified.\n"
      "  --single_process\n    Runs the tests and the launcher in the same "
      "process. Useful for debugging a\n    specific test in a debugger\n  "
      "--test-terminate-timeout\n    Specifies a timeout (in milliseconds) "
      "after which a running test will be\n    forcefully terminated\n  "
      "--help\n    Shows this message.\n  --gtest_help\n    Shows the gtest "
      "help message\n");
}

}  // namespace

int main(int argc, char** argv) {
  CommandLine::Init(argc, argv);
  const CommandLine* command_line = CommandLine::ForCurrentProcess();

  if (command_line->HasSwitch(kHelpFlag)) {
    PrintUsage();
    return 0;
  }

  if (command_line->HasSwitch(kSingleProcessFlag)) {
    fprintf(stdout,
            "\n  Did you mean --%s instead? (note underscore)\n\n",
            kSingleProcessAltFlag);
  }

  if (command_line->HasSwitch(kChildProcessFlag) ||
      command_line->HasSwitch(kSingleProcessFlag) ||
      command_line->HasSwitch(kSingleProcessAltFlag) ||
      command_line->HasSwitch(kGTestListTestsFlag) ||
      command_line->HasSwitch(kGTestHelpFlag)) {
    return ChromeTestSuite(argc, argv).Run();
  }

  fprintf(stdout,
          "Starting tests...\nIMPORTANT DEBUGGING NOTE: each test is run inside"
          " its own process.\nFor debugging a test inside a debugger, use the "
          "--single_process and\n--gtest_filter=<your_test_name> flags.\n");
  OutOfProcTestRunnerFactory test_runner_factory;
  return tests::RunTests(test_runner_factory) ? 0 : 1;
}
