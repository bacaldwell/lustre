#!/bin/bash
#
# Run select tests by setting ONLY, or as arguments to the script.
# Skip specific tests by setting EXCEPT.
#
# e.g. ONLY="22 23" or ONLY="`seq 32 39`" or EXCEPT="31"
set -e

ONLY=${ONLY:-"$*"}
ALWAYS_EXCEPT=${ALWAYS_EXCEPT:-""}
[ "$ALWAYS_EXCEPT$EXCEPT" ] && echo "Skipping tests: $ALWAYS_EXCEPT $EXCEPT"

SRCDIR=`dirname $0`
export PATH=$PWD/$SRCDIR:$SRCDIR:$SRCDIR/../utils:$PATH
export SECURITY=${SECURITY:-"null"}

TMP=${TMP:-/tmp}
FSTYPE=${FSTYPE:-ext3}

CHECKSTAT=${CHECKSTAT:-"checkstat -v"}
CREATETEST=${CREATETEST:-createtest}
LFS=${LFS:-lfs}
LSTRIPE=${LSTRIPE:-"$LFS setstripe"}
LFIND=${LFIND:-"$LFS find"}
LVERIFY=${LVERIFY:-ll_dirstripe_verify}
LCTL=${LCTL:-lctl}
MCREATE=${MCREATE:-mcreate}
OPENFILE=${OPENFILE:-openfile}
OPENUNLINK=${OPENUNLINK:-openunlink}
TOEXCL=${TOEXCL:-toexcl}
TRUNCATE=${TRUNCATE:-truncate}
MUNLINK=${MUNLINK:-munlink}
SOCKETSERVER=${SOCKETSERVER:-socketserver}
SOCKETCLIENT=${SOCKETCLIENT:-socketclient}
IOPENTEST1=${IOPENTEST1:-iopentest1}
IOPENTEST2=${IOPENTEST2:-iopentest2}
PTLDEBUG=${PTLDEBUG:-0}

. krb5_env.sh

if [ $UID -ne 0 ]; then
	RUNAS_ID="$UID"
	RUNAS=""
else
	RUNAS_ID=${RUNAS_ID:-500}
	RUNAS=${RUNAS:-"runas -u $RUNAS_ID"}
fi

if [ `using_krb5_sec $SECURITY` == 'y' ] ; then
    start_krb5_kdc || exit 1
    if [ $RUNAS_ID -ne $UID ]; then
        $RUNAS ./krb5_refresh_cache.sh || exit 2
    fi
fi

export NAME=${NAME:-local}

SAVE_PWD=$PWD

clean() {
	echo -n "cln.."
	sh llmountcleanup.sh > /dev/null || exit 20
	I_MOUNTED=no
}
CLEAN=${CLEAN:-clean}

start() {
	echo -n "mnt.."
	sh llrmount.sh > /dev/null || exit 10
	I_MOUNTED=yes
	echo "done"
}
START=${START:-start}

log() {
	echo "$*"
	lctl mark "$*" 2> /dev/null || true
}

trace() {
	log "STARTING: $*"
	strace -o $TMP/$1.strace -ttt $*
	RC=$?
	log "FINISHED: $*: rc $RC"
	return 1
}
TRACE=${TRACE:-""}

check_kernel_version() {
	VERSION_FILE=/proc/fs/lustre/kernel_version
	WANT_VER=$1
	[ ! -f $VERSION_FILE ] && echo "can't find kernel version" && return 1
	GOT_VER=`cat $VERSION_FILE`
	[ $GOT_VER -ge $WANT_VER ] && return 0
	log "test needs at least kernel version $WANT_VER, running $GOT_VER"
	return 1
}

run_one() {
	if ! mount | grep -q $DIR; then
		$START
	fi
	echo $PTLDEBUG >/proc/sys/portals/debug	
	log "== test $1: $2"
	export TESTNAME=test_$1
	test_$1 || error "test_$1: exit with rc=$?"
	unset TESTNAME
	pass
	cd $SAVE_PWD
	$CLEAN
}

build_test_filter() {
        for O in $ONLY; do
            eval ONLY_${O}=true
        done
        for E in $EXCEPT $ALWAYS_EXCEPT; do
            eval EXCEPT_${E}=true
        done
}

_basetest() {
    echo $*
}

