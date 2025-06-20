This is a quick and dirty program to use the Cynthion from the command line with as little overhead as possible.

## Build

Just execute `make` in the project directory


## Setup

Follow the [Cynthion documentation](https://cynthion.readthedocs.io/en/latest/getting_started_packetry.html#usb-analyzer-bitstream) to flash the usb analyzer bitstream:

```bash
cynthion flash analyzer
```

When I tested this, this was the output of `cynthion info --force-offline`:

```
Cynthion version: 0.2.2
Apollo version: 1.1.1
Python version: 3.13.3 (main, Apr  9 2025, 07:44:25) [GCC 14.2.1 20250207]

Found Apollo stub interface!
        Bitstream: USB Analyzer (Cynthion Project)

[...]

Found Cynthion device!
        Hardware: Cynthion r1.4
        Manufacturer: Great Scott Gadgets

```

## Usage

Dump USB traffic and compress it using:
```bash
./cynthion-dump | gzip > dump.gz
```

Then later convert it to a readable pcap file using
```bash
gunzip dump.gz -c | ./cynthion-decode > output.pcap
```
