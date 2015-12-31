## What's this ?

Recently I got my hands on a quite ancient microcontroller kit (http://www.amazon.de/Lernpaket-Roboter-selbst-bauen-Roboter-Programmierung/dp/3645650261/ref=sr_1_2/275-2526774-4865333?ie=UTF8&qid=1451581669&sr=8-2&keywords=franzis+lernpaket+roboter+selbst+bauen) sold by Franzis Verlag.

Since the kit only came with tools for programming in Basic on Windows (Bascom-AVR) but I wanted to do C programming on linux, I spend some time figuring out how the Basic compiler uploads programs to the controller.

The hardware in the kit is an ATmega88pa hooked-up to a FTDI232RL and the controller came pre-programmed with a MCS bootloader.

The MCS bootloader works in the following way:

1. It configures to UART to run with 19200 baud 8N1
2. It polls the serial port waiting for either 123 (flash upload) or 124 (eeprom upload) 
3. After it receives one of those two bytes, it runs a simple X-modem protocol to read the data in 128-byte chunks and writes it to flash/eeprom.

## Compiling 

You need a linux system with libftdi-dev (and libusb since this is what libftdi uses) installed. I compiled the program using gcc but I suspect any C compiler will do.

To build the program, just run


```
make   
```

## Uploading software to the microcontroller

Before you begin...

1. The program currently only works if only one FTDI device is present on the USB bus (but it would be trivial to extend it and read the device ID from commandline arguments) 
2. The program currently only supports uploading raw files (=just the data). You can use my hex2raw Java program to extract just the raw bytes from an Intel hex file.
3. The bootloader will write the data starting at 0x0000 so the very first word needs to be a rjmp instruction that jumps to the actual start of your program (because the interrupt vectors are stored starting at 0x0000). The bootloader
   jumps to 0x0000 to execute your program (just like the regular boot sequence on an ATmega88).
4. You should make sure to put a sufficient number of rti instructions right after the jump to your actual program. For example on an ATmega88 the words 0x0000..0x0019 are all interrupt vectors...and you don't want a stray interrupt to crash the controller.

To upload a raw file to the controller, just run

```
mcs_upload <raw file>
```

You now need to reset/power-cycle the controller so that the bootloader gets executed. If all went well, the controller should execute your program after the upload finishes.
