"""Guest code delivered alongside the Council runner's standard TTY checks."""

import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


event = standard["event"]
wait_for = standard["wait_for"]
upload = standard["upload"]
git = standard["git"]
run_logged = standard["run_logged"]
active = standard["active"]
base = standard["BASE"]
revision = standard["REVISION"]
mode = standard["MODE"]


def configure(temporary, layout):
    config = temporary / "config" / "seaside"
    config.mkdir(parents=True)
    (config / "config.toml").write_text(f'''[window]
layout = "{layout}"
[keybinds]
"Super+Return" = "exec foot"
"Super+Left" = "focus left"
"Super+Right" = "focus right"
"Super+1" = "workspace 1"
"Super+2" = "workspace 2"
"Super+MouseLeftDrag" = "move_window"
"Super+MouseRightDrag" = "resize_window"
''')


def marker(directory, process, evidence, name, *, key=None, mouse=None, expected=None):
    path = directory / f"{name}.txt"
    event("part5-input", name, marker=str(path), key=key, mouse=mouse)
    wait_for(path.is_file, 14, name, process)
    pid, size, socket = path.read_text().splitlines()
    rows, columns = map(int, size.split())
    if not re.fullmatch(r"wayland-\d+", socket) or int(pid) <= 1 or min(rows, columns) < 1:
        raise RuntimeError(f"Invalid terminal response for {name}")
    result = {"pid": int(pid), "rows": rows, "columns": columns, "socket": socket}
    evidence[name] = result
    if expected and result["pid"] != expected["pid"]:
        raise RuntimeError(f"Wrong keyboard focus after {name}: {result['pid']} != {expected['pid']}")
    return result


def capture(directory, socket, name):
    destination = directory / f"{name}.png"
    subprocess.run(["grim", "-c", str(destination)], check=True, capture_output=True, timeout=5,
                   env=dict(os.environ, WAYLAND_DISPLAY=socket))
    with destination.open("rb") as stream:
        header = stream.read(24)
    if len(header) != 24 or header[:16] != b"\x89PNG\r\n\x1a\n\0\0\0\rIHDR":
        raise RuntimeError(f"Invalid display capture for {name}")
    width, height = (int.from_bytes(header[i:i + 4], "big") for i in (16, 20))
    if min(width, height) < 100:
        raise RuntimeError(f"Unexpected display dimensions for {name}")
    return destination, width, height


def drag(button, start, end):
    return {"button": button, "start": start, "end": end}


def foot_children(process):
    children = subprocess.run(["ps", "--ppid", str(process.pid), "-o", "pid=", "-o", "comm="],
                              capture_output=True, text=True, timeout=2, check=False)
    if children.returncode not in (0, 1):
        raise RuntimeError(f"Could not inspect terminal children: {children.stderr.strip()}")
    return {int(pid) for pid, name in (line.split(maxsplit=1) for line in children.stdout.splitlines())
            if name == "foot"}


