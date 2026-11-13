#!/bin/bash
# mkifs.sh — build the QNX 8.0 IFS image that carries the Cadence service.
#
# Runs on the machine that owns the QNX SDP (r630). It assembles the
# boot image (procnto, ksh, the Cadence binaries, the GGUF model) and
# produces ifs-cadence.bin, to be booted with:  qemu -kernel ifs-cadence.bin
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_DIR=$(cd "$SCRIPT_DIR/.." && pwd)

QNX_SDP="${QNX_SDP:-$HOME/qnx800}"
QNX_HOST="$QNX_SDP/host/linux/x86_64"
QNX_TARGET="$QNX_SDP/target/qnx"
export QNX_HOST QNX_TARGET
# mkifs links the startup scripts, it needs the QNX compiler in the PATH.
PATH="$QNX_HOST/usr/bin:$PATH"
export PATH

WORK_DIR="${WORK_DIR:-$HOME/qnx-cadence}"
IMG_DIR="$WORK_DIR/img"
OUT_BIN="${OUT_BIN:-$WORK_DIR/out}"
MODEL="${MODEL:-$WORK_DIR/model.gguf}"

mkdir -p "$IMG_DIR"

cp "$OUT_BIN/cadenced" "$IMG_DIR/cadenced"
cp "$OUT_BIN/cadence-cli" "$IMG_DIR/cadence-cli"
cp "$REPO_DIR/tools/run-in-guest.sh" "$IMG_DIR/run-in-guest.sh"
chmod 755 "$IMG_DIR/run-in-guest.sh"

cat > "$IMG_DIR/buildfile" <<EOF
# IFS QNX 8.0 x86_64 : service Cadence (cadenced + cadence-cli).
[search=.:$QNX_TARGET/x86_64:$QNX_TARGET/x86_64/boot/sys:]

[+dupignore]
[+optional]

[uid=0 gid=0 dperms=0555 perms=0555]

[image=800m]
[virtual=x86_64,multiboot ] boot = {
    startup-x86 -D8250..115200 -zz -f 0,0,1000M
    PATH=/proc/boot:/system/bin
    LD_LIBRARY_PATH=/proc/boot:/system/lib:/system/lib/dll
    [+keeplinked uid=0 gid=0 perms=0700 ] CONFIG_PATH=/proc/boot:/system/etc procnto-smp-instr -mr -d 022 -u 066 -F 2000
}

[+script] startup-script = {
    procmgr_symlink /proc/boot/ksh /bin/sh
    procmgr_symlink /proc/boot/ksh /bin/ksh
    procmgr_symlink /proc/boot/ldqnx-64.so.2 /usr/lib/ldqnx-64.so.2
    procmgr_symlink /proc/boot/pidin /bin/pidin
    procmgr_symlink /proc/boot/mount /bin/mount
    procmgr_symlink /proc/boot/umount /bin/umount
    procmgr_symlink /proc/boot/slay /bin/slay
    procmgr_symlink /proc/boot/toybox /bin/cat
    procmgr_symlink /proc/boot/toybox /bin/echo
    procmgr_symlink /proc/boot/toybox /bin/sleep
    procmgr_symlink /proc/boot/toybox /bin/ls
    procmgr_symlink /proc/boot/toybox /bin/head
    procmgr_symlink /proc/boot/toybox /bin/tail
    procmgr_symlink /proc/boot/toybox /bin/wc
    PATH=/proc/boot:/system/bin
    LD_LIBRARY_PATH=/proc/boot:/system/lib:/system/lib/dll
    HOME=/proc/boot
    TERM=qansi
    devc-con
    devc-ser8250 -e
    waitfor /dev/ser1
    on -d -t /dev/ser1 -e HOME=/proc/boot ksh -c /proc/boot/run-in-guest.sh
}

multiboot.boot = boot/sys/multiboot.boot
procnto-smp-instr = boot/sys/procnto-smp-instr
startup-x86 = boot/sys/startup-x86

devc-con = sbin/devc-con
devc-ser8250 = sbin/devc-ser8250
on = bin/on
ksh = bin/ksh
mount = bin/mount
umount = bin/umount
pidin = bin/pidin
slay = bin/slay
toybox = usr/bin/toybox

ldqnx-64.so.2 = usr/lib/ldqnx-64.so.2
libc.so.6 = lib/libc.so.6
libsecpol.so.1 = lib/libsecpol.so.1
libslog2.so.1 = lib/libslog2.so.1
libcam.so.2 = lib/libcam.so.2
libm.so.3 = lib/libm.so.3
libgcc_s.so.1 = lib/libgcc_s.so.1
libsocket.so.4 = lib/libsocket.so.4
libc++.so.2 = usr/lib/libc++.so.2

cadenced = ./cadenced
cadence-cli = ./cadence-cli
run-in-guest.sh = ./run-in-guest.sh
model.gguf = $MODEL
EOF

cd "$IMG_DIR"
echo "== mkifs =="
"$QNX_HOST/usr/bin/mkifs" -v -o "$WORK_DIR" buildfile ifs-cadence.bin

ls -la "$WORK_DIR/ifs-cadence.bin"
echo "mkifs.sh: OK"