basetest() {
    IFS=abcdefghijklmnopqrstuvwxyz _basetest $1
}

run_test() {
         base=`basetest $1`
         if [ "$ONLY" ]; then
                 testname=ONLY_$1
                 if [ ${!testname}x != x ]; then
 			run_one $1 "$2"
 			return $?
                 fi
                 testname=ONLY_$base
                 if [ ${!testname}x != x ]; then
                         run_one $1 "$2"
                         return $?
                 fi
                 echo -n "."
                 return 0
 	fi
        testname=EXCEPT_$1
        if [ ${!testname}x != x ]; then
                 echo "skipping excluded test $1"
                 return 0
        fi
        testname=EXCEPT_$base
        if [ ${!testname}x != x ]; then
                 echo "skipping excluded test $1 (base $base)"
                 return 0
        fi
        run_one $1 "$2"
 	return $?
}

[ "$SANITYLOG" ] && rm -f $SANITYLOG || true

error() { 
	log "FAIL: $@"
	if [ "$SANITYLOG" ]; then
		echo "FAIL: $TESTNAME $@" >> $SANITYLOG
	else
		exit 1
	fi
}

pass() { 
	echo PASS
}

MOUNT="`mount | awk '/^'$NAME' .* lustre_lite / { print $3 }'`"
if [ -z "$MOUNT" ]; then
	sh llmount.sh
	MOUNT="`mount | awk '/^'$NAME' .* lustre_lite / { print $3 }'`"
	[ -z "$MOUNT" ] && error "NAME=$NAME not mounted"
	I_MOUNTED=yes
fi

[ `echo $MOUNT | wc -w` -gt 1 ] && error "NAME=$NAME mounted more than once"

DIR=${DIR:-$MOUNT}
[ -z "`echo $DIR | grep $MOUNT`" ] && echo "$DIR not in $MOUNT" && exit 99

rm -rf $DIR/[Rdfs][1-9]*
build_test_filter

echo preparing for tests involving mounts
EXT2_DEV=${EXT2_DEV:-/tmp/SANITY.LOOP}
touch $EXT2_DEV
mke2fs -j -F $EXT2_DEV 8000 >/dev/null 2>&1

find_free_loop() {
    local LOOP_DEV=""
    test -b /dev/loop0 && 
	base="/dev/loop" || base="/dev/loop/"

    for ((i=0;i<256;i++)); do
	test -b $base$i || continue
	
	losetup $base$i >/dev/null 2>&1 || {
	    LOOP_DEV="$base$i"
	    break
	}
    done
    echo $LOOP_DEV
}

setup_loop() {
    local LOOP_DEV=$1
    local LOOP_FILE=$2
    
    dd if=/dev/zero of=$LOOP_FILE bs=1M count=10 2>/dev/null || return $?

    losetup $LOOP_DEV $LOOP_FILE || {
	rc=$?
	cleanup_loop $LOOP_DEV $LOOP_FILE
	return $rc
    }
    
    mke2fs -F $LOOP_DEV >/dev/null 2>&1 || {
	rc=$?
	cleanup_loop $LOOP_DEV $LOOP_FILE
	echo "cannot create test ext2 fs on $LOOP_DEV"
	return $?
    }
    return 0
}

cleanup_loop() {
    local LOOP_DEV=$1
    local LOOP_FILE=$2
    
    losetup -d $LOOP_DEV >/dev/null 2>&1
    rm -fr $LOOP_FILE >/dev/null 2>&1
}

