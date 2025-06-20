#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>


/*
This program dumps usb data from the Cynthion to stdout as fast as possible.
We don't care about decoding the data because that can be done later,
the objective is to be able to collect USB data for hours while piping it into gzip.
*/

volatile static int caught_signal = 0;


static void on_signal(int sig) {
	caught_signal = sig;
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


static int cynthion_recv_capture_block(libusb_device_handle *cynthion, uint8_t *data, int length) {
    assert(cynthion != NULL);
    assert(data != NULL);
    assert(length > 0);

    const uint8_t endpoint = 0x81;
    int transferred;
    const unsigned timeout = 1000;
    int err = libusb_bulk_transfer(cynthion,
        endpoint, data, length, &transferred, timeout);
    if (err == LIBUSB_ERROR_TIMEOUT) {
        return 0;
    }
    if (err != 0) {
        fprintf(stderr, "libusb_bulk_transfer: %s\r\n", libusb_error_name(err));
        return -1;
    } else {
        return transferred;
    }
}


static int dump_from_handle(libusb_device_handle *cynthion) {
    assert(cynthion != NULL);

    uint8_t data[0x4000];
    const int length = 0x4000;
    uint8_t speeds;
    cynthion_get_speeds(cynthion, &speeds);
    cynthion_start_capture(cynthion, 0);
    long long unsigned cumsum = 0;
    int err = 0;
    while (caught_signal == 0) {
        int rx = cynthion_recv_capture_block(cynthion, data, length);
        if (rx < 0) {
            err = -1;
            break;
        }
        if (rx == 0) {
            fprintf(stderr, "Cynthion timeout\r\n");
            err = -2;
            break;
        }
        int r = fwrite(data, rx, 1, stdout);
        if (r != 1) {
            perror("fwrite");
            err = -3;
            break;
        }
        cumsum += rx;
        fprintf(stderr, "total read: %lluB\r", cumsum);
    }
    if (caught_signal > 0) {
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

    libusb_free_device_list(devs, 1);
    libusb_exit(NULL);
    return 0;
}

