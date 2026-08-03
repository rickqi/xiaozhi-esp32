#! /usr/bin/env python3
"""
detect_env.py — 多环境开发环境检测工具

用于识别当前开发机器的 ESP-IDF / Python 虚拟环境 / 设备串口 / 项目版本，
避免在 AGENTS.md 或文档中硬编码单机路径（环境迁移后会失真）。

用法（PowerShell）:
  python scripts/detect_env.py                 # 人类可读摘要
  python scripts/detect_env.py --json          # 输出 JSON（供 agent 脚本消费）
  python scripts/detect_env.py --check         # 与 env_expect.json 快照对比，判定环境冲突
  python scripts/detect_env.py --health        # 健康检查：验证环境是否满足开发/调试/烧录要求
  python scripts/detect_env.py --export-ps1    # 输出可直接执行的 PowerShell 环境变量赋值

--health 检查项（10 项）:
  IDF 路径/版本、Python venv 存在性、venv 版本标记匹配（idf_version.txt）、
  Python 依赖满足 IDF 约束（check-python-dependencies）、工具链（idf_tools.py check）、
  串口设备、sdkconfig 芯片/板/console/FATFS/唤醒词配置、分区表、构建产物。
  任一 FAIL => exit 2，表示环境不满足开发要求。

设计原则:
  - 无第三方依赖（仅标准库），可在任何 Python 3.8+ 上运行
  - 检测优先级: 环境变量 > idf-env.json（官方安装器元数据）> 常见路径探测
  - 串口识别: 优先匹配 Espressif USB-Serial/JTAG（VID 0x303A）设备
  - 不修改任何文件（--check 只读对比），不依赖已激活的 IDF shell
"""

import argparse
import json
import os
import platform
import re
import subprocess
import sys
import time

# Windows 控制台默认 GBK，强制 UTF-8 输出避免中文乱码
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXPECT_FILE = os.path.join(REPO_ROOT, "scripts", "env_expect.json")

# Espressif 官方 USB-Serial/JTAG 设备 VID（ESP32-C3/S3 等板载调试口）
ESPRESSIF_VID = 0x303A
# 本板常见的 USB 串口设备 VID（USB-UART 桥接，如 CP2102/CH340 等）
BRIDGE_VENDORS = {0x10C4, 0x1A86, 0x0403, 0x303A}


def get_git_repo_head():
    """当前仓库 HEAD 短哈希"""
    try:
        out = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            cwd=REPO_ROOT, capture_output=True, text=True, timeout=5,
        )
        if out.returncode == 0:
            return out.stdout.strip()
    except Exception:
        pass
    return None


def get_project_version():
    """从 CMakeLists.txt 读取 PROJECT_VER（版本号唯一来源）"""
    try:
        with open(os.path.join(REPO_ROOT, "CMakeLists.txt"), "r",
                  encoding="utf-8", errors="replace") as f:
            for line in f:
                m = re.search(r'set\s*\(\s*PROJECT_VER\s+"([^"]+)"\s*\)', line)
                if m:
                    return m.group(1)
    except Exception:
        pass
    return None


def get_idf_path_from_env():
    """环境变量 IDF_PATH（已激活 IDF 时存在）"""
    return os.environ.get("IDF_PATH") or None


def get_idf_from_installer_metadata():
    """从 idf-env.json 读取官方安装器记录的所有 IDF 实例（权威检测源）

    返回 [(idf_path, version), ...]，按 idfInstalled 顺序。
    """
    candidates = []
    # 官方安装器元数据位置：IDF_TOOLS_PATH 或用户主目录下 .espressif/idf-env.json
    tools_path = os.environ.get("IDF_TOOLS_PATH")
    search_dirs = []
    if tools_path:
        search_dirs.append(tools_path)
    search_dirs.append(os.path.expanduser("~/.espressif"))
    search_dirs.append(os.path.expanduser("~/.esp-idf"))
    for d in search_dirs:
        p = os.path.join(d, "idf-env.json")
        if not os.path.isfile(p):
            continue
        try:
            with open(p, "r", encoding="utf-8") as f:
                data = json.load(f)
        except Exception:
            continue
        for key, info in (data.get("idfInstalled") or {}).items():
            path = info.get("path") or key
            ver = info.get("version") or ""
            if os.path.isfile(os.path.join(path, "tools", "idf.py")) or \
               os.path.isfile(os.path.join(path, "idf.py")):
                candidates.append((path, ver))
    return candidates


