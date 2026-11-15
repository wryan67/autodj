# autodj

Console player for a folder of MP3 files. Songs crossfade into each other. A USB keypad, an XP-Pen ACK05 Shortcut Remote, or a MIDI controller such as the Akai MPD218 pauses, skips, replays an earlier song, rewinds a few seconds, and turns nine effects on or off: reverb, chipmunk, autopan, phaser, chorus, flanger, underwater, lasers, and bubbles. One more button turns all of them off. The shortcut remote's dial sets the volume, or jogs the song, and separate keys or knobs can do either one on their own. The song volume, the laser volume, and the bubbles volume are stored in the config file and restored on the next start. Every effect is off when autodj starts.

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
autodj smooth bad
autodj --check
autodj --config
autodj --config /path/to/master.cfg
autodj --calibrate
autodj --sfx /home/pi/Music/autodj/sfx
```

autodj always reads `~/.config/autodj/master.cfg`. Only `--config` takes another path, to edit that file instead.

Names after `autodj` are songs to play first. Each one is the start of a file name in `Folder`, and letter case does not matter, so `smooth` and `SMOO` both match `Smooth Criminal.mp3`. A track number at the start of a file name, such as `001-` in `001-smooth-criminal.mp3`, can be left out, so `smooth` matches that file too. A name can match several songs, and they all play, in alphabetical order. The songs play in the order the names are given, and a song is not repeated when two names match it. A folder in front of a name is ignored, so a tab-completed path works. After the last of them, playback continues from the saved list in `~/.local/state/autodj/songs.cache`. Those command-line songs are moved to the bottom of the saved list, keeping their order, and the next song is one that was not named. A name that matches nothing is logged and skipped. A name in quotes can contain spaces, such as `autodj "baby sh"`. Put `--` before a name that starts with a dash. `--check` and `--calibrate` take no names.

`--check` loads the playlist and runs the built-in tests, including a grab of the keypad. It does not start playback, and it leaves `songs.cache` unchanged.

`--sfx folder` is the sound-effects folder. It defaults to `/home/pi/Music/autodj/sfx`. Bubbles loops an mp3 from that folder, `bubbles.mp3` unless `[Bubbles]` names another file. The folder is not written into the config. `--sfx` can be used with song names, `--config`, `--check`, and `--calibrate`.

`--config` opens a full-screen editor for that file. The editor has twelve tabs: Main, Buttons, Knobs, Reverb, Chipmunk, Autopan, Phaser, Chorus, Flanger, Underwater, Lasers, and Bubbles. Lasers and Bubbles wrap onto a second row even in a wide window, and more tabs wrap when the window is narrower. Click a tab, or move up to the tab row and press Left or Right. Main holds the folder, the crossfade, and loudness. Buttons holds the key bindings for previous, pause/resume, skip, the three rewinds, one button each for effect 1 (reverb), effect 2 (chipmunk), effect 3 (autopan), effect 4 (phaser), effect 5 (chorus), effect 6 (flanger), effect 7 (underwater), effect 8 (lasers), and effect 9 (bubbles), plus All effects off. Knobs holds Dial forward, Dial back, Next dial mode, Volume up, Volume down, Jog up, Jog down, Reverb up, Reverb down, Lasers up, Lasers down, Bubbles up, Bubbles down, Chorus up, and Chorus down, then Dial mode 1, Dial mode 2, and Dial mode 3. Each binding row on Buttons and Knobs has three boxes: Keyboard, Keypad, then MIDI. Each box is only as wide as its title or the longest value in that column on that tab. The other nine tabs hold the settings of those effects. The Bubbles tab has File and Volume. File is a droplist of the mp3 files in the sound-effects folder when that folder has any, shown with a `v` at the right, and it works the same way as a dial mode. An empty folder leaves File as a text field. Bool settings are checkboxes. Each dial mode is a droplist of `none`, `volume`, `jog`, and `reverb`, shown with a `v` at the right: Enter, Space, or a click opens it, Up and Down move, Enter, Space, or a click picks, and Esc closes it unchanged. Key bindings show the current key name. The other settings are text fields. Tab, Shift-Tab, and the arrow keys move between fields on the current tab. Left and right move the cursor inside a text field, and leave the field at either end. Enter, Space, or a click on a Keypad box asks for the USB keypad or the shortcut remote. The same on a MIDI box asks for a pad press or a knob turn on any MIDI controller, and the prompt names the controllers it is listening to. A knob turned clockwise is stored as `CC_<n>_UP` and counterclockwise as `CC_<n>_DOWN`, so turn it the way that row should go. The same on a Keyboard box asks for the next key typed on this terminal: a character such as `p`, or Space, Enter, Tab, Backspace, Delete, an arrow, Home, or End. `p` and `P` are different keys. The Keyboard column starts with Space, Right, Left, 1, 2, 3, q, w, e, r, t, y, u, i, b, and Tab on Buttons, and Up, Down, Enter, `=`, `-`, `.`, `,`, `]`, `[`, `o`, `p`, `k`, `l`, `n`, and `m` on Knobs. The Keypad and MIDI columns start blank. On a binding row, Left and Right move between the three boxes and stop at the edge. Up and Down stay in the same column. Tab moves from the keyboard box to the keypad box to the MIDI box, then to the next row. A key held with Ctrl, Shift, Alt, or Meta on the keypad or remote is captured as a combination such as `CTRL+KEY_Z`. Esc or Cancel leaves either binding unchanged. Delete on the keypad or MIDI prompt removes that binding. Left and Right there move between Delete and Cancel, and Enter or Space presses the highlighted button. On the keyboard prompt those keys are bindings, so click Delete to clear it. A key already used in the same column, on either tab, opens an error and keeps the old binding. The same action can have a keyboard key, a keypad key, and a MIDI control at once. The mouse selects a field, a tab, or a button. Save checks the values and writes the file. A problem opens a message with an OK button and leaves the file unchanged. Cancel and Esc close the editor without writing. A player that is already running keeps its current settings until the next start, and it keeps the keypad, remote, and MIDI controllers open, so the editor reads them after playback stops. The effect buttons are the exception: they take effect while a song is playing.

`--calibrate` opens a full-screen view with two tabs, Keypad and MPD218. It opens on the Keypad tab. Left and Right on that row switch tabs. Up from the first field returns to the tabs, and Down from the tabs enters the first field. A click on a tab selects it and focuses its first field. The Keypad tab is the shortcut remote dial and the USB keypad, and it edits `[Jog]`. The MPD218 tab edits the jog knob and writes `[Jog MPD218]`. Volume, reverb, lasers, bubbles, and chorus knobs keep the steps they already have. Each tab shows the six jog settings, with a short note beside each one about what it changes in the sound. Calibrate measures the control on the current tab in two steps. First, turn it exactly one full turn and press Enter; the click count becomes `clicks_per_turn`. Then spin it as fast as you can; timing starts at the first click and runs for 3 seconds. `max_speed` becomes the fastest measured speed plus 25%. Switching tabs cancels a measurement in progress. Test plays a random song from `Folder`, so you can turn the dial or the jog knob and hear the result. That shuffle is separate and leaves `songs.cache` unchanged. A change on a tab takes effect in the test at once. Stop test ends playback. Save writes the current tab's section. Close or Esc leaves without writing; with unsaved changes on the current tab, it asks you to press Close again. Stop a running autodj first. When `[Jog MPD218]` is missing, the jog knob uses `[Jog]` until the MPD218 tab is saved.

The keypad device must be named `USB Keypad` and must report `KEY_KP0`. The XP-Pen ACK05 Shortcut Remote (USB `28bd:0202`, shown as `Hanvon Ugee Shortcut Remote Keyboard`) is also used. The button in the center of its dial sends nothing in the remote's normal mode, so autodj switches the remote to raw mode on its vendor HID channel (`/dev/hidraw*`) and reads every button and the dial there. In raw mode the buttons have the names `REMOTE_BTN1` to `REMOTE_BTN10` and `REMOTE_CENTER`, and the dial has `REMOTE_DIAL_CW` and `REMOTE_DIAL_CCW`. The remote stays in raw mode after autodj exits, and its factory keyboard shortcuts do not work until it is unplugged and plugged back in. If the raw channel cannot be opened, autodj logs a warning to run `./build.sh` and keeps using the factory shortcuts, such as `CTRL+KEY_Z`, `KEY_SPACE`, or a bare `KEY_LEFTCTRL`, with the dial sending `CTRL+KEY_KPPLUS` and `CTRL+KEY_KPMINUS`. It tries to open the channel again every 3 seconds. Both devices can be connected at once. autodj grabs them so the keys are not also typed into the console, and Ctrl+Z from the remote does not suspend the shell. The process needs read access to `/dev/input/event*`, which on Raspberry Pi OS means the user is in the `input` group. If neither device is found, playback still runs.

autodj also reads every MIDI controller with an input port, such as the Akai MPD218 (USB `09e8:0034`), from the ALSA raw MIDI devices `/dev/snd/midiC*D*`. Virtual ports from `snd-virmidi` are skipped. The log names each controller it opens. A controller plugged in while autodj runs is opened within 3 seconds, and one that is unplugged is dropped. Reading them needs the `audio` group. MIDI bindings do not name the controller or the MIDI channel, so any controller that sends the same note or knob runs the same action. See `[Keys]` for the names.

Only one autodj can play at a time. Two would both read the shortcut remote's raw channel, which cannot be grabbed, so each click of the dial would reach both: one could change the volume while the other jogs. A second `autodj`, or `autodj --calibrate`, logs `autodj is already running (pid N)` and exits. The lock is `$XDG_RUNTIME_DIR/autodj.lock`, or `/tmp/autodj-<uid>.lock` without it, and it is released when autodj exits. `--config` and `--check` do not take it.

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
dial_mode_1=volume
dial_mode_2=jog
dial_mode_3=none

[Keys]
pause_resume_kbd=Space
skip_kbd=Right
previous_kbd=Left
rewind_0_5_kbd=1
rewind_1_kbd=2
rewind_1_5_kbd=3
dial_forward_kbd=Up
dial_back_kbd=Down
dial_mode_kbd=Enter
reverb_kbd=q
chipmunk_kbd=w
autopan_kbd=e
phaser_kbd=r
chorus_kbd=t
flanger_kbd=y
underwater_kbd=u
lasers_kbd=i
effects_off_kbd=Tab
volume_up_kbd==
volume_down_kbd=-
jog_up_kbd=.
jog_down_kbd=,
reverb_up_kbd=]
reverb_down_kbd=[
lasers_up_kbd=o
lasers_down_kbd=p
bubbles_kbd=b
bubbles_up_kbd=k
bubbles_down_kbd=l
chorus_up_kbd=n
chorus_down_kbd=m
skip_midi=NOTE_37
pause_resume_midi=NOTE_36
volume_up_midi=CC_3_UP
volume_down_midi=CC_3_DOWN
jog_up_midi=CC_9_UP
jog_down_midi=CC_9_DOWN
reverb_up_midi=CC_12_UP
reverb_down_midi=CC_12_DOWN

[Reverb]
decay=0.5
bw=1
damping=0
send_min=0
send_max=1

[Chipmunk]
pitch=1
range=1
semitones=true
formant=false

[Autopan]
period=4
smoothing=0.5
distance=75

[Phaser]
period=1
low=300
high=3000
stages=6
feedback=0.5
mix=50
boost=15

[Chorus]
period=1
delay=20
depth=5
voices=2
mix=50

[Flanger]
period=2
delay=1
depth=0.5
feedback=0.5
mix=50

[Underwater]
cutoff=500
resonance=0.4
period=0.5
wobble=1.5
mix=100
boost=20

[Lasers]
rate=4
low=280
high=700
length=260
level=30

[Bubbles]
file=bubbles.mp3
volume=50

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
`true` or `false`. Also accepts `yes`/`no`, `on`/`off`, and `1`/`0`. Default `true`. The play order is kept in `~/.local/state/autodj/songs.cache`, one file name per line. The first line is the next song. When a song starts, its name moves to the end of the file, so the list is a queue: pop from the top, push onto the bottom. Names already in the file stay in that order the next time autodj starts. A song in `Folder` that is not listed yet is shuffled onto the end when `shuffle` is on, and added in alphabetical order when `shuffle` is off. A listed name that is no longer in `Folder` is removed. The first time the file is missing, every song in `Folder` is added that way. With one track, that track repeats.

`overlap`  
Seconds. Default `10`. The last N seconds of the current song play together with the first N seconds of the next song. The outgoing song fades down while the incoming song fades up. Skip and previous start that same crossfade immediately.

`trim_silence`  
`true` or `false`. Default `true`. Leading and trailing silence is skipped so the crossfade lands on the music.

`pause_time`  
Seconds. Default `5`. Pause slows the current song to a stop over this many seconds. The pitch falls with the tempo.

`resume_time`  
Seconds. Default `5`. Resume brings the song back up to full speed over this many seconds.

`volume`  
Percent from 0 to 100. Default `100`. In volume mode the shortcut remote's dial changes it in steps of 1, and a keyboard volume key changes it in steps of 5. Each change is written back into this file, so the next boot starts at the last volume.

`loudness`  
`true` or `false`. Default `true`. Each file is measured from start to finish and then played at one steady gain so tracks match each other. This is a level change only. It does not compress the song.

`loudness_lufs`  
Target integrated loudness, from `-30` to `-5`. Default `-18`, which is the ReplayGain 2.0 reference. A quieter track is turned up and a louder track is turned down. The gain is held back if it would clip the loudest sample.

`dial_mode_1`, `dial_mode_2`, `dial_mode_3`  
`none`, `volume`, `jog`, or `reverb`. Defaults `volume`, `jog`, and `none`. What the dial (`dial_forward` and `dial_back`) does. autodj starts in the first mode that is not `none`, and each press of `dial_mode` moves to the next one that is not `none`, then back to the first. With the defaults it starts as a volume knob and switches between volume and jog. In `reverb` mode it is the reverb knob, like `reverb_up` and `reverb_down`. With only one mode set, `dial_mode` leaves it there. With all three `none`, the dial does nothing. The older `dial_default=jog` is still read when none of these lines are present, and makes the dial start in jog with volume second. `autodj --config` writes the three lines and removes `dial_default`.

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

 A modifier key on its own, such as `KEY_LEFTCTRL`, fires when it is released, but only if no other key was pressed while it was held. A missing keypad line is unbound. A blank value, such as `pause_resume=`, stays unbound. Pressing a key that is not bound logs the key name and does nothing. `KEY_NUMLOCK` from the keypad is ignored. A press within 30 ms of the previous press is ignored, except for dial, volume, jog, reverb, laser, bubbles, and chorus keys. Holding a button does not repeat it, except a dial, volume, jog, reverb, laser, bubbles, or chorus key, which repeats while it is held. Old `volume_dn` lines are ignored, and `autodj --config` removes them. `volume_up` and `volume_down` are bindings again, see below.

Each action can also have a keyboard binding. Add `_kbd` to the name, for example `pause_resume_kbd=p` or `skip_kbd=Space`. These are keys typed on the terminal while autodj is running, not `KEY_*` names. A single character is stored as typed, so `p` and `P` are different. The other names are `Space`, `Enter`, `Tab`, `Backspace`, `Delete`, `Up`, `Down`, `Left`, `Right`, `Home`, and `End`. A missing line uses the default: pause/resume `Space`, skip `Right`, previous `Left`, rewind 0.5s `1`, rewind 1s `2`, rewind 1.5s `3`, dial forward `Up`, dial back `Down`, dial mode `Enter`, reverb `q`, chipmunk `w`, autopan `e`, phaser `r`, chorus `t`, flanger `y`, underwater `u`, lasers `i`, bubbles `b`, all effects off `Tab`, volume up `=`, volume down `-`, jog up `.`, jog down `,`, reverb up `]`, reverb down `[`, lasers up `o`, lasers down `p`, bubbles up `k`, bubbles down `l`, chorus up `n`, and chorus down `m`. A blank line, such as `pause_resume_kbd=`, stays unbound. A keyboard key can be used for an action that already has a keypad key. Two actions cannot share one keyboard key. autodj reads the keyboard only when it is running in a terminal, and Ctrl-C still stops it. The same 30 ms gap applies, except a keyboard key bound to the dial, volume, jog, reverb, lasers, bubbles, or chorus, which repeats. One keyboard press changes the volume by 5, the reverb knob by 10, the laser volume by 10, the bubbles volume by 10, and the chorus depth by 1 ms. The shortcut remote's dial and a keypad key still change the volume by 1, move the reverb knob, the laser volume, or the bubbles volume by 2, and move the chorus depth by 0.2 ms.

Each action can also have a MIDI binding. Add `_midi` to the name, for example `skip_midi=NOTE_37`. `NOTE_<n>` is a pad or key that sends note `n` (0 to 127). `CC_<n>_UP` is a knob that sends control change `n` turned clockwise, and `CC_<n>_DOWN` the same knob turned counterclockwise. Names are not case sensitive, and `autodj --config` writes them in capitals. A missing or blank line is unbound. Two actions cannot share one MIDI control, but a knob can have its `_UP` on one action and its `_DOWN` on another. A pad changes the volume by 5 and moves the jog one click. Each step of a knob changes the volume by 1 and moves the jog one click. A pad moves the reverb knob, the laser volume, or the bubbles volume by 10, and the chorus depth by 1 ms. A knob step moves the reverb knob, the laser volume, or the bubbles volume by 2, and the chorus depth by 0.2 ms. A pad or knob bound to another action runs it once per press or step, with the same 30 ms gap. A MIDI control that is not bound is logged once.

A pad bound to reverb, chipmunk, autopan, phaser, chorus, flanger, underwater, lasers, or bubbles shows that effect's state: lit while it is on and dark while it is off. While the song is paused, the lasers pad and the bubbles pad go dark until that effect's button turns it back on. autodj sends the pad a Note On to light it and a Note Off to darken it, on the channel the pad sends on (channel 10, the MPD218's factory pad channel, until the pad is first pressed). The MPD218 lights a pad while it is held and darkens it when it is let go, so autodj lights it again after the release when the effect is on. An effect turned on or off by another key or by All effects off updates its pad too. When autodj stops, it darkens the pads it lit. Leave the pads in Momentary mode, the factory setting. Other pads light only while they are held. A pad bound to pause/resume is the play light. It stays red while a song plays and dark while the song is paused. Over `resume_time` it blinks, starting with one flash every 0.1 seconds and flashing faster until it stays on. Over `pause_time` it does the opposite: it starts on and the blink slows to one flash every 0.1 seconds, then goes dark.

With the MPD218's factory presets, the 16 pads of bank A send notes 36 to 51 (bottom left is 36, top right is 51), bank B 52 to 67, and bank C 68 to 83. The six knobs send CC 3, 9, 12, 13, 14, and 15 in knob bank A, CC 16 to 21 in bank B, and CC 22 to 27 in bank C. In those presets the knobs work like faders with ends: each one counts from 0 to 127 and sends nothing more past either end, so turn it back to keep going. For an endless knob, set it to "Inc/Dec 2" in Akai's MPD218 Editor. autodj tells the two apart by itself: a knob that sends the same value twice in a row is counted as Inc/Dec 2 from then on. The first message from a knob in the factory mode only sets its position. "Inc/Dec 1" mode is not supported.

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

`reverb`, `chipmunk`, `autopan`, `phaser`, `chorus`, `flanger`, `underwater`, `lasers`, `bubbles`  
Press to turn effect 1 (reverb), effect 2 (chipmunk), effect 3 (autopan), effect 4 (phaser), effect 5 (chorus), effect 6 (flanger), effect 7 (underwater), effect 8 (lasers), or effect 9 (bubbles) on or off. Lasers and bubbles go quiet when the song pauses. Pressing that button while the song is paused turns the effect back on, and it is heard while the song stays paused. Another press turns it off. When the song resumes, lasers and bubbles stay off. A press of that button during the pause leaves that effect on. Chorus, flanger, underwater, lasers, and bubbles are unbound on the keypad until a key is set. All effects are always off when autodj starts; the buttons turn them on for the current run only, and nothing is saved. Old `on=` lines in the effect sections are ignored, and `autodj --config` removes them when it saves.

`effects_off`  
Press to turn every effect off. They stay off, including when the song resumes. Unbound until a key is set.

`phaser=` is the effect 4 button. It no longer means `autopan`, as it did in older versions. See `[Phaser]` for its settings.

Chipmunk was called pitch shift before. The old key `pitch_shift` is still read as `chipmunk`, and an old `[Pitch Shift]` section is still read as `[Chipmunk]`. `autodj --config` writes both back under the new names.

`dial_forward`, `dial_back`  
The dial. It is a volume knob or a jog wheel, depending on the dial mode (see `dial_mode_1` in `[Main]`). In volume mode each click raises or lowers the volume by 1. In jog mode it is the shortcut remote's dial: `REMOTE_DIAL_CW` and `REMOTE_DIAL_CCW` in raw mode, or `CTRL+KEY_KPPLUS` and `CTRL+KEY_KPMINUS` with the factory shortcuts. The dial works like a hand on a record. With the default `[Jog]` settings, each click moves the song 90 ms, so one turn of 20 clicks moves it 1.8 seconds, which is one turn of a 33⅓ RPM record. The time between clicks sets the speed. A slow turn plays slowly, a fast spin plays up to 4 times as fast, and turning back plays the song backward. While the dial turns, normal playback is held. When the dial stops, the song spins back up to normal speed within 0.3 seconds. See `[Jog]` for the keypad dial and the keyboard jog keys, and `[Jog MPD218]` for the MPD218 jog knob. If the music is paused, the dial still moves the song and you can hear it, but the song stays paused when the dial stops. The position stops at the start of the song.

The old names `jog_forward` and `jog_back` are still read. `autodj --config` writes them back under the new names.

`dial_mode`  
Press to move the dial to its next mode, as set by `dial_mode_1` to `dial_mode_3` in `[Main]`. The editor calls it Next dial mode. The log shows the mode after each switch. Ten presses of the keypad or shortcut-remote mode button in a row, each within 2 seconds of the last and with no other keypad or remote key between them, set the modes to volume, jog, and none for this run and start on volume. That is not written into the file. The next start uses the saved modes. The keyboard and MIDI mode bindings still only move to the next mode.

`volume_up`, `volume_down`  
Raise or lower the volume, whatever mode the dial is in. A keypad or remote key, or a knob step, changes it by 1, and a keyboard key or a pad by 5.

`reverb_up`, `reverb_down`  
The reverb knob. It moves the reverb send, which is how much reverb is mixed in, between `send_min` and `send_max` in `[Reverb]`. It starts halfway each time autodj starts. A knob step or a keypad key moves it 2% of the way, and a keyboard key or a pad 10%, so a knob goes from one end to the other in 50 steps. The change is heard at once and is smoothed, so it does not click. It does not turn reverb on or off; the `reverb` button still does that. The log shows the position and the send after each change.

`lasers_up`, `lasers_down`  
The laser volume. It moves `level` in `[Lasers]` between 0 and 100, as a share of the song volume. It starts at the saved level, which is 30 when that line is missing. A knob step or a keypad key moves it by 2, and a keyboard key or a pad by 10, so a knob goes from one end to the other in 50 steps. The change is heard at once. It does not turn lasers on or off; the `lasers` button still does that. The new level is written back to the config file, like the song volume, and restored on the next start. Turning the knob while lasers are off still stores the level. The log shows the level after each change.

`bubbles_up`, `bubbles_down`  
The bubbles volume. It moves `volume` in `[Bubbles]` between 0 and 100, as a share of the song volume. It starts at the saved volume, which is 50 when that line is missing. A knob step or a keypad key moves it by 2, and a keyboard key or a pad by 10, so a knob goes from one end to the other in 50 steps. The change is heard at once. It does not turn bubbles on or off; the `bubbles` button still does that. The new volume is written back to the config file, like the song volume, and restored on the next start. Turning the knob while bubbles are off still stores the volume. The log shows the volume after each change.

`chorus_up`, `chorus_down`  
The chorus depth. It moves `depth` in `[Chorus]`, how far the delayed copies drift in pitch. The range is 0 to 10 ms, and the value stays below `delay`. It starts at the saved depth, which is 5 ms when that line is missing. A knob step or a keypad key moves it by 0.2 ms, and a keyboard key or a pad by 1 ms, so a knob goes from one end to the other in 50 steps. The change is heard at once. It does not turn chorus on or off; the `chorus` button still does that. The new depth is written back to the config file, like the song volume, and restored on the next start. Turning the knob while chorus is off still stores the depth. The log shows the depth after each change.

An old `[Bubbles]` section that still has `rate`, `low`, `high`, `length`, or `level` is read as lasers and written back under `[Lasers]`. The key names `bubbles`, `bubbles_up`, and `bubbles_down` are the bubbles loop. A config that only has an old `bubbles_kbd` line, from when that name meant lasers, now binds the bubbles button.

`jog_up`, `jog_down`  
Jog the song forward or back one click, like the dial in jog mode, whatever mode the dial is in. A keyboard or keypad binding uses `[Jog]`. A MIDI binding, such as the MPD218 jog knob, uses `[Jog MPD218]` when that section is present, and `[Jog]` when it is missing. These are separate from `jog_forward` and `jog_back`, which are old names of `dial_forward` and `dial_back`.

### [Reverb]

A plate reverb added on top of the song. The tail lasts about two seconds at the default decay. The knob names come from Mixxx. The processing is autodj's own.

`decay`  
`0` to `1`. Default `0.5`. Lower values fade out sooner.

`bw`  
`0` to `1`. Default `1`. Bandwidth of the low pass at the input. Higher values keep more of the high frequencies.

`damping`  
`0` to `1`. Default `0`. Higher values make the high frequencies die out sooner than the low ones.

`send_min`  
`0` to `1`. Default `0`. How much of the song is sent into the reverb with the reverb knob all the way down. `0` adds nothing, so the knob can fade the reverb out.

`send_max`  
`0` to `1`. Default `1`. The send with the reverb knob all the way up. It must be above `send_min`; otherwise both go back to their defaults. The knob starts halfway between the two, which with the defaults is `0.5`, the old fixed send. An old `send` line is ignored, and `autodj --config` removes it.

### [Chipmunk]

A pitch shifter. Raises or lowers the pitch. The default pitch is one octave up, which is the chipmunk sound. `0` leaves the song unchanged. The shift is spectral, like Mixxx, so the beat and tempo stay put; it adds about 40 ms of delay while on.

`pitch`  
`-1` to `1`. Default `1`. `0` is the original pitch. The ends of the knob are `range` octaves down and up.

`range`  
`0` to `2`. Default `1`. Octaves from the middle of Pitch to either end. `1` is one octave each way. `0` leaves the pitch unchanged.

`semitones`  
`true` or `false`. Default `true`. Pitch moves in semitone steps. `false` moves it continuously.

`formant`  
`true` or `false`. Default `false`. Keeps the tone color of voices and instruments while the pitch moves, so an octave up sounds less like a chipmunk.

### [Autopan]

Moves the song from one speaker to the other and back. Each time it is turned on it heads left or right first, at random. Turned off, it keeps swinging until it reaches the middle, then stops there, so the song never jumps back to the center. The knob names come from Mixxx. The processing is autodj's own. The swing follows the beat: autodj finds each song's tempo when it loads it, and Period counts measures of 4 beats. When a song has no steady beat, autodj assumes 120 BPM. The far speaker also gets up to 1 ms of delay, which helps the ear place the sound.

`period`  
`0.25` to `16`. Default `4`. Measures for one full swing, left, right, and back, rounded to whole beats. At `4` the song moves out to the left and back to the middle over two measures, then out to the right and back over the next two, so it passes the middle every other measure.

`smoothing`  
`0.25` to `0.5`. Default `0.5`. `0.5` swings smoothly. Lower values wait at each side and move across faster.

`distance`  
`0` to `100`. Default `75`. How far it swings, in percent from the middle toward each speaker. `100` goes all the way to each speaker. `0` stays in the middle.

### [Phaser]

Sends the song through a chain of allpass filters and mixes the result with the original, so notches sweep up and down between Low and High for a swirling sound. The sweep follows the beat like autopan: Period counts measures of 4 beats, and when a song has no steady beat, autodj assumes 120 BPM. The volume boost, which makes up for what the notches take out, steps up the moment the phaser is turned on. When it is turned off, the boost fades out together with the phasing over a few milliseconds, so the volume stays even. The settings of the old Mixxx-style phaser, `depth`, `range`, `triplets`, and `stereo`, are ignored, and `autodj --config` removes them when it saves. A value out of range is logged and the default is used.

`period`  
`0.25` to `16`. Default `1`. Measures for one sweep up and back down.

`low`  
`20` to `20000`. Default `300`. Bottom of the sweep in Hz.

`high`  
`20` to `20000`. Default `3000`. Top of the sweep in Hz. It must be above `low`; otherwise both go back to their defaults.

`stages`  
A whole number from `1` to `12`. Default `6`. Allpass filters in the chain. Every two make one notch, so more stages give more notches.

`feedback`  
`0` to `0.9`. Default `0.5`. Feeds the output back into the chain. Higher values make sharper, ringing notches.

`mix`  
`0` to `100`. Default `50`. Percent of phased sound in the mix. `50` gives the deepest notches. `0` leaves the song unchanged.

`boost`  
`0` to `100`. Default `15`. Percent louder while the phaser is on. The whole song steps up by this much at the press of the button, with no fade, and eases back down with the phasing when it is turned off.

### [Chorus]

Mixes the song with slightly delayed copies whose delay drifts, so the pitch of each copy moves a little. Several copies at once sound like more than one voice or instrument. The drift follows the beat like the phaser: Period counts measures of 4 beats, and when a song has no steady beat, autodj assumes 120 BPM. The processing is autodj's own. A value out of range is logged and the default is used. `depth` must be less than `delay`; otherwise both go back to their defaults.

`period`  
`0.25` to `16`. Default `1`. Measures for one drift up and back down.

`delay`  
`5` to `40`. Default `20`. How late the copies are, in milliseconds.

`depth`  
`0` to `10`. Default `5`. How far that delay swings, in milliseconds, which is how much the pitch drifts. It must be less than `delay`. `0` holds the copies at a fixed delay. A wider swing is easier to hear on a full mix. `chorus_up` and `chorus_down` move this while a song plays, and the value is saved.

`voices`  
A whole number from `1` to `4`. Default `2`. How many copies are mixed in. They are spread a little apart in time so they do not land on the same delay.

`mix`  
`0` to `100`. Default `50`. Percent of the copies in the mix. `0` leaves the song unchanged.

### [Flanger]

The same idea as chorus, with a much shorter delay. The delayed sound mixes with the original and cancels some harmonics, which is a comb filter. As the delay sweeps, those cancellations move and the song whooshes, like a jet. The sweep follows the beat the same way as chorus. The processing is autodj's own. A value out of range is logged and the default is used. `depth` must be less than `delay`; otherwise both go back to their defaults.

`period`  
`0.25` to `16`. Default `2`. Measures for one sweep out and back.

`delay`  
`0.2` to `5`. Default `1`. Length of the short delay, in milliseconds.

`depth`  
`0` to `4`. Default `0.5`. How far that delay sweeps, in milliseconds. It must be less than `delay`. `0` holds the notches still.

`feedback`  
`0` to `0.9`. Default `0.5`. Feeds the delayed sound back into the delay. Higher values make a sharper, ringing whoosh.

`mix`  
`0` to `100`. Default `50`. Percent of delayed sound in the mix. `50` gives the deepest notches. `0` leaves the song unchanged.

### [Underwater]

Makes the song sound as if it is heard under water. A steep low-pass filter (24 dB per octave) takes out the highs, so only a muffled, boomy body is left, and a resonant bump at the cutoff makes it sound hollow. A slowly moving delay bends the pitch a few cents up and down, like sound passing through moving water; the right speaker's wobble runs a little behind the left. autodj cannot pick the singer out of a finished song, so the whole song goes under water, singer included. The wobble follows the beat the same way as chorus. It fades in and out over a few milliseconds when it is turned on or off. The processing is autodj's own. A value out of range is logged and the default is used.

`cutoff`  
`100` to `4000`. Default `500`. Frequency in Hz above which the sound is taken out. Lower sounds deeper. Around `300` the words are hard to make out, and around `1500` it sounds more like a wall than water.

`resonance`  
`0` to `1`. Default `0.4`. A peak at the cutoff that makes the sound hollow and boomy. `0` is a plain, smooth muffle.

`period`  
`0.25` to `16`. Default `0.5`. Measures for one wobble up and back, which is one second at 120 BPM.

`wobble`  
`0` to `5`. Default `1.5`. How far the delay swings, in milliseconds. At the default period that bends the pitch about 15 cents each way. `0` holds the pitch steady.

`mix`  
`0` to `100`. Default `100`. Percent of underwater sound in the mix. Lower values leave some of the clear song on top.

`boost`  
`0` to `100`. Default `20`. Percent louder while underwater is on, to make up for the highs the filter takes out. `20` makes the underwater sound 1.2 times as loud as the filtered song.

### [Lasers]

Adds falling zaps on top of the song. The song itself is left as it is. The zaps follow the song volume, the same way the bubbles loop does. Each zap is one low tone that falls for as long as you can hear it. The onset is rounded, so it does not tick. There is no noise and no second tone. Most zaps start near the middle of `low` and `high`. Fewer start at either end. Lower zaps are a little louder and a little longer, and they fall further. About half of them are followed by a second zap a moment later, and then there is a longer gap. Which speaker a zap sits in is chosen at random. It fades in over a few milliseconds when it is turned on. Turning it off lets a zap that is already sounding finish, and a second zap that is about to follow it, and then it stops. No new zap is started. Pausing the song stops new zaps the same way. A zap that is already sounding finishes, and so does a second zap that was about to follow it. Pressing lasers while the song is paused turns the zaps back on, and they are heard while the song stays paused. Pressing it again turns them off. When the song resumes, lasers stay off. A press during the pause leaves them on. The processing is autodj's own. A value out of range is logged and the default is used. If `low` is above `high`, both go back to the defaults.

`rate`  
`0.2` to `20`. Default `4`. Average number of zaps per second before the paired ones. They are not evenly spaced. A higher rate is busier.

`low`  
`80` to `4000`. Default `280`. Lowest starting pitch, in Hz. Lower is a deeper zap.

`high`  
`80` to `6000`. Default `700`. Highest starting pitch, in Hz. It must be at least `low`. The same number as `low` makes every zap start at that pitch. Most zaps start well below this.

`length`  
`30` to `400`. Default `260`. How long one zap lasts, in milliseconds. Lower ones run longer than this, and higher ones run shorter. The pitch falls through this whole time.

`level`  
`0` to `100`. Default `30`. How loud the zaps are, as a share of the song volume. Song volume `0` adds nothing. At full song volume, `0` adds nothing and `100` mixes the zaps at twice unity, so the same saved number is twice as loud as it was before. The song stays at its own volume. `lasers_up` and `lasers_down` move this while a song plays, and the value is saved.

### [Bubbles]

Plays an mp3 from the sound-effects folder in a loop, added on top of the song. The song itself is left as it is, and the other effects do not change the loop. The folder is `--sfx`, which defaults to `/home/pi/Music/autodj/sfx`. The loop follows the song volume and the bubbles volume. Pausing the song fades the loop out over about 20 ms. Pressing bubbles while the song is paused fades the loop back in, and it is heard while the song stays paused. The bubbles volume still changes it then. Pressing it again turns the loop off. When the song resumes, the loop stays off. A press during the pause leaves it on. While the dial jogs and the loop is sounding, the loop keeps its normal speed. It fades in and out over about 20 ms when it is turned on or off, so the start and the stop do not click. It starts off. A missing file is logged and the button still toggles, but nothing is added. A value out of range is logged and the default is used.

`file`  
A file name in the sound-effects folder, with no folder in front of it. Default `bubbles.mp3`. `autodj --config` lists the mp3 files in that folder.

`volume`  
`0` to `100`. Default `50`. How loud the loop is, as a share of the song volume. Song volume `0` adds nothing. At full song volume, `100` mixes the loop at unity and `50` mixes it at half. The song stays at its own volume. `bubbles_up` and `bubbles_down` move this while a song plays, and the value is saved.

### [Jog]

How the keypad dial and the keyboard jog keys move the song. `autodj --calibrate` edits these settings on the Keypad tab and plays a song so you can hear each change. A value out of range is logged and the default is used.

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

### [Jog MPD218]

The MPD218 jog knob. The six settings are the same as `[Jog]`, with the same ranges and defaults. When this section is missing, the jog knob uses the `[Jog]` values. Saving the MPD218 tab in `autodj --calibrate` writes this section. Saving the Keypad tab leaves this section as it is. Volume, reverb, lasers, bubbles, and chorus knobs are not in this section.
