#!/bin/bash

# Note -- this test spawns xterm's, so be sure you've logged in with `ssh -Y`.
#
# TODO: For Lab 1, this only has the client send data to the server. Once Lab 2
# is implemented, this should be updated to have the server echo back what it
# receives so that we can test unreliable simultaneous tx and rx.
#
# Also worth noting is that, according to valgrind, the reference
# implementation leaks memory, when the network unreliability flags are used.
# I have not investigated this to determine the root cause. For the sake of
# finishing this project and moving on with life, I'm not going to fix
# valgrind errors in my implementation that also exist in the reference
# implementation.

###############################################################################
# Test setup
###############################################################################

# Set this to 'reference' if you want to run your test against the reference
# implementation. This is a way to check if the test is too unreasonable
# (e.g., the network is too unreliable for the test to pass).
executable=ctcp
#executable=reference

echo
echo "Testing with $executable executable".
echo

# Spawn xterm window so that we can monitor file progress
xterm -e "watch -n 1 \"ls -lrt | egrep 'reference($|_copy)'\"" &
xterm_monitor_pid=$!

make clean
make

###############################################################################
# Test 1 -- Try to send the 'reference' binary to the server.
###############################################################################

rm -f reference_copy

# Spawn server
xterm -e "sudo valgrind --leak-check=full --show-leak-kinds=all ./$executable -s -p 8888 > reference_copy" &
server_pid=$!
echo "Server spawned with PID $server_pid"
sleep 6s # Give server time to start up

# Spawn client
xterm -e "sudo valgrind --leak-check=full --show-leak-kinds=all ./$executable -p 9999 -c localhost:8888 < reference" &
client_pid=$!
echo "Client spawned with PID: $client_pid"

sleep 4s # Give time for the file to transfer.

cmp reference reference_copy
return_code=$?
if [ "$return_code" -eq "0" ]; then
    echo "*** Test 1 passed - successfully sent reference binary to server."
else
    echo "*** Test 1 FAILED - didn't send reference binary to server"
fi
read  -n 1 -p "Press enter to continue:" dummy_variable

kill $server_pid
kill $client_pid
rm -f reference_copy


###############################################################################
# Test 2 -- Send 'reference' again, but with a 5% unreliable network.
###############################################################################


# ctcp connection will be problematic this percentage of the time
unreliability=5
seed=1337

# Spawn server
xterm -e "sudo valgrind --leak-check=full --show-leak-kinds=all ./$executable -s -p 8888 --seed $seed --drop $unreliability --corrupt $unreliability --delay $unreliability --duplicate $unreliability > reference_copy" &
server_pid=$!
echo "Server spawned with PID $server_pid"
sleep 3s # Give server time to start up

# Spawn client
xterm -e "sudo valgrind --leak-check=full --show-leak-kinds=all ./$executable -p 9999 -c localhost:8888 --seed $seed --drop $unreliability --corrupt $unreliability --delay $unreliability --duplicate $unreliability < reference" &
client_pid=$!
echo "Client spawned with PID: $client_pid"

# Give time for the file to transfer. Reference takes about 6s, so we should
# not need much more time than that.
sleep 8s

cmp reference reference_copy
return_code=$?
if [ "$return_code" -eq "0" ]; then
    echo "*** Test 2 passed - send reference binary over unreliable network."
else
    echo "*** Test 2 FAILED - didn't send reference binary to server"
fi
read  -n 1 -p "Press enter to continue:" dummy_variable

kill $server_pid
kill $client_pid
rm -f reference_copy



###############################################################################
# Test 3 -- Send 'reference' again, but with an even more unreliable network.
###############################################################################


seed=1338

# Spawn server
xterm -e "sudo valgrind --leak-check=full --show-leak-kinds=all ./$executable -s -p 8888                --drop 10 --corrupt 10 --delay 15 --duplicate 15  --seed $seed > reference_copy" &
server_pid=$!
echo "Server spawned with PID $server_pid"
sleep 3s # Give server time to start up

# Spawn client
xterm -e "sudo valgrind --leak-check=full --show-leak-kinds=all ./$executable -p 9999 -c localhost:8888 --drop 10 --corrupt 10 --delay 15 --duplicate 15 --seed $seed < reference" &
client_pid=$!
echo "Client spawned with PID: $client_pid"

# Give time for the file to transfer. Don't allow much more time than the
# reference implementation takes.
sleep 12s

cmp reference reference_copy
return_code=$?
if [ "$return_code" -eq "0" ]; then
    echo "*** Test 3 passed - send reference binary over an even more unreliable network."
else
    echo "*** Test 3 FAILED - didn't send reference binary to server"
fi
read  -n 1 -p "Press enter to continue:" dummy_variable


kill $server_pid
kill $client_pid
rm -f reference_copy

###############################################################################
# Test cleanup.
###############################################################################


echo
echo "Finished testing with $executable executable".
echo

kill $xterm_monitor_pid

