/* SPDX-License-Identifier: MIT */
/*
 * PhantomFPGA Viewer - YOUR IMPLEMENTATION
 *
 * This is the file you need to edit. Implement the 7 TODO methods below
 * to receive, validate, display, and record frames from the device.
 *
 * Read phantomfpga_view.h for the class interface and frame constants.
 *
 * Available members from the base class (PhantomFpgaViewer):
 *   client_        :  TcpClient with read_exact(buf, len, &running_)
 *   terminal_      :  Terminal with clear_screen(), cursor_home(), etc.
 *   frame_buffer_  :  std::array<uint8_t, 5120> for the current frame
 *   stats_         :  ViewerStats (frames_received, frames_dropped, etc.)
 *   running_       :  volatile bool, goes false on Ctrl+C
 *   record_path_   :  filename from --record flag (empty = no recording)
 *
 * Frame layout (frame::SIZE = 5120 bytes):
 *   Offset 0:    FrameHeader (16 bytes) :  magic, sequence, reserved
 *   Offset 16:   Payload (4995 bytes)  :  frame data
 *   Offset 5116: CRC32 (4 bytes)       :  IEEE 802.3
 *
 * Utility:
 *   CRC32::compute(data, len)  :  returns uint32_t
 */

#include "phantomfpga_view.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
/* ----------------------------------------------------------------------- */
/* PhantomFpgaViewerImpl: YOUR CODE GOES HERE                              */
/*                                                                         */
/* Implement the 7 TODO methods below. The base class handles TCP          */
/* connection, terminal setup, signal handling, and the main loop.         */
/* ----------------------------------------------------------------------- */

class PhantomFpgaViewerImpl : public PhantomFpgaViewer {
private:
	FILE* record_file_ = nullptr;

protected:

	bool receive_frame() override
	{
		uint32_t wire_len = 0;

		/*
		 * First read the 4-byte length prefix.
		 * The app sends this length in network byte order.
		 */
		if (!client_.read_exact(reinterpret_cast<uint8_t*>(&wire_len),
					sizeof(wire_len),
					&running_)) {
			return false;
		}

		/*
		 * Convert the length from network byte order to host byte order.
		 */
		const uint32_t len = ntohl(wire_len);

		/*
		 * The PhantomFPGA protocol must always send exactly one full frame:
		 * 5120 bytes.
		 */
		if (len != frame::SIZE) {
			fprintf(stderr,
				"Invalid frame length: got %u, expected %zu\n",
				len,
				frame::SIZE);
			return false;
		}

		/*
		 * Read the complete frame payload into the viewer frame buffer.
		 * read_exact() handles partial TCP reads internally.
		 */
		if (!client_.read_exact(frame_buffer_.data(),
					frame::SIZE,
					&running_)) {
			return false;
		}

		/*
		 * If recording is enabled, save the raw frame exactly as received.
		 * This happens before validation, so even corrupted frames are saved.
		 */
		if (!record_path_.empty()) {
			if (!record_file_) {
				record_file_ = fopen(record_path_.c_str(), "wb");

				if (!record_file_) {
					fprintf(stderr,
						"Failed to open recording file: %s\n",
						record_path_.c_str());
					return false;
				}
			}

			if (fwrite(frame_buffer_.data(), 1, frame::SIZE, record_file_) !=
			    frame::SIZE) {
				fprintf(stderr,
					"Failed to write frame to recording file\n");
				return false;
			}
		}

		return true;
	}

	bool validate_frame() override
	{
		const auto* hdr =
			reinterpret_cast<const FrameHeader*>(frame_buffer_.data());

		bool valid = true;

		/*
		 * Check the frame magic value.
		 * If it is wrong, this is not a valid PhantomFPGA frame.
		 */
		if (hdr->magic != frame::MAGIC) {
			stats_.magic_errors++;
			valid = false;
		}

		/*
		 * Compute CRC over the frame bytes before the CRC field.
		 * The CRC field itself starts at frame::CRC_OFFSET.
		 */
		const uint32_t computed_crc =
			CRC32::compute(frame_buffer_.data(), frame::CRC_OFFSET);

		/*
		 * Read the stored CRC from the end of the frame.
		 * memcpy avoids alignment problems from reading uint32_t directly
		 * out of a uint8_t buffer.
		 */
		uint32_t stored_crc = 0;
		memcpy(&stored_crc,
		       frame_buffer_.data() + frame::CRC_OFFSET,
		       sizeof(stored_crc));

		if (computed_crc != stored_crc) {
			stats_.crc_errors++;
			valid = false;
		}

		return valid;
	}

