#!/usr/bin/env python3
"""Generate a tiny 2-track test NSF (pulse arpeggio + triangle bass)."""
import struct, sys
code = bytearray()
def emit(*b): code.extend(b)
# init @ $8000: A = song
emit(0x85,0x00)             # STA $00 (song)
emit(0xA9,0x0F,0x8D,0x15,0x40)  # enable channels
emit(0xA9,0xBF,0x8D,0x00,0x40)  # pulse1 duty 50%, const vol 15
emit(0xA9,0x08,0x8D,0x01,0x40)  # no sweep
emit(0xA9,0xFF,0x8D,0x08,0x40)  # triangle linear counter
emit(0xA9,0x00,0x85,0x01)       # counter = 0
emit(0x60)                      # RTS
play = 0x8000 + len(code)
emit(0xE6,0x01)                 # INC $01
emit(0xA5,0x01,0x29,0x07)       # LDA $01 AND #7
bne_pos = len(code); emit(0xD0,0x00)  # BNE done
emit(0xA5,0x01,0x4A,0x4A,0x4A)  # LDA $01, LSR x3
emit(0x29,0x07)                 # AND #7
emit(0x18,0x65,0x00)            # CLC ADC song  (song 1 shifted up)
emit(0x29,0x0F,0xAA)            # AND #$0F TAX
lo_ref = len(code); emit(0xBD,0,0)  # LDA lo,X
emit(0x8D,0x02,0x40)
hi_ref = len(code); emit(0xBD,0,0)  # LDA hi,X
emit(0x8D,0x03,0x40)
emit(0xA5,0x01,0x29,0x3F)       # every 64 frames change bass
bne2 = len(code); emit(0xD0,0x00)
emit(0xA5,0x01,0x4A,0x4A,0x4A,0x4A,0x4A,0x4A,0x29,0x03,0xAA) # X = (cnt>>6)&3
blo = len(code); emit(0xBD,0,0); emit(0x8D,0x0A,0x40)
bhi = len(code); emit(0xBD,0,0); emit(0x8D,0x0B,0x40)
done = len(code); emit(0x60)
code[bne_pos+1] = done - (bne_pos+2)
code[bne2+1] = done - (bne2+2)
# period tables: C major scale-ish, NTSC pulse periods
notes = [0x1AB,0x17C,0x153,0x140,0x11C,0x0FD,0x0E2,0x0D5,0x0BE,0x0A9,0x09F,0x08E,0x07E,0x071,0x06A,0x05F]
bass  = [0x357,0x2A6,0x23A,0x2F8]
def table(vals, f):
    return bytes(f(v) for v in vals)
lo_addr = 0x8000+len(code); code += table(notes, lambda v: v & 0xFF)
hi_addr = 0x8000+len(code); code += table(notes, lambda v: 0x08 | (v>>8))
blo_addr = 0x8000+len(code); code += table(bass, lambda v: v & 0xFF)
bhi_addr = 0x8000+len(code); code += table(bass, lambda v: 0x08 | (v>>8))
for pos,a in ((lo_ref,lo_addr),(hi_ref,hi_addr),(blo,blo_addr),(bhi,bhi_addr)):
    code[pos+1]=a&0xFF; code[pos+2]=a>>8
def s32(t): return t.encode()[:31].ljust(32,b'\0')
hdr = b'NESM\x1a' + bytes([1, 2, 1]) + struct.pack('<HHH', 0x8000, 0x8000, play)
hdr += s32('NSFPlay Linux Test') + s32('test generator') + s32('public domain')
hdr += struct.pack('<H', 16639) + bytes(8) + struct.pack('<H', 19997) + bytes([0, 0]) + bytes(4)
assert len(hdr) == 128
open(sys.argv[1], 'wb').write(hdr + bytes(code))
