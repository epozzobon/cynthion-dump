#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>

/*
This program dumps usb data from the Cynthion to stdout as fast as possible.
We don't care about decoding the data because that can be done later,
the objective is to be able to collect USB data for hours while piping it into gzip.
*/

#define TRANSFERS_COUNT 4
#define TRANSFER_SIZE 0x4000

volatile static long long unsigned cumsum = 0;
volatile static int caught_signal = 0;
volatile static int do_exit = 0;

struct transfer_in {
    int index;
    struct libusb_transfer *xfr;
    uint8_t buf[TRANSFER_SIZE];
};

static struct transfer_in transfers[TRANSFERS_COUNT];


static void on_signal(int sig) {
	caught_signal = sig;
    do_exit = 1;
}


static int cynthion_get_speeds(libusb_device_handle *cynthion, uint8_t *speeds) {
    assert(cynthion != NULL);

    const uint8_t bmRequestType = 0xc1;
    const uint8_t bRequest = 2;
    const uint16_t wValue = 0;
    const uint16_t wIndex = 0;
    uint8_t data[64];
    const uint16_t wLength = 64;
    const unsigned timeout = 1000;
    int err = libusb_control_transfer(cynthion,
        bmRequestType, bRequest, wValue, 
        wIndex, data, wLength, timeout);
    if (err < 0) {
        fprintf(stderr, "libusb_control_transfer: %s\r\n", libusb_error_name(err));
    } else if (err != 1) {
        fprintf(stderr, "libusb_control_transfer: Unexpected data size received (%d)\r\n", err);
    } else {
        fprintf(stderr, "Available speeds: 0x%02x\r\n", data[0]);
        *speeds = data[0];
    }
    return err;
}


static int cynthion_start_capture(libusb_device_handle *cynthion, int speed) {
    assert(0 <= speed && speed <= 3);
    assert(cynthion != NULL);

    const uint8_t bmRequestType = 0x41;
    const uint8_t bRequest = 1;
    const uint16_t wValue = 1 | (speed << 1);
    const uint16_t wIndex = 0;
    uint8_t data[0];
    const uint16_t wLength = 0;
    const unsigned timeout = 1000;
    int err = libusb_control_transfer(cynthion,
        bmRequestType, bRequest, wValue, 
        wIndex, data, wLength, timeout);
    if (err < 0) {
        fprintf(stderr, "libusb_control_transfer: %s\r\n", libusb_error_name(err));
    }
    return err;
}


static int cynthion_stop_capture(libusb_device_handle *cynthion) {
    assert(cynthion != NULL);

    const uint8_t bmRequestType = 0x41;
    const uint8_t bRequest = 1;
    const uint16_t wValue = 0;
    const uint16_t wIndex = 0;
    uint8_t data[0];
    const uint16_t wLength = 0;
    const unsigned timeout = 1000;
    int err = libusb_control_transfer(cynthion,
        bmRequestType, bRequest, wValue, 
        wIndex, data, wLength, timeout);
    if (err < 0) {
        fprintf(stderr, "libusb_control_transfer: %s\r\n", libusb_error_name(err));
    }
    return err;
}


static void on_transfer_complete(struct libusb_transfer *xfr) {
    static int last_transfer_index = -1;

    // Assert that transfers stay in order, though I think this is redundant
    struct transfer_in *usr = xfr->user_data;
    assert((last_transfer_index + 1) % TRANSFERS_COUNT == usr->index || last_transfer_index == -1);
    last_transfer_index = usr->index;

	if (xfr->status != LIBUSB_TRANSFER_COMPLETED) {
		fprintf(stderr, "transfer status %d\r\n", xfr->status);
        do_exit = 3;
        return;
	}

    cumsum += xfr->actual_length;

    if (xfr->actual_length <= 0) {
		fprintf(stderr, "transfer length %d\r\n", xfr->actual_length);
        do_exit = 4;
        return;
    }

    int w = fwrite(xfr->buffer, xfr->actual_length, 1, stdout);
    if (w != 1) {
        perror("fwrite");
        do_exit = 5;
        return;
    }

	if (libusb_submit_transfer(xfr) < 0) {
		fprintf(stderr, "error re-submitting URB\r\n");
        do_exit = 7;
        return;
	}
}

