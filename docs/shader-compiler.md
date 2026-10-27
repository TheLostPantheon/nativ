# The shader compiler: design

**Status: designed, not built.** Today's renderer uses the direct translator in `runtime/gfx/usse_spirv.c`. This
document describes the compiler meant to replace it, as part of the graphics plans in [How it works](architecture.md).
It was written on 2026-10-09 from a survey of `usse.c`, `usse_internal.h`, `usse_spirv.c`, `usse_run.c` and
`usse_dis.c`.

The idea: instead of translating each Vita shader instruction by instruction, lift a whole program into an
intermediate form, optimise it, and emit SPIR-V that a GPU's own compiler can do something useful with.

## Why the translator's output is slow

`usse_spirv.c` ports the interpreter's instruction handlers one by one, repeat by repeat, component by component:

- **Every register bank is an array of 32-bit words.** Every operand is a load or a store at word granularity, so the
  GPU's compiler sees memory traffic instead of values, and keeps the arrays alive for the whole program.
- **Half-precision values are converted in software every time.** Each read of a half-precision lane decodes it
  (about 15 operations) and each write encodes it (about 20). Nothing remembers that a word was just written, so it's
  decoded again at every read.
- **Nothing is optimised.** There's no common-subexpression elimination, dead-code removal or constant folding. One
  four-component half-precision multiply-add comes to about 300 SPIR-V words, and every repeat repeats them.
- **Control flow is limited.** Only forward predicated skips become structured `if`s, and each predicated instruction
  gets its own.

The fix isn't a cleverer emitter for the same model. It's a different model: values, not memory.

## What's already been measured

Before building any of this, we tested how much the half-precision conversions alone cost. On Unit 13 at render scale
3, they were most of the GPU's frame: 19.7 ms per frame with the software conversions, 6.9 ms with native ones.
Running `spirv-opt` over the existing output saved only about 2.5 ms.

The translator now uses the GPU's own conversions, kept bit-exact with the interpreter (`RECOMP_GPU_F16=masked`):
18.1 ms dropped to 7.5 ms, a locked 60 fps at 3x. So the compiler is no longer needed to make high render scales
playable. What it still offers: removing the remaining overhead (the generated Metal code is still 3 to 6 times larger
than it needs to be), headroom for weaker GPUs such as Android's, room for enhancements, and the resource model
(milestone 5). Measure again before each milestone.

## The intermediate form

- **One function per program**, made of basic blocks in SSA form. The secondary program stays on the CPU (see below).
  Values are scalars (f32, f16, u32, i32, bool), plus a few vector operations where the target wants vectors: texture
  coordinates and results, and outputs. Scalar SSA suits Apple's GPUs and is the simplest form to optimise; the GPU
  compilers re-vectorise where it pays.
- **Registers are an environment, not storage.** While walking the program, the lifter keeps a map from each register
  word to its current value, in the most useful form it's known in:
  - raw u32, for integer operations, memory access and anything that looks at bits;
  - f32, for a full-precision lane;
  - two half-precision lanes, each held as a float already rounded to half precision;
  - packed byte or fixed-point lanes.

  Converting between forms is an explicit operation, inserted only when a read wants a form the word isn't in. A value
  written as half precision and read back as half precision never touches its bits. That one rule removes most of the
  conversion code.
- **Rounding is explicit.** Writing a half-precision lane emits a quantise operation; writing a byte lane emits the
  interpreter's saturate-and-scale. The Vita's precision rules become ordinary operations the passes can reason about.
- **Read-before-write within an instruction**, which the interpreter handles with a staging buffer, comes for free:
  an instruction's sources are looked up before any of its results update the map.
- **Repeats are unrolled while lifting**, with their offsets worked out by the interpreter's own addressing code. It's
  shared C, not a second copy, so the two can't drift apart; an earlier miscompile lived in exactly such a copy.
- **Predicates are bools.** A predicated instruction becomes a select per result, with no branch, unless it has side
  effects (discarding the fragment, a memory store, writing depth); those become a structured `if`. Texture samples
  stay out of divergent control flow where the select form allows, because they need derivatives.
- **Control flow.** Forward branches become structured selections, and backward branches that form natural loops
  become structured loops. The interpreter already runs loops; the translator rejects them today. Anything irreducible
  falls back to the old path.
- **Dynamically indexed banks stay arrays**, per bank and per program: if any instruction indexes the temporary bank
  dynamically, that bank stays in memory for that program, and every other bank stays as values.
- **Inputs and outputs are explicit**: vertex and iterator inputs, uniforms, the framebuffer colour (read by
  framebuffer fetch or a copy, as the backend chooses) and the program's outputs. Everything else is dead at the end.

## The secondary program

