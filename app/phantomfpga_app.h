/* SPDX-License-Identifier: MIT */
/*
 * PhantomFPGA Userspace Application v3.0 - C++ Edition
 *
 * This header defines the application framework for receiving frames from
 * the PhantomFPGA kernel driver and streaming them over TCP to a viewer.
 *
 * Architecture:
 *   PhantomFpgaApp (base)  :  provided infrastructure (don't touch)
 *   PhantomFpgaAppImpl     :  your implementation (in phantomfpga_app_impl.cpp)
 *
 * The base class handles CLI parsing, TCP server, signal handling, and
 * cleanup. You implement the pure virtual methods to talk to the driver.
 */

#ifndef PHANTOMFPGA_APP_H
#define PHANTOMFPGA_APP_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <array>
#include <memory>

#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

/* The UAPI header is C, wrap it for C++ */
extern "C" {
#include "phantomfpga_uapi.h"
}

/* Device node path */
static constexpr const char* DEVICE_PATH = "/dev/phantomfpga0";

/* Default configuration */
static constexpr uint32_t DEFAULT_FRAME_RATE  = 25;
static constexpr uint32_t DEFAULT_DESC_COUNT  = 256;
static constexpr uint16_t DEFAULT_IRQ_COUNT   = 8;
static constexpr uint16_t DEFAULT_IRQ_TIMEOUT = 40000; /* 40ms = 1 frame @ 25fps */
static constexpr uint16_t DEFAULT_TCP_PORT    = 5000;

/* ======================================================================== */
/* RAII Wrappers                                                            */
/* ======================================================================== */

/*
 * RAII wrapper for POSIX file descriptors.
 * Move-only. Closes on destruction. No exceptions, no drama.
 */
class FileDescriptor {
public:
	FileDescriptor() = default;
	explicit FileDescriptor(int fd) : fd_(fd) {}
	~FileDescriptor() { if (fd_ >= 0) ::close(fd_); }

	/* Move-only. Copying a file descriptor is a recipe for double-close. */
	FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.fd_) {
		other.fd_ = -1;
	}
	FileDescriptor& operator=(FileDescriptor&& other) noexcept {
		if (this != &other) {
			if (fd_ >= 0) ::close(fd_);
			fd_ = other.fd_;
			other.fd_ = -1;
		}
		return *this;
	}
	FileDescriptor(const FileDescriptor&) = delete;
	FileDescriptor& operator=(const FileDescriptor&) = delete;

	int get() const { return fd_; }
	int release() { int f = fd_; fd_ = -1; return f; }
	bool valid() const { return fd_ >= 0; }
	explicit operator bool() const { return valid(); }

private:
	int fd_ = -1;
};

/*
 * RAII wrapper for mmap'd memory regions.
 * Move-only. Calls munmap on destruction.
 */
class MappedMemory {
public:
	MappedMemory() = default;
	MappedMemory(void* addr, size_t size) : addr_(addr), size_(size) {}
	~MappedMemory() {
		if (addr_ != MAP_FAILED && addr_ != nullptr)
			::munmap(addr_, size_);
	}

	MappedMemory(MappedMemory&& other) noexcept
		: addr_(other.addr_), size_(other.size_) {
		other.addr_ = MAP_FAILED;
		other.size_ = 0;
	}
	MappedMemory& operator=(MappedMemory&& other) noexcept {
		if (this != &other) {
			if (addr_ != MAP_FAILED && addr_ != nullptr)
				::munmap(addr_, size_);
			addr_ = other.addr_;
			size_ = other.size_;
			other.addr_ = MAP_FAILED;
			other.size_ = 0;
		}
		return *this;
	}
	MappedMemory(const MappedMemory&) = delete;
	MappedMemory& operator=(const MappedMemory&) = delete;

	void* get() const { return addr_; }
	size_t size() const { return size_; }
	bool valid() const { return addr_ != MAP_FAILED && addr_ != nullptr; }

private:
	void* addr_ = MAP_FAILED;
	size_t size_ = 0;
};

/* ======================================================================== */
/* CRC32                                                                    */
/* ======================================================================== */

/*
 * IEEE 802.3 CRC32, same polynomial as Ethernet and the FPGA device.
 * Static-only class. No instances needed, just call CRC32::compute().
 */
class CRC32 {
public:
	static uint32_t compute(const void* data, size_t len);

	CRC32() = delete;

private:
	static const std::array<uint32_t, 256> table_;
};

/* ======================================================================== */
/* Configuration and Statistics                                             */
/* ======================================================================== */