static void cynthion_start_transfers(libusb_device_handle *cynthion)
{
    for (unsigned i = 0; i < TRANSFERS_COUNT; i++)
    {
        transfers[i].index = i;
        libusb_fill_bulk_transfer(transfers[i].xfr, cynthion, 0x81,
                                  transfers[i].buf, TRANSFER_SIZE,
                                  on_transfer_complete, &transfers[i], 0);
        int err = libusb_submit_transfer(transfers[i].xfr);
        if (err < 0)
        {
            fprintf(stderr, "libusb_submit_transfer: %s\r\n", libusb_error_name(err));
        }
    }
}

static int dump_from_handle(libusb_device_handle *cynthion) {
    assert(cynthion != NULL);

    uint8_t speeds;
    cynthion_get_speeds(cynthion, &speeds);
    cynthion_start_capture(cynthion, 0);
    cynthion_start_transfers(cynthion);

    int err = 0;
    while (!do_exit) {
        err = libusb_handle_events(NULL);
        if (err != LIBUSB_SUCCESS) {
            fprintf(stderr, "libusb_handle_events: %s\r\n", libusb_error_name(err));
            break;
        }
        fprintf(stderr, "total read: %lluB\r", cumsum);
    }
    if (caught_signal != 0) {
        fprintf(stderr, "Stopped due to signal \"%s\"\r\n", strsignal(caught_signal));
    }

    fprintf(stderr, "total read: %lluB\r\n", cumsum);
    cynthion_stop_capture(cynthion);
    return err;
}

static int dump_from_device(libusb_device *cynthion_dev) {
    libusb_device_handle *cynthion = NULL;
    int r = libusb_open(cynthion_dev, &cynthion);
    if (r != 0) {
        fprintf(stderr, "libusb_open: %s\r\n", libusb_error_name(r));
        return r;
    }

    if (cynthion == NULL) {
        fprintf(stderr, "Unexpected libusb error\r\n");
        return -1;
    }

    dump_from_handle(cynthion);
    libusb_close(cynthion);
    return 0;
}


int main(const int argc, const char *argv[]) {
    signal(SIGABRT, &on_signal);
	signal(SIGINT, &on_signal);
	signal(SIGTERM, &on_signal);

    int r = libusb_init_context(NULL, NULL, 0);
    if (r < 0) {
        fprintf(stderr, "Error initializing libusb.\r\n");
        return r;
    }

    for (unsigned i = 0; i < TRANSFERS_COUNT; i++) {
        struct libusb_transfer *xfr = libusb_alloc_transfer(0);
        if (!xfr) {
            errno = ENOMEM;
            fprintf(stderr, "Out of memory for USB transfers!\r\n");
            return 1;
        }
        transfers[i].xfr = xfr;
    }

    libusb_device **devs;
    int cnt = libusb_get_device_list(NULL, &devs);
    if (cnt < 0) {
        libusb_exit(NULL);
        fprintf(stderr, "Error getting USB devices list.\r\n");
        return 1;
    }

    for (int i = 0; devs[i]; i++) {
        libusb_device *dev = devs[i];
        struct libusb_device_descriptor desc;
        r = libusb_get_device_descriptor(dev, &desc);
        if (r >= 0) {
            if (desc.idVendor == 0x1d50 && desc.idProduct == 0x615b) {
                fprintf(stderr, "Cynthion found!\r\n");
                dump_from_device(dev);
            }
        } else {
            fprintf(stderr, "Error getting USB device descriptor from %p.\r\n", dev);
        }
    }

    for (unsigned i = 0; i < TRANSFERS_COUNT; i++) {
        libusb_free_transfer(transfers[i].xfr);
    }

    libusb_free_device_list(devs, 1);
    libusb_exit(NULL);
    return 0;
}

