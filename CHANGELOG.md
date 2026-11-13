# Changelog

## 1.5

The second public release.

### New

- **Fonts**, a new category. Put `.ttf`, `.otf` or `.pvf` fonts in `ur0:data/engram/fonts` and choose:
  - **Live Area**: the whole home screen's font, the Live Area and its app pages included.
  - **Lock Screen**, **Status Bar & Notifications**, **Boot Warning** and **Quick Menu**: a font of their own each, or Same as Live Area. Letters a place's font doesn't have come from the Live Area font. The quick menu's buttons and toggles are covered, and so are Quick Menu Plus's Restart and Power Off and VitaTweaks' CustomWarning text.
  - **Apps**: a font for each of 17 system apps, once Engram's line is under the app in `config.txt`.
  - Fonts over 1 MB are marked "(too big)" and left out on the home screen, which has little memory to spare. Apps take bigger fonts.
- **Icon Placement** (Live Area): Open Book, Sphere and Inverted Sphere layouts for the home screen's icons, with Strength, and their own layout for open folders.
- **App Page Animation** (Live Area): the scroll animations for the apps' LiveArea pages as you swipe between them: Cube (Inside), Spin, Shrink or Random, with Fade Out, Fade In and Intensity.
- **The Engram switch**, at the top of the Engram page: on while Engram is running, off after the failsafe has turned it off. Turn it on to try again, or off to stop Engram.

### Improved

- Engram rests while an app is open: Motion Depth stops reading the motion sensor and nothing else runs, to save battery.
- The failsafe turns Engram off only after two boots in a row that crashed within 30 seconds, so a quick restart doesn't trip it.
- The log starts again past 256 KB, keeping the previous one as `log.old.txt`.

### Fixed

- Motion Depth didn't respond on Vitas where nothing else had started the motion sensor: its deadband swallowed slow turns. It now works with or without reVitaMotion (#1).
- Icons could be left tilted at the wrong angles after opening and closing folders with Icon Placement and scroll effects on.
- Other plugins' Settings entries (Turbo Storage's) could disappear next to Engram's. Engram now adds its rows after theirs when listed straight after `henkaku.suprx`, and a smaller page otherwise.
- Long descriptions in Settings were cut off after 255 characters.
- Possibly fixed: the Settings app not responding to touch after using App Page Animation.
- The README has a Troubleshooting section, including what to do when Engram's Settings page is missing (#2).

### Shelved

- A second try at Wallpaper Parallax, with Shadows.

## 1.0

The first public release: Live Area with Scroll Animations (a port of Cylinder for iOS, 15 effects) and Motion Depth (inspired by ShadowBoard for iOS).
