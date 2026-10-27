# The USSE instruction set and GXP program format

A reference for the Vita's GPU shaders: the GXP file a shader program comes in, the register model, every instruction
group, and how a program's inputs are filled and its outputs read. Nativ's shader interpreter (`runtime/gfx/usse.c`)
and its SPIR-V translator (`runtime/gfx/usse_spirv.c`) are built from it, and fixes to our understanding land here
first.

Sections 1 and 2 describe the program file and the registers, section 3 every instruction, and section 4 the
outputs. Section 5 collects the mistakes most likely to bite an implementation, and section 6 what's still unknown.

## 0. About this document

### 0.1 Sources and confidence

The Vita's GPU is an Imagination PowerVR SGX543MP4+. Its shader cores run *USSE* (Universal Scalable Shader Engine)
code in 64-bit instruction words. There's no public manual for it. The only detailed public description is the Vita3K
emulator's shader recompiler, which is GPL-licensed: we read it to learn how the hardware behaves, and wrote
everything here from scratch without copying any code. A few hardware facts, such as the constant-register table
(§2.7), were cross-checked against the community wiki (wiki.henkaku.xyz, "SGX543"). Since then, behaviour found by
running real games through our own interpreter has corrected and extended it.

Vita3K translates whole shaders to SPIR-V, which has two consequences for how far to trust each part:

1. **Bit positions are reliable.** Vita3K decodes every instruction with tables of 64-character bit patterns, and the
   tables in §3 were generated from those patterns mechanically.
2. **Meanings are reliable only where Vita3K implements them.** Many fields are decoded but never used: scheduling
   hints, cache bits, some addressing modes. Some instructions are stubs (PHAS, SPEC, I8MAD2). What Vita3K does
   implement runs a large library of commercial games correctly, but it's a translator, not a cycle-accurate model.
   So this document marks its claims:
   - **Confirmed (Vita3K)**: implemented and exercised by real games.
   - **Uncertain**: Vita3K guesses, has a TODO, or comments that it is not sure.
   - **Unimplemented in Vita3K**: decoded but ignored; the meaning given is a reading of the field
     name only. These are listed together in §6.

### 0.2 Conventions

- Bits are numbered 63 (MSB) … 0 (LSB) within the 64-bit instruction word. Instruction words are
  stored little-endian in the GXP file, so read them as `uint64_t` from 8-byte-aligned offsets.
- `insn[a:b]` means bits a down to b inclusive, read as an unsigned integer.
- "Register" always means a **32-bit** register word unless stated otherwise.
- Swizzle channels are written x, y, z, w (index 0–3). A 4-bit write mask has bit 0 = x,
  bit 1 = y, bit 2 = z, bit 3 = w.
- Struct offsets are byte offsets. All multi-byte fields are little-endian.

### 0.3 How a draw executes (overview)

```
per draw:
  host/driver fills SA registers of each program: uniform blocks, literals, buffer base
      addresses, texture-state words (for dependent texture reads)          (§1.9)
  run the vertex program's SECONDARY program once  -> writes SA registers     (§1.10)
  run the fragment program's SECONDARY program once -> writes SA registers
  per vertex:
      PA registers <- vertex attributes, converted per the GXP parameter types (§1.11)
      run the vertex PRIMARY program -> O registers (position first, then varyings) (§4.1)
  rasterise; per fragment:
      PA registers <- interpolated varyings and non-dependent texture reads (§1.12)
      run the fragment PRIMARY program -> color in O0 or PA0, optional depth via DEPTHF (§4.2)
      KILL may discard the fragment
```

---

## 1. GXP program binary

This section describes the file a shader program comes in: its header, parameters, constants and the tables that say
how the program is fed. A GXP file (a `SceGxmProgram` in memory) is a single relocatable blob: every pointer inside it is a
**self-relative offset**, measured from the address of the offset field itself. Games pass the
blob's address to `sceGxmShaderPatcherRegister*`; the blob is never modified.

### 1.1 Header (`SceGxmProgram`, 156 bytes)

"rel@N" means "self-relative: target = base + N + value".

| Off | Size | Field | Meaning |
|---|---|---|---|
| 0 | 4 | magic | `"GXP\0"` |
| 4 | 1 | major_version | ≥ 1 |
| 5 | 1 | minor_version | ≥ 4 |
| 6 | 2 | sdk_version | e.g. 0x350 = SDK 3.50 |
| 8 | 4 | size | size of the program (excludes trailing pad after the parameter table) |
| 12 | 4 | binary_guid | |
| 16 | 4 | source_guid | |
| 20 | 4 | program_flags | §1.2. **Bit 0 = 1 → fragment program, 0 → vertex program.** |
| 24 | 4 | buffer_flags | 2 bits per buffer: 1 = loaded into registers, 2 = read from memory |
| 28 | 8 | texunit_flags[2] | 4 bits per texture unit: 1 = non-dependent read, 2 = dependent read |
| 36 | 4 | parameter_count | number of 16-byte entries in the parameter table |
| 40 | 4 | parameters_offset | rel@40 → `SceGxmProgramParameter[parameter_count]` (§1.3) |
| 44 | 4 | varyings_offset | rel@44 → varyings block (§1.7) |
| 48 | 2 | primary_reg_count | PA registers used (0 is a hint that symbols were stripped) |
| 50 | 2 | secondary_reg_count | SA registers used |
| 52 | 4 | temp_reg_count1 | temporaries |
| 56 | 2 | temp_reg_count2 | temporaries in the per-sample ("selective rate") phase |
| 58 | 2 | primary_program_phase_count | |
| 60 | 4 | primary_program_instr_count | number of 64-bit instructions in the primary program |
| 64 | 4 | primary_program_offset | rel@64 → first primary instruction |
| 68 | 4 | secondary_program_instr_count | |
| 72 | 4 | secondary_program_offset | rel@72 → first secondary instruction |
| 76 | 4 | secondary_program_offset_end | rel@76 → **one past** the last secondary instruction |
| 80 | 4 | scratch_buffer_count | |
| 84 | 4 | thread_buffer_count | bytes of per-thread scratch (thread buffer, container 18) |
| 88 | 4 | literal_buffer_count | size of the literal buffer (container 16) |
| 92 | 4 | data_buffer_count | |
| 96 | 4 | texture_buffer_count | size of the texture buffer (container 15) |
| 100 | 4 | default_uniform_buffer_count | size, in 32-bit words, of the default uniform buffer (14) |
| 104 | 4 | literal_buffer_data_offset | rel@104 → literal-buffer bytes |
| 108 | 4 | compiler_version | version << 4 |
| 112 | 4 | literals_count | |
| 116 | 4 | literals_offset | rel@116 → `SceGxmProgramLiteral[literals_count]` (§1.6) |
| 120 | 4 | uniform_buffer_count | |
| 124 | 4 | uniform_buffer_offset | rel@124 → `SceGxmUniformBufferInfo[uniform_buffer_count]` (§1.5) |
| 128 | 4 | dependent_sampler_count | |
| 132 | 4 | dependent_sampler_offset | rel@132 → `SceGxmDependentSampler[...]` (§1.8) |
| 136 | 4 | texture_buffer_dependent_sampler_count | |
| 140 | 4 | texture_buffer_dependent_sampler_offset | rel@140 |
| 144 | 4 | container_count | |
| 148 | 4 | container_offset | rel@148 → `SceGxmProgramParameterContainer[container_count]` (§1.4) |
| 152 | 4 | sampler_query_info_offset | rel@152 → array of `uint16_t` (unused by Vita3K) |

The **primary program** is `primary_program_instr_count` words at `base+64+primary_program_offset`.
The **secondary program** spans `[base+72+secondary_program_offset, base+76+secondary_program_offset_end)`.
It exists iff that range is non-empty, which is equivalent to
`secondary_program_offset < secondary_program_offset_end + 4`. Its length in instructions is
`(end − start) / 8` (Vita3K uses this rather than `secondary_program_instr_count`).

For `parameters_offset` and `varyings_offset` an offset of 0 means "absent".

### 1.2 `program_flags`

| Bit | Name | Meaning |
|---|---|---|
| 0 | FRAGMENT | 1 = fragment program |
| 1 | PER_INSTANCE_MODE | |
| 3 | DISCARD_USED | program may KILL |
| 4 | DEPTH_USED | program replaces depth (DEPTHF). Vita3K keys depth-replace off the presence of a DEPTHF instruction, not this flag. |
| 5 | SPRITECOORD_USED | reads point-sprite coordinates |
| 6 | NATIVECOLOR_USED | final color is in **O0**, not PA0 (§4.2) |
| 7 | FRAGCOLOR_USED | program reads the current framebuffer color (programmable blending; preloaded into O0, §4.2) |
| 8 | NATIVECOLOR_MSAA2X | |
| 9 | NATIVECOLOR_MSAA4X | |
| 14 | BUFFER_STORE | (uncertain) program stores to memory |
| 16 | OUTPUT_UNDEFINED | program writes no color. With DISCARD and DEPTH clear, the program has no effect. |
| 17 | INDEX_USED | vertex program reads the vertex index (§1.11) |
| 18 | INSTANCE_USED | vertex program reads the instance index (§1.11) |

### 1.3 Parameter table (`SceGxmProgramParameter`, 16 bytes each)

| Off | Bits | Field | Meaning |
|---|---|---|---|
| 0 | 32 | name_offset | signed, relative to the **start of this entry** → NUL-terminated name. Struct members appear as `"struct.field"`. |
| 4 | 3:0 | category | 0 ATTRIBUTE, 1 UNIFORM, 2 SAMPLER, 3 AUXILIARY_SURFACE, 4 UNIFORM_BUFFER |
| 4 | 7:4 | type | 0 F32, 1 F16, 2 C10, 3 U32, 4 S32, 5 U16, 6 S16, 7 U8, 8 S8, 9 AGGREGATE |
| 5 | 3:0 | component_count | 1–4 (constants and attributes) |
| 5 | 7:4 | container_index | container (§1.4) holding the value (uniforms) |
| 6 | 8 | semantic | attributes only: 0 NONE, 1 ATTR, 2 BCOL, 3 BINORMAL, 4 BLENDINDICES, 5 BLENDWEIGHT, 6 COLOR, 7 DIFFUSE, 8 FOGCOORD, 9 NORMAL, 10 POINTSIZE, 11 POSITION, 12 SPECULAR, 13 TANGENT, 14 TEXCOORD, 15 INDEX, 16 INSTANCE. For uniform buffers, 3 appears to mean "written to". |
| 7 | 8 | semantic_index | for samplers, **bit 4 set = cube sampler** |
| 8 | 32 | array_size | elements (1 = not an array) |
| 12 | 32 | resource_index | meaning depends on category (below) |

`resource_index` by category:

| Category | resource_index is… |
|---|---|
| ATTRIBUTE | the **PA register number** (32-bit units) where the attribute is placed. This is the value `sceGxmProgramParameterGetResourceIndex` returns and games put in `SceGxmVertexAttribute.regIndex`. |
| UNIFORM | offset in 32-bit words **within its buffer** (`container_index`). If that buffer's container is register-resident, the uniform also lives in **SA[container.base_sa_offset + resource_index]**. |
| SAMPLER | texture unit index (0–15), as used by `sceGxmSetFragmentTexture` / `sceGxmSetVertexTexture`. |
| UNIFORM_BUFFER | the buffer index (0–13). `array_size` is its size in bytes. |
| AUXILIARY_SURFACE | (Vita3K never handles these.) |

