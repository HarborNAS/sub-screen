## Installation

For easy installation on Debian/Ubuntu systems, run:

```bash
curl -sSL https://raw.githubusercontent.com/HarborNAS/sub-screen/main/install.sh | bash
```

This will:
- Install required dependencies
- Download and install the latest .deb package
- Start the subscreen service

### Manual Installation

If you prefer manual installation:

**Prerequisites** (tested on Debian 12/13 or Ubuntu):

```bash
sudo apt install build-essential
sudo apt install libhidapi-dev
sudo apt install libnvidia-ml-dev
sudo apt-get install libusb-1.0-0-dev
```

**Install the .deb package:**

```bash
wget https://github.com/HarborNAS/sub-screen/releases/download/v1.0.0/harboros-subscreen_1.0.0_linux_amd64.deb
sudo apt install ./harboros-subscreen_1.0.0_linux_amd64.deb
```

**Start the service:**

```bash
sudo systemctl start harboros-subscreen
```

After starting, the screen will display time, then enter firmware update mode (about 40 seconds). During update:
- Screen sliding has no effect
- Screen will blink black twice when complete
- Then you can view system information

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
6. Support WLANPage
7. Support PowerPage
8. Support SleepPage
9. Support LocalPage
## TODO

NA
