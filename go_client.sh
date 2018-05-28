#!/bin/bash
set -e
make clean
make
# sudo valgrind --leak-check=full --show-leak-kinds=all ./ctcp -p 9999 -c localhost:8888
sudo ./ctcp -p 9999 -c localhost:8888