def get_idf_from_common_paths():
    """常见安装路径探测（Windows / Linux / macOS）"""
    home = os.path.expanduser("~")
    common = [
        os.path.join(home, "esp", "esp-idf"),
        os.path.join(home, "esp", "esp-idf-v5.5"),
        os.path.join(home, "esp-idf"),
        r"D:\esp\esp-idf",
        r"C:\esp\esp-idf",
        "/opt/esp-idf",
        "/opt/esp/idf",
    ]
    found = []
    for p in common:
        if os.path.isfile(os.path.join(p, "tools", "idf.py")) or \
           os.path.isfile(os.path.join(p, "idf.py")):
            found.append((p, ""))
    return found


def get_idf_path():
    """定位 IDF 路径：环境变量 > 安装器元数据 > 常见路径"""
    env_path = get_idf_path_from_env()
    if env_path:
        return env_path
    for path, _ in get_idf_from_installer_metadata():
        return path
    for path, _ in get_idf_from_common_paths():
        return path
    return None


def get_idf_version(idf_path):
    """读取 IDF 版本：version.txt > git describe > idf-env.json"""
    if not idf_path:
        return None
    # version.txt（新版 IDF 有）
    for name in ("version.txt",):
        p = os.path.join(idf_path, name)
        if os.path.isfile(p):
            try:
                with open(p, "r", encoding="utf-8") as f:
                    ver = f.read().strip()
                if ver:
                    return ver
            except Exception:
                pass
    # git describe（IDF 是 git 克隆时）
    try:
        out = subprocess.run(
            ["git", "describe", "--tags", "--always"],
            cwd=idf_path, capture_output=True, text=True, timeout=5,
        )
        if out.returncode == 0:
            return out.stdout.strip()
    except Exception:
        pass
    # 安装器元数据兜底
    for path, ver in get_idf_from_installer_metadata():
        if path == idf_path and ver:
            return ver
    return None


def get_python_env_path(idf_path):
    """定位 IDF 的 Python 虚拟环境

    优先级: IDF_PYTHON_ENV_PATH > ~/.espressif/python_env/<idf5.x_py3.x_env>
    """
    env_var = os.environ.get("IDF_PYTHON_ENV_PATH")
    if env_var and os.path.isfile(os.path.join(env_var, "Scripts", "python.exe")):
        return env_var
    if env_var and os.path.isfile(os.path.join(env_var, "bin", "python3")):
        return env_var

    # 从 tools/activate.py 读取实际 python env（权威）
    if idf_path:
        activate = os.path.join(idf_path, "tools", "activate.py")
        if os.path.isfile(activate):
            try:
                out = subprocess.run(
                    [sys.executable, activate, "--export"],
                    capture_output=True, text=True, timeout=30,
                )
                m = re.search(r"IDF_PYTHON_ENV_PATH[=:]\s*([^\s;'\"]+)", out.stdout)
                if m:
                    p = os.path.normpath(m.group(1).strip())
                    if os.path.isfile(os.path.join(p, "Scripts", "python.exe")) or \
                       os.path.isfile(os.path.join(p, "bin", "python3")):
                        return p
            except Exception:
                pass

    # ~/.espressif/python_env/ 下按 idf5.x_py3.x_env 命名扫描
    tools_path = os.environ.get("IDF_TOOLS_PATH")
    dirs = []
    if tools_path:
        dirs.append(os.path.join(tools_path, "python_env"))
    dirs.append(os.path.expanduser("~/.espressif/python_env"))
    for d in dirs:
        d = os.path.normpath(d)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d), reverse=True):
            cand = os.path.normpath(os.path.join(d, name))
            if os.path.isfile(os.path.join(cand, "Scripts", "python.exe")):
                return cand
            if os.path.isfile(os.path.join(cand, "bin", "python3")):
                return cand
    return None