	void check_sequence() override
	{
		/*
		 * The sequence number is stored inside the frame header.
		 * frame_buffer_ contains the full 5120-byte frame, and the header
		 * starts at offset 0, so we can interpret the beginning of the buffer
		 * as a FrameHeader.
		 */
		const auto* hdr =
			reinterpret_cast<const FrameHeader*>(frame_buffer_.data());

		const uint32_t current = hdr->sequence;

		/*
		 * stats_.last_sequence starts as -1.
		 * That means no previous frame was seen yet, so the first frame
		 * cannot be checked for a gap.
		 */
		if (stats_.last_sequence != -1) {
			/*
			 * The expected sequence is previous + 1.
			 * We use modulo frame::COUNT because the FPGA sequence wraps
			 * after the last frame back to 0.
			 */
			const uint32_t expected =
				(static_cast<uint32_t>(stats_.last_sequence) + 1) %
				frame::COUNT;

			/*
			 * If the current sequence is not the expected one,
			 * then one or more frames were skipped/dropped.
			 */
			if (current != expected) {
				/*
				 * Calculate how many frames were dropped.
				 *
				 * The + frame::COUNT keeps the result positive even when
				 * the sequence wrapped around from the end back to 0.
				 */
				const uint32_t dropped =
					(current - expected + frame::COUNT) %
					frame::COUNT;

				stats_.frames_dropped += dropped;
			}
		}

		/*
		 * Save the current sequence number so the next frame can be checked
		 * against this one.
		 */
		stats_.last_sequence = current;
	}

	void display_frame() override
	{
		/*
		 * Move the terminal cursor back to the top-left corner.
		 *
		 * The viewer repeatedly redraws frames in the same terminal area.
		 * Without moving the cursor home, every new frame would be printed
		 * after the previous one instead of replacing it visually.
		 */
		terminal_.cursor_home();

		/*
		 * The real visible ASCII image starts after the 16-byte frame header.
		 *
		 * frame_buffer_.data() points to the beginning of the full frame.
		 * frame::DATA_OFFSET skips the header and reaches the ASCII payload.
		 *
		 * frame::DATA_SIZE tells fwrite exactly how many payload bytes to print.
		 * The payload already contains newline characters, so we do not need
		 * to manually print row by row.
		 */
		fwrite(frame_buffer_.data() + frame::DATA_OFFSET,
		       1,
		       frame::DATA_SIZE,
		       stdout);

		/*
		 * Force the terminal to show the frame immediately.
		 *
		 * stdout can be buffered, so fflush() makes sure the user sees the
		 * frame right now instead of waiting for the buffer to fill.
		 */
		fflush(stdout);
	}

	void frame_delay() override
	{
		/*
		 * Sleep for one frame period.
		 * 1 second = 1,000,000,000 nanoseconds.
		 */
		struct timespec ts = {
			0,
			1000000000L / frame::DEFAULT_FPS
		};

		nanosleep(&ts, nullptr);
	}

	void print_stats() override
	{
		if (record_file_) {
			fclose(record_file_);
			record_file_ = nullptr;
		}

		fprintf(stderr, "\n=== Viewer Statistics ===\n");
		fprintf(stderr, "Frames received : %llu\n",
			static_cast<unsigned long long>(stats_.frames_received));
		fprintf(stderr, "Frames dropped  : %llu\n",
			static_cast<unsigned long long>(stats_.frames_dropped));
		fprintf(stderr, "CRC errors      : %llu\n",
			static_cast<unsigned long long>(stats_.crc_errors));
		fprintf(stderr, "Magic errors    : %llu\n",
			static_cast<unsigned long long>(stats_.magic_errors));
	}


};

/* ----------------------------------------------------------------------- */
/* main()                                                                  */
/* ----------------------------------------------------------------------- */

int main(int argc, char* argv[])
{
	PhantomFpgaViewerImpl viewer;
	return viewer.run(argc, argv);
}
