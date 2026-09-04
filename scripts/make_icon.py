"""Renders the NX-Nexus mark as the 256x256 JPEG the NRO format wants.

Drawn with PIL rather than rasterised from the SVG so the build has no
extra dependency, and supersampled 4x because the Switch home menu shows
this icon large enough that aliased edges are obvious.
"""
import math
from PIL import Image, ImageDraw

S = 4                      # supersample factor
SIZE = 256
W = SIZE * S

BG = (15, 23, 42)          # slate-900, matching the logo's node fill
C0 = (56, 189, 248)        # sky-400
C1 = (129, 140, 248)       # indigo-400

img = Image.new("RGB", (W, W), BG)
d = ImageDraw.Draw(img)

cx = cy = W / 2
R = W * 0.30               # satellite orbit radius
NODE = W * 0.052
HUB = W * 0.105
HUB_CORE = W * 0.048
STROKE = int(W * 0.028)


def lerp(a, b, t):
    return tuple(int(round(x + (y - x) * t)) for x, y in zip(a, b))


# Six satellites, as in the logo: a hub with links radiating out.
points = []
for i in range(6):
    ang = math.radians(-90 + i * 60)
    points.append((cx + R * math.cos(ang), cy + R * math.sin(ang)))

# Edges first, so the nodes sit on top of them.
for i, (x, y) in enumerate(points):
    d.line([cx, cy, x, y], fill=lerp(C0, C1, i / 5), width=STROKE)

for i, (x, y) in enumerate(points):
    col = lerp(C0, C1, i / 5)
    d.ellipse([x - NODE, y - NODE, x + NODE, y + NODE], fill=BG, outline=col,
              width=STROKE)

d.ellipse([cx - HUB, cy - HUB, cx + HUB, cy + HUB], fill=C0)
d.ellipse([cx - HUB_CORE, cy - HUB_CORE, cx + HUB_CORE, cy + HUB_CORE], fill=BG)

img.resize((SIZE, SIZE), Image.LANCZOS).save("icon.jpg", "JPEG", quality=95)
print("wrote icon.jpg 256x256")
