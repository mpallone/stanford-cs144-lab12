#!/bin/bash
set -e
make clean
make

# Lab 1
#sudo valgrind --leak-check=full --show-leak-kinds=all ./ctcp -s -p 8888
#sudo ./ctcp -s -p 8888

# Lab 2
sudo ./ctcp -s -p 8888 -w 6 > reference_copy
#sudo valgrind --leak-check=full --show-leak-kinds=all ./ctcp -s -p 8888 -w 6
