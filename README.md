# CACHESIM

The cache simulator for [ECE 550 Homework 5](http://people.ee.duke.edu/~jab/ece550/homeworks/CacheSWAssign.html).

Features:

- Simulating one or two levels of caches.
- Caches can be split I$/D$, or unified.
- Various associativity (direct mapped/set associative/fully associative)
- Statistics for hit/miss and access time.

# How to use

```
cc -o cachesim cachesim.c
./cachesim --help
```

An example simulating the last test case on [this page](http://people.ee.duke.edu/~jab/ece550/homeworks/CacheSimulatorTesting.html):
```
./cachesim -I1,1,32,16384,1,01 -D1,1,32,16384,1,01 -L2,4,64,262144,1,01 -T,100 spec026.ucomp.din
```

## Special thanks

to my group partners: Jie Wang and Yan Su.
