# Engram

Engram is a set of tweaks for the PS Vita's home screen, set in **Settings → System → Engram**. Its settings are grouped by category. So far there is one, **Live Area**, with two tweaks:

- **Scroll Animations**, a faithful port of [Cylinder](https://github.com/rweichler/cylinder) for iOS. The icons move as you scroll between pages. Effects can be combined, or picked at random for each scroll.
- **Motion Depth**, inspired by ShadowBoard for iOS. Tilt the Vita and the icons float above the wallpaper in 3D.

More categories, such as a Lock Screen one, will sit next to Live Area.

```text
Settings > System > Engram
  Live Area
    Scroll Animations   switch, Effects, Randomize, Fade Out, Intensity, 3D Icon Tilt
    Motion Depth        switch, Style, Strength
  Credits
```

## Credits

- **Cylinder** for iOS: Reed Weichler ([rweichler](https://github.com/rweichler/cylinder)).
- **ShadowBoard** for iOS, the idea behind Motion Depth.
- **PS Vita port**: badmanwazzy37.
- The Settings page follows the recipe of HENkaku's settings and [Turbo Storage](https://github.com/juanjsebgarcia/vita-turbo-storage) (MIT).

The same credits appear at the bottom of the Engram page in Settings, as the small description text of a Credits row.

Retail firmware 3.65 only, because it checks SceShell's module NID.

## Scroll Animations

The Vita's home screen scrolls vertically, so each effect is Cylinder's idea turned 90 degrees.

| Effect | What it does |
|---|---|
| Stay Put | Icons hold still and fade out while the page slides away |
| Stairs | Columns leave one after another, left to right |
| Cascade | Icons leave one by one in reading order |
| Wave | A ripple runs across the rows as the page moves |
| Chomp | The page splits down the middle and the halves slide apart |
| Vortex | Icons swirl into the middle of the screen and vanish |
| Scatter | Icons fly apart in every direction and fade |
| Cube (Outside) | Pages are the faces of a turning cube, seen from outside. On by default |
| Cube (Inside) | Pages are the walls of a turning cube, seen from inside |
| Hinge | The page swings away on its top or bottom edge |
| Card Flip | The page flips over like a card to show the next one |
| Blinds | Each icon flips in place like a slat of a blind |
| Spin | Every icon turns one full circle per page |
| Shrink | Icons shrink to nothing as their page leaves |
| Page Fade | The page fades out as it leaves and in as it arrives |

When several are on, they're applied in the order above. For example, Spin and Shrink together give a spinning shrink, and Cube (Outside) with Page Fade gives a fading cube.

| Setting | Default | |
|---|---|---|
| Scroll Animations | On | This tweak's switch (called Icon Animations before 1.1) |
| Effects | Cube (Outside) | One toggle per effect |
| Randomize | Off | Each scroll uses one of the effects you've turned on, picked at random |
| Intensity | Normal (100%) | 50% to 150%: how far the effects move, turn and fade the icons |
| 3D Icon Tilt | On | The 3D effects turn the icons themselves. Turn it off to flatten them instead, if tilted icons look wrong |
| Fade Out | During the Swipe | When the page you swipe away from fades out. **During the Swipe**: gone by 60% of the way. **As the Next Page Lands**: fully visible until 60% of the way, then gone as the next page settles. **Off**: no fade. The incoming page always stays fully visible, and it works with no effects turned on too |

## Motion Depth

The icons act as a layer floating above the wallpaper. Turn the Vita and they respond to the turn. Hold it still at any angle and they drift back to the middle over a second or two, the way iOS's parallax does.

| Setting | Default | |
|---|---|---|
| Motion Depth | On | This tweak's switch |
| Style | Float and Turn | **Float**: the icons drift against the wallpaper, towards the edge you tilt away. **Turn to Face You**: each icon turns back against the Vita's turn, so it keeps facing you. **Float and Turn**: both |
| Strength | Normal | Subtle, Normal, Strong or Extreme. At Normal the icons float up to about 40 px and turn up to about 25° |

It works with Scroll Animations, including mid-scroll. When the Vita stops turning and the icons are back in the middle, Engram puts every bubble back exactly as SceShell had it.

It reads the gyroscope through `sceMotionGetState`. SceShell already loads SceMotion, and Paf imports it. If sampling isn't running, Engram starts it, and stops it again when Motion Depth goes off.

### Shelved: Shadows and Wallpaper Parallax (1.7)

Neither worked on the console in 1.2 to 1.6, so 1.7 turns both off with `ENGRAM_SHADOWS_SHELVED` in `engram.h` and leaves them out of Settings. The code is all still there; set the flag to 0 to bring them back. Their old config keys (`shadows`, `wallpaper`, `shadow_layer`, `shadow_depth`) are still read and written, and do nothing while shelved.

What was learned:

- **Shadows were always drawn over the icons.** The public `Widget::SetBehind` only queues a reorder. `SetBehind_core` reorders straight away and did put the shadow layer before the bubble container (1.4's log: layer child 1, container child 2), but the shadows still showed on top. Paf's draw objects have a depth test (`graph::DrawStyle::depth`), and in Cube (Outside) icons hide behind each other mid-turn, so depth is clearly in play. 1.5 pushed the shadows 16 px back in z, and 1.6 offered six orders and z offsets (Settings > Shadow Layering, A to F). None of them put the shadows behind, going by the user's report; no 1.6 log was fetched. Next to try: read the shadow plane's and a bubble's `DrawStyle` (blend and depth) through `GetDrawObj`, or make the shadows draw objects of the bubble itself.
- **The wallpaper never moved.** The plane `0xF1B811BF` inside each page exists but is hidden with a custom photo theme. 1.4's log showed a widget beside the pages' scroller (`37677DEF`, 13 children for 13 pages, drawn before them), which 1.5 and 1.6 moved, but nothing moved on screen. 1.6's log ruled it out: its first child (`C69A0237`, 960×512, 3 children) is hidden, and its second is a 138×138 widget, not a page-sized one, so it isn't one photo per page. The photo wallpaper's widget is still to be found; it may not be a widget at all, like the default wave. 1.6's log also confirms each arrangement put the layer where intended (A, B and E before the container; C, D and F after it), so it's the depth test and not the order that keeps the shadows on top.

| Setting (shelved) | Default | |
|---|---|---|
| Shadows | On | A round shadow with a crisp edge under each icon, about the icon's size, as in ShadowBoard. It stays down on the wallpaper, 10 px below the icon as if lit from above, and slides with the wallpaper when there's one that moves. So tilting opens a gap between an icon and its shadow, and the icons look like they hover. Works with Motion Depth off too, as still shadows |
| Wallpaper Parallax | On | The wallpaper zooms in 10% and slides the other way as you tilt, half as far as the icons float, as on iOS. This works with a custom theme's wallpaper. 1.3 and 1.4 looked for it in the plane `0xF1B811BF` in each page, which is there but hidden. 1.4's log showed a widget beside the pages' scroller, drawn before them, with one child per page (`37677DEF` on the test console), so 1.5 moves the page's child of that, found by walking up from the page. The Vita's own wave background isn't a widget, because Paf's graphics code draws it (see psp2wpp), so it stays still |

ShadowBoard itself draws a drop shadow behind each icon that moves as you tilt (by the accelerometer, with no settings).

How the shadows are made (1.2):

- **The texture** is a 64×64 circle that Engram draws itself, solid out to 88% of its radius and then fading over the rest, built once with `graph::Surface`'s constructor and `Copy`, and never freed. Vita icons are square images that SceShell shows as circles, so a tinted copy of the icon would have made square shadows. Every byte of a pixel is the same, so the planes are tinted black with `SetColor(0, 0, 0, a)` whatever byte order the format uses.
- **The widgets:** each page on screen gets one layer of Engram's own, a size-0 `plane` made with `Plugin::CreateWidget` from topmenu_plugin's plain `style_plane` (`0x03C69492`). It sits beside the bubble container, not in it, in case SceShell counts the container's children, and just before it in the child list, because Paf draws children in order. Each bubble gets one 132×132 plane in that layer.
- **The draw order:** the public `Widget::SetBehind` doesn't move anything itself. Paf's code shows it posting `{parent, widget, target}` to a request list and returning 0, which is why 1.2 and 1.3 saw the order unchanged and the shadows on top. 1.4 calls `SetBehind_core` (NID `0xD6FB21CB`), which does the move there and then through the parent's `ReorderChild`, and checks the order every frame. That worked (the layer became child 1, the container child 2), but the shadows still showed on top: Paf's draw objects have a depth test (`graph::DrawStyle::depth`), and drawn first at the icons' own z, the shadows made the icons fail it. 1.5 also puts the shadows `shadow_depth` px further back (16 by default). That's a config key with no Settings entry, and it's reread every second, so it can be tuned live in `ur0:data/engram/config.txt`.
- **No stored pointers:** each frame Engram finds the layers and shadows again by their own made-up ids (`0x45475348`, `0x45475353`), under the pages on screen. When SceShell frees a page, its shadows go with it.
- Shadows fade out as soon as a page starts to scroll, and stay hidden while SceShell is animating a bubble (`FindAnim`) and for 1.5 s after the home page appears.
- `IDParam` is filled in by hand: its constructor would import Paf's shared empty string, a variable that a plugin loading before Paf can't rely on.

## Settings file

Settings are saved to `ur0:data/engram/config.txt`, and the home screen picks up changes within a second, with no restart needed.

## Install

1. Copy `build\engram.suprx` to `ur0:tai/engram.suprx`.
2. Add it to `ur0:tai/config.txt` in **two** places:
   ```text
   *main
   ur0:tai/engram.suprx

   *NPXS10015
   ur0:tai/engram.suprx
   ```
   `*main` is the home screen. `*NPXS10015` is the Settings app, and that line adds the settings page.
3. Reboot.

If `shelltheme` is also in `*main`, its page covers the native home screen, so you won't see Engram. Test Engram with shelltheme off: create `ur0:data/shelltheme/disabled`, or remove its line.

## Safety

- **Crash guard:** `ur0:data/engram/booting` exists from boot until Engram has run for 30 seconds. If a boot dies before then, the next boot writes `ur0:data/engram/disabled` and Engram stays off. Turning **Scroll Animations** or **Motion Depth** on in Settings deletes that file. Reboot afterwards.
- **Hold L at boot** to skip Engram for that boot. This also skips shelltheme.
- With the pages and the Vita both still, Engram changes nothing. It only touches bubbles on pages that are mid-scroll, remembers how each one looked first, and puts every one back exactly when its page settles. If SceShell moves a bubble itself mid-scroll (folders, recreated bubbles), Engram takes SceShell's value as the new resting state instead of fighting it.
- Every Settings hook is an import hook that passes through anything that isn't Engram's. If something doesn't match, the page simply doesn't appear.

Log: `ur0:data/engram/log.txt`.

## How it works

The probe found that the home screen (`page_top` in `topmenu_plugin`) has a scroll box (`0x0E61B084`). Its first child is a plane that moves as you scroll, and the pages hang off that plane 512 px apart. So each frame, a page's offset from the screen is `plane.y + page.y`, and its progress is that offset divided by 512. Engram registers a `MainThreadCallList` callback, which Paf runs every frame on its main thread. The callback works out each page's progress, then sets position, scale, rotation and alpha on that page's 138×138 bubbles in container `0xC6063E76`.

The 3D effects (cubes, hinge, card) turn the page about a horizontal axis, then project it with a perspective of 1000 px. Each icon's position and scale come from that projection, and with tilt on, the icon is also rotated about X.

The Settings page uses the same recipe as HENkaku and Turbo Storage ([vita-turbo-storage](https://github.com/juanjsebgarcia/vita-turbo-storage), MIT). It hooks SceSettings' `sceKernelLoadStartModule` and waits for `system_settings_core`. Then it splices a `setting_list` into the System page's XML, backs the rows with a private `/CONFIG/ENGRAM` registry namespace that reads and writes the config file, and resolves the row text through `scePafToplevelGetText`.

## Build

You need [VitaSDK](https://vitasdk.org) and Paf's C++ headers from [ScePaf-RE](https://github.com/GrapheneCt/ScePaf-RE), which aren't included here. Engram was built against a lightly modified copy of them (its `paf/math/math.h` defines the vector classes), so the current upstream may need small changes.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPAF_HEADERS_DIR=/path/to/ScePaf-RE/include
cmake --build build
```

The plugin is `build/engram.suprx`.

## Status

**Works on the console** (2026-11-06, retail 3.65, LiveArea NoLimits with 13 pages). The Settings page appears under System, toggles apply within a second, and both cube effects animate. Each scroll animates the 20 bubbles on the two pages on screen.

Findings from getting there:

- **Register the frame callback from SceShell's main thread.** Calling `MainThreadCallList::Register` from our own thread, as soon as Paf's stubs resolved, crashed SceShell early on every boot. Registering from SceShell's `sceAVConfigRegisterCallback` import, as shelltheme does, is stable.
- **The scroll box appears about 90 frames after `page_top`.** Engram has to keep looking for it rather than look once.
- **Rotation is in radians.** The widget kept exactly the value Engram set, and the cube looks right.
- **Never read alpha back from a bubble.** The Paf headers' `m_color` always read fully opaque, even right after `SetColor`, so Engram never saw that it had faded anything and never restored it. That is why Card Flip, Blinds and a Fade Out Previous Page option left icons invisible, while Page Fade (which ends each swipe almost fully opaque) looked fine. Engram now remembers the alpha it set, always sets alpha back to full when a page settles, and treats bubbles as untinted white. Blinds and Fade Out Previous Page were removed before this was found, and are back now that it's fixed.
- **Restore scale and rotation to SceShell's own values.** Some bubbles (four on the test console, likely folders) rest at 98% scale. A build that only took scale once it was exactly 1, and handed it back to SceShell whenever it changed, never animated those bubbles, so only some icons turned in 3D. SceShell didn't write scale, rotation or alpha mid-scroll in any logged session.

As of 2026-11-07 all 15 effects and Fade Out Previous Page work on the console.

**1.1 (2026-11-07):** the Live Area category and Motion Depth. The first console run showed:

- The gyro works. `sceMotionGetState` reports angular velocity in rad/s (about 1 to 2 rad/s for a quick turn), and SceShell already has sampling running.
- A bubble has no child widgets: it draws its icon itself. So a drop shadow needs a new widget behind each bubble.
- **Don't set a property SceShell is animating.** Setting a property without a timer replaces that property's running animation (each property has its own anim id: `ANIM_MOVE`, `ANIM_SCALE` and so on). Motion Depth started moving the first page's bubbles while SceShell was still bringing them in after boot. That left most of them invisible and one stranded near the top of the screen. Now a page that is only tilting skips any bubble on which `Widget::FindAnim` finds one of SceShell's animations, and Motion Depth waits 1.5 s after the home page appears.

What the log still has to confirm:

- `motion: sampling started` or `was already running`, and a few `motion: gyro ...` lines. These confirm the gyro's units (assumed rad/s) and which way each axis turns. If the icons move the wrong way, the sign of that axis flips in `depth_apply`.
- `wallpaper: ...`, once: whether the page has plane `0xF1B811BF` and whether it's shown, then each page's widgets and the ones beside the pages, to find where the wallpaper really is.
- `shadows: ...`, the texture, the first shadow, and the draw-order calls. In 1.2, `layer->SetBehind(container)` left the shadows drawn on top of the icons. 1.3 tried all four `SetBehind`/`SetForward` combinations: each returned 0 and left the order as it was. 1.4 logs `SetBehind_core: ...` with the order after the move, which should show the layer before the container. If anything fails four times, shadows give up for that boot.
- `wallpaper ...`: the wallpaper plane Engram found and whether it was visible.
- `home page visible=...`, to learn whether SceShell hides the home page while an app is in front. If it does, Motion Depth can stop sampling then, to save battery.

Not checked yet:

1. **Scale origin:** does Shrink shrink icons about their centres, or drift towards a corner?
2. Edge cases: dragging an icon to another page in edit mode, opening a folder mid-scroll, and pulling past the first or last page.
