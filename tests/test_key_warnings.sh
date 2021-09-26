#!/bin/sh

PROG="$1"

mkdir -p testoutput
TEST=testoutput

TESTNAME="Test that failed decryption warning only prints once."
TESTRESULT="OK"

$PROG --format=json simulations/simulation_bad_keys.txt room fhkvdataiii 03065716 NOKEY > $TEST/test_output.txt 2> $TEST/test_stderr.txt

cat > $TEST/expected_err.txt <<EOF
(meter) room: meter detection did not match the selected driver fhkvdataiii! correct driver is: fhkvdataiv
(meter) Not printing this warning agin for id: 03065716 mfct: (TCH) Techem Service (0x5068) type: Heat Cost Allocator (0x08) ver: 0x94
(wmbus) decrypted content failed check, did you use the correct decryption key? Permanently ignoring telegrams from id: 03065716 mfct: (TCH) Techem Service (0x5068) type: Heat Cost Allocator (0x08) ver: 0x94
EOF

diff $TEST/test_stderr.txt $TEST/expected_err.txt

if [ "$?" != "0" ]
then
    TESTRESULT="ERROR"
fi

$PROG --format=json simulations/simulation_bad_keys.txt room fhkvdataiv 03065716 00112233445566778899AABBCCDDEEFF > $TEST/test_output.txt 2> $TEST/test_stderr.txt

cat > $TEST/expected_err.txt <<EOF
(meter) room: meter detection did not match the selected driver fhkvdataiv! correct driver is: fhkvdataiii
(meter) Not printing this warning agin for id: 03065716 mfct: (TCH) Techem Service (0x5068) type: Heat Cost Allocator (0x80) ver: 0x94
(wmbus) decrypted content failed check, did you use the correct decryption key? Permanently ignoring telegrams from id: 03065716 mfct: (TCH) Techem Service (0x5068) type: Heat Cost Allocator (0x08) ver: 0x94
EOF

diff $TEST/test_stderr.txt $TEST/expected_err.txt

if [ "$?" != "0" ]
then
    TESTRESULT="ERROR"
fi

echo ${TESTRESULT}: $TESTNAME
