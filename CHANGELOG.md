# Changelog

## Unreleased

### New

- **Retail firmware 3.60**, as well as 3.65. Engram accepts 3.60's SceShell (NID `0x0552F692`). Place fonts also check that ScePaf is 3.60's (`0xCD679177`) or 3.65's (`0x73F90499`), not only its segment sizes: the two builds' segments are the same size, and every offset Engram reads in Paf is the same in both.

## 1.6

### New

- **Engram Installer** (`engram_installer.vpk`): an app that installs, updates and uninstalls Engram for you. Install copies `engram.suprx` to `ur0:tai` and adds Engram's lines under `*main`, `*NPXS10015` and `*NPXS10016` in the `config.txt` taiHEN uses, keeping the old file beside it as `config.txt.engram-installer-old`. It can also add Poppins and Righteous, the two fonts its own text is in, to Engram's fonts folder. **Check for a new version** asks GitHub for the latest release, and when there's a newer one, downloads its installer, checks it against the SHA-256 GitHub lists and has the Vita install it over the one running. That goes through a small updater app (an Engram Updater bubble that the new installer deletes again), as an app can't replace its own files.

- **My other projects**, in the installer: the author's other plugins and apps for the Vita, Tweaks/Plugins and CyberFlow Launcher, each with photos and what it does. **Download** lists a project's releases with their notes, and installs the newest one; an older release can be read about and opened on GitHub. Both go in through the Vita's own installer: CyberFlow with a switch for its data files (422 MB), which are unpacked to `ux0:data/RetroFlow`, and Tweaks/Plugins as its own installer app, which then puts the plugin in and keeps it up to date. Everything downloaded is checked against the SHA-256 GitHub lists for it. The photos, releases and notes are kept in `ux0:data/engram_installer`, so they show at once the next time, and with no network.
- The installer's enter and back buttons are the Vita's own: O and X on a Japanese console, X and O elsewhere, or as HENkaku Settings has them.
- The installer says when the Engram installed isn't the one it carries, so an update isn't missed.
- The installer has a version of its own, 1.0, beside Engram's, and its main page says who Engram is by.
- **Install or update Engram** could leave the old plugin in place on a Vita that wouldn't let the file be renamed over or removed. It now writes over the file where it is when that happens, and a copy that still fails shows the system's error code and puts the old plugin back.

### Improved

- **Fonts > Apps sets itself up:** giving an app a font adds Engram's line under that app in taiHEN's `config.txt`, and setting it back to System Font takes it out. Engram keeps a copy from before each change (`config.txt.engram-old`) and the original (`ur0:data/engram/tai_config_original.txt`).
- **Place fonts with the Live Area on System Font:** the lock screen, status bar, boot warning and quick menu can have fonts of their own while the rest of the home screen keeps the system font. In this setup Chinese text on the home screen uses the Japanese font's kanji, and Korean letters show as blanks.
- Place fonts go in once a boot, and places on the same font share one copy of it. Fonts up to 128 KB are read into the home screen's memory; bigger ones are opened from their files instead, which saves memory but holds the file open.
- The Settings page can show under `*NPXS10016` too, the title ID Settings has when the system or another app opens it.
- A **Logging** switch in Settings.

### Changed

- An app with no font chosen keeps the system font. Choose Same as Live Area for it to follow the Live Area font, as it did in 1.5 by default.

### Fixed

- With a Live Area font set, Photos, Music and Camera wouldn't start (#3). The home screen no longer opened the system's shared fonts, which those apps rely on; Engram now opens them.
- After a quarter of an hour or so, place fonts could stop changing and the log grew by megabytes: each new font object left a system font open until Paf's font library was full.
- With the Live Area on System Font, a place's font could come out blank.
- A place whose font couldn't be opened showed one of the system's decorative fonts (the quick menu in a script font). It now keeps the Live Area font, or the system font.

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
