# RACER: Avoiding End-to-End Slowdowns in Accelerated Chip Multi-Processors

Below are instructions to build all dependencies and configure INTEL(R) XEON(R) PLATINUM 8571N running Ubuntu 24.04 to run RACER

* Build idxd-config
```sh
./build_all.sh
```

* Configure IAA && DSA
```sh
sudo python3 scripts/accel_conf.py --load=scripts/confs/idxd-2n8d64e8w128s-s-n1-n2.conf
```

`scripts/fig*` directories reproduce the figures from [RACER: Avoiding End-to-End Slowdowns in Accelerated Chip Multi-Processors]()