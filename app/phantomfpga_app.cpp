/* SPDX-License-Identifier: MIT */
/*
 * PhantomFPGA Application - Infrastructure (COMPLETE)
 *
 * This file contains all the provided infrastructure code. Trainees
 * should read this to understand the framework, but should NOT edit it.
 * All trainee work goes in phantomfpga_app_impl.cpp.
 */

#include "phantomfpga_app.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstdio>

#include <getopt.h>
#include <sys/ioctl.h>

/* ======================================================================== */
/* CRC32 Implementation                                                     */
/* ======================================================================== */

/* Pre-computed CRC32 lookup table (IEEE 802.3 polynomial) */
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
/* TcpServer Implementation                                                 */
/* ======================================================================== */

TcpServer::TcpServer(uint16_t port) : port_(port)
{
	int fd = ::socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return;
	}
	listen_fd_ = FileDescriptor(fd);

	int opt = 1;
	setsockopt(listen_fd_.get(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(port);

	if (::bind(listen_fd_.get(), reinterpret_cast<struct sockaddr*>(&addr),
	           sizeof(addr)) < 0) {
		perror("bind");
		listen_fd_ = FileDescriptor(); /* close it */
		return;
	}

	if (::listen(listen_fd_.get(), 1) < 0) {
		perror("listen");
		listen_fd_ = FileDescriptor();
		return;
	}

	/* Non-blocking accept */
	int flags = fcntl(listen_fd_.get(), F_GETFL, 0);
	fcntl(listen_fd_.get(), F_SETFL, flags | O_NONBLOCK);

	fprintf(stderr, "[*] TCP server listening on port %u\n", port);
}

void TcpServer::try_accept()
{
	if (!listen_fd_.valid() || client_fd_.valid())
		return;

	struct sockaddr_in client_addr = {};
	socklen_t addr_len = sizeof(client_addr);

	int fd = ::accept(listen_fd_.get(),
	                  reinterpret_cast<struct sockaddr*>(&client_addr),
	                  &addr_len);
	if (fd < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			perror("accept");
		return;
	}

	client_fd_ = FileDescriptor(fd);
	fprintf(stderr, "[*] Viewer connected from %s:%u\n",
	        inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
}

bool TcpServer::send_frame(const void* data, size_t len)
{
	if (!client_fd_.valid())
		return false;

	/* Length-prefixed protocol: 4-byte network-order length, then data */
	uint32_t net_len = htonl(static_cast<uint32_t>(len));
	if (::send(client_fd_.get(), &net_len, sizeof(net_len), MSG_NOSIGNAL) <= 0) {
		if (errno == EPIPE || errno == ECONNRESET) {
			fprintf(stderr, "[*] Viewer disconnected\n");
			client_fd_ = FileDescriptor(); /* close */
		} else {
			stats_.send_errors++;
		}
		return false;
	}

	ssize_t sent = ::send(client_fd_.get(), data, len, MSG_NOSIGNAL);
	if (sent <= 0) {
		if (errno == EPIPE || errno == ECONNRESET) {
			fprintf(stderr, "[*] Viewer disconnected\n");
			client_fd_ = FileDescriptor();
		} else {
			stats_.send_errors++;
		}
		return false;
	}

	stats_.frames_sent++;
	stats_.bytes_sent += static_cast<uint64_t>(sent);
	return true;
}

/* ======================================================================== */
/* PhantomFpgaApp Implementation                                            */
/* ======================================================================== */

PhantomFpgaApp* PhantomFpgaApp::instance_ = nullptr;

PhantomFpgaApp::PhantomFpgaApp() = default;
PhantomFpgaApp::~PhantomFpgaApp() = default;

void PhantomFpgaApp::signal_handler(int /* sig */)
{
	if (instance_)
		instance_->running_ = false;
}

void PhantomFpgaApp::print_usage(const char* prog)
{
	fprintf(stderr,
		"Usage: %s [OPTIONS]\n"
		"\n"
		"PhantomFPGA frame receiver and TCP streamer.\n"
		"\n"
		"Options:\n"
		"  -r, --frame-rate RATE     Frames per second (1-60, default %u)\n"
		"  -n, --desc-count COUNT    Descriptor ring size (power of 2, default %u)\n"
		"  -c, --no-crc              Disable CRC validation\n"
		"  -S, --stats               Print device stats and exit\n"
		"  -V, --verbose             Verbose output\n"
		"      --tcp-server [PORT]   Start TCP server (default port %u)\n"
		"      --zero-copy           Use mmap zero-copy path (bonus)\n"
		"      --version             Show version and exit\n"
		"  -h, --help                Show this help\n"
		"\n"
		"Frame format: %u bytes per frame, %u frames total (%.1f sec @ %u fps)\n"
		"  Header:  16 bytes (magic 0x%X, sequence, reserved)\n"
		"  Payload: %u bytes (ASCII %dx%d)\n"
		"  CRC32:   4 bytes (at offset %u)\n"
		"\n"
		"Examples:\n"
		"  %s --tcp-server              Stream frames on port %u\n"
		"  %s --tcp-server 8080 -V      Stream on port 8080, verbose\n"
		"  %s --stats                   Just print device statistics\n",
		prog,
		DEFAULT_FRAME_RATE, DEFAULT_DESC_COUNT, DEFAULT_TCP_PORT,
		PHANTOMFPGA_FRAME_SIZE, PHANTOMFPGA_FRAME_COUNT,
		static_cast<float>(PHANTOMFPGA_FRAME_COUNT) / DEFAULT_FRAME_RATE,
		DEFAULT_FRAME_RATE, PHANTOMFPGA_FRAME_MAGIC,
		PHANTOMFPGA_FRAME_DATA_SIZE, PHANTOMFPGA_FRAME_COLS,
		PHANTOMFPGA_FRAME_ROWS,
		PHANTOMFPGA_FRAME_SIZE - 4,
		prog, DEFAULT_TCP_PORT, prog, prog);
}

int PhantomFpgaApp::parse_arguments(int argc, char* argv[])
{
	static const struct option long_options[] = {
		{"frame-rate",  required_argument, nullptr, 'r'},
		{"desc-count",  required_argument, nullptr, 'n'},
		{"no-crc",      no_argument,       nullptr, 'c'},
		{"stats",       no_argument,       nullptr, 'S'},
		{"verbose",     no_argument,       nullptr, 'V'},
		{"tcp-server",  optional_argument, nullptr, 'T'},
		{"zero-copy",   no_argument,       nullptr, 'Z'},
		{"version",     no_argument,       nullptr, 'v'},
		{"help",        no_argument,       nullptr, 'h'},
		{nullptr, 0, nullptr, 0}
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "r:n:cSVh", long_options, nullptr)) != -1) {
		switch (opt) {
		case 'r':
			config_.frame_rate = static_cast<uint32_t>(strtoul(optarg, nullptr, 10));
			if (config_.frame_rate < 1 || config_.frame_rate > 60) {
				fprintf(stderr, "Error: frame rate must be 1-60\n");
				return -1;
			}
			break;
		case 'n':
			config_.desc_count = static_cast<uint32_t>(strtoul(optarg, nullptr, 10));
			if (config_.desc_count < 4 || config_.desc_count > 4096 ||
			    (config_.desc_count & (config_.desc_count - 1)) != 0) {
				fprintf(stderr, "Error: desc count must be power of 2, 4-4096\n");
				return -1;
			}
			break;
		case 'c':
			config_.validate_crc = false;
			break;
		case 'S':
			config_.stats_only = true;
			break;
		case 'V':
			config_.verbose = true;
			break;
		case 'T':
			config_.tcp_port = optarg
				? static_cast<uint16_t>(strtoul(optarg, nullptr, 10))
				: DEFAULT_TCP_PORT;
			break;
		case 'Z':
			config_.zero_copy = true;
			break;
		case 'v':
			fprintf(stderr, "PhantomFPGA App v3.0 (C++ edition)\n");
			return 1; /* signal to exit cleanly */
		case 'h':
		default:
			print_usage(argv[0]);
			return (opt == 'h') ? 1 : -1;
		}
	}

	return 0;
}

