#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

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
    fprintf(stderr, "Event %u (%u, %u)\n", event_code, a, b);
    if (event_code == 0) {
        ctx->clks += a * 256 + b;
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
    assert (1 == fwrite(data, length, 1, stdout));
}


int main(const int argc, const char *argv[]) {
    unsigned head = 0;
    ctx_t ctx_storage = {};
    ctx_t *ctx = &ctx_storage;
    static uint8_t data[0x10004];

    assert (1 == fwrite("\xa1\xb2\x3c\x4d\x00\x02\x00\x04"
                        "\x00\x00\x00\x00\x00\x00\x00\x00"
                        "\x00\x00\xff\xff\x00\x00\x01\x20", 24, 1, stdout));

    for (;;) {
        int r = fread(data, 1, 0x10004 - head, stdin);
        if (r > 0) {
            head += r;
            assert(head <= 0x10004);
            unsigned pos = 0;
            while (1) {
                assert(head >= pos);
                unsigned available = head - pos;
                if (available >= 4) {
                    if (data[pos] == 0xff) {
                        on_event(ctx, data[pos+1], data[pos+2], data[pos+3]);
                        pos += 4;
                    } else {
                        uint16_t length = htons(*((uint16_t *)&data[pos]));
                        uint16_t timestamp = htons(*((uint16_t *)&data[pos+2]));
                        uint8_t *packetdata = data + pos + 4;
                        if (available >= 4 + length) {
                            on_packet(ctx, length, timestamp, packetdata);
                            pos += length + 4 + (length & 1);
                        } else {
                            break;
                        }
                    }
                } else {
                    break;
                }
            }
            memcpy(data, data + pos, head - pos);
            head -= pos;
            pos = 0;
        }
        if (r <= 0) {
            break;
        }
    }

    if (ferror(stdin)) {
        fprintf(stderr, "stdin error\r\n");
        return 1;
    } else if (feof(stdin)) {
        return 0;
    } else {
        fprintf(stderr, "I have no clue why I terminated, sorry.\r\n");
        return 2;
    }
}

