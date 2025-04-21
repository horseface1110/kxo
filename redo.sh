#!/bin/bash
sudo rmmod kxo
make clean
make
sudo insmod kxo.ko
sudo ./xo-user
