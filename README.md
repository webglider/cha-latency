### Compile

```
gcc -o cha_latency cha_latency.c
```

### Usage

Run while pinning to a dedicated core (measurements are with respect to socket containing the core to which the program is pinned).

```
sudo taskset -c 4 ./cha_latency
```

Outputs local latency, remote latency