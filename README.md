## prerequisites

tested in debian 12 or ubuntu xx

```bash
sudo apt install build-essential
sudo apt install libhidapi-dev
sudo apt install libnvidia-ml-dev
```

## Build

```bash
cat directive.txt | bash
```

## features

1. Support CPU temperature,CPU usage,memory usage,diskcount,disusage
2. Support FRD screen function
3. Support HomePage 2025/11/11 testOK
4. Support SystemPage(CPU,IGPU,Memory,NVDGPU) 2025/11/11 testOK
5. Support DiskPage 2025/11/11 testOK

## TODO

1. WLANPage
2. PowerPage
3. SleepPage
4. LocalPage
