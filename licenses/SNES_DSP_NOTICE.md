# S-DSP reference provenance and scope

`src/snes_dsp_core.c` and `include/snesrecomp_platform/snes_dsp_core.h`
derive from `runner/src/snes/dsp.c` and `dsp.h` in
https://github.com/Cellenseres/snesrecomp, revision
`678b0656de909ffb9b762ff32dabc9e13425c811`.
Copyright (c) 2026 Matthew Stanley. These derived files retain the
PolyForm Noncommercial License 1.0.0, copied without alteration in
`SNES_DSP_REFERENCE_LICENSE.txt`. They are NOT BSD-licensed OpenDSP code.
Distributing a custom firmware containing this module must retain this notice
and that license. This does not change licenses of other platform/OpenDSP files.
Publication/commercial licensing remains subject to the upstream license and
rightsholders; this implementation does not grant additional permissions.

The module preserves the pinned core's default supported sample-level output,
including its separate Gaussian products shifted by 10, signed-16 wrap after
three products, fourth product, clamp and final shift by 1. This differs in
rounding from variants that shift each Gaussian product by 11. It also preserves
the reference sample-level envelope counters, alternating-sample KON/KOF poll,
BRR history retained at KON, and lack of hardware five-sample KON pipeline. It
does not claim transistor-level/cycle-phase equivalence with SNES hardware.
The opt-in enhancement shadow mixer and host output ring are not synthesizer
state and are not included. The event engine provides lossless output capacity
checks before synthesis instead of the old host ring's overflow drop.
