"""Embed the Walter testbed source revision for serial build identification."""

import subprocess

Import("env")  # Provided by PlatformIO/SCons.

project_dir = env.subst("$PROJECT_DIR")
try:
    revision = subprocess.check_output(
        ["git", "rev-parse", "--short=12", "HEAD"], cwd=project_dir, text=True
    ).strip()
    if subprocess.check_output(
        ["git", "status", "--porcelain", "--untracked-files=normal"], cwd=project_dir
    ):
        revision += "-dirty"
except (OSError, subprocess.CalledProcessError):
    revision = "unknown"

env.Append(CPPDEFINES=[("BLUEPAWS_WALTER_SOURCE_REV", f'\\"{revision}\\"')])
