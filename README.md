# autodj

Console player for a folder of MP3 files. Songs crossfade into each other. A USB keypad or an XP-Pen ACK05 Shortcut Remote pauses, skips, replays an earlier song, rewinds a few seconds, and sets the volume. The volume is stored in the config file and restored on the next start.

## Compile

This builds on 64-bit Raspberry Pi OS. The Makefile links these shared libraries:

- `/usr/lib/aarch64-linux-gnu/libmpg123.so.0`
- `/usr/lib/aarch64-linux-gnu/libsoxr.so.0`
- `/usr/lib/aarch64-linux-gnu/libasound.so.2`

Install the compiler and those libraries:

```
sudo apt install build-essential libmpg123-0 libsoxr0 libasound2
```

The mpg123, soxr, and ALSA headers used at compile time are in `third_party/include`. `make` also generates `src/keytab.inc` from `/usr/include/linux/input-event-codes.h`.

```
make
```

That produces `./autodj` in the project directory. `make clean` removes the binary, object files, and the generated key table.

If the libraries are installed somewhere else, change `LDLIBS` in the Makefile.

## Install

```
make install
```

This copies the binary to `/home/pi/bin/autodj`. `PREFIX` in the Makefile is that directory. Add it to `PATH` if it is not already there.

```
./build.sh
```

This does the same build and install. Before that, it installs `udev/70-autodj-remote.rules` to `/etc/udev/rules.d` with `sudo`, then reloads udev. It does this only when that file is missing or different, so it asks for the password only the first time. The rule gives the `input` group read and write access to the shortcut remote's `/dev/hidraw*` devices, which autodj needs for the center button.

## Run

```
autodj
autodj --check
autodj --config
autodj --calibrate
autodj /path/to/master.cfg
```

With no config path, autodj reads `~/.config/autodj/master.cfg`.

`--check` loads the playlist and runs the built-in tests, including a grab of the keypad. It does not start playback.

`--config` opens a full-screen editor for that file. The folder is a text field. Bool settings are checkboxes. Space or Enter switches `Dial starts in` between `volume` and `jog`. Key bindings show the current key name. The other settings are text fields. Tab, Shift-Tab, and the arrow keys move between fields. Left and right move the cursor inside a text field, and leave the field at either end. Enter, Space, or a click on a key binding asks for a new key. The keypad and the shortcut remote are read first, then any other keyboard. A key held with Ctrl, Shift, Alt, or Meta is captured as a combination such as `CTRL+KEY_Z`. Esc or Cancel leaves that binding unchanged. A key already used by another action opens an error and keeps the old binding. The mouse selects a field or a button. Save checks the values and writes the file. A problem opens a message with an OK button and leaves the file unchanged. Cancel and Esc close the editor without writing. A player that is already running keeps its current settings until the next start, and it keeps the keypad and remote grabbed, so the editor reads them after playback stops.

`--calibrate` opens a full-screen view of the `[Jog]` dial settings, with a short note beside each one about what it changes in the sound. Calibrate measures the dial in two steps. First, turn the dial exactly one full turn and press Enter; the click count becomes `clicks_per_turn`. Then spin the dial as fast as you can; timing starts at the first click and runs for 3 seconds. `max_speed` becomes the fastest measured speed plus 25%. Test plays a random song from `Folder`, so you can turn the dial and hear the result. A change to a setting takes effect in the test at once. Stop test ends playback. Save writes only the `[Jog]` section. Close or Esc leaves without writing; with unsaved changes, it asks you to press Close again. The view needs the remote, so stop a running autodj first.

The keypad device must be named `USB Keypad` and must report `KEY_KP0`. The XP-Pen ACK05 Shortcut Remote (USB `28bd:0202`, shown as `Hanvon Ugee Shortcut Remote Keyboard`) is also used. The button in the center of its dial sends nothing in the remote's normal mode, so autodj switches the remote to raw mode on its vendor HID channel (`/dev/hidraw*`) and reads every button and the dial there. In raw mode the buttons have the names `REMOTE_BTN1` to `REMOTE_BTN10` and `REMOTE_CENTER`, and the dial has `REMOTE_DIAL_CW` and `REMOTE_DIAL_CCW`. The remote stays in raw mode after autodj exits, and its factory keyboard shortcuts do not work until it is unplugged and plugged back in. If the raw channel cannot be opened, autodj logs a warning to run `./build.sh` and keeps using the factory shortcuts, such as `CTRL+KEY_Z`, `KEY_SPACE`, or a bare `KEY_LEFTCTRL`, with the dial sending `CTRL+KEY_KPPLUS` and `CTRL+KEY_KPMINUS`. It tries to open the channel again every 3 seconds. Both devices can be connected at once. autodj grabs them so the keys are not also typed into the console, and Ctrl+Z from the remote does not suspend the shell. The process needs read access to `/dev/input/event*`, which on Raspberry Pi OS means the user is in the `input` group. If neither device is found, playback still runs.

