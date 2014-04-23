# Reference Counting Performance Test

This is just a rundown of the cost of reference counting under various scenarios.
To profile your system just run:

```sh
make
./sample.sh cpu_name
```

A good `cpu_name` would be something like core-i7-4470s. You can get the model name from /proc/cpuinfo.

Make sure you have gnuplot installed to generate the graph.

**This requires Intel TSX extensions for the TSX test.**

If you have an idea for more tests (like adding support for PowerPC, or other clever ideas), please share them.
