# autodj

Console player for a folder of MP3 files. Songs crossfade into each other. A USB keypad pauses, skips, and sets the volume. The volume is stored in the config file and restored on the next start.

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

## Run

```
autodj
autodj --check
autodj /path/to/master.cfg
```

With no config path, autodj reads `~/.config/autodj/master.cfg`.

`--check` loads the playlist and runs the built-in tests, including a grab of the keypad. It does not start playback.

The keypad device must be named `USB Keypad` and must report `KEY_KP0`. autodj grabs that device so the keys are not also typed into the console. The process needs read access to `/dev/input/event*`, which on Raspberry Pi OS means the user is in the `input` group. If no keypad is found, playback still runs.

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

[Keys]
pause_resume=KEY_KP0
skip=KEY_KPDOT
volume_up=KEY_KPPLUS
volume_dn=KEY_KPMINUS
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
Seconds. Default `10`. The last N seconds of the current song play together with the first N seconds of the next song. The outgoing song fades down while the incoming song fades up. Skip starts that same crossfade immediately.

`trim_silence`  
`true` or `false`. Default `true`. Leading and trailing silence is skipped so the crossfade lands on the music.

`pause_time`  
Seconds. Default `5`. Pause slows the current song to a stop over this many seconds. The pitch falls with the tempo.

`resume_time`  
Seconds. Default `5`. Resume brings the song back up to full speed over this many seconds.

`volume`  
Percent from 0 to 100. Default `100`. The volume keys change it in steps of 5. Each change is written back into this file, so the next boot starts at the last volume.

### [Keys]

Each value is a `KEY_*` name from `linux/input-event-codes.h`, for example `KEY_KP0`. An action that is left out is unbound. Pressing a key that is not bound logs the key name and does nothing. `KEY_NUMLOCK` from the keypad is ignored.

`pause_resume`  
Press once to pause. Press again to resume.

`skip`  
Press to skip to the next track.

`volume_up`  
Press or hold to raise the volume. The name `brake_delayvolume_up` is accepted as this same action.

`volume_dn`  
Press or hold to lower the volume. `volume_down` is the same action.