/* Parsed from command-line arguments */
struct AppConfig {
	uint32_t desc_count  = DEFAULT_DESC_COUNT;
	uint32_t frame_rate  = DEFAULT_FRAME_RATE;
	uint32_t buffer_size = 0;     /* filled by setup_mmap */
	uint16_t tcp_port    = DEFAULT_TCP_PORT;
	bool verbose         = false;
	bool stats_only      = false;
	bool validate_crc    = true;
	bool zero_copy       = false;
};

/* Application-level counters */
struct AppStats {
	uint64_t frames_received = 0;
	uint64_t frames_valid    = 0;
	uint64_t seq_errors      = 0;
	uint64_t magic_errors    = 0;
	uint64_t crc_errors      = 0;
	uint32_t last_seq        = 0;
	bool     seq_initialized = false;
	struct timespec start_time = {};
};

/* Network counters (inside TcpServer) */
struct NetStats {
	uint64_t frames_sent  = 0;
	uint64_t bytes_sent   = 0;
	uint64_t send_errors  = 0;
};

/* ======================================================================== */
/* TCP Server                                                               */
/* ======================================================================== */

/*
 * Single-client TCP server for streaming frames to the viewer.
 * Manages a listen socket and one connected client at a time.
 */
class TcpServer {
public:
	explicit TcpServer(uint16_t port);
	~TcpServer() = default; /* FileDescriptors handle cleanup */

	TcpServer(const TcpServer&) = delete;
	TcpServer& operator=(const TcpServer&) = delete;

	/* Try to accept a pending connection (non-blocking) */
	void try_accept();

	/* Send a length-prefixed frame to the connected client.
	 * Returns true if sent, false if no client or error. */
	bool send_frame(const void* data, size_t len);

	bool has_client() const { return client_fd_.valid(); }
	uint16_t port() const { return port_; }
	const NetStats& stats() const { return stats_; }

private:
	FileDescriptor listen_fd_;
	FileDescriptor client_fd_;
	uint16_t port_;
	NetStats stats_;
};

/* ======================================================================== */
/* Application Base Class                                                   */
/* ======================================================================== */

/*
 * Base class for the PhantomFPGA userspace application.
 *
 * This class provides all the infrastructure: CLI parsing, TCP server,
 * signal handling, and cleanup. Trainees implement the pure virtual
 * methods in PhantomFpgaAppImpl (in phantomfpga_app_impl.cpp).
 *
 * Protected members are available to the derived class:
 *   dev_fd_      :  device file descriptor (wrap with FileDescriptor)
 *   buffer_pool_ :  mmap'd DMA buffers (wrap with MappedMemory)
 *   config_      :  parsed configuration
 *   stats_       :  runtime statistics
 *   tcp_server_  :  TCP server (may be nullptr if not requested)
 *   running_     :  set to false by signal handler on Ctrl+C
 */
class PhantomFpgaApp {
public:
	PhantomFpgaApp();
	virtual ~PhantomFpgaApp();

	/* Entry point: call from main() */
	int run(int argc, char* argv[]);

protected:
	/* --- State available to your implementation --- */
	FileDescriptor dev_fd_;
	MappedMemory   buffer_pool_;
	AppConfig      config_;
	AppStats       stats_;
	std::unique_ptr<TcpServer> tcp_server_;
	volatile bool  running_ = true;

	/* --- Pure virtual methods: implement these in PhantomFpgaAppImpl --- */

	/* Open the device node. Store the fd in dev_fd_. */
	virtual int open_device() = 0;

	/* Configure the device via ioctl(SET_CFG). Use config_ for parameters. */
	virtual int configure_device() = 0;

	/* Get buffer info and mmap the DMA buffers into buffer_pool_. */
	virtual int setup_mmap() = 0;

	/* Start frame streaming via ioctl(START). */
	virtual int start_streaming() = 0;

	/* Stop frame streaming via ioctl(STOP). */
	virtual int stop_streaming() = 0;

	/* Main processing loop: poll for frames, process them, stream to viewer. */
	virtual void main_loop() = 0;

	/* Process a single frame: validate, update stats, stream. */
	virtual int process_frame(const void* buffer, uint32_t len) = 0;

	/* Validate a frame: check magic, sequence, CRC. */
	virtual bool validate_frame(const void* frame, uint32_t frame_size) = 0;

	/* Print device and application statistics. */
	virtual void print_statistics() = 0;

private:
	int parse_arguments(int argc, char* argv[]);
	void print_usage(const char* prog_name);
	void cleanup();
	static void signal_handler(int sig);
	static PhantomFpgaApp* instance_;
};

#endif /* PHANTOMFPGA_APP_H */
