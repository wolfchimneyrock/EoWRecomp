#!/usr/bin/env python3
"""Build "Open Pad Glyphs": an original, freely licensed controller-button font for the Switch's private-use
button codepoints (the characters games ask the NintendoExt shared font for), so recompiled Switch games can draw
their in-text button prompts without the console's font.

Every shape is drawn here from circles, rounded rectangles, polygons and strokes. Button letters and digits are
outlines from Baloo 2 (SIL OFL 1.1, Google Fonts / EkType), so the output is licensed under the SIL OFL 1.1 too.
No glyph, outline or table is taken from any console font; only the public codepoint conventions are used.

Usage:  python3 build_padglyphs.py [--baloo Baloo2[wght].ttf] [--out DIR] [--preview]
Writes OpenPadGlyphs.ttf (Switch letters, the default), OpenPadGlyphs-XboxLabels.ttf and OpenPadGlyphs-PSLabels.ttf.
The label variants keep the Switch's positional layout: the east face button (Switch A) reads B / circle, south
(Switch B) reads A / cross, north (X) reads Y / triangle, west (Y) reads X / square.

Needs: fonttools, skia-pathops (pip install fonttools skia-pathops; Pillow for --preview).
"""
import argparse
import math
import os
import sys

import pathops
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.cu2quPen import Cu2QuPen
from fontTools.pens.transformPen import TransformPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont

HERE = os.path.dirname(os.path.abspath(__file__))
VERSION = "1.000"
FAMILY = "Open Pad Glyphs"

# ---- metrics ------------------------------------------------------------------------------------------------------
# The game's font engine sizes and places a shared font by its OS/2 Windows metrics (ascent 1158 + descent 378 of
# 1024 units, the same values LASRecomp gives every substitute font) and draws each button as a full-width
# character: 1024 units wide, the shape filling about 980 units of height centred a little above the baseline.
UPEM = 1024
ADV = 1024
CX, CY = 512, 390     # centre of a button
R = 486               # radius of a full-size round button (bbox ~ -96 .. 876)
T = 64                # outline thickness
WIN_ASC, WIN_DESC = 1158, 378
TYPO_ASC, TYPO_DESC, LINE_GAP = 901, -123, 512

K = 0.5522847498      # cubic circle constant


# ---- path helpers -------------------------------------------------------------------------------------------------
def P():
    return pathops.Path()


def union(*ps):
    out = P()
    for p in ps:
        if p is None:
            continue
        out = pathops.op(out, p, pathops.PathOp.UNION, fix_winding=True)
    return out


def diff(a, *bs):
    for b in bs:
        a = pathops.op(a, b, pathops.PathOp.DIFFERENCE, fix_winding=True)
    return a


def inter(a, b):
    return pathops.op(a, b, pathops.PathOp.INTERSECTION, fix_winding=True)


def ellipse(cx, cy, rx, ry):
    p = P()
    pen = p.getPen()
    pen.moveTo((cx + rx, cy))
    pen.curveTo((cx + rx, cy + K * ry), (cx + K * rx, cy + ry), (cx, cy + ry))
    pen.curveTo((cx - K * rx, cy + ry), (cx - rx, cy + K * ry), (cx - rx, cy))
    pen.curveTo((cx - rx, cy - K * ry), (cx - K * rx, cy - ry), (cx, cy - ry))
    pen.curveTo((cx + K * rx, cy - ry), (cx + rx, cy - K * ry), (cx + rx, cy))
    pen.closePath()
    return p


def circle(cx, cy, r):
    return ellipse(cx, cy, r, r)


def ring(cx, cy, r, t=T):
    return diff(circle(cx, cy, r), circle(cx, cy, r - t))


def rrect(x0, y0, x1, y1, r, rtl=None, rtr=None, rbl=None, rbr=None):
    """Rounded rectangle; per-corner radii optional (top-left, top-right, bottom-left, bottom-right)."""
    rtl = r if rtl is None else rtl
    rtr = r if rtr is None else rtr
    rbl = r if rbl is None else rbl
    rbr = r if rbr is None else rbr
    p = P()
    pen = p.getPen()
    pen.moveTo((x0 + rbl, y0))
    pen.lineTo((x1 - rbr, y0))
    pen.curveTo((x1 - rbr + K * rbr, y0), (x1, y0 + rbr - K * rbr), (x1, y0 + rbr))
    pen.lineTo((x1, y1 - rtr))
    pen.curveTo((x1, y1 - rtr + K * rtr), (x1 - rtr + K * rtr, y1), (x1 - rtr, y1))
    pen.lineTo((x0 + rtl, y1))
    pen.curveTo((x0 + rtl - K * rtl, y1), (x0, y1 - rtl + K * rtl), (x0, y1 - rtl))
    pen.lineTo((x0, y0 + rbl))
    pen.curveTo((x0, y0 + rbl - K * rbl), (x0 + rbl - K * rbl, y0), (x0 + rbl, y0))
    pen.closePath()
    return p


def poly(pts):
    p = P()
    pen = p.getPen()
    pen.moveTo(pts[0])
    for q in pts[1:]:
        pen.lineTo(q)
    pen.closePath()
    return pathops.simplify(p, fix_winding=True)


