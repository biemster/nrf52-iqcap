// gcc -O3 test_iso_stream.c -o test_iso_stream -lusb-1.0

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>
#include <libusb-1.0/libusb.h>

#define VID 0xcafe
#define PID 0x4000
#define EP_IN_ISO 0x88
#define EP_OUT_CMD 0x01
#define PKT_SIZE 1000

#define NUM_XFERS 16
#define PKTS_PER_XFER 8
#define STREAM_BUF_SIZE (1000 * 100)

static volatile int keep_running = 1;
static libusb_context *ctx = NULL;

static int count_1000 = 0;
static int drops_1000 = 0;
static double sum_1000 = 0;
static int min_1000 = 999999;
static int max_1000 = 0;

static int count_100 = 0;
static double sum_100 = 0;
static int min_100 = 999999;
static int max_100 = 0;

static int garbage_bytes = 0;
static int last_ts_main = -1;
static int last_ts_sub = -1;

static uint8_t stream_buf[STREAM_BUF_SIZE];
static int stream_len = 0;

double get_time() {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec + (tv.tv_usec / 1000000.0);
}

void int_handler(int dummy) {
	keep_running = 0;
}

static void process_frame(const uint8_t *frame) {
	int ts_main = frame[2] | (frame[3] << 8);
	if (last_ts_main != -1) {
		int delta_m = (ts_main - last_ts_main) & 0xFFFF;
		count_1000++;
		sum_1000 += delta_m;
		if (delta_m < min_1000) min_1000 = delta_m;
		if (delta_m > max_1000) max_1000 = delta_m;
		if (delta_m > 1800) drops_1000++;
	}
	last_ts_main = ts_main;

	for (int b = 0; b < 10; b++) {
		int offset = b * 100;
		int ts_sub = frame[offset + 2] | (frame[offset + 3] << 8);
		if (last_ts_sub != -1) {
			int delta_s = (ts_sub - last_ts_sub) & 0xFFFF;
			count_100++;
			sum_100 += delta_s;
			if (delta_s < min_100) min_100 = delta_s;
			if (delta_s > max_100) max_100 = delta_s;
		}
		last_ts_sub = ts_sub;
	}
}

static void LIBUSB_CALL xfer_cb(struct libusb_transfer *xfer) {
	if (xfer->status != LIBUSB_TRANSFER_COMPLETED && xfer->status != LIBUSB_TRANSFER_TIMED_OUT) {
		return; 
	}

	for (int i = 0; i < xfer->num_iso_packets; i++) {
		struct libusb_iso_packet_descriptor *pack = &xfer->iso_packet_desc[i];
		
		if (pack->status == LIBUSB_TRANSFER_COMPLETED && pack->actual_length > 0) {
			uint8_t *buf = libusb_get_iso_packet_buffer_simple(xfer, i);
			int len = pack->actual_length;

			if (len != 1000) {
				printf("[USB Packet] Unexpected size: %d bytes (Status: %d)\n", len, pack->status);
			}

			if (stream_len + len <= STREAM_BUF_SIZE) {
				memcpy(stream_buf + stream_len, buf, len);
				stream_len += len;
			}
			else {
				stream_len = 0; 
			}
		}
		else if (pack->status != LIBUSB_TRANSFER_COMPLETED && pack->status != LIBUSB_TRANSFER_TIMED_OUT) {
			printf("[USB Frame Error] Packet slot %d failed with status %d\n", i, pack->status);
			stream_len = 0;
		}
	}

	// Process sliding window with granular garbage mapping
	int p = 0;
	while (p + 1000 <= stream_len) {
		// 1. Check for a VALID data frame (starts with 0xBB 0x55)
		if (stream_buf[p] == 0xBB && stream_buf[p+1] == 0x55 &&
			stream_buf[p+100] == 0xAA && stream_buf[p+101] == 0x55 &&
			stream_buf[p+200] == 0xAA && stream_buf[p+201] == 0x55) {
			
			process_frame(&stream_buf[p]);
			p += 1000;
		} 
		// 2. Check for a DUMMY padding frame (starts with 0xDD 0x55)
		else if (stream_buf[p] == 0xDD && stream_buf[p+1] == 0x55 &&
				 stream_buf[p+100] == 0xAA && stream_buf[p+101] == 0x55 &&
				 stream_buf[p+200] == 0xAA && stream_buf[p+201] == 0x55) {
			
			// Silently discard the dummy frame and advance
			p += 1000;
		} 
		// 3. Misalignment. Hunt down where the NEXT frame begins.
		else {
			int next_sync_idx = -1;
			for (int s = p + 1; s + 1000 <= stream_len; s++) {
				if ((stream_buf[s] == 0xBB || stream_buf[s] == 0xDD) && stream_buf[s+1] == 0x55 &&
					stream_buf[s+100] == 0xAA && stream_buf[s+101] == 0x55 &&
					stream_buf[s+200] == 0xAA && stream_buf[s+201] == 0x55) {
					next_sync_idx = s;
					break;
				}
			}

			if (next_sync_idx != -1) {
				int garbage_len = next_sync_idx - p;
				printf("[Stream Error] Skipped %d bytes of garbage to re-sync\n", garbage_len);
				garbage_bytes += garbage_len;
				p = next_sync_idx; // Snap alignment directly to the next frame
			}
			else {
				// No valid sync words found in the remainder of this buffer.
				// Stop parsing and wait for more USB data to append.
				break;
			}
		}
	}

	if (p < stream_len) {
		memmove(stream_buf, stream_buf + p, stream_len - p);
		stream_len -= p;
	} else {
		stream_len = 0;
	}

	if (keep_running) {
		libusb_submit_transfer(xfer);
	}
}

