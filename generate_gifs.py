import math
import os
from PIL import Image, ImageDraw, ImageFont, ImageChops, ImageFilter

# ---------------- LED rendering ----------------
W, H = 480, 480
AX0, AY0, AX1, AY1 = 40, 40, 440, 440
RED = (255, 40, 35)
BLUE = (35, 60, 255)
WHITE = (255, 255, 255)
FONT = "/System/Library/Fonts/Supplemental/Arial Bold.ttf"
RED_POS = (95, 95)
BLUE_POS = (385, 385)
RMAX, R0, STEPS, INTENSITY = 620.0, 260.0, 72, 0.55

def wash_layer(rgb, brightness, pos, R0, hot_r, blur, boost=1.0, whiten=1.0):
    rgb = tuple(min(255, int(v * boost)) for v in rgb)
    b = max(0.0, min(1.5, brightness / 100.0))
    layer = Image.new('RGB', (W, H), (0, 0, 0))
    d = ImageDraw.Draw(layer)
    for i in range(STEPS, 0, -1):
        t = i / STEPS
        r = RMAX * t
        lvl = INTENSITY * b / (1 + (r / R0) ** 2)
        if hot_r is None:
            col = tuple(int(ch * lvl) for ch in rgb)
        else:
            hot = math.exp(-r / hot_r)
            col = tuple(int((ch * lvl) * (1 - whiten * hot) + 255 * min(1.0, lvl) * whiten * hot) for ch in rgb)
        d.ellipse([pos[0] - r, pos[1] - r, pos[0] + r, pos[1] + r], fill=col)
    return layer.filter(ImageFilter.GaussianBlur(blur))

def draw_frame(master_rgb, master_b, slave_rgb, slave_b, text=None, text_size=84):
    frame = Image.new('RGB', (W, H), (10, 11, 13))
    d = ImageDraw.Draw(frame, 'RGBA')
    d.rectangle([AX0, AY0, AX1, AY1], fill=(26, 28, 31))
    mb_boost = 1.8 if master_rgb != WHITE else 1.0
    sb_boost = 1.8 if slave_rgb != WHITE else 1.0
    mb_whiten = 0.7 if master_rgb != WHITE else 1.0
    sb_whiten = 0.7 if slave_rgb != WHITE else 1.0
    mb_b = master_b * (1.0 if master_rgb == WHITE else 1.4)
    sb_b = slave_b * (1.0 if slave_rgb == WHITE else 1.4)
    frame = ImageChops.add(frame, wash_layer(master_rgb, mb_b, RED_POS, 260, 60, 1.5, boost=mb_boost, whiten=mb_whiten))
    frame = ImageChops.add(frame, wash_layer(slave_rgb, sb_b, BLUE_POS, 260, 60, 1.5, boost=sb_boost, whiten=sb_whiten))
    frame = ImageChops.add(frame, wash_layer(master_rgb, mb_b * 0.07, RED_POS, 520, None, 4.0, boost=mb_boost))
    frame = ImageChops.add(frame, wash_layer(slave_rgb, sb_b * 0.07, BLUE_POS, 520, None, 4.0, boost=sb_boost))
    if text:
        d = ImageDraw.Draw(frame, 'RGBA')
        font = ImageFont.truetype(FONT, text_size)
        tw = d.textlength(text, font=font)
        d.text(((W - tw) / 2, H / 2 - text_size / 2), text, font=font, fill=(255, 255, 255, 235))
    return frame

# ---------------- Timer rendering ----------------
TCW, TCH = 110, 180
TOP = 40
T, SG = 16, 9
GE, CW_COL = 36, 20
LM = 40
digits = [LM]
for i in range(5):
    gap = GE + (CW_COL + GE if i in (1, 3) else 0)
    digits.append(digits[-1] + TCW + gap)
c0x = digits[1] + TCW + GE + CW_COL // 2
c1x = digits[3] + TCW + GE + CW_COL // 2
TW = digits[-1] + TCW + LM
TH = TCH + 2 * TOP
GREEN = (70, 255, 80)
GHOST = (14, 18, 15)

