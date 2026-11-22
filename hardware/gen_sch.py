#!/usr/bin/env python3
"""Generate hardware/rpi5datv-tuner-link.kicad_sch (KiCad 8 format) and check
that every pin endpoint lands on a wire endpoint / GND symbol."""
import sys, uuid

OUT = sys.argv[1]
PROJ = "rpi5datv-tuner-link"
ROOT = str(uuid.uuid4())
U = lambda: str(uuid.uuid4())
F = '(effects (font (size 1.27 1.27)))'
FH = '(effects (font (size 1.27 1.27)) hide)'

def pin(kind, x, y, ang, length, name, num, hide=False):
    return (f'(pin {kind} line (at {x} {y} {ang}) (length {length}){" hide" if hide else ""} '
            f'(name "{name}" {F}) (number "{num}" {F}))')

def libsym(lid, ref, val, body, pins, extra='', props_at=((2.032, 0, 90), (0, 0, 90))):
    short = lid.split(':')[1]
    (rx, ry, ra), (vx, vy, va) = props_at
    return (f'(symbol "{lid}" {extra} (exclude_from_sim no) (in_bom yes) (on_board yes)\n'
            f'  (property "Reference" "{ref}" (at {rx} {ry} {ra}) {F})\n'
            f'  (property "Value" "{val}" (at {vx} {vy} {va}) {F})\n'
            f'  (property "Footprint" "" (at 0 0 0) {FH})\n'
            f'  (property "Datasheet" "~" (at 0 0 0) {FH})\n'
            f'  (symbol "{short}_0_1" {body})\n'
            f'  (symbol "{short}_1_1" {" ".join(pins)}))')

