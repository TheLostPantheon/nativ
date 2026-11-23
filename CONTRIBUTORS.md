# Contributors

Project Nativ is developed by [TheLostPantheon](https://github.com/TheLostPantheon). Pull requests are open only to the
maintainer, but the work of others has gone into it, and this file names them. Ideas taken from someone's issue,
analysis or closed pull request are reimplemented here and credited in the commit that uses them. Code reused from
someone carries a `Co-authored-by` line, so GitHub lists them as a contributor as well.

| Contributor | What went in |
|---|---|
| [Tzeentchnet](https://github.com/Tzeentchnet) | The Windows firmware secure-module report fix (#6) and the Windows source-checkout docs (#5). The wording that says when online discovery starts (#4). The plan for running the tests that need no game in CI (#84). The analysis that made FSR's half precision safe to turn on (#99). Finding that SOP2, SOP2M, SOP3 and I8MAD work on whole packed words, the back faces' stencil state, and the F16 render targets' channel order (#77). |
| [Luufen](https://github.com/Luufen) | Compiling the runtime to cached objects (#73), the idea behind the faster rebuilds in #105. The idea of the kernel timer imports (#35). The idea of the audio decoder (sceAudiodec) for ATRAC9 and AAC (#54). Finds in their fork, reimplemented here: VBW's repeat count, USSE subroutine calls, draws with the viewport disabled, GPU views of render targets of any swizzle, Thumb IT-block returns, tail jumps that set LR, ARM/Thumb classification, computed jumps to another function, the sceCtrl record size, the exclusive monitor on context switches, sceAudioOutOutput's result, fiber stacks, and the NGS player's start buffer and Play. From their Phantasy Star Nova work: the kernel object table past a million objects, sceKernelCancelEventFlag, a Thumb tail call keeping its target Thumb, an entry inside an instruction, pointer-only functions that keep their switch cases, ARM methods only a vtable reaches, a register BLX's target state for missed functions, and Windows' short sleeps and timed waits. |
| [Ecstatic-Magician](https://github.com/Ecstatic-Magician) | Palettized textures (P4, P8), found in their Persona 4: Dancing All Night work. |
| [nckstwrt](https://github.com/nckstwrt) | From their Vita3K-Plus fork: libfios2's lost scheduler kick (a finished decompression job left queued, every later read behind it) and its patch, applied here before recompiling (recompiler/guestfix.py). |

Everyone who files an issue with its logs helps as much as code does: most fixes start from a report.

To add to this file, a commit that uses someone's work adds a line here or extends theirs.