def stroke(pts, w, closed=False, cap="round", join="round"):
    p = P()
    pen = p.getPen()
    pen.moveTo(pts[0])
    for q in pts[1:]:
        pen.lineTo(q)
    if closed:
        pen.closePath()
    else:
        pen.endPath()
    p.stroke(w, {"round": pathops.LineCap.ROUND_CAP, "butt": pathops.LineCap.BUTT_CAP,
                 "square": pathops.LineCap.SQUARE_CAP}[cap],
             {"round": pathops.LineJoin.ROUND_JOIN, "miter": pathops.LineJoin.MITER_JOIN}[join], 4)
    p.convertConicsToQuads()
    return pathops.simplify(p, fix_winding=True)


def arc_pts(cx, cy, rx, ry, a0, a1, n=48):
    return [(cx + rx * math.cos(math.radians(a0 + (a1 - a0) * i / n)),
             cy + ry * math.sin(math.radians(a0 + (a1 - a0) * i / n))) for i in range(n + 1)]


def xform(p, xx=1, xy=0, yx=0, yy=1, dx=0, dy=0):
    out = P()
    p.draw(TransformPen(out.getPen(), (xx, xy, yx, yy, dx, dy)))
    return out


def rot(p, deg, cx=CX, cy=CY):
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    return xform(p, c, s, -s, c, cx - c * cx + s * cy, cy - s * cx - c * cy)


def scale(p, k, cx=CX, cy=CY):
    return xform(p, k, 0, 0, k, cx - k * cx, cy - k * cy)


def move(p, dx, dy):
    return xform(p, 1, 0, 0, 1, dx, dy)


def mirror_x(p, cx=CX):
    return xform(p, -1, 0, 0, 1, 2 * cx, 0)


def bounds(p):
    return p.bounds


# ---- text (Baloo 2) -----------------------------------------------------------------------------------------------
class Letters:
    def __init__(self, path, wght):
        from fontTools.varLib import instancer
        f = TTFont(path)
        if "fvar" in f:
            f = instancer.instantiateVariableFont(f, {"wght": wght})
        self.font = f
        self.gs = f.getGlyphSet()
        self.cmap = f.getBestCmap()
        self.upem = f["head"].unitsPerEm

    def path(self, text, height, cx=CX, cy=CY, max_w=None, ref="H", tracking=0.0):
        """Text centred on (cx, cy); `height` is the cap height (of `ref`) in font units."""
        k = height / self._h(ref)
        out = P()
        x = 0.0
        for ch in text:
            gn = self.cmap[ord(ch)]
            g = P()
            self.gs[gn].draw(TransformPen(g.getPen(), (1, 0, 0, 1, x, 0)))
            out = union(out, g)
            x += self.gs[gn].width + tracking * self.upem
        x0, y0, x1, y1 = out.bounds
        if max_w and (x1 - x0) * k > max_w:
            k = max_w / (x1 - x0)
        # centre the ink box horizontally; vertically centre the cap height (so baselines line up between letters)
        capk = self._h(ref) * k
        return xform(out, k, 0, 0, k, cx - (x0 + x1) / 2 * k, cy - capk / 2)

    def ink(self, text, height, cx=CX, cy=CY, max_w=None):
        """Like path() but centres the ink box both ways (for symbols such as ! ? + digits)."""
        p = self.path(text, height, cx, cy, max_w)
        x0, y0, x1, y1 = p.bounds
        return move(p, cx - (x0 + x1) / 2, cy - (y0 + y1) / 2)

    def _h(self, ch):
        bp = BoundsPen(self.gs)
        self.gs[self.cmap[ord(ch)]].draw(bp)
        return bp.bounds[3]


# ---- labels -------------------------------------------------------------------------------------------------------
# Controller buttons ("hosts") by SDL3's SDL_GamepadButton numbers (+ the triggers as 26 / 27, LASRecomp's PADB_*).
SOUTH, EAST, WEST, NORTH, BACK, START, LSTICK, RSTICK, LB, RB, DUP, DDOWN, DLEFT, DRIGHT, LT, RT = \
    0, 1, 2, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 26, 27
HOSTS = (SOUTH, EAST, WEST, NORTH, BACK, START, LSTICK, RSTICK, LB, RB, DUP, DDOWN, DLEFT, DRIGHT, LT, RT)
# what each controller family prints on those buttons
FAMILIES = ("nintendo", "xbox", "ps")
_DPAD = {DUP: "@up", DDOWN: "@down", DLEFT: "@left", DRIGHT: "@right"}
HOST_LABEL = {
    "nintendo": {**_DPAD, EAST: "A", SOUTH: "B", NORTH: "X", WEST: "Y", LB: "L", RB: "R", LT: "ZL", RT: "ZR",
                      START: "+", BACK: "-", LSTICK: "LS", RSTICK: "RS"},
    "xbox": {**_DPAD, EAST: "B", SOUTH: "A", NORTH: "Y", WEST: "X", LB: "LB", RB: "RB", LT: "LT", RT: "RT",
                  START: "menu", BACK: "view", LSTICK: "LS", RSTICK: "RS"},
    "ps": {**_DPAD, EAST: "@circle", SOUTH: "@cross", NORTH: "@triangle", WEST: "@square", LB: "L1", RB: "R1", LT: "L2",
                RT: "R2", START: "START", BACK: "SELECT", LSTICK: "L3", RSTICK: "R3"},
}
# Switch inputs (the runtimes' BIND_* slot numbers) and the controller button each is on by default: by position,
# the Switch's own layout (east = A, south = B, north = X, west = Y)
ROLES = {"A": 0, "B": 1, "X": 2, "Y": 3, "L": 6, "R": 7, "ZL": 8, "ZR": 9, "PLUS": 10, "MINUS": 11}
DEFAULT_HOST = {"A": EAST, "B": SOUTH, "X": NORTH, "Y": WEST, "L": LB, "R": RB, "ZL": LT, "ZR": RT, "PLUS": START,
                "MINUS": BACK}
