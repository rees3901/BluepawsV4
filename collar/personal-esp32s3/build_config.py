"""Require local provisioning for every flashable build; never print secrets."""
from pathlib import Path
Import("env")

project = Path(env.subst("$PROJECT_DIR"))
if env.subst("$PIOENV") == "personal_compile_check":
    if any("upload" in str(t).lower() for t in COMMAND_LINE_TARGETS):
        raise RuntimeError("Compile-check firmware cannot be uploaded")
else:
    header = project / ".secrets" / "personal_config.h"
    if not header.is_file():
        raise RuntimeError("Provision .secrets/personal_config.h before building live firmware; see README.md")
    env.Append(CPPPATH=[str(header.parent)])
