"""PlatformIO pre-build hook: UTC epoch, independent of host locale/timezone.

This is an approximate bench clock anchor, not a replacement for GNSS time.
Never parse __DATE__/__TIME__: those macros use the compiler's local timezone.
"""
import time

Import("env")  # Provided by PlatformIO/SCons.
env.Append(CPPDEFINES=[("BLUEPAWS_BUILD_UNIX_TIME", int(time.time()))])
