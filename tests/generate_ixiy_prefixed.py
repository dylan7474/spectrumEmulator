#!/usr/bin/env python3
"""Generate a CP/M COM program that exercises IX/IY-prefixed helpers.

The emitted binary uses BDOS function calls to report success or
identify the failing sub-test. The individual routines cover
indirect loads/stores, arithmetic ops, and DD/FD CB prefixed
rotates/bit twiddling to catch regressions in the emulator helpers.
"""

import argparse
import struct
from typing import Dict, List, Tuple

BASE_ADDR = 0x0100


class ProgramBuilder:
    def __init__(self) -> None:
        self.code = bytearray()
        self.labels: Dict[str, int] = {}
        self.fixups: List[Tuple[int, str]] = []

    def current_addr(self) -> int:
        return BASE_ADDR + len(self.code)

    def label(self, name: str) -> None:
        if name in self.labels:
            raise ValueError(f"Label '{name}' already defined")
        self.labels[name] = self.current_addr()

    def emit(self, *values: int) -> None:
        self.code.extend(values)

    def emit_word_placeholder(self, label: str) -> None:
        pos = len(self.code)
        self.fixups.append((pos, label))
        self.code.extend(b"\x00\x00")

    def emit_word(self, value: int) -> None:
        self.code.extend(struct.pack('<H', value & 0xFFFF))

    def ld_de(self, label: str) -> None:
        self.emit(0x11)
        self.emit_word_placeholder(label)

    def call(self, label: str) -> None:
        self.emit(0xCD)
        self.emit_word_placeholder(label)

    def jp(self, label: str) -> None:
        self.emit(0xC3)
        self.emit_word_placeholder(label)

    def jp_condition(self, opcode: int, label: str) -> None:
        self.emit(opcode)
        self.emit_word_placeholder(label)

    def patch(self) -> None:
        for pos, label in self.fixups:
            if label not in self.labels:
                raise ValueError(f"Unknown label '{label}' in fixups")
            value = self.labels[label]
            self.code[pos:pos + 2] = struct.pack('<H', value)