def get_serial_port():
    """检测设备串口

    优先: 通过 pyserial 枚举，匹配 Espressif USB-Serial/JTAG（VID 0x303A）
    兜底: Windows 注册表 / 平台常规设备名
    """
    # 方法 1: pyserial（IDF python env 通常自带 pyserial）
    try:
        import serial.tools.list_ports
    except Exception:
        serial = None
    else:
        try:
            ports = list(serial.tools.list_ports.comports())
        except Exception:
            ports = []
        # 优先 Espressif USB-Serial/JTAG
        for p in ports:
            if getattr(p, "vid", None) == ESPRESSIF_VID:
                return p.device
        # 其次 USB-UART 桥接芯片（CP2102/CH340/FTDI），且描述含调试字样
        for p in ports:
            if getattr(p, "vid", None) in BRIDGE_VENDORS:
                return p.device
        # 再退一步: 任何 "USB" 串口
        for p in ports:
            desc = (p.description or "").lower()
            if "usb" in desc or "serial" in desc or "jtag" in desc:
                return p.device
        return None

    # 方法 2: Windows 注册表（无 pyserial 时）
    if platform.system() == "Windows":
        try:
            out = subprocess.run(
                ["reg", "query", r"HKLM\HARDWARE\DEVICEMAP\SERIALCOMM"],
                capture_output=True, text=True, timeout=10,
            )
            coms = re.findall(r"COM\d+", out.stdout)
            if coms:
                return coms[0]
        except Exception:
            pass

    # 方法 3: Linux/macOS 常规设备名
    for pattern in ("/dev/ttyACM*", "/dev/ttyUSB*", "/dev/cu.usb*", "/dev/cu.SLAB*"):
        import glob
        hits = sorted(glob.glob(pattern))
        if hits:
            return hits[0]
    return None


