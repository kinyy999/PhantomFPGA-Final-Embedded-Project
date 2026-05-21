/* SPDX-License-Identifier: MIT */
/*
 * PhantomFPGA Application - userspace implementation
 *
 * This file implements the application-side control and streaming path.
 * It configures the kernel driver, maps the DMA buffer pool, starts frame
 * streaming, reads completed frames, validates them, and optionally forwards
 * valid frames to the TCP viewer.
 *
 * Read phantomfpga_app.h for the class interface and available members.
 * Read phantomfpga_uapi.h for ioctl definitions and data structures.
 *
 * Available members from the base class (PhantomFpgaApp):
 *   dev_fd_       :  FileDescriptor for the device (you set this in open_device)
 *   buffer_pool_  :  MappedMemory for the DMA buffers (you set this in setup_mmap)
 *   config_       :  AppConfig with parsed CLI parameters
 *   stats_        :  AppStats for your counters
 *   tcp_server_   :  TcpServer (may be nullptr if --tcp-server wasn't used)
 *   running_      :  volatile bool, goes false on Ctrl+C
 *
 * Utility classes:
 *   CRC32::compute(data, len)  :  IEEE 802.3 CRC32, returns uint32_t
 *   FileDescriptor(fd)         :  RAII file descriptor, use std::move()
 *   MappedMemory(addr, size)   :  RAII mmap wrapper, use std::move()
 */

#include "phantomfpga_app.h"

#include <cerrno>
#include <poll.h>
#include <sys/ioctl.h>

/* ----------------------------------------------------------------------- */
/* PhantomFpgaAppImpl                                                       */
/*                                                                         */
/* The base class handles CLI parsing, TCP setup, signal handling, and      */
/* cleanup. This derived class implements the device-specific app logic.    */
/* ----------------------------------------------------------------------- */

class PhantomFpgaAppImpl : public PhantomFpgaApp {
protected:

	/*
	 * Open the PhantomFPGA character device.
	 *
	 * The kernel driver exposes /dev/phantomfpga0. The raw file descriptor
	 * is wrapped in FileDescriptor so it is closed automatically during
	 * cleanup.
	 */
	int open_device() override
	{
		/* Open the character device created by the kernel driver */
		int fd = ::open(DEVICE_PATH, O_RDWR);
		if (fd < 0) {
			int err = -errno;
			fprintf(stderr, "Failed to open %s: %s\n", DEVICE_PATH, strerror(errno));
			return err;
		}

		/* Store fd in RAII wrapper so it closes automatically */
		dev_fd_ = FileDescriptor(fd);
		return 0;
	}

	/*
	 * Configure the kernel driver for frame streaming.
	 *
	 * The app sends descriptor count, frame rate, and IRQ coalescing settings
	 * through PHANTOMFPGA_IOCTL_SET_CFG. The driver then allocates DMA
	 * resources, programs the device registers, and initializes the
	 * descriptor ring.
	 */
	int configure_device() override
	{
		struct phantomfpga_config cfg = {};

		cfg.desc_count = config_.desc_count;
		cfg.frame_rate = config_.frame_rate;
		cfg.irq_coalesce_count = DEFAULT_IRQ_COUNT;
		cfg.irq_coalesce_timeout = DEFAULT_IRQ_TIMEOUT;

		if (ioctl(dev_fd_.get(), PHANTOMFPGA_IOCTL_SET_CFG, &cfg) < 0) {
			int err = -errno;
			fprintf(stderr, "Failed to configure device: %s\n", strerror(errno));
			return err;
		}

		return 0;
	}

	/*
	 * Map the coherent DMA buffer pool into userspace.
	 *
	 * The driver reports the buffer layout through GET_BUFFER_INFO. The app
	 * then maps the whole pool with mmap(), so future zero-copy mode can read
	 * frame buffers directly from userspace.
	 */
	int setup_mmap() override
	{
		struct phantomfpga_buffer_info info = {};

		if (ioctl(dev_fd_.get(), PHANTOMFPGA_IOCTL_GET_BUFFER_INFO, &info) < 0) {
			int err = -errno;
			fprintf(stderr, "Failed to get buffer info: %s\n", strerror(errno));
			return err;
		}

		config_.buffer_size = static_cast<uint32_t>(info.buffer_size);

		void* addr = mmap(nullptr,
				  info.total_size,
				  PROT_READ,
				  MAP_SHARED,
				  dev_fd_.get(),
				  0);

		if (addr == MAP_FAILED) {
			int err = -errno;
			fprintf(stderr, "Failed to mmap DMA buffers: %s\n", strerror(errno));
			return err;
		}

		buffer_pool_ = MappedMemory(addr, info.total_size);
		return 0;
	}

