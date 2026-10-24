#!/usr/bin/env python3
"""Makes Musashi fit the Game & Watch (run by tools/update_musashi.sh after
m68kmake, on the generated files in the build directory).

Musashi decodes opcodes through a table of 65,536 handler pointers built at
start-up (256 KB of RAM) and counts cycles with a 64 KB table per CPU type
(320 KB). On the Game & Watch the code and its data share about 724 KB of
RAM, so:

- The decode table is built here, as Musashi's m68ki_build_opcode_table
  would (each entry of m68k_opcode_handler_table, in order, takes every
  opcode with opcode & mask == match), and stored in two constant levels:
  for each block of BLOCK opcodes, the index of an identical block in a
  shared list of distinct blocks of handler numbers. m68kcpu.c calls
  M68KI_HANDLER(opcode)() instead of the table.
- Instructions count a fixed 4 cycles (the port layer ends the time slices
  itself; nothing uses cycle counts).
- FPU, PMMU and other coprocessor opcodes (line F) are left to the line F
  exception, as on a 68020 without coprocessors; their code is no longer
  referenced and the linker drops it with the softfloat library.
- Musashi's generated code leaves variables unused (bit field handlers,
  m68ki_exception_trapN): -Wunused-variable is off in its files, so the
  build only warns about the port's own code.

usage: slim_musashi.py build_dir
"""

import re
import sys

DROPPED = {"040fpu0_32", "040fpu1_32", "cpbcc_32", "cpdbcc_32", "cpgen_32", "cpscc_32",
           "cptrapcc_32", "move16_32", "pflush_32", "pmmu_32"}


def replace(text, old, new, count=1):
    if text.count(old) < count:
        raise SystemExit(f"slim_musashi: {old!r} not found {count} time(s)")
    return text.replace(old, new)


def main():
    build = sys.argv[1]

    ops_path = f"{build}/m68kops.c"
    ops = open(ops_path).read()
    if "M68KI_DECODE_BLOCK" in ops:
        return  # already done

    # The handler table: (name, mask, match) in order, up to the terminator.
    table_match = re.search(r"static const opcode_handler_struct m68k_opcode_handler_table\[\] =\n\{.*?\n\};\n",
                            ops, re.S)
    entries = [(name, int(mask, 16), int(match, 16)) for name, mask, match in
               re.findall(r"\{m68k_op_(\w+)\s*,\s*(0x[0-9a-f]+),\s*(0x[0-9a-f]+),", table_match.group(0))]

    names = ["illegal"]
    numbers = {"illegal": 0}
    decode = [0] * 0x10000
    for name, mask, match in entries:
        if name in DROPPED:
            continue
        if name not in numbers:
            numbers[name] = len(names)
            names.append(name)
        number = numbers[name]
        for opcode in range(0x10000):
            if opcode & mask == match:
                decode[opcode] = number

    # Two levels: the smallest total for a block size.
    best = None
    for shift in range(3, 11):
        block = 1 << shift
        blocks, index = {}, []
        for start in range(0, 0x10000, block):
            key = tuple(decode[start:start + block])
            index.append(blocks.setdefault(key, len(blocks)))
        index_bytes = 1 if len(blocks) <= 256 else 2
        size = len(blocks) * block * 2 + len(index) * index_bytes
        if best is None or size < best[0]:
            best = (size, shift, blocks, index, index_bytes)
    size, shift, blocks, index, index_bytes = best

    lines = ["\n/* Made by tools/slim_musashi.py: the opcode decode table in two",
             f"   levels ({len(blocks)} distinct blocks of {1 << shift} opcodes, {size} bytes). */",
             f"#define M68KI_DECODE_BLOCK {shift}",
             "void (*const m68ki_handlers[])(void) = {"]
    lines += [f"\tm68k_op_{name}," for name in names]
    lines.append("};")
    index_type = "uint8_t" if index_bytes == 1 else "uint16_t"
    lines.append(f"const {index_type} m68ki_decode_index[{len(index)}] = {{")
    for start in range(0, len(index), 32):
        lines.append("\t" + ",".join(str(v) for v in index[start:start + 32]) + ",")
    lines.append("};")
    lines.append(f"const uint16_t m68ki_decode_blocks[{len(blocks)}][{1 << shift}] = {{")
    for key in blocks:
        lines.append("\t{" + ",".join(str(v) for v in key) + "},")
    lines.append("};")

    # m68kops.c: no runtime tables, no builder, no handler table (it would
    # keep the dropped handlers referenced); the unused handlers stay as
    # static functions the compiler drops.
    ops = ops.replace(table_match.group(0), "")
    ops = re.sub(r"\nvoid m68ki_build_opcode_table\(void\)\n\{.*?\n\}\n", "\n", ops, count=1, flags=re.S)
    ops = re.sub(r"\n[^\n]*m68ki_instruction_jump_table\[0x10000\][^\n]*;[^\n]*", "", ops)
    ops = re.sub(r"\n[^\n]*m68ki_cycles\[NUM_CPU_TYPES\]\[0x10000\][^\n]*;[^\n]*", "", ops)
    ops = ('#include <stdint.h>\n#pragma GCC diagnostic ignored "-Wunused-function"\n'
           '#pragma GCC diagnostic ignored "-Wunused-variable"\n' + ops + "\n".join(lines) + "\n")
    open(ops_path, "w").write(ops)

    # m68kcpu.c: decode through the two levels, fixed cycles, no table setup.
    cpu_path = f"{build}/m68kcpu.c"
    cpu = open(cpu_path).read()
    cpu = replace(cpu, "extern void m68ki_build_opcode_table(void);",
                  "#include <stdint.h>\n"
                  "extern void (*const m68ki_handlers[])(void);\n"
                  f"extern const {index_type} m68ki_decode_index[];\n"
                  f"extern const uint16_t m68ki_decode_blocks[][{1 << shift}];\n"
                  f"#define M68KI_HANDLER(op) m68ki_handlers[m68ki_decode_blocks[m68ki_decode_index[(op) >> {shift}]][(op) & {(1 << shift) - 1}]]")
    cpu = replace(cpu, "m68ki_instruction_jump_table[REG_IR]();", "M68KI_HANDLER(REG_IR)();")
    cpu = replace(cpu, "USE_CYCLES(CYC_INSTRUCTION[REG_IR]);", "USE_CYCLES(4);")
    cpu = re.sub(r"= m68ki_cycles\[\d\];", "= NULL;", cpu)
    cpu = replace(cpu, "m68ki_build_opcode_table();", "/* decode table: tools/slim_musashi.py */")
    open(cpu_path, "w").write(cpu)

    header_path = f"{build}/m68kcpu.h"
    header = open(header_path).read()
    header = header.replace("CYC_INSTRUCTION[REG_IR]", "4")
    # Musashi's code (this header is included by its .c files only) leaves
    # variables unused in some handlers and in m68ki_exception_trapN: not
    # ours to fix, as m68kmake makes it again.
    header = '#pragma GCC diagnostic ignored "-Wunused-variable"\n' + header
    open(header_path, "w").write(header)

    print(f"slim_musashi: {len(names)} handlers, decode table {size} bytes "
          f"({len(blocks)} blocks of {1 << shift})")


main()