Stop autodj with Ctrl-C or SIGTERM.

## Config file

Create the directory, then create the file:

```
mkdir -p ~/.config/autodj
```

Save this as `~/.config/autodj/master.cfg` and change `Folder` to the directory that holds the music:

```
[Main]
Folder=/home/pi/Music
shuffle=true
overlap=8
trim_silence=true
pause_time=3
resume_time=3
volume=100
loudness=true
loudness_lufs=-18
dial_default=volume

[Keys]
pause_resume=KEY_KP0
skip=KEY_KPDOT
previous=KEY_KPSLASH
rewind_0_5=KEY_KP1
rewind_1=KEY_KP2
rewind_1_5=KEY_KP3
volume_up=KEY_KPPLUS
volume_dn=KEY_KPMINUS
dial_forward=REMOTE_DIAL_CW
dial_back=REMOTE_DIAL_CCW
dial_mode=REMOTE_BTN4

[Jog]
seconds_per_turn=1.8
clicks_per_turn=20
max_speed=4
spinup_time=0.3
grab_time=0.02
smoothing=0.01
```

Blank lines are ignored. A line that starts with `#` or `;` is a comment. Setting names are not case sensitive.

`Folder` is required. Every other setting can be left out. A missing setting keeps the default listed below. If `volume` is missing, autodj adds `volume=100` under `[Main]` the first time it starts.

Only `.mp3` files in `Folder` are played. Files in subfolders are not.

### [Main]

`Folder`  
Directory of MP3 files. Required.

`shuffle`  
`true` or `false`. Also accepts `yes`/`no`, `on`/`off`, and `1`/`0`. Default `true`. The list is shuffled again each time it wraps. With more than one track, the same file is not played twice in a row.

`overlap`  
Seconds. Default `10`. The last N seconds of the current song play together with the first N seconds of the next song. The outgoing song fades down while the incoming song fades up. Skip and previous start that same crossfade immediately.

`trim_silence`  
`true` or `false`. Default `true`. Leading and trailing silence is skipped so the crossfade lands on the music.

`pause_time`  
Seconds. Default `5`. Pause slows the current song to a stop over this many seconds. The pitch falls with the tempo.

`resume_time`  
Seconds. Default `5`. Resume brings the song back up to full speed over this many seconds.

`volume`  
Percent from 0 to 100. Default `100`. The volume keys change it in steps of 5. Each change is written back into this file, so the next boot starts at the last volume.

`loudness`  
`true` or `false`. Default `true`. Each file is measured from start to finish and then played at one steady gain so tracks match each other. This is a level change only. It does not compress the song.

`loudness_lufs`  
Target integrated loudness, from `-30` to `-5`. Default `-18`, which is the ReplayGain 2.0 reference. A quieter track is turned up and a louder track is turned down. The gain is held back if it would clip the loudest sample.

`dial_default`  
`volume` or `jog`. Default `volume`. What the shortcut remote's dial does when autodj starts. `dial_mode` switches it while autodj runs.

### [Keys]

Each value is a `KEY_*` name from `linux/input-event-codes.h`, for example `KEY_KP0`. The name can have `CTRL+`, `SHIFT+`, `ALT+`, or `META+` in front of it, for example `CTRL+SHIFT+KEY_Z`. These prefixes are not case-sensitive. Left and right modifiers are treated the same. The shortcut remote's raw-mode names can't take a prefix:

