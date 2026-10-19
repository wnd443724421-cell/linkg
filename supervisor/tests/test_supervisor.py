#!/usr/bin/env python3
"""Run real Supervisor and real client against isolated mock processes."""

import concurrent.futures
import errno
import os
import pathlib
import re
import signal
import subprocess
import sys
import tempfile
import time


SUPERVISOR = os.path.abspath(sys.argv[1])
MOCK = os.path.abspath(sys.argv[2])
FAULTS = os.path.abspath(sys.argv[3])
STOP_WAIT_SECONDS = 9 if "--production" in sys.argv[3:] else 4


def require(condition, detail):
    if not condition:
        raise AssertionError(detail)


def run_case(mode, seconds, checks, expected_starts=None, minimum_starts=None, extra_environment=None):
    with tempfile.TemporaryDirectory(prefix="linkg-supervisor-") as directory:
        log_path = pathlib.Path(directory) / "log"
        environment = dict(os.environ, SUPERVISOR_MOCK_LOCK=directory + "/child.lock", SUPERVISOR_MOCK_STATE=directory + "/state")
        environment.update(extra_environment or {})
        with log_path.open("w+") as log:
            process = subprocess.Popen(
                [SUPERVISOR, "--lock-file", directory + "/supervisor.lock", MOCK, mode],
                stdout=log, stderr=log, env=environment,
            )
            try:
                time.sleep(seconds)
                require(process.poll() is None, "Supervisor exited before explicit stop")
                process.send_signal(signal.SIGTERM)
                process.wait(timeout=STOP_WAIT_SECONDS)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
            log.seek(0)
            output = log.read()

        require(process.returncode == 0, output)
        require("MOCK_OVERLAP" not in output, output)
        starts = re.findall(r"t=(\d+) pid=(\d+) state=STARTING", output)
        if expected_starts is not None:
            require(len(starts) == expected_starts, output)
        if minimum_starts is not None:
            require(len(starts) >= minimum_starts, output)
        for text in checks:
            require(text in output, output)
        for _, child in starts:
            require(not pathlib.Path("/proc", child).exists(), "child leaked: " + output)
        return output


def healthy():
    output = run_case("healthy", 1.5, ["state=RUNNING", "stable failures=0"], 1)
    require("timeout=" not in output, output)


def startup():
    run_case("startup", 1.65, ["timeout=READY", "failures=1", "failures=2"], minimum_starts=2)


def heartbeat_before_ready():
    run_case("startup-heartbeat", 0.95, ["timeout=READY"], 2)


def heartbeat_timeout():
    run_case("heartbeat", 0.8, ["timeout=HEARTBEAT"], 2)


def abnormal_exit():
    output = run_case("exit", 1.3, ["exited code=17", "retry_ms=100", "retry_ms=200", "retry_ms=400"])
    require(len(re.findall("state=STARTING", output)) >= 3, output)


def early_clean_exit():
    run_case("early-clean", 0.55, ["exited code=0", "retry_ms=200"], minimum_starts=2)


def stable_reset():
    output = run_case("recover-stable", 1.65, ["stable failures=0"], 3)
    require(output.count("failures=1 retry_ms=100") == 2 and "failures=2" not in output, output)


def crash():
    run_case("crash", 0.7, ["exited signal=11", "recovery error="], 3)


def restarting():
    output = run_case("restart", 1.5, ["state=RESTARTING", "mock_cycle=0 threads=2"], 1)
    require(output.count("state=RUNNING") == 2 and "timeout=" not in output, output)


def restarting_timeout():
    run_case("restart-timeout", 1.25, ["timeout=RESTARTING"], 2)


def repeated_restarting():
    run_case("restart-repeat", 1.25, ["timeout=RESTARTING"], 2)


def repeated_ready():
    run_case("ready-repeat", 0.8, ["timeout=HEARTBEAT"], 2)


def force_kill():
    output = run_case("ignore-term", 0.95, ["timeout=HEARTBEAT", "signal=SIGKILL", "exited signal=9"], 2)
    require(output.index("exited signal=9") < output.rindex("state=STARTING"), output)


def backoff():
    output = run_case("invalid", 2.8, ["retry_ms=100", "retry_ms=200", "retry_ms=400", "retry_ms=800"])
    stamps = [int(stamp) for stamp, _ in re.findall(r"t=(\d+) pid=(\d+) state=STARTING", output)]
    intervals = [right - left for left, right in zip(stamps, stamps[1:])]
    require(len(intervals) >= 4, output)
    for actual, minimum in zip(intervals, [100, 200, 400, 800]):
        require(minimum <= actual < minimum + 300, str(intervals) + output)