void PhantomFpgaApp::cleanup()
{
	tcp_server_.reset();
	buffer_pool_ = MappedMemory(); /* munmap */
	dev_fd_ = FileDescriptor();    /* close */
}

int PhantomFpgaApp::run(int argc, char* argv[])
{
	/* Parse command line */
	int ret = parse_arguments(argc, argv);
	if (ret != 0)
		return (ret > 0) ? 0 : 1;

	/* Install signal handlers */
	instance_ = this;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* Start TCP server if requested */
	if (config_.tcp_port > 0)
		tcp_server_ = std::make_unique<TcpServer>(config_.tcp_port);

	/* Open the device */
	ret = open_device();
	if (ret < 0) {
		fprintf(stderr, "Error: failed to open device (%d)\n", ret);
		cleanup();
		return 1;
	}

	/* Stats-only mode: just print and exit */
	if (config_.stats_only) {
		print_statistics();
		cleanup();
		return 0;
	}

	/* Configure and start */
	ret = configure_device();
	if (ret < 0) {
		fprintf(stderr, "Error: failed to configure device (%d)\n", ret);
		cleanup();
		return 1;
	}

	ret = setup_mmap();
	if (ret < 0) {
		fprintf(stderr, "Error: failed to setup mmap (%d)\n", ret);
		cleanup();
		return 1;
	}

	ret = start_streaming();
	if (ret < 0) {
		fprintf(stderr, "Error: failed to start streaming (%d)\n", ret);
		cleanup();
		return 1;
	}

	/* Record start time */
	clock_gettime(CLOCK_MONOTONIC, &stats_.start_time);

	fprintf(stderr, "[*] Streaming at %u fps, %u descriptors",
	        config_.frame_rate, config_.desc_count);
	if (tcp_server_)
		fprintf(stderr, ", TCP port %u", config_.tcp_port);
	fprintf(stderr, "\n[*] Press Ctrl+C to stop\n");

	/* Run the main loop */
	main_loop();

	/* Shut down */
	stop_streaming();

	fprintf(stderr, "\n");
	print_statistics();

	cleanup();
	return 0;
}
