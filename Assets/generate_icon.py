# -*- coding: utf-8 -*-
"""Generate the MediaPlay application icon.

Design language matches the plugin UI: a deep-blue rounded square, a glowing
play disc, a crescent moon and floating bubbles, plus the LED segment spectrum
(green -> yellow -> red) with peak caps and a soft reflection.

Outputs (into Assets/):
  MediaPlay-icon.png        1024x1024 master artwork (used as ICON_BIG)
  MediaPlay-icon-small.png  64x64 simplified artwork (used as ICON_SMALL)
  MediaPlay.ico             multi-size Windows icon (16..256)

Run:  python Assets/generate_icon.py
"""

import os
from PIL import Image, ImageDraw, ImageFilter, ImageChops

OUT = os.path.dirname(os.path.abspath(__file__))
S = 1024          # logical canvas size
SS = 2            # supersampling for smooth edges
W = S * SS


def p(v):
    return int(round(v * SS))


def rgba(hexstr, a=255):
    h = hexstr.lstrip('#')
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), a)


def new():
    return Image.new('RGBA', (W, W), (0, 0, 0, 0))


def lerp(a, b, t):
    t = max(0.0, min(1.0, t))
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(4))


def rounded_polygon(points, cut, steps=12):
    """Polygon with smooth quadratic-bezier corners (no ball-joint artefacts)."""
    result = []
    n = len(points)
    for i, v in enumerate(points):
        prev, nxt = points[(i - 1) % n], points[(i + 1) % n]

        def towards(target):
            dx, dy = target[0] - v[0], target[1] - v[1]
            length = (dx * dx + dy * dy) ** 0.5
            return (dx / length, dy / length)

        u1, u2 = towards(prev), towards(nxt)
        a = (v[0] + u1[0] * cut, v[1] + u1[1] * cut)
        b = (v[0] + u2[0] * cut, v[1] + u2[1] * cut)
        for k in range(steps + 1):
            t = k / steps
            x = (1 - t) ** 2 * a[0] + 2 * (1 - t) * t * v[0] + t ** 2 * b[0]
            y = (1 - t) ** 2 * a[1] + 2 * (1 - t) * t * v[1] + t ** 2 * b[1]
            result.append((x, y))
    return result


def vgrad(top, bottom):
    im = Image.new('RGBA', (W, W))
    d = ImageDraw.Draw(im)
    for y in range(W):
        t = (y / (W - 1)) ** 1.25
        d.line([(0, y), (W, y)], fill=lerp(top, bottom, t))
    return im


def round_mask(inset=36, radius=215):
    m = Image.new('L', (W, W), 0)
    ImageDraw.Draw(m).rounded_rectangle(
        [p(inset), p(inset), W - p(inset), W - p(inset)],
        radius=p(radius), fill=255)
    return m


def make_background():
    mask = round_mask()
    # Outer neon glow
    glow = new()
    ImageDraw.Draw(glow).rounded_rectangle(
        [p(36), p(36), W - p(36), W - p(36)],
        radius=p(215), fill=rgba('#2E6BFF', 130))
    glow = glow.filter(ImageFilter.GaussianBlur(p(30)))
    base = Image.alpha_composite(new(), glow)

    # Deep blue vertical gradient, clipped to the rounded square
    grad = vgrad(rgba('#1C2E78'), rgba('#050818'))
    grad.putalpha(mask)
    base = Image.alpha_composite(base, grad)

    # Soft corner light pools
    hl = new()
    d = ImageDraw.Draw(hl)
    d.ellipse([p(-190), p(-400), p(700), p(520)], fill=rgba('#5C9CFF', 72))
    d.ellipse([p(280), p(720), p(1040), p(1180)], fill=rgba('#2A6BFF', 66))
    hl = hl.filter(ImageFilter.GaussianBlur(p(58)))
    hl.putalpha(ImageChops.multiply(hl.getchannel('A'), mask))
    return Image.alpha_composite(base, hl), mask


def make_moon():
    cx, cy, r = p(792), p(208), p(76)
    r2, cx2, cy2 = p(72), p(826), p(176)
    m = Image.new('L', (W, W), 0)
    d = ImageDraw.Draw(m)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)
    d.ellipse([cx2 - r2, cy2 - r2, cx2 + r2, cy2 + r2], fill=0)

    glow = new()
    ImageDraw.Draw(glow).ellipse(
        [cx - r - p(34), cy - r - p(34), cx + r + p(34), cy + r + p(34)],
        fill=rgba('#FFD98A', 95))
    glow = glow.filter(ImageFilter.GaussianBlur(p(38)))

    moon = Image.new('RGBA', (W, W), rgba('#FFEBC4'))
    moon.putalpha(m.point(lambda v: int(v * 0.96)))
    return Image.alpha_composite(Image.alpha_composite(new(), glow), moon)


def spectrum_color(t):
    if t < 0.52:
        return lerp(rgba('#33D433'), rgba('#EED500'), t / 0.52)
    return lerp(rgba('#EED500'), rgba('#E4390E'), (t - 0.52) / 0.48)


def make_spectrum():
    bars = [0.30, 0.48, 0.70, 0.56, 0.86, 0.98, 0.80, 0.62, 0.44, 0.32, 0.22]
    spec = new()
    d = ImageDraw.Draw(spec)
    base_y, max_h = 870, 248
    bar_w, gap = 40, 18
    seg_h, seg_gap = 14, 7
    total = len(bars) * bar_w + (len(bars) - 1) * gap
    x0 = (S - total) / 2.0

    for j, h in enumerate(bars):
        x = x0 + j * (bar_w + gap)
        target = h * max_h
        i = 0
        while (i + 1) * seg_h + i * seg_gap <= target:
            y0 = base_y - i * (seg_h + seg_gap)
            y1 = y0 - seg_h
            t = (base_y - (y0 + y1) / 2.0) / 205.0
            d.rounded_rectangle(
                [p(x), p(y1), p(x + bar_w), p(y0 - 3)],
                radius=p(4), fill=spectrum_color(t))
            i += 1
        top = base_y - i * (seg_h + seg_gap)
        d.rounded_rectangle(
            [p(x), p(top - 17), p(x + bar_w), p(top - 9)],
            radius=p(3), fill=rgba('#FFF8D8', 255))
    return spec


def make_reflection(spec):
    # Mirror around the 872px baseline: flipped_y + 720 = 2*872 - y
    flipped = spec.transpose(Image.FLIP_TOP_BOTTOM)
    shifted = new()
    shifted.paste(flipped, (0, p(720)))

    fade = Image.new('L', (W, W), 0)
    d = ImageDraw.Draw(fade)
    start, end = p(882), W
    for y in range(start, end):
        t = (y - start) / (end - start)
        d.line([(0, y), (W, y)], fill=int(78 * max(0.0, 1.0 - t)))
    shifted.putalpha(ImageChops.multiply(shifted.getchannel('A'), fade))
    return shifted.filter(ImageFilter.GaussianBlur(p(2)))


def make_play_disc():
    cx, cy, r = p(512), p(418), p(204)
    glow = new()
    ImageDraw.Draw(glow).ellipse(
        [cx - r - p(36), cy - r - p(36), cx + r + p(36), cy + r + p(36)],
        fill=rgba('#2E9CFF', 120))
    glow = glow.filter(ImageFilter.GaussianBlur(p(52)))

    disc = new()
    d = ImageDraw.Draw(disc)
    for k in range(140, 0, -1):
        rr = int(r * k / 140)
        col = lerp(rgba('#55C0FF'), rgba('#123CA6'), (k / 140) ** 1.15)
        d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=col)
    # upper gloss
    gloss = new()
    ImageDraw.Draw(gloss).ellipse(
        [cx - r + p(26), cy - r + p(8), cx + r - p(26), cy + p(20)],
        fill=rgba('#FFFFFF', 46))
    gloss = gloss.filter(ImageFilter.GaussianBlur(p(30)))
    disc_mask = Image.new('L', (W, W), 0)
    ImageDraw.Draw(disc_mask).ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)
    gloss.putalpha(ImageChops.multiply(gloss.getchannel('A'), disc_mask))
    disc = Image.alpha_composite(disc, gloss)
    d = ImageDraw.Draw(disc)
    d.ellipse([cx - r, cy - r, cx + r, cy + r],
              outline=rgba('#D6ECFF', 200), width=p(4))

    # smooth rounded play triangle
    tri = [(p(454), p(313)), (p(454), p(523)), (p(630), p(418))]
    d.polygon(rounded_polygon(tri, p(30)), fill=rgba('#FFFFFF'))
    return Image.alpha_composite(Image.alpha_composite(new(), glow), disc)