def pipe_eof():
    output = run_case("pipe-eof", 0.65, ["pipe=unexpected_EOF", "recovery error=-32"], minimum_starts=2)
    require("state=STOPPING" not in output and "timeout=STOPPING" not in output, output)


def eof_during_start_or_restart():
    for mode in ["eof-startup", "eof-restart"]:
        output = run_case(mode, 0.45, ["pipe=unexpected_EOF"], minimum_starts=2)
        require("timeout=" not in output, output)


def stopping_repeated_messages():
    output = run_case("stopping-repeat", 1.25, ["state=STOPPING", "timeout=STOPPING"], minimum_starts=2)
    require(output.count("state=RUNNING") == output.count("state=STARTING"), output)


def stopping_during_start_or_restart():
    for mode, seconds in [("stopping-startup", 0.95), ("stopping-restart", 1.25)]:
        run_case(mode, seconds, ["state=STOPPING", "timeout=STOPPING"], minimum_starts=2)


def cleanup_grace():
    output = run_case("cleanup", 1.2, ["state=STOPPING", "pipe=EOF", "exited code=0", "retry_ms=100"], 2)
    require("timeout=" not in output, output)


def normal_exit():
    run_case("clean", 0.55, ["exited code=0", "retry_ms=200"], 2)


def fast_normal_exit():
    run_case("fast-clean", 0.55, ["exited code=0", "retry_ms=200"], minimum_starts=2)


def externally_terminated_child():
    with tempfile.TemporaryDirectory(prefix="linkg-supervisor-signal-") as directory:
        log_path = pathlib.Path(directory) / "log"
        with log_path.open("w+") as log:
            process = subprocess.Popen([SUPERVISOR, "--lock-file", directory + "/lock", MOCK, "healthy"], stdout=log, stderr=log)
            try:
                for index, child_signal in enumerate([signal.SIGTERM, signal.SIGKILL]):
                    deadline = time.monotonic() + 3
                    while True:
                        output = log_path.read_text()
                        children = re.findall(r"pid=(\d+) state=RUNNING", output)
                        if len(children) > index:
                            break
                        require(process.poll() is None and time.monotonic() < deadline, output)
                        time.sleep(0.02)
                    os.kill(int(children[index]), child_signal)
                deadline = time.monotonic() + 3
                while True:
                    output = log_path.read_text()
                    if len(re.findall(r"pid=(\d+) state=RUNNING", output)) >= 3:
                        break
                    require(process.poll() is None and time.monotonic() < deadline, output)
                    time.sleep(0.02)
                require("exited signal=15" in output and "exited signal=9" in output, output)
            finally:
                process.terminate()
                process.wait(timeout=4)
        starts = re.findall(r"pid=(\d+) state=STARTING", log_path.read_text())
        require(all(not pathlib.Path("/proc", child).exists() for child in starts), str(starts))


def restart_cycles():
    output = run_case("cycles", 3.5, ["mock_cycle=3 threads=2"], 1)
    require(output.count("state=RESTARTING") == 4 and "timeout=" not in output, output)


def standalone():
    environment = dict(os.environ)
    environment.pop("LINKG_SUPERVISOR_FD", None)
    result = subprocess.run([MOCK, "probe"], env=environment, capture_output=True, text=True, timeout=2)
    require(result.returncode == 0 and "client_result=0 threads=1" in result.stdout, result.stdout + result.stderr)


def bad_environment():
    for value in ["", "-1", "2", "abc", "99999999999999999999", "12345"]:
        result = subprocess.run([MOCK, "probe"], env=dict(os.environ, LINKG_SUPERVISOR_FD=value), capture_output=True, text=True, timeout=2)
        require(result.returncode == 0 and "client_result=-" in result.stdout, result.stdout + result.stderr)


def client_pipe_error(full):
    read_fd, write_fd = os.pipe2(os.O_NONBLOCK)
    try:
        if full:
            while True:
                try:
                    os.write(write_fd, b"x" * 4096)
                except BlockingIOError:
                    break
        else:
            os.close(read_fd)
            read_fd = -1
        result = subprocess.run([MOCK, "probe"], env=dict(os.environ, LINKG_SUPERVISOR_FD=str(write_fd)), pass_fds=(write_fd,), capture_output=True, text=True, timeout=2)
        expected = errno.EAGAIN if full else errno.EPIPE
        require(result.returncode == 0 and f"client_result=-{expected}" in result.stdout, result.stdout + result.stderr)
        require("client_closed threads=1" in result.stdout, result.stdout)
    finally:
        if read_fd >= 0:
            os.close(read_fd)
        os.close(write_fd)


