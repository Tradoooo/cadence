#!/bin/sh
# guest-cadence.sh — lance l'invité QNX 8.0 de Cadence sous QEMU/KVM.
#
# Dérivé de ~/qnx-emu/guest-qemu.sh, avec le mode KVM ajouté : sans
# -enable-kvm, l'invité tourne en émulation TCG et le débit passe de ~17 t/s
# à ~3 t/s (mesuré). Le boot KVM exige aussi -cpu host.
#
# Usage :
#   guest-cadence.sh [--ifs <ifs.bin>] [--serie <log>] [--moniteur <sock>]
#                    [--memoire <Mo>] [--smp <n>] [--qemu <binaire>]
set -eu

IFS_BIN="${QNX_EMS_IFS:-$HOME/qnx-cadence/img/ifs-cadence.bin}"
SERIE="${QNX_EMS_SERIE:-$HOME/qnx-cadence-live/serial.log}"
MON="${QNX_EMS_MON:-$HOME/qnx-cadence-live/mon.sock}"
MEM="${QNX_EMS_MEM:-6144}"
SMP="${QNX_EMS_SMP:-6}"
CPU="${QNX_EMS_CPU:-host}"
QEMU="${QNX_EMS_QEMU:-$HOME/bin/qemu-system-x86_64}"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --ifs)      IFS_BIN="$2"; shift 2 ;;
        --serie)    SERIE="$2"; shift 2 ;;
        --moniteur) MON="$2"; shift 2 ;;
        --memoire)  MEM="$2"; shift 2 ;;
        --smp)      SMP="$2"; shift 2 ;;
        --qemu)     QEMU="$2"; shift 2 ;;
        *)          echo "guest-cadence: argument inconnu : $1" >&2; exit 1 ;;
    esac
done

[ -f "$IFS_BIN" ] || { echo "guest-cadence: IFS introuvable : $IFS_BIN" >&2; exit 1; }
[ -x "$QEMU" ]   || { echo "guest-cadence: qemu introuvable : $QEMU" >&2; exit 1; }

mkdir -p "$(dirname "$SERIE")" "$(dirname "$MON")"

# Pièges déjà payés : --cpu max obligatoire sous TCG, le périphérique RNG
# virtio est obligatoire sinon le noyau s'arrête, l'IFS se lance par -kernel
# (pas par le disque), et -no-reboot évite les boucles de redémarrage.
echo "guest-cadence: IFS=$IFS_BIN mem=${MEM}Mo smp=$SMP"
exec "$QEMU" \
    -enable-kvm -cpu "$CPU" -smp "$SMP" -m "$MEM" \
    -kernel "$IFS_BIN" \
    -object rng-random,filename=/dev/urandom,id=rng0 \
    -device virtio-rng-pci,rng=rng0 \
    -display none -vga std \
    -no-reboot \
    -serial "file:$SERIE" \
    -monitor "unix:$MON,server,nowait"