def build_program() -> bytearray:
    prog = ProgramBuilder()
    ix_base = 0x8000
    iy_base = 0x8100

    # --- Program entry ---
    prog.label('start')
    prog.call('test_ixiy_indirect')
    prog.call('test_ixiy_alu')
    prog.call('test_ixiy_ddcb')
    prog.ld_de('msg_pass')
    prog.jp('report')

    # --- Report routine ---
    prog.label('report')
    prog.emit(0x0E, 0x09)  # LD C,9
    prog.emit(0xCD, 0x05, 0x00)  # CALL 5 (BDOS print string)
    prog.emit(0x0E, 0x00)  # LD C,0
    prog.emit(0xCD, 0x05, 0x00)  # CALL 5 (terminate)
    prog.emit(0xC9)  # RET

    # --- Test: indirect loads/stores ---
    prog.label('test_ixiy_indirect')
    prog.emit(0xDD, 0x21, ix_base & 0xFF, (ix_base >> 8) & 0xFF)  # LD IX,ix_base
    prog.emit(0xFD, 0x21, iy_base & 0xFF, (iy_base >> 8) & 0xFF)  # LD IY,iy_base
    prog.emit(0xDD, 0x36, 0x00, 0x11)  # LD (IX+0),0x11
    prog.emit(0xDD, 0x36, 0x01, 0x22)  # LD (IX+1),0x22
    prog.emit(0xFD, 0x36, 0x00, 0x33)  # LD (IY+0),0x33
    prog.emit(0xFD, 0x36, 0x01, 0x44)  # LD (IY+1),0x44
    prog.emit(0xDD, 0x7E, 0x00)        # LD A,(IX+0)
    prog.emit(0xFE, 0x11)              # CP 0x11
    prog.jp_condition(0xC2, 'fail_indirect_ix0')  # JP NZ
    prog.emit(0xDD, 0x7E, 0x01)        # LD A,(IX+1)
    prog.emit(0xFE, 0x22)              # CP 0x22
    prog.jp_condition(0xC2, 'fail_indirect_ix1')
    prog.emit(0x0E, 0x55)              # LD C,0x55
    prog.emit(0xDD, 0x71, 0x03)        # LD (IX+3),C
    prog.emit(0xDD, 0x7E, 0x03)        # LD A,(IX+3)
    prog.emit(0xFE, 0x55)              # CP 0x55
    prog.jp_condition(0xC2, 'fail_indirect_store_ix')
    prog.emit(0xFD, 0x77, 0x02)        # LD (IY+2),A
    prog.emit(0xFD, 0x7E, 0x02)        # LD A,(IY+2)
    prog.emit(0xFE, 0x55)              # CP 0x55
    prog.jp_condition(0xC2, 'fail_indirect_store_iy')
    prog.emit(0xC9)                    # RET

    # --- Test: arithmetic helpers ---
    prog.label('test_ixiy_alu')
    prog.emit(0xDD, 0x21, ix_base & 0xFF, (ix_base >> 8) & 0xFF)
    prog.emit(0xFD, 0x21, iy_base & 0xFF, (iy_base >> 8) & 0xFF)
    prog.emit(0xDD, 0x36, 0x00, 0x10)  # LD (IX+0),0x10
    prog.emit(0x3E, 0x05)              # LD A,0x05
    prog.emit(0xDD, 0x86, 0x00)        # ADD A,(IX+0)
    prog.emit(0xFE, 0x15)              # CP 0x15
    prog.jp_condition(0xC2, 'fail_alu_add')
    prog.emit(0xDD, 0x36, 0x01, 0x01)  # LD (IX+1),0x01
    prog.emit(0xDD, 0x8E, 0x01)        # ADC A,(IX+1)
    prog.emit(0xFE, 0x16)              # CP 0x16
    prog.jp_condition(0xC2, 'fail_alu_adc')
    prog.emit(0xDD, 0x36, 0x02, 0x02)  # LD (IX+2),0x02
    prog.emit(0xDD, 0x96, 0x02)        # SUB (IX+2)
    prog.emit(0xFE, 0x14)              # CP 0x14
    prog.jp_condition(0xC2, 'fail_alu_sub')
    prog.emit(0xDD, 0x36, 0x03, 0x08)  # LD (IX+3),0x08
    prog.emit(0xDD, 0xAE, 0x03)        # XOR (IX+3)
    prog.emit(0xFE, 0x1C)              # CP 0x1C
    prog.jp_condition(0xC2, 'fail_alu_xor')
    prog.emit(0xFD, 0x36, 0x00, 0x0F)  # LD (IY+0),0x0F
    prog.emit(0xFD, 0xB6, 0x00)        # OR (IY+0)
    prog.emit(0xFE, 0x1F)              # CP 0x1F
    prog.jp_condition(0xC2, 'fail_alu_or')
    prog.emit(0xFD, 0x36, 0x01, 0x1F)  # LD (IY+1),0x1F
    prog.emit(0xFD, 0xBE, 0x01)        # CP (IY+1)
    prog.jp_condition(0xC2, 'fail_alu_cp')
    prog.emit(0xC9)                    # RET

    # --- Test: DD/FD CB prefixed helpers ---
    prog.label('test_ixiy_ddcb')
    prog.emit(0xDD, 0x21, ix_base & 0xFF, (ix_base >> 8) & 0xFF)
    prog.emit(0xFD, 0x21, iy_base & 0xFF, (iy_base >> 8) & 0xFF)
    prog.emit(0x21, ix_base & 0xFF, (ix_base >> 8) & 0xFF)  # LD HL,ix_base
    prog.emit(0xDD, 0x36, 0x00, 0x81)  # LD (IX+0),0x81
    prog.emit(0xDD, 0xCB, 0x00, 0x24)  # SLA (IX+0),H -> IXH path
    prog.emit(0x7E)                    # LD A,(HL)
    prog.emit(0xFE, 0x02)              # CP 0x02
    prog.jp_condition(0xC2, 'fail_ddcb_sla_mem')
    prog.emit(0xDD, 0x7C)              # LD A,IXH
    prog.emit(0xFE, 0x02)              # CP 0x02
    prog.jp_condition(0xC2, 'fail_ddcb_sla_ixh')
    prog.emit(0xDD, 0x21, ix_base & 0xFF, (ix_base >> 8) & 0xFF)
    prog.emit(0x21, (ix_base + 1) & 0xFF, ((ix_base + 1) >> 8) & 0xFF)
    prog.emit(0xDD, 0x36, 0x01, 0x01)  # LD (IX+1),0x01
    prog.emit(0xDD, 0xCB, 0x01, 0x10)  # RL (IX+1),B
    prog.emit(0x7E)                    # LD A,(HL)
    prog.emit(0xFE, 0x02)              # CP 0x02
    prog.jp_condition(0xC2, 'fail_ddcb_rl_mem')
    prog.emit(0x78)                    # LD A,B
    prog.emit(0xFE, 0x02)              # CP 0x02
    prog.jp_condition(0xC2, 'fail_ddcb_rl_reg')
    prog.emit(0xFD, 0x21, iy_base & 0xFF, (iy_base >> 8) & 0xFF)
    prog.emit(0x21, iy_base & 0xFF, (iy_base >> 8) & 0xFF)
    prog.emit(0xFD, 0x36, 0x00, 0x40)  # LD (IY+0),0x40
    prog.emit(0xFD, 0xCB, 0x00, 0x2D)  # SRA (IY+0),L -> IYL path
    prog.emit(0x7E)                    # LD A,(HL)
    prog.emit(0xFE, 0x20)              # CP 0x20
    prog.jp_condition(0xC2, 'fail_ddcb_sra_mem')
    prog.emit(0xFD, 0x7D)              # LD A,IYL
    prog.emit(0xFE, 0x20)              # CP 0x20
    prog.jp_condition(0xC2, 'fail_ddcb_sra_iyl')
    prog.emit(0xFD, 0xCB, 0x00, 0x46)  # BIT 0,(IY+0)
    prog.jp_condition(0xC2, 'fail_ddcb_bit_zero')  # JP NZ -> should not take branch
    prog.emit(0xC9)                    # RET

    # --- Failure handlers ---
    for label, message in [
        ('fail_indirect_ix0', 'msg_fail_indirect_ix0'),
        ('fail_indirect_ix1', 'msg_fail_indirect_ix1'),
        ('fail_indirect_store_ix', 'msg_fail_indirect_store_ix'),
        ('fail_indirect_store_iy', 'msg_fail_indirect_store_iy'),
        ('fail_alu_add', 'msg_fail_alu_add'),
        ('fail_alu_adc', 'msg_fail_alu_adc'),
        ('fail_alu_sub', 'msg_fail_alu_sub'),
        ('fail_alu_xor', 'msg_fail_alu_xor'),
        ('fail_alu_or', 'msg_fail_alu_or'),
        ('fail_alu_cp', 'msg_fail_alu_cp'),
        ('fail_ddcb_sla_mem', 'msg_fail_ddcb_sla_mem'),
        ('fail_ddcb_sla_ixh', 'msg_fail_ddcb_sla_ixh'),
        ('fail_ddcb_rl_mem', 'msg_fail_ddcb_rl_mem'),
        ('fail_ddcb_rl_reg', 'msg_fail_ddcb_rl_reg'),
        ('fail_ddcb_sra_mem', 'msg_fail_ddcb_sra_mem'),
        ('fail_ddcb_sra_iyl', 'msg_fail_ddcb_sra_iyl'),
        ('fail_ddcb_bit_zero', 'msg_fail_ddcb_bit_zero'),
    ]:
        prog.label(label)
        prog.ld_de(message)
        prog.jp('report')

    # --- Data section ---
    def emit_string(label: str, text: str) -> None:
        prog.label(label)
        prog.emit(*(text.encode('ascii')))
        prog.emit(ord('$'))

    emit_string('msg_pass', 'IXIY prefixed helpers PASS\r\n')
    emit_string('msg_fail_indirect_ix0', 'IXIY FAIL indirect: IX load 0$')
    emit_string('msg_fail_indirect_ix1', 'IXIY FAIL indirect: IX load 1$')
    emit_string('msg_fail_indirect_store_ix', 'IXIY FAIL indirect: IX store$')
    emit_string('msg_fail_indirect_store_iy', 'IXIY FAIL indirect: IY store$')
    emit_string('msg_fail_alu_add', 'IXIY FAIL alu: add$')
    emit_string('msg_fail_alu_adc', 'IXIY FAIL alu: adc$')
    emit_string('msg_fail_alu_sub', 'IXIY FAIL alu: sub$')
    emit_string('msg_fail_alu_xor', 'IXIY FAIL alu: xor$')
    emit_string('msg_fail_alu_or', 'IXIY FAIL alu: or$')
    emit_string('msg_fail_alu_cp', 'IXIY FAIL alu: cp$')
    emit_string('msg_fail_ddcb_sla_mem', 'IXIY FAIL dd/fd: SLA mem$')
    emit_string('msg_fail_ddcb_sla_ixh', 'IXIY FAIL dd/fd: SLA IXH$')
    emit_string('msg_fail_ddcb_rl_mem', 'IXIY FAIL dd/fd: RL mem$')
    emit_string('msg_fail_ddcb_rl_reg', 'IXIY FAIL dd/fd: RL reg$')
    emit_string('msg_fail_ddcb_sra_mem', 'IXIY FAIL dd/fd: SRA mem$')
    emit_string('msg_fail_ddcb_sra_iyl', 'IXIY FAIL dd/fd: SRA IYL$')
    emit_string('msg_fail_ddcb_bit_zero', 'IXIY FAIL dd/fd: BIT zero$')

    prog.patch()
    return prog.code


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', help='Path to the generated CP/M COM binary')
    args = parser.parse_args()

    binary = build_program()
    with open(args.output, 'wb') as fh:
        fh.write(binary)


if __name__ == '__main__':
    main()
