# Single source of truth for the version. Everything else derives from it:
# project(), the VERSIONINFO resource, and the tag check in CI.
#
# installer.toml carries the same number and CI asserts the two agree, because
# Forge stamps the installer from the TOML and cannot see this file.
set(DOCK_VERSION_MAJOR 0)
set(DOCK_VERSION_MINOR 3)
set(DOCK_VERSION_PATCH 0)

set(DOCK_VERSION "${DOCK_VERSION_MAJOR}.${DOCK_VERSION_MINOR}.${DOCK_VERSION_PATCH}")
set(DOCK_VERSION_RC "${DOCK_VERSION_MAJOR},${DOCK_VERSION_MINOR},${DOCK_VERSION_PATCH},0")
set(DOCK_VERSION_RC_STR "${DOCK_VERSION}.0")

set(DOCK_PRODUCT   "Docked Console")
set(DOCK_COMPANY   "Locke Werks")
set(DOCK_COPYRIGHT "Copyright (C) 2026 Locke Werks")