setup_upcall() {
    local INJECTION=""
    local UPCALL=$1
    local MODE=$2
    local LOG=$3

    test "x$MODE" = "xDEADLOCK" &&
    INJECTION="touch \$MNTPATH/file"
    
    cat > $UPCALL <<- EOF
#!/bin/sh

MOUNT=\`which mount 2>/dev/null\`
test "x\$MOUNT" = "x" && MOUNT="/bin/mount"

OPTIONS=\$1
MNTPATH=\$2

test "x\$OPTIONS" = "x" || "x\$MNTPATH" = "x" &&
exit 1

$INJECTION
\$MOUNT \$OPTIONS \$MNTPATH > $LOG 2>&1
exit \$?
EOF
    chmod +x $UPCALL
    return $?
}

cleanup_upcall() {
    local UPCALL=$1
    rm -fr $UPCALL
}

check_gns() {
    local LOG="/tmp/gns-log"
    local UPCALL_PATH=""
    
    local UPCALL=$1
    local OBJECT=$2
    local TIMOUT=$3
    local TICK=$4
    
    rm -fr $LOG >/dev/null 2>&1
    UPCALL_PATH="/tmp/gns-upcall-$UPCALL.sh"
    
    echo "generating upcall $UPCALL_PATH"
    setup_upcall $UPCALL_PATH $UPCALL $LOG || return $rc
    echo "======================== upcall script ==========================="
    cat $UPCALL_PATH 2>/dev/null || return $?
    echo "=================================================================="
   
    echo "$UPCALL_PATH" > /proc/fs/lustre/llite/fs0/gns_upcall || return $?
    echo "upcall:  $(cat /proc/fs/lustre/llite/fs0/gns_upcall)"

    echo -n "mount on open $OBJECT/test_file1: "
    echo -n "test data" > $OBJECT/test_file1 >/dev/null 2>&1 || return $?

    local ENTRY="`basename $OBJECT`"
    
    cat /proc/mounts | grep -q "$ENTRY" || {
	echo "fail"
	test -f $LOG && {
	    echo "======================== upcall log ==========================="
	    cat $LOG
	    echo "==============================================================="
	} || {
	    echo "upcall log file $LOG is not found"
	}
	cleanup_upcall $UPCALL_PATH
	return 1
    }
    echo "success"

    local sleep_time=$TIMOUT
    let sleep_time+=$TICK*2
    echo -n "waiting for umount ${sleep_time}s (timeout + tick*2): "
    sleep $sleep_time

    cat /proc/mounts | grep -q "$ENTRY" && {
	echo "failed"
	cleanup_upcall $UPCALL_PATH
	return 2
    }
    echo "success"
    cleanup_upcall $UPCALL_PATH
    return 0
}

setup_object() {
    local OBJPATH=$1
    local OBJECT=$2
    local CONTENT=$3
    
    mkdir -p $OBJPATH || return $?
    echo -n $CONTENT > $OBJPATH/$OBJECT || return $?
    
    echo "======================== mount object ==========================="
    cat $OBJPATH/$OBJECT
    echo ""
    echo "================================================================="
    
    chmod u+s $OBJPATH
    return $?
}

cleanup_object() {
    local OBJPATH=$1

    chmod u-s $OBJPATH >/dev/null 2>&1
    umount $OBJPATH >/dev/null 2>&1
    rm -fr $OBJPATH >/dev/null 2>&1
}

setup_gns() {
    local OBJECT=$1
    local TIMOUT=$2
    local TICK=$3

    echo "$OBJECT" > /proc/fs/lustre/llite/fs0/gns_object_name || error
    echo "$TIMOUT" > /proc/fs/lustre/llite/fs0/gns_timeout || error
    echo "$TICK" > /proc/fs/lustre/llite/fs0/gns_tick || error

    echo ""
    echo "timeout: $(cat /proc/fs/lustre/llite/fs0/gns_timeout)s"
    echo "object:  $(cat /proc/fs/lustre/llite/fs0/gns_object_name)"
    echo "tick:    $(cat /proc/fs/lustre/llite/fs0/gns_tick)s"
    echo ""

}

test_1a() {
    local LOOP_DEV=$(find_free_loop 2>/dev/null)
    local UPCALL="/tmp/gns-upcall.sh"
    local LOOP_FILE="/tmp/gns_loop_1a"
    local OBJECT=".mntinfo"
    local TIMOUT=5
    local TICK=1

    test "x$LOOP_DEV" != "x" && test -b $LOOP_DEV ||
	error "can't find free loop device"

    echo "preparing loop device $LOOP_DEV <-> $LOOP_FILE..."
    cleanup_loop $LOOP_DEV $LOOP_FILE
    setup_loop $LOOP_DEV $LOOP_FILE || error

    echo "setting up GNS timeouts and mount object..."
    setup_gns $OBJECT $TIMOUT $TICK || error

    echo "preparing mount object at $DIR/gns_test_1a/$OBJECT..."
    setup_object $DIR/gns_test_1a $OBJECT "-t ext2 $LOOP_DEV" || error

    echo ""
    echo "testing GNS with GENERIC upcall 3 times on the row"
    for ((i=0;i<3;i++)); do
	check_gns GENERIC $DIR/gns_test_1a $TIMOUT $TICK || {
	    cleanup_object $DIR/gns_test_1a
	    cleanup_loop $LOOP_DEV $LOOP_FILE
	    error
	}
    done
    
    cleanup_object $DIR/gns_test_1a
    cleanup_loop $LOOP_DEV $LOOP_FILE
}

run_test 1a " general GNS test - mounting/umount (GENERIC) ============="

test_2a() {
    local LOOP_DEV=$(find_free_loop 2>/dev/null)
    local UPCALL="/tmp/gns-upcall.sh"
    local LOOP_FILE="/tmp/gns_loop_2a"
    local OBJECT=".mntinfo"
    local TIMOUT=5
    local TICK=1

    test "x$LOOP_DEV" != "x" && test -b $LOOP_DEV ||
	error "can't find free loop device"

    echo "preparing loop device $LOOP_DEV <-> $LOOP_FILE..."
    cleanup_loop $LOOP_DEV $LOOP_FILE
    setup_loop $LOOP_DEV $LOOP_FILE || error

    echo "setting up GNS timeouts and mount object..."
    setup_gns $OBJECT $TIMOUT $TICK || error

    echo "preparing mount object at $DIR/gns_test_2a/$OBJECT..."
    setup_object $DIR/gns_test_2a $OBJECT "-t ext2 $LOOP_DEV" || error

    echo ""
    echo "testing GNS with DEADLOCK upcall 3 times on the row"
    for ((i=0;i<3;i++)); do
	check_gns DEADLOCK $DIR/gns_test_2a $TIMOUT $TICK || {
	    cleanup_object $DIR/gns_test_2a
	    cleanup_loop $LOOP_DEV $LOOP_FILE
	    error
	}
    done
    
    cleanup_object $DIR/gns_test_2a
    cleanup_loop $LOOP_DEV $LOOP_FILE
}

run_test 2a " general GNS test - mounting/umount (DEADLOCK) ============"

test_3a() {
    local LOOP_DEV=$(find_free_loop 2>/dev/null)
    local UPCALL="/tmp/gns-upcall.sh"
    local LOOP_FILE="/tmp/gns_loop_3a"
    local OBJECT=".mntinfo"
    local TIMOUT=5
    local TICK=1

    test "x$LOOP_DEV" != "x" && test -b $LOOP_DEV ||
	error "can't find free loop device"

    echo "preparing loop device $LOOP_DEV <-> $LOOP_FILE..."
    cleanup_loop $LOOP_DEV $LOOP_FILE
    setup_loop $LOOP_DEV $LOOP_FILE || error

    echo "setting up GNS timeouts and mount object..."
    setup_gns $OBJECT $TIMOUT $TICK || error

    echo "preparing mount object at $DIR/gns_test_3a/$OBJECT..."
    setup_object $DIR/gns_test_3a $OBJECT "-t ext2 $LOOP_DEV" || error

    echo ""
    echo "testing GNS with DEADLOCK upcall 4 times on the row"
    for ((i=0;i<4;i++)); do
	local MODE="GENERIC"
	
	test $(($i%2)) -eq 1 && MODE="DEADLOCK"
	
	check_gns $MODE $DIR/gns_test_3a $TIMOUT $TICK || {
	    cleanup_object $DIR/gns_test_3a
	    cleanup_loop $LOOP_DEV $LOOP_FILE
	    error
	}
    done
    
    cleanup_object $DIR/gns_test_3a
    cleanup_loop $LOOP_DEV $LOOP_FILE
}

run_test 3a " general GNS test - mounting/umount (GENERIC/DEADLOCK) ====="

test_4a() {
    echo "Not implemented yet!"
}

run_test 4a " general GNS test - concurrent mounting/umount ============="

TMPDIR=$OLDTMPDIR
TMP=$OLDTMP
HOME=$OLDHOME

log "cleanup: ==========================================================="
if [ "`mount | grep ^$NAME`" ]; then
	rm -rf $DIR/[Rdfs][1-9]*
	if [ "$I_MOUNTED" = "yes" ]; then
		sh llmountcleanup.sh || error
	fi
fi

echo '=========================== finished ==============================='
[ -f "$SANITYLOG" ] && cat $SANITYLOG && exit 1 || true