Size of a uniform in the buffer: `component_count × sizeof(type)` per element. For arrays
(`array_size ≠ 1`) each element is padded to 8 bytes if it is a multi-component float type,
otherwise to 4 bytes. (This is Vita3K's sizing rule for bounds; not otherwise needed to execute.)

### 1.4 Containers (`SceGxmProgramParameterContainer`, 8 bytes each)

| Off | Size | Field |
|---|---|---|
| 0 | 2 | container_index |
| 2 | 2 | (unknown) |
| 4 | 2 | base_sa_offset: first SA register of this container's register-resident window |
| 6 | 2 | size_in_f32: number of 32-bit words of the buffer mirrored into SA |

Find a container by scanning for a matching `container_index` (absent = not register-resident).

| container_index | Meaning |
|---|---|
| 0–13 | user uniform buffers 0–13 |
| 14 | default uniform buffer (`sceGxmSetVertexDefaultUniformBuffer` / fragment equivalent) |
| 15 | texture buffer |
| 16 | literal buffer (compiler constants) |
| 17 | scratch |
| 18 | thread buffer (per-thread private memory) |
| 19 | "DATA": holds buffer base addresses (§1.5), dependent-sampler state (§1.8), and literals if container 16 is absent |

**Register-resident uniform windows.** For each container 0–14 present, before the program runs,
SA words `[base_sa_offset, base_sa_offset + size_in_f32)` hold words `[0, size_in_f32)` of the
corresponding uniform buffer's memory. If the program has a default uniform buffer
(`default_uniform_buffer_count ≠ 0`) and no container 14, Vita3K treats it as a window of
`default_uniform_buffer_count` words at SA0. Everything beyond the window is read with LDST from
memory (§1.5, §3.22).

### 1.5 Uniform-buffer address table (`SceGxmUniformBufferInfo`, 8 bytes each)

| Off | Size | Field | Meaning |
|---|---|---|---|
| 0 | 2 | reside_buffer | which buffer: 0–13 user, 14 default, 15 texture buffer, 16 literal buffer, 18 thread buffer |
| 2 | 2 | ldst_base_offset | SA register (relative to container 19's `base_sa_offset`) that receives the base address |
| 4 | 4 | ldst_base_value | signed byte bias added to the base address |

For each entry, before the program runs:

```
SA[container19.base_sa_offset + ldst_base_offset] = address_of(reside_buffer) + ldst_base_value
```

`ldst_base_value` is almost always −4; it pairs with LDST's `moe_expand = 0` adding +4 (§3.22).

`address_of` per buffer:
- 0–14: the GPU address the game bound for that uniform buffer (`sceGxmSet*UniformBuffer`,
  default buffer from `sceGxmSet*DefaultUniformBuffer` / reserve calls).
- 16 (literal buffer): the address of the program's own literal data
  (`base + 104 + literal_buffer_data_offset`, `literal_buffer_count` bytes).
- 15 (texture buffer): a block holding the 16-byte texture-control words of the bound textures
  (`texture_buffer_count` bytes). Used for runtime-indexed texture selection; Vita3K supports
  only a narrow case of it.
- 18 (thread buffer): per-thread private memory of `thread_buffer_count` bytes for the whole GPU.
  Vita3K assumes it is split evenly among 4 cores × 4 pipes and gives each invocation a private
  array of `thread_buffer_count / 64` floats. A CPU interpreter can give each invocation its own
  buffer.

Vita3K also tags the base value with a 4-bit buffer number in bits 31:28. That is its own
host-side scheme, not hardware. **For a CPU interpreter, store the real guest address** (or any
32-bit handle the LDST implementation understands).

### 1.6 Literals (`SceGxmProgramLiteral`, 8 bytes each)

| Off | Size | Field |
|---|---|---|
| 0 | 4 | offset: SA register relative to the literal container's base |
| 4 | 4 | data: 32 raw bits (usually an F32) |

`SA[container(16).base_sa_offset + offset] = data`. If container 16 is absent, container 19's base is
used. Larger constant tables live in the literal buffer and are read with LDST (§1.5).

### 1.7 Varyings block (`SceGxmProgramVertexVaryings`, 32 bytes, packed)

The first 12 bytes differ between vertex and fragment programs:

| Off | Size | Vertex program | Fragment program |
|---|---|---|---|
| 0 | 8 | attrib_pa_regs (u64): bitmask of PA registers holding attributes | unk[8] |
| 8 | 1 | untyped_pa_regs (u64, bytes 8–15): bit *r* set → attribute at PA *r* is **untyped**, copied raw (§1.11) | fragment_output_start (u8) |
| 9 | 1 | (cont.) | unk (u8) |
| 10 | 1 | (cont.) | output_param_type (u8, a §1.3 type code): format of the color output |
| 11 | 1 | (cont.) | output_comp_count (u8): components of the color output |
| 12 | 2 | (cont.) | varyings_count (u16): number of attribute descriptors (§1.12) |
| 14 | 2 | (cont.) | (pad; possibly the count of non-dependent samplers) |
| 16 | 4 | vertex_outputs1 | |
| 20 | 4 | vertex_outputs2 | |
| 24 | 4 | texcoord_pack_format: 1 bit per texcoord, 0 = F32, 1 = F16 | |
| 28 | 2 | semantic_index_offset: PA register receiving the vertex index | |
| 30 | 2 | semantic_instance_offset: PA register receiving the instance index | |

In a fragment program, the field at offset 16 (`vertex_outputs1`) is reused as a **self-relative
offset** (rel@16) to an array of `varyings_count` attribute descriptors (§1.12).

#### Vertex outputs (vertex programs)

`vertex_outputs1`:

| Bit(s) | Output |
|---|---|
| 7:0 | CLIP0–CLIP7 (user clip distances) |
| 8 (0x100) | PSIZE (point size) |
| 9 (0x200) | FOG |
| 10 (0x400) | COLOR1 |
| 11 (0x800) | COLOR0 |

POSITION is always present (not flagged).

`vertex_outputs2`: ten 3-bit groups, texcoord *n* in bits `[3n+2 : 3n]`. A group ≠ 0 means
TEXCOORDn is written. Group bits: bit 0 = uses x,y; bit 1 = uses z; bit 2 = uses w. So the
component count is `2·b0 + b1 + b2`.

The output-register layout these fields imply is in §4.1.

### 1.8 Dependent samplers (`SceGxmDependentSampler`, 4 bytes each)

| Off | Size | Field | Meaning |
|---|---|---|---|
| 0 | 2 | resource_index_layout_offset | `texture_unit * 4 + word` (word 0–3 of the 16-byte texture control block) |
| 2 | 2 | sa_offset | SA register, relative to container 19's base |

Before the program runs:

```
unit = resource_index_layout_offset / 4;  word = resource_index_layout_offset % 4
SA[container19.base_sa_offset + sa_offset] = texture_control_word[unit][word]
```

So each texture used by an SMP instruction occupies **four consecutive SA registers** holding its
`SceGxmTexture` control words. SMP names the texture by the SA register number of word 0 (§3.13).
"Dependent" means the coordinates are computed by the shader. Non-dependent reads are done by the
iterator hardware before the program starts (§1.12). If a sampler has no symbol, its unit index is
still recoverable from this table.

`SceGxmTexture` control words, for reference (bit 0 = LSB of each word):

| Word | Bits | Field |
|---|---|---|
| 0 | 2:0 | unk0 |
| 0 | 5:3 | vaddr_mode |
| 0 | 8:6 | uaddr_mode |
| 0 | 9 | mip_filter |
| 0 | 11:10 | min_filter |
| 0 | 13:12 | mag_filter |
| 0 | 16:14 | unk1 |
| 0 | 20:17 | mip_count (levels − 1, wraps mod 16; 0 means 1 level) |
| 0 | 26:21 | lod_bias |
| 0 | 28:27 | gamma_mode |
| 0 | 30:29 | unk2 |
| 0 | 31 | format0 (top bit of the format code) |
| 1 | 11:0 / 23:12 | height−1 / width−1 (linear/tiled/strided); for swizzled and cube textures, log2 forms in bits 3:0 and 15:12 |
| 1 | 28:24 | base_format (5 bits) |
| 1 | 31:29 | type (`SceGxmTextureType >> 29`: 0 swizzled, 2 cube, 3 linear, 4 tiled, 5 swizzled-arbitrary, 6 linear-strided, 7 cube-arbitrary) |
| 2 | 1:0 | lod_min0 |
| 2 | 31:2 | data_addr >> 2 |
| 3 | 25:0 | palette_addr >> 6 |
| 3 | 27:26 | lod_min1 |
| 3 | 30:28 | swizzle (component order) |
| 3 | 31 | normalize_mode |

The full texture format is `(format0 << 31) | (base_format << 24) | (swizzle << 12)`.

### 1.9 Register initialisation per draw (summary)

Before the secondary program runs:

1. For each register-resident uniform container (0–14): copy the buffer window into SA (§1.4).
2. For each literal: write SA (§1.6).
3. For each uniform-buffer-info entry: write the buffer base address into SA (§1.5).
4. For each dependent-sampler entry: write the texture control word into SA (§1.8).

On hardware these copies are performed by a DMA/PDS step programmed by the driver from the same
tables. Vita3K performs them on the host, and an interpreter should do the same.

### 1.10 The secondary program ("uniform setup")

The secondary program runs **once per draw** (per program) before any vertex or fragment instance.
It precomputes values that are the same for every instance, such as matrix products or derived
constants, and leaves them in SA registers. These are read-only for the primary program. Rules:

- Its results go to the SA bank. Vita3K remaps register banks inside the secondary program: any
  operand that decodes to TEMP, PRIMATTR, OUTPUT, SECATTR, INDEXED or GLOBAL is treated as **SA**,
  with the same number. FPINTERNAL, FPCONSTANT and IMMEDIATE operands are left alone. SMP or LDST
  destinations that would be PA become SA. **Uncertain:** this is Vita3K's model. It works for
  commercial games, but the hardware's real view of TEMP inside the secondary program may be a
  separate bank. An interpreter adopting it should make secondary-program TEMP/PA/O references
  alias SA.
- There are no PA inputs (no per-vertex data).
- Vita3K treats the fragment program's secondary program the same way.

### 1.11 Vertex inputs → PA registers

For each ATTRIBUTE parameter, the vertex fetch unit writes the attribute into **PA[resource_index]**
onward. `SceGxmVertexAttribute` (8 bytes: streamIndex u16, offset u16, format u8, componentCount u8,
regIndex u16) gives the memory format, and `regIndex` equals the parameter's `resource_index`.

Placement in PA uses the **parameter's** type (`SceGxmProgramParameter.type`) and packing (§2.5):
F32 takes one register per component, F16/U16/S16 two components per register, U8/S8 four per
register, and C10 three per register. The memory format is converted to the parameter type:
normalized formats (U8N, S8N, U16N, S16N) give 0..1 or −1..1 floats, and non-normalized integer
formats give the integer value as a float when the parameter is float. Matrices and arrays occupy
`array_size × component_count` consecutive components from `resource_index` on, packed the same
way.

**Untyped attributes** (bit `resource_index` set in `untyped_pa_regs`): copy the raw bytes from the
vertex stream into the PA registers with no conversion. The data is reinterpreted by the shader
(for example, packed bytes unpacked with VPCK). For C10 the byte count is `ceil(n × 10 / 8)`.

Special inputs, written as one 32-bit integer each:
- `program_flags.INDEX_USED` → **PA[semantic_index_offset]** = vertex index (u32)
- `program_flags.INSTANCE_USED` → **PA[semantic_instance_offset]** = instance index (u32)

If the program has no ATTRIBUTE parameters (symbols stripped) but `primary_reg_count ≠ 0`, the
attribute→register mapping must come from the game's `SceGxmVertexAttribute.regIndex` and
`format` values instead (Vita3K does this from the vertex-program creation call).

### 1.12 Fragment inputs: iterators and non-dependent texture reads

A fragment program's PA registers are **filled by fixed-function hardware before the primary
program starts**. The shader never interpolates. The descriptor array (§1.7) describes what goes
where. Each `SceGxmProgramAttributeDescriptor` is 16 bytes:

| Off | Size | Field |
|---|---|---|
| 0 | 4 | attribute_info |
| 4 | 4 | resource_index (texture unit for a texture read) |
| 8 | 4 | size |
| 12 | 4 | component_info |

Descriptors are processed **in order**, with a running PA cursor `pa` starting at 0. Each
descriptor may produce up to two entries in this order: an interpolated varying, then a texture
read.

**A. Interpolated varying** — present when `(attribute_info & 0x4000F000) != 0xF000`:

| attribute_info bits | Meaning |
|---|---|
| 15:12 = 0–9 | TEXCOORD0–9 |
| 15:12 = 0xA / 0xB | COLOR0 / COLOR1 |
| 15:12 = 0xC | FOG |
| 15:12 = 0xD | POSITION (window-space fragment coordinate: x, y in pixels, z, w) |
| bit 30 | point-sprite coordinate (overrides 15:12) |
| 23:22 | components − 1 |
| 29:28, 20 | storage type: `0x20000000` = F16; `0x10000000` = "fixed" (likely C10, uncertain); `0x00100000` = F32 for colors; none of these = F32 for non-colors and **U8** for COLOR0/1 |

The interpolated value is written to PA starting at `pa`, packed by its storage type (§2.5).
Then `pa += ((size >> 4) & 3) + 1` registers.

**B. Non-dependent texture read** — present when `(attribute_info & 0x40F) != 0xF`:

| Field | Meaning |
|---|---|
| attribute_info bits 3:0 | texcoord used as the coordinate (0–9) |
| attribute_info bit 10 (0x400) | coordinate is the point-sprite coordinate instead |
| attribute_info bits 9:8 | `1` (0x100): sample with `.xy`; `3` (0x300): projective, `.xyz` with divide by z (for cube maps: plain 3-D direction, no divide); `2` (0x200): projective, `.xyw` with divide by w (uncertain) |
| attribute_info bit 4 | centroid sampling |
| resource_index | texture unit |
| component_info bits 5:4 | result format: 0 = raw/native integral packed texel, 2 = F16, 3 = F32 (1 unknown) |
| component_info bit 6 | when set, result count = texture's component count (else 4) — not supported by Vita3K |

The fetched texel is written to PA at `pa`, packed per the result format, and then
`pa += ((size >> 6) & 3) + 1`.

These descriptor rules come from Vita3K's reverse engineering. The bit meanings are confirmed for
the common cases (texcoords, colors, plain 2-D reads). Projective variants, "fixed" type and
centroid are lightly tested.

### 1.13 Recovering names and types

- Uniform PA/SA mapping: `SA[container(c).base_sa_offset + resource_index]`. Vita3K's logs print
  this as `[DEFAULT + 4] sa20 = (float4) u_matrix`.
- Shaders with stripped symbols still have containers, literals, buffer infos and dependent
  samplers, which is all execution needs. Parameter names only help debugging.

---

## 2. Register model

This section describes the registers a program works on: the banks, how operands name them, how values are packed into
32-bit words, and the modifiers, predicates and repeats that apply to every instruction.

### 2.1 Banks

| Bank | Disasm | Size (Vita3K) | Per-instance? | Purpose |
|---|---|---|---|---|
| TEMP | `r` | 80 words (hardware allows more; `temp_reg_count1`) | yes | scratch |
| PRIMATTR (PA) | `pa` | 128 words | yes | vertex attributes or fragment iterators (inputs); also a result bank |
| SECATTR (SA) | `sa` | 128 words | no (per draw) | uniforms, literals, buffer addresses, texture state |
| OUTPUT (O) | `o` | 80 words | yes | vertex outputs, fragment native color |
| FPINTERNAL (GPI) | `i` | 3–4 registers × **128 bits** | yes | ALU-internal registers, used by VMAD/VDP/VDUAL |
| SPECIAL | — | — | — | encoding only: resolves to GLOBAL or FPCONSTANT (§2.3) |
| GLOBAL | (none) | unknown (6-bit number) | — | global registers. Vita3K has no storage for them and reads them as 0. |
| FPCONSTANT | `c` | 64 × 64 bits, read-only | — | hardware constant ROM (§2.7) |
| IMMEDIATE | `#n` | — | — | the register-number field is the value (§2.6) |
| INDEX | `idx` | 2 | yes | index registers idx1/idx2 for INDEXED addressing |
| INDEXED1 / INDEXED2 | `r[idx1*2+k]` | — | — | indirect addressing through idx1/idx2 (§2.4) |
| PREDICATE | `p` | 4 booleans, p0–p3 | yes | condition flags written by VTST |

"Per-instance" banks are private to each vertex or fragment. SA is shared, read-only during the
primary program.

**Storage model.** Every bank except FPINTERNAL, FPCONSTANT, PREDICATE and INDEX is a flat array
of 32-bit words. An operand addresses a starting word plus a swizzle that selects components
(§2.5). FPINTERNAL registers are 128 bits: internal register *k* = four 32-bit words `4k..4k+3`,
always holding full F32 lanes (F16 data is widened to F32 there, and 8/16-bit integers to 32
bits).

### 2.2 Bank fields in operands

Three operand slots have different bank encodings. Most instructions have a 1-bit "ext" (extended
bank) bit per operand next to the bank field.

**Destination** (2-bit `dest_bank` + 1-bit `dest_bank_ext`):

| bank | ext = 0 | ext = 1 |
|---|---|---|
| 0 | TEMP | SECATTR |
| 1 | OUTPUT | SPECIAL |
| 2 | PRIMATTR | INDEX |
| 3 | INDEXED1 | INDEXED2 |

**src0** (1-bit `src0_bank` + 1-bit `src0_bank_ext`):

| bank | ext = 0 | ext = 1 |
|---|---|---|
| 0 | TEMP | OUTPUT |
| 1 | PRIMATTR | SECATTR |

**src1 / src2** (2-bit `srcN_bank` + 1-bit `srcN_bank_ext`):

| bank | ext = 0 | ext = 1 |
|---|---|---|
| 0 | TEMP | INDEXED1 |
| 1 | OUTPUT | SPECIAL |
| 2 | PRIMATTR | IMMEDIATE |
| 3 | SECATTR | INDEXED2 |

Where an instruction has no ext bit for an operand, ext = 0.

### 2.3 Register-number decoding

After choosing the bank, the number is post-processed in this order.

**Step 1: doubling ("64-bit register units").** For instructions that operate on 64-bit units
(the F32/F16 vector ALU: VMAD, VMAD2, VNMAD, VDP, VCOMP, VDUAL vector operands, VMOV with
F32/F16/C10 data, VPCK float sources and src2, SMP src0/src1), the field gives a register **pair**:

```
if bank not in {SPECIAL, IMMEDIATE}: num = (num << 1) & 0xFF
```

So field value *n* addresses 32-bit word 2n, and `r5` in a 6-bit F32 vector field means words
r10, r11, …. Instructions that do not double (integer ops, SOP, LDST, LIMM, SMP dest, VPCK dest)
address 32-bit words directly.

**Step 2: high temporaries alias FPINTERNAL.** If the bank is TEMP and the *encoded* field value
is in the top four of its range, it is an internal register:

```
F = width of the number field in bits (6 or 7)
if bank == TEMP and field >= 2^F - 4:
    bank = FPINTERNAL;  num = field - (2^F - 4)        // i0..i3
```

(Vita3K computes this after doubling, as `num >= 2^(F+d) - 4·2^d` with d = 1 for doubled
operands, followed by `>> d`. The result is the same.) Examples: a 6-bit doubled field 60–63 →
i0–i3. A 7-bit field 124–127 → i0–i3.

**Step 3: SPECIAL splits into GLOBAL / FPCONSTANT.** If the bank is SPECIAL: when bit 6 (0x40) of
the number is set, the bank is GLOBAL and the number is `num & 0x3F`. Otherwise the bank is
FPCONSTANT and the number is the constant index (§2.7).

**Step 4: secondary-program remap** (§1.10): inside a secondary program, every bank other than
FPINTERNAL, FPCONSTANT and IMMEDIATE becomes SECATTR with the same number. (Vita3K model.)

### 2.4 INDEX and INDEXED addressing

- **INDEX bank** (`idx`): two index registers. Vita3K maps operand number *n* to index register
  n−1, so `idx1` is number 1 and `idx2` is number 2. Loads of sub-32-bit types from INDEX (and
  PREDICATE) are masked to the type width.
- **INDEXED1 / INDEXED2** operand: the (7-bit) number is split as

  | Bits of number | Meaning |
  |---|---|
  | 6:5 | base bank: 0 TEMP, 1 OUTPUT, 2 PRIMATTR, 3 SECATTR |
  | 4:0 | offset *k* |

  Effective word = `idxN * 2 + k (+ repeat offset)` in that base bank, where N = 1 for INDEXED1
  and 2 for INDEXED2. **Uncertain**: Vita3K flags this split ("usually 7 bits") as an assumption.
  The ×2 matches the 64-bit register unit.

### 2.5 Data types and lane packing

Each operand carries a data type from its instruction. The type determines how components map
onto 32-bit words. For an operand starting at word *W*, component *c* (after swizzle) is at:

| Type | Lanes per word | Component *c* lives at |
|---|---|---|
| F32, U32, S32 | 1 | word W+c |
| F16, U16, S16 | 2 | word W + c/2, bits `16·(c%2) .. +15` (low half = even component) |
| U8, S8, O8 | 4 | word W + c/4, bits `8·(c%4) .. +7` |
| C10 | 3 | word W + c/3, bits `10·(c%3) .. +9` (bits 31:30 unused) |

Formats:

- **F16**: IEEE half, unpacked with the usual `unpackHalf2x16` semantics.
- **C10**: 10-bit **signed** two's-complement fixed point with range [−2, 2). As a float:
  `value = sext10(bits) / 255.0`, clamped to ≥ −2.0. Converting from float clamps to [−2, 2],
  computes `round(f × 255)` and keeps the low 10 bits. 1.0 is therefore 255 (0x0FF). Used for
  low-precision colors. **Uncertain**: Vita3K comments that it hopes the 0/10/20 bit layout is
  right.
- **O8**: an 8-bit format that only appears as a VPCK source/destination type. Vita3K has **no**
  implementation and no description. Its meaning ("offset" 8-bit?) is unknown. Treat as a gap and
  log it if seen.
- **Normalized conversions** (used by VPCK with `scale = 1` and by SOP/conversion paths). Integer
  → float divides by: U8 255, S8 127, C10 255, U16 65535, S16 32767, U32 2³²−1, S32 2³¹−1. Signed
  results are clamped to ≥ −1 (C10 to ≥ −2). Float → integer clamps to [0,1] for unsigned, [−1,1]
  for signed and [−2,2] for C10, multiplies by the same constant and rounds to nearest.
- **Raw conversions** (`scale = 0`): plain value conversion (`(float)int`, float → int
  truncation).

Swizzle channels can be literal constants: **0 → 0.0, 1 → 1.0, 2 → 2.0, H → 0.5** (for integer
types, the same numbers as integers, with 0.5 truncating to 0).

### 2.6 IMMEDIATE operands

When an src1/src2 bank decodes to IMMEDIATE, the operand's **register-number field is the value**
(unsigned, 6–7 bits, not doubled). It is broadcast to every component and typed as the operand's
type: integer types take the number itself, float types the number converted to float (so `#3` as
F32 is 3.0). VBW builds a 16-bit immediate from extra fields (§3.10). LDST scales immediate offsets
by the element size (§3.22).

### 2.7 FPCONSTANT: the constant ROM

Constant register *k* (0–63) is a read-only **64-bit** value made of two 32-bit words. Reading it as:
- **F32 pair** (64-bit unit): component x = low word, y = high word. (Vita3K returns the low word
  again for z/w.)
- **F16 quad**: components x, y, z, w = the four 16-bit halves, low half of the low word first.
  Vita3K's separate F16 table agrees with this reinterpretation except at k=12 x (table: 0.5)
  and k=16 x (table: 1.0), where the halves give 0.0. Treat these two as uncertain.
- **Integer**: the raw bits.

Modifiers (negate/abs) apply after reading. Values (from the community-documented table):

| k | low word (hex) | as F32 | high word (hex) | as F32 |
|---|---|---|---|---|
| 0 | `00000000` | 0 | `00000000` | 0 |
| 1 | `00000000` | 0 | `3F800000` | 1 |
| 2 | `3F800000` | 1 | `00000000` | 0 |
| 3 | `3F800000` | 1 | `3F800000` | 1 |
| 4 | `40000000` | 2 | `40800000` | 4 |
| 5 | `41000000` | 8 | `41800000` | 16 |
| 6 | `42000000` | 32 | `42800000` | 64 |
| 7 | `43000000` | 128 | `43800000` | 256 |
| 8 | `44000000` | 512 | `44800000` | 1024 |
| 9 | `45000000` | 2048 | `45800000` | 4096 |
| 10 | `46000000` | 8192 | `46800000` | 16384 |
| 11 | `47000000` | 32768 | `47800000` | 65536 |
| 12 | `3F000000` | 0.5 | `3E800000` | 0.25 |
| 13 | `3E000000` | 0.125 | `3D800000` | 0.0625 |
| 14 | `3D000000` | 0.03125 | `3C800000` | 0.015625 |
| 15 | `3C000000` | 0.0078125 | `3B800000` | 0.00390625 |
| 16 | `3B000000` | 0.001953125 | `3A800000` | 0.0009765625 |
| 17 | `3A000000` | 0.00048828125 | `39800000` | 0.000244140625 |
| 18 | `39000000` | 0.000122070312 | `38800000` | 6.10351562e-05 |
| 19 | `38000000` | 3.05175781e-05 | `37800000` | 1.52587891e-05 |
| 20 | `402DF854` | 2.71828175 | `3EBC5AB2` | 0.36787945 |
| 21 | `3FB504F3` | 1.41421354 | `3F3504F3` | 0.707106769 |
| 22 | `40490FDB` | 3.14159274 | `3FC90FDB` | 1.57079637 |
| 23 | `3F490FDB` | 0.785398185 | `3EC90FDB` | 0.392699093 |
| 24 | `40C90FDB` | 6.28318548 | `41490FDB` | 12.566371 |
| 25 | `41C90FDB` | 25.1327419 | `00000000` | 0 |
| 26 | `37800000` | 1.52587891e-05 | `38000000` | 3.05175781e-05 |
| 27 | `37800080` | 1.52590219e-05 | `38000100` | 3.05185094e-05 |
| 28 | `35D00D01` | 1.55009923e-06 | `37B60B61` | 2.17013894e-05 |
| 29 | `39888889` | 0.00026041668 | `3B2AAAAB` | 0.00260416674 |
| 30 | `3CAAAAAB` | 0.020833334 | `3E000000` | 0.125 |
| 31 | `3F000000` | 0.5 | `3F800000` | 1 |
| 32 | `00000000` | 0 | `3C000000` | 0.0078125 |
| 33 | `00000000` | 0 | `00000000` | 0 |
| 34 | `3C003C00` | 0.00782680511 | `3C003C00` | 0.00782680511 |
| 35 | `44004000` | 513 | `4C004800` | 33628160 |
| 36 | `54005000` | 2.20439196e+12 | `5C005800` | 1.44502216e+17 |
| 37 | `64006000` | 9.47240308e+21 | `6C006800` | 6.20934524e+26 |
| 38 | `74007000` | 4.07034685e+31 | `00007800` | 4.30478888e-41 |
| 39 | `34003800` | 1.19413016e-07 | `2C003000` | 1.82165394e-12 |
| 40 | `24002800` | 2.77894569e-17 | `1C002000` | 4.23930064e-22 |
| 41 | `14001800` | 6.4670817e-27 | `0C001000` | 9.86557614e-32 |
| 42 | `04000800` | 1.50500011e-36 | `00000000` | 0 |
| 43 | `35E2416F` | 1.68573558e-06 | `00000000` | 0 |
| 44 | `39A83DA8` | 0.000320893945 | `00000000` | 0 |
| 45 | `3E484248` | 0.195565343 | `36483A48` | 2.98362465e-06 |
| 46 | `4A484648` | 3281298 | `00004E48` | 2.80820212e-41 |
| 47 | `00000000` | 0 | `00000000` | 0 |
| 48 | `00000000` | 0 | `19550C44` | 1.10143189e-23 |
| 49 | `30002555` | 4.66191807e-10 | `3C003800` | 0.00782585144 |
| 50 | `00000000` | 0 | `00000000` | 0 |
| 51 | `00000000` | 0 | `00000000` | 0 |
| 52 | `00000000` | 0 | `00000000` | 0 |
| 53 | `00000000` | 0 | `00000000` | 0 |
| 54 | `00000000` | 0 | `00000000` | 0 |
| 55 | `00000000` | 0 | `00000000` | 0 |
| 56 | `FFFFFFFF` | NaN | `00000000` | 0 |
| 57 | `FFFFFFFF` | NaN | `00000000` | 0 |
| 58 | `FFFFFFFF` | NaN | `00000000` | 0 |
| 59 | `FFFFFFFF` | NaN | `00000000` | 0 |
| 60 | `7FFF7FFF` | NaN | `00000000` | 0 |
| 61 | `7FFF7FFF` | NaN | `00000000` | 0 |
| 62 | `7FFF7FFF` | NaN | `00000000` | 0 |
| 63 | `7FFF7FFF` | NaN | `00000000` | 0 |

Notable ones: k=2 → (1, 0), k=3 → (1, 1), k=12 → (0.5, 0.25), k=20 → (e, 1/e), k=21 → (√2, √½),
k=22 → (π, π/2), k=34 → F16 (1,1,1,1).

### 2.8 Swizzles

A swizzle maps each destination component to a source channel or constant. Encodings differ by
instruction:

**(a) Full 12-bit swizzle** (VNMAD src1): four 3-bit fields, component *i* in bits `[3i+2:3i]`,
values 0 x, 1 y, 2 z, 3 w, 4 '0', 5 '1', 6 '2', 7 'H'.

**(b) 4-bit table swizzle `decode_vec34(code, ext, kind)`**, used by VMAD/VDP/VNMAD src2/VMOV:

vec4 (`kind = vec4`), ext = 0:

| code | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| | xxxx | yyyy | zzzz | wwww | xyzw | yzww | xyzz | xxyz | xyxy | xywz | zxyw | zwzw | yzxz | xxyy | xzww | xyz1 |

vec4, ext = 1:

| code | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| | yzxw | zwxy | xzwy | yyww | wyzw | wzwz | xyzx | zzww | xwzx | yyyx | yyyz | xzyw | xxxy | zyxw | yyzz | zzzy |

vec3 (`kind = vec3`, w is forced to x), ext = 0:

| code | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| | xxx | yyy | zzz | www | xyz | yzw | xxy | xyx | yyx | yyz | zxy | xzy | yzx | zyx | zzy | xy1 |

vec3, ext = 1:

| code | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11–15 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| | xyy | yxy | xxz | yxx | xy0 | x10 | 000 | 111 | HHH | 222 | x00 | undefined |

scalar (`kind = scalar`, ext must be 0):

| code | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8–15 |
|---|---|---|---|---|---|---|---|---|---|
| | xxxx | yyyy | zzzz | wwww | 0000 | 1111 | 2222 | HHHH | undefined |

**(c) VDUAL swizzle** `decode_dual(code, ext, vec4)` (a different table):

vec4, ext = 0: `xxxx yyyy zzzz wwww xyzw yzww xyzz xxyz xyxy xywz zxyw zwzw 0000 HHHH 1111 2222`
(codes 0–15)

vec4, ext = 1: `yzxw zwxy xzwy yyww wyzw wzwz xyzx zzww xwzx yyyx yyyz zwzw yzxz xxyy xzww xyz1`

vec3, ext = 0: `xxx yyy zzz www xyz yzw xxy xyx yyx yyz zxy xzy 000 HHH 111 222`

vec3, ext = 1: `xyy yxy xxz yxx xy0 x10 xzy yzx zyx zzy xy1` (codes 0–10; 11–15 undefined)

**(d) VMAD2 swizzles**: 3-bit codes with per-source tables (§3.1).

Swizzle selection and the write mask interact: an instruction computes only the components whose
mask bit is set, reading source component *i* through swizzle entry *i*.

### 2.9 Write masks

A 4-bit write mask selects which destination components are written (bit 0 = x … bit 3 = w).
Unselected components keep their old contents. With packed types this means only some bytes or
halves of a word change.

For **VMAD2** and **F16/F32 VMOV** the encoded mask is rewritten according to the destination bank
(`decode_write_mask`):

- Destination FPINTERNAL: mask used as is.
- F32 to a 64-bit-unit destination in TEMP/PA/SA/O: only bits 0–1 are meaningful
  (`mask &= 0b0011`). A 64-bit unit holds two F32s.
- F16 to TEMP/PA/SA/O: encoded bit 0 → components x,y (`0b0011`), encoded bit 2 → components z,w
  (`0b1100`). Bits 1 and 3 are ignored. Here each mask bit enables one 32-bit word = two halves.
- F16 to other banks (INDEXED, SPECIAL, …): mask used as is.

**This means a single VMAD2 or VNMAD writes at most one 64-bit unit**: 2×F32 or 4×F16. Wider vectors
need repeats (§2.11) or several instructions. VNMAD itself uses its 4-bit mask as is (it does
not go through `decode_write_mask`). **Uncertain** whether hardware treats it the same.

### 2.10 Source modifiers

- 2-bit modifier code (`decode_modifier`): 0 none, 1 negate, 2 absolute, 3 negate(abs).
- Separate `neg` / `abs` bits where an instruction has them.
- Applied after reading and swizzling, before the operation: `abs` first, then `neg`. For integer
  types negate is two's complement.

### 2.11 Predicates

Four predicate registers p0–p3 hold one boolean each per instance. They are written by
VTST (§3.9). Instruction predicate fields come in four encodings:

| Value | ExtPredicate (3-bit) | ExtVecPredicate (3-bit, vector ops 0b00001–0b00011) | ShortPredicate (2-bit) | Short vector (VDUAL, 2-bit `sv_pred`) |
|---|---|---|---|---|
| 0 | always | always | always | always |
| 1 | p0 | p0 | p0 | p0 |
| 2 | p1 | p1 | p1 | !p0 |
| 3 | p2 | p2 | !p0 | pN |
| 4 | p3 | !p0 | — | — |
| 5 | !p0 | !p1 | — | — |
| 6 | !p1 | !p2 | — | — |
| 7 | pN | pN | — | — |

- A predicated instruction executes only if its predicate is true. Otherwise it has no effect at
  all, including no write and no repeat side effects.
- **pN** (per-instance / "predicate N"): meaning not established. Vita3K treats it as "always". It
  probably selects a per-repeat-iteration predicate (p0..p3 for iteration 0..3); flag it if seen.
- **VMAD2** uses a 2-bit predicate read as ExtPredicate 0–3: always, p0, p1, p2.
- **KILL** has its own inverted mapping (§3.18).
- Vita3K never handles ExtVecPredicate !p2 (asserts).

Where each group keeps its predicate field (useful for a fast pre-decode):

| Group (insn[63:59]) | Predicate field |
|---|---|
| 00000 VMAD2 | insn[57:56] (2-bit, as ExtPredicate 0–3) |
| 00001–00011 vector | insn[58:56] ExtVecPredicate |
| 00100–00101 VDUAL | insn[57:56] short vector predicate (`sv_pred`) |
| 10000–10101 SOP/integer | insn[58:57] ShortPredicate |
| 11111 KILL | insn[42:41] (inverted, §3.18) |
| 11111 LIMM | insn[43:41] ExtPredicate |
| 11111 DEPTHF | insn[42:41] ShortPredicate |
| 11111 BR | insn[58:56] ExtPredicate |
| others | insn[58:56] ExtPredicate |

### 2.12 Repeats

Many instructions repeat their operation 1–16 times (`repeat_count + 1`), stepping operand
register numbers between iterations. This is how one 64-bit instruction processes a whole vec4
of F32 (two iterations of 2×F32) or a matrix row. Each iteration *r* (0-based) executes the
instruction with every operand's register number increased by an **offset**. The offset depends on
the instruction's *repeat mode*:

| Mode | Used by | Offset for operand in bank B at iteration r |
|---|---|---|
| EXTERNAL | VMAD/VDP with `repeat_mode = 0` | FPINTERNAL: 0. Others: **4·r words**. |
| INTERNAL | `repeat_mode = 1` | FPINTERNAL: r (next internal register). Others: 0. |
| BOTH | `repeat_mode = 2` | FPINTERNAL: r. Others: 4·r words. |
| SLMSI | VCOMP, VMOV, VPCK, SOP2, LDST stores, integer ops; `repeat_mode = 3` on VMAD/VDP | `inc[slot][r]`, multiplied by `mult[slot]` if B ∈ {TEMP, PA, O, SA, PREDICATE, INDEXED1, INDEXED2} |

Rules common to all modes:
- An operand marked **GPI** (the fixed internal-register operands of VMAD/VDP, which are encoded
  as gpi*n* fields) gets an offset only in INTERNAL and BOTH modes, where it is r. In other modes
  it is 0 -- in SLMSI mode too, whatever SMLSI programmed for that slot (confirmed on Unit 13's
  sniper-scope lens vertex program: `SMLSI src2 -2` then `VDP3 rpt3 O18 = dot(SA50/46/42, i2)`;
  stepping the GPI by the table read i0 for the binormal's y and made the lens opaque).
- Operand slots: **0 = src0, 1 = src1, 2 = src2, 3 = dest.**
- `inc[slot][r]` defaults to `r` (identity). It is reprogrammed by **SMLSI** (§3.17) and **SMBO**
  (§3.17). The table is per-program state that persists until the next SMLSI/SMBO. Reset it to
  identity at program start.
- `mult[slot]` is set **per instruction** (not by SMLSI). It converts "elements" to 32-bit words:

| Instruction | mult (src0, src1, src2, dest) |
|---|---|
| default | 2, 2, 2, 2 |
| VMAD (GPI form) | 2, 2, 2, 4 |
| VMOV with F16/F32 data | 2, 2, 2, 2 |
| VMOV other types | 1, 1, 1, 1 |
| VPCK float↔int | 1, 2, 2, 1 |
| VPCK other | 1, 1, 1, 1 |
| LDST | 1, 1, 1, 1 |

- Offsets are added to the **decoded** register number (after doubling, so in 32-bit words). For
  FPINTERNAL the unit is one 128-bit internal register.
- **VDP rotates its write mask** left by one position (within 4 bits) after each iteration. A VDP
  with mask x repeated 4× writes x, y, z, w of the destination in turn, which is how a 4×4 matrix ×
  vector is one instruction.

**Uncertain**: Vita3K's offsets come from fitting real shaders, not from documentation. The
EXTERNAL "×4" stride in particular is in 32-bit words. Verify on hardware traces if a mismatch
appears.

### 2.13 Instruction-level control bits

Fields present in most groups. None changes the value an instruction computes:

| Field | Meaning | Interpreter action |
|---|---|---|
| `skipinv` | skip the instruction for invalid instances (pixels outside the primitive used only as derivative helpers) | ignore, or skip for helper lanes if you model quads |
| `nosched` | scheduler hint: do not switch threads after this instruction | ignore |
| `syncstart` / `sync_start` / `syncend` | synchronisation points for texture/memory results | ignore (results are immediate in an interpreter) |
| `end` | marks the last instruction of the program/phase | may be used as a stop marker. Vita3K stops at the instruction count instead. |
| `minpack`, `cache_*`, `drc_sel` | data-return / cache control | ignore |

---

## 3. Instruction encoding

This section takes the instruction groups one at a time: how each is recognised, its fields, and what it computes.

### 3.0 Decoding

Every instruction is one 64-bit word. Decode by testing the patterns below **in this order** and
taking the first match (`(insn & mask) == match`). Most groups are identified by `insn[63:59]`
("op1"). Overlaps are resolved by the order: for example, VBW's pattern covers op1 = 01xxx, but
01000, 01001 and 01111 are matched first by VPCK, VTST and VTSTMSK.

| # | insn[63:59] | Extra condition | Group | § |
|---|---|---|---|---|
| 1 | 00000 | | VMAD2: 3-source F32/F16 multiply-add | 3.1 |
| 2 | 00001 | | V32NMAD: F32 vector ALU (add/mul/min/max/frc/dsx/dsy/dp) | 3.2 |
| 3 | 00010 | | V16NMAD: F16 vector ALU | 3.2 |
| 4 | 00011 | bit 53 = 1 | VMAD: multiply-add with internal-register operands | 3.3 |
| 5 | 00011 | bit 53 = 0 | VDP: dot product | 3.4 |
| 6 | 00100, 00101 | | VDUAL: dual-issue pair | 3.5 |
| 7 | 00110 | | VCOMP: rcp/rsq/log/exp | 3.6 |
| 8 | 00111 | | VMOV / VMOVC / VMOVCU8 | 3.7 |
| 9 | 01000 | | VPCK: pack/unpack/convert | 3.8 |
| 10 | 01001 | | VTST: test → predicate | 3.9 |
| 11 | 01111 | | VTSTMSK: test → mask register | 3.9 |
| 12 | 01010–01110 | | VBW: bitwise and shifts | 3.10 |
| 13 | 10000 | | SOP2 | 3.11 |
| 14 | 10010 | | SOP2M (SOP2 with write mask) | 3.11 |
| 15 | 10001 | | SOP3 | 3.11 |
| 16 | 10011 | | I8MAD | 3.12 |
| 17 | 10100 | | I16MAD | 3.12 |
| 18 | 10101 | | I32MAD | 3.12 |
| 19–21 | 10110, 10111, 11000 | | illegal | 3.23 |
| 22 | 11001 | | I8MAD2 (unimplemented) | 3.12 |
| 23 | 11010 | | I32MAD2 | 3.12 |
| 24 | 11011 | | illegal | 3.23 |
| 25 | 11100 | | SMP: texture sample | 3.13 |
| 26 | 11111 | `insn[58:56] = 010`, `insn[54:52] = 100` | PHAS | 3.14 |
| 27 | 11111 | `insn[54:52] = 000`, `insn[40:38] = 101` | NOP | 3.15 |
| 28 | 11111 | `insn[54:52] = 000`, `insn[40:39] = 00` | BR | 3.16 |
| 29 | 11111 | `insn[58:56] = 010`, `insn[53:52] = 01` | SMLSI | 3.17 |
| 30 | 11111 | `insn[58:56] = 011`, `insn[53:52] = 01` | SMBO | 3.17 |
| 31 | 11111 | `insn[58:56] = 001`, `insn[53:52] = 11`, fixed bits | KILL | 3.18 |
| 32 | 11111 | `insn[58:56] = 100`, `insn[53:52] = 10` | LIMM | 3.19 |
| 33 | 11111 | `insn[58:56] = 011`, `insn[53:52] = 11` | DEPTHF | 3.20 |
| 34 | 11111 | anything else | SPEC (unimplemented catch-all) | 3.21 |
| 35 | 11101, 11110 | (pattern 111xx; 11100/11111 are taken earlier) | LDST: memory load/store | 3.22 |

Group 11111 ("special/control") is subdivided by **op2 = insn[58:56]**, **opcat = insn[53:52]**
and, for opcat 0, **opcat_extra = insn[54]**:

| opcat | op2 | opcat_extra | Instruction |
|---|---|---|---|
| 00 | (predicate) | 0 | flow control: insn[40:38] = 000/001 BR, 010 return (see §3.16), 101 NOP |
| 00 | 010 | 1 | PHAS |
| 01 | 010 | – | SMLSI |
| 01 | 011 | – | SMBO |
| 10 | 100 | – | LIMM |
| 11 | 001 | – | KILL |
| 11 | 011 | – | DEPTHF |

In the field tables, "(not decoded)" bits are ignored by Vita3K. Field names follow Vita3K's
naming for easy cross-reference.

---

### 3.1 VMAD2: three-source multiply-add (compact form)

Pattern (bit 63 first): `00000dpps-ry-cbawwwineeeemmookttffgghhhhhhzzjjllllllqqqqqquuuuuu`  
Match: `(insn & 0xF800000000000000) == 0x0000000000000000`

| Bits | Width | Field |
|---|---|---|
| 63:59 | 5 | fixed `00000` |
| 58 | 1 | dat_fmt |
| 57:56 | 2 | pred |
| 55 | 1 | skipinv |
| 54 | 1 | (not decoded) |
| 53 | 1 | src0_swiz_bits2 |
| 52 | 1 | syncstart |
| 51 | 1 | (not decoded) |
| 50 | 1 | src0_abs |
| 49 | 1 | src1_bank_ext |
| 48 | 1 | src2_bank_ext |
| 47:45 | 3 | src2_swiz |
| 44 | 1 | src1_swiz_bit2 |
| 43 | 1 | nosched |
| 42:39 | 4 | dest_mask |
| 38:37 | 2 | src1_mod |
| 36:35 | 2 | src2_mod |
| 34 | 1 | src0_bank |
| 33:32 | 2 | dest_bank |
| 31:30 | 2 | src1_bank |
| 29:28 | 2 | src2_bank |
| 27:22 | 6 | dest_n |
| 21:20 | 2 | src1_swiz_bits01 |
| 19:18 | 2 | src0_swiz_bits01 |
| 17:12 | 6 | src0_n |
| 11:6 | 6 | src1_n |
| 5:0 | 6 | src2_n |

**Operation:** `dest = src0 * src1 + src2` per component, as a fused multiply-add.

| Item | Rule |
|---|---|
| Type | `dat_fmt` 0 = F32, 1 = F16 (all operands) |
| Registers | all four operands **doubled** (64-bit units). 6-bit fields. dest uses the dest table with ext = 0, src0 the src0 table with ext = 0, src1/src2 the src1/2 table with their ext bits. |
| Write mask | `dest_mask` rewritten by bank (§2.9): F32 → x,y only; F16 → bit0→xy, bit2→zw |
| Predicate | 2-bit `pred`: 0 always, 1 p0, 2 p1, 3 p2 |
| Modifiers | src0: abs if `src0_abs`. src1/src2: 2-bit modifier code (§2.10). |
| Repeat | none |

Swizzles: 3-bit codes per source. src0 code = `src0_swiz_bits2 << 2 | src0_swiz_bits01`. src1 code =
`src1_swiz_bit2 << 2 | src1_swiz_bits01`. src2 code = `src2_swiz`.

| code | src0 | src1 | src2 |
|---|---|---|---|
| 0 | xxxx | xxxx | xxxx |
| 1 | yyyy | yyyy | yyyy |
| 2 | zzzz | zzzz | zzzz |
| 3 | wwww | wwww | wwww |
| 4 | xyzw | xyzw | xyzw |
| 5 | yzxw | xyyz | xzww |
| 6 | xyww | yyww | xxyz |
| 7 | zwxy | wyzw | xyzz |

---

### 3.2 V32NMAD / V16NMAD: vector ALU ("NMAD" = non-MAD vector ops)

Pattern (bit 63 first): `00001pppsrrydcbawwwwneeeemmoiittkkllffffffzzzzzzzggghhhhhhjjjjjj`  
Match: `(insn & 0xF800000000000000) == 0x0800000000000000`

| Bits | Width | Field |
|---|---|---|
| 63:59 | 5 | fixed `00001` |
| 58:56 | 3 | pred |
| 55 | 1 | skipinv |
| 54:53 | 2 | src1_swiz_10_11 |
| 52 | 1 | syncstart |
| 51 | 1 | dest_bank_ext |
| 50 | 1 | src1_swiz_9 |
| 49 | 1 | src1_bank_ext |
| 48 | 1 | src2_bank_ext |
| 47:44 | 4 | src2_swiz |
| 43 | 1 | nosched |
| 42:39 | 4 | dest_mask |
| 38:37 | 2 | src1_mod |
| 36 | 1 | src2_mod |
| 35:34 | 2 | src1_swiz_7_8 |
| 33:32 | 2 | dest_bank_sel |
| 31:30 | 2 | src1_bank_sel |
| 29:28 | 2 | src2_bank_sel |
| 27:22 | 6 | dest_n |
| 21:15 | 7 | src1_swiz_0_6 |
| 14:12 | 3 | op2 |
| 11:6 | 6 | src1_n |
| 5:0 | 6 | src2_n |

V16NMAD has the identical layout with op1 = 00010 (bits 63:59 = `00010`):

Pattern (bit 63 first): `00010pppsrrydcbawwwwneeeemmoiittkkllffffffzzzzzzzggghhhhhhjjjjjj`  
Match: `(insn & 0xF800000000000000) == 0x1000000000000000`

| Bits | Width | Field |
|---|---|---|
| 63:59 | 5 | fixed `00010` |
| 58:56 | 3 | pred |
| 55 | 1 | skipinv |
| 54:53 | 2 | src1_swiz_10_11 |
| 52 | 1 | syncstart |
| 51 | 1 | dest_bank_ext |
| 50 | 1 | src1_swiz_9 |
| 49 | 1 | src1_bank_ext |
| 48 | 1 | src2_bank_ext |
| 47:44 | 4 | src2_swiz |
| 43 | 1 | nosched |
| 42:39 | 4 | dest_mask |
| 38:37 | 2 | src1_mod |
| 36 | 1 | src2_mod |
| 35:34 | 2 | src1_swiz_7_8 |
| 33:32 | 2 | dest_bank_sel |
| 31:30 | 2 | src1_bank_sel |
| 29:28 | 2 | src2_bank_sel |
| 27:22 | 6 | dest_n |
| 21:15 | 7 | src1_swiz_0_6 |
| 14:12 | 3 | op2 |
| 11:6 | 6 | src1_n |
| 5:0 | 6 | src2_n |

**Type:** op1 = 00001 → F32, op1 = 00010 → F16 (Vita3K tests bit 59). All operands are doubled
(6-bit fields, 64-bit units). Neither has a src0.

**op2 (3 bits) selects the operation:**

| op2 | F32 | F16 | Result per component *i* (enabled by `dest_mask`) |
|---|---|---|---|
| 0 | VMUL | VF16MUL | `src1[i] * src2[i]` |
| 1 | VADD | VF16ADD | `src1[i] + src2[i]` |
| 2 | VFRC | VF16FRC | `src1[i] − floor(src2[i])`. With src1 = src2 this is `fract`. |
| 3 | VDSX | VF16DSX | screen-space x derivative of src1 (fragment only, needs a 2×2 quad) |
| 4 | VDSY | VF16DSY | y derivative of src1 |
| 5 | VMIN | VF16MIN | `min(src1[i], src2[i])` |
| 6 | VMAX | VF16MAX | `max(src1[i], src2[i])` |
| 7 | VDP | VF16DP | dot product over **all 4** components (sources read with mask 1111), replicated into every enabled dest component |

- **src1 swizzle** is a full 12-bit swizzle (§2.8a), assembled as
  `src1_swiz_0_6 | src1_swiz_7_8 << 7 | src1_swiz_9 << 9 | src1_swiz_10_11 << 10`.
- **src2 swizzle**: 4-bit table code `src2_swiz`, vec4, not extended (§2.8b).
- **Modifiers**: src1 has a 2-bit modifier code (`src1_mod`). src2's 1-bit `src2_mod` = abs.
- **Write mask**: `dest_mask` used directly (not bank-rewritten). Vita3K warns when VDP has more
  than one enabled component; the result is replicated.
- No repeat field. VDSX/VDSY derivatives require executing fragments in 2×2 quads (§5).

---

### 3.3 VMAD: multiply-add with internal-register (GPI) operands

Pattern (bit 63 first): `00011pppsg1oderaaittnwwwwcbfhzkkjjllmmmmmmqqqquuuuvvxyAAAABBBBBB`  
Match: `(insn & 0xF820000000000000) == 0x1820000000000000`

| Bits | Width | Field |
|---|---|---|
| 63:59 | 5 | fixed `00011` |
| 58:56 | 3 | pred |
| 55 | 1 | skipinv |
| 54 | 1 | gpi1_swiz_ext |
| 53 | 1 | fixed `1` |
| 52 | 1 | opcode2 |
| 51 | 1 | dest_use_bank_ext |
| 50 | 1 | end |
| 49 | 1 | src1_bank_ext |
| 48:47 | 2 | repeat_mode |
| 46 | 1 | gpi0_abs |
| 45:44 | 2 | repeat_count |
| 43 | 1 | nosched |
| 42:39 | 4 | write_mask |
| 38 | 1 | src1_neg |
| 37 | 1 | src1_abs |
| 36 | 1 | gpi1_neg |
| 35 | 1 | gpi1_abs |
| 34 | 1 | gpi0_swiz_ext |
| 33:32 | 2 | dest_bank |
| 31:30 | 2 | src1_bank |
| 29:28 | 2 | gpi0_n |
| 27:22 | 6 | dest_n |
| 21:18 | 4 | gpi0_swiz |
| 17:14 | 4 | gpi1_swiz |
| 13:12 | 2 | gpi1_n |
| 11 | 1 | gpi0_neg |
| 10 | 1 | src1_swiz_ext |
| 9:6 | 4 | src1_swiz |
| 5:0 | 6 | src1_n |

**Operation:** `dest = gpi0 * src1 + gpi1` (fused multiply-add), F32.

| Operand | Encoding |
|---|---|
| src0 = **gpi0** | FPINTERNAL register `gpi0_n` (0–3), swizzle `decode_vec34(gpi0_swiz, gpi0_swiz_ext, kind)`, abs `gpi0_abs`, neg `gpi0_neg` |
| src1 | src1 table with `src1_bank_ext`. Doubled, 6-bit. Swizzle `decode_vec34(src1_swiz, src1_swiz_ext, kind)`. Abs `src1_abs`, neg `src1_neg`. |
| src2 = **gpi1** | FPINTERNAL `gpi1_n`, swizzle `decode_vec34(gpi1_swiz, gpi1_swiz_ext, kind)`, abs `gpi1_abs`, neg `gpi1_neg` |
| dest | dest table with `dest_use_bank_ext`. Doubled, 6-bit. |

- `opcode2` = 1 → VMAD4 (kind = vec4). `opcode2` = 0 → VMAD3 (kind = vec3; destination w treated
  as x).
- `write_mask` (4 bits) selects destination components. It is not bank-rewritten.
- **Repeat:** `repeat_count + 1` iterations (1–4) in `repeat_mode` (§2.12), with
  mult = (2, 2, 2, 4). gpi0/gpi1 are GPI-flagged, so they step only in INTERNAL/BOTH modes.

---

### 3.4 VDP: dot product (single issue)

Pattern (bit 63 first): `00011pppsc0oderaagttnwwwwbflllkkhhiijjjjjjzzzzmmmqqqyyyxxxuuuuuu`  
Match: `(insn & 0xF820000000000000) == 0x1800000000000000`

| Bits | Width | Field |
|---|---|---|
| 63:59 | 5 | fixed `00011` |
| 58:56 | 3 | pred |
| 55 | 1 | skipinv |
| 54 | 1 | clip_plane_enable |
| 53 | 1 | fixed `0` |
| 52 | 1 | opcode2 |
| 51 | 1 | dest_use_bank_ext |
| 50 | 1 | end |
| 49 | 1 | src1_bank_ext |
| 48:47 | 2 | repeat_mode |
| 46 | 1 | gpi0_abs |
| 45:44 | 2 | repeat_count |
| 43 | 1 | nosched |
| 42:39 | 4 | write_mask |
| 38 | 1 | src1_neg |
| 37 | 1 | src1_abs |
| 36:34 | 3 | clip_plane_n |
| 33:32 | 2 | dest_bank |
| 31:30 | 2 | src1_bank |
| 29:28 | 2 | gpi0_n |
| 27:22 | 6 | dest_n |
| 21:18 | 4 | gpi0_swiz |
| 17:15 | 3 | src1_swiz_w |
| 14:12 | 3 | src1_swiz_z |
| 11:9 | 3 | src1_swiz_y |
| 8:6 | 3 | src1_swiz_x |
| 5:0 | 6 | src1_n |

**Operation:** `d = dot(src1.xyz[w], gpi0.xyz[w])`. VDP4 (`opcode2` = 1) uses 4 components,
VDP3 (`opcode2` = 0) uses 3. `d` is written to every component enabled in `write_mask`.

| Operand | Encoding |
|---|---|
| src1 | src1 table + `src1_bank_ext`. Doubled, 6-bit. **Per-component 3-bit swizzle** `src1_swiz_x/y/z/w`: 0 x, 1 y, 2 z, 3 w, 4 '0', 5 '1', 6 '2', 7 'H'. Abs/neg bits. |
| src2 = gpi0 | FPINTERNAL `gpi0_n`. Swizzle `decode_vec34(gpi0_swiz, ext = 0, kind)`. Abs bit `gpi0_abs`. |
| dest | dest table + `dest_use_bank_ext`. Doubled. |

- **Repeat:** `repeat_count + 1` iterations in `repeat_mode`. After each iteration the write mask
  **rotates left by 1** (`m = ((m << 1) | (m >> 3)) & 0xF`). So `VDP4 rpt4 mask=x` computes four
  dot products into x, y, z, w (a matrix × vector). Each iteration's src1 steps by the repeat
  offset (the next matrix row). Vita3K applies the per-iteration offset to the sources but writes
  the destination without a repeat offset, relying on the mask rotation.
- `clip_plane_enable` / `clip_plane_n`: when set, the result also feeds user clip plane *n*.
  **Unimplemented in Vita3K.** An interpreter can ignore it unless it implements user clipping.

---

### 3.5 VDUAL: dual-issue

Pattern (bit 63 first): `0010cgsskdtpuuuunaaalriiiiwwwwmmffeebbbbbbbooohhjqvvxxyyyzzzzzzz`  
Match: `(insn & 0xF000000000000000) == 0x2000000000000000`

| Bits | Width | Field |
|---|---|---|
| 63:60 | 4 | fixed `0010` |
| 59 | 1 | comp_count_type |
| 58 | 1 | gpi1_neg |
| 57:56 | 2 | sv_pred |
| 55 | 1 | skipinv |
| 54 | 1 | dual_op1_ext_vec3_or_has_w_vec4 |
| 53 | 1 | type_f16 |
| 52 | 1 | gpi1_swizz_ext |
| 51:48 | 4 | unified_store_swizz |
| 47 | 1 | unified_store_neg |
| 46:44 | 3 | dual_op1 |
| 43 | 1 | dual_op2_ext |
| 42 | 1 | prim_ustore |
| 41:38 | 4 | gpi0_swizz |
| 37:34 | 4 | gpi1_swizz |
| 33:32 | 2 | prim_dest_bank |
| 31:30 | 2 | unified_store_slot_bank |
| 29:28 | 2 | prim_dest_num_gpi_case |
| 27:21 | 7 | prim_dest_num |
| 20:18 | 3 | dual_op2 |
| 17:16 | 2 | src_config |
| 15 | 1 | gpi2_slot_num_bit_1 |
| 14 | 1 | gpi2_slot_num_bit_0_or_unified_store_abs |
| 13:12 | 2 | gpi1_slot_num |
| 11:10 | 2 | gpi0_slot_num |
| 9:7 | 3 | write_mask_non_gpi |
| 6:0 | 7 | unified_store_slot_num |

Two independent F32/F16 operations ("op1" primary, "op2" secondary) issued together. Most of their
operands are internal registers (GPI). One operand slot, the **unified store** (US), can reference
any bank.

**Types and sizes:** `type_f16` → F16 else F32. `comp_count_type` = 1 → vec4 operations,
0 → vec3.

**Opcode selection:**
- op1 = `table1[(!comp_count_type && dual_op1_ext_vec3_or_has_w_vec4) << 3 | dual_op1]`
- op2 = `table2[dual_op2_ext << 3 | dual_op2]`

| idx | table1 (op1) | table2 (op2) | sources | vector? |
|---|---|---|---|---|
| 0 | VMAD | (invalid) | 3 | vector |
| 1 | VDP | VDP | 2 | vector load, scalar result |
| 2 | VSSQ (dot(a,a)) | VSSQ | 1 | vector load, scalar result |
| 3 | VMUL | VMUL | 2 | vector |
| 4 | VADD | VADD | 2 | vector |
| 5 | VMOV | VMOV | 1 | vector |
| 6 | FRSQ | FRSQ | 1 | scalar |
| 7 | FRCP | FRCP | 1 | scalar |
| 8 | FMAD | FMAD | 3 | scalar |
| 9 | FADD | FADD | 2 | scalar |
| 10 | FMUL | FMUL | 2 | scalar |
| 11 | FSUBFLR (a − floor(b)) | FSUBFLR | 2 | scalar |
| 12 | FEXP | FEXP | 1 | scalar |
| 13 | FLOG | FLOG | 1 | scalar |
| 14–15 | invalid | invalid | | |

(VMAD/FMAD here are `a*b + c`. FEXP/FLOG are natural exp/log in Vita3K, see §3.6. FRSQ is 1/√x.)

**Destinations:**
- `prim_ustore` = 1 → op1 writes the **unified destination** and op2 writes the internal
  destination. 0 → the reverse.
- Unified destination: bank from the dest table (`prim_dest_bank`, ext = 0), number
  `prim_dest_num` (7 bits). Doubled (8-bit register space) only if the op writing it is a
  **vector-store** op (VMAD, VMUL, VADD, VMOV); a scalar op's -- and VDP/VSSQ's, which load
  vectors but store one scalar -- counts 32-bit words. Confirmed on Killzone: Mercenary
  (PCSA00107): vertex program 0x10c409d0's op2 FEXP has `prim_dest_num` 6 in O and the program's
  TEXCOORD1 is O6..O9 with nothing else writing O6 (doubled, O12 is no output at all); fragment
  0x112ef460's op2 FMUL writes r6 (7 temps; doubled, r12), which a later VMAD2 reads as `r6.xxxx`.
- Internal destination: FPINTERNAL register `prim_dest_num_gpi_case`. Like every internal
  register operand it holds one F32 per component whatever `type_f16` says (§2.1, §5 gotcha 4): an
  F16 VDUAL's internal sources read the F32 words and its internal result is stored as the half
  widened to F32. Killzone: Mercenary's F16 VDUALs read i-registers that F32 instructions wrote
  (fragment 0x11305670: i1.x from an F32 VNMAD; its i2.x result is read by an F32 VPCK).

**Sources:** a layout table picks, for each source of op1 and op2, one of: US (unified store),
I0, I1, I2 (internal registers). The layout depends on the source counts of both ops and on
`src_config`:

op1 layout (by op1 source count, `src_config`):

| count | cfg 0 | cfg 1 | cfg 2 | cfg 3 |
|---|---|---|---|---|
| 1 | US | I0 | I1 | I2 |
| 2 | US, I1 | I0, US | I0, I1 | – |
| 3 | US, I1, I2 | I0, US, I2 | I0, I1, US | I0, I1, I2 |

op2 layout (by op1 count, op2 count, `src_config`):

| op1 n | op2 n | cfg 0 | cfg 1 | cfg 2 | cfg 3 |
|---|---|---|---|---|---|
| 1 | 1 | I0 | US | US | US |
| 1 | 2 | I0, I1 | US, I1 | I0, US | I1, US |
| 1 | 3 | I0, I1, I2 | US, I1, I2 | I0, US, I2 | I0, US, I1 |
| 2 | 1 | I0 | I1 | US | – |
| 2 | 2 | I0, I2 | I1, I2 | I2, US | – |
| 3 | 1 | I0 | I1 | I2 | US |

Source operand details:

| Source | Register | Swizzle | Modifiers |
|---|---|---|---|
| US | src1/2 table, bank `unified_store_slot_bank` with ext **0** (bit 14 is the abs flag or I2's low bit, never a bank extension), number `unified_store_slot_num`, doubled only if the op reading it is a **vector-load** op (VMAD, VDP, VSSQ, VMUL, VADD, VMOV) | `decode_dual(unified_store_swizz, ext, comp_count_type)`. ext = `gpi2_slot_num_bit_1` when op1 has fewer than 2 sources. | neg `unified_store_neg`. abs `gpi2_slot_num_bit_0_or_unified_store_abs` when op1 has fewer than 2 sources. |
| I0 | i[`gpi0_slot_num`] | `decode_dual(gpi0_swizz, 0, comp_count_type)` | – |
| I1 | i[`gpi1_slot_num`] | `decode_dual(gpi1_swizz, gpi1_swizz_ext, comp_count_type)` | neg `gpi1_neg` |
| I2 | i[`gpi2_slot_num_bit_1 << 1 \| gpi2_slot_num_bit_0_or_unified_store_abs`] if op1 has ≥ 2 sources, else i2 | xyzw | – |

Evidence for the two US rules, also Killzone: Mercenary: vertex 0x112ef2c0's
`VMAD i1 = US.xyzw * i0.xxxx + i1` is its MVP's first row times x (`unified_store_slot_bank` 3 =
SA, number 0, bit 14 set because I2 = i1); with bit 14 taken as the bank extension US became
INDEXED2 -- temps the program doesn't have -- and every vertex lost its x term. Fragment
0x11312920's `FADD PA0.x = i0.x + US.y` has US number 9: SA9 is the last word of its F16
`mRgbGammaContrastSatPremul`, doubled it would be SA18 and the program has 18 SA words (0–17).

Sources the layout tables leave out (op1 with 2 sources and op2 with 3, op1 with 3 and op2 with
2 or 3, an invalid op1 with a valid op2): Vita3K refuses the instruction; usse.c reads them as
0.0. None seen in a game.

**Write masks** (Vita3K's model, explicitly marked "please double check"):
- The op writing the unified destination uses
  `write_mask_non_gpi | (comp_count_type && dual_op1_ext_vec3_or_has_w_vec4) << 3`.
- The op writing the internal destination uses `comp_count_type ? 1111 : 0111` (Vita3K's
  function also has F16/F32 masks for a non-internal destination, but the destination it is
  called for is always FPINTERNAL).
- A scalar op (FRSQ … FLOG) writes component x only (usse.c masks with 0001); **uncertain**:
  Vita3K stores the scalar to every enabled component. VDP/VSSQ write their scalar to every enabled
  component in both.

Scalar ops read only component x. VDP/VSSQ read 4 components (vec4) or 3 (vec3). FEXP/FLOG are
taken as base 2 here, like VCOMP's EXP/LOG (§3.6).

**Both operations read their sources before either writes** (results are committed together).

Predicate: `sv_pred` (short vector predicate, §2.11); usse.c and the SPIR-V translator apply it
like any other instruction predicate. **Uncertain**: Vita3K does not apply it in the translator
body.

**Implementation note.** VDUAL has more operand roles than a decoded micro-op has slots, so
usse.c keeps the raw word and decodes it with `usse_vdual_decode` (usse_internal.h), which both the
interpreter's `exec_vdual` and the translator's `emit_vdual` call.

---

### 3.6 VCOMP: complex scalar functions

Pattern (bit 63 first): `00110pppsddyenr-aaaaobbccmmff-ttkk--ggggggg-------hhhhhhh---wwww`  
Match: `(insn & 0xF800000000000000) == 0x3000000000000000`

| Bits | Width | Field |
|---|---|---|
| 63:59 | 5 | fixed `00110` |
| 58:56 | 3 | pred |
| 55 | 1 | skipinv |
| 54:53 | 2 | dest_type |
| 52 | 1 | syncstart |
| 51 | 1 | dest_bank_ext |
| 50 | 1 | end |
| 49 | 1 | src1_bank_ext |
| 48 | 1 | (not decoded) |
| 47:44 | 4 | repeat_count |
| 43 | 1 | nosched |
| 42:41 | 2 | op2 |
| 40:39 | 2 | src_type |
| 38:37 | 2 | src1_mod |
| 36:35 | 2 | src_comp |
| 34 | 1 | (not decoded) |
| 33:32 | 2 | dest_bank |
| 31:30 | 2 | src1_bank |
| 29:28 | 2 | (not decoded) |
| 27:21 | 7 | dest_n |
| 20:14 | 7 | (not decoded) |
| 13:7 | 7 | src1_n |
| 6:4 | 3 | (not decoded) |
| 3:0 | 4 | write_mask |

**Operation:** applies one function to **one** source component and broadcasts the result.

| op2 | Function |
|---|---|
| 0 | VRCP: `1/x` |
| 1 | VRSQ: `1/sqrt(x)` |
| 2 | VLOG: logarithm (Vita3K uses natural log; the hardware is probably **log2**, **uncertain**) |
| 3 | VEXP: exponential (Vita3K uses natural exp with `exp(NaN) := 1.0`; the hardware is probably **exp2**, **uncertain**) |

> **Note on LOG/EXP base.** USSE compilers express `exp2`/`log2` directly. Vita3K's use of e is a
> long-standing approximation that happens to work because most shaders use the ops in pairs. For
> an interpreter, prefer base 2 and verify against a hardware trace. The NaN→1 rule is a Vita3K
> workaround for Freedom Wars; keep it behind a flag.

| Field | Rule |
|---|---|
| `src_type` / `dest_type` | 0 F32, 1 F16, 2 C10, 3 invalid |
| `src_comp` | which source component is read: 0 x, 1 y, 2 z, 3 w |
| `src1_mod` | 2-bit modifier code |
| Registers | dest and src1 doubled, 7-bit fields (8-bit register space) |
| `write_mask` | components receiving the (broadcast) result |
| Repeat | `repeat_count + 1` iterations (4-bit field, up to 16), SLMSI mode |

---

### 3.7 VMOV, VMOVC, VMOVCU8: moves and conditional moves

Pattern (bit 63 first): `00111pppstrydecbmmaanoooiwwwwkllffgghhhhjjjjjjqqqqqquuuuuuvvvvvv`  
Match: `(insn & 0xF800000000000000) == 0x3800000000000000`

| Bits | Width | Field |
|---|---|---|
| 63:59 | 5 | fixed `00111` |
| 58:56 | 3 | pred |
| 55 | 1 | skipinv |
| 54 | 1 | test_bit_2 |
| 53 | 1 | src0_comp_sel |
| 52 | 1 | syncstart |
| 51 | 1 | dest_bank_ext |
| 50 | 1 | end_or_src0_bank_ext |
| 49 | 1 | src1_bank_ext |
| 48 | 1 | src2_bank_ext |
| 47:46 | 2 | move_type |
| 45:44 | 2 | repeat_count |
| 43 | 1 | nosched |
| 42:40 | 3 | move_data_type |
| 39 | 1 | test_bit_1 |
| 38:35 | 4 | src0_swiz |
| 34 | 1 | src0_bank_sel |
| 33:32 | 2 | dest_bank_sel |
| 31:30 | 2 | src1_bank_sel |
| 29:28 | 2 | src2_bank_sel |
| 27:24 | 4 | dest_mask |
| 23:18 | 6 | dest_n |
| 17:12 | 6 | src0_n |
| 11:6 | 6 | src1_n |
| 5:0 | 6 | src2_n |

| `move_type` | Instruction | Operation per enabled component *i* |
|---|---|---|
| 0 | VMOV | `dest[i] = src1[i]` |
| 1 | VMOVC | `dest[i] = cmp(src0[i]) ? src1[i] : src2[i]` |
| 2 | VMOVCU8 | as VMOVC, but src0 is read as U8 lanes |
| 3 | invalid | |

**`move_data_type`** (3 bits): the operand type, interpreted as 0 S8, 1 S16, 2 S32, 3 C10, 4 F16,
5 F32, 6 U8, 7 U16. VMOV performs **no conversion**: it copies values of that type (bit-exact).

- **Registers:** F32, F16 and C10 moves are **doubled** (64-bit units, 7-bit space). All other types
  address 32-bit words (6-bit fields, not doubled).
- **Write mask:** F32/F16 rewrite `dest_mask` by bank (§2.9). C10 uses it as is. All integer types
  force the mask to x only (non-float moves are scalar, one element).
- **Swizzle:** for F32/F16/C10, src1's swizzle is `decode_vec34(src0_swiz, ext = 0, vec4)` (the field
  is named src0_swiz but applies to src1). Integer moves use identity. src2 always uses src1's
  swizzle. src0 uses src1's swizzle if `src0_comp_sel` = 1, otherwise identity.
- **Conditional compare:** `cmp` is selected by `test_bit_2:test_bit_1`:

  | value | cmp(s) |
  |---|---|
  | 0 | s == 0 |
  | 1 | s != 0 |
  | 2 | s < 0 |
  | 3 | s <= 0 |

  The comparison is signed, unsigned or float according to the test type: U8 for VMOVCU8,
  otherwise `move_data_type`.
- **src0 bank ext** shares bit 50 with `end`: it is src0's bank ext for conditional moves and the
  `end` flag for VMOV.
- **Repeat:** `repeat_count + 1` (1–4), SLMSI mode, mult = 2 for F16/F32, 1 otherwise.
- **Uncertain**: Vita3K marks dest-mask and modifier handling TODO. There are no neg/abs fields.

---

### 3.8 VPCK: pack, unpack and convert

Pattern (bit 63 first): `01000pppsnuydercaaaaffftttmmmmbbkkllgggggggoohiijjqqqqqqvwwwwwwx`  
Match: `(insn & 0xF800000000000000) == 0x4000000000000000`

| Bits | Width | Field |
|---|---|---|
| 63:59 | 5 | fixed `01000` |
| 58:56 | 3 | pred |
| 55 | 1 | skipinv |
| 54 | 1 | nosched |
| 53 | 1 | unknown |
| 52 | 1 | syncstart |
| 51 | 1 | dest_bank_ext |
| 50 | 1 | end |
| 49 | 1 | src1_bank_ext |
| 48 | 1 | src2_bank_ext |
| 47:44 | 4 | repeat_count |
| 43:41 | 3 | src_fmt |
| 40:38 | 3 | dest_fmt |
| 37:34 | 4 | dest_mask |
| 33:32 | 2 | dest_bank_sel |
| 31:30 | 2 | src1_bank_sel |
| 29:28 | 2 | src2_bank_sel |
| 27:21 | 7 | dest_n |
| 20:19 | 2 | comp_sel_3 |
| 18 | 1 | scale |
| 17:16 | 2 | comp_sel_1 |
| 15:14 | 2 | comp_sel_2 |
| 13:8 | 6 | src1_n |
| 7 | 1 | comp0_sel_bit1 |
| 6:1 | 6 | src2_n |
| 0 | 1 | comp_sel_0_bit0 |

Converts up to 4 components from one format to another, optionally normalizing.

**Formats** (same table for `src_fmt` and `dest_fmt`):

| code | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| type | U8 | S8 | O8 | U16 | S16 | F16 | F32 | C10 |

Valid combinations (Vita3K's opcode matrix, with its own note that some may be wrong):
- dest U8/S8/O8/U16/S16 ← src U8, S8, O8, U16, S16, F16, F32 (not C10)
- dest F16/F32 ← any source including C10
- dest C10 ← F16, F32, C10 only

Mnemonic: `VPCK<dest><src>`, e.g. VPCKU8F32 = float → unorm8 packing; VPCKF32U8 = unpack.

**Operation:** for each destination component *i* enabled in `dest_mask`:
`dest[i] = convert(src[sel(i)])`. `convert` is:
- int → float: normalized if `scale` = 1 (§2.5: ÷255 for U8, etc.), raw if 0
- float → int: normalized (clamp, ×255, round) if `scale` = 1, raw truncating conversion if 0
- float → float, int → int: value conversion (F32↔F16 rounding; integer width change)

The destination is written in its packed form (§2.5). For example, VPCKU8F32 with mask xyzw writes
all 4 bytes of one 32-bit destination word.

**Operand decoding:**
- dest: 7-bit, **not doubled** (32-bit word units).
- src1: doubled if `src_fmt` is a float type (F16/F32/C10). For integer source formats the register
  number is 7 bits, `src1_n << 1 | comp0_sel_bit1`, not doubled.
- src2: doubled (it extends src1, see below).
- No modifiers. Destination banks other than the normal ones (SPECIAL) are rejected.

**Component selection:** `comp_sel_0..3` are 2-bit source-channel indices (0 x … 3 w) giving the
source swizzle. `comp_sel_0` is split: its bit 0 is `comp_sel_0_bit0`, and its bit 1 is
`comp0_sel_bit1` when `src_fmt` = F32, otherwise bit 0 of `src2_n`.

Vita3K then applies a **swizzle compaction** unless both formats are float, or `scale` = 1 and
either side is U8. Enabled destination components consume `comp_sel` entries in order (the first
enabled destination component uses comp_sel_0, the next comp_sel_1, …). For a U8 source or
destination with mask xyzw, the components alternate between comp_sel_0 and comp_sel_1.
**Uncertain:** this is empirical.

**F32 sources wider than one 64-bit unit:** an F32 vec4 spans two 64-bit units. src1 supplies
components x,y and **src2** supplies z,w. Vita3K reads 4 consecutive words from src1 only when
src1 = src2, the swizzle is identity and the mask is contiguous from x (x, xy, xyz, xyzw).
Otherwise it reads the pair src1:src2. Treat src2 as "the second half of the source vector".

**Repeat:** `repeat_count + 1` (4-bit, up to 16), SLMSI mode. mult = (1, 2, 2, 1) when one side is
float and the other integer, else (1, 1, 1, 1).

**Sources in the constant ROM (§2.7) or an immediate (§2.6):** read like any other bank, word by
word -- constant *k* is words 2k (F32 x, or F16/U8 lanes 0-1) and 2k+1 (y, or lanes 2-3), an
immediate is its own value. Secondary programs use `VPCK.f16->f32 sa <- c<k>` to seed uniforms with
hardware constants: Uncharted's swipe-prompt threshold pass (fragment 0x1f1a9fc8) loads its
inside/outline selectors as `sa8..9 <- c15.y` (1.0) and `sa10..11 <- c0` (0.0). Both executors
used to read a VPCK source in these banks as 0 (2026-10-09): every pixel of the arrow became
(0,0,0,0) and the prompt never showed.

---

### 3.9 VTST / VTSTMSK: tests

Pattern (bit 63 first): `01001ppps-oydrceavttiizzmhhhnnbbkkffgggggggwlluuuujjjjjjjqqqqqqq`  
Match: `(insn & 0xF800000000000000) == 0x4800000000000000`

| Bits | Width | Field |
|---|---|---|
| 63:59 | 5 | fixed `01001` |
| 58:56 | 3 | pred |
| 55 | 1 | skipinv |
| 54 | 1 | (not decoded) |
| 53 | 1 | onceonly |
| 52 | 1 | syncstart |
| 51 | 1 | dest_ext |
| 50 | 1 | src1_neg |
| 49 | 1 | src1_ext |
| 48 | 1 | src2_ext |
| 47 | 1 | prec |
| 46 | 1 | src2_vscomp |
| 45:44 | 2 | rpt_count |
| 43:42 | 2 | sign_test |
| 41:40 | 2 | zero_test |
| 39 | 1 | test_crcomb_and |
| 38:36 | 3 | chan_cc |
| 35:34 | 2 | pdst_n |
| 33:32 | 2 | dest_bank |
| 31:30 | 2 | src1_bank |
| 29:28 | 2 | src2_bank |
| 27:21 | 7 | dest_n |
| 20 | 1 | test_wben |
| 19:18 | 2 | alu_sel |
| 17:14 | 4 | alu_op |
| 13:7 | 7 | src1_n |
| 6:0 | 7 | src2_n |

Pattern (bit 63 first): `01111ppps-oydtrcevuuiizzm-aa--bbnnkkfffffffwllgggghhhhhhhjjjjjjj`  
Match: `(insn & 0xF800000000000000) == 0x7800000000000000`

| Bits | Width | Field |
|---|---|---|
| 63:59 | 5 | fixed `01111` |
| 58:56 | 3 | pred |
| 55 | 1 | skipinv |
| 54 | 1 | (not decoded) |
| 53 | 1 | onceonly |
| 52 | 1 | syncstart |
| 51 | 1 | dest_ext |
| 50 | 1 | test_flag_2 |
| 49 | 1 | src1_ext |
| 48 | 1 | src2_ext |
| 47 | 1 | prec |
| 46 | 1 | src2_vscomp |
| 45:44 | 2 | rpt_count |
| 43:42 | 2 | sign_test |
| 41:40 | 2 | zero_test |
| 39 | 1 | test_crcomb_and |
| 38 | 1 | (not decoded) |
| 37:36 | 2 | tst_mask_type |
| 35:34 | 2 | (not decoded) |
| 33:32 | 2 | dest_bank |
| 31:30 | 2 | src1_bank |
| 29:28 | 2 | src2_bank |
| 27:21 | 7 | dest_n |
| 20 | 1 | test_wben |
| 19:18 | 2 | alu_sel |
| 17:14 | 4 | alu_op |
| 13:7 | 7 | src1_n |
| 6:0 | 7 | src2_n |

Both compute an ALU operation on src1/src2, compare the result, and record the outcome. VTST writes
a predicate register. VTSTMSK writes an all-ones/all-zeros mask into a normal register.

**ALU operation** = (`alu_sel`, `alu_op`):

`alu_sel` = 0 → float vector op. `prec` = 1 → F32, 0 → F16:

| alu_op | 0–1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| op | – | ADD | FRC | RCP | RSQ | LOG | EXP | DP | MIN | MAX | DSX | DSY | MUL | SUB | – |

`alu_sel` = 1 → 16/32-bit integer:

| alu_op | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---|---|---|---|---|---|---|---|---|---|---|
| op | IADD16 (s) | ISUB16 (s) | IMUL16 (s) | IADDU16 | ISUBU16 | IMULU16 | IADD32 (s) | IADDU32 | ISUB32 (s) | ISUBU32 |

(0–5 invalid. "(s)" = signed compare; the others are unsigned.)

`alu_sel` = 2 → 8-bit (operands treated as U8):

| alu_op | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|---|
| op | IADD8 | ISUB8 | IADDU8 | ISUBU8 | IMUL8 | FPMUL8 | IMULU8 | FPADD8 | FPSUB8 |

`alu_sel` = 3 → bitwise on U32:

| alu_op | 0 | 1 | 2 | 3 | 4 | 5 | 7 |
|---|---|---|---|---|---|---|---|
| op | AND | OR | XOR | SHL | SHR | ROL | ASR |

**Operands:** registers are doubled (8-bit space) for `alu_sel` = 0 and plain 7-bit otherwise.
src1 and src2 have identity swizzles, except that `src2_vscomp` (float only) makes src2 a scalar
broadcast (xxxx). src1 can be negated: `src1_neg` for VTST, `test_flag_2` for VTSTMSK.

**Comparison.** The result *r* (or the pair src1, src2 for SUB) is classified by two 2-bit fields.
The model that fits the encoding: `sign_test` 0 none, 1 r < 0, 2 r > 0. `zero_test` 0 none,
1 r == 0, 2 r != 0. The two are combined with AND when `test_crcomb_and` = 1, OR otherwise. Vita3K
implements a table that agrees with this model for every useful combination:

| sign_test | zero_test = 1 | zero_test = 0 or 2 |
|---|---|---|
| 0 | r == 0 | r != 0 |
| 1 | r <= 0 | r < 0 |
| 2 | r >= 0 | r > 0 |
| 3 | invalid | invalid |

- For the **SUB** operations (VSUB, ISUB*, FPSUB8) Vita3K compares src1 against src2 directly
  instead of subtracting first. This is equivalent without overflow, and is the "CMP" idiom.
- For all other operations it computes `r = op(src1, src2)` and compares *r* with 0, as float,
  signed or unsigned according to the operation's type.
- `test_crcomb_and` is not read by Vita3K. **Uncertain**.

**VTST destination:** predicate register **p[`pdst_n`]**. `chan_cc` selects the single component
tested: 0 x, 1 y, 2 z, 3 w (≥ 4 unsupported). With a DP operation all 4 components feed the dot
product. `dest_bank`/`dest_n` are unused by Vita3K for VTST.

**VTSTMSK destination:** register (`dest_bank`/`dest_ext`/`dest_n`, doubled for float). For each
tested component it writes "true" = all ones, "false" = 0:
- Float types: 1.0 / 0.0.
- U8 0xFF, U16 0xFFFF, U32 0xFFFFFFFF; S8 0x7F, S16 0x7FFF, S32 0x7FFFFFFF.
- `tst_mask_type` = 2 with a float op → all 4 components (vec4 mask). 0/1 → single component (x).
  3 is invalid.
- `test_wben` = 0 (write disabled) is unimplemented in Vita3K. Presumably the instruction then only
  updates predicates.

**Repeat** (`rpt_count`) and `onceonly`: unimplemented in Vita3K.

---

### 3.10 VBW: bitwise and shifts

Pattern (bit 63 first): `01ooopppsnrydecxaaaaittttthhbwkkffggjjjjjjjlllllllmmmmmmmqqqqqqq`  
Match: `(insn & 0xC000000000000000) == 0x4000000000000000`

| Bits | Width | Field |
|---|---|---|
| 63:62 | 2 | fixed `01` |
| 61:59 | 3 | op1 |
| 58:56 | 3 | pred |
| 55 | 1 | skipinv |
| 54 | 1 | nosched |
| 53 | 1 | repeat_sel |
| 52 | 1 | sync_start |
| 51 | 1 | dest_ext |
| 50 | 1 | end |
| 49 | 1 | src1_ext |
| 48 | 1 | src2_ext |
| 47:44 | 4 | repeat_count |
| 43 | 1 | src2_invert |
| 42:38 | 5 | src2_rot |
| 37:36 | 2 | src2_exth |
| 35 | 1 | op2 |
| 34 | 1 | bitwise_partial |
| 33:32 | 2 | dest_bank |
| 31:30 | 2 | src1_bank |
| 29:28 | 2 | src2_bank |
| 27:21 | 7 | dest_n |
| 20:14 | 7 | src2_sel |
| 13:7 | 7 | src1_n |
| 6:0 | 7 | src2_n |

`dest = src1 OP src2'`, on 32-bit (or 16-bit if `bitwise_partial`) integers.

The effective op1 values are 010–110 (01000, 01001, 01111 are VPCK/VTST/VTSTMSK):

| op1 (insn[61:59]) | op2 = 0 | op2 = 1 |
|---|---|---|
| 010 | AND | OR |
| 011 | XOR | XOR |
| 100 | SHL (logical) | ROL (rotate left) |
| 101 | SHR (logical) | ASR (arithmetic) |
| 110 | unknown (not implemented in Vita3K; probably RLP or another op) | unknown |

**src2' preparation** (in this order):
1. Value: if src2's bank is IMMEDIATE (`src2_ext` = 1 and `src2_bank` = 2), the 16-bit immediate
   `src2_n | src2_sel << 7 | src2_exth << 14`. Otherwise the register value.
2. Rotate left by `src2_rot` within the width (32, or 16 with `bitwise_partial`, where
   `rot &= 15`).
3. If `src2_invert`, bitwise NOT (masked to 16 bits in partial mode).

Notes:
- Registers are not doubled (7-bit, 32-bit units).
- Shift counts come from src2' (use the low 5 bits for 32-bit shifts, as on most hardware;
  **uncertain** for counts ≥ 32).
- ROL is recognised but Vita3K cannot translate it. Implement as a rotate.
- `bitwise_partial` = 16-bit operation (U16 operands, mnemonic suffix "16").
- `repeat_count`/`repeat_sel`: not implemented by Vita3K for VBW. Presumably SLMSI-style repeats.

---

### 3.11 SOP2, SOP2M, SOP3: blend-style sum of products

These operate on **U8 RGBA** values (4 bytes in one 32-bit word: x/R = byte 0 … w/A = byte 3),
converted to 0..1 floats (÷255), combined, and converted back with clamping and rounding. They
implement fixed-function-style blending and color arithmetic. Registers are never doubled (7-bit
fields, 32-bit units).

#### SOP2

Pattern (bit 63 first): `10000ppcsnaaderbmooofllggghhhittkkjjqqqqqqquvvwwxyzzzzzzzAAAAAAA`  
Match: `(insn & 0xF800000000000000) == 0x8000000000000000`

| Bits | Width | Field |
|---|---|---|
| 63:59 | 5 | fixed `10000` |
| 58:57 | 2 | pred |
| 56 | 1 | cmod1 |
| 55 | 1 | skipinv |
| 54 | 1 | nosched |
| 53:52 | 2 | asel1 |
| 51 | 1 | dest_bank_ext |
| 50 | 1 | end |
| 49 | 1 | src1_bank_ext |
| 48 | 1 | src2_bank_ext |
| 47 | 1 | cmod2 |
| 46:44 | 3 | count |
| 43 | 1 | amod1 |
| 42:41 | 2 | asel2 |
| 40:38 | 3 | csel1 |
| 37:35 | 3 | csel2 |
| 34 | 1 | amod2 |
| 33:32 | 2 | dest_bank |
| 31:30 | 2 | src1_bank |
| 29:28 | 2 | src2_bank |
| 27:21 | 7 | dest_n |
| 20 | 1 | src1_mod |
| 19:18 | 2 | cop |
| 17:16 | 2 | aop |
| 15 | 1 | asrc1_mod |
| 14 | 1 | dest_mod |
| 13:7 | 7 | src1_n |
| 6:0 | 7 | src2_n |

Per word: color = rgb (bytes 0–2), alpha = byte 3.

```
s1 = src1, s2 = src2;  if src1_mod: s1.rgb = 1 - s1.rgb, s1.a = 1 - s1.a  (complement)
cf1 = csel(csel1); cf2 = csel(csel2); af1 = asel(asel1); af2 = asel(asel2)
if cmod1: cf1 = 1 - cf1;  if cmod2: cf2 = 1 - cf2;  if amod1: af1 = 1 - af1;  if amod2: af2 = 1 - af2
rgb = cop(cf1 * s1.rgb, cf2 * s2.rgb)
a   = aop(af1 * s1.a,   af2 * s2.a)
dest = pack_u8(rgb, a)
```

| csel1 / csel2 | factor |
|---|---|
| 0 | 0 |
| 1 | src1.rgb |
| 2 | src2.rgb |
| 3 | src1.aaa |
| 4 | src2.aaa |
| 5 | source-alpha saturate (unimplemented; presumably min(src1.a, 1 − src2.a)) |
| 6 | csel1: "source alpha scale"; csel2: "zero source 2 minus half" (both unimplemented) |

| asel1 / asel2 | factor |
|---|---|
| 0 | 0 |
| 1 | src1.a |
| 2 | src2.a |
| 3 | asel1: "source 2 scale"; asel2: "zero source 2 minus half" (unimplemented) |

| cop / aop | op |
|---|---|
| 0 | add |
| 1 | subtract (lhs − rhs) |
| 2 | min |
| 3 | max |

- `asrc1_mod`, `dest_mod`: decoded, unused in Vita3K (uncertain: alpha-source complement, result
  complement).
- `count` = repeat count (`count + 1` iterations, SLMSI mode).

#### SOP2M (SOP2 with write mask)

Pattern (bit 63 first): `10010ppmsnccderbowwwwaalllfff-ttkkgguuuuuuu-------hhhhhhhiiiiiii`  
Match: `(insn & 0xF800000000000000) == 0x9000000000000000`

| Bits | Width | Field |
|---|---|---|
| 63:59 | 5 | fixed `10010` |
| 58:57 | 2 | pred |
| 56 | 1 | mod1 |
| 55 | 1 | skipinv |
| 54 | 1 | nosched |
| 53:52 | 2 | cop |
| 51 | 1 | destbankext |
| 50 | 1 | end |
| 49 | 1 | src1bankext |
| 48 | 1 | src2bankext |
| 47 | 1 | mod2 |
| 46:43 | 4 | wmask |
| 42:41 | 2 | aop |
| 40:38 | 3 | sel1 |
| 37:35 | 3 | sel2 |
| 34 | 1 | (not decoded) |
| 33:32 | 2 | destbank |
| 31:30 | 2 | src1bank |
| 29:28 | 2 | src2bank |
| 27:21 | 7 | destnum |
| 20:14 | 7 | (not decoded) |
| 13:7 | 7 | src1num |
| 6:0 | 7 | src2num |

```
f1 = sel(sel1); f2 = sel(sel2)        // vec4 factors
if mod1: f1 = 1 - f1;  if mod2: f2 = 1 - f2
t1 = f1 * src1;  t2 = f2 * src2       // all 4 channels
res.rgb = cop(t1, t2);  res.a = aop(t1.a, t2.a)
```

| sel | factor (vec4) |
|---|---|
| 0 | 0 |
| 1 | source-alpha saturate (unimplemented) |
| 2 | src1 |
| 3 | src1.aaaa |
| 4, 5 | unimplemented |
| 6 | src2 |
| 7 | src2.aaaa |

cop/aop as SOP2. **Write mask** `wmask` puts alpha in bit 0. The real mask is
`((wmask >> 1) & 0b0111) | ((wmask & 1) << 3)`, so wmask bit0 = A, bit1 = R, bit2 = G, bit3 = B.
That is the GXM color-mask order (`SCE_GXM_COLOR_MASK_A = 1`, R = 2, G = 4, B = 8).

#### SOP3

Pattern (bit 63 first): `10001ppcsnooderbmallfgghhhiiikttjjqquuuuuuuvvvvvvvwwwwwwwxxxxxxx`  
Match: `(insn & 0xF800000000000000) == 0x8800000000000000`

| Bits | Width | Field |
|---|---|---|
| 63:59 | 5 | fixed `10001` |
| 58:57 | 2 | pred |
| 56 | 1 | cmod1 |
| 55 | 1 | skipinv |
| 54 | 1 | nosched |
| 53:52 | 2 | cop |
| 51 | 1 | destbext |
| 50 | 1 | end |
| 49 | 1 | src1bext |
| 48 | 1 | src2bext |
| 47 | 1 | cmod2 |
| 46 | 1 | amod1 |
| 45:44 | 2 | asel1 |
| 43 | 1 | dmod |
| 42:41 | 2 | aop |
| 40:38 | 3 | csel1 |
| 37:35 | 3 | csel2 |
| 34 | 1 | src0bank |
| 33:32 | 2 | destbank |
| 31:30 | 2 | src1bank |
| 29:28 | 2 | src2bank |
| 27:21 | 7 | destn |
| 20:14 | 7 | src0n |
| 13:7 | 7 | src1n |
| 6:0 | 7 | src2n |

Three U8 sources. `cop`/`aop` ∈ {0 add, 1 subtract} only.

```
cf1 = csel_tbl(csel1); cf2 = csel_tbl(csel2)
af1 = asel_tbl(asel1); af2 = alpha2_tbl(csel2)       // alpha factor 2 is chosen by csel2
cmod1/cmod2/amod1 complement cf1/cf2/af1;  cmod2 also complements af2
rgb = cop(cf1 * src1.rgb, cf2 * src2.rgb)
a   = aop(af1 * src1.a,   af2 * src2.a)
```

| csel (csel_tbl) | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| factor | 0 | alpha-sat (unimpl.) | src1.rgb | src1.aaa | src0.rgb | src0.aaa | src2.rgb | src2.aaa |

| asel1 (asel_tbl) | 0 | 1 | 2 | 3 |
|---|---|---|---|---|
| factor | 0 | src0.a | src1.a | src2.a |

| csel2 → alpha2_tbl | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| factor | 0 | alpha-sat | src1.a | src1.a | src0.a | src0.a | src2.a | src2.a |

src0 uses the src0 table (ext = 0). src0 only enters through the selectors, so it is a
"blend factor" source. `dmod` ≠ 0 is unimplemented (it rejects the instruction). There is no
repeat. "Uses csel2 for alpha 2" and "cmod2 for alpha 2" are Vita3K's empirically chosen rules.

---

### 3.12 Integer multiply-add: I8MAD, I16MAD, I32MAD, I32MAD2, I8MAD2

Registers are never doubled (7-bit fields). Predicates are ShortPredicate for the 10xxx group and
ExtPredicate for I32MAD2.

#### I8MAD (op1 10011)

Pattern (bit 63 first): `10011ppcsneedarbmtttuolfghijkqvvwwxxyyyyyyyzzzzzzzAAAAAAABBBBBBB`  
Match: `(insn & 0xF800000000000000) == 0x9800000000000000`

| Bits | Width | Field |
|---|---|---|
| 63:59 | 5 | fixed `10011` |
| 58:57 | 2 | pred |
| 56 | 1 | cmod1 |
| 55 | 1 | skipinv |
| 54 | 1 | nosched |
| 53:52 | 2 | csel0 |
| 51 | 1 | dest_bank_ext |
| 50 | 1 | end |
| 49 | 1 | src1_bank_ext |
| 48 | 1 | src2_bank_ext |
| 47 | 1 | cmod2 |
| 46:44 | 3 | repeat_count |
| 43 | 1 | saturated |
| 42 | 1 | cmod0 |
| 41 | 1 | asel0 |
| 40 | 1 | amod2 |
| 39 | 1 | amod1 |
| 38 | 1 | amod0 |
| 37 | 1 | csel1 |
| 36 | 1 | csel2 |
| 35 | 1 | src0_neg |
| 34 | 1 | src0_bank |
| 33:32 | 2 | dest_bank |
| 31:30 | 2 | src1_bank |
| 29:28 | 2 | src2_bank |
| 27:21 | 7 | dest_num |
| 20:14 | 7 | src0_num |
| 13:7 | 7 | src1_num |
| 6:0 | 7 | src2_num |

Four U8 lanes (one word):

```
add.rgb = (csel0: 0 src0.rgb, 1 src1.rgb, 2 src0.aaa, 3 src1.aaa)
add.a   = (asel0: 0 src0.a,  1 src1.a)
m1 = csel1 ? src1.aaaa-for-rgb : src1      // csel1/csel2: replace the rgb lanes with alpha
m2 = csel2 ? src2.aaaa-for-rgb : src2
dest = src0_neg ? m1*m2 - add : m1*m2 + add      (per lane)
```

- **Unimplemented in Vita3K:** `cmod0/1/2`, `amod0/1/2` (presumably complement modifiers, 1−x) and
  `saturated`. Vita3K's lane arithmetic is plain integer arithmetic on 0–255 lanes. Real hardware
  almost certainly renormalizes (a product of two U8 values ÷255) and saturates. **Uncertain**:
  verify before relying on it.

#### I16MAD (op1 10100)

Pattern (bit 63 first): `10100ppasnredbck-tttmmffoolhhgiijjqquuuuuuuvvvvvvvwwwwwwwxxxxxxx`  
Match: `(insn & 0xF800000000000000) == 0xA000000000000000`

| Bits | Width | Field |
|---|---|---|
| 63:59 | 5 | fixed `10100` |
| 58:57 | 2 | pred |
| 56 | 1 | abs |
| 55 | 1 | skipinv |
| 54 | 1 | nosched |
| 53 | 1 | src2_neg |
| 52 | 1 | sel1h_upper8 |
| 51 | 1 | dest_bank_ext |
| 50 | 1 | end |
| 49 | 1 | src1_bank_ext |
| 48 | 1 | src2_bank_ext |
| 47 | 1 | (not decoded) |
| 46:44 | 3 | repeat_count |
| 43:42 | 2 | mode |
| 41:40 | 2 | src2_format |
| 39:38 | 2 | src1_format |
| 37 | 1 | sel2h_upper8 |
| 36:35 | 2 | or_shift |
| 34 | 1 | src0_bank |
| 33:32 | 2 | dest_bank |
| 31:30 | 2 | src1_bank |
| 29:28 | 2 | src2_bank |
| 27:21 | 7 | dest_n |
| 20:14 | 7 | src0_n |
| 13:7 | 7 | src1_n |
| 6:0 | 7 | src2_n |

`dest = src0 * src1 + src2` on one 16-bit lane (low half of each register).

| Field | Meaning |
|---|---|
| `mode` | bit1 = signed, bit0 = saturate (saturation unimplemented in Vita3K) |
| `src1_format` / `src2_format` | 0 = 16-bit operand. 1 = 8-bit operand: the low byte, or the high byte of the low half if `sel1h_upper8`/`sel2h_upper8`. 2/3 unimplemented (probably sign-extended 8-bit). |
| `or_shift`, `abs`, `src2_neg` | unimplemented |

#### I32MAD (op1 10101)

Pattern (bit 63 first): `10101pps-nrcdeba0tttif00yy000kgghhjjlllllllmmmmmmmoooooooqqqqqqq`  
Match: `(insn & 0xF800833800000000) == 0xA800000000000000`

| Bits | Width | Field |
|---|---|---|
| 63:59 | 5 | fixed `10101` |
| 58:57 | 2 | pred |
| 56 | 1 | src0_high |
| 55 | 1 | (not decoded) |
| 54 | 1 | nosched |
| 53 | 1 | src1_high |
| 52 | 1 | src2_high |
| 51 | 1 | dest_bank_ext |
| 50 | 1 | end |
| 49 | 1 | src1_bank_ext |
| 48 | 1 | src2_bank_ext |
| 47 | 1 | fixed `0` |
| 46:44 | 3 | repeat_count |
| 43 | 1 | is_signed |
| 42 | 1 | is_sat |
| 41:40 | 2 | fixed `00` |
| 39:38 | 2 | src2_type |
| 37:35 | 3 | fixed `000` |
| 34 | 1 | src0_bank |
| 33:32 | 2 | dest_bank |
| 31:30 | 2 | src1_bank |
| 29:28 | 2 | src2_bank |
| 27:21 | 7 | dest_n |
| 20:14 | 7 | src0_n |
| 13:7 | 7 | src1_n |
| 6:0 | 7 | src2_n |

`dest(32) = src0(16) * src1(16) + src2`.
- `src0_high` / `src1_high`: take the high 16 bits of the source word instead of the low 16.
- `src2_type` = 2 → src2 is a full 32-bit value. Otherwise src2 is 16-bit, with `src2_high` selecting
  the half.
- `is_signed` selects signed or unsigned multiplication/extension. `is_sat` (saturation) is
  unimplemented.
- Vita3K marks "high/low modifier" and "more src2 types" as TODO. **Uncertain** (Vita3K's mnemonic
  for this one is "IMAD2").

#### I32MAD2 (op1 11010)

Pattern (bit 63 first): `11010ppp-nssdercbooo00iga0000kttffhhjjjjjjjlllllllmmmmmmmqqqqqqq`  
Match: `(insn & 0xF8000C7800000000) == 0xD000000000000000`

| Bits | Width | Field |
|---|---|---|
| 63:59 | 5 | fixed `11010` |
| 58:56 | 3 | pred |
| 55 | 1 | (not decoded) |
| 54 | 1 | nosched |
| 53:52 | 2 | sn |
| 51 | 1 | dest_bank_ext |
| 50 | 1 | end |
| 49 | 1 | src1_bank_ext |
| 48 | 1 | src2_bank_ext |
| 47 | 1 | src0_bank_ext |
| 46:44 | 3 | count |
| 43:42 | 2 | fixed `00` |
| 41 | 1 | is_signed |
| 40 | 1 | negative_src1 |
| 39 | 1 | negative_src2 |
| 38:35 | 4 | fixed `0000` |
| 34 | 1 | src0_bank |
| 33:32 | 2 | dest_bank |
| 31:30 | 2 | src1_bank |
| 29:28 | 2 | src2_bank |
| 27:21 | 7 | dest_n |
| 20:14 | 7 | src0_n |
| 13:7 | 7 | src1_n |
| 6:0 | 7 | src2_n |

Full 32-bit multiply-add: `dest = src0 * (neg1 ? -src1 : src1) + (neg2 ? -src2 : src2)`, signed or
unsigned per `is_signed`. src0's bank uses `src0_bank` with `src0_bank_ext`.

- `count` is a **repeat count** (`count + 1` iterations) with SLMSI stepping. It is used, for
  example, to compute several store addresses in one instruction (Unity GPU skinning).
- `sn` ("mysterious" in Vita3K): `sn` = 0 → normal. With `sn` ≠ 0, Vita3K writes `src2` unchanged
  to dest. Hardware tests show that an `sn = 1` instruction reading, as src2, the destination of a
  preceding `sn = 0` instruction reproduces the multiply-add result. This suggests `sn` relates to
  an internal accumulator / result forwarding. Writing PA directly with `sn = 1` crashed the
  hardware. 2/3 are untested. **Uncertain.**

#### I8MAD2 (op1 11001)

Pattern (bit 63 first): `11001-----------------------------------------------------------`  
Match: `(insn & 0xF800000000000000) == 0xC800000000000000`

| Bits | Width | Field |
|---|---|---|
| 63:59 | 5 | fixed `11001` |
| 58:0 | 59 | (not decoded) |

Entirely unimplemented in Vita3K: no fields decoded, treated as a no-op. Log it if encountered.

---

### 3.13 SMP: texture sample

Pattern (bit 63 first): `11100pppsn-ymrceffaaddlltbbggkhhiijjoooooooqqqqqqquuuuuuuvvvvvvv`  
Match: `(insn & 0xF800000000000000) == 0xE000000000000000`

| Bits | Width | Field |
|---|---|---|
| 63:59 | 5 | fixed `11100` |
| 58:56 | 3 | pred |
| 55 | 1 | skipinv |
| 54 | 1 | nosched |
| 53 | 1 | (not decoded) |
| 52 | 1 | syncstart |
| 51 | 1 | minpack |
| 50 | 1 | src0_ext |
| 49 | 1 | src1_ext |
| 48 | 1 | src2_ext |
| 47:46 | 2 | fconv_type |
| 45:44 | 2 | mask_count |
| 43:42 | 2 | dim |
| 41:40 | 2 | lod_mode |
| 39 | 1 | dest_use_pa |
| 38:37 | 2 | sb_mode |
| 36:35 | 2 | src0_type |
| 34 | 1 | src0_bank |
| 33:32 | 2 | drc_sel |
| 31:30 | 2 | src1_bank |
| 29:28 | 2 | src2_bank |
| 27:21 | 7 | dest_n |
| 20:14 | 7 | src0_n |
| 13:7 | 7 | src1_n |
| 6:0 | 7 | src2_n |

| Operand | Meaning |
|---|---|
| src0 | **coordinates.** src0 table + `src0_ext`, doubled (7-bit field, 8-bit space). Type by `src0_type`: 0 F32, 1 F16, 2 C10. Components: 1 (1D), 2 (2D), 3 (3D/cube). |
| src1 | **texture state.** src1 table + `src1_ext`, doubled. Names the first of the **four SA registers holding the texture's control words** (§1.8). The register *number* identifies the texture. The four words fully describe it (address, format, size, filtering, addressing). |
| src2 | LOD bias, explicit LOD, or gradients (only if `lod_mode` ≠ 0). Doubled, type = src0's type. |
| dest | `dest_n`, **not doubled**. Bank TEMP, or PA if `dest_use_pa` (SA when in a secondary program). |

| Field | Meaning |
|---|---|
| `dim` | encoded 0/1/2 → 1D/2D/3D. Cube textures are 3D-coordinate samples of a cube texture. The texture type in the control words decides the cube addressing. Vita3K samples 1D as 2D with y = 0 (a guess). |
| `lod_mode` | 0 implicit LOD, 1 bias (`lod + src2.x`), 2 explicit LOD = `src2.x` ("replace"), 3 gradients: 2D uses ddx = src2.xy, ddy = src2.zw; 3D uses ddx = src2.xyz, ddy = the next three components (Vita3K reads them one register later, uncertain) |
| `sb_mode` | 0 normal sample. 1 **gather4** (the four bilinear-footprint texels of one channel). 2 **info**: returns LOD/coordinate-fraction information as U8 (Vita3K: lod-mode ≠ 0 unsupported). 3 gather4 plus bilinear weights (F16). |
| `fconv_type` | result format: 0 = the texture's native integral format, still packed (e.g. a U8U8U8U8 texture gives one word of 4 bytes). 1 = unknown (Vita3K falls back to the native format). 2 = F16. 3 = F32. |
| `mask_count` | unused by Vita3K. It writes as many components as the texture format has (1, 2 or 4; 3-component formats are expanded to 4). Possibly the hardware's component count, **uncertain**. |
| `minpack`, `drc_sel`, `syncstart`, `nosched`, `skipinv` | scheduling / data-return hints. Ignore. |

**Result:** the sampled texel after the texture's own swizzle (component order from control word
3), converted per `fconv_type`, written packed into dest (§2.5): F32 = one word per component,
F16 = two per word, native = the texel's packed form.

**Texture-buffer indexing (dynamic texture choice):** if src1 does not name a dependent-sampler
register, the program selects the texture at run time. src1 then holds an integer index into the
texture buffer (§1.5). Vita3K supports this only for `sb_mode = 0`, `lod_mode = 2`.

**Observed** (72 SMP instructions in 326 programs): all 2D with F32 coordinates and `sb_mode` 0.
`lod_mode` 2 (explicit LOD) is the most common, then 3 (gradients) and 0. `fconv_type` is 2 (F16)
or 3 (F32). Most programs sample through the iterator path (§1.12) instead.

Coordinates are **normalized** (0..1 maps across the texture). Filtering, wrapping and mipmapping
follow the texture control words.

---

### 3.14 PHAS: phase change

Pattern (bit 63 first): `11111010s100eirc--matwwwppppppppbbnn--------xxxxxxoooooooddddddd`  
Match: `(insn & 0xFF70000000000000) == 0xFA40000000000000`

| Bits | Width | Field |
|---|---|---|
| 63:56 | 8 | fixed `11111010` |
| 55 | 1 | sprvv |
| 54:52 | 3 | fixed `100` |
| 51 | 1 | end |
| 50 | 1 | imm |
| 49 | 1 | src1_bank_ext |
| 48 | 1 | src2_bank_ext |
| 47:46 | 2 | (not decoded) |
| 45 | 1 | mode |
| 44 | 1 | rate_hi |
| 43 | 1 | rate_lo_or_nosched |
| 42:40 | 3 | wait_cond |
| 39:32 | 8 | temp_count |
| 31:30 | 2 | src1_bank |
| 29:28 | 2 | src2_bank |
| 27:20 | 8 | (not decoded) |
| 19:14 | 6 | exe_addr_high |
| 13:7 | 7 | src1_n_or_exe_addr_mid |
| 6:0 | 7 | src2_n_or_exe_addr_low |

Marks a boundary between execution phases of a fragment program, e.g. between the
per-sample/"selective rate" part and the per-pixel part, or a hand-off back to the scheduler
before writing the tile. It carries the number of temporaries for the next phase, a wait condition
and the next phase's start address:

`next_pc = exe_addr_high << 14 | src1_n_or_exe_addr_mid << 7 | src2_n_or_exe_addr_low` (20 bits,
in instructions, the same width as the PC) when `imm` = 1. Otherwise src1/src2 are register
operands holding it. **Inferred from field names and widths only.** Vita3K does not decode these.

**Unimplemented in Vita3K** (a disassembly stub). Vita3K runs phases as one straight-line program.
For an interpreter: treat PHAS as a no-op and continue with the next instruction. Programs Vita3K
runs show this is sufficient. Revisit if a program uses a non-sequential `next_pc`.

---

### 3.15 NOP

Pattern (bit 63 first): `11111----000-----------101--------------------------------------`  
Match: `(insn & 0xF87001C000000000) == 0xF800014000000000`

| Bits | Width | Field |
|---|---|---|
| 63:59 | 5 | fixed `11111` |
| 58:55 | 4 | (not decoded) |
| 54:52 | 3 | fixed `000` |
| 51:41 | 11 | (not decoded) |
| 40:38 | 3 | fixed `101` |
| 37:0 | 38 | (not decoded) |

No operation.

---

### 3.16 BR: branch

Pattern (bit 63 first): `11111ppps000e-----wynba00r----------------iloooooooooooooooooooo`  
Match: `(insn & 0xF870018000000000) == 0xF800000000000000`

| Bits | Width | Field |
|---|---|---|
| 63:59 | 5 | fixed `11111` |
| 58:56 | 3 | pred |
| 55 | 1 | syncend |
| 54:52 | 3 | fixed `000` |
| 51 | 1 | exception |
| 50:46 | 5 | (not decoded) |
| 45 | 1 | pwait |
| 44 | 1 | sync_ext |
| 43 | 1 | nosched |
| 42 | 1 | br_monitor |
| 41 | 1 | save_link |
| 40:39 | 2 | fixed `00` |
| 38 | 1 | br_type |
| 37:22 | 16 | (not decoded) |
| 21 | 1 | any_inst |
| 20 | 1 | all_inst |
| 19:0 | 20 | br_off |

**Target:** `br_off` is a **signed offset in instructions relative to the BR instruction itself**:
`target = pc_of_BR + br_off`. The 20-bit field is sign-extended only when `br_type` = 1
(Vita3K: `if br_type and br_off & 0x80000: br_off -= 0x100000`). With `br_type` = 0 it is used
as an unsigned 20-bit value. In practice, forward branches use `br_type` 0 and backward branches
(loops) use 1. The PC is 20 bits on SGX543.

**Condition:** the ExtPredicate in insn[58:56] (always / p0–p3 / !p0 / !p1). The branch is taken
iff the predicate holds.

**Calls and returns:**
- `save_link` (bit 41): presumably a call that saves the return address in a link register.
  Vita3K does not read the bit. It recognises calls structurally (a branch whose target is a
  subroutine at the program's start) and inlines them.
- Return: Vita3K's analyser treats the flow-control encoding with insn[40:38] = 010 (bit 54 = 0,
  insn[53:52] = 00) as "return from subroutine". It does not match the BR pattern (bits 40:39 are
  fixed 00 there). It would fall to SPEC, but the analyser handles it first. Implement it as
  "pop the saved link and jump".

**Unimplemented/uncertain fields** (unread by Vita3K): `exception`, `pwait` (presumably wait for
pending predicate results), `sync_ext`, `br_monitor`, `any_inst`/`all_inst` (for branches whose
predicate is evaluated across repeat iterations or instances: taken if any / all instances agree),
`syncend`.

> **Interpreter note:** USSE runs groups of instances in lock-step. When instances disagree on a
> predicated branch, the hardware runs both paths with masking. A CPU interpreter that runs one
> instance at a time can just take or not take the branch per instance. That is semantically
> identical as long as no cross-instance operation (derivatives) sits inside divergent code.

---

### 3.17 SMLSI and SMBO: repeat stepping control

Pattern (bit 63 first): `11111010--01-n--ttttppppssssdrcieeeeeeeeaaaaaaaabbbbbbbbffffffff`  
Match: `(insn & 0xFF30000000000000) == 0xFA10000000000000`

| Bits | Width | Field |
|---|---|---|
| 63:56 | 8 | fixed `11111010` |
| 55:54 | 2 | (not decoded) |
| 53:52 | 2 | fixed `01` |
| 51 | 1 | (not decoded) |
| 50 | 1 | nosched |
| 49:48 | 2 | (not decoded) |
| 47:44 | 4 | temp_limit |
| 43:40 | 4 | pa_limit |
| 39:36 | 4 | sa_limit |
| 35 | 1 | dest_inc_mode |
| 34 | 1 | src0_inc_mode |
| 33 | 1 | src1_inc_mode |
| 32 | 1 | src2_inc_mode |
| 31:24 | 8 | dest_inc |
| 23:16 | 8 | src0_inc |
| 15:8 | 8 | src1_inc |
| 7:0 | 8 | src2_inc |

SMLSI programs the per-operand repeat increment table `inc[slot][r]` used by SLMSI-mode repeats
(§2.12). Slots: **src0 = 0, src1 = 1, src2 = 2, dest = 3** (`dest_*` fields go to slot 3).

For each slot, with its `*_inc` (8 bits) and `*_inc_mode` bit:
- `inc_mode` = 0 (**linear**): `inc[slot][r] = r × (int8)inc` for r = 0..16 (signed stride).
- `inc_mode` = 1 (**pattern**): `inc[slot][r] = (inc >> 2r) & 3` for r = 0..3. Entries 4..16 keep
  their previous values. These are four 2-bit offsets, i.e. a swizzle-like step pattern.

`temp_limit`, `pa_limit`, `sa_limit` (4 bits each) are unread by Vita3K. They are presumably
wrap-around limits for the stepped register numbers. The settings persist until changed.

Pattern (bit 63 first): `11111011--01-n--ddddddddddddssssssssssssrrrrrrrrrrrrcccccccccccc`  
Match: `(insn & 0xFF30000000000000) == 0xFB10000000000000`

| Bits | Width | Field |
|---|---|---|
| 63:56 | 8 | fixed `11111011` |
| 55:54 | 2 | (not decoded) |
| 53:52 | 2 | fixed `01` |
| 51 | 1 | (not decoded) |
| 50 | 1 | nosched |
| 49:48 | 2 | (not decoded) |
| 47:36 | 12 | dest_offset |
| 35:24 | 12 | src0_offset |
| 23:12 | 12 | src1_offset |
| 11:0 | 12 | src2_offset |

SMBO sets a **base offset** per slot: `inc[slot][r] = r + offset` for r = 0..16 (12-bit offsets,
same slot order). It shares the table with SMLSI, and the last SMLSI or SMBO wins. In SLMSI-mode
repeats the offset is still scaled by the instruction's multiplier for TEMP/PA/O/SA operands.
**Uncertain:** Vita3K applies SMBO offsets only through repeat stepping. Hardware probably adds
them to every operand number (repeated or not). If a program uses SMBO ahead of non-repeated
instructions, check this.

---

### 3.18 KILL: discard fragment

Pattern (bit 63 first): `11111001--11000000000pp0000001101111----------------------------`  
Match: `(insn & 0xFF3FF9FFF0000000) == 0xF9300006F0000000`

| Bits | Width | Field |
|---|---|---|
| 63:56 | 8 | fixed `11111001` |
| 55:54 | 2 | (not decoded) |
| 53:43 | 11 | fixed `11000000000` |
| 42:41 | 2 | pred |
| 40:28 | 13 | fixed `0000001101111` |
| 27:0 | 28 | (not decoded) |

Terminates the fragment: no color or depth is written and the fragment is discarded (alpha-test
style). Predicate encoding (insn[42:41]) is Vita3K's **inverted** reading:

| pred | executes (kills) when |
|---|---|
| 0 | always |
| 1 | !p0 |
| 2 | !p1 |
| 3 | p0 |

The matcher passes the raw 2 bits as a ShortPredicate, but Vita3K's control-flow pass uses the
table above, with the comment "opposite of the short predicate". **Uncertain**: check against a
shader whose alpha test is known.

---

### 3.19 LIMM: load 32-bit immediate

Pattern (bit 63 first): `11111100sn10deiiiiiipppmmmmm--tt----uuuuuuuvvvvvvvvvvvvvvvvvvvvv`  
Match: `(insn & 0xFF30000000000000) == 0xFC20000000000000`

| Bits | Width | Field |
|---|---|---|
| 63:56 | 8 | fixed `11111100` |
| 55 | 1 | skipinv |
| 54 | 1 | nosched |
| 53:52 | 2 | fixed `10` |
| 51 | 1 | dest_bank_ext |
| 50 | 1 | end |
| 49:44 | 6 | imm_value_bits26to31 |
| 43:41 | 3 | pred |
| 40:36 | 5 | imm_value_bits21to25 |
| 35:34 | 2 | (not decoded) |
| 33:32 | 2 | dest_bank |
| 31:28 | 4 | (not decoded) |
| 27:21 | 7 | dest_num |
| 20:0 | 21 | imm_value_first_21bits |

`dest = imm` with `imm = imm_value_first_21bits | imm_value_bits21to25 << 21 | imm_value_bits26to31 << 26`.
The 32 raw bits are written to one register (any type, e.g. an F32 constant's bit pattern).
dest: dest table + `dest_bank_ext`, 7-bit, not doubled. Predicate: ExtPredicate in insn[43:41].

---

### 3.20 DEPTHF: depth replace

Pattern (bit 63 first): `11111011s-11recb----npp---tffa--kkddggggggghhhhhhhiiiiiiijjjjjjj`  
Match: `(insn & 0xFF30000000000000) == 0xFB30000000000000`

| Bits | Width | Field |
|---|---|---|
| 63:56 | 8 | fixed `11111011` |
| 55 | 1 | sync |
| 54 | 1 | (not decoded) |
| 53:52 | 2 | fixed `11` |
| 51 | 1 | src0_bank_ext |
| 50 | 1 | end |
| 49 | 1 | src1_bank_ext |
| 48 | 1 | src2_bank_ext |
| 47:44 | 4 | (not decoded) |
| 43 | 1 | nosched |
| 42:41 | 2 | pred |
| 40:38 | 3 | (not decoded) |
| 37 | 1 | two_sided |
| 36:35 | 2 | feedback |
| 34 | 1 | src0_bank |
| 33:32 | 2 | (not decoded) |
| 31:30 | 2 | src1_bank |
| 29:28 | 2 | src2_bank |
| 27:21 | 7 | dest_n |
| 20:14 | 7 | src0_n |
| 13:7 | 7 | src1_n |
| 6:0 | 7 | src2_n |

Writes the fragment's depth: `depth = src0.x` (F32), replacing the interpolated depth for the depth
test/write. src0 uses the src0 table with `src0_bank_ext`, 7-bit, not doubled.

**Uncertain/unimplemented:** `two_sided`, `feedback` (2 bits; perhaps "also perform the depth
test now / feed back the result"), src1, src2 and dest (Vita3K: "figure out how/when to use src1
and src2"). The predicate is ShortPredicate in insn[42:41].

---

### 3.21 SPEC: special (catch-all)

Pattern (bit 63 first): `11111----scc----------------------------------------------------`  
Match: `(insn & 0xF800000000000000) == 0xF800000000000000`

| Bits | Width | Field |
|---|---|---|
| 63:59 | 5 | fixed `11111` |
| 58:55 | 4 | (not decoded) |
| 54 | 1 | special |
| 53:52 | 2 | category |
| 51:0 | 52 | (not decoded) |

Matches every 11111-group encoding not claimed above. Vita3K decodes `special` and `category` and
does nothing. Treat anything here as a no-op, but log unknown encodings.

**Observed in real programs.** A scan of 326 GXP programs (Shovel Knight and PCSA00014) found
exactly three encodings that land in SPEC:

| Word | Where | Interpretation |
|---|---|---|
| `0xFB275000A0200000` | **last instruction of every vertex primary program** (136/136) | end of vertex program / emit vertex (the opcode list has EMITVERTEX, EMITPDS, …). op2 = 011, opcat = 10. Treat as "stop". |
| `0xF920000000000000` | mid-program, right after LD or SMP | wait for outstanding memory/texture results (a data fence; candidates in the opcode list are IDF/WDF). op2 = 001, opcat = 10. No-op in an interpreter. |
| `0xF920080000000000` | same, bit 43 set | same fence, another variant |

Also observed: **PHAS is the first instruction of every primary program** (vertex and fragment),
and appears again mid-program in some fragment programs.

---

### 3.22 LDST: memory load and store

Pattern (bit 63 first): `111oopppsnmycrbakkkkddeetgffihjlqquuvvvvvvvwwwwwwwxxxxxxxzzzzzzz`  
Match: `(insn & 0xE000000000000000) == 0xE000000000000000`

| Bits | Width | Field |
|---|---|---|
| 63:61 | 3 | fixed `111` |
| 60:59 | 2 | op1 |
| 58:56 | 3 | pred |
| 55 | 1 | skipinv |
| 54 | 1 | nosched |
| 53 | 1 | moe_expand |
| 52 | 1 | sync_start |
| 51 | 1 | cache_ext |
| 50 | 1 | src0_bank_ext |
| 49 | 1 | src1_bank_ext |
| 48 | 1 | src2_bank_ext |
| 47:44 | 4 | mask_count |
| 43:42 | 2 | addr_mode |
| 41:40 | 2 | mode |
| 39 | 1 | dest_bank_primattr |
| 38 | 1 | range_enable |
| 37:36 | 2 | data_type |
| 35 | 1 | increment_or_decrement |
| 34 | 1 | src0_bank |
| 33 | 1 | cache_by_pass12 |
| 32 | 1 | drc_sel |
| 31:30 | 2 | src1_bank |
| 29:28 | 2 | src2_bank |
| 27:21 | 7 | dest_n |
| 20:14 | 7 | src0_n |
| 13:7 | 7 | src1_n |
| 6:0 | 7 | src2_n |

| `op1` | Operation |
|---|---|
| 1 (insn[63:59] = 11101) | **LD**: load from memory into registers |
| 2 (insn[63:59] = 11110) | **ST**: store registers to memory |

| `data_type` | element | Mnemonic suffix |
|---|---|---|
| 0 | 32-bit | D (dword) |
| 1 | 16-bit | W (word) |
| 2 | 8-bit | B (byte) |
| 3 | (unmapped in Vita3K; maybe 64-bit Q) | Q? |

**Element count** = `mask_count + 1` (1–16).

**Operands** (all 7-bit, not doubled, read as 32-bit integers):
- src0: base address. src0 table + `src0_bank_ext`. Typically the SA register the driver filled
  with a buffer address (§1.5).
- src1: offset. src1 table + `src1_bank_ext`. If IMMEDIATE, the immediate is **in elements**:
  multiply by the element size.
- src2: for loads, a second offset added to the address. For stores, **the data source register**.
- dest (loads): `dest_n` in TEMP, or PA if `dest_bank_primattr` (SA inside the secondary program).

**Address:**

```
off = src1_is_immediate ? imm * elem_size : src1_value - 0x10000      // see note
if !moe_expand: off += 4
addr = src0_value + off + (is_load ? src2_value : 0)
```

> **Note on the −0x10000 and +4.** Vita3K subtracts 0x10000 (0x1000000 when src0 is the thread
> buffer register) from register-sourced offsets and adds 4 when `moe_expand` = 0. Its authors call
> both guesses fitted to observed shaders ("no absolute proof"). The +4 cancels the usual −4
> `ldst_base_value` bias (§1.5), so `SA_base + 4·k` reads word *k* of the buffer. Implement them
> as stated, keep them behind named constants, and confirm with a hardware memory trace.

**Load:** read `count × elem_size` bytes starting at `addr` and write them consecutively into
registers starting at `dest`. Pack 8/16-bit elements into words in order (2 halves or 4 bytes per
word, low first). Vita3K's fallback reads whole words.

**Store:** repeats `count` times. Iteration *r* stores one 32-bit word (Vita3K) from
`src2 + step(src2, r)` to the address computed with `src0 + step(src0, r)` and
`src1 + step(dest-slot, r)`. SLMSI-mode stepping with multiplier 1 (§2.12/§3.17). Vita3K stores 4
bytes per iteration regardless of `data_type`.

**Special buffers.** When src0 names the SA register of the literal buffer (§1.5), the read comes
from the program's literal data. Texture-buffer reads give texture control words. Thread-buffer
accesses go to per-invocation private memory. In an interpreter with real addresses these are
ordinary memory reads. Only the thread buffer needs a private per-instance allocation.

**Unimplemented in Vita3K:** `addr_mode`, `mode` (addressing modes), `increment_or_decrement` and
`range_enable` (pre/post-increment addressing and bounds checks), `cache_ext`, `cache_by_pass12`,
`drc_sel`. Vita3K's comment lists "post/pre/any increment mode" as missing. If a program depends on
auto-increment, these fields will need hardware investigation.

---

### 3.23 Illegal encodings

Pattern (bit 63 first): `10110-----------------------------------------------------------`  
Match: `(insn & 0xF800000000000000) == 0xB000000000000000`

| Bits | Width | Field |
|---|---|---|
| 63:59 | 5 | fixed `10110` |
| 58:0 | 59 | (not decoded) |

op1 = 10110, 10111, 11000, 11011 are illegal (Vita3K logs an error and skips the instruction).
Same layout for ILLEGAL23/24/27, differing only in the op1 bits.

---

## 4. Program outputs

This section covers where a program leaves its results, and in what order.

### 4.1 Vertex program: position and varyings

The vertex program writes its results to the **O bank** as packed F32 words. The order is fixed,
and an output occupies registers only if the program has it (§1.7):

| Order | Output | Words | Notes |
|---|---|---|---|
| 1 | POSITION | 4 (x, y, z, w) | **always O0–O3**; always present |
| 2 | COLOR0 | 4 | if `vertex_outputs1` bit 11 |
| 3 | COLOR1 | 4 | if bit 10 |
| 4 | FOG | 2 | if bit 9 |
| 5–14 | TEXCOORD0–9 | per §1.7 group: 2·b0 + b1 + b2 | if group ≠ 0 |
| 15 | PSIZE | 1 | if bit 8 |
| – | CLIP0–7 | ? | position in the sequence unknown (Vita3K does not implement clip outputs) |

Each output starts right after the previous **present** one. Vita3K advances the O cursor by the
counts above but reads 4 words for every output except PSIZE, so it is not sensitive to the exact
count. **Uncertain:** whether colors or texcoords with `texcoord_pack_format` = F16 occupy fewer
words (packed halves). Vita3K reads all outputs as F32. A shader-dump comparison will settle it:
look at which O registers the program's last writes target.

O registers are zeroed before each vertex.

**Position semantics:** O0–O3 is a **clip-space** position. Hardware applies perspective divide
and the viewport transform from `sceGxmSetViewport` (x/y scale + offset; z: `z_ndc*scale + offset`,
clamped at 0). When the context's viewport is disabled (`SCE_GXM_VIEWPORT_DISABLED`), the position
is already in **screen space** (pixels). Vita3K then maps x,y to NDC with
`(2/width, −2/height)` and offset `(−1, +1)`, and sets z = w. Depth range on Vita is [0, 1].

**Fragment-side linkage:** the iterator descriptor of the fragment program (§1.12) names which
vertex output (TEXCOORDn, COLORn, FOG, POSITION) feeds each PA slot. Interpolation is
perspective-correct (centroid if flagged). Colors may be delivered as U8 (§1.12).

### 4.2 Fragment program: color and depth

**Color register:**
- `program_flags.NATIVECOLOR_USED` set → color is in **O0** onward.
- Otherwise → color is in **PA0** onward. If `output_param_type` = 1 (F16), the start is
  `PA[fragment_output_start]` instead of PA0 (rare; Vita3K logs "non-zero PA offset").

**Color format:** `output_param_type` (§1.3 code) and `output_comp_count` from the varyings block
(§1.7). Read `output_comp_count` components of that type from the color register, packed per §2.5:
- U8 ×4 (the common case): one word RGBA (R in byte 0).
- F16 ×4: two words.
- F32: one word per component.
- U32/S32: the program has already packed the color itself. Take the raw bits as the surface
  format's bits (Vita3K reinterprets them as F32 "raw content").
- If the color surface is F32F32 and `output_comp_count` > 2, F16 outputs are really F32. This is
  a quirk Vita3K handles for Assassin's Creed III.

The value is then converted to the render target's `SceGxmColorFormat` and blended by the
fixed-function blender (`SceGxmBlendInfo`), unless the program does its own blending.

**Programmable blending:** with `program_flags.FRAGCOLOR_USED`, the current framebuffer color at
this pixel is **loaded into O0** (in the output format) before the primary program runs. The
program blends itself and writes the result to the color register.

**Depth:** written only by a DEPTHF instruction (§3.20). Otherwise the interpolated depth is used.

**Discard:** KILL (§3.18) stops the fragment and writes nothing.

**sRGB:** gamma correction of the output (when the surface is sRGB) is applied after the program,
as part of the output path, not by shader code.

---

## 5. Gotchas

The mistakes most likely to bite an implementation.

1. **Two different register units.** Float vector instructions count registers in 64-bit pairs
   (field × 2). Integer, SOP, LDST, LIMM and SMP-dest count single 32-bit words. Mixing them up is
   the most likely interpreter bug. VPCK even mixes both in one instruction: the dest is in words,
   a float src1 in pairs.
2. **TEMP top encodings are internal registers** (§2.3 step 2). `r60`–`r63` in a 6-bit doubled
   field (r120–r127 after doubling) are i0–i3, not temporaries.
3. **VMAD2 and F32/F16 VMOV write at most 64 bits** (2×F32 or 4×F16) because of the write-mask
   rewriting of §2.9. VMAD/VDP/VNMAD use their 4-bit masks as given (Vita3K). A vec4 F32 result
   from VMAD2 therefore takes two instructions.
4. **F16 lives packed.** F16 vectors are two halves per word (x in the low half). FPINTERNAL
   registers hold F16 values widened to F32: an F16-typed write of component c lands as the F32 in
   word 4k+c, an F16-typed read converts the word back. Moving F16 data through internal registers
   converts it. This holds for EVERY instruction, not just VPCK's wide-source case: Killzone's text
   shader writes i0 with an F16 VNMAD and reads i0.x with an F32 VCOMP (2026-10-07; packed as
   halves, every glyph's alpha came out -3.6).
5. **SA is per-draw and shared;** never write SA from a primary program. The secondary program is
   the only writer after the driver's setup (§1.9–1.10).
6. **Uniforms may be in SA or only in memory.** Register-resident windows come from containers.
   Everything else is LDST with the base address from the buffer-info table, plus the
   −4 bias / +4 `moe_expand` pairing.
7. **Textures are named by SA register number** (SMP src1), and that SA window holds the actual
   texture descriptor (four words). Texture *units* only matter for filling the window
   (dependent samplers) and for non-dependent reads in the iterator table.
8. **Non-dependent texture reads happen before the program runs.** A fragment program that only
   samples `tex2D(s, texcoord)` may contain **no SMP at all**: the texel is already in a PA register
   (§1.12). Implement iterators before trusting any output.
9. **Derivatives** (VDSX/VDSY/FDSX/FDSY, implicit-LOD sampling) need 2×2 fragment quads.
   A one-fragment-at-a-time interpreter must either run quads in lock-step or approximate
   (e.g. LOD 0). `skipinv` marks instructions helper instances may skip.
10. **Repeat stepping state persists.** SMLSI/SMBO change `inc[][]` for all following
    SLMSI-mode instructions until reprogrammed. Reset at program start (Vita3K resets per program;
    whether hardware resets between the secondary and primary programs is unknown).
11. **VDP rotates its mask on repeat**, writing successive components with one instruction.
12. **Predicated instructions do nothing** when false, including their repeats. SOP3 has a known
    Vita3K hack (Resogun) where a SOP3 alone in a predicated block was executed unconditionally.
    This suggests SOP3's 2-bit field at the predicate position may not be a predicate.
    **Uncertain.**
13. **LOG/EXP base** (§3.6): Vita3K uses natural log/exp. Hardware is probably base 2.
14. **Constant swizzle channels** (0, 1, 2, ½) and the **FPCONSTANT** ROM (§2.7) provide literals
    without SA registers. Expect them in compiler output, e.g. `xyz1` swizzles for homogeneous
    vectors.
15. **Uncharted quirk:** `g_mSkinTransforms` is declared with 96 vec4s but indexed up to 147. Do not
    bounds-check uniform buffer reads to the declared size.
16. **Thread buffer** = per-thread private memory (`thread_buffer_count` bytes for the whole GPU,
    Vita3K assumes 16-way split). Give each instance its own copy.
17. **The `end` bit** marks the last instruction. Execution also ends when the PC reaches
    `instr_count`. Some programs contain padding after `end`.

---

## 6. Gaps and uncertain areas

Fields/instructions with **no semantics available from Vita3K** (decoded but ignored, or stubbed):

| Area | Item |
|---|---|
| Control flow | BR `exception`, `pwait`, `sync_ext`, `br_monitor`, `save_link` (calls inferred structurally), `any_inst`, `all_inst`, `syncend`. PHAS entirely. Return encoding only known from the analyser. |
| Repeat | SMLSI `temp_limit`, `pa_limit`, `sa_limit`. VTST/VTSTMSK `rpt_count`, `onceonly`. VBW repeats. |
| Predicates | `pN` meaning. ExtVecPredicate `!p2`. VDUAL `sv_pred` application. |
| Tests | `test_crcomb_and`. VTSTMSK with `test_wben = 0`. |
| ALU | VDP clip-plane output. VLOG/VEXP base (natural in Vita3K). VNMAD write-mask rewriting (not applied by Vita3K). |
| Integer | I8MAD `cmod*`, `amod*`, `saturated` (and its lane renormalization). I8MAD2 entirely. I16MAD `or_shift`, `abs`, `src2_neg`, formats 2/3, saturation. I32MAD `is_sat`, src2 types 0/1/3. I32MAD2 `sn` ≠ 0. VBW op1 = 110, RLP. |
| SOP | alpha-saturate selectors, "scale" and "minus half" selectors, `asrc1_mod`, `dest_mod`, SOP3 `dmod`. |
| Formats | O8 entirely. C10 bit layout (assumed 0/10/20). |
| Texture | SMP `mask_count`, `fconv_type` = 1, `sb_mode` = 2 with LOD modes, 3-D gradient layout, 1-D. Texture-buffer indexing beyond `sb_mode=0, lod_mode=2`. |
| Memory | LDST `addr_mode`, `mode`, `increment_or_decrement`, `range_enable`, cache bits, `data_type` = 3. The −0x10000 / +4 adjustments are fitted guesses. |
| Depth | DEPTHF `two_sided`, `feedback`, src1/src2/dest. |
| Other | SPEC entirely. GLOBAL bank (Vita3K reads as 0). INDEXED number split. Secondary-program bank remapping. SMBO scope (repeat-only vs all operands). |

How to handle these: implement the confirmed paths, and make every uncertain rule a named function that can be
switched. Log each unimplemented field value the first time it's met, with the instruction word and the program's
hash. Settle open points against the hardware: run shaders on a real Vita (through vagentd) with known inputs and
compare register dumps, the way the ARM recompiler is verified.

---

## 7. Interpreter skeleton (informative)

A sketch of the interpreter's main loop, as written before `usse.c` existed. The real one lives there.

```c
typedef struct {
    uint32_t r[128];     // TEMP
    uint32_t pa[128];    // PRIMATTR
    uint32_t o[128];     // OUTPUT
    float    i[4][4];    // FPINTERNAL (128-bit each)
    uint8_t  p[4];       // predicates
    uint32_t idx[2];     // index registers
    int8_t   inc[4][17]; // repeat increments (slot: src0,src1,src2,dest)
    bool     killed;
} Inst;                  // per vertex/fragment instance
typedef struct { uint32_t sa[128]; } Draw; // shared, per program per draw

void run(const uint64_t *code, uint32_t n, Inst *t, Draw *d, bool secondary) {
    reset_inc(t);                       // inc[s][r] = r
    for (uint32_t pc = 0; pc < n && !t->killed; ) {
        uint64_t w = code[pc];
        const Op *op = decode(w);       // first-match table, §3.0
        if (!pred_true(t, op)) { pc++; continue; }
        if (op->is_branch) { pc = branch_target(pc, w); continue; }
        int reps = op->repeat_count + 1;
        for (int r = 0; r < reps; r++)
            exec_iteration(op, t, d, r, secondary);   // operand numbers += step(slot, r)
        pc++;
    }
}
```

Operand access goes through one `read(bank, num, type, swizzle, mask)` / `write(...)` pair that
implements §2.3–§2.6 (doubling, internal aliasing, packing, constants, modifiers), so each
instruction body only does arithmetic. Unit-test the decoder against Vita3K-style disassembly of
real GXP files (Shovel Knight's and Uncharted's shaders are in their game dumps) before running
anything.