# The characters that name one Switch input, so they follow the binding: the runtime points each at the glyph drawn
# for (family, controller button now driving that input), found at U+F0000 + family << 14 + button << 9 + (cp - E000)
ROLE_OF = {}
for _role, _cps in (("A", (0xE000, 0xE0A0, 0xE0E0, 0xE042, 0xE04C, 0xE0AB)), ("B", (0xE001, 0xE0A1, 0xE0E1, 0xE04D, 0xE0AC)),
                    ("X", (0xE002, 0xE0A2, 0xE0E2, 0xE04E, 0xE0AD)), ("Y", (0xE003, 0xE0A3, 0xE0E3, 0xE04F, 0xE0AE)),
                    ("L", (0xE004, 0xE052, 0xE083, 0xE0A4, 0xE0E4)), ("R", (0xE005, 0xE053, 0xE084, 0xE0A5, 0xE0E5)),
                    ("ZL", (0xE054, 0xE085, 0xE0A6, 0xE0E6)), ("ZR", (0xE055, 0xE086, 0xE0A7, 0xE0E7)),
                    ("PLUS", (0xE045, 0xE0B3, 0xE0EF, 0xE0B5, 0xE0F1)), ("MINUS", (0xE046, 0xE0B4, 0xE0F0, 0xE0B6, 0xE0F2))):
    for _cp in _cps:
        ROLE_OF[_cp] = _role


def alt_cp(family, host, cp):
    return 0xF0000 + (FAMILIES.index(family) << 14) + (host << 9) + (cp - 0xE000)


