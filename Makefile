cc = arm-none-linux-gnueabi-gcc

#single_buffer : single_dumb_buffer.c
#	$(cc) single_dumb_buffer.c -o single_buffer 	\


all:
	$(cc) drm_demo.c -o drm_demo \
	-I ../libdrm-2.4.101 \
	-I ../libdrm-2.4.101/include/drm 		\
	-ldrm -L../libdrm-2.4.101/build/


clean:
	rm drm_demo
