"""PC-side decryption for PS Vita content: the same approach Vita3K uses (the
community-known SCE keys it ships), so a user can bring their own dumped/legit
game and firmware instead of needing a hacked console.

Submodules:
  keys       -- the SCE key store (data only).
  sce_self   -- SCE SELF (.self/.suprx/.skprx, NPDRM eboot.bin) -> plain ELF.
  rif        -- work.bin/.rif parsing; klicensee/content-id extraction.
  sfo        -- param.sfo parser.
  pkg        -- PKG (+ licence file) -> a NoNpDrm-style extracted folder.
  pfs        -- PFS-protected folder (files.db/unicv.db) -> plain files.
  game       -- one entry point: PKG/NoNpDrm folder/decrypted folder -> app0 tree + game.json.
"""
