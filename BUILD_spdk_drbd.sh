#!/bin/bash
#
# To download the repositories and build SPDK to support DRBD and tcmu-runner
# handlers:  make an empty directory and cd into it, then run this script.
#
# This script assumes you already have the build tools and libraries installed
# so that you can build the standard SPDK, DRBD, and tcmu-runner repositories.
#
# Script updated:  Mon Sep  9 10:18:55 MDT 2019

### Some of the makefiles require various build tools -- here are package names
### I added to a fresh installation of Ubuntu 18.04 LTS to complete the build:
###
###    build-essential g++ gcc git make gdb valgrind cscope exuberant-ctags
###    libfuse-dev libaio-dev libglib2.0-dev libkmod-dev libnl-3-dev
###    libnl-genl-3-dev librbd-dev autoconf automake flex coccinelle cmake

echo Getting sudo password at start of script rather than sometime later
sudo echo Got sudo password

# Get our reference kernel
# The reference kernel is the kernel level emulated by UMC
wget https://cdn.kernel.org/pub/linux/kernel/v2.6/linux-2.6.32.27.tar.gz
echo -n "Unpack the reference kernel..."
gunzip linux-2.6.32.27.tar.gz
tar xf linux-2.6.32.27.tar
rm linux-2.6.32.27.tar		# for space if FS is only 1GB
echo " ...done"

# Clone these repositories:
# XXX SCST is temporarily needed until I straighten out the Makefiles
git clone https://github.com/DavidButterfield/spdk.git
git clone https://github.com/DavidButterfield/drbd-9.0.git
git clone https://github.com/DavidButterfield/drbd-utils.git
git clone https://github.com/DavidButterfield/tcmu-runner.git
git clone https://github.com/DavidButterfield/usermode_compat.git
git clone https://github.com/DavidButterfield/MTE.git
git clone https://github.com/DavidButterfield/SCST-Usermode-Adaptation.git

# Checkout the right branches:
(cd usermode_compat; \
    git checkout drbd; \
)

(cd SCST-Usermode-Adaptation; \
    git checkout drbd; \
)

# Make the multi-threaded engine library used by UMC
# XXX Most or all calls to MTE should be replaced by calls to SPDK equivalents
#     (MTE calls go through an ops vector, which can change to point at SPDK shim ops)
(cd MTE/src; \
    make clean; make; \
)

# In the tcmu-runner directory:
# Make the standard tcmu-runner stuff (in particular the loadable handlers)
#     Omit glfs because I can't find the library for my Ubuntu 18.04 LTS system XXX
#     Copy handler_file.so manually because make install doesn't do it
# But don't make libtcmur now -- that will be driven with CONFIG_BIO=1 through UMC make below
(cd tcmu-runner; \
    cmake -Dwith-glfs=false .; \
    make; \
    sudo make install
    sudo cp handler_file.so /usr/local/lib/tcmu-runner
)

# Make the usermode compatibility library; this also makes libtcmur with correct options
(cd usermode_compat/src; \
    make clean; make; \
)

# In the drbd-utils source directory:
    ## If you omit --without-manual, it will take a long time for the make to complete.
(cd drbd-utils; \
    ./autogen.sh; \
    ./configure --prefix=/usr --localstatedir=/var --sysconfdir=/etc --without-manual; \
    make; \
    sudo make install)

# In the drbd-9.0 source directory:
    ## You should not need to "make" in the drbd-9.0 directory.  The make for
    ## usermode DRBD is done from other Makefiles passing in the appropriate
    ## flags to drbd-9.0/drbd/Makefile.usermode
    ## 
    ## However, it is necessary to download some headers and compatibility code
    ## external to the drbd-9.0 repository:
(cd drbd-9.0; \
    make check-submods)

# After that additional code gets downloaded, there is a patch to apply to it:
(cd drbd-9.0/drbd/drbd-kernel-compat; \
    patch -p1 < ../../PATCH.drbd-kernel-compat)

# XXX Use of SCST directory is temporary until the Makefiles get straightened out
(cd SCST-Usermode-Adaptation/usermode; \
    make drbd_compat.o drbd; \
)

# In the SPDK directory:
(cd spdk; \
    git submodule update --init; \
    ./configure --enable-debug --with-tcmur --with-drbd; \
    make;
    sudo cp drbdadm_up_primary /usr/sbin	# DRBD config helper
)

echo ""
echo "Executable:  " `ls -l spdk/app/iscsi_tgt/iscsi_tgt`

# Create backing files in /tmp for the default configuration
echo ""
echo -n "Creating default backing files in /tmp..."
sudo truncate --size=2G /tmp/tcmur_ram00;    sudo chmod 666 /tmp/tcmur_ram00
sudo truncate --size=2G /tmp/tcmur_ram01;    sudo chmod 666 /tmp/tcmur_ram01
sudo truncate --size=2G /tmp/tcmur_ram02;    sudo chmod 666 /tmp/tcmur_ram02
sudo truncate --size=2G /tmp/tcmur_file00;   sudo chmod 666 /tmp/tcmur_file00
sudo truncate --size=2G /tmp/tcmur_file01;   sudo chmod 666 /tmp/tcmur_file01
sudo truncate --size=2G /tmp/tcmur_file02;   sudo chmod 666 /tmp/tcmur_file02
sudo truncate --size=2G /tmp/myfile;         sudo chmod 666 /tmp/myfile
echo " ...done"

# Create DRBD config file to match spdk/etc/spdk/iscsi.drbd_conf.in
echo ""
echo "Copy spdk/module/bdev/bio/spdk1.res.example to /etc/drbd.d/spdk1.res"
echo "     and modify it to match your local names, IP addresses, etc."

echo ""
echo "Server process and DRBD utilities must be run with:"
echo "    export UMC_FS_ROOT=/UMCfuse"
echo ""
echo "Use sudo -E to pass the environment variable from your shell through sudo"

echo ""
echo "To run default test configuration:"
echo "    export UMC_FS_ROOT=/UMCfuse"
echo "    sudo -E spdk/app/iscsi_tgt/iscsi_tgt -c spdk/etc/spdk/iscsi.drbd_conf.in"
echo ""
echo "NOTE:  So far only has been tested using the 'file' and 'ram' tcmur handlers."
echo "       Needs testing on an async handler (nr_threads == 0)!"
echo ""
