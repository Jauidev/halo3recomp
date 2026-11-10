"""Makes the app's adaptive icon in LAUNCHER/res from "LAUNCHER/ICONO DEL LAUNCHER.png". That image
is a picture with rounded corners on black, without transparency. The launcher's mask shows at most
the middle 72 of the icon's 108 dp, so the picture goes in the middle 80 dp, on a black background:
every mask shape falls inside it and the black corners never show. Needs Pillow.

A game's own icon (games/<game>/LAUNCHER/res, which build-apk.sh prefers) comes from a photo without
borders: a square of it fills the whole icon, and the mask shows its middle two thirds.

usage: py -3 tools/make_icon.py     (run from the project root)
       py -3 tools/make_icon.py <photo> <x0,y0,x1,y1> <res folder>
       e.g. games/gears_of_war/LAUNCHER/FONDO.png 0,100,560,660 games/gears_of_war/LAUNCHER/res
"""
import os
import sys
from PIL import Image

SOURCE = "LAUNCHER/ICONO DEL LAUNCHER.png"
RESOURCES = "LAUNCHER/res"
# The picture inside the source file, without the black margins.
PICTURE = (16, 0, 1250, 1234)
CANVAS_DP, PICTURE_DP = 108, 80
if len(sys.argv) == 4:
    SOURCE, RESOURCES = sys.argv[1], sys.argv[3]
    PICTURE = tuple(int(v) for v in sys.argv[2].split(","))
    PICTURE_DP = CANVAS_DP
DENSITIES = {"mdpi": 1.0, "hdpi": 1.5, "xhdpi": 2.0, "xxhdpi": 3.0, "xxxhdpi": 4.0}
ADAPTIVE = """<?xml version="1.0" encoding="utf-8"?>
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@android:color/black" />
    <foreground android:drawable="@mipmap/ic_launcher_foreground" />
</adaptive-icon>
"""

picture = Image.open(SOURCE).convert("RGB").crop(PICTURE)
for density, scale in DENSITIES.items():
    canvas_size, picture_size = round(CANVAS_DP * scale), round(PICTURE_DP * scale)
    canvas = Image.new("RGB", (canvas_size, canvas_size), (0, 0, 0))
    corner = (canvas_size - picture_size) // 2
    canvas.paste(picture.resize((picture_size, picture_size), Image.LANCZOS), (corner, corner))
    folder = os.path.join(RESOURCES, "mipmap-" + density)
    os.makedirs(folder, exist_ok=True)
    canvas.save(os.path.join(folder, "ic_launcher_foreground.png"), optimize=True)

folder = os.path.join(RESOURCES, "mipmap-anydpi-v26")
os.makedirs(folder, exist_ok=True)
with open(os.path.join(folder, "ic_launcher.xml"), "w", encoding="utf-8", newline="\n") as file:
    file.write(ADAPTIVE)