ST = '(stroke (width 0.254) (type default)) (fill (type none))'
LIB = {
 'Device:R': libsym('Device:R', 'R', 'R',
    f'(rectangle (start -1.016 -2.54) (end 1.016 2.54) {ST})',
    [pin('passive', 0, 3.81, 270, 1.27, '~', 1), pin('passive', 0, -3.81, 90, 1.27, '~', 2)],
    '(pin_numbers hide) (pin_names (offset 0))'),
 'Device:C': libsym('Device:C', 'C', 'C',
    f'(polyline (pts (xy -2.032 -0.762) (xy 2.032 -0.762)) (stroke (width 0.508) (type default)) (fill (type none))) '
    f'(polyline (pts (xy -2.032 0.762) (xy 2.032 0.762)) (stroke (width 0.508) (type default)) (fill (type none)))',
    [pin('passive', 0, 3.81, 270, 3.048, '~', 1), pin('passive', 0, -3.81, 90, 3.048, '~', 2)],
    '(pin_numbers hide) (pin_names (offset 0.254))', ((0.635, 2.54, 0), (0.635, -2.54, 0))),
 'Device:L': libsym('Device:L', 'L', 'L',
    ' '.join(f'(arc (start 0 {a}) (mid 0.6323 {a + 0.635}) (end 0 {a + 1.27}) {ST})'
             for a in (-2.54, -1.27, 0, 1.27)),
    [pin('passive', 0, 3.81, 270, 1.27, '1', 1), pin('passive', 0, -3.81, 90, 1.27, '2', 2)],
    '(pin_numbers hide) (pin_names (offset 1.016) hide)', ((-1.27, 0, 90), (1.905, 0, 90))),
 'Connector:Conn_Coaxial': libsym('Connector:Conn_Coaxial', 'J', 'Conn_Coaxial',
    f'(circle (center 0 0) (radius 0.508) (stroke (width 0.254) (type default)) (fill (type outline))) '
    f'(circle (center 0 0) (radius 2.286) {ST}) '
    f'(polyline (pts (xy -2.54 0) (xy -0.508 0)) {ST}) (polyline (pts (xy 0 -2.54) (xy 0 -2.286)) {ST})',
    [pin('passive', -5.08, 0, 0, 2.54, 'In', 1), pin('passive', 0, -5.08, 90, 2.54, 'Ext', 2)],
    '(pin_names (offset 1.016) hide)', ((0.254, 3.048, 0), (2.921, 0, 90))),
 'rpi5datv:Pi500_GPIO': libsym('rpi5datv:Pi500_GPIO', 'J', 'Pi500_GPIO',
    f'(rectangle (start -10.16 2.54) (end 2.54 -5.08) (stroke (width 0.254) (type default)) (fill (type background)))',
    [pin('passive', 5.08, 0, 180, 2.54, 'GPIO12', 32), pin('passive', 5.08, -2.54, 180, 2.54, 'GND', 34)],
    '(pin_names (offset 1.016))', ((-3.81, 3.81, 0), (-3.81, -6.35, 0))),
 'power:PWR_FLAG': (
    f'(symbol "power:PWR_FLAG" (power) (pin_numbers hide) (pin_names (offset 0) hide) (exclude_from_sim no) (in_bom yes) (on_board yes)\n'
    f'  (property "Reference" "#FLG" (at 0 1.905 0) {FH})\n'
    f'  (property "Value" "PWR_FLAG" (at 0 3.81 0) {F})\n'
    f'  (property "Footprint" "" (at 0 0 0) {FH})\n'
    f'  (property "Datasheet" "~" (at 0 0 0) {FH})\n'
    f'  (symbol "PWR_FLAG_0_0" {pin("power_out", 0, 0, 90, 0, "~", 1)})\n'
    f'  (symbol "PWR_FLAG_0_1" (polyline (pts (xy 0 0) (xy 0 1.27) (xy -1.016 1.905) (xy 0 2.54) (xy 1.016 1.905) (xy 0 1.27)) '
    f'(stroke (width 0) (type default)) (fill (type none)))))'),
 'power:GND': (
    f'(symbol "power:GND" (power) (pin_names (offset 0)) (exclude_from_sim no) (in_bom yes) (on_board yes)\n'
    f'  (property "Reference" "#PWR" (at 0 -6.35 0) {FH})\n'
    f'  (property "Value" "GND" (at 0 -3.81 0) {F})\n'
    f'  (property "Footprint" "" (at 0 0 0) {FH})\n'
    f'  (property "Datasheet" "" (at 0 0 0) {FH})\n'
    f'  (symbol "GND_0_1" (polyline (pts (xy 0 0) (xy 0 -1.27) (xy 1.27 -1.27) (xy 0 -2.54) (xy -1.27 -1.27) (xy 0 -1.27)) '
    f'(stroke (width 0) (type default)) (fill (type none))))\n'
    f'  (symbol "GND_1_1" {pin("power_in", 0, 0, 270, 0, "GND", 1, hide=True)}))'),
}
# pin endpoints in library coordinates (y up)
PINS = {'Device:R': [(0, 3.81), (0, -3.81)], 'Device:C': [(0, 3.81), (0, -3.81)],
        'Device:L': [(0, 3.81), (0, -3.81)], 'Connector:Conn_Coaxial': [(-5.08, 0), (0, -5.08)],
        'power:PWR_FLAG': [(0, 0)], 'rpi5datv:Pi500_GPIO': [(5.08, 0), (5.08, -2.54)], 'power:GND': [(0, 0)], 'power:PWR_FLAG': [(0, 0)]}

items, endpoints, pinpts = [], [], []
pwr = [0]

