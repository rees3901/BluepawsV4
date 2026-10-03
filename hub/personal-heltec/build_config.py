"""Build canonical Heltec services and the personal UI with private configuration."""
from pathlib import Path
Import("env")
header = Path(env.subst("$PROJECT_DIR")) / ".secrets" / "hub_secrets.h"
if not header.is_file():
    raise RuntimeError("Provision .secrets/hub_secrets.h before building the personal hub")
env.Append(CPPPATH=[str(header.parent)])
