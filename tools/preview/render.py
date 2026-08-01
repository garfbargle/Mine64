"""Draw Mine64's models on this machine, without a console or an emulator.

Every model in the game is a handful of untextured boxes and quads pushed
through guRotateRPY and guPerspective, and none of that needs an N64 to
evaluate.  This module reads the real vertex data out of src/graphics.c, runs
the same arithmetic the RSP would, and rasterises the result to a PNG in about
a second -- so a question like "which way does the head end up facing?" can be
answered in one command instead of a build, a ROM, and a scripted emulator run.

What it reproduces faithfully:

  * guRotateRPY's matrix, row-vector (v' = v * M), X then Y then Z -- the same
    layout setMobRotation builds by hand.
  * guPerspective at the game's FOV_Y, 320x240, and its near plane.
  * G_ZBUFFER, G_SHADE | G_SHADING_SMOOTH and G_CULL_BACK: a z-buffer,
    per-vertex colours interpolated across the triangle, and back faces
    dropped by screen-space winding.

What it does not: textures, fog, the CPU-side game logic that decides the
angles, and the RDP's exact fixed-point rounding.  It answers questions about
pose, framing and occlusion.  It cannot answer "does this run at 20fps" or
"does the RDP lock up", and it is not a substitute for looking at the ROM once
the geometry is settled.

Typical use is a ten-line script that imports this and poses something:

    from render import Geometry, Scene, rpy
    g = Geometry.load()
    s = Scene()
    s.add(g.mesh("pig_head_verts"), rpy(0, 40, 0), (0, 46, -16))
    s.look_at((0, 40, 0), distance=200, yaw=25)
    s.save("head.png")
"""

import math
import os
import re

import numpy as np
from PIL import Image, ImageDraw

DTOR = math.pi / 180.0
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GRAPHICS_C = os.path.join(ROOT, "src", "graphics.c")

# graphics.c: 320x240 framebuffer, FOV_Y 60, near 10, far 6400.
WIDTH, HEIGHT = 320, 240
FOVY, NEAR, FAR = 60.0, 10.0, 6400.0


# ---------------------------------------------------------------- matrices

def rpy(pitch, yaw, roll, scale=1.0):
    """guRotateRPYF's 3x3, row-vector: v' = v * M.  Matches setMobRotation."""
    p, y, r = pitch * DTOR, yaw * DTOR, roll * DTOR
    sp, cp = math.sin(p), math.cos(p)
    sy, cy = math.sin(y), math.cos(y)
    sr, cr = math.sin(r), math.cos(r)
    return np.array([
        [cy * cr,                     cy * sr,                     -sy    ],
        [sp * sy * cr - cp * sr,      sp * sy * sr + cp * cr,      sp * cy],
        [cp * sy * cr + sp * sr,      cp * sy * sr - sp * cr,      cp * cy],
    ]) * scale


IDENTITY = rpy(0, 0, 0)


# ---------------------------------------------------------------- geometry

# `static` is optional: a model array shared with another translation unit --
# the face sheet the player and 64MON's trainers both wear -- is still a model
# array, and a preview that stopped seeing it would quietly draw a faceless
# head instead of saying anything.
_VTX_ARRAY = re.compile(
    r"(?:static\s+)?Vtx\s+(\w+)\[\]\s*=\s*\{(.*?)\}", re.S)
_DEFINE_FN = re.compile(r"^#define\s+(\w+)\(([^)]*)\)\s*(.*)$", re.M)
_DEFINE_ALIAS = re.compile(r"^#define\s+(\w+)\s+(\w+)\s*$", re.M)
_STEVE_VERTEX = re.compile(r"STEVE_VERTEX\(([^)]*)\)")
_GFX_ARRAY = re.compile(
    r"^(?:static\s+)?Gfx\s+(\w+)\[\]\s*=\s*\{(.*?)\};", re.M | re.S)
