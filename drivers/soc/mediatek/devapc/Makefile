# SPDX-License-Identifier: GPL-2.0

ifeq ($(CONFIG_MTK_GCOV_KERNEL),y)
GCOV_PROFILE := y
endif

LOCAL_PATH := $(srctree)/drivers/soc/mediatek/devapc

ifneq ($(wildcard $(LOCAL_PATH)/$(MTK_PLATFORM)/Makefile),)
  obj-$(CONFIG_DEVAPC_LEGACY) += $(MTK_PLATFORM)/
else
  # Core
  obj-$(CONFIG_DEVAPC_ARCH_V1) += devapc-mtk-common.o
  obj-$(CONFIG_DEVAPC_ARCH_V2) += devapc-mtk-multi-ao.o

  # SoC
  obj-$(CONFIG_DEVAPC_MT6779) += devapc-mt6779.o
endif
