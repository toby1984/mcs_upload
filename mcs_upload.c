#include <stdio.h>
#include <stdlib.h>
#include <ftdi.h>
#include <signal.h>
#include <unistd.h>

// X-Modem control sequences
#define NACK (unsigned char) 0x15
#define ACK (unsigned char) 0x06
#define CANCEL (unsigned char) 0x18

// FTDI buffer sizes
#define RX_BUFFER_SIZE 4096
#define TX_BUFFER_SIZE 4096

// Baudrate to use, must match
// baudrate used by bootloader
#define BAUDRATE 19200

// X-modem regular frame type
#define REGULAR_FRAME 0x01

// X-modem EOT frame type
#define END_OF_TRANSMISSION 0x04

#define PAYLOAD_SIZE 128

// marker byte telling the bootloader
// to perform a FLASH upload
#define FLASH_UPLOAD 123

// marker byte telling the bootloader
// to perform a EEPROM upload
#define EEPROM_UPLOAD 124

int retval = EXIT_FAILURE;

struct libusb_device *device = NULL;
struct ftdi_context *ftdiContext = NULL;
FILE *fileToSend = NULL;
struct ftdi_device_list *devlist = NULL;
char manufacturer[128], description[128];

int deviceOpened = 0;
int transmissionRunning = 0;

typedef struct xmodemframe
{
  unsigned char frametype;
  unsigned char blocknum;
  unsigned char invblocknum;
  unsigned char data[PAYLOAD_SIZE];
  unsigned char checksum;
} xmodemframe;

xmodemframe frame;

int write_byte(unsigned char data);

int recv_ack();

void mssleep(int millis) {  
    usleep( millis * 1000 );
}

/*
 * Cleanup function.
 */
void cleanup()
{
    if ( transmissionRunning )
    {
        printf("Cancelling transmission...\n");

        transmissionRunning = 0;
        write_byte( CANCEL );

        if ( ! recv_ack() )
        {
            fprintf(stderr,"Failed to cancel transmission\n");
        }
    }

    if ( deviceOpened ) {
        ftdi_usb_close(ftdiContext);
        deviceOpened =  0;
    }

    if ( devlist != NULL ) {
        ftdi_list_free(&devlist);
        devlist = NULL;
    }
    if ( ftdiContext != NULL ) {
        ftdi_free(ftdiContext);
        ftdiContext = NULL;
    }

    if ( fileToSend != NULL ) {
        fclose(fileToSend);
        fileToSend = NULL;
    }
}

/*
 * SIGINT handler.
 */
void sig_handler(int signo)
{
  if (signo == SIGINT)
  {
    fprintf(stderr,"Caught SIGINT...\n");
    cleanup();
    exit(1);
  }
}

/*
 * Print error message with decoded error
 */
void fail(const char *msg,int ret)
{
    fprintf(stderr, "%s failed: %d (%s)\n", msg, ret, ftdi_get_error_string(ftdiContext));
}

/*
 *  Send frame.
 */
int send_frame(xmodemframe *frame)
{
    int ret;

    ret = ftdi_write_data(ftdiContext,(const char*) frame,sizeof(xmodemframe));
    if ( ret != sizeof(xmodemframe) ) {
        fail("send_frame()", ret);
        return 0;
    }
    return 1;
}

size_t file_to_frame(FILE *file,xmodemframe *frame)
{
       size_t bytesRead;
       int i;

       // clear data block
       for ( i = 0 ; i < PAYLOAD_SIZE ; i++ ) {
           frame->data[i]=0;
       }

       bytesRead = fread( &frame->data,1,PAYLOAD_SIZE,file);
       if ( bytesRead < 0 ) {
           return 0;
       }
       return bytesRead;
}

/*
 * Write a single byte.
 */
int write_byte(unsigned char data)
{
    int ret;
    unsigned char txbuffer;
    txbuffer=data;

    ret = ftdi_write_data(ftdiContext,&txbuffer,1);
    if ( ret != 1 ) {
        fail("write()", ret);
        return 0;
    }
    return 1;
}

/*
 * Read a single byte.
 */
int read_byte(unsigned char *p)
{
    int ret;
    unsigned char rxbuffer;
    int retries = 50;

    while ( retries-- > 0)
    {
        ret = ftdi_read_data(ftdiContext,&rxbuffer,1);
        if ( ret == 1 ) {
            printf("Received %d\n",(int) rxbuffer);
            *p=rxbuffer;
            return 1;
        }
        mssleep(100);
    };

    fprintf(stderr,"read() timeout elapsed\n");

    if ( ret < 0 ) {
        fail("read()", ret);
    }
    return 0;
}

/*
 * Expect to receive a certain byte.
 */
int expect(const char *errorMsg,unsigned char value)
{
    unsigned char buffer;
    int retries = 10;

    while ( retries-- > 0 )
    {
      if ( read_byte( &buffer ) )
      {
          if ( buffer == value ) {
              return 1;
          }
          fprintf(stderr,"Expected %s but received %d",errorMsg,(int) buffer );
          return 0;
      }
    }
    fprintf(stderr,"expect() failed");
    return 0;
}

/*
 * Receive NAK.
 */
