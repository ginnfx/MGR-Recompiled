"""dmgbuild layout for the installer window: the app, an arrow, Applications.

Run by scripts/package-macos.sh, which passes -D app=... and -D icon=...
"""
import os.path

application = defines["app"]  # noqa: F821 (dmgbuild injects `defines`)
app_name = os.path.basename(application)

format = "ULFO"
filesystem = "HFS+"
files = [application]
symlinks = {"Applications": "/Applications"}
icon = defines["icon"]  # noqa: F821  volume icon

background = "builtin-arrow"
window_rect = ((200, 200), (640, 280))
default_view = "icon-view"
show_status_bar = False
show_tab_view = False
show_toolbar = False
show_pathbar = False
show_sidebar = False
icon_size = 128
text_size = 13
icon_locations = {app_name: (140, 120), "Applications": (500, 120)}
