# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Check final object layout for every small loop size and preceding offset.

This checks code generation, not execution time. Compare the actual bytes of
the loop against the ordinary policy to ensure only its padding changes.
"""

import argparse
import json
from pathlib import Path
import subprocess


def generate_mir(cases):
    chunks = []
    for size, prefix in cases:
        chunks.append("\n".join([
            "---",
            "name: loop_{}_{}".format(size, prefix),
            "alignment: 64",
            "tracksRegLiveness: true",
            "body: |",
            "  bb.0:",
            "    successors: %bb.1(0x80000000)",
            "    liveins: $x0",
            *("    HINT 0" for _ in range(prefix)),
            "  bb.1:",
            "    successors: %bb.1(0x7c000000), %bb.2(0x04000000)",
            "    liveins: $x0",
            *("    HINT 0" for _ in range(size // 4 - 2)),
            "    $x0 = SUBSXri $x0, 1, 0, implicit-def $nzcv",
            "    Bcc 1, %bb.1, implicit $nzcv",
            "  bb.2:",
            "    RET undef $lr",
            "...",
        ]))
    return "\n".join(chunks) + "\n"


def build(args, mir, mode):
    assembly = str(args.output) + "." + mode + ".s"
    obj = str(args.output) + "." + mode + ".o"
    subprocess.run([
        args.llc, "-mtriple=aarch64-linux-gnu", "-mcpu=cortex-a53",
        "-cangjie-pipeline", "-verify-machineinstrs",
        "-cj-adaptive-loop-alignment=" + mode,
        "-start-before=block-placement", str(mir), "-o", assembly,
    ], check=True)
    subprocess.run([
        args.mc, "-triple=aarch64-linux-gnu", "-filetype=obj",
        "-save-temp-labels", assembly, "-o", obj,
    ], check=True)
    result = json.loads(subprocess.check_output([
        args.readobj, "--symbols", "--sections", "--elf-output-style=JSON", obj,
    ]))
    info = next(iter(result[0].values()))
    symbols = {
        item["Symbol"]["Name"]["Value"]: item["Symbol"]["Value"]
        for item in info["Symbols"]
    }
    section = next(item["Section"] for item in info["Sections"]
                   if item["Section"]["Name"]["Value"] == ".text")
    raw = Path(obj).read_bytes()
    text = raw[section["Offset"]:section["Offset"] + section["Size"]]
    return symbols, text


def main():
    parser = argparse.ArgumentParser()
    for name in ("llc", "mc", "readobj", "output"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    cases = [(size, prefix) for size in range(8, 65, 4)
             for prefix in range(16)]
    mir = Path(str(args.output) + ".mir")
    mir.write_text(generate_mir(cases))
    adaptive, code = build(args, mir, "true")
    ordinary, baseline = build(args, mir, "false")
    for index, (size, prefix) in enumerate(cases):
        name = "loop_{}_{}".format(size, prefix)
        header = ".LBB{}_1".format(index)
        start, old_start = adaptive[header], ordinary[header]
        padding = start - adaptive[name] - prefix * 4
        assert 0 <= padding <= 63, (name, "padding", padding)
        assert start % 64 + size <= 64, (name, "crosses 64-byte window")
        assert adaptive[".Lfunc_end{}".format(index)] - start == size + 4, name
        assert code[start:start + size] == baseline[old_start:old_start + size], (
            name, "loop instructions changed")
    print("Verified 240 loop layouts: one 64-byte window and unchanged loop instructions.")


if __name__ == "__main__":
    main()
