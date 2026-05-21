/* SPDX-License-Identifier: MIT */
/*
 * PhantomFPGA Viewer - Infrastructure (COMPLETE)
 *
 * This file contains all the provided infrastructure code. Trainees
 * should read this to understand the framework, but should NOT edit it.
 * All trainee work goes in phantomfpga_view_impl.cpp.
 */

#include "phantomfpga_view.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>

/* ======================================================================== */
/* CRC32 Implementation                                                     */
/* ======================================================================== */

static constexpr std::array<uint32_t, 256> build_crc32_table()
{
	std::array<uint32_t, 256> table = {};
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t crc = i;
		for (int j = 0; j < 8; j++) {
			if (crc & 1)
				crc = (crc >> 1) ^ 0xEDB88320;
			else
				crc >>= 1;
		}
		table[i] = crc;
	}
	return table;
}

const std::array<uint32_t, 256> CRC32::table_ = build_crc32_table();

uint32_t CRC32::compute(const void* data, size_t len)
{
	auto bytes = static_cast<const uint8_t*>(data);
	uint32_t crc = 0xFFFFFFFF;
	for (size_t i = 0; i < len; i++)
		crc = (crc >> 8) ^ table_[(crc ^ bytes[i]) & 0xFF];
	return crc ^ 0xFFFFFFFF;
}

/* ======================================================================== */
/* TcpClient Implementation                                                 */
/* ======================================================================== */

TcpClient::~TcpClient()
{
	if (fd_ >= 0) ::close(fd_);
}

bool TcpClient::connect(const std::string& host, int port)
{
	char port_str[16];
	snprintf(port_str, sizeof(port_str), "%d", port);

	struct addrinfo hints = {};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	struct addrinfo* result = nullptr;
	int ret = getaddrinfo(host.c_str(), port_str, &hints, &result);
	if (ret != 0) {
		fprintf(stderr, "Error: getaddrinfo: %s\n", gai_strerror(ret));
		return false;
	}

	/* Try each address until one works */
	for (auto rp = result; rp != nullptr; rp = rp->ai_next) {
		fd_ = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (fd_ < 0)
			continue;

		if (::connect(fd_, rp->ai_addr, rp->ai_addrlen) == 0)
			break; /* connected */

		::close(fd_);
		fd_ = -1;
	}

	freeaddrinfo(result);

	if (fd_ < 0) {
		fprintf(stderr, "Error: could not connect to %s:%d\n",
		        host.c_str(), port);
		return false;
	}

	fprintf(stderr, "[*] Connected to %s:%d\n", host.c_str(), port);
	return true;
}

bool TcpClient::read_exact(void* buf, size_t len, const volatile bool* running)
{
	auto ptr = static_cast<uint8_t*>(buf);
	size_t remaining = len;

	while (remaining > 0 && *running) {
		ssize_t n = ::read(fd_, ptr, remaining);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		if (n == 0)
			return false; /* EOF */
		ptr += n;
		remaining -= static_cast<size_t>(n);
	}

	return remaining == 0;
}

/* ======================================================================== */
/* Terminal Implementation                                                  */
/* ======================================================================== */

void Terminal::clear_screen()
{
	printf("\033[2J\033[H");
	fflush(stdout);
}

void Terminal::hide_cursor()
{
	printf("\033[?25l");
	fflush(stdout);
	cursor_hidden_ = true;
}

void Terminal::show_cursor()
{
	printf("\033[?25h");
	fflush(stdout);
	cursor_hidden_ = false;
}

void Terminal::cursor_home()
{
	printf("\033[H");
}

/* ======================================================================== */
/* PhantomFpgaViewer Implementation                                         */
/* ======================================================================== */

PhantomFpgaViewer* PhantomFpgaViewer::instance_ = nullptr;

PhantomFpgaViewer::PhantomFpgaViewer()
{
	frame_buffer_.fill(0);
}

PhantomFpgaViewer::~PhantomFpgaViewer() = default;

void PhantomFpgaViewer::signal_handler(int /* sig */)
{
	if (instance_)
		instance_->running_ = false;
}

void PhantomFpgaViewer::usage(const char* prog)
{
	fprintf(stderr,
		"Usage: %s [options] [host] [port]\n"
		"\n"
		"Connect to a PhantomFPGA server and display frame data.\n"
		"\n"
		"  host           Server hostname (default: localhost)\n"
		"  port           Server port (default: 5000)\n"
		"  --record FILE  Save raw frames to FILE for validation\n"
		"  -h             Show this help\n"
		"\n"
		"Terminal must be at least %dx%d.\n",
		prog, frame::COLS, frame::ROWS);
}

int PhantomFpgaViewer::parse_arguments(int argc, char* argv[],
                                        std::string& host, int& port)
{
	host = "localhost";
	port = 5000;

	/* First pass: extract flags */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			return 1;
		}
		if (strcmp(argv[i], "--record") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "Error: --record requires a filename\n");
				return -1;
			}
			record_path_ = argv[++i];
		}
	}

	/* Second pass: positional args (skip consumed flags) */
	int pos = 0;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--record") == 0) {
			i++; /* skip filename */
			continue;
		}
		if (argv[i][0] == '-')
			continue;

		if (pos == 0)
			host = argv[i];
		else if (pos == 1) {
			port = atoi(argv[i]);
			if (port < 1 || port > 65535) {
				fprintf(stderr, "Error: invalid port %s\n", argv[i]);
				return -1;
			}
		}
		pos++;
	}

	return 0;
}

int PhantomFpgaViewer::run_viewer()
{
	terminal_.clear_screen();
	terminal_.hide_cursor();

	while (running_) {
		if (!receive_frame())
			break;

		stats_.frames_received++;

		if (!validate_frame())
			continue;

		check_sequence();
		display_frame();
		frame_delay();
	}

	terminal_.show_cursor();
	printf("\n");
	print_stats();

	return 0;
}

int PhantomFpgaViewer::run(int argc, char* argv[])
{
	std::string host;
	int port;

	int ret = parse_arguments(argc, argv, host, port);
	if (ret != 0)
		return (ret > 0) ? 0 : 1;

	/* Install signal handlers */
	instance_ = this;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	fprintf(stderr,
		"[*] PhantomFPGA Viewer\n"
		"[*] Connecting to %s:%d...\n"
		"[*] Make sure your terminal is at least %dx%d\n",
		host.c_str(), port, frame::COLS, frame::ROWS);

	if (!record_path_.empty())
		fprintf(stderr, "[*] Recording to %s\n", record_path_.c_str());

	if (!client_.connect(host, port))
		return 1;

	int result = run_viewer();

	return result;
}