def client_descriptor_flags():
    read_fd, write_fd = os.pipe()
    try:
        result = subprocess.run([MOCK, "probe"], env=dict(os.environ, LINKG_SUPERVISOR_FD=str(write_fd)), pass_fds=(write_fd,), capture_output=True, text=True, timeout=2)
        require(result.returncode == 0 and "client_flags cloexec=1 nonblock=1 environment_removed=1" in result.stdout, result.stdout)
        require(os.read(read_fd, 1) == b"\x01", result.stdout)
    finally:
        os.close(read_fd)
        os.close(write_fd)


def client_protocol():
    read_fd, write_fd = os.pipe2(os.O_NONBLOCK)
    environment = dict(os.environ, LINKG_SUPERVISOR_FD=str(write_fd))
    process = subprocess.Popen([MOCK, "restart"], env=environment, pass_fds=(write_fd,), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    os.close(write_fd)
    messages = []
    start = time.monotonic()
    try:
        while time.monotonic() - start < 1.2:
            try:
                messages.extend((time.monotonic() - start, byte) for byte in os.read(read_fd, 4096))
            except BlockingIOError:
                pass
            time.sleep(0.01)
        restarting_index = next(index for index, (_, byte) in enumerate(messages) if byte == 3)
        next_ready = next(index for index in range(restarting_index + 1, len(messages)) if messages[index][1] == 1)
        require(messages[0][1] == 1, str(messages))
        require(next_ready == restarting_index + 1, "heartbeats during restart: " + str(messages))
        require(messages[next_ready][0] - messages[restarting_index][0] >= 0.55, str(messages))
        require(any(byte == 2 for _, byte in messages[next_ready + 1:]), str(messages))
    finally:
        process.terminate()
        process.communicate(timeout=2)
        os.close(read_fd)


def client_stopping_protocol():
    read_fd, write_fd = os.pipe2(os.O_NONBLOCK)
    process = subprocess.Popen([MOCK, "cleanup"], env=dict(os.environ, LINKG_SUPERVISOR_FD=str(write_fd)), pass_fds=(write_fd,), stdout=subprocess.DEVNULL)
    os.close(write_fd)
    messages = []
    try:
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            try:
                data = os.read(read_fd, 128)
                if not data:
                    break
                messages.extend(data)
            except BlockingIOError:
                pass
            time.sleep(0.01)
        process.wait(timeout=2)
        require(messages[0] == 1 and messages[-1] == 4 and messages.count(4) == 1, str(messages))
        require(process.returncode == 0, str(process.returncode))
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        os.close(read_fd)


def fault_environment(directory, mode):
    return dict(os.environ, LD_PRELOAD=FAULTS, LINKG_SUPERVISOR_FAULT_MODE=mode,
                LINKG_SUPERVISOR_FAULT_MARKER=directory + "/inject", SUPERVISOR_MOCK_LOCK=directory + "/child.lock")


def wait_for_log(process, path, text, timeout=4):
    deadline = time.monotonic() + timeout
    while True:
        output = path.read_text()
        if text in output:
            return output
        require(process.poll() is None and time.monotonic() < deadline, output)
        time.sleep(0.02)


def cleanup_fault(mode, stopping):
    with tempfile.TemporaryDirectory(prefix="linkg-supervisor-fault-") as directory:
        path = pathlib.Path(directory) / "log"
        marker = pathlib.Path(directory) / "inject"
        arguments = [SUPERVISOR, "--lock-file", directory + "/lock", MOCK, "healthy"]
        with path.open("w+") as log:
            process = subprocess.Popen(arguments, env=fault_environment(directory, mode), stdout=log, stderr=log)
            try:
                output = wait_for_log(process, path, "state=RUNNING")
                child = int(re.search(r"pid=(\d+) state=RUNNING", output)[1])
                marker.touch()
                if stopping:
                    process.terminate()
                else:
                    os.kill(child, signal.SIGSTOP if mode == "kill" else signal.SIGKILL)
                wait_for_log(process, path, "cleanup unresolved")
                if stopping:
                    second = subprocess.run(arguments, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, timeout=2)
                    require(second.returncode == 1 and "lock failed" in second.stderr, second.stderr)
                    process.wait(timeout=5)
                    require(process.returncode == 0, path.read_text())
                else:
                    deadline = time.monotonic() + 5
                    while path.read_text().count("state=RUNNING") < 2:
                        require(process.poll() is None and time.monotonic() < deadline, path.read_text())
                        time.sleep(0.02)
            finally:
                marker.unlink(missing_ok=True)
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)
        output = path.read_text()
        require("MOCK_OVERLAP" not in output and "cleanup unresolved" in output, output)
        require(("FAULT kill attempt=2" if mode == "kill" else "FAULT waitpid attempt=3") in output, output)
        starts = re.findall(r"pid=(\d+) state=STARTING", output)
        require(len(starts) == (1 if stopping else 2), output)
        require(all(not pathlib.Path("/proc", child).exists() for child in starts), output)


