# Patches to the vendored LVGL submodule

`lvgl_ui_recovered/src/lvgl` tracks **upstream** `lvgl/lvgl`, so local fixes
cannot be committed into it — and a fresh `git clone --recursive` would silently
come up without them. Every patch freetoon needs against LVGL therefore lives
here, and the build applies them automatically.

`make` (any target) depends on the `lvgl-patches` target, which applies each
`patches/lvgl-*.patch` to the submodule if it isn't applied already. It is
idempotent — re-running does nothing — and it fails the build loudly rather than
compiling an unpatched tree.

To apply them by hand:

    make -C lvgl_ui_recovered/src lvgl-patches

## The patches

- **`lvgl-0001-gifdec-accept-gif-without-global-color-table.patch`**
  LVGL's GIF decoder rejects any GIF with no Global Color Table. Buienradar's
  radar animation is exactly that — no GCT, every frame carrying its own Local
  Color Table — so without this the radar image on the weather screen fails to
  decode and never renders. The patch accepts the no-GCT case, leaves the GCT
  empty, and (importantly) skips the GCT byte read so it doesn't consume bytes
  belonging to the first frame block.

- **`lvgl-0002-font-euro-and-superscript3-glyphs.patch`**
  Adds the Euro sign (€) and superscript 3 (³, for m³) to the built-in
  Montserrat fonts. Without it every price and gas volume shows a missing
  glyph.

- **`lvgl-0003-draw-sw-transform-alpha8-with-recolor.patch`**
  The software renderer could not rotate or scale `LV_IMG_CF_ALPHA_8BIT`
  images with recolor; the animation overlay (leaves, hearts) depends on it.

0002 and 0003 were first committed inside the submodule itself (2026-11),
which made the superproject point at a commit that exists only on one PC: a
fresh clone could not build. Never commit inside the submodule; add a patch
here instead.

## Adding a patch

    cd lvgl_ui_recovered/src/lvgl
    # ...edit...
    git diff src/path/to/file.c > ../../../patches/lvgl-000N-what-it-does.patch

Keep the `git diff` format (paths relative to the submodule root) — that's what
`git -C lvgl apply` expects.

The submodule is marked `ignore = dirty` in `.gitmodules`, so the applied
patches do not show up as uncommitted changes. Do not commit inside the
submodule; that is what made fresh clones unbuildable.
