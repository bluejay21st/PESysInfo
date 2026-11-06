#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""PESysInfo CLI + GUI 冒烟测试（标准库 + Win32 API，无需第三方包）。

运行：python tests\\test_pesysinfo.py
"""
from __future__ import annotations

import ctypes
import json
import subprocess
import sys
import time
from ctypes import wintypes
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "test"
CLI = ROOT / "dist" / "x64" / "PESysInfo-cli.exe"
GUI = ROOT / "dist" / "x64" / "PESysInfo.exe"

failures: list[str] = []
checks = 0


def check(name: str, condition: bool, detail: str = "") -> None:
    global checks
    checks += 1
    if condition:
        print(f"  [PASS] {name}")
    else:
        print(f"  [FAIL] {name} {detail}")
        failures.append(name)


def run(args: list[str], exe: Path = CLI) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run([str(exe), *args], capture_output=True, timeout=120)


def decode(data: bytes) -> str:
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return ""


def validate_model(report: dict) -> None:
    check("JSON schema", report.get("schema") == "pesysinfo/1")
    check("JSON top fields", all(k in report for k in (
        "schema", "generated_at", "sections", "flat", "warnings")))
    sections = {s.get("key"): s for s in report.get("sections", [])}
    check("sections 存在", all(k in sections for k in (
        "system", "cpu", "memory", "storage", "graphics", "network")))
    flat = report.get("flat", {})
    check("flat 是对象", isinstance(flat, dict) and len(flat) > 0)
    all_keys: list[str] = []
    for section in report.get("sections", []):
        check(f"section {section.get('key')} items",
              isinstance(section.get("items"), list))
        for item in section.get("items", []):
            all_keys.append(item["key"])
            check(f"item {item['key']} 字段",
                  all(k in item for k in (
                      "key", "name", "data", "display", "type", "unit",
                      "estimated", "unavailable")), item.get("key", ""))
    check("flat key 与 sections 一致",
          set(all_keys) == set(flat.keys()), f"{len(all_keys)} vs {len(flat)}")
    required = [
        "cpu.brand", "cpu.cores", "cpu.logical",
        "memory.installed_bytes", "memory.slot_count", "memory.module_count",
        "storage.disk_count", "system.computer_name",
    ]
    for key in required:
        check(f"必需字段 {key}", key in flat)
    check("CPU 型号非空",
          isinstance(flat.get("cpu.brand", {}).get("data"), str) and
          len(flat["cpu.brand"]["data"]) > 0)
    check("内存插槽数合法", int(flat.get("memory.slot_count", {}).get("data", -1)) >= 0)
    check("磁盘数合法", int(flat.get("storage.disk_count", {}).get("data", -1)) >= 0)
    check("warnings 是数组", isinstance(report.get("warnings"), list))


def test_cli() -> None:
    print("\n== CLI ==")
    check("CLI 存在", CLI.is_file(), str(CLI))
    if not CLI.is_file():
        return

    r = run(["--json"])
    check("--json 退出码", r.returncode == 0, f"rc={r.returncode}")
    check("--json 无 BOM", not r.stdout.startswith(b"\xef\xbb\xbf"))
    text = decode(r.stdout)
    check("--json 严格 UTF-8", bool(text))
    if text:
        try:
            report = json.loads(text)
            validate_model(report)
        except Exception as exc:  # noqa: BLE001
            check("--json 可解析", False, repr(exc))

    for key in ("cpu.brand", "memory.slot_count", "memory.installed_bytes"):
        r = run(["--get", key])
        value = decode(r.stdout).strip()
        check(f"--get {key}", r.returncode == 0 and len(value) > 0,
              f"rc={r.returncode} out={value!r}")

    r = run(["--display", "--get", "cpu.brand"])
    check("--display --get", r.returncode == 0 and len(decode(r.stdout).strip()) > 0)

    r = run(["--get", "no.such.key"])
    check("未知 --get 退出码 2", r.returncode == 2, f"rc={r.returncode}")

    r = run(["--get"])
    check("--get 缺参退出码 2", r.returncode == 2, f"rc={r.returncode}")

    r = run(["--not-an-option"])
    check("未知选项退出码 2", r.returncode == 2, f"rc={r.returncode}")

    out_file = OUT / "cli-output.json"
    if out_file.exists():
        out_file.unlink()
    r = run(["--output", str(out_file)])
    check("--output 退出码", r.returncode == 0 and out_file.is_file(),
          f"rc={r.returncode}")
    if out_file.is_file():
        raw = out_file.read_bytes()
        check("--output 无 BOM", not raw.startswith(b"\xef\xbb\xbf"))
        try:
            file_model = json.loads(raw.decode("utf-8"))
            validate_model(file_model)
        except Exception as exc:  # noqa: BLE001
            check("--output JSON 可解析", False, repr(exc))

    r = run(["--help"])
    check("--help", r.returncode == 0 and "用法" in decode(r.stdout))
    r = run(["--version"])
    check("--version", r.returncode == 0 and "PESysInfo" in decode(r.stdout))


def find_window_for_pid(pid: int, timeout: float = 10.0) -> int | None:
    user32 = ctypes.windll.user32
    end = time.time() + timeout
    title_buffer = ctypes.create_unicode_buffer(512)

    while time.time() < end:
        found: list[int] = []

        @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        def enum_proc(hwnd, _):
            length = user32.GetWindowTextLengthW(hwnd)
            if length > 0:
                user32.GetWindowTextW(hwnd, title_buffer, len(title_buffer))
                if "PESysInfo" in title_buffer.value:
                    owner = wintypes.DWORD()
                    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
                    if owner.value == pid and user32.IsWindowVisible(hwnd):
                        found.append(hwnd)
            return True

        user32.EnumWindows(enum_proc, 0)
        if found:
            return found[0]
        time.sleep(0.2)
    return None


def test_gui() -> None:
    print("\n== GUI 冒烟 ==")
    check("GUI 存在", GUI.is_file(), str(GUI))
    if not GUI.is_file():
        return
    process = subprocess.Popen([str(GUI)], stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
    try:
        hwnd = find_window_for_pid(process.pid, timeout=10.0)
        check("主窗口出现", hwnd is not None, f"pid={process.pid}")
        if hwnd:
            user32 = ctypes.windll.user32
            title = ctypes.create_unicode_buffer(512)
            user32.GetWindowTextW(hwnd, title, len(title))
            check("主窗口标题", "PESysInfo" in title.value, title.value)
            user32.PostMessageW(hwnd, 0x0010, 0, 0)  # WM_CLOSE
        try:
            rc = process.wait(timeout=10)
            check("关闭后正常退出", rc == 0, f"rc={rc}")
        except subprocess.TimeoutExpired:
            process.kill()
            check("关闭后正常退出", False, "timeout")
    finally:
        if process.poll() is None:
            process.kill()


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    print(f"PESysInfo test root: {ROOT}")
    test_cli()
    test_gui()
    print(f"\n共 {checks} 项检查，失败 {len(failures)} 项。")
    if failures:
        for name in failures:
            print("  -", name)
        return 1
    print("全部通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
