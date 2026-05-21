/* SPDX-License-Identifier: MIT */
/*
 * PhantomFPGA Viewer v3.0 - C++ Edition
 *
 * TCP client that connects to the PhantomFPGA app server and displays
 * ASCII frames in the terminal.
 *
 * Architecture:
 *   PhantomFpgaViewer (base)  :  provided infrastructure (don't touch)
 *   PhantomFpgaViewerImpl     :  your implementation (phantomfpga_view_impl.cpp)
 *
 * This viewer is independent of the kernel UAPI header. All frame
 * constants are defined locally. The viewer only needs TCP and a
 * terminal to do its thing.
 */

#ifndef PHANTOMFPGA_VIEW_H
#define PHANTOMFPGA_VIEW_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <array>
#include <string>

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <time.h>

/* ======================================================================== */
/* Frame Constants                                                          */
/* Independent of the kernel header. The viewer only knows TCP frames.      */
/* ======================================================================== */

namespace frame {
	constexpr uint32_t MAGIC       = 0xF00DFACE;
	constexpr size_t   SIZE        = 5120;
	constexpr size_t   DATA_SIZE   = 4995;
	constexpr int      ROWS        = 45;
	constexpr int      COLS        = 110;
	constexpr int      COUNT       = 250;
	constexpr size_t   CRC_OFFSET  = SIZE - 4;  /* CRC32 at the end */
	constexpr size_t   HEADER_SIZE = 16;
	constexpr size_t   DATA_OFFSET = HEADER_SIZE;
	constexpr int      DEFAULT_FPS = 25;
}

/* ======================================================================== */
/* Frame Header                                                             */
/* ======================================================================== */

/* Local definition, NOT from the UAPI header */
struct FrameHeader {
	uint32_t magic;       /* 0xF00DFACE */
	uint32_t sequence;    /* Frame sequence number (0-249, wraps) */
	uint64_t reserved;    /* Must be 0 */
} __attribute__((packed));

/* ======================================================================== */
/* Statistics                                                               */
/* ======================================================================== */

struct ViewerStats {
	uint64_t frames_received = 0;
	uint64_t frames_dropped  = 0;   /* Sequence gaps detected */
	uint64_t crc_errors      = 0;
	uint64_t magic_errors    = 0;
	int32_t  last_sequence   = -1;  /* -1 = no frame yet */
};

/* ======================================================================== */
/* CRC32                                                                    */
/* ======================================================================== */

/* IEEE 802.3 CRC32, same as the device uses */
class CRC32 {
public:
	static uint32_t compute(const void* data, size_t len);

	CRC32() = delete;

private:
	static const std::array<uint32_t, 256> table_;
};

/* ======================================================================== */
/* TCP Client                                                               */
/* ======================================================================== */

/*
 * TCP client connection with RAII cleanup.
 * Connects to the server and provides read_exact() for reliable reads.
 */
class TcpClient {
public:
	TcpClient() = default;
	~TcpClient();

	TcpClient(const TcpClient&) = delete;
	TcpClient& operator=(const TcpClient&) = delete;

	/* Connect to host:port. Returns true on success. */
	bool connect(const std::string& host, int port);

	/* Read exactly len bytes. Handles partial reads and EINTR.
	 * Returns true on success, false on error/EOF.
	 * Checks the running flag via the provided pointer. */
	bool read_exact(void* buf, size_t len, const volatile bool* running);

	int fd() const { return fd_; }
	bool connected() const { return fd_ >= 0; }

private:
	int fd_ = -1;
};

/* ======================================================================== */
/* Terminal                                                                 */
/* ======================================================================== */

/*
 * ANSI terminal control. Restores cursor visibility on destruction,
 * because leaving invisible cursors behind is rude.
 */
class Terminal {
public:
	Terminal() = default;
	~Terminal() { if (cursor_hidden_) show_cursor(); }

	void clear_screen();
	void hide_cursor();
	void show_cursor();
	void cursor_home();

private:
	bool cursor_hidden_ = false;
};

/* ======================================================================== */
/* Viewer Base Class                                                        */
/* ======================================================================== */

/*
 * Base class for the PhantomFPGA viewer application.
 *
 * Provides TCP connection, terminal control, signal handling, and the
 * main viewer loop. Trainees implement the pure virtual methods to
 * handle frame reception, validation, and display.
 *
 * Protected members available to your implementation:
 *   client_        :  TcpClient for receiving frames
 *   terminal_      :  Terminal for ANSI escape sequences
 *   frame_buffer_  :  5120-byte buffer for the current frame
 *   stats_         :  ViewerStats counters
 *   running_       :  false after Ctrl+C
 *   record_path_   :  filename from --record flag (empty = no recording)
 */
class PhantomFpgaViewer {
public:
	PhantomFpgaViewer();
	virtual ~PhantomFpgaViewer();

	/* Entry point: call from main() */
	int run(int argc, char* argv[]);

protected:
	/* --- State available to your implementation --- */
	TcpClient client_;
	Terminal  terminal_;
	std::array<uint8_t, frame::SIZE> frame_buffer_;
	ViewerStats stats_;
	volatile bool running_ = true;
	std::string record_path_;  /* --record filename, empty = no recording */

	/* --- Pure virtual methods: implement in PhantomFpgaViewerImpl --- */

	/* Receive one frame from the server into frame_buffer_.
	 * Returns true on success, false on error/disconnect. */
	virtual bool receive_frame() = 0;

	/* Validate the frame in frame_buffer_ (magic + CRC).
	 * Returns true if valid. */
	virtual bool validate_frame() = 0;

	/* Check sequence continuity, update stats_.frames_dropped. */
	virtual void check_sequence() = 0;

	/* Display the frame data to the terminal. */
	virtual void display_frame() = 0;

	/* Sleep for one frame interval (1/fps seconds). */
	virtual void frame_delay() = 0;

	/* Print final statistics. */
	virtual void print_stats() = 0;

private:
	int parse_arguments(int argc, char* argv[],
	                    std::string& host, int& port);
	void usage(const char* prog);
	int run_viewer();
	static void signal_handler(int sig);
	static PhantomFpgaViewer* instance_;
};

#endif /* PHANTOMFPGA_VIEW_H */
