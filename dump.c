#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h> 
#endif
#include <libusb-1.0/libusb.h>

/*
This program dumps usb data from the Cynthion to stdout as fast as possible.
We don't care about decoding the data because that can be done later,
the objective is to be able to collect USB data for hours while piping it into gzip.
*/

#define TRANSFERS_COUNT 4
#define TRANSFER_SIZE 0x4000

volatile static long long unsigned cumsum = 0;
volatile static sig_atomic_t caught_signal = 0;
volatile static sig_atomic_t do_exit = 0;

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
    // Even though only 1 byte is returned, packetry asks for 64 so I do the same
    const uint16_t wLength = 64;
    const unsigned timeout = 1000;
    int err = libusb_control_transfer(cynthion,
        bmRequestType, bRequest, wValue, 
        wIndex, data, wLength, timeout);
    if (err < 0) {
        fprintf(stderr, "libusb_control_transfer: %s\r\n", libusb_error_name(err));
        return err;
    } else if (err != 1) {
        fprintf(stderr, "libusb_control_transfer: Unexpected data size received (%d)\r\n", err);
        return err;
    } else {
        fprintf(stderr, "Available speeds: 0x%02x\r\n", data[0]);
        *speeds = data[0];
        return 0;
    }
}


static int cynthion_start_capture(libusb_device_handle *cynthion, int speed) {
    assert(0 <= speed && speed <= 3);
    assert(cynthion != NULL);

    const uint8_t bmRequestType = 0x41;
    const uint8_t bRequest = 1;
    const uint16_t wValue = 1 | (speed << 1);
    const uint16_t wIndex = 0;
    const uint16_t wLength = 0;
    const unsigned timeout = 1000;

    int err = libusb_control_transfer(cynthion,
        bmRequestType, bRequest, wValue, 
        wIndex, NULL, wLength, timeout);
    if (err < 0) {
        fprintf(stderr, "libusb_control_transfer: %s\r\n", libusb_error_name(err));
        return err;
    }
    return 0;
}


static int cynthion_stop_capture(libusb_device_handle *cynthion) {
    assert(cynthion != NULL);

    const uint8_t bmRequestType = 0x41;
    const uint8_t bRequest = 1;
    const uint16_t wValue = 0;
    const uint16_t wIndex = 0;
    const uint16_t wLength = 0;
    const unsigned timeout = 1000;
    int err = libusb_control_transfer(cynthion,
        bmRequestType, bRequest, wValue, 
        wIndex, NULL, wLength, timeout);
    if (err < 0) {
        fprintf(stderr, "libusb_control_transfer: %s\r\n", libusb_error_name(err));
        return err;
    }
    return 0;
}


static int on_transfer_complete_impl(struct libusb_transfer *xfr) {
    static int last_transfer_index = -1;
    if (do_exit) {
        // An error already happened in another transfer, so don't output to stdout
        return 1;
    }

    struct transfer_in *usr = xfr->user_data;
    int last = last_transfer_index;
    last_transfer_index = usr->index;

    if ((last + 1) % TRANSFERS_COUNT != usr->index && last != -1) {
		fprintf(stderr, "ERROR: out of order transfers (%d after %d)!\r\n", usr->index, last);
        return 2;
    }

	if (xfr->status != LIBUSB_TRANSFER_COMPLETED) {
		fprintf(stderr, "transfer status %d\r\n", xfr->status);
        return 3;
	}

    cumsum += xfr->actual_length;

    if (xfr->actual_length <= 0) {
		fprintf(stderr, "transfer length %d\r\n", xfr->actual_length);
        return 4;
    }

    int w = fwrite(xfr->buffer, xfr->actual_length, 1, stdout);
    if (w != 1) {
        perror("fwrite");
        return 5;
    }

    int err = libusb_submit_transfer(xfr);
	if (err != 0) {
		fprintf(stderr, "error re-submitting URB\r\n");
        return err;
	}

    return 0;
}


static void on_transfer_complete(struct libusb_transfer *xfr) {
    int err = on_transfer_complete_impl(xfr);
    if (err != 0) {
        do_exit = err;
    }
}

static int cynthion_start_transfers(libusb_device_handle *cynthion)
{
    for (unsigned i = 0; i < TRANSFERS_COUNT; i++)
    {
        assert(transfers[i].xfr != NULL);
        transfers[i].index = i;
        libusb_fill_bulk_transfer(transfers[i].xfr, cynthion, 0x81,
                                  transfers[i].buf, TRANSFER_SIZE,
                                  on_transfer_complete, &transfers[i], 0);
        int err = libusb_submit_transfer(transfers[i].xfr);
        if (err != 0)
        {
            fprintf(stderr, "libusb_submit_transfer: %s\r\n", libusb_error_name(err));
            return err;
        }
    }
    return 0;
}