def unresolved_child_identity():
    with tempfile.TemporaryDirectory(prefix="linkg-supervisor-unknown-") as directory:
        path = pathlib.Path(directory) / "log"
        marker = pathlib.Path(directory) / "inject"
        arguments = [SUPERVISOR, "--lock-file", directory + "/lock", MOCK, "healthy"]
        with path.open("w+") as log:
            process = subprocess.Popen(arguments, env=fault_environment(directory, "unknown"), stdout=log, stderr=log)
            try:
                output = wait_for_log(process, path, "state=RUNNING")
                child = re.search(r"pid=(\d+) state=RUNNING", output)[1]
                marker.touch()
                process.terminate()
                wait_for_log(process, path, "cleanup unresolved")
                time.sleep(0.4)
                output = path.read_text()
                require(process.poll() is None and pathlib.Path("/proc", child).exists(), output)
                require("FAULT unsafe_signal" not in output and output.count("state=STARTING") == 1, output)
                require(1 <= output.count("cleanup unresolved") <= 5, output)
                second = subprocess.run(arguments, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, timeout=2)
                require(second.returncode == 1 and "lock failed" in second.stderr, second.stderr)
            finally:
                marker.unlink(missing_ok=True)
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=5)
            require(process.returncode == 0 and not pathlib.Path("/proc", child).exists(), path.read_text())


def stopping_send_failure():
    with tempfile.TemporaryDirectory(prefix="linkg-supervisor-send-") as directory:
        pathlib.Path(directory, "inject").touch()
        output = run_case("cleanup", 0.65, ["pipe=unexpected_EOF"], minimum_starts=2,
                          extra_environment=fault_environment(directory, "stopping-write"))
        require("state=STOPPING" not in output, output)


def control_send_failure(mode, child_mode, code, forbidden_state):
    with tempfile.TemporaryDirectory(prefix="linkg-supervisor-control-") as directory:
        pathlib.Path(directory, "inject").touch()
        output = run_case(child_mode, 0.7, [f"exited code={code}"], minimum_starts=2,
                          extra_environment=fault_environment(directory, mode))
        require(forbidden_state not in output and "timeout=" not in output, output)


def singleton():
    with tempfile.TemporaryDirectory(prefix="linkg-supervisor-lock-") as directory:
        arguments = [SUPERVISOR, "--lock-file", directory + "/lock", MOCK, "healthy"]
        with open(os.devnull, "w") as output:
            first = subprocess.Popen(arguments, stdout=output, stderr=output)
            try:
                time.sleep(0.1)
                second = subprocess.run(arguments, capture_output=True, text=True, timeout=2)
                require(second.returncode == 1 and "lock failed" in second.stderr, second.stderr)
            finally:
                first.terminate()
                first.wait(timeout=2)


def fd_leaks():
    with tempfile.TemporaryDirectory(prefix="linkg-supervisor-fds-") as directory:
        with open(os.devnull, "w") as output:
            process = subprocess.Popen([SUPERVISOR, "--lock-file", directory + "/lock", MOCK, "invalid"], stdout=output, stderr=output)
            try:
                time.sleep(0.15)
                baseline = len(os.listdir(f"/proc/{process.pid}/fd"))
                time.sleep(2.5)
                current = len(os.listdir(f"/proc/{process.pid}/fd"))
                require(current <= baseline + 1, f"fd leak baseline={baseline} current={current}")
            finally:
                process.terminate()
                process.wait(timeout=2)