def read_expect():
    """读取 env_expect.json 快照"""
    if not os.path.isfile(EXPECT_FILE):
        return None
    try:
        with open(EXPECT_FILE, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def collect_env():
    """收集当前环境全部信息"""
    idf_path = get_idf_path()
    py_env = get_python_env_path(idf_path)
    # python 可执行文件绝对路径（供直接调用）
    if py_env:
        if platform.system() == "Windows":
            python_exe = os.path.join(py_env, "Scripts", "python.exe")
        else:
            python_exe = os.path.join(py_env, "bin", "python3")
    else:
        python_exe = None
    env = {
        "idf_path": idf_path,
        "idf_version": get_idf_version(idf_path),
        "python_env_path": py_env,
        "python_exe": python_exe,
        "port": get_serial_port(),
        "project_version": get_project_version(),
        "git_commit": get_git_repo_head(),
        "os": platform.system(),
    }
    return env


def render_human(env):
    """人类可读摘要"""
    lines = [
        "=== 开发环境检测 ===",
        f"操作系统       : {env['os']}",
        f"ESP-IDF 路径   : {env['idf_path'] or '(未找到)'}",
        f"ESP-IDF 版本   : {env['idf_version'] or '(未知)'}",
        f"Python 虚拟环境: {env['python_env_path'] or '(未找到)'}",
        f"Python 可执行  : {env['python_exe'] or '(未找到)'}",
        f"设备串口       : {env['port'] or '(未检测到)'}",
        f"项目版本       : {env['project_version'] or '(未知)'}",
        f"Git 提交       : {env['git_commit'] or '(未知)'}",
    ]
    return "\n".join(lines)


def render_export_ps1(env):
    """输出 PowerShell 环境变量赋值，可直接 Invoke-Expression / 手动执行"""
    lines = ["# 由 scripts/detect_env.py 生成的 PowerShell 环境变量（当前机器）"]
    if env["idf_path"]:
        lines.append(f"$env:IDF_PATH = '{env['idf_path']}'")
    if env["python_env_path"]:
        lines.append(f"$env:IDF_PYTHON_ENV_PATH = '{env['python_env_path']}'")
    if env["port"]:
        lines.append(f"$env:XIAOZHI_PORT = '{env['port']}'")
    if env["idf_path"]:
        lines.append(f"$env:IDF_VERSION = '{env['idf_version'] or ''}'")
    return "\n".join(lines)


def check_conflicts(env):
    """与 env_expect.json 快照对比，返回差异列表"""
    expect = read_expect()
    if not expect:
        return [("快照缺失", "scripts/env_expect.json 不存在，无法对比。"
                "运行 `python scripts/detect_env.py --export-ps1` 后手动生成快照。")]
    diffs = []
    mapping = {
        "idf_path": "ESP-IDF 路径",
        "idf_version": "ESP-IDF 版本",
        "python_env_path": "Python 虚拟环境",
        "port": "设备串口",
        "project_version": "项目版本",
    }
    for key, label in mapping.items():
        got = env.get(key)
        expected = expect.get(key)
        if got is None:
            diffs.append((f"{label} 缺失", f"当前环境未检测到 {label}"))
        elif expected is not None and got != expected:
            diffs.append((f"{label} 冲突",
                          f"快照={expected}，当前={got}（环境已迁移或被重新配置？）"))
    return diffs


# ===================== 健康检查（--health） =====================

# 本板期望的 sdkconfig 关键配置（与 sdkconfig.defaults + sdkconfig.defaults.esp32s3 对应）
EXPECTED_SDKCONFIG = {
    # (sdkconfig 键, 期望值, 说明)
    "CONFIG_IDF_TARGET": ("esp32s3", "目标芯片"),
    "CONFIG_BOARD_TYPE_WAVESHARE_S3_RLCD_4_2": ("y", "目标板"),
    "CONFIG_ESPTOOLPY_FLASHSIZE_16MB": ("y", "Flash 16MB"),
    "CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG": ("y", "Console=USB-JTAG"),
    "CONFIG_FATFS_CODEPAGE_936": ("y", "FATFS 中文 codepage"),
    "CONFIG_FATFS_API_ENCODING_UTF_8": ("y", "FATFS UTF-8 文件名"),
    "CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS": ("y", "唤醒词模型"),
}
# 这些键在 sdkconfig 中以 "# CONFIG_X is not set" 形式出现时也视为无效
NEGATIVE_KEYS = {
    "CONFIG_ESP_CONSOLE_UART_DEFAULT",   # 该板无 UART 桥接，必须为 not set
    "CONFIG_BOARD_TYPE_BREAD_COMPACT_ESP32",  # 其他板必须为 not set
}


def _run(cmd, timeout=60):
    """运行命令，返回 (returncode, stdout, stderr)"""
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout, p.stderr
    except FileNotFoundError:
        return -1, "", f"命令不存在: {cmd[0]}"
    except subprocess.TimeoutExpired:
        return -1, "", f"命令超时(>{timeout}s): {cmd}"
    except Exception as e:
        return -1, "", str(e)


def _python_exe(env):
    return env.get("python_exe") or sys.executable


def check_python_deps(env):
    """Python 依赖检查（IDF 约束对比）"""
    if not env["idf_path"] or not env["python_exe"]:
        return "WARN", "IDF 或 Python env 缺失，无法检查依赖"
    idf_tools = os.path.join(env["idf_path"], "tools", "idf_tools.py")
    if not os.path.isfile(idf_tools):
        return "WARN", f"idf_tools.py 不存在: {idf_tools}"
    rc, out, err = _run([env["python_exe"], idf_tools, "check-python-dependencies"], timeout=120)
    if rc == 0:
        return "PASS", "Python 依赖满足 IDF 约束"
    # 提取缺失/不匹配的依赖
    issues = [l.strip() for l in (out + "\n" + err).splitlines()
              if "Requirement '" in l or "was not met" in l or "was not found" in l]
    detail = "; ".join(issues[:5]) if issues else out.strip().splitlines()[-1] if out.strip() else "未知错误"
    return "FAIL", f"依赖不满足: {detail}"


def check_toolchain(env):
    """工具链检查（idf_tools.py check）"""
    if not env["idf_path"] or not env["python_exe"]:
        return "WARN", "IDF 或 Python env 缺失，无法检查工具链"
    idf_tools = os.path.join(env["idf_path"], "tools", "idf_tools.py")
    rc, out, err = _run([env["python_exe"], idf_tools, "check"], timeout=120)
    missing = [l.strip() for l in out.splitlines() if "required tool" in l.lower() or "not found" in l.lower()]
    if rc == 0:
        return "PASS", "工具链齐全（或未激活，工具在 tools 目录）"
    return "FAIL", f"工具链缺失: {'; '.join(missing[:5]) or 'idf_tools.py check 返回非零'}"


def check_venv_marker(env):
    """venv 版本标记检查（idf_version.txt 与当前 IDF 是否匹配）"""
    py_env = env.get("python_env_path")
    if not py_env or not env.get("idf_version"):
        return "WARN", "无 venv 或 IDF 版本，跳过"
    marker = os.path.join(py_env, "idf_version.txt")
    if not os.path.isfile(marker):
        return "WARN", f"venv 无版本标记文件 {marker}（旧版安装）"
    try:
        with open(marker, "r", encoding="utf-8") as f:
            marker_ver = f.read().strip()
    except Exception:
        return "WARN", "venv 版本标记读取失败"
    idf_ver = env["idf_version"]
    # IDF v5.5.2 -> 期望标记 "5.5"
    idf_mm = re.match(r"v?(\d+\.\d+)", idf_ver)
    expected = idf_mm.group(1) if idf_mm else idf_ver
    if marker_ver == expected or marker_ver in idf_ver:
        return "PASS", f"venv 标记匹配 (idf_version.txt={marker_ver})"
    return "FAIL", f"venv 标记不匹配: idf_version.txt={marker_ver}, 当前 IDF={idf_ver}（venv 已过时，需重新 install）"


def check_sdkconfig(env):
    """sdkconfig 关键配置检查（目标板/芯片/console/FATFS/唤醒词）"""
    sdk_path = os.path.join(REPO_ROOT, "sdkconfig")
    if not os.path.isfile(sdk_path):
        return "FAIL", "sdkconfig 不存在（首次构建需先 idf.py set-target esp32s3）"
    try:
        with open(sdk_path, "r", encoding="utf-8", errors="replace") as f:
            content = f.read()
    except Exception as e:
        return "FAIL", f"sdkconfig 读取失败: {e}"
    issues = []
    for key, (expected, label) in EXPECTED_SDKCONFIG.items():
        m = re.search(rf"^{re.escape(key)}=\"?([^\"\r\n]*)\"?", content, re.M)
        if m:
            got = m.group(1)
            if got != expected:
                issues.append(f"{label}: {key}={got}（期望 {expected}）")
        else:
            issues.append(f"{label}: 缺少 {key}")
    for key in NEGATIVE_KEYS:
        if re.search(rf"^{re.escape(key)}=y", content, re.M):
            issues.append(f"{key} 不应为 y（当前板不需要）")
    if issues:
        return "FAIL", "; ".join(issues)
    return "PASS", "sdkconfig 目标板/芯片/console/FATFS/唤醒词配置全部正确"


def check_serial(env):
    """串口设备检查"""
    port = env.get("port")
    if not port:
        return "FAIL", "未检测到串口（设备未连接或驱动未安装）"
    # 尝试确认端口可打开
    try:
        import serial
        s = serial.Serial(port, 115200, timeout=0.5)
        s.close()
        return "PASS", f"串口 {port} 可打开"
    except ImportError:
        return "PASS", f"串口 {port}（pyserial 不可用，仅报告存在）"
    except Exception as e:
        return "WARN", f"串口 {port} 检测到但打开失败: {e}（可能被 monitor 占用）"


def check_partition(env):
    """分区表文件检查"""
    p = os.path.join(REPO_ROOT, "partitions", "v2", "16m.csv")
    if os.path.isfile(p):
        return "PASS", f"分区表存在: partitions/v2/16m.csv"
    return "FAIL", "分区表缺失: partitions/v2/16m.csv"


def check_build_artifact(env):
    """构建产物检查（最新固件是否存在）"""
    b = os.path.join(REPO_ROOT, "build", "xiaozhi.bin")
    if os.path.isfile(b):
        size = os.path.getsize(b)
        mtime = time.strftime("%Y-%m-%d %H:%M", time.localtime(os.path.getmtime(b)))
        return "PASS", f"构建产物存在: build/xiaozhi.bin ({size//1024}KB, {mtime})"
    return "FAIL", "build/xiaozhi.bin 不存在（需先 idf.py build）"


def run_health(env):
    """执行全部健康检查，返回 (results, exit_code)"""
    checks = [
        ("IDF 路径", lambda: ("PASS" if env["idf_path"] else "FAIL",
                              env["idf_path"] or "未找到 ESP-IDF（检查 IDF_PATH 或安装）")),
        ("IDF 版本", lambda: ("PASS" if env["idf_version"] else "WARN",
                              env["idf_version"] or "未识别")),
        ("Python venv", lambda: ("PASS" if env["python_exe"] and os.path.isfile(env["python_exe"]) else "FAIL",
                                 env["python_exe"] or "未找到 Python 虚拟环境")),
        ("venv 版本匹配", lambda: check_venv_marker(env)),
        ("Python 依赖", lambda: check_python_deps(env)),
        ("工具链", lambda: check_toolchain(env)),
        ("串口设备", lambda: check_serial(env)),
        ("sdkconfig 芯片/板", lambda: check_sdkconfig(env)),
        ("分区表", lambda: check_partition(env)),
        ("构建产物", lambda: check_build_artifact(env)),
    ]
    results = []
    n_pass = n_fail = n_warn = 0
    for name, fn in checks:
        try:
            status, detail = fn()
        except Exception as e:
            status, detail = "WARN", f"检查异常: {e}"
        results.append((name, status, detail))
        if status == "PASS":
            n_pass += 1
        elif status == "FAIL":
            n_fail += 1
        else:
            n_warn += 1
    return results, n_pass, n_fail, n_warn


def render_health(results, n_pass, n_fail, n_warn):
    """渲染健康检查结果"""
    lines = ["=== 环境健康检查（开发/调试/烧录就绪度）==="]
    for name, status, detail in results:
        icon = {"PASS": "[PASS]", "FAIL": "[FAIL]", "WARN": "[WARN]"}[status]
        lines.append(f"  {icon} {name:<14} {detail}")
    lines.append(f"\n结果: {n_pass} PASS / {n_fail} FAIL / {n_warn} WARN")
    if n_fail == 0:
        lines.append(">>> 环境满足开发调试要求 ✅")
    else:
        lines.append(">>> 存在 FAIL 项，环境不满足开发调试要求 ❌（按提示修复后重跑）")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description="多环境开发环境检测")
    parser.add_argument("--json", action="store_true", help="输出 JSON")
    parser.add_argument("--check", action="store_true",
                        help="与 scripts/env_expect.json 快照对比，报告环境冲突")
    parser.add_argument("--health", action="store_true",
                        help="健康检查：验证环境是否满足开发/调试/烧录要求")
    parser.add_argument("--export-ps1", action="store_true",
                        help="输出 PowerShell 环境变量赋值")
    args = parser.parse_args()

    env = collect_env()

    if args.json:
        print(json.dumps(env, ensure_ascii=False, indent=2))
        return 0
    if args.health:
        results, n_pass, n_fail, n_warn = run_health(env)
        print(render_health(results, n_pass, n_fail, n_warn))
        return 2 if n_fail > 0 else 0
    if args.check:
        diffs = check_conflicts(env)
        print(render_human(env))
        if diffs:
            print("\n=== 环境冲突检测 ===")
            for label, detail in diffs:
                print(f"  [差异] {label}: {detail}")
            print("\n>>> 检测到环境差异。若已迁移环境，请更新 scripts/env_expect.json；"
                  "若快照过时，以检测结果为准。")
            return 2
        print("\n=== 环境一致性 ===")
        print("  OK: 与 env_expect.json 快照一致")
        return 0
    if args.export_ps1:
        print(render_export_ps1(env))
        return 0
    print(render_human(env))
    return 0


if __name__ == "__main__":
    sys.exit(main())
