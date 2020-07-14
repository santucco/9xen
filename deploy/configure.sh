#!/bin/bash

usage="Usage $0 <9front ISO file>"

[[ $# -lt 1 ]] && echo $usage && exit -1
[[ ! -f $1 ]] && echo "File '$1' has not been found" && echo $usage && exit -1
[[ "$1" != "9.iso" ]] && cp -v "$1" 9.iso

function header() {
cat <<EOF
type = "pv"
kernel = "9xenpcf"
memory = 256
name = "9xen"
#vif = [ 'script=vif-bridge,bridge=br0']
EOF
}

function footer1() {
cat <<EOF
disk = [ '9xen.img,,sda','9.iso,,sdb,cdrom' ]
# This is the equivalent of plan9.ini:
extra="\nnobootprompt=local!/dev/sd01/data\nuser=glenda\nfourcc=XR24\n"
EOF
}

function footer2() {
cat <<EOF
disk = [ '9xen.img,,sda' ]
# This is the equivalent of plan9.ini:
extra="\nbootfile=9pc\nnobootprompt=local!/dev/sd00/fscache\nuser=glenda\nfourcc=XR24\n"
EOF
}

function vdispl() {
	for p in /sys/class/drm/*/status
	do
		status=$(cat $p)
		if [[ "$status" = "disconnected" ]]
		then
			continue
		fi
		con=${p%/status}
		mod=$(cat $con/modes)
		con=${con#*/card?-}
		echo "vdispl = [ 'connectors=$con:$mod' ]"
		break
	done
}

function vkb() {
	for m in $(ls /dev/input/by-path/*event-mouse)
	do
		mouse=$m
		break
	done
	for k in $(ls /dev/input/by-path/*event-kbd)
	do
		kbd=$k
		break
	done
	echo "vkb = [ 'backend-type=linux,unique-id=K:$kbd;P:$mouse' ]"
}

(header; vdispl; vkb; footer1) > 9xen_install

header > 9xen
vdispl >> 9xen
vkb >> 9xen
footer2 >> 9xen

if [[ -f 9xen.img ]]
then 
	echo "9xen.img already exists, skip "
else
	echo "Creating of 10Gb image for the system "
	dd if=/dev/zero of=9xen.img seek=$((1024*1024*1024*10-1)) bs=1 count=1
fi

echo "Run 'xl create -c 9xen_install' to install 9xen"
echo "then run 'xl create -c 9xen' to start 9xen"