def seg_rect(name, x, y):
    if name == 'a': return (x + T + SG, y,              x + TCW - T - SG, y + T)
    if name == 'g': return (x + T + SG, y + (TCH - T) // 2, x + TCW - T - SG, y + (TCH + T) // 2)
    if name == 'd': return (x + T + SG, y + TCH - T,    x + TCW - T - SG, y + TCH)
    if name == 'f': return (x,          y + T + SG,     x + T,            y + (TCH - T) // 2 - SG)
    if name == 'b': return (x + TCW - T, y + T + SG,    x + TCW,          y + (TCH - T) // 2 - SG)
    if name == 'e': return (x,          y + (TCH + T) // 2 + SG, x + T,   y + TCH - T - SG)
    if name == 'c': return (x + TCW - T, y + (TCH + T) // 2 + SG, x + TCW, y + TCH - T - SG)

def colon_boxes(cx):
    return [(cx - CW_COL // 2, TOP + TCH // 2 - 30, cx + CW_COL // 2, TOP + TCH // 2 - 10),
            (cx - CW_COL // 2, TOP + TCH // 2 + 10, cx + CW_COL // 2, TOP + TCH // 2 + 30)]

DIGITS = {
    '0': 'abcdef', '1': 'bc', '2': 'abged', '3': 'abcdg', '4': 'fgbc',
    '5': 'afgcd', '6': 'afgecd', '7': 'abc', '8': 'abcdefg', '9': 'abcdfg',
    'P': 'abefg', ' ': '',
}

def render_timer(string, ghosts=True):
    img = Image.new('RGB', (TW, TH), (0, 0, 0))
    d = ImageDraw.Draw(img)
    if ghosts:
        for x in digits:
            for s in 'abcdefg':
                d.rounded_rectangle(seg_rect(s, x, TOP), radius=T // 2, fill=GHOST)
        for cx in (c0x, c1x):
            for b in colon_boxes(cx):
                d.rounded_rectangle(b, radius=8, fill=GHOST)
    slot = 0
    colons = 0
    for ch in string:
        if ch == ':':
            cx = c0x if colons == 0 else c1x
            for b in colon_boxes(cx):
                d.rounded_rectangle(b, radius=8, fill=GREEN)
            colons += 1
        elif ch in DIGITS:
            for s in DIGITS[ch]:
                d.rounded_rectangle(seg_rect(s, digits[slot], TOP), radius=T // 2, fill=GREEN)
            slot += 1
        else:
            slot += 1
    glow = img.filter(ImageFilter.GaussianBlur(7))
    return ImageChops.add(img, glow).resize((TW // 2, TH // 2), Image.LANCZOS)

# ---------------- combined ----------------
TIMER_W, TIMER_H = 360, 90
LED_S = 360

def stack(led, timer):
    timer = [t.resize((TIMER_W, TIMER_H), Image.LANCZOS) for t in timer]
    led = [l.resize((LED_S, LED_S), Image.LANCZOS) for l in led]
    out = []
    for t, l in zip(timer, led):
        img = Image.new('RGB', (TIMER_W, TIMER_H + LED_S), (0, 0, 0))
        img.paste(t, (0, 0))
        img.paste(l, (0, TIMER_H))
        out.append(img)
    return out


# ---------------- Web UI (mobile) rendering ----------------
UI_W, UI_H = 360, 450
FONT_BOLD = "/System/Library/Fonts/Supplemental/Arial Bold.ttf"
FONT_REG = "/System/Library/Fonts/Supplemental/Arial.ttf"
TEAL = (31, 162, 155)
RED2 = (214, 69, 69)
BLUE2 = (59, 111, 214)
GRAY = (107, 114, 128)
DARK = (26, 26, 26)

LOGO_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "stem_coliseum_logo.webp")
_LOGO = Image.open(LOGO_PATH).convert("RGBA").resize((235, 34), Image.LANCZOS)
_LOGO_MASK = _LOGO.getchannel("A")
_LOGO_PAL = _LOGO.convert("RGB").quantize(colors=24, method=Image.MEDIANCUT)
_LOGO_COLORS = [tuple(_LOGO_PAL.getpalette()[i*3:i*3+3]) for i in range(24)]
_LOGO_Q = _LOGO_PAL.convert("RGB")

LOGO_X = 360 + (UI_W - _LOGO_Q.width) // 2
LOGO_Y = (50 - _LOGO_Q.height) // 2
LOGO_BOX = (LOGO_X, LOGO_Y, LOGO_X + _LOGO_Q.width, LOGO_Y + _LOGO_Q.height)

def _phase_palette(frames):
    pool = []
    for fr in frames:
        px = fr.load()
        for x in range(0, 720, 4):
            for y in range(0, 450, 4):
                pool.append(px[x, y])
    tmp = Image.new('RGB', (len(pool), 1))
    tmp.putdata(pool)
    q = tmp.quantize(colors=256 - len(_LOGO_COLORS), method=Image.MEDIANCUT,
                     dither=Image.Dither.NONE)
    adaptive = [tuple(q.getpalette()[i*3:i*3+3]) for i in range(256 - len(_LOGO_COLORS))]
    pal = [c for c in _LOGO_COLORS] + adaptive
    palimg = Image.new('P', (1, 1))
    palimg.putpalette([v for c in pal for v in c])
    return palimg

def _quantize_frames(frames):
    pal = _phase_palette(frames)
    out = []
    for f in frames:
        q = f.quantize(colors=256, method=Image.MEDIANCUT, palette=pal,
                       dither=Image.Dither.FLOYDSTEINBERG)
        qcrop = f.crop(LOGO_BOX).quantize(colors=256, method=Image.MEDIANCUT,
                                          palette=pal, dither=Image.Dither.NONE)
        q.paste(qcrop, (LOGO_X, LOGO_Y))
        out.append(q)
    return out

def ui_font(path, size):
    return ImageFont.truetype(path, size)

def render_ui(phase_name, countdown_num=None, time_str=None,
              winner=None, winner_color=None, buttons=()):
    img = Image.new('RGB', (UI_W, UI_H), (255, 255, 255))
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, UI_W, 50], fill=TEAL)
    img.paste(_LOGO_Q, ((UI_W - _LOGO_Q.width) // 2, (50 - _LOGO_Q.height) // 2), _LOGO_MASK)
    d.rounded_rectangle([14, 62, 346, 384], radius=16, fill=(255, 255, 255),
                        outline=(229, 231, 235), width=2)
    d.text((180, 86), "CURRENT PHASE", font=ui_font(FONT_BOLD, 11), fill=GRAY, anchor="mm")
    d.text((180, 112), phase_name, font=ui_font(FONT_BOLD, 24), fill=DARK, anchor="mm")
    if countdown_num is not None:
        d.text((180, 176), countdown_num, font=ui_font(FONT_BOLD, 48), fill=DARK, anchor="mm")
    if time_str is not None:
        d.text((180, 158), "TIME REMAINING", font=ui_font(FONT_BOLD, 10), fill=GRAY, anchor="mm")
        d.text((180, 188), time_str, font=ui_font(FONT_BOLD, 36), fill=DARK, anchor="mm")
    if winner is not None:
        d.text((180, 178), winner, font=ui_font(FONT_BOLD, 24), fill=winner_color, anchor="mm")
    if buttons:
        n = len(buttons)
        y0, y1 = 300, 346
        if n == 1:
            boxes = [(60, y0, 300, y1)]
        elif n == 2:
            boxes = [(60, y0, 176, y1), (184, y0, 300, y1)]
        else:
            boxes = [(60, y0, 134, y1), (142, y0, 216, y1), (224, y0, 298, y1)]
        for (label, color), (x0, yy0, x1, yy1) in zip(buttons, boxes):
            d.rounded_rectangle([x0, yy0, x1, yy1], radius=12, fill=color)
            d.text(((x0 + x1) / 2, (yy0 + yy1) / 2), label, font=ui_font(FONT_BOLD, 17),
                   fill=(255, 255, 255), anchor="mm")
    d.text((180, 414), "Connected", font=ui_font(FONT_REG, 12), fill=GRAY, anchor="mm")
    return img

def ui_phase(phase, f, N):
    if phase == "cleanup":
        return render_ui("Arena Cleanup", buttons=[("End Cleanup", TEAL)])
    if phase == "ready":
        return render_ui("Ready", buttons=[("Start Match", TEAL)])
    if phase == "countdown":
        return render_ui("Countdown", countdown_num=str(3 - f // 20))
    if phase == "match":
        rem = max(110, 120 - f // 20)
        return render_ui("Match", time_str="00:%02d:%02d" % (rem // 60, rem % 60),
                         buttons=[("Red Win", RED2), ("Blue Win", BLUE2), ("Judges", GRAY)])
    if phase == "judging":
        return render_ui("Judging", buttons=[("Red Win", RED2), ("Blue Win", BLUE2)])
    if phase == "announcement":
        return render_ui("Winner Announcement", winner="RED WINS", winner_color=RED2,
                         buttons=[("Begin Cleanup", TEAL)])

def save_comb(name, frames, dur=50):
    frames = _quantize_frames(frames)
    frames[0].save(f"gifs/{name}", save_all=True, append_images=frames[1:], duration=dur, loop=0)
    print(name, len(frames), "frames")

def full_composite(led, timer, ui):
    timer = [t.resize((360, 90), Image.LANCZOS) for t in timer]
    led = [l.resize((360, 360), Image.LANCZOS) for l in led]
    out = []
    for t, l, u in zip(timer, led, ui):
        img = Image.new('RGB', (720, 450), (0, 0, 0))
        img.paste(t, (0, 0))
        img.paste(l, (0, 90))
        img.paste(u, (360, 0))
        out.append(img)
    return out

N = 50  # ms per frame

# Cleanup
led = [draw_frame(WHITE, 100, WHITE, 100) for _ in range(10)]
timer = [render_timer("", ghosts=True) for _ in range(10)]
ui = [ui_phase("cleanup", f, N) for f in range(10)]
save_comb("combined_cleanup.gif", full_composite(led, timer, ui))

# Ready
led = [draw_frame(RED, 100, BLUE, 100) for _ in range(10)]
timer = [render_timer("00:00:00") for _ in range(10)]
ui = [ui_phase("ready", f, N) for f in range(10)]
save_comb("combined_ready.gif", full_composite(led, timer, ui))

# Countdown
led, timer, ui = [], [], []
for f in range(60):
    t = f * N
    b = 100 * (1000 - (t % 1000)) / 1000
    led.append(draw_frame(RED, b, BLUE, b))
    timer.append(render_timer("P3" if f < 20 else "P2" if f < 40 else "P1"))
    ui.append(ui_phase("countdown", f, N))
save_comb("combined_countdown.gif", full_composite(led, timer, ui))

# Match: 10s countdown from 00:00:10 to 00:00:00
flash_windows = [(2000, 2300), (4200, 4700), (6500, 6700), (8800, 9200)]
led, timer, ui = [], [], []
for f in range(220):
    t = f * N
    hit = next((i for i, (a, c) in enumerate(flash_windows) if a <= t <= c), None)
    if hit is not None:
        idx = (t - flash_windows[hit][0]) // N
        b = 100 if idx % 2 == 0 else 30
    else:
        b = 100
    led.append(draw_frame(WHITE, b, WHITE, b))
    rem = max(110, 120 - f // 20)
    timer.append(render_timer("00:%02d:%02d" % (rem // 60, rem % 60)))
    ui.append(ui_phase("match", f, N))
save_comb("combined_match.gif", full_composite(led, timer, ui))

# Judging
led, timer, ui = [], [], []
for f in range(60):
    t = f * N
    sine = (math.sin(math.pi * t / 1500) + 1.0) / 2.0
    led.append(draw_frame(RED, 40 + 60 * sine, BLUE, 40 + 60 * (1 - sine)))
    timer.append(render_timer("", ghosts=True))
    ui.append(ui_phase("judging", f, N))
save_comb("combined_judging.gif", full_composite(led, timer, ui))

# Winner Announcement
led, timer, ui = [], [], []
side = 0
for i in range(16):
    delay = 300 - 25 * (i // 2)
    led += [draw_frame(RED if side == 0 else BLUE, 100, BLUE if side == 0 else RED, 100)
            for _ in range(max(1, round(delay / N)))]
    side ^= 1
for i in range(24):
    led.append(draw_frame(WHITE, 100 if i % 2 == 1 else 0, WHITE, 100 if i % 2 == 0 else 0))
led += [draw_frame(RED, 100, RED, 100) for _ in range(20)]
timer = [render_timer("", ghosts=True) for _ in range(len(led))]
ui = [ui_phase("announcement", f, N) for f in range(len(led))]
save_comb("combined_announcement.gif", full_composite(led, timer, ui))