def place(lid, ref, val, x, y, rot=0, ref_at=None, val_at=None, fp=''):
    pins = PINS[lid]
    for px, py in pins:  # lib -> sheet: flip y, then rotate (only 0/90 used)
        sx, sy = px, -py
        if rot == 90:
            sx, sy = sy, -sx
        pinpts.append((round(x + sx, 3), round(y + sy, 3), ref))
    ra = ref_at or (x + 2.54, y - 1.27)
    va = val_at or (x + 2.54, y + 1.27)
    hide_ref = lid.startswith('power:')
    pin_lines = ' '.join(f'(pin "{i + 1 if lid != "rpi5datv:Pi500_GPIO" else (32, 34)[i]}" (uuid "{U()}"))'
                         for i in range(len(pins)))
    items.append(
        f'(symbol (lib_id "{lid}") (at {x} {y} {rot}) (unit 1) (exclude_from_sim no) (in_bom {"no" if hide_ref else "yes"}) '
        f'(on_board yes) (dnp no) (uuid "{U()}")\n'
        f'  (property "Reference" "{ref}" (at {ra[0]} {ra[1]} 0) {FH if hide_ref else "(effects (font (size 1.27 1.27)) (justify left))"})\n'
        f'  (property "Value" "{val}" (at {va[0]} {va[1]} 0) {"(effects (font (size 1.27 1.27)))" if hide_ref else "(effects (font (size 1.27 1.27)) (justify left))"})\n'
        f'  (property "Footprint" "{fp}" (at {x} {y} 0) {FH})\n'
        f'  (property "Datasheet" "~" (at {x} {y} 0) {FH})\n'
        f'  {pin_lines}\n'
        f'  (instances (project "{PROJ}" (path "/{ROOT}" (reference "{ref}") (unit 1)))))')

def gnd(x, y):
    pwr[0] += 1
    place('power:GND', f'#PWR0{pwr[0]}', 'GND', x, y, val_at=(x, y + 3.81))

def wire(*pts):
    for a, b in zip(pts, pts[1:]):
        endpoints.extend([a, b])
        items.append(f'(wire (pts (xy {a[0]} {a[1]}) (xy {b[0]} {b[1]})) '
                     f'(stroke (width 0) (type default)) (uuid "{U()}"))')

def junction(x, y):
    items.append(f'(junction (at {x} {y}) (diameter 0) (color 0 0 0 0) (uuid "{U()}"))')

def text(s, x, y, size=1.27):
    s = s.replace('"', '\\"')
    items.append(f'(text "{s}" (at {x} {y} 0) (effects (font (size {size} {size})) (justify left top)) (uuid "{U()}"))')

Y = 76.2
# J1: Pi 500 header, GPIO12 = pin 32, GND = pin 34
place('rpi5datv:Pi500_GPIO', 'J1', 'Pi 500 40-pin header', 45.72, Y, ref_at=(39.37, 71.12), val_at=(30.48, 83.82))
wire((50.8, Y), (54.61, Y))                       # GPIO12 -> R1
wire((50.8, 78.74), (52.07, 78.74), (52.07, 86.36)); gnd(52.07, 86.36)
place('power:PWR_FLAG', '#FLG01', 'PWR_FLAG', 52.07, 78.74, ref_at=(0, 0), val_at=(52.07, 73.66))
place('Device:R', 'R1', '47', 58.42, Y, 90, ref_at=(56.515, 72.39), val_at=(56.515, 80.01))
# 10 dB 50-ohm pi attenuator R2 / R3 / R4
wire((62.23, Y), (68.58, Y)); junction(68.58, Y)
place('Device:R', 'R2', '100', 68.58, 81.28, ref_at=(70.485, 80.01), val_at=(70.485, 82.55))
wire((68.58, Y), (68.58, 77.47)); gnd(68.58, 85.09)
wire((68.58, Y), (72.39, Y))
place('Device:R', 'R3', '68', 76.2, Y, 90, ref_at=(74.295, 72.39), val_at=(74.295, 80.01))
wire((80.01, Y), (86.36, Y)); junction(86.36, Y)
place('Device:R', 'R4', '100', 86.36, 81.28, ref_at=(88.265, 80.01), val_at=(88.265, 82.55))
wire((86.36, Y), (86.36, 77.47)); gnd(86.36, 85.09)
# high-pass: shunt L1 then series C1 (also the DC block, at the tuner end)
wire((86.36, Y), (96.52, Y)); junction(96.52, Y)
place('Device:L', 'L1', '47nH', 96.52, 81.28, ref_at=(98.425, 80.01), val_at=(98.425, 82.55))
wire((96.52, Y), (96.52, 77.47)); gnd(96.52, 85.09)
wire((96.52, Y), (100.33, Y))
place('Device:C', 'C1', '22pF C0G 100V', 104.14, Y, 90, ref_at=(102.235, 71.12), val_at=(99.06, 81.28))
wire((107.95, Y), (116.84, Y))
place('Connector:Conn_Coaxial', 'J2', 'F/SMA socket to receiver RF in', 121.92, Y,
      ref_at=(119.38, 71.12), val_at=(124.46, 76.2))
