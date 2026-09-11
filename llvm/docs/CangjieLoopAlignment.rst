Cangjie adaptive loop alignment
==============================

The Cangjie code generation pipeline chooses loop alignment from the estimated
machine code size of small AArch64 loops. A fixed alignment can place an
otherwise identical loop across a fetch or cache boundary when preceding code
changes. Aligning a small loop to a power-of-two window large enough to contain
it reduces this sensitivity to code layout.

Policy
------

``MachineBlockPlacement`` first applies its usual loop hotness, fallthrough and
size-optimization checks. It then considers the first block in the physical
layout of each eligible innermost loop. All blocks of the loop must be
contiguous. The size estimate includes the target's instruction size estimates
and padding for alignment of blocks inside the loop. Debug and other metadata
instructions contribute no bytes. Loops containing inline assembly retain the
ordinary target policy because assembler directives can depend on their
location.

For a loop that fits in 64 bytes, the preferred alignment is:

=====================  ===================
Estimated loop size    Alignment in bytes
=====================  ===================
Up to 16 bytes         16
17 to 32 bytes         32
33 to 64 bytes         64
=====================  ===================

A stronger target alignment is preserved, as is any alignment required by
blocks inside the loop. Larger loops, outer loops and split loops retain the
ordinary policy. Cold loops and functions optimized for size are not newly
aligned. The adaptive policy permits sufficient padding to realize the chosen
alignment instead of conditionally skipping it based on preceding code size.
The selected 16/32/64-byte alignment requires at most 63 bytes of padding.
An inherited stronger alignment can require more. When the ordinary target
policy leaves existing block alignment unchanged, the adaptive policy also
preserves it, including for loops that are too large or contain inline assembly.

This is a layout heuristic, not a runtime performance guarantee. Instruction
sizes are target estimates, and transformations after block placement can
change the final loop size. Performance and code size should be evaluated on
the intended hardware and workload.

Options and compatibility
-------------------------

The policy is enabled by default with ``--cangjie-pipeline`` on AArch64. Other
targets and non-Cangjie pipelines keep their existing behavior. In particular,
this implementation does not estimate X86 instruction sizes.

``--cj-adaptive-loop-alignment=false`` restores the ordinary target policy.
When invoking the compiler driver, pass this through ``--llc-options``.
The existing backend option is spelled ``--align-loops`` and takes a byte
alignment; an explicit nonzero value, including ``--align-loops=1``, takes
precedence over adaptive selection. ``--align-all-blocks`` and
``--align-all-nofallthru-blocks`` also retain precedence. An explicit
``--max-bytes-for-alignment`` remains in effect and may prevent the assembler
from realizing the selected alignment.

The transformation changes only block alignment and its padding allowance.
It does not change the CFG, instruction semantics, calling conventions, or
public language and library APIs.
