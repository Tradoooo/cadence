#!/bin/sh
# run-in-guest.sh — demo executed INSIDE the QNX 8.0 guest.
#
# The IFS startup script has no pipe and no redirection support, so this is
# launched through ksh:
#   on -d -t /dev/ser1 ksh -c /proc/boot/run-in-guest.sh
#
# NOTE: the guest has no `cat`/`sleep` (toybox is not present), so the client
# binary provides `--wait` (retry until the service is ready) and `--cat`.
PATH=/proc/boot:/system/bin
LD_LIBRARY_PATH=/proc/boot:/system/lib:/system/lib/dll
export PATH LD_LIBRARY_PATH
HOME=/proc/boot
TERM=qansi

MODEL=/proc/boot/model.gguf

echo ""
echo "############################################################"
echo "# Cadence : service LLM natif QNX 8.0 (resource manager)"
echo "############################################################"
echo "--- version de l invite ---"
pidin info

echo ""
echo "--- demarrage de cadenced (modele: $MODEL) ---"
/proc/boot/cadenced -m $MODEL -n cadence-llm -c 4096 -t 4 -P 20 -C 0 -M 96 &
DPID=$!

echo "--- attente du service (chargement du modele) ---"
/proc/boot/cadence-cli --wait 120 --ping

echo ""
echo "--- configuration temps reel du service ---"
/proc/boot/cadence-cli --status

echo ""
echo "--- threads QNX (pidin threads) ---"
/proc/boot/cadence-cli --threads

echo ""
echo "--- question en francais (flux de tokens) ---"
/proc/boot/cadence-cli -M 64 \
    -p "Quelle est la capitale de la France ? Reponds en francais, en une seule phrase."

echo ""
echo "--- arret propre du service (SIGTERM) ---"
slay cadenced

echo ""
echo "=== FIN DEMO CADENCE ==="