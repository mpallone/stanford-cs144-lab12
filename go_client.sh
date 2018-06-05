#!/bin/bash
set -e
make clean
make

# Lab 1
#sudo valgrind --leak-check=full --show-leak-kinds=all ./ctcp -p 9999 -c localhost:8888
#sudo ./ctcp -p 9999 -c localhost:8888

# Lab 2
sudo ./ctcp -p 9999 -c localhost:8888 -w 5 < reference
#sudo valgrind --leak-check=full --show-leak-kinds=all ./ctcp -p 9999 -c localhost:8888 -w 5