_TRIANGLES = re.compile(r"gsSP(2?)Triangles?\(([^)]*)\)")
_STRUCT = re.compile(
    r"^static\s+const\s+(\w+)\s+(\w+)\s*=\s*\{(.*?)\};", re.M | re.S)


def _ints(text):
    return [int(v) for v in re.findall(r"-?\d+", text)]


def _split_args(text):
    """Top-level commas only; the macros here never nest a call in an argument."""
    args, depth, current = [], 0, ""
    for ch in text:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            args.append(current.strip())
            current = ""
        else:
            current += ch
    args.append(current.strip())
    return args


class Mesh(object):
    """Vertices (x, y, z, r, g, b) plus the triangle list a display list draws."""

    def __init__(self, verts, tris):
        self.verts = np.array([v[:3] for v in verts], float)
        self.colors = np.array([v[3:] for v in verts], float)
        self.tris = tris

    def __len__(self):
        return len(self.tris)


class Geometry(object):
    """Every Vtx and Gfx array in graphics.c, parsed straight from the source."""

    def __init__(self, verts, lists, structs):
        self.verts = verts
        self.lists = lists
        self.structs = structs

    @classmethod
    def load(cls, path=GRAPHICS_C):
        with open(path) as handle:
            src = re.sub(r"\\\n\s*", " ", handle.read())

        # DETAIL_BOX, MOB_BOX, SWORD_BLADE_VERTS and friends all expand to a
        # `static Vtx name[]`, so expanding the macros beats special-casing
        # each one -- a new model macro needs no change here.
        macros = {n: (_split_args(p), b) for n, p, b in _DEFINE_FN.findall(src)
                  if "static Vtx" in b}
        for alias, target in _DEFINE_ALIAS.findall(src):
            if target in macros:
                macros[alias] = macros[target]

        # Directives are gone from the body text: a macro's own definition
        # would otherwise read as an array whose coordinates are parameter
        # names.
        expanded = [re.sub(r"^#define.*$", "", src, flags=re.M)]
        for name, (params, body) in macros.items():
            for call in re.finditer(r"^%s\((.*?)\);" % name, src, re.M | re.S):
                args = _split_args(call.group(1))
                if len(args) != len(params):
                    continue
                text = body
                for param, value in zip(params, args):
                    text = re.sub(r"\b%s\b" % re.escape(param), value, text)
                expanded.append(text)

        verts = {}
        for chunk in expanded:
            for name, body in _VTX_ARRAY.findall(chunk):
                rows = [_ints(m) for m in _STEVE_VERTEX.findall(body)]
                if rows and all(len(r) == 6 for r in rows):
                    verts[name] = [tuple(r) for r in rows]

        lists = {}
        for name, body in _GFX_ARRAY.findall(src):
            tris = []
            for pair, args in _TRIANGLES.findall(body):
                n = _ints(args)
                tris.append(tuple(n[0:3]))
                if pair:
                    tris.append(tuple(n[4:7]))
            if tris:
                lists[name] = tris

        structs = {}
        for kind, name, body in _STRUCT.findall(src):
            structs[name] = (kind, [t.strip() for t in
                                    re.sub(r"/\*.*?\*/", "", body, flags=re.S)
                                    .replace("\n", " ").split(",")])
        return cls(verts, lists, structs)

    def mesh(self, verts_name, list_name="steve_box_display_list", count=None):
        v = self.verts[verts_name]
        if count:
            v = v[:count]
        tris = [t for t in self.lists[list_name] if max(t) < len(v)]
        return Mesh(v, tris)

    def fields(self, struct_name):
        """The initialiser of a `static const T name = {...}` as a flat list."""
        return self.structs[struct_name][1]


def define(name, source="src/mobs.c"):
    """Read a numeric #define, so a preview quotes the game's own constant."""
    with open(os.path.join(ROOT, source)) as handle:
        match = re.search(r"^#define\s+%s\s+(-?[\d.]+)f?\s*$" % name,
                          handle.read(), re.M)
    if not match:
        raise KeyError("%s is not a numeric #define in %s" % (name, source))
    return float(match.group(1))


