#!/bin/bash
set -e
make clean
make
# sudo valgrind --leak-check=full --show-leak-kinds=all ./ctcp -s -p 8888
sudo ./ctcp -s -p 8888
