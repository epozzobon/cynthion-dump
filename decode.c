#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h> 
#include <winsock.h>
#else
#include <unistd.h>
#include <arpa/inet.h>
#endif

/* Decode Cynthion dump to pcap format */

struct packet_record_header {
    uint32_t seconds;
    uint32_t nanoseconds;
    uint32_t ori_pkt_len;
    uint32_t pkt_len;
};


typedef struct ctx {
    long long unsigned pkts;
    long long unsigned clks;
} ctx_t;


static void on_event(ctx_t *ctx, uint8_t event_code, uint8_t a, const uint8_t b) {
    if (event_code == 0) {
        ctx->clks += a * 256 + b;
    } else {
        fprintf(stderr, "Unknown event %u (%u, %u)\n", event_code, a, b);
    }
}

static void on_packet(ctx_t *ctx, uint16_t length, uint16_t timestamp, const uint8_t *data) {
    ctx->clks += timestamp;
    ctx->pkts += 1;
    long long unsigned ns = ctx->clks * 100 / 6;
    //fprintf(stderr, "Packet #%llu with %u bytes at %lluns\n", ctx->pkts, length, ns);

    struct packet_record_header hdr;
    hdr.seconds = htonl(ns / 1000000000);
    hdr.nanoseconds = htonl(ns % 1000000000);
    hdr.pkt_len = hdr.ori_pkt_len = htonl(length);
    assert (1 == fwrite(&hdr, 16, 1, stdout));
    if (length) {
        assert (1 == fwrite(data, length, 1, stdout));
    }
}


int main(const int argc, const char *argv[]) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    ctx_t ctx_storage = {};
    ctx_t *ctx = &ctx_storage;
    uint8_t hdr[4];
    static uint8_t data[0x10004];

    assert (1 == fwrite("\xa1\xb2\x3c\x4d\x00\x02\x00\x04"
                        "\x00\x00\x00\x00\x00\x00\x00\x00"
                        "\x00\x00\xff\xff\x00\x00\x01\x20", 24, 1, stdout));

    uint16_t to_read;
    unsigned long long count = 0;
    for (;;) {
        to_read = 4;
        int r = fread(hdr, to_read, 1, stdin);
        if (r != 1) {
            break;
        }

        if (hdr[0] == 0xff) {
            // First byte is 0xff, this is an event
            on_event(ctx, hdr[1], hdr[2], hdr[3]);
        } else {
            // This is an USB packet.
            uint16_t length = hdr[0] * 256 + hdr[1];
            uint16_t timestamp = hdr[2] * 256 + hdr[3];

            if (length >= 0x8000) {
                // I don't believe an USB packet can be this long!
                // Feel free to remove this check if it's actually possible
                fprintf(stderr, "ERROR: Found a %uB USB packet?!", length);
                break;
            }

            if (length > 0) {
                to_read = length + (length & 1);  // +&1 for padding
                r = fread(data, to_read, 1, stdin);
                if (r != 1) {
                    break;
                }
            }
            on_packet(ctx, length, timestamp, data);
            count++;
        }
    }

    fprintf(stderr, "%llu packets decoded.\r\n", count);

    if (ferror(stdin)) {
        fprintf(stderr, "stdin error\r\n");
        return 1;
    } else if (feof(stdin)) {
        return 0;
    } else if (to_read) {
        int r = fread(data, 1, to_read, stdin);
        if (r > 0) {
            fprintf(stderr, "(%u bytes leftover)\r\n", r);
        } else if (r < 0) {
            perror("fread");
        } else {
            fprintf(stderr, "I have no clue why I terminated, sorry.\r\n");
        }
        return 2;
    }
    return 0;
}

