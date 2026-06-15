# thcrap-menu

A small Windows GUI launcher for choosing a thcrap configuration with a mouse,
keyboard, or gamepad. It was written for Steam Deck / Proton use, where a
console menu is awkward from a controller.

## Usage

Put `thcrap_menu.exe` in your thcrap directory, next to
`thcrap_loader.exe`, then run:

```text
thcrap_menu.exe <game_id> <config_prefix>
```

Example:

```text
thcrap_menu.exe th06 th06;
```

The launcher searches:

```text
.\config\<config_prefix>*.js
```

It strips `<config_prefix>` and `.js` in the menu, but passes the full config
filename to `thcrap_loader.exe`.

For example, `thcrap_menu.exe th06 th06;` will show
`config\th06;en.js` as `en`, then launch:

```text
thcrap_loader.exe "th06;en.js" "th06"
```

*Note:* Using `thcrap.exe`, you can avoid generating shortcuts and instead rely
on this menu. But after downloading a new config, you'll need to rename it with
your desired prefix for this menu to detect it.

For example, if your thcrap config is currently:

```text
config\en.js
```

rename it to:

```text
config\th06;en.js
```

Then launch this menu with:

```text
thcrap_menu.exe th06 th06;
```

If you want a config to appear in more than one game's menu, copy it and use a
different prefix for each game.

## Controls

- Arrow keys / D-pad: move selection
- Enter / A: launch selected config
- Double-click: launch selected config
- Escape / B: close the menu

## Windows Shortcuts

1. Right-click `thcrap_menu.exe` and choose **Create shortcut**.
2. Right-click the shortcut and choose **Properties**.
3. Add game ID and desired prefix to the end of **Target**:

   ```text
   "C:\path\to\thcrap\thcrap_menu.exe" "th06" "th06;"
   ```

4. Keep **Start in** in your thcrap directory:

   ```text
   C:\path\to\thcrap
   ```

5. Rename the shortcut to something friendly, such as `Touhou 6`.

## Steam Deck / Steam

These steps are easiest from Desktop Mode.

1. Copy `thcrap_menu.exe` into your thcrap directory, next to
   `thcrap_loader.exe`.
2. Rename your configs in `config` so they have the prefix you want, such as
   `th06;en.js`, `th06;en-alt.js`, and so on.
3. In Steam, choose **Add a Game** -> **Add a Non-Steam Game**.
4. Browse to and select `thcrap_menu.exe`.
5. Open the shortcut's **Properties** in Steam.
6. Set **Target** to the full path to `thcrap_menu.exe`, for example:

   ```text
   "/home/deck/Games/thcrap/thcrap_menu.exe"
   ```

7. Set **Start In** to your thcrap directory, for example:

   ```text
   "/home/deck/Games/thcrap"
   ```

8. Set **Launch Options** to the game ID and prefix:

```text
th06 th06;
```

9. Open **Compatibility** and check **Force the use of a specific Steam Play
   compatibility tool**.
10. Pick your desired Proton version. (I use ProtonGE version 10.)

After that, return to Gaming Mode and launch the shortcut from Steam. The D-pad,
A button, and B button should work directly through Steam Input / XInput.

The executable sets its working directory to its own directory as a fallback,
but setting **Start In** correctly is still recommended.

## Build

If you'd like to build the executable yourself, then with MinGW:

```sh
gcc -std=c11 -O2 -Wall -Wextra -mwindows -o thcrap_menu.exe thcrap_menu.c -lshell32 -lgdi32
```

The source also builds with MSVC. GitHub Actions is configured to build a
Windows artifact on pushes and pull requests.

## XInput

Gamepad support uses XInput, but the project does not require `xinput.h`, an
XInput import library, or a bundled XInput DLL. It dynamically loads one of:

- `xinput1_4.dll`
- `xinput1_3.dll`
- `xinput9_1_0.dll`

If XInput is unavailable, mouse and keyboard controls still work.

## Known Issue

Steam may continue to show the game as running after all visible
windows are closed. This appears to be Steam/Proton tracking a child thcrap or
game process rather than the menu window itself.