The secondary program sets up per-draw uniforms and writes only those. It keeps running on the CPU interpreter for
each draw, as today. Lifting it too, and folding it to constants per material when its inputs are known, is a later
option.

## Optimisation passes

In order of expected value:

1. **Copy propagation and dead-code removal**, working back from the outputs. Most register traffic disappears with
   the arrays.
2. **Conversion removal**: unpacking a value just packed gives the original; quantising twice is quantising once;
   half-precision lanes only ever used as floats are never packed.
3. **Common-subexpression elimination** across repeats and components: the same swizzled source read for four
   components, the same uniform read in every repeat.
4. **Constant folding**: the constant ROM, immediates, `x*1`, `x+0`, double negation.
5. **Hoisting uniform loads**: uniforms are fixed for a draw, so each is loaded once.
6. **Later, if measurements ask for it**: recognising dot products, normalisation and matrix multiplies. The Metal and
   Vulkan compilers already do much of this once the code is clean.

## Two modes

- **Exact**, for verification. Every quantisation is the bit-exact software pair, and every value the interpreter
  would produce is produced. It must render byte-identically to today's translator.
- **Fast**, for play. Quantisation becomes the GPU's own instruction wherever the rounded value can be observed (packed
  into bits, or written to a half-precision output), and disappears where a value only flows into more float math.
  Chains the IR shows staying in half precision use native 16-bit arithmetic. It may differ from the reference within
  a measured tolerance, per program and per frame.

One open question for exact mode: Metal compiles with fast math on by default, which allows reassociation and fused
operations, and SDL's GPU API doesn't expose the compile options. That's the likely source of a small, unexplained
difference (1 to 3 levels out of 255) seen after the vertex-fetch change in d417e2a. Exact mode may need SPIR-V's
`NoContraction` on the quantisation chains, carried through by spirv-cross as `precise`.

## Verification

- **A local corpus** of every shader program in our captures, from all four games, saved with
  `RECOMP_REPLAY_SAVE_PROG` into `dump/decrypted/shaders/` and never committed. `usse_dis` statistics per program
  (instruction mix, half-precision share, repeats, indexing, branches) size each feature before it's written.
- **An IR interpreter**, small and in C, runs the lifted and optimised IR on the CPU. Differential tests run the USSE
  interpreter and the IR interpreter on the same inputs, random ones within each input's range plus real ones taken
  from replays, and compare every output word. Exact mode must match bit for bit; fast mode reports the distance per
  program. This tests the lifting and every pass without a GPU, in milliseconds per program.
- **Emission is tested by replays.** In exact mode, every capture replays byte-identically to the old translator at
  render scales 1 and 2. Fast mode is held to `framediff.py` tolerances and the regression gate.
- **Size and speed**: SPIR-V words and Metal source bytes per program, old against new, and interleaved A/B frame
  timing with `RECOMP_GPU_FENCE_STATS`, since absolute GPU speed drifts between sessions.
- `usse_dis` exiting non-zero on an unsupported encoding remains the coverage check.

## Fitting it in

The translator's interface stays the same, so `gfx_gpu.c` only changes which function it calls:
`RECOMP_SHADER_COMPILER=old|exact|fast`. If the compiler fails on a program, the old translator takes it, and failing
that the software path, each failure logged with its reason. The shader cache key gains the compiler mode; the
on-disk Metal cache is keyed by the SPIR-V already. The old translator retires once exact mode covers everything it
does.

## Milestones

| | What | Done when |
|---|---|---|
| M0 | Corpus and statistics | Every captured program of the four games saved locally, with a table of instruction mix, half-precision share, repeats, indexed banks and branch forms. |
| M1 | Lifter and IR interpreter | Differential tests: the whole corpus bit-exact against the USSE interpreter. |
| M2 | SPIR-V emission, exact mode | Replays byte-identical to the old translator on every capture at scales 1 and 2; the gate passes. |
| M3 | Passes 1 to 5, still exact | M2's parity holds; SPIR-V and Metal sizes and GPU time measured. Most of the gain is expected from arrays becoming values alone. |
| M4 | Fast mode | A per-program report within agreed bounds; frame tolerances; an A/B at render scale 3. |
| M5 | Decoding in the reader | View passes replaced by decodes inside the programs that read them (the resource model). |

M5 then leads into the frame-graph work in `gfx_gpu.c`.

## Risks

- **Indexed register access** in busy programs keeps a bank in memory. M0's statistics will say how common it is.
- **Exactness under Metal's fast math**, above.
- **Control flow** beyond forward skips is new ground for the GPU path. The interpreter remains the fallback.
- **Two executors of one instruction set** can drift apart. Sharing the addressing and lane helpers, and the
  differential tests, are the defence. Every instruction-set fix lands in `usse.c` first.
