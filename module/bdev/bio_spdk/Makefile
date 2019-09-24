SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

CFLAGS += -DUSE_UMC

#XXX What is the right way to import a non-system archive from another repository?
TCMUR_ROOT_DIR := $(SPDK_ROOT_DIR)/../tcmu-runner
CFLAGS += -I$(TCMUR_ROOT_DIR)/libtcmur -I$(TCMUR_ROOT_DIR)/ccan

MTE_ROOT_DIR := $(SPDK_ROOT_DIR)/../MTE
CFLAGS += -I$(MTE_ROOT_DIR)/include

UMC_ROOT_DIR := $(SPDK_ROOT_DIR)/../usermode_compat/src
CFLAGS += -I$(UMC_ROOT_DIR) -isystem $(UMC_ROOT_DIR)/arch/x86/include -isystem $(UMC_ROOT_DIR)/include

C_SRCS = bdev_bio_spdk.c bdev_bio_spdk_rpc.c
LIBNAME = bdev_bio_spdk

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
