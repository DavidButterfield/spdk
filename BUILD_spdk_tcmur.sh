#!/bin/bash
#
# To download the repositories and build SPDK to support tcmu-runner loadable
# handlers:  make an empty directory and cd into it, then run this script.
#
# This script assumes you already have the build tools and libraries installed
# such that you can build in the standard SPDK and tcmu-runner repositories.
#
# Script updated: Mon Sep  9 10:13:05 MDT 2019

echo Getting sudo password at start of script rather than sometime later
sudo echo Got sudo password

# Start in an empty directory and clone these repositories into it:
git clone https://github.com/DavidButterfield/spdk.git
git clone https://github.com/DavidButterfield/tcmu-runner.git

# In the tcmu-runner directory:
# First make the standard tcmu-runner stuff (in particular the loadable handlers)
#     Omit glfs because I can't find the library for my Ubuntu 18.04 LTS system XXX
#     Copy handler_file.so manually because make install doesn't do it
# Then make libtcmur, used by bdev_tcmur in SPDK
(cd tcmu-runner; \
    cmake -Dwith-glfs=false .; \
    make; \
    sudo make install
    sudo cp handler_file.so /usr/local/lib/tcmu-runner
    cd libtcmur; \
    make clean; make;
)

# In the SPDK directory:
(cd spdk; \
    git submodule update --init; \
    ./configure --enable-debug --with-tcmur; \
    make;
)

echo ""
echo "Executable:  " `ls -l spdk/app/iscsi_tgt/iscsi_tgt`

# Create backing files in /tmp for the default configuration
echo ""
echo -n "Creating default backing files in /tmp..."
sudo truncate --size=1G /tmp/tcmur_ram00;    sudo chmod 666 /tmp/tcmur_ram00
sudo truncate --size=1G /tmp/tcmur_file00;   sudo chmod 666 /tmp/tcmur_file00
sudo truncate --size=1G /tmp/myfile;         sudo chmod 666 /tmp/myfile
echo " ...done"

echo ""
echo "To run example test configuration:"
echo "    # Modify spdk/etc/spdk/iscsi_tcmur.conf.in to suit your environment"
echo "    export UMC_FS_ROOT=/UMCfuse"
echo "    sudo -E spdk/app/iscsi_tgt/iscsi_tgt -c spdk/etc/spdk/iscsi_tcmur.conf.in"
echo ""
echo "NOTE:  So far only has been tested using the 'file' and 'ram' tcmur handlers."
echo "       Needs testing on an async handler (nr_threads == 0)!"
echo ""
