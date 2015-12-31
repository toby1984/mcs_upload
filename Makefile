# INCLUDES=-I/usr/include/libftdi1
INCLUDES=-I/usr/include/libftdi1
LIBS=-lftdi1

all:	mcs_upload.c
	gcc mcs_upload.c -o mcs_upload $(INCLUDES) $(LIBS)