gnd(121.92, 81.28)

text("GPIO12 is header pin 32, GND is pin 34 (30 / 39 also GND).", 25.4, 40.64)
text("R1 + R2/R3/R4: protects the GPIO and gives a ~10 dB, 50 ohm pad. Bridge R3 and leave out R2/R4 for 10 dB more signal.", 25.4, 44.45)
text("L1 + C1: high-pass, knocks the strong low-frequency DDS tone down ~30 dB; passes 400 MHz - 2 GHz. L1: 4 turns of 0.5 mm wire on a 3 mm drill.", 25.4, 48.26)
text("C1 is ALSO the DC block: a satellite receiver puts 13/18 V on the cable. Rate it >= 50 V. Turn LNB power OFF in the receiver anyway.", 25.4, 52.07)
text("Build it small: leads under 5 mm, inside an F-connector or a tin box, coax from there to the receiver.", 25.4, 55.88)
text("Expected level at the receiver: about -30 dBm on 437 MHz, about -40 dBm on 1050 MHz (rough estimate, unmeasured).", 25.4, 59.69)
text("Satellite receiver (950-2150 MHz IF):  rpi5datv -t -f 1050 -s 2000 -c 1/2  ->  receiver: LNB LO 9750, transponder 10800 MHz, SR 2000, FEC 1/2, LNB power off.", 25.4, 100.33)
text("RTL-SDR + leandvb, MiniTiouner/Ryde: same circuit, 437 MHz directly (rpi5datv -t -f 437 -s 333). A DVB-T/T2 TV tuner can NOT decode DVB-S.", 25.4, 104.14)
text("DO NOT radiate: this is for a cable into the receiver only. Without the band-pass filter + licence it must not go to an antenna.", 25.4, 107.95)

hdr = f'''(kicad_sch (version 20231120) (generator "eeschema") (generator_version "8.0")
  (uuid "{ROOT}")
  (paper "A4")
  (title_block (title "rpi5datv: Pi 500 GPIO to DVB-S receiver (cable link)") (date "2026-11-17") (rev "1")
    (comment 1 "Bench test link for receiver lock. Not a transmitter output stage."))
  (lib_symbols
{chr(10).join(LIB.values())}
  )
'''
body = '\n'.join(items)
with open(OUT, 'w') as f:
    f.write(hdr + body + f'\n  (sheet_instances (path "/" (page "1")))\n)\n')

# --- checks: balanced parens; every pin endpoint touches a wire endpoint or GND pin
src = open(OUT).read()
d = 0
instr = False
prev = ''
for ch in src:
    if ch == '"' and prev != '\\': instr = not instr
    elif not instr:
        d += ch == '('; d -= ch == ')'
        assert d >= 0
    prev = ch
assert d == 0, d
eps = {(round(x, 3), round(y, 3)) for x, y in endpoints}
gndpts = {(x, y) for x, y, r in pinpts if r.startswith('#PWR')}
bad = [(x, y, r) for x, y, r in pinpts if not r.startswith('#PWR') and (x, y) not in eps | gndpts]
gnd_unwired = [(x, y) for x, y in gndpts if (x, y) not in eps | {(a, b) for a, b, r in pinpts if not r.startswith('#PWR')}]
print('parens ok; pins:', len(pinpts), 'unconnected:', bad, 'floating GND:', gnd_unwired)
