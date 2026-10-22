# Grim Dawn VR

An unofficial mod that plays Grim Dawn in true stereo VR: the world in 3D around you, your head
looking around, the controllers as a laser pointer, mouse and keyboard.

Not affiliated with or endorsed by Crate Entertainment. Free for non-commercial use
(PolyForm Noncommercial 1.0.0, see `LICENSE.md`).

## You need

- Grim Dawn on Steam (64-bit; the expansions are optional), set to **DirectX 11** in the game's options
- SteamVR, set as the OpenXR runtime (SteamVR settings, Developer, "Set SteamVR as OpenXR Runtime")
- A PC VR headset. Tested on Valve Index; other controllers work through SteamVR's bindings

## Install

**With the installer** (`GrimDawnVR-Setup-<version>.exe`, recommended):

1. Run it. No administrator rights are needed; it installs to `%LOCALAPPDATA%\Programs\GrimDawnVR` unless
   you pick another folder.
2. Leave **Add "Grim Dawn VR" to my Steam library** ticked. Steam has to be closed for that step; the
   installer offers to close it for you. The entry shows up with your VR games.
3. Optional: **Add Windows Defender exclusions** (asks for administrator permission; see below).
4. In Steam, open the Properties of **Grim Dawn VR**, **Controller**, and set Steam Input to **Disabled**.
   (Otherwise Steam turns the VR controllers into a desktop gamepad and keyboard on top of the mod, and
   buttons misfire.)
5. Start "Grim Dawn VR" from Steam. The first start takes a little longer.

**From the zip** instead: unzip the folder anywhere, then in Steam **Games > Add a Non-Steam Game to My
Library > Browse**, pick `gdvr_launch.exe`, and do step 4 above.

### What the launcher does

- Puts one file, `dinput8.dll`, into the game's `x64` folder. It stays dormant when you start Grim Dawn
  normally; only the VR launcher turns the mod on.
- Backs up your saves to `%LOCALAPPDATA%\gdvr\save_backups` (the last 10).
- Keeps separate game settings for VR (windowed 2560x1440, no depth of field, no camera shake...):
  `options_vr.txt` is swapped in while VR runs and your normal `options.txt` is put back when you quit.
- Starts SteamVR, then the game.

Normal Grim Dawn from Steam keeps working as before, with your own settings.

## Controls (Valve Index)

| Control | Does |
|---|---|
| Laser (right hand) | The mouse pointer: on the ground, enemies, items and menus |
| Right trigger | Left click (walk, attack, pick up, press buttons) |
| Left trigger | Right click (skill) |
| Left stick | Walk |
| A / B (right), A / B (left) | Hotbar 1-4 |
| Right stick flick up / right / down / left | Hotbar 5-8 |
| Grips | Health potion (left), energy potion (right) |
| Left stick click / right stick click | Swap weapons / Y; both together: recentre the view |
| Left touch pad press | Inventory (hold: map) |
| Right touch pad press | Game menu (hold: character) |
| Right touch pad slide up / down | Zoom in / out |
| Left touch pad slide | Turn the camera |

Assign skills to the hotbar the usual way: point at a hotbar slot and pull the right trigger.

Keyboard: **F8** settings window, **F9** VR mode (off, flat screen, stereo), **F10** recentre,
**F7** fade at the edge of the play area, **F11** save screenshots of both eyes.

## Recording and streaming

The game window on your monitor shows what you see in the headset: the left eye, cropped to a normal
field of view, with the HUD over it. Record or stream that window with any tool (OBS "Game Capture" or
"Window Capture", Steam recording, ShadowPlay...). In the **F8** window, **Desktop mirror** turns it off
and sets its field of view.

## Updates

When you start Grim Dawn VR, the launcher checks for a newer release at most once a day and asks before
doing anything: **Yes** downloads the installer from GitHub, checks it against its published SHA-256
checksum, updates in place (your settings stay) and starts the game again; **No** plays your current
version; **Cancel** stops asking about that version. To turn the check off, put this in
`%LOCALAPPDATA%\gdvr\launcher.ini`:

```ini
[update]
check=0
```

You can always update by hand: download the newest setup from the Releases page and run it over the
installed version.

## Settings

Everything is in the **F8** window (HUD distance and size, labels, sky, controls...). **Save settings**
writes them to `%LOCALAPPDATA%\gdvr\control.ini`; delete that file to go back to the defaults.

The sky behind the world is `skybox\skybox.png` (an equirectangular 2:1 image, 4096x2048 works well). To use
your own, put `backdrop.png` or `backdrop.jpg` in `%LOCALAPPDATA%\gdvr\sky`. The Devotion constellations
are drawn from your game's own files and light up as you complete them.

## Uninstall

Installed with the installer: Windows **Settings > Apps > Grim Dawn VR > Uninstall**. It removes the
`dinput8.dll` from the game folder, puts your normal game settings back, removes the Steam entry (close Steam
when it asks), removes the Defender exclusions if the installer added them, and asks whether to keep your
mod settings and save backups.

From the zip, by hand:

1. Delete `dinput8.dll` from the game's `x64` folder
   (`...\steamapps\common\Grim Dawn\x64`).
2. Delete this folder and the Steam entry.
3. Optional: `%LOCALAPPDATA%\gdvr` (settings, logs, save backups) and `options_vr.txt` in
   `Documents\My Games\Grim Dawn\Settings`.

## Windows Defender / antivirus

The mod hooks into the game's rendering and input, which antivirus tools sometimes flag as suspicious
("Behavior:Win32/..." or a "!ml" detection), especially for a new release nobody has run yet. The files are
built from the public source code. If Defender quarantines a file or blocks the launcher, exclude these
two folders (and only these):

| Folder | What is in it |
|---|---|
| The Grim Dawn VR folder (where `gdvr_launch.exe` is) | The launcher, the mod, the skybox |
| `%LOCALAPPDATA%\gdvr` | Settings and logs; the mod runs from a copy in its `run` folder |

The `dinput8.dll` the launcher puts in the game's `x64` folder is normally left alone. If Defender
removes it too, also exclude that one file (`...\steamapps\common\Grim Dawn\x64\dinput8.dll`).

**With the Windows Security app:** Virus & threat protection, Manage settings, Exclusions, Add or remove
exclusions, Add an exclusion, Folder, then pick the folder. Repeat for the second folder.

**Or in PowerShell** (right-click Start, "Terminal (Admin)"), with your own Grim Dawn VR folder:

```powershell
Add-MpPreference -ExclusionPath "C:\Games\GrimDawnVR"
Add-MpPreference -ExclusionPath "$env:LOCALAPPDATA\gdvr"
```

If a file was already quarantined: Virus & threat protection, Protection history, open the entry,
Actions, Restore. Then start Grim Dawn VR again.

## Troubleshooting

- **Buttons open the Windows menu, keyboard stuck**: a key-mapping tool for SteamVR (for example
  OpenVR2Key) is turning controller buttons into real keys. Close it while playing.
- **Game shows flat on the monitor only**: check that SteamVR is the OpenXR runtime and the headset is on;
  press F9 to cycle the VR mode.
- **Logs** for bug reports: `%LOCALAPPDATA%\gdvr\gdvr.log` and `launcher.log`.
