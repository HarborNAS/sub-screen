#!/bin/bash

# HarborOS Subscreen Installer
# This script installs the HarborOS subscreen service on Debian/Ubuntu systems

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}HarborOS Subscreen Installer${NC}"
echo "================================="

# Check if running on Debian/Ubuntu
if ! command -v apt &> /dev/null; then
    echo -e "${RED}Error: This installer is only for Debian/Ubuntu systems${NC}"
    exit 1
fi

# Check if running as root
if [[ $EUID -eq 0 ]]; then
    echo -e "${RED}Error: This script should not be run as root. It will use sudo when needed.${NC}"
    exit 1
fi

echo -e "${YELLOW}Installing dependencies...${NC}"
sudo apt update
sudo apt install -y build-essential libhidapi-dev libnvidia-ml-dev libusb-1.0-0-dev

echo -e "${YELLOW}Finding latest release...${NC}"
LATEST_RELEASE=$(curl -s https://api.github.com/repos/HarborNAS/sub-screen/releases/latest)
DEB_URL=$(echo "$LATEST_RELEASE" | grep -o 'https://github.com/HarborNAS/sub-screen/releases/download/[^"]*\.deb')
DEB_NAME=$(basename "$DEB_URL")

echo -e "${YELLOW}Downloading HarborOS Subscreen package ($DEB_NAME)...${NC}"
curl -L -o "$DEB_NAME" "$DEB_URL"

echo -e "${YELLOW}Installing package...${NC}"
sudo apt install -y "./$DEB_NAME"

echo -e "${YELLOW}Starting service...${NC}"
sudo systemctl start harboros-subscreen

echo -e "${GREEN}Installation completed successfully!${NC}"
echo ""
echo "The subscreen service has been started."
echo "The screen will display the time initially, then enter firmware update mode."
echo "During firmware update (about 40 seconds):"
echo "- Screen sliding will have no effect"
echo "- Screen will blink black twice when update completes"
echo "- After that, you can use the subscreen to view system information"
echo ""
echo "To check service status: sudo systemctl status harboros-subscreen"
echo "To stop service: sudo systemctl stop harboros-subscreen"
echo "To restart service: sudo systemctl restart harboros-subscreen"