#!/bin/bash
#
# To download the repositories and build SPDK to support tcmu-runner loadable
# handlers:  make an empty directory and cd into it, then run this script.
#
# This script assumes you already have the build tools and libraries installed
# such that you can build in the standard SPDK and tcmu-runner repositories.
#
# Script updated: Wed 04 Sep 2019 04:10:28 PM MDT

# Start in an empty directory and clone these repositories into it:
git clone https://github.com/DavidButterfield/spdk.git
git clone https://github.com/DavidButterfield/tcmu-runner.git

# Checkout the SPDK branch containing the tcmu-runner support
(cd spdk; \
    git checkout tcmu-runner;
)

# Checkout the tcmu-runner branch containing the libtcmur support
# which provides access to tcmu-runner backend storage handlers
# (tcmu-runner itself is not involved here)
(cd tcmu-runner; \
    git checkout libtcmur;
)

# In the tcmu-runner directory:
# First make the standard tcmu-runner stuff:
#     Omit glfs because I can't find the library for my Ubuntu 18.04 LTS system
#     Copy handler_file.so manually because make install doesn't do it
# Then make libtcmur, used by bdev_tcmur in SPDK
(cd tcmu-runner; \
    cmake -Dwith-glfs=false .; \
    make; \
    sudo make install
    sudo cp handler_file.so /usr/local/lib/tcmu-runner
    cd libtcmur; \
    make;
)

# In the SPDK directory:
(cd spdk; \
    git submodule update --init; \
    ./configure; \
    make;
)

# Create three 1GiB backing files in /tmp for the default configuration
echo -n "Creating default backing files in /tmp..."
truncate --size=1G /tmp/tcmur_ram00	# bdev_tcmur using handler_ram.so
truncate --size=1G /tmp/tcmur_file01	# bdev_tcmur using handler_file.so
truncate --size=1G /tmp/myfile		# bdev_aio
chmod 666 /tmp/tcmur_ram00 /tmp/tcmur_file01 /tmp/myfile
echo " ...done"

echo ""
echo "First edit spdk/etc/spdk/iscsi.tcmur_conf.in to suit your environment, then try:"
echo "    sudo spdk/app/iscsi_tgt/iscsi_tgt -c spdk/etc/spdk/iscsi.tcmur_conf.in"
echo ""
echo "NOTE:  So far only has been tested using the 'file' and 'ram' tcmur handlers."
echo "       Needs testing on an async handler (nr_threads == 0)!"
echo ""