	/*
	 * Start frame streaming in the kernel driver and PhantomFPGA device.
	 */
	int start_streaming() override
	{
		if (ioctl(dev_fd_.get(), PHANTOMFPGA_IOCTL_START) < 0) {
			int err = -errno;
			fprintf(stderr, "Failed to start streaming: %s\n", strerror(errno));
			return err;
		}

		return 0;
	}

	int stop_streaming() override
	{
		if (ioctl(dev_fd_.get(), PHANTOMFPGA_IOCTL_STOP) < 0) {
			int err = -errno;
			fprintf(stderr, "Failed to stop streaming: %s\n", strerror(errno));
			return err;
		}

		return 0;
	}

	/*
	 * Main frame-processing loop.
	 *
	 * The loop accepts a TCP client if needed, waits for the device file to
	 * become readable with poll(), reads one frame from the driver, and sends
	 * the frame to process_frame().
	 */
	void main_loop() override
	{
		uint8_t frame_buf[PHANTOMFPGA_FRAME_SIZE];
		uint32_t mmap_index = 0;

		while (running_) {
			if (tcp_server_)
				tcp_server_->try_accept();

			struct pollfd pfd;
			pfd.fd = dev_fd_.get();
			pfd.events = POLLIN;
			pfd.revents = 0;

			const int ret = poll(&pfd, 1, 100);

			if (ret < 0) {
				if (errno == EINTR)
					continue;

				fprintf(stderr, "poll() failed: %s\n", strerror(errno));
				break;
			}

			if (ret == 0)
				continue;

			if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
				fprintf(stderr, "poll() reported device error: revents=0x%x\n",
					pfd.revents);
				break;
			}

			if (!(pfd.revents & POLLIN))
				continue;

			if (config_.zero_copy) {
				if (!buffer_pool_.get() ||
				    config_.buffer_size < PHANTOMFPGA_FRAME_SIZE) {
					fprintf(stderr, "zero-copy buffer pool is not ready\n");
					break;
				}

				const uint8_t* frame =
					static_cast<const uint8_t*>(buffer_pool_.get()) +
					(static_cast<size_t>(mmap_index) * config_.buffer_size);

				process_frame(frame, PHANTOMFPGA_FRAME_SIZE);

				if (ioctl(dev_fd_.get(), PHANTOMFPGA_IOCTL_CONSUME_FRAME) < 0) {
					if (errno == EINTR || errno == EAGAIN)
						continue;

					fprintf(stderr, "CONSUME_FRAME failed: %s\n",
						strerror(errno));
					break;
				}

				mmap_index = (mmap_index + 1) % config_.desc_count;
				continue;
			}

			const ssize_t bytes_read =
				read(dev_fd_.get(), frame_buf, sizeof(frame_buf));

			if (bytes_read < 0) {
				if (errno == EINTR || errno == EAGAIN)
					continue;

				fprintf(stderr, "read() failed: %s\n", strerror(errno));
				break;
			}

			if (bytes_read == 0)
				continue;

			process_frame(frame_buf, static_cast<uint32_t>(bytes_read));
		}
	}

	/*
	 * Process one frame received from the driver.
	 *
	 * The frame is counted, validated, optionally sent to the connected TCP
	 * viewer, and printed in verbose mode.
	 */
	int process_frame(const void* buffer, uint32_t len) override
	{
		stats_.frames_received++;

		const bool valid = validate_frame(buffer, len);

		if (valid) {
			stats_.frames_valid++;

			if (tcp_server_ && tcp_server_->has_client())
				tcp_server_->send_frame(buffer, len);
		}

		if (config_.verbose) {
			uint32_t sequence = 0;

			if (buffer && len >= sizeof(struct phantomfpga_frame_header)) {
				const auto* header =
					static_cast<const struct phantomfpga_frame_header*>(buffer);
				sequence = header->sequence;
			}

			fprintf(stdout,
				"Frame received: seq=%u size=%u valid=%s\n",
				sequence, len, valid ? "yes" : "no");
		}

		return 0;
	}

	/*
	 * Validate one PhantomFPGA frame.
	 *
	 * Validation checks the frame size, magic value, sequence continuity, and
	 * optional CRC32. App-side error counters are updated for each failed
	 * validation category.
	 */
	bool validate_frame(const void* frame, uint32_t frame_size) override
	{
		bool valid = true;

		if (!frame || frame_size != PHANTOMFPGA_FRAME_SIZE)
			return false;

		const auto* header =
			static_cast<const struct phantomfpga_frame_header*>(frame);

		const uint32_t magic = header->magic;
		const uint32_t sequence = header->sequence;

		if (magic != PHANTOMFPGA_FRAME_MAGIC) {
			stats_.magic_errors++;
			valid = false;
		}

		if (stats_.seq_initialized) {
			const uint32_t expected_seq =
				(stats_.last_seq + 1) % PHANTOMFPGA_FRAME_COUNT;

			if (sequence != expected_seq) {
				stats_.seq_errors++;
				valid = false;
			}
		}

		if (config_.validate_crc) {
			const auto* bytes = static_cast<const uint8_t*>(frame);
			const uint32_t* stored_crc_ptr =
				reinterpret_cast<const uint32_t*>(
					bytes + PHANTOMFPGA_FRAME_SIZE - sizeof(uint32_t));

			const uint32_t stored_crc = *stored_crc_ptr;
			const uint32_t computed_crc =
				CRC32::compute(frame, PHANTOMFPGA_FRAME_SIZE - sizeof(uint32_t));

			if (computed_crc != stored_crc) {
				stats_.crc_errors++;
				valid = false;
			}
		}

		stats_.last_seq = sequence;
		stats_.seq_initialized = true;

		return valid;
	}

	/*
	 * Print app, network, and device statistics.
	 *
	 * App counters come from stats_, network counters come from TcpServer,
	 * and device/driver counters are fetched with PHANTOMFPGA_IOCTL_GET_STATS.
	 */
	void print_statistics() override
	{
		fprintf(stdout, "\n=== App Statistics ===\n");
		fprintf(stdout, "Frames received : %lu\n", stats_.frames_received);
		fprintf(stdout, "Frames valid    : %lu\n", stats_.frames_valid);
		fprintf(stdout, "Sequence errors : %lu\n", stats_.seq_errors);
		fprintf(stdout, "Magic errors    : %lu\n", stats_.magic_errors);
		fprintf(stdout, "CRC errors      : %lu\n", stats_.crc_errors);

		if (tcp_server_) {
			const NetStats& net = tcp_server_->stats();
			fprintf(stdout, "\n=== Network Statistics ===\n");
			fprintf(stdout, "Frames sent     : %lu\n", net.frames_sent);
			fprintf(stdout, "Bytes sent      : %lu\n", net.bytes_sent);
			fprintf(stdout, "Send errors     : %lu\n", net.send_errors);
		}

		struct phantomfpga_stats dev_stats = {};
		if (ioctl(dev_fd_.get(), PHANTOMFPGA_IOCTL_GET_STATS, &dev_stats) < 0) {
			fprintf(stderr, "Failed to get device stats: %s\n", strerror(errno));
			return;
		}

		fprintf(stdout, "\n=== Device Statistics ===\n");
		fprintf(stdout, "Frames produced : %llu\n", (unsigned long long)dev_stats.frames_produced);
		fprintf(stdout, "Frames dropped  : %llu\n", (unsigned long long)dev_stats.frames_dropped);
		fprintf(stdout, "Frames consumed : %llu\n", (unsigned long long)dev_stats.frames_consumed);
		fprintf(stdout, "Bytes produced  : %llu\n", (unsigned long long)dev_stats.bytes_produced);
		fprintf(stdout, "Bytes consumed  : %llu\n", (unsigned long long)dev_stats.bytes_consumed);
		fprintf(stdout, "Desc completed  : %u\n", dev_stats.desc_completed);
		fprintf(stdout, "Errors          : %u\n", dev_stats.errors);
		fprintf(stdout, "CRC errors      : %u\n", dev_stats.crc_errors);
		fprintf(stdout, "IRQ count       : %u\n", dev_stats.irq_count);
		fprintf(stdout, "Desc head       : %u\n", dev_stats.desc_head);
		fprintf(stdout, "Desc tail       : %u\n", dev_stats.desc_tail);
		fprintf(stdout, "Current frame   : %u\n", dev_stats.current_frame);
		fprintf(stdout, "Status register : 0x%08x\n", dev_stats.status);
	}
};

/* ----------------------------------------------------------------------- */
/* main()                                                                  */
/* ----------------------------------------------------------------------- */

int main(int argc, char* argv[])
{
	PhantomFpgaAppImpl app;
	return app.run(argc, argv);
}
