obj-y := boot.o main.o

# Everest has its own FDT workspace and must not invoke the generic
# initrd patcher (CONFIG_RAMDISK_NO_COPY / Huawei SAR).
ifneq ($(filter y,$(CONFIG_HUAWEI_EVEREST) $(CONFIG_HUAWEI_SCHUMANN)),)
obj-$(CONFIG_LIBFDT) += everest-fdt.o
else
obj-$(CONFIG_LIBFDT) += boot-fdt.o
endif