static int cynthion_transfer_loop(libusb_device_handle *cynthion) {
    int err = cynthion_start_transfers(cynthion);
    while (err == 0 && do_exit == 0) {
        err = libusb_handle_events_completed(NULL, (int*) &do_exit);
        if (err != 0) {
            fprintf(stderr, "libusb_handle_events: %s\r\n", libusb_error_name(err));
        } else {
            fprintf(stderr, "total read: %lluB\r", cumsum);
        }
    }

    if (caught_signal != 0) {
#ifdef _WIN32
        fprintf(stderr, "Stopped due to signal \"%d\"\r\n", caught_signal);
#else
        fprintf(stderr, "Stopped due to signal \"%s\"\r\n", strsignal(caught_signal));
#endif
    }

    for (unsigned i = 0; i < TRANSFERS_COUNT; i++) {
        // Cancel transfers, even the ones that failed to start
        int r = libusb_cancel_transfer(transfers[i].xfr);
        if (err == 0) {
            // Only propagate error if we didn't already have an error
            err = r;
        }
    }

    fprintf(stderr, "total read: %lluB\r\n", cumsum);
    return err;
}

static int cynthion_dump(libusb_device_handle *cynthion) {
    assert(cynthion != NULL);

    int err;
    uint8_t speeds;

    err = cynthion_get_speeds(cynthion, &speeds);
    if (err != 0) {
        return err;
    }

    err = cynthion_start_capture(cynthion, 0);
    if (err != 0) {
        return err;
    }

    int r = cynthion_transfer_loop(cynthion);

    err = cynthion_stop_capture(cynthion);
    return err ? err : r;
}

static libusb_device_handle *open_cynthion() {
    libusb_device **devs;
    int cnt = libusb_get_device_list(NULL, &devs);
    if (cnt < 0) {
        libusb_exit(NULL);
        fprintf(stderr, "Error getting USB devices list.\r\n");
        return NULL;
    }

    libusb_device_handle *cynthion = NULL;
    for (int i = 0; devs[i]; i++) {
        libusb_device *dev = devs[i];
        struct libusb_device_descriptor desc;
        int r = libusb_get_device_descriptor(dev, &desc);
        if (r >= 0) {
            if (desc.idVendor == 0x1d50 && desc.idProduct == 0x615b) {
                int r = libusb_open(dev, &cynthion);
                if (r != 0) {
                    fprintf(stderr, "libusb_open: %s\r\n", libusb_error_name(r));
                    cynthion = NULL;
                } else {
                    assert(cynthion != NULL);
                    break;
                }
            }
        } else {
            fprintf(stderr, "Error getting USB device descriptor from %p.\r\n", dev);
        }
    }

    libusb_free_device_list(devs, 1);

    return cynthion;
}

static int claim_and_dump(libusb_device_handle *cynthion)
{
    int err = libusb_claim_interface(cynthion, 0);
    if (err != 0) {
        fprintf(stderr, "libusb_claim_interface: %s\r\n", libusb_error_name(err));
        return err;
    }
    cynthion_dump(cynthion);
    err = libusb_release_interface(cynthion, 0);
    if (err != 0) {
        fprintf(stderr, "libusb_release_interface: %s\r\n", libusb_error_name(err));
        return err;
    }
    return 0;
}


int main(const int argc, const char *argv[]) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    setvbuf(stdout, NULL, _IOFBF, 64 * 1024);
    signal(SIGABRT, &on_signal);
	signal(SIGINT, &on_signal);
	signal(SIGTERM, &on_signal);

    int r = libusb_init_context(NULL, NULL, 0);
    if (r < 0) {
        fprintf(stderr, "Error initializing libusb.\r\n");
        return r;
    }

    for (unsigned i = 0; i < TRANSFERS_COUNT; i++) {
        transfers[i].xfr = libusb_alloc_transfer(0);
        if (!transfers[i].xfr) {
            // If any of the transfers fails to allocate, cancel execution entirely
            errno = ENOMEM;
            fprintf(stderr, "Out of memory for USB transfers!\r\n");
            for (unsigned j = 0; j < i; j++) {
                libusb_free_transfer(transfers[j].xfr);
            }
            return 1;
        }
    }

    libusb_device_handle *cynthion = open_cynthion();

    if (cynthion != NULL) {
        claim_and_dump(cynthion);
        libusb_close(cynthion);
    } else {
        fprintf(stderr, "Cynthion NOT found!\r\n");
    }

    for (unsigned i = 0; i < TRANSFERS_COUNT; i++) {
        libusb_free_transfer(transfers[i].xfr);
    }
    libusb_exit(NULL);
    return 0;
}