# ---------------------------------------------------------------- the scene

class Scene(object):
    """A pile of posed meshes and a camera, rasterised the way the RDP would."""

    def __init__(self, width=WIDTH, height=HEIGHT, background=(88, 132, 196)):
        self.width, self.height = width, height
        self.background = background
        self.parts = []
        self.eye = np.array([0.0, 0.0, 240.0])
        self.center = np.array([0.0, 0.0, 0.0])
        self.cull = True

    def add(self, mesh, rotation=None, translation=(0, 0, 0)):
        """Place a mesh: v' = v * rotation + translation, as gSPMatrix stacks."""
        r = IDENTITY if rotation is None else rotation
        self.parts.append((mesh, r, np.array(translation, float)))
        return self

    def ground(self, y=0, extent=512, tile=64,
               colors=((92, 140, 62), (104, 154, 70))):
        """A checkered plane so a floating model reads as standing on something."""
        verts, tris = [], []
        steps = int(extent * 2 / tile)
        for ix in range(steps):
            for iz in range(steps):
                x0, z0 = -extent + ix * tile, -extent + iz * tile
                c = colors[(ix + iz) % 2]
                base = len(verts)
                for x, z in ((x0, z0), (x0 + tile, z0),
                             (x0 + tile, z0 + tile), (x0, z0 + tile)):
                    verts.append((x, y, z) + tuple(c))
                tris += [(base, base + 1, base + 2), (base, base + 2, base + 3)]
        mesh = Mesh(verts, tris)
        self.parts.append((mesh, IDENTITY, np.zeros(3)))
        return self

    def camera_space(self):
        """For models drawn after loadCameraProjection: the eye is the origin."""
        self.eye = np.zeros(3)
        self.center = np.array([0.0, 0.0, -1.0])
        return self

    def look_at(self, center, distance=260.0, yaw=30.0, pitch=12.0):
        """Orbit the camera around a point: yaw in degrees, +yaw to the right."""
        self.center = np.array(center, float)
        y, p = yaw * DTOR, pitch * DTOR
        self.eye = self.center + np.array([
            math.sin(y) * math.cos(p), math.sin(p), math.cos(y) * math.cos(p)
        ]) * distance
        return self

    # ------------------------------------------------------------ internals

    def _view(self):
        forward = self.center - self.eye
        forward /= np.linalg.norm(forward)
        right = np.cross(forward, np.array([0.0, 1.0, 0.0]))
        right /= np.linalg.norm(right)
        up = np.cross(right, forward)
        return np.array([right, up, -forward]).T  # world -> view, row-vector

    def _project(self, p):
        f = 1.0 / math.tan(FOVY * DTOR / 2)
        aspect = float(self.width) / self.height
        z = -p[:, 2]
        return np.stack([
            (p[:, 0] * f / aspect / z * 0.5 + 0.5) * self.width,
            (0.5 - p[:, 1] * f / z * 0.5) * self.height,
            z,
        ], axis=1)

    def _clip_near(self, poly, cols):
        """Clip a view-space triangle against z <= -NEAR, carrying colours."""
        out_p, out_c = [], []
        for i in range(len(poly)):
            a, b = poly[i], poly[(i + 1) % len(poly)]
            ca, cb = cols[i], cols[(i + 1) % len(cols)]
            a_in, b_in = a[2] <= -NEAR, b[2] <= -NEAR
            if a_in:
                out_p.append(a)
                out_c.append(ca)
            if a_in != b_in:
                t = (-NEAR - a[2]) / (b[2] - a[2])
                out_p.append(a + (b - a) * t)
                out_c.append(ca + (cb - ca) * t)
        return out_p, out_c

    def image(self):
        img = np.zeros((self.height, self.width, 3), np.float64)
        img[:] = self.background
        zbuf = np.full((self.height, self.width), np.inf)
        view = self._view()

        for mesh, rot, trans in self.parts:
            world = mesh.verts @ rot + trans
            eye = (world - self.eye) @ view
            for tri in mesh.tris:
                poly, cols = self._clip_near([eye[i] for i in tri],
                                             [mesh.colors[i] for i in tri])
                if len(poly) < 3:
                    continue
                screen = self._project(np.array(poly))
                for k in range(1, len(poly) - 1):
                    self._raster(img, zbuf, screen[[0, k, k + 1]],
                                 [cols[0], cols[k], cols[k + 1]])
        return Image.fromarray(img.clip(0, 255).astype(np.uint8))

    def _raster(self, img, zbuf, s, cols):
        a, b, c = s
        area = (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1])
        # Front faces are clockwise seen from outside; the y-down raster flips
        # that to a positive signed area.  G_CULL_BACK drops the rest.
        if area <= 0 if self.cull else area == 0:
            return
        x0 = max(int(min(a[0], b[0], c[0])), 0)
        x1 = min(int(max(a[0], b[0], c[0])) + 1, self.width)
        y0 = max(int(min(a[1], b[1], c[1])), 0)
        y1 = min(int(max(a[1], b[1], c[1])) + 1, self.height)
        if x0 >= x1 or y0 >= y1:
            return

        ys, xs = np.mgrid[y0:y1, x0:x1]
        px, py = xs + 0.5, ys + 0.5
        w0 = ((b[1] - c[1]) * (px - c[0]) + (c[0] - b[0]) * (py - c[1]))
        w1 = ((c[1] - a[1]) * (px - c[0]) + (a[0] - c[0]) * (py - c[1]))
        d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
        if abs(d) < 1e-9:
            return
        w0, w1 = w0 / d, w1 / d
        w2 = 1.0 - w0 - w1
        mask = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        if not mask.any():
            return

        inv_z = w0 / a[2] + w1 / b[2] + w2 / c[2]
        depth = np.where(inv_z > 0, 1.0 / np.maximum(inv_z, 1e-9), np.inf)
        window = zbuf[y0:y1, x0:x1]
        mask &= depth < window
        if not mask.any():
            return
        window[mask] = depth[mask]
        color = (w0[..., None] * cols[0] + w1[..., None] * cols[1] +
                 w2[..., None] * cols[2])
        region = img[y0:y1, x0:x1]
        region[mask] = color[mask]

    def image_with_crosshair(self):
        """drawCrosshair's mark, which is what a first-person pose is judged against."""
        img = self.image()
        draw = ImageDraw.Draw(img)
        cx, cy = self.width // 2, self.height // 2
        draw.line((cx - 5, cy, cx + 5, cy), fill=(255, 255, 255))
        draw.line((cx, cy - 5, cx, cy + 5), fill=(255, 255, 255))
        return img

    def save(self, path):
        img = self.image()
        img.save(path)
        return path


# ---------------------------------------------------------------- filmstrip

def filmstrip(images, path, labels=None, scale=1, gap=4,
              background=(18, 18, 22), text=(255, 235, 150)):
    """Lay frames out left to right, which is how a motion reads on a page."""
    frames = [im if scale == 1 else
              im.resize((im.width * scale, im.height * scale), Image.NEAREST)
              for im in images]
    head = 16 if labels else 0
    w = sum(f.width for f in frames) + gap * (len(frames) + 1)
    out = Image.new("RGB", (w, frames[0].height + head + gap * 2), background)
    draw = ImageDraw.Draw(out)
    x = gap
    for i, frame in enumerate(frames):
        out.paste(frame, (x, head + gap))
        if labels:
            draw.text((x + 4, 3), labels[i], fill=text)
        x += frame.width + gap
    out.save(path)
    return path
