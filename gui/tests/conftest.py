# SPDX-License-Identifier: MIT
"""Load NiceGUI's user fixture early enough to register its markers."""

pytest_plugins = ["nicegui.testing.user_plugin"]