class Builder:
    def __init__(self, letters, family, hostmap=None):
        self.L = letters
        self.family = family
        self.hostmap = dict(DEFAULT_HOST, **(hostmap or {}))

    def text_of(self, role_or_text):
        if role_or_text.startswith("="):  # a literal label, never a Switch input's
            return role_or_text[1:]
        r = role_or_text.upper() if role_or_text in ("a", "b", "x", "y") else role_or_text
        if r not in ROLES:
            return role_or_text
        t = HOST_LABEL[self.family][self.hostmap[r]]
        return t.lower() if role_or_text in ("a", "b", "x", "y") and len(t) == 1 and t.isalpha() else t

    # a label (letters or a PlayStation symbol) fitted in a box of the given height, centred at (cx, cy)
    def label(self, role_or_text, h, cx=CX, cy=CY, max_w=None, weight=None):
        t = self.text_of(role_or_text)
        sw = weight or h * 0.2
        if t in ("@up", "@down", "@left", "@right"):
            r = h * 0.5
            ang = {"@up": 90, "@down": -90, "@left": 180, "@right": 0}[t]
            return rot(poly([(cx + r, cy), (cx - r * 0.6, cy + r * 0.9), (cx - r * 0.6, cy - r * 0.9)]), ang, cx, cy)
        if t == "@circle":
            return ring(cx, cy, h * 0.5, sw)
        if t == "@cross":
            d = h * 0.42
            return union(
                stroke([(cx - d, cy - d), (cx + d, cy + d)], sw, cap="round"),
                stroke([(cx - d, cy + d), (cx + d, cy - d)], sw, cap="round"))
        if t == "@triangle":
            r = h * 0.56
            pts = [(cx + r * math.cos(math.radians(90 + 120 * i)), cy - h * 0.08 + r * math.sin(math.radians(90 + 120 * i)))
                   for i in range(3)]
            return stroke(pts, sw, closed=True)
        if t == "@square":
            d = h * 0.4
            return stroke([(cx - d, cy - d), (cx + d, cy - d), (cx + d, cy + d), (cx - d, cy + d)], sw, closed=True)
        if t == "+":
            d = h * 0.48
            return union(stroke([(cx - d, cy), (cx + d, cy)], sw * 1.1, cap="butt"),
                         stroke([(cx, cy - d), (cx, cy + d)], sw * 1.1, cap="butt"))
        if t == "-":
            d = h * 0.48
            return stroke([(cx - d, cy), (cx + d, cy)], sw * 1.1, cap="butt")
        if t == "menu":
            d = h * 0.42
            return union(*[stroke([(cx - d, cy + o), (cx + d, cy + o)], sw * 0.75, cap="round") for o in (-d * 0.8, 0, d * 0.8)])
        if t == "view":
            d = h * 0.3
            a = rrect(cx - d - d * 0.35, cy - d + d * 0.35, cx + d - d * 0.35, cy + d + d * 0.35, d * 0.2)
            b = rrect(cx - d + d * 0.35, cy - d - d * 0.35, cx + d + d * 0.35, cy + d - d * 0.35, d * 0.2)
            w = sw * 0.6
            return union(diff(a, scale(a, 1 - w / d, cx - d * 0.35, cy + d * 0.35), b), diff(b, scale(b, 1 - w / d, cx + d * 0.35, cy - d * 0.35)))
        if t == "create":  # share: an arrow up out of a tray
            d = h * 0.42
            tray = stroke([(cx - d, cy), (cx - d, cy - d), (cx + d, cy - d), (cx + d, cy)], sw * 0.7, join="round")
            arrow = union(stroke([(cx, cy - d * 0.45), (cx, cy + d)], sw * 0.7),
                          stroke([(cx - d * 0.55, cy + d * 0.45), (cx, cy + d), (cx + d * 0.55, cy + d * 0.45)], sw * 0.7))
            return union(tray, arrow)
        if len(t) >= 5:  # START / SELECT: small capitals
            return self.L.path(t, h * 0.42, cx, cy, min(max_w or 1e9, h * 1.15))
        if len(t) == 1 and t.islower():
            return self.L.path(t, h * 0.78, cx, cy, max_w, ref="x")
        return self.L.path(t, h if len(t) == 1 else h * 0.82, cx, cy, max_w)

    # -- round face buttons
    def face_ring(self, role, h=520):
        return union(ring(CX, CY, R), self.label(role, h, max_w=640))

    def face_disc(self, role, h=540):
        return diff(circle(CX, CY, R), self.label(role, h, max_w=660))

    # -- shoulder buttons: the rounded side faces the outer edge
    def shoulder_shape(self, kind, side):
        if kind == "LR":
            s = rrect(28, -44, 996, 822, 110, rtl=330)
        elif kind == "Z":
            s = rrect(24, -100, 1000, 880, 110, rtl=470)
        else:  # SL / SR rail buttons
            s = rrect(20, -14, 1004, 794, 150)
        return mirror_x(s) if side == "R" else s

    def shoulder(self, role, kind, side, filled):
        s = self.shoulder_shape(kind, side)
        oy = {"LR": -44, "Z": -100, "S": -14}[kind]
        top = {"LR": 822, "Z": 880, "S": 794}[kind]
        ly = (oy + top) / 2 - (25 if kind == "Z" else 0)
        lx = CX + (40 if side == "L" else -40) * (kind != "S")
        lab = self.label(role, 440, lx, ly, max_w=640)
        if filled:
            return diff(s, lab)
        inner = self.inset(s, T)
        return union(diff(s, inner), lab)

    @staticmethod
    def inset(p, t):
        """The shape shrunk by t (a stroke of 2t subtracted)."""
        edge = P()
        p.draw(edge.getPen())
        edge.stroke(2 * t, pathops.LineCap.BUTT_CAP, pathops.LineJoin.ROUND_JOIN, 4)
        edge.convertConicsToQuads()
        return diff(p, edge)

    def outline(self, p, t=T):
        return diff(p, self.inset(p, t))

    # -- the four face buttons as a diamond of dots; `filled` = set of positions drawn solid
    def diamond(self, filled):
        out = P()
        r, d = 175, 300
        pos = {"top": (CX, CY + d), "bottom": (CX, CY - d), "left": (CX - d, CY), "right": (CX + d, CY)}
        for k, (x, y) in pos.items():
            out = union(out, circle(x, y, r) if k in filled else ring(x, y, r, 56))
        return out

    # -- d-pad
    def dpad_shape(self, arm=330, half=486):
        a = arm / 2
        h = rrect(CX - half, CY - a, CX + half, CY + a, 60)
        v = rrect(CX - a, CY - half, CX + a, CY + half, 60)
        return union(h, v)

    def dpad(self, filled=False, marks=(), style="square"):
        s = self.dpad_shape()
        base = s if filled else self.outline(s)
        out = base
        for m in marks:
            dx, dy, ang = {"up": (0, 1, 90), "down": (0, -1, -90), "left": (-1, 0, 180), "right": (1, 0, 0)}[m]
            mx, my = CX + dx * 330, CY + dy * 330
            if style == "square":
                mark = rrect(mx - 62, my - 62, mx + 62, my + 62, 10)
            else:
                mark = rot(poly([(mx + 85, my), (mx - 60, my + 95), (mx - 60, my - 95)]), ang, mx, my)
            out = diff(out, mark) if filled else union(out, mark)
        return out

    # -- analog stick (side view: cap over a short shaft); label inside the cap
    def stick(self, role=None, filled=False, press=False, small=False):
        cy_cap, rx, ry = 520, 450, 270
        shaft = rrect(CX - 230, -100, CX + 230, cy_cap - 120, 60)
        cap = ellipse(CX, cy_cap, rx, ry)
        shaft = diff(shaft, ellipse(CX, cy_cap, rx + 45, ry + 45))
        inner = ellipse(CX, cy_cap + 25, rx - 105, ry - 85)
        lab = self.label(role, 230, CX, cy_cap + 25, max_w=300) if role else None
        if filled:
            body = union(shaft, diff(cap, diff(ellipse(CX, cy_cap + 25, rx - 70, ry - 55), inner)))
            out = diff(body, lab) if lab is not None else body
        else:
            out = union(self.outline(shaft), self.outline(cap), self.outline(inner, 40), lab)
        if press:  # the stick pushed down: smaller, an arrow above it
            out = xform(out, 0.8, 0, 0, 0.8, CX - 0.8 * CX, -100 - 0.8 * -100)
            top = -100 + 0.8 * (cy_cap + ry + 100)
            arrow = union(rrect(CX - 65, top + 120, CX + 65, 885, 0), poly([(CX - 180, top + 130), (CX + 180, top + 130), (CX, top - 30)]))
            out = union(out, arrow)
        if small:
            out = scale(out, 0.72, CX, 300)
        return out

    def stick_tilt(self, side, arrow_at):
        """Half-arc beside a stick with an arrowhead (rotate hints); 500 units wide."""
        cx = 600 if side == "left" else -100
        pts = arc_pts(cx, 390, 360, 440, 145, 215, 32) if side == "left" else arc_pts(cx, 390, 360, 440, 35, -35, 32)
        a = stroke(pts, 60)
        end = pts[0] if arrow_at == "top" else pts[-1]
        nxt = pts[3] if arrow_at == "top" else pts[-4]
        ang = math.degrees(math.atan2(end[1] - nxt[1], end[0] - nxt[0]))
        head = rot(poly([(end[0] + 110, end[1]), (end[0] - 40, end[1] + 85), (end[0] - 40, end[1] - 85)]), ang, end[0], end[1])
        return union(a, head)

    # -- arrows
    def arrow(self, ang, thick=True):
        if thick:
            p = poly([(CX - 440, CY - 110), (CX + 60, CY - 110), (CX + 60, CY - 300), (CX + 470, CY),
                      (CX + 60, CY + 300), (CX + 60, CY + 110), (CX - 440, CY + 110)])
        else:
            p = union(stroke([(CX - 420, CY), (CX + 400, CY)], 64), stroke([(CX + 120, CY - 280), (CX + 410, CY), (CX + 120, CY + 280)], 64))
        return rot(p, ang)

    def double_arrow(self, ang):
        p = poly([(CX - 480, CY), (CX - 180, CY + 290), (CX - 180, CY + 110), (CX + 180, CY + 110), (CX + 180, CY + 290),
                  (CX + 480, CY), (CX + 180, CY - 290), (CX + 180, CY - 110), (CX - 180, CY - 110), (CX - 180, CY - 290)])
        return rot(p, ang)

    def circ_arrow(self, cw):
        pts = arc_pts(CX, CY, 330, 330, 120, 120 + 290, 64) if not cw else arc_pts(CX, CY, 330, 330, 60, 60 - 290, 64)
        a = stroke(pts, 130, cap="butt")
        end, nxt = pts[-1], pts[-3]
        ang = math.degrees(math.atan2(end[1] - nxt[1], end[0] - nxt[0]))
        head = rot(poly([(end[0] + 170, end[1]), (end[0] - 20, end[1] + 190), (end[0] - 20, end[1] - 190)]), ang, end[0], end[1])
        return union(a, head)

    def chevron(self, ang):
        return rot(stroke([(CX - 160, CY + 330), (CX + 170, CY), (CX - 160, CY - 330)], 64), ang)

    # -- system buttons
    def power_symbol(self, r=250, w=70, cx=CX, cy=CY):
        a = stroke(arc_pts(cx, cy, r, r, 120, 420, 48), w)
        return union(a, stroke([(cx, cy + r * 0.15), (cx, cy + r * 1.12)], w))

    def house(self, s=1.0, cx=CX, cy=CY):
        roof = poly([(cx - 300 * s, cy + 20 * s), (cx, cy + 300 * s), (cx + 300 * s, cy + 20 * s)])
        body = rrect(cx - 200 * s, cy - 240 * s, cx + 200 * s, cy + 60 * s, 20 * s)
        door = rrect(cx - 60 * s, cy - 240 * s, cx + 60 * s, cy - 70 * s, 10 * s)
        return diff(union(roof, body), door)

    def capture(self, filled):
        sq = rrect(CX - 470, CY - 470, CX + 470, CY + 470, 60)
        rg = ring(CX, CY, 300, 70)
        return diff(sq, rg) if filled else union(self.outline(sq), rg)

    def text_badge(self, text, shape, filled, h=520):
        lab = self.L.ink(text, h, max_w=520)
        if filled:
            return diff(shape, lab)
        return union(self.outline(shape), lab)


