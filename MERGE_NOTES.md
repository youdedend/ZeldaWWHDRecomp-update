# Merge notes: upstream v0.2.8 into this fork (`port-0.2.8`)

Base: this fork's `android` branch at `1afeabf`. Upstream range:
`v0.2.7..v0.2.8` of ZeldaWWHDRecomp/ZeldaWWHDRecomp (27 commits). All
Android-relevant changes are in; the skips below say why. Work branch:
`port-0.2.8` in this repo.

## Upstream commits, one by one

| Upstream | Subject | Here |
|---|---|---|
| 56ea39a | Language sources: text/fonts/layouts from EUR/JPN game | SKIP — needs the desktop installer, overlay Language tab and game_lang system; this fork runs EUR/JPN games natively instead (see below) |
| a152986 | swkbd: WWHD_SWKBD_TEXT as UTF-8 | PORTED (`bf3d74b`) |
| b844821 | Mod manager: fan translations as content mods | SKIP — no mod manager in this fork (mods/ holds gameplay mods only); translations install as replaced pack files (docs/rtl-text.md) |
| 53840af | Language sources: EUR test, German genitive | SKIP — part of 56ea39a |
| 9fa6698 | Quick doors: deletion out of the extra steps (#61) | PORTED (`b9163ce`): queue addresses as release::Data; test as macOS tool exe (no ctest here) |
| 710f04b | Recompiler barrier on loop back-edges (#62) | SUPERSEDED — this tree already emits `PPC_LOOP_HEAD()` (same barrier) at every loop head |
| 121b20a | Right-to-left text for Arabic/Hebrew translations | PORTED (`90b8b37`): shaping core + hooks verbatim; font reader subset; pack matched by file name; hooks embed for the on-device recompiler |
| 107208e | rtl_text test stub on Windows | SKIP — the touched test files don't exist here |
| 65493ec | Issue template: save file, not states | SKIP — .github is out of scope for this merge |
| 5440d7a | macOS: closing TV window quits (#65) | SKIP — macOS-only |
| 8cfcd62 | Buffer cache default on desktop Linux | SKIP — the cache doesn't exist in this renderer; upstream keeps Android opt-in too |
| bca6888 | Shadow maps keep 1024x1024 (#67) | PORTED (`42cd279`): Vulkan already behaved so (arrays never scale); Metal follows upstream exactly |
| ea45a3d | Blur taps keep console footprint (#66) | PORTED (`42cd279`): area_sample.h + decompiler option + both renderers; no Cemu-pack branch (no packs here) |
| cbe5a89 | Gyro: axis modes, sensitivity, Pro mode, dropouts (#45, #71) | ADAPTED (`ed8b6f7`): diagnostics + NaN drop + WWHD_GYRO_LOG; the virtual-GamePad stack doesn't apply (this tree feeds fused motion Cemu-style; Pro gyro already works via gyro_pro.cpp) |
| 963f8aa | gyro.md: in-game Gyroscope option in Pro mode | PORTED into docs/gyro.md |
| ac81cff | Vulkan: draw unblittable depth copies (#72) | ADAPTED (`d47e834` + `ed8b6f7`): clear instead of draw — no temp-pass infrastructure at the copy sites; deterministic and self-healing |
| 81e57e0 | Scaled copies never abort; plain same-size copy (#72) | PORTED: same-size already used CopyImage; scaled path clears via the helpers above |
| 7c038ea | Shadow maps scale by default; FIX keeps 1024 | PORTED (`42cd279`): Metal exactly; Vulkan keeps 1024 with WWHD_SHADOW_SCALE=n opt-in |
| 4eff63e | Portable save state format + guards | PORTED (`7cc5cdb`): format verbatim, capture/apply release-mapped |
| ba4285d | Portable states by default | ADAPTED (`7cc5cdb`): full snapshots stay the default (fork UX); slots load the newer file of either kind |
| b92d6b9 | Docs, issue template, wwstate.py | PORTED (`7cc5cdb`, `b585994`): docs + wwstate.py; template skipped (.github out of scope) |
| db30347 | Portable states: boat, control of Link, notices | PORTED (`7cc5cdb`) |
| aa8828e | Portable scenario cases + docs | PORTED (`7cc5cdb`) |
| 1c58359 | wwstate.py: boat fields | PORTED (`b585994`) |
| 95beced | Portable scenario: Vulkan shader cache | SKIP — the scenario test tool wasn't ported |
| 760910e | Portable states: not on a rope | PORTED (`7cc5cdb`) |
| d77512f | README: What's new in v0.2.8 | Fork README extended instead (this commit): portable-state bullet + RTL bullet |

## Decisions worth knowing

- **Language sources vs native multi-release.** Upstream runs the USA executable with
  foreign text packs. This fork runs the EUR/JPN executables themselves, with the
  player's own languages, fonts and layouts — the fuller solution. Nothing from 56ea39a
  / 53840af / b844821 is needed here; the installer/overlay/mod-manager parts would be
  thousands of lines for a worse outcome.
- **Unblittable copies clear.** Upstream draws depth/stencil copies through a pipeline
  where blits don't exist. This classic-RenderPass renderer has no throwaway-pass
  plumbing at the copy sites, so such copies clear (depth 1, stencil/colour 0) and log
  once per format. The game re-renders depth every frame, so a cleared copy heals on
  the next frame. `WWHD_VK_DEPTH_COPY=none` forces the path for depth.
- **Vulkan shadows stay 1024.** Arrays never took the resolution scale in this
  renderer, which is upstream's fixed behaviour. `WWHD_SHADOW_SCALE=n` (restart)
  scales them for whoever prefers the sharper upstream-default look.
- **Host tests without ctest.** This tree builds test executables on macOS but never
  registered them with ctest; `turbo_steps_test` and `rtl_text_test` follow that
  convention (`-UNDEBUG`, no `add_test`).

## Verification (host)

- `turbo_steps_test`, `rtl_text_test`, an `area_sample::rewrite` harness: all pass.
- `g++ -fsyntax-only` passes for: motion, savestate, portable_state, turbo,
  padscore, system_stubs, hle/fs, game_font, rtl_text_hooks.
- strings.xml is valid XML and every `R.string.*` referenced from the touched Java
  resolves; brace balance checked on every touched C++/Java file.
- The Vulkan and Metal renderer files can't compile on this host (volk from the NDK,
  Metal from Xcode) — reviewed by diff; hunks mirror adjacent code. Java isn't
  compiled either (no Android SDK) — reviewed the same way.
- Full-branch diff was reviewed file by file; two tooling-corrupted tails and three
  silently dropped hunks were found that way and repaired (see `ed8b6f7`).

## Needs a device

Nothing here has run on hardware yet. Suggested order: build the APK, boot the game,
exercise a door event with quick doors, change resolution/aspect on an Adreno device
(the #72 paths; force with `WWHD_VK_DEPTH_COPY=none`), save/share a portable state,
aim with the gyro (`WWHD_GYRO_LOG=1`), and — with a fan translation installed —
`WWHD_RTL=1` on USA plus one EUR/JPN run. The EUR/JPN RTL hooks land through the
address map (positions verified); if text misbehaves there, `WWHD_RTL=0` isolates it.
