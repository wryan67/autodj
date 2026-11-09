#!/bin/sh
# Build and install autodj. The first run installs the udev rule for the
# shortcut remote, which needs sudo once. Later runs skip it.
set -e
cd "$(dirname "$0")"

rule=udev/70-autodj-remote.rules
dest=/etc/udev/rules.d/70-autodj-remote.rules
if ! cmp -s "$rule" "$dest"; then
    echo "installing $dest (needs sudo once)"
    sudo install -m 644 "$rule" "$dest"
    sudo udevadm control --reload-rules
    sudo udevadm trigger --subsystem-match=hidraw --action=change
fi

make
make install
