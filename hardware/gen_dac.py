#!/usr/bin/env python3
"""Generate hardware/rpi5datv-tuner-link.kicad_sch (KiCad 8 format) and check
that every pin endpoint lands on a wire endpoint / GND symbol."""
import sys, uuid

OUT = sys.argv[1]
PROJ = "rpi5datv-dvbt-dac"
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
 'rpi5datv:Pi500_DAC': libsym('rpi5datv:Pi500_DAC', 'J', 'Pi500_DAC',
    f'(rectangle (start -10.16 2.54) (end 2.54 -33.02) (stroke (width 0.254) (type default)) (fill (type background)))',
    [pin('passive', 5.08, 0, 180, 2.54, 'GPIO22', 15), pin('passive', 5.08, -7.62, 180, 2.54, 'GPIO23', 16),
     pin('passive', 5.08, -15.24, 180, 2.54, 'GPIO24', 18), pin('passive', 5.08, -22.86, 180, 2.54, 'GPIO25', 22),
     pin('passive', 5.08, -30.48, 180, 2.54, 'GND', 14)],
    '(pin_names (offset 1.016))', ((-3.81, 3.81, 0), (-10.16, -35.56, 0))),
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
        'power:PWR_FLAG': [(0, 0)], 'rpi5datv:Pi500_DAC': [(5.08, 0), (5.08, -7.62), (5.08, -15.24), (5.08, -22.86), (5.08, -30.48)], 'power:GND': [(0, 0)], 'power:PWR_FLAG': [(0, 0)]}

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
    pin_lines = ' '.join(f'(pin "{i + 1 if lid != "rpi5datv:Pi500_DAC" else (15, 16, 18, 22, 14)[i]}" (uuid "{U()}"))'
                         for i in range(len(pins)))
    items.append(
        f'(symbol (lib_id "{lid}") (at {x} {y} {rot}) (unit 1) (exclude_from_sim no) (in_bom {"no" if hide_ref else "yes"}) '
        f'(on_board yes) (dnp no) (uuid "{U()}")\n'
        f'  (property "Reference" "{ref}" (at {ra[0]} {ra[1]} {rot}) {FH if hide_ref else "(effects (font (size 1.27 1.27)) (justify left))"})\n'
        f'  (property "Value" "{val}" (at {va[0]} {va[1]} {rot}) {"(effects (font (size 1.27 1.27)))" if hide_ref else "(effects (font (size 1.27 1.27)) (justify left))"})\n'
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

Y0 = 63.5
place('rpi5datv:Pi500_DAC', 'J1', 'Pi 500 40-pin header', 45.72, Y0, ref_at=(39.37, 58.42), val_at=(33.02, 101.6))
vals = [('R1', '8.06k 1%', 'GPIO22 = LSB'), ('R2', '4.02k 1%', 'GPIO23'), ('R3', '2.00k 1%', 'GPIO24'), ('R4', '1.00k 1%', 'GPIO25 = MSB')]
for i, (ref, val, _) in enumerate(vals):
    y = Y0 + 7.62 * i
    wire((50.8, y), (57.15, y))
    place('Device:R', ref, val, 60.96, y, 90, ref_at=(59.69, y - 1.905), val_at=(58.42, y + 3.175))
    wire((64.77, y), (71.12, y))
for i in range(3):
    wire((71.12, Y0 + 7.62 * i), (71.12, Y0 + 7.62 * (i + 1)))
junction(71.12, Y0 + 7.62)
junction(71.12, Y0 + 15.24)
# header GND
wire((50.8, Y0 + 30.48), (53.34, Y0 + 30.48), (53.34, Y0 + 33.02)); gnd(53.34, Y0 + 33.02)
place('power:PWR_FLAG', '#FLG01', 'PWR_FLAG', 53.34, Y0 + 30.48, ref_at=(0, 0), val_at=(56.515, Y0 + 31.115))
# summing node -> shunt L1 -> series C1 (high-pass + DC block) -> coax
wire((71.12, Y0), (81.28, Y0)); junction(81.28, Y0)
place('Device:L', 'L1', '47nH', 81.28, Y0 + 7.62, ref_at=(83.185, Y0 + 6.35), val_at=(83.185, Y0 + 8.89))
wire((81.28, Y0), (81.28, Y0 + 3.81)); gnd(81.28, Y0 + 11.43)
wire((81.28, Y0), (85.09, Y0))
place('Device:C', 'C1', '22pF C0G 50V', 88.9, Y0, 90, ref_at=(87.63, Y0 - 6.35), val_at=(84.455, Y0 - 3.175))
wire((92.71, Y0), (97.79, Y0))
place('Connector:Conn_Coaxial', 'J2', 'coax to TV aerial input', 102.87, Y0, ref_at=(100.33, Y0 - 5.08), val_at=(105.41, Y0))
gnd(102.87, Y0 + 5.08)

T = 25.4
text("4-bit DAC for DVB-T from the PIO (experiments/dvbt): GPIO22..25 = header pins 15, 16, 18, 22 (LSB first). GND = pin 14 (20 also GND).", T, 30.48)
text("Binary-weighted 1% resistors into one summing node. Large values so the GPIO's own ~40 ohm drive impedance only shifts the weights ~2-3% (fine: sim shows 3% costs < 1 dB).", T, 34.29)
text("TIMING SKEW BETWEEN THE 4 BITS IS WHAT MATTERS (the TV channel is the 14th image at 474 MHz: 100 ps = 17 deg): keep all four leads short and EQUAL length,", T, 38.1)
text("resistors right at the header, summing node 1-2 cm away. Sim: <= 50 ps skew -> MER ~14-18 dB, 100 ps -> 9-13 dB, 200 ps -> ~5-8 dB (QPSK 1/2 needs ~5).", T, 41.91)
text("L1 + C1: high-pass, removes the strong 3.5-11 MHz baseband signal (~ -13 dBm) before the tuner; C1 also blocks DC (some TVs/boxes feed 5 V to the aerial).", T, 45.72)
text("L1: 4 turns of 0.5 mm wire on a 3 mm drill. Expected level at 474 MHz: roughly -50 dBm (unmeasured). The tuner input (75 ohm) is the load.", T, 49.53)
text("Play: ./ppplay tc8_474_4b.u32 22 4 32 6 247520 3600   (gap-free, 33.333 MS/s). TV: manual tune 474.000 MHz (UK ch 21), bandwidth 8 MHz.", T, 110.49)
text("3-bit mode on the same board: use GPIO23-25 (R2-R4 have the right 4:2:1 weights): ./ppplay tc8_474_3b.u32 23 3 30 6 198016 3600  (GPIO22 stays off).", T, 114.3)
text("Cable to the tuner only. UHF TV channels are broadcast spectrum: do not connect an aerial.", T, 118.11)

hdr = f'''(kicad_sch (version 20231120) (generator "eeschema") (generator_version "8.0")
  (uuid "{ROOT}")
  (paper "A4")
  (title_block (title "rpi5datv: 4-bit GPIO DAC for DVB-T (cable link to a TV tuner)") (date "2026-11-22") (rev "1")
    (comment 1 "Experimental: experiments/dvbt. Not a transmitter output stage."))
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