# ---- the glyph table ----------------------------------------------------------------------------------------------
def glyphs(b):
    """codepoint -> (path, advance)"""
    g = {}
    sq = lambda: rrect(CX - 470, CY - 470, CX + 470, CY + 470, 70)
    disc = lambda: circle(CX, CY, R)
    # face buttons, round: outlined (E0A0-E0A3; also the older E000-E003, E042) and filled (E0E0-E0E3)
    for i, role in enumerate("ABXY"):
        g[0xE0A0 + i] = b.face_ring(role)
        g[0xE000 + i] = b.face_ring(role)
        g[0xE0E0 + i] = b.face_disc(role)
    g[0xE042] = b.face_ring("A")
    for i, role in enumerate("abxy"):
        g[0xE04C + i] = b.face_ring(role)
    g[0xE047] = union(ring(CX, CY, R), b.L.ink("1", 520))
    g[0xE048] = union(ring(CX, CY, R), b.L.ink("2", 520))
    # shoulder buttons
    for cp, role, kind, side in ((0xE0A4, "L", "LR", "L"), (0xE0A5, "R", "LR", "R"), (0xE0A6, "ZL", "Z", "L"),
                                 (0xE0A7, "ZR", "Z", "R"), (0xE0A8, "SL", "S", "L"), (0xE0A9, "SR", "S", "R")):
        g[cp] = b.shoulder(role, kind, side, False)
        g[cp + 0x40] = b.shoulder(role, kind, side, True)       # E0E4-E0E9 filled
    for cp, role, kind, side in ((0xE083, "L", "LR", "L"), (0xE084, "R", "LR", "R"), (0xE085, "ZL", "Z", "L"),
                                 (0xE086, "ZR", "Z", "R")):
        g[cp] = b.shoulder(role, kind, side, False)
    g[0xE004] = b.shoulder("L", "S", "L", False)
    g[0xE005] = b.shoulder("R", "S", "R", False)
    g[0xE052] = b.shoulder("L", "S", "L", False)
    g[0xE053] = b.shoulder("R", "S", "R", False)
    g[0xE054] = union(b.outline(ellipse(CX, CY, 490, 400)), b.label("ZL", 440, max_w=640))
    g[0xE055] = union(b.outline(ellipse(CX, CY, 490, 400)), b.label("ZR", 440, max_w=640))
    # face-button diamonds: E0AA none, E0AB-E0AE one (A east, B south, X north, Y west), E0CA-E0CF two, E0EA all
    g[0xE0AA] = b.diamond(set())
    for cp, role in ((0xE0AB, "A"), (0xE0AC, "B"), (0xE0AD, "X"), (0xE0AE, "Y")):
        pos = {EAST: "right", SOUTH: "bottom", NORTH: "top", WEST: "left"}.get(b.hostmap[role])
        g[cp] = b.diamond({pos}) if pos else b.face_ring(role)
    for cp, f in ((0xE0CA, {"right", "bottom"}), (0xE0CB, {"left", "right"}), (0xE0CC, {"top", "right"}),
                  (0xE0CD, {"bottom", "left"}), (0xE0CE, {"top", "bottom"}), (0xE0CF, {"top", "left"})):
        g[cp] = b.diamond(f)
    g[0xE0EA] = b.diamond({"top", "bottom", "left", "right"})
    # direction triangles in a circle: E0AF-E0B2 outlined, E0EB-E0EE filled (up, down, left, right)
    for i, ang in enumerate((90, -90, 180, 0)):
        tri = rot(poly([(CX + 250, CY), (CX - 150, CY + 250), (CX - 150, CY - 250)]), ang)
        g[0xE0AF + i] = union(ring(CX, CY, R), tri)
        g[0xE0EB + i] = diff(disc(), tri)
    # Plus / Minus
    g[0xE0B3] = union(ring(CX, CY, R), b.label("PLUS", 520))
    g[0xE0B4] = union(ring(CX, CY, R), b.label("MINUS", 520))
    g[0xE045] = g[0xE0B3]
    g[0xE046] = g[0xE0B4]
    g[0xE0EF] = diff(disc(), b.label("PLUS", 540))
    g[0xE0F0] = diff(disc(), b.label("MINUS", 540))
    # the bare signs: Plus / Minus drawn as strokes; another controller's button as its label, large
    def bare(role, d, w):
        t = b.text_of(role)
        if t == "+":
            return union(stroke([(CX - d, CY), (CX + d, CY)], w, cap="butt"), stroke([(CX, CY - d), (CX, CY + d)], w, cap="butt"))
        if t == "-":
            return stroke([(CX - d, CY), (CX + d, CY)], w, cap="butt")
        return b.label(role, 2 * d * 0.9, max_w=900)
    g[0xE0B5] = bare("PLUS", 400, 110)
    g[0xE0B6] = bare("MINUS", 400, 110)
    g[0xE0F1] = bare("PLUS", 370, 170)
    g[0xE0F2] = bare("MINUS", 370, 170)
    # Power, HOME, Capture
    g[0xE0B7] = union(ring(CX, CY, R), b.power_symbol())
    g[0xE040] = g[0xE0B7]
    g[0xE0B8] = b.power_symbol(400, 110)
    g[0xE0F3] = diff(disc(), b.power_symbol(270, 85))
    g[0xE0B9] = union(ring(CX, CY, R), b.house(0.95))
    g[0xE044] = g[0xE0B9]
    g[0xE0F4] = diff(disc(), b.house(1.0))
    g[0xE073] = b.house(1.5, cy=CY + 30)
    g[0xE0BA] = b.capture(False)
    g[0xE0F5] = b.capture(True)
    # sticks: E0C0-E0C2 outlined (plain, L, R), E0C3-E0C5 outlined + press, E100-E105 filled; E080-E082 / E08A-E08B
    for i, role in enumerate((None, "=L", "=R")):
        g[0xE0C0 + i] = b.stick(role, False, False)
        g[0xE0C3 + i] = b.stick(role, False, True)
        g[0xE100 + i] = b.stick(role, True, False)
        g[0xE103 + i] = b.stick(role, True, True)
        g[0xE080 + i] = b.stick(role, False, False)
    g[0xE08A] = b.stick("=L", False, True, small=True)
    g[0xE08B] = b.stick("=R", False, True, small=True)
    # stick turn hints (half width)
    for cp, side, at in ((0xE0C6, "left", "bottom"), (0xE0C7, "right", "bottom"), (0xE0C8, "left", "top"), (0xE0C9, "right", "top")):
        g[cp] = (b.stick_tilt(side, at), 500)
    # d-pad: E006 / E041 / E0D0 plain; E079-E07E square marks; E0D1-E0D6 arrows; E110-E116 filled
    g[0xE006] = b.dpad()
    g[0xE041] = b.dpad()
    g[0xE0D0] = b.dpad()
    g[0xE110] = b.dpad(True)
    for i, marks in enumerate((("up",), ("down",), ("left",), ("right",), ("up", "down"), ("left", "right"))):
        g[0xE079 + i] = b.dpad(False, marks, "square")
        g[0xE0D1 + i] = b.dpad(False, marks, "arrow")
        g[0xE111 + i] = b.dpad(True, marks, "arrow")
    # arrows
    for i, ang in enumerate((0, 180, 90, -90, 45, -45, -135, 135)):
        g[0xE090 + i] = b.arrow(ang)
    g[0xE08C] = b.double_arrow(0)
    g[0xE08D] = b.double_arrow(90)
    g[0xE08E] = b.circ_arrow(False)
    g[0xE08F] = b.circ_arrow(True)
    g[0xE098] = union(rot(rrect(CX - 450, CY - 95, CX + 450, CY + 95, 30), 45), rot(rrect(CX - 450, CY - 95, CX + 450, CY + 95, 30), -45))
    for i, ang in enumerate((90, -90, 180, 0)):
        g[0xE143 + i] = b.arrow(ang, thick=False)
    for i, ang in enumerate((90, -90, 180, 0)):
        g[0xE147 + i] = b.chevron(ang)
    g[0xE14B] = stroke([(CX - 400, CY + 20), (CX - 120, CY - 280), (CX + 430, CY + 330)], 70)
    g[0xE14C] = union(stroke([(CX - 380, CY - 380), (CX + 380, CY + 380)], 70), stroke([(CX - 380, CY + 380), (CX + 380, CY - 380)], 70))
    g[0xE072] = union(stroke([(CX - 150, CY + 230)] + arc_pts(CX + 60, CY - 10, 300, 240, 90, -90, 40) + [(CX - 200, CY - 250)], 120, cap="butt"),
                      poly([(CX - 470, CY + 230), (CX - 150, CY + 470), (CX - 150, CY - 10)]))
    g[0xE056] = union(stroke([(CX + 300, CY + 420), (CX + 300, CY - 120), (CX - 200, CY - 120)], 140, cap="butt", join="miter"),
                      poly([(CX - 470, CY - 120), (CX - 170, CY + 170), (CX - 170, CY - 410)]))
    # notices
    excl, ques, info = (lambda h=560: b.L.ink("!", h)), (lambda h=560: b.L.ink("?", h)), (lambda h=560: b.L.ink("i", h))
    g[0xE140] = union(ring(CX, CY, R), excl())
    g[0xE141] = b.text_badge("!", sq(), False)
    g[0xE142] = union(ring(CX, CY, R), ques())
    g[0xE14D] = union(ring(CX, CY, R), info())
    g[0xE14E] = union(ring(CX, CY, R, 80), rot(rrect(CX - 440, CY - 40, CX + 440, CY + 40, 0), 45))
    g[0xE150] = diff(disc(), excl())
    g[0xE151] = diff(sq(), excl())
    g[0xE152] = diff(disc(), ques())
    g[0xE153] = diff(disc(), info())
    g[0xE06B] = diff(sq(), ques())
    g[0xE070] = union(b.outline(sq()), union(stroke([(CX - 220, CY - 220), (CX + 220, CY + 220)], 80), stroke([(CX - 220, CY + 220), (CX + 220, CY - 220)], 80)))
    g[0xE071] = diff(sq(), union(stroke([(CX - 230, CY - 230), (CX + 230, CY + 230)], 100), stroke([(CX - 230, CY + 230), (CX + 230, CY - 230)], 100)))
    # brightness
    sun = circle(CX, CY, 200)
    for i in range(8):
        a = math.radians(45 * i)
        sun = union(sun, stroke([(CX + 290 * math.cos(a), CY + 290 * math.sin(a)), (CX + 440 * math.cos(a), CY + 440 * math.sin(a))], 70))
    g[0xE130] = diff(sun, circle(CX, CY, 140))
    # TV and handheld outlines
    tv = rrect(CX - 480, CY - 330, CX + 480, CY + 400, 60)
    g[0xE089] = union(b.outline(tv), b.L.path("TV", 360, CX, CY + 35, max_w=560))
    hh = rrect(CX - 490, CY - 260, CX + 490, CY + 260, 120)
    g[0xE087] = union(b.outline(hh), b.outline(rrect(CX - 290, CY - 190, CX + 290, CY + 190, 20), 48))
    return {cp: (v if isinstance(v, tuple) else (v, ADV)) for cp, v in g.items()}


