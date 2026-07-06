obj-m += snd-usb-us1800.o
snd-usb-us1800-y := us1800.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