int recv_nack()
{
    return expect( "NACK",NACK );
}

/*
 * Receive ACK.
 */
int recv_ack()
{
    return expect( "ACK",ACK );
}

/*
 * Calculate XModem data checksum.
 */
void calc_checksum(xmodemframe *frame)
{
    unsigned char chksum;
    int i;

    chksum=1;
    chksum += frame->blocknum;
    chksum += frame->invblocknum;
    for ( i = 0 ; i < sizeof(frame->data) ; i++ )
    {
      chksum += frame->data[i];
    }
    frame->checksum=chksum;
}

int main(int argc,char **args)
{
    int ret;
    int bytesToTransmit;

    unsigned char rxbuffer;

    if ( argc < 2 ) {
        fprintf(stderr,"Expected one argument (file to send)\n");
        return EXIT_FAILURE;
    }


    if (signal(SIGINT, sig_handler) == SIG_ERR) {
        fprintf(stderr,"Failed to attach SIGINT handler\n");
        return 1;
    }

    fileToSend = fopen( args[1] , "r" );
    if ( fileToSend == NULL ) {
        fprintf(stderr,"Failed to open file %s\n",args[0]);
        return EXIT_FAILURE;
    }

    if ( (ftdiContext = ftdi_new()) == NULL )
    {
        fprintf(stderr, "ftdi_new failed\n");
        fclose(fileToSend);
        return EXIT_FAILURE;
    }

    if ((ret = ftdi_usb_find_all(ftdiContext, &devlist, 0, 0)) < 0)
    {
        fail("ftdi_usb_find_all", ret);
        goto done;
    }

    if ( ret != 1 ) {
        printf("Expected exactly one FTDI device but found %d\n", ret);
        goto done;
    }

    device=devlist->dev;

    printf("Found device.\n");

    if ((ret = ftdi_usb_get_strings(ftdiContext, device, manufacturer, 128, description, 128, NULL, 0)) < 0)
    {
        fail("ftdi_usb_get_strings", ret);
        goto done;
    }

    printf("Manufacturer: %s, Description: %s\n\n", manufacturer, description);


    // open USB device
    if ( (ret = ftdi_usb_open_dev(ftdiContext,  device ) ) < 0)
    {
        fail("open device", ret);
        goto done;
    }

    deviceOpened = 1;

    // setup serial parameters
    if ( (ret = ftdi_set_baudrate(ftdiContext, BAUDRATE ) ) < 0)
    {
        fail("ftdi_set_baud_rate",ret);
        goto done;
    }

    if ( (ret = ftdi_set_line_property(ftdiContext, 8, STOP_BIT_1, NONE) ) < 0)
    {
        fail("ftdi_set_line_property",ret);
        goto done;
    }

    if ( (ret = ftdi_write_data_set_chunksize(ftdiContext, TX_BUFFER_SIZE ) ) < 0 )
    {
        fail("ftdi_write_data_set_chunksize", ret);
        goto done;
    }

    if ( (ret = ftdi_read_data_set_chunksize(ftdiContext, RX_BUFFER_SIZE ) ) < 0 )
    {
        fail("ftdi_read_data_set_chunksize", ret);
        goto done;
    }

    // Transmit file
    while( 1 )
    {
        printf("Sending Init byte...\n");
        if ( ! write_byte( FLASH_UPLOAD ) ) {
            goto done;
        }

        if ( read_byte( &rxbuffer ) )
        {
            if ( rxbuffer == FLASH_UPLOAD )
            {
                break;
            }
            fprintf(stderr, "Expected 123 but got : %d\n", (int) rxbuffer);
            goto done;
        }
    }

    // send zero byte to tell loader to go on
    if ( ! write_byte( 0 ) ) {
        goto done;
    }

    printf("Waiting for NACK to start transmission...\n");

    // wait for NACK from loader that tells us to start sending data
    if ( ! recv_nack() )
    {
        fprintf(stderr,"Failed to receive NAK\n");
        goto done;
    }

    // X-modem transmission
    frame.blocknum=0;
    frame.frametype=REGULAR_FRAME;
    while (1)
    {
        bytesToTransmit = file_to_frame(fileToSend,&frame);
        if ( bytesToTransmit < 0 )
        {
            fprintf(stderr,"I/O error while reading data from file");
            goto done;
        }
        frame.blocknum++;
        frame.invblocknum = ~frame.blocknum;
        calc_checksum(&frame);

        printf("Now sending frame %d with %d bytes\n", frame.blocknum , bytesToTransmit);

        transmissionRunning=1;
        send_frame(&frame);

        if ( ! recv_ack() )
        {
            fprintf(stderr,"Failed to receive ACK\n");
            goto done;
        }

        if ( bytesToTransmit < PAYLOAD_SIZE ) {
            break;
        }
    }

    printf("Sending EOT ...\n");

    write_byte( END_OF_TRANSMISSION );
    transmissionRunning = 0;

    if ( ! recv_ack() )
    {
        fprintf(stderr,"Failed to receive ACK\n");
        goto done;
    }

    printf("Success.\n");

    retval = EXIT_SUCCESS;

done:

    cleanup();

    return retval;
}
