obj-y := boot.o main.o
ifeq ($(CONFIG_KIRIN710),y)
obj-$(CONFIG_LIBFDT) += kirin710-fdt.o
else
obj-$(CONFIG_LIBFDT) += boot-fdt.o
endif