# ---- font assembly ------------------------------------------------------------------------------------------------
def to_glyph(path):
    path = pathops.simplify(path, fix_winding=True, clockwise=True)
    pen = TTGlyphPen(None)
    path.draw(Cu2QuPen(pen, max_err=1.0, reverse_direction=False))
    return pen.glyph()


def build(letters, family, out_path, alternates):
    """One font: the glyphs of `family` at the default (positional) binding on the Switch's codepoints; with
    `alternates`, also every binding-dependent glyph for every family and controller button (see ROLE_OF)."""
    table = glyphs(Builder(letters, family))
    order = [".notdef", "space", "uni3000", "uni25A0", "uni25A1", "uni0000", "uni000D"]
    glyf = {}
    hmtx = {}
    # .notdef and the box: the game counts on U+25A1 being in every font it is given (its box for a missing character)
    box = diff(rrect(104, -17, 920, 797, 0), rrect(164, 43, 860, 737, 0))
    glyf[".notdef"] = to_glyph(box)
    glyf["uni25A1"] = to_glyph(box)
    glyf["uni25A0"] = to_glyph(rrect(104, -17, 920, 797, 0))
    for gn in ("space", "uni3000", "uni0000", "uni000D"):
        glyf[gn] = TTGlyphPen(None).glyph()
    for gn in order:
        hmtx[gn] = ADV
    hmtx["space"] = 341
    hmtx["uni0000"] = hmtx["uni000D"] = 0
    cmap = {0x20: "space", 0x3000: "uni3000", 0x25A0: "uni25A0", 0x25A1: "uni25A1", 0x0: "uni0000", 0xD: "uni000D"}
    seen = {}  # outline + advance -> glyph name (identical drawings share one glyph)

    def add(cp, path, adv):
        g = to_glyph(path)
        key = (g.compile(glyf) if g.numberOfContours else b"", adv)
        gn = seen.get(key)
        if gn is None:
            gn = "uni%04X" % cp if cp <= 0xFFFF else "u%05X" % cp
            glyf[gn] = g
            hmtx[gn] = adv
            order.append(gn)
            seen[key] = gn
        cmap[cp] = gn

    for cp in sorted(table):
        add(cp, *table[cp])
    if alternates:
        for fam in FAMILIES:
            for host in HOSTS:
                alt = glyphs(Builder(letters, fam, {role: host for role in ROLES}))
                for cp in sorted(ROLE_OF):
                    add(alt_cp(fam, host, cp), *alt[cp])
    fb = FontBuilder(UPEM, isTTF=True)
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap(cmap)
    fb.setupGlyf(glyf)
    metrics = {}
    for gn in order:
        g = glyf[gn]
        g.recalcBounds(glyf)
        metrics[gn] = (hmtx[gn], getattr(g, "xMin", 0))
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=TYPO_ASC, descent=TYPO_DESC, lineGap=LINE_GAP)
    vname = "" if alternates else {"nintendo": " Switch Labels", "xbox": " Xbox Labels", "ps": " PS Labels"}[family]
    style = "Regular"
    fam = FAMILY + vname
    fb.setupNameTable({
        "copyright": "Copyright 2026 The Open Pad Glyphs Authors. Button letters from Baloo 2, "
                     "Copyright 2019 The Baloo 2 Project Authors (https://github.com/EkType/Baloo2)",
        "familyName": fam, "styleName": style, "uniqueFontIdentifier": "%s;%s" % (fam.replace(" ", ""), VERSION),
        "fullName": fam, "psName": fam.replace(" ", "") + "-" + style, "version": "Version " + VERSION,
        "licenseDescription": "This Font Software is licensed under the SIL Open Font License, Version 1.1. "
                              "This license is available with a FAQ at: https://openfontlicense.org",
        "licenseInfoURL": "https://openfontlicense.org",
        "description": "Controller button glyphs at the Switch's private-use button codepoints, drawn from scratch "
                       "for recompiled games that run without the console's fonts.",
    })
    fb.setupOS2(sTypoAscender=TYPO_ASC, sTypoDescender=TYPO_DESC, sTypoLineGap=LINE_GAP, usWinAscent=WIN_ASC,
                usWinDescent=WIN_DESC, achVendID="NONE", fsType=0, sxHeight=500, sCapHeight=700,
                ulUnicodeRange1=1, ulUnicodeRange2=1 << (60 - 32))  # Basic Latin, Private Use Area
    fb.setupPost(keepGlyphNames=True)
    fb.setupMaxp()
    fb.font["head"].fontRevision = float(VERSION)
    from fontTools.misc.timeTools import timestampFromString
    fb.font["head"].created = fb.font["head"].modified = timestampFromString("Sun Oct 25 00:00:00 2026")  # reproducible
    fb.font["head"].flags |= 1 << 3  # integer ppem scaling
    fb.save(out_path)
    return len(table), len(order)