| Name | Button |
|---|---|
| `REMOTE_BTN1` to `REMOTE_BTN3` | Top row, left to right |
| `REMOTE_BTN4` | Tall button on the right |
| `REMOTE_BTN5` to `REMOTE_BTN7` | Middle row, left to right |
| `REMOTE_BTN8` to `REMOTE_BTN10` | Bottom row, left to right |
| `REMOTE_CENTER` | Button in the center of the dial |
| `REMOTE_DIAL_CW`, `REMOTE_DIAL_CCW` | Dial clockwise, counterclockwise |

 A modifier key on its own, such as `KEY_LEFTCTRL`, fires when it is released, but only if no other key was pressed while it was held. `previous` defaults to `KEY_KPSLASH`, `rewind_0_5` to `KEY_KP1`, `rewind_1` to `KEY_KP2`, and `rewind_1_5` to `KEY_KP3` when that line is missing and the key is not already used. Any other action that is left out is unbound. Pressing a key that is not bound logs the key name and does nothing. `KEY_NUMLOCK` from the keypad is ignored. A press within 30 ms of the previous press is ignored, except for dial clicks. Holding a volume key still repeats.

`pause_resume`  
Press once to pause. Press again to resume.

`skip`  
Press to skip to the next track. While a song is playing, skip starts the overlap crossfade. If the music is paused, or still slowing to a pause, skip moves to the next track and resumes playback. A full stop cuts straight to the next track, then brings it back up to speed over `resume_time`.

`previous`  
Press to replay a song that already started. It uses the same crossfade, pause, and full-stop rules as skip. Up to 20 earlier songs are kept. Another press past that does nothing.

`rewind_0_5`  
Press to jump back 0.5 seconds inside the current song. The position stops at the start of the song. Holding the key does not repeat.

`rewind_1`  
Press to jump back 1 second. The position stops at the start of the song.

`rewind_1_5`  
Press to jump back 1.5 seconds. The position stops at the start of the song.

`volume_up`  
Press or hold to raise the volume. The name `brake_delayvolume_up` is accepted as this same action.

`volume_dn`  
Press or hold to lower the volume. `volume_down` is the same action.

`dial_forward`, `dial_back`  
The shortcut remote's dial: `REMOTE_DIAL_CW` and `REMOTE_DIAL_CCW` in raw mode, or `CTRL+KEY_KPPLUS` and `CTRL+KEY_KPMINUS` with the factory shortcuts. The dial works like a hand on a record. With the default `[Jog]` settings, each click moves the song 90 ms, so one turn of 20 clicks moves it 1.8 seconds, which is one turn of a 33⅓ RPM record. The time between clicks sets the speed. A slow turn plays slowly, a fast spin plays up to 4 times as fast, and turning back plays the song backward. While the dial turns, normal playback is held. When the dial stops, the song spins back up to normal speed within 0.3 seconds. See `[Jog]` to change these numbers. If the music is paused, the dial still moves the song and you can hear it, but the song stays paused when the dial stops. The position stops at the start of the song.

The old names `jog_forward` and `jog_back` are still read. `autodj --config` writes them back under the new names.

`dial_mode`  
Press to switch the dial between jog and volume. In volume mode each click of the dial raises or lowers the volume by one step of 5, the same as a volume key. autodj starts in the mode set by `dial_default`, and the log shows the mode after each switch.


### [Jog]

How the dial moves the song. `autodj --calibrate` edits these settings and plays a song so you can hear each change. A value out of range is logged and the default is used.

`seconds_per_turn`  
Song time moved by one full turn of the dial, from 0.1 to 20. Default `1.8`. A larger value moves further and plays faster for the same hand speed.

`clicks_per_turn`  
Dial clicks in one full turn, a whole number from 1 to 1000. Default `20`. Calibrate counts it.

`max_speed`  
Fastest scratch, as a multiple of normal speed, from 0.5 to 20. Default `4`. Calibrate sets it 25% above your fastest spin. A lower value keeps a hard spin from racing through the song.

`spinup_time`  
Seconds for the song to return to normal speed after the dial stops, from 0 to 5. Default `0.3`. A larger value sounds like a heavier platter.

`grab_time`  
Seconds for normal playback to stop when the dial starts to turn, from 0 to 1. Default `0.02`. `0` is instant.

`smoothing`  
Seconds for the speed to follow the hand, from 0 to 1. Default `0.01`. A larger value smooths out the steps between clicks, and a smaller value follows the dial more sharply.