def make_bubbles():
    b = new()
    d = ImageDraw.Draw(b)
    for x, y, r, a in [(168, 306, 18, 150), (232, 238, 10, 120),
                       (884, 512, 14, 135), (142, 606, 12, 110),
                       (872, 686, 9, 105), (318, 158, 7, 100),
                       (742, 336, 6, 95)]:
        d.ellipse([p(x - r), p(y - r), p(x + r), p(y + r)],
                  fill=rgba('#8FC4FF', int(a * 0.26)),
                  outline=rgba('#C6E4FF', a), width=max(1, p(2)))
        d.ellipse([p(x - r * 0.48), p(y - r * 0.55),
                   p(x - r * 0.08), p(y - r * 0.15)],
                  fill=rgba('#FFFFFF', int(a * 0.85)))
    return b


def make_big():
    base, mask = make_background()
    art = make_bubbles()
    art = Image.alpha_composite(art, make_moon())
    art = Image.alpha_composite(art, make_reflection(make_spectrum()))
    # disc glow first, then spectrum on top so the peak caps keep their colour
    art = Image.alpha_composite(art, make_play_disc())
    art = Image.alpha_composite(art, make_spectrum())

    d = ImageDraw.Draw(art)
    d.line([(p(170), p(884)), (p(854), p(884))],
           fill=rgba('#66B0FF', 175), width=p(2))

    # keep everything inside the rounded square
    art.putalpha(ImageChops.multiply(art.getchannel('A'), mask))
    out = Image.alpha_composite(base, art)
    ImageDraw.Draw(out).rounded_rectangle(
        [p(36), p(36), W - p(36), W - p(36)],
        radius=p(215), outline=rgba('#8AB8FF', 95), width=p(3))
    return out.resize((S, S), Image.LANCZOS)


def make_small():
    """Simplified 64px glyph: bold shapes stay legible at 16/32px."""
    n, ss, w = 64, 8, 64 * 8
    q = lambda v: int(round(v * ss))
    cv = lambda c, a=255: rgba(c, a)
    img = Image.new('RGBA', (w, w), (0, 0, 0, 0))
    m = Image.new('L', (w, w), 0)
    ImageDraw.Draw(m).rounded_rectangle(
        [q(2), q(2), w - q(2), w - q(2)], radius=q(14), fill=255)
    grad = Image.new('RGBA', (w, w))
    gd = ImageDraw.Draw(grad)
    for y in range(w):
        gd.line([(0, y), (w, y)], fill=lerp(cv('#203A8E'), cv('#050A20'), y / (w - 1)))
    grad.putalpha(m)
    img = Image.alpha_composite(img, grad)

    art = Image.new('RGBA', (w, w), (0, 0, 0, 0))
    d = ImageDraw.Draw(art)
    cx, cy, r = q(32), q(28), q(20)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=cv('#2E93FF'))
    d.ellipse([cx - r, cy - r, cx + r, cy + r],
              outline=cv('#BFE0FF', 200), width=q(1))
    tri = [(q(26), q(19)), (q(26), q(37)), (q(43), q(28))]
    d.polygon(tri, fill=cv('#FFFFFF'))
    for x, y in tri:
        d.ellipse([x - q(1), y - q(1), x + q(1), y + q(1)], fill=cv('#FFFFFF'))
    # five LED bars, green -> yellow -> red
    heights = [5, 8, 11, 7, 4]
    colors = ['#33D433', '#8EDB1E', '#EED500', '#F08A0C', '#E4390E']
    for j, (h, c) in enumerate(zip(heights, colors)):
        x = 9 + j * 10
        d.rounded_rectangle([q(x), q(58 - h), q(x + 6), q(58)],
                            radius=q(1), fill=cv(c))
    art.putalpha(ImageChops.multiply(art.getchannel('A'), m))
    img = Image.alpha_composite(img, art)
    return img.resize((n, n), Image.LANCZOS)


def main():
    big = make_big()
    big.save(os.path.join(OUT, 'MediaPlay-icon.png'))
    small = make_small()
    small.save(os.path.join(OUT, 'MediaPlay-icon-small.png'))
    big.save(os.path.join(OUT, 'MediaPlay.ico'), format='ICO',
             sizes=[(16, 16), (24, 24), (32, 32), (48, 48),
                    (64, 64), (128, 128), (256, 256)])
    print('icon written:', os.path.join(OUT, 'MediaPlay-icon.png'))
    for f in ('MediaPlay-icon.png', 'MediaPlay-icon-small.png', 'MediaPlay.ico'):
        print(' ', f, os.path.getsize(os.path.join(OUT, f)), 'bytes')


if __name__ == '__main__':
    main()