def run_layout(repo, directory, result_dir, state, layout):
    directory.mkdir()
    configure(directory, layout)
    env = dict(os.environ, XDG_CONFIG_HOME=str(directory / "config"),
               XDG_STATE_HOME=str(directory / "state"), WLR_RENDERER="pixman")
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("PIPEWIRE_REMOTE", None)
    log = result_dir / f"{layout}.log"
    process = standard["start_seaside"](repo, env, log)
    evidence = state[layout] = {}

    def observe(name, **actions):
        if actions.get("key") == "open":
            existing = foot_children(process)
            event("part5-input", f"{layout}-{name}-open", key=actions.pop("key"))
            wait_for(lambda: foot_children(process) - existing, 10, f"{layout} terminal launch",
                     process)
        return marker(directory.parent, process, evidence, f"{layout}-{name}", **actions)

    try:
        wait_for(lambda: "seaside READY cursor-plane=" in log.read_text(errors="replace"),
                 25, f"{layout} startup gate", process)
        first = observe("first", key="open")
        _, width, height = capture(directory, first["socket"], "initial")
        evidence["output"] = [width, height]
        second = observe("second", key="open")
        if second["pid"] == first["pid"]:
            raise RuntimeError(f"The second {layout} terminal did not gain keyboard focus")
        reselected = observe("workspace-reselect", key="workspace-one", expected=second)
        if (reselected["columns"], reselected["rows"]) != (second["columns"], second["rows"]):
            raise RuntimeError(f"Reselecting the {layout} workspace changed its terminal geometry")
        middle = height // 2
        left = width // 4
        right = width * 3 // 4
        if layout == "dwindle":
            split = observe("split", key="left", expected=first)
            if not (split["columns"] < first["columns"] and split["rows"] == first["rows"]):
                raise RuntimeError("Dwindle did not split the wide output horizontally")
            resized = observe("resize", mouse=drag(2, (left, middle), (left + width // 10, middle)),
                              expected=first)
            if resized["columns"] <= split["columns"]:
                raise RuntimeError("Dwindle pointer resize did not widen the left tile")
            restored = observe("resize-reverse",
                               mouse=drag(2, (left + width // 10, middle), (left, middle)),
                               expected=first)
            if abs(restored["columns"] - split["columns"]) > 1 or restored["rows"] != split["rows"]:
                raise RuntimeError("Reversing the Dwindle drag did not restore the terminal geometry")
            observe("reinsert", mouse=drag(1, (right + width // 10, middle), (left, middle)),
                    expected=second)
            observe("left-after-drop", mouse={"button": 1, "start": (left, middle)}, expected=second)
            observe("right-after-drop", mouse={"button": 1, "start": (right, middle)}, expected=first)
        else:
            third = observe("third", key="open")
            if len({first["pid"], second["pid"], third["pid"]}) != 3:
                raise RuntimeError("The scrolling trial requires three independent terminals")
            observe("right-edge", key="right", expected=third)
            observe("left-from-third", key="left", expected=second)
            revealed = observe("left-from-second", key="left", expected=first)
            observe("left-edge", key="left", expected=first)
            if revealed["columns"] != first["columns"]:
                raise RuntimeError("Scrolling changed the original column width during reveal")
            resized = observe("resize", mouse=drag(2, (left, middle), (left + width // 10, middle)),
                              expected=first)
            if resized["columns"] <= revealed["columns"]:
                raise RuntimeError("Scrolling pointer resize did not widen the selected column")
            neighbor = observe("neighbor", key="right", expected=second)
            if neighbor["columns"] != second["columns"]:
                raise RuntimeError("Resizing a scrolling column changed its neighbor's width")
            event("part5-input", "show an empty workspace", key="workspace-two")
            back = observe("workspace-return", key="workspace-one", expected=second)
            if back["columns"] != neighbor["columns"]:
                raise RuntimeError("Scrolling failed to restore the selected workspace")

        screen, _, _ = capture(directory, first["socket"], layout)
        upload(f"{layout}.png", screen)
        if (standard["stop"](process) != 0 or active() != "tty1" or
                os.tcgetpgrp(0) != os.getpgrp()):
            raise RuntimeError(f"{layout} did not return cleanly to TTY 1")
        evidence["normal_exit"] = process.returncode
    except (RuntimeError, OSError, ValueError, subprocess.SubprocessError) as error:
        event("diagnostic", str(error))
        raise
    finally:
        try:
            standard["stop"](process)
        finally:
            upload(log.name, log)


def prepare(source, result_dir, state):
    bundle = source / "revision.bundle"
    with standard["OPENER"].open(base + "/bundle", timeout=30) as response:
        limit = 64 * 1024 * 1024
        bundle.write_bytes(response.read(limit + 1))
    if bundle.stat().st_size > limit:
        raise RuntimeError("The delivered bundle exceeds the test limit")
    repo = source / "checkout"
    log = result_dir / "prepare.log"
    event("update", f"isolated checkout of {revision[:12]}")
    if run_logged(["git", "clone", "--quiet", str(bundle), str(repo)], source, log, 45):
        raise RuntimeError("Could not clone the supplied revision bundle")
    if git(repo, "rev-parse", "HEAD") != revision or git(repo, "status", "--porcelain"):
        raise RuntimeError("The isolated checkout does not match the requested revision")
    env = os.environ.copy()
    if part5_pkg_config_path:
        if not Path(part5_pkg_config_path).is_dir():
            raise RuntimeError("The guest pkg-config directory is unavailable")
        env["PKG_CONFIG_PATH"] = os.pathsep.join(filter(None, (
            part5_pkg_config_path, env.get("PKG_CONFIG_PATH"))))
    if mode == "tty":
        state["software_checks"] = "not-run"
        event("build", revision[:12])
        if (run_logged(["meson", "setup", "build", "--buildtype=debug"], repo, log, 120, env=env) or
                run_logged(["ninja", "-C", "build", "-j2", "seaside", "seasidectl"], repo, log, 240, env=env)):
            raise RuntimeError("Could not build the isolated checkout")
    else:
        # Run static analysis locally. The guest runs the real portal and TTY
        # checks, the regression suites, and the Part 5 protocol fixtures.
        event("harness", revision[:12])
        reports = result_dir / "harness"
        code = run_logged(["tools/run-tests", "--results-dir", str(reports), "--suite", "unit",
                           "--suite", "integration", "--suite", "runner", "--suite", "hardware"],
                          repo, result_dir / "harness.log", 760, env=env)
        summaries = list(reports.glob("*/summary.json"))
        if not summaries:
            raise RuntimeError("The guest harness produced no summary")
        summary = summaries[0]
        upload("summary.json", summary)
        upload("summary.txt", summary.with_suffix(".txt"))
        report = json.loads(summary.read_text())
        state["software_checks"] = report["software_status"]
        tests = report["tests"]
        portals = [row for row in tests if row["test_id"].endswith("part3-real-portals")]
        layouts = [row for row in tests if row["part"] == 5]
        if (code or state["software_checks"] != "pass" or not layouts or
                any(row["status"] != "pass" for row in layouts) or
                not any(row["status"] == "pass" for row in portals)):
            raise RuntimeError(f"Guest regression, Part 5, or real portal check failed (exit {code})")
    return repo


def trial():
    tty = os.ttyname(0)
    current = active()
    existing = subprocess.run(["pgrep", "-x", "-t", "tty1", "seaside"], capture_output=True,
                              text=True, timeout=3)
    event("connected", f"stdin={tty} active={current}")
    if (tty != "/dev/tty1" or current != "tty1" or os.tcgetpgrp(0) != os.getpgrp() or
            existing.returncode != 1):
        event("done", "An idle foreground shell on active TTY 1 is required", ok=False)
        return
    result_dir = Path.home() / "Projects/seaside/test-results" / f"vm-{base.rsplit('/', 1)[-1][:12]}"
    result_dir.mkdir(mode=0o700, parents=True)
    state = {"revision": revision, "kernel": os.uname().release, "renderer": "pixman", "ok": False}
    try:
        runtime = os.environ.get("XDG_RUNTIME_DIR")
        if not runtime:
            raise RuntimeError("The guest TTY shell did not provide XDG_RUNTIME_DIR")
        with tempfile.TemporaryDirectory(prefix="seaside-vm-source-") as source:
            repo = prepare(Path(source), result_dir, state)
            with tempfile.TemporaryDirectory(prefix="seaside-vm-", dir=runtime) as name:
                temporary = Path(name)
                standard["tty_trial"](repo, temporary, result_dir, state)
                for layout in ("dwindle", "scrolling"):
                    run_layout(repo, temporary / layout, result_dir, state, layout)
        state["ok"] = True
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError, KeyboardInterrupt) as error:
        state["error"] = str(error) or type(error).__name__
    finally:
        for name in ("prepare.log", "harness.log", "normal.log", "failure.log"):
            upload(name, result_dir / name)
        state["guest_evidence"] = str(result_dir)
        standard["send"]("guest.json", json.dumps(state, indent=2).encode() + b"\n")
        event("done", state.get("error", "Part 5 VM trial passed"), ok=state["ok"])


trial()