def preview(font_paths, out_png):
    from PIL import Image, ImageDraw, ImageFont
    cols = 16
    rows = []
    for fp in font_paths:
        f = TTFont(fp)
        cps = sorted(c for c in f.getBestCmap() if c >= 0xE000)
        rows.append((fp, cps))
    n = len(rows[0][1])
    cell, lab = 84, 14
    W = cols * cell
    H = sum(((len(c) + cols - 1) // cols) * (cell + lab) + 30 for _, c in rows)
    im = Image.new("RGB", (W, H), "white")
    d = ImageDraw.Draw(im)
    small = ImageFont.load_default()
    y = 0
    for fp, cps in rows:
        fnt = ImageFont.truetype(fp, 48)
        d.text((4, y + 8), os.path.basename(fp), font=small, fill="blue")
        y += 30
        for i, cp in enumerate(cps):
            x = (i % cols) * cell
            yy = y + (i // cols) * (cell + lab)
            d.text((x + 16, yy + 4), chr(cp), font=fnt, fill="black")
            d.text((x + 4, yy + cell), "%04X" % cp, font=small, fill="red")
        y += ((len(cps) + cols - 1) // cols) * (cell + lab)
    im.save(out_png)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--baloo", default=os.path.join(HERE, "sources", "Baloo2[wght].ttf"),
                    help="Baloo 2 variable font (google/fonts ofl/baloo2; see fetch_sources.sh)")
    ap.add_argument("--out", default=HERE)
    ap.add_argument("--preview", action="store_true", help="also write preview.png (needs Pillow)")
    a = ap.parse_args()
    letters = Letters(a.baloo, 700)
    outs = []
    # OpenPadGlyphs.ttf: the runtime's (Switch letters by default, every family / binding as alternates it maps in);
    # the others are fixed sets for runtimes that load a font as it is (default positional binding)
    for family, name, alts in (("nintendo", "OpenPadGlyphs.ttf", True), ("nintendo", "OpenPadGlyphs-SwitchLabels.ttf", False),
                               ("xbox", "OpenPadGlyphs-XboxLabels.ttf", False), ("ps", "OpenPadGlyphs-PSLabels.ttf", False)):
        p = os.path.join(a.out, name)
        n, total = build(letters, family, p, alts)
        print("%s: %d button characters, %d glyphs" % (p, n, total))
        outs.append(p)
    if a.preview:
        preview(outs[1:], os.path.join(a.out, "preview.png"))


if __name__ == "__main__":
    main()