int main() {
	signal(SIGINT, int_handler);

	if (libusb_init(&ctx) < 0) {
		fprintf(stderr, "Failed to initialize libusb\n");
		return 1;
	}

	libusb_device_handle *dev = libusb_open_device_with_vid_pid(ctx, VID, PID);
	if (!dev) {
		fprintf(stderr, "Error: nRF52 USB device not found.\n");
		libusb_exit(ctx);
		return 1;
	}

	libusb_set_auto_detach_kernel_driver(dev, 1);
	if (libusb_claim_interface(dev, 0) < 0) {
		fprintf(stderr, "Failed to claim interface 0\n");
		libusb_close(dev);
		libusb_exit(ctx);
		return 1;
	}

	printf("[+] Starting stream on 2402 MHz...\n");
	uint8_t cmd_start[4] = {0xcc, 2, 0x01, 0x01};
	int transferred;
	libusb_bulk_transfer(dev, EP_OUT_CMD, cmd_start, sizeof(cmd_start), &transferred, 1000);

	struct libusb_transfer *xfers[NUM_XFERS];
	uint8_t *buffers[NUM_XFERS];

	for (int i = 0; i < NUM_XFERS; i++) {
		xfers[i] = libusb_alloc_transfer(PKTS_PER_XFER);
		buffers[i] = malloc(PKT_SIZE * PKTS_PER_XFER);
		
		libusb_fill_iso_transfer(xfers[i], dev, EP_IN_ISO, buffers[i], 
								 PKT_SIZE * PKTS_PER_XFER, PKTS_PER_XFER, 
								 xfer_cb, NULL, 1000);
		
		libusb_set_iso_packet_lengths(xfers[i], PKT_SIZE);
		libusb_submit_transfer(xfers[i]);
	}

	double last_report = get_time();
	struct timeval tv = {0, 100000}; 

	while (keep_running) {
		libusb_handle_events_timeout(ctx, &tv);

		double now = get_time();
		if (now - last_report >= 0.5) {
			printf("\n--- USB Timing Report (%.2fs) ---\n", now - last_report);
			
			if (count_1000 > 0) {
				printf("1000B ISO Frames : Count=%4d | Drops=%3d | Avg=%6.1f µs | Min=%5d µs | Max=%5d µs\n",
					   count_1000, drops_1000, sum_1000 / count_1000, min_1000, max_1000);
			} else {
				printf("1000B ISO Frames : No data\n");
			}

			if (count_100 > 0) {
				printf(" 100B Sub-Frames : Count=%4d |           | Avg=%6.1f µs | Min=%5d µs | Max=%5d µs\n",
					   count_100, sum_100 / count_100, min_100, max_100);
			} else {
				printf(" 100B Sub-Frames : No data\n");
			}

			if (garbage_bytes > 0) {
				printf("Unmatched Garbage Bytes Discarded: %d\n", garbage_bytes);
			}

			count_1000 = 0; drops_1000 = 0; sum_1000 = 0; min_1000 = 999999; max_1000 = 0;
			count_100 = 0; sum_100 = 0; min_100 = 999999; max_100 = 0;
			garbage_bytes = 0;
			last_report = now;
		}
	}

	printf("\n[+] Stopping...\n");
	uint8_t cmd_stop[4] = {0xcf, 0x00, 0x00, 0x00};
	libusb_bulk_transfer(dev, EP_OUT_CMD, cmd_stop, sizeof(cmd_stop), &transferred, 1000);

	for (int i = 0; i < NUM_XFERS; i++) {
		libusb_cancel_transfer(xfers[i]); 
	}
	
	for(int i=0; i<5; i++) libusb_handle_events_timeout(ctx, &tv);

	for (int i = 0; i < NUM_XFERS; i++) {
		libusb_free_transfer(xfers[i]);
		free(buffers[i]);
	}

	libusb_release_interface(dev, 0);
	libusb_close(dev);
	libusb_exit(ctx);

	return 0;
}