def missing_program():
    with tempfile.TemporaryDirectory(prefix="linkg-supervisor-exec-") as directory:
        process = subprocess.Popen([SUPERVISOR, "--lock-file", directory + "/lock", directory + "/missing"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(0.45)
        process.terminate()
        _, output = process.communicate(timeout=2)
        require("exited code=127" in output and "retry_ms=200" in output, output)


def production_startup():
    run_case("startup-heartbeat", 61.7, ["timeout=READY", "retry_ms=1000"], 2)


def production_restart_timeout():
    run_case("restart-repeat", 61.7, ["timeout=RESTARTING", "retry_ms=1000"], 2)


def production_restart30():
    output = run_case("restart30", 32, ["state=RESTARTING", "mock_cycle=0 threads=2"], 1)
    require(output.count("state=RUNNING") == 2 and "timeout=" not in output, output)


def production_heartbeat():
    output = run_case("heartbeat", 5.6, ["timeout=HEARTBEAT", "retry_ms=1000"], 2)
    stamps = [int(stamp) for stamp, _ in re.findall(r"t=(\d+) pid=(\d+) state=STARTING", output)]
    require(5000 <= stamps[1] - stamps[0] < 5500, output)


def production_kill():
    configuration = pathlib.Path(__file__).parents[1] / "include/supervisor_config.h"
    term_ms = int(re.findall(r"^#define SUPERVISOR_TERMINATE_MS\s+(\d+)U", configuration.read_text(), re.MULTILINE)[-1])
    recovery_ms = 4000 + term_ms + 1000
    output = run_case("ignore-term", (recovery_ms + 700) / 1000, ["timeout=HEARTBEAT", "signal=SIGKILL", "exited signal=9"], 2)
    stamps = [int(stamp) for stamp, _ in re.findall(r"t=(\d+) pid=(\d+) state=STARTING", output)]
    require(recovery_ms <= stamps[1] - stamps[0] < recovery_ms + 600, output)


def production_healthy():
    output = run_case("healthy", 6, ["state=RUNNING"], 1)
    require("timeout=" not in output, output)


def production_normal_exit():
    output = run_case("fast-clean", 1.7, ["exited code=0", "retry_ms=1000", "retry_ms=2000"], 2)
    stamps = [int(stamp) for stamp, _ in re.findall(r"t=(\d+) pid=(\d+) state=STARTING", output)]
    require(1000 <= stamps[1] - stamps[0] < 1500, output)


def production_cleanup():
    output = run_case("cleanup-long", 7.6, ["state=STOPPING", "exited code=0"], minimum_starts=2)
    require("timeout=" not in output, output)


def production_unexpected_eof():
    output = run_case("pipe-eof", 1.5, ["pipe=unexpected_EOF", "retry_ms=1000"], minimum_starts=2)
    require("state=STOPPING" not in output, output)


TESTS = [healthy, startup, heartbeat_before_ready, heartbeat_timeout, abnormal_exit, crash, restarting,
         restarting_timeout, repeated_restarting, repeated_ready, force_kill, backoff, pipe_eof,
         cleanup_grace, normal_exit, restart_cycles, standalone, bad_environment,
         lambda: client_pipe_error(True), lambda: client_pipe_error(False), client_protocol, singleton,
         fd_leaks, missing_program, early_clean_exit, stable_reset, client_descriptor_flags, fast_normal_exit,
         externally_terminated_child, eof_during_start_or_restart, stopping_repeated_messages,
         stopping_during_start_or_restart, client_stopping_protocol, stopping_send_failure,
         lambda: cleanup_fault("kill", False), lambda: cleanup_fault("waitpid", False),
         lambda: cleanup_fault("kill", True), lambda: cleanup_fault("waitpid", True), unresolved_child_identity,
         lambda: control_send_failure("ready-write", "healthy", 10, "state=RUNNING"),
         lambda: control_send_failure("restarting-write", "restart", 11, "state=RESTARTING")]


if __name__ == "__main__":
    if "--production" in sys.argv[3:]:
        TESTS = [production_startup, production_restart_timeout, production_restart30,
                 production_heartbeat, production_kill, production_healthy, production_normal_exit,
                 production_cleanup, production_unexpected_eof]
    failures = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
        jobs = {executor.submit(test): test.__name__ + f"[{index}]" for index, test in enumerate(TESTS)}
        for future in concurrent.futures.as_completed(jobs):
            name = jobs[future]
            try:
                future.result()
                print("PASS", name, flush=True)
            except Exception as error:
                failures.append(name)
                print("FAIL", name, error, flush=True)
    print(f"RESULT {len(TESTS) - len(failures)}/{len(TESTS)} passed", flush=True)
    sys.exit(bool(failures))
