#!/bin/sh
export PATH=/sbin:/usr/sbin:$PATH

SRCDIR="`dirname $0`"
. $SRCDIR/common.sh

SERVER=compila

mknod /dev/portals c 10 240

insmod $R/usr/src/portals/linux/oslib/portals.o
# insmod $R/usr/src/portals/linux/socknal/ksocknal.o
insmod $R/usr/src/portals/linux/qswnal/kqswnal.o

$PTLCTL <<EOF
mynid
setup elan 
connect 5
add_uuid self
add_uuid mds
EOF

insmod $R/usr/src/obd/rpc/ptlrpc.o
insmod $R/usr/src/obd/class/obdclass.o 
insmod $R/usr/src/obd/ext2obd/obdext2.o
insmod $R/usr/src/obd/ost/ost.o
insmod $R/usr/src/obd/osc/osc.o
insmod $R/usr/src/obd/mds/mds.o
insmod $R/usr/src/obd/mdc/mdc.o
insmod $R/usr/src/obd/llight/llight.o

tmp_fs ext2 /tmp/ost 10000
OST=${LOOPDEV}

mknod /dev/obd c 10 241

$OBDCTL <<EOF
device 1
attach obdext2
setup ${OST}
device 2
attach ost
setup 1
device 3
attach osc
setup 2
quit
EOF

mkdir /mnt/obd
# mount -t lustre_light -o device=3 none /mnt/obd
