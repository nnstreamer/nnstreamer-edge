#!/usr/bin/env bash
##
## @file run_unittests.sh
## @author Jijoong Moon <jijoong.moon@samsung.com>
## @date 17 September 2021
## @brief Run unit test for nnstreamer-edge
##

ret=0
pushd build

check_mosquitto() {
    pid=`ps aux | grep mosquitto | grep -v grep | awk '{print $2 }'`

    if [ $pid > 0 ]; then
        echo "running"
        return 1
    fi
    echo "not running"
    return 0
}

run_entry() {
  entry=$1
  ${entry} --gtest_output="xml:${entry##*/}.xml"
  return $?
}

# Check whether mqtt broker is running or not
check_mosquitto
ret=$?
if [ $ret -ne 1 ]; then
    echo "Warning! mosquitto is not running so unittest is ignored."
    exit 0
fi

if [ -f "$1" ]; then
    echo $1
    run_entry $1
    exit $?
elif [ -d "$1" ]; then
    testlist=$(find $1 -type f -executable -name "unittest_*")
    for t in ${testlist}; do
	echo "running: ${t} @$(pwd)"
	run_entry ${t}
	ret=$?
	if [ $ret -ne 0 ]; then
	    break
	fi
    done
fi

popd
exit $ret
