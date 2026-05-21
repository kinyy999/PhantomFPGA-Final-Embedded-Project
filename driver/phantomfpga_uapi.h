/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * PhantomFPGA IOCTL Interface v3.0
 *
 * This header defines the interface between userspace applications
 * and the PhantomFPGA kernel driver. Include this in your application
 * to communicate with the /dev/phantomfpga device.
 *
 * v3.0 streams pre-built data frames (fixed 5120 bytes each).
 * Build a driver, stream frames over TCP, display them on the host.
 *
 * Usage:
 *   #include "phantomfpga_uapi.h"
 *   int fd = open("/dev/phantomfpga0", O_RDWR);
 *   ioctl(fd, PHANTOMFPGA_IOCTL_SET_CFG, &config);
 *   ioctl(fd, PHANTOMFPGA_IOCTL_START);
 *   // Read frames or mmap the buffer
 */

#ifndef PHANTOMFPGA_UAPI_H
#define PHANTOMFPGA_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* Device node path */
#define PHANTOMFPGA_DEV_NAME    "phantomfpga"
#define PHANTOMFPGA_DEV_PATH    "/dev/phantomfpga"

/* Magic number for ioctl commands - using 'P' for Phantom */
#define PHANTOMFPGA_IOC_MAGIC   'P'

/* ------------------------------------------------------------------------ */
/* Frame Constants                                                          */
/* These are fixed - the device streams pre-built data frames               */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_FRAME_MAGIC     0xF00DFACE
#define PHANTOMFPGA_FRAME_SIZE      5120    /* Bytes per frame */
#define PHANTOMFPGA_FRAME_COUNT     250     /* Total frames (10 sec @ 25fps) */
#define PHANTOMFPGA_FRAME_DATA_SIZE 4995    /* ASCII data portion */
#define PHANTOMFPGA_FRAME_ROWS      45
#define PHANTOMFPGA_FRAME_COLS      110

/* ------------------------------------------------------------------------ */
/* Configuration Structure                                                  */
/* Simplified for fixed-size frame streaming                                */
/* ------------------------------------------------------------------------ */

/*
 * struct phantomfpga_config - Device configuration
 *
 * @desc_count:            Number of descriptors in the ring (power of 2)
 * @frame_rate:            Frames per second (1-60, default 25)
 * @irq_coalesce_count:    Generate IRQ after N completions
 * @irq_coalesce_timeout:  Generate IRQ after N microseconds
 * @reserved:              Reserved for future use, must be zero
 *
 * Frame size is fixed at 5120 bytes. No size configuration needed.
 * Use with PHANTOMFPGA_IOCTL_SET_CFG before starting streaming.
 */
struct phantomfpga_config {
	__u32 desc_count;           /* Descriptor count (power of 2, 4-4096) */
	__u32 frame_rate;           /* Frames per second (1-60, default 25) */
	__u16 irq_coalesce_count;   /* IRQ after N completions */
	__u16 irq_coalesce_timeout; /* IRQ timeout in microseconds */
	__u32 reserved[4];          /* Reserved, must be zero */
};

/* ------------------------------------------------------------------------ */
/* Statistics Structure                                                     */
/* ------------------------------------------------------------------------ */

/*
 * struct phantomfpga_stats - Device statistics
 *
 * @frames_produced:    Frames transmitted by device
 * @frames_dropped:     Frames dropped (no descriptor available)
 * @frames_consumed:    Frames consumed by driver/userspace
 * @bytes_produced:     Total bytes transmitted
 * @bytes_consumed:     Total bytes consumed
 * @desc_completed:     Descriptors completed
 * @errors:             Error counter
 * @crc_errors:         CRC validation failures (driver-side)
 * @irq_count:          Total interrupt count
 * @desc_head:          Current descriptor head (submitted)
 * @desc_tail:          Current descriptor tail (completed)
 * @current_frame:      Current frame index (0-249)
 * @status:             Device status register value
 * @reserved:           Reserved for future use
 *
 * Use with PHANTOMFPGA_IOCTL_GET_STATS to see what's going on.
 * Check frames_dropped if playback stutters.
 */
struct phantomfpga_stats {
	__u64 frames_produced;    /* Frames transmitted */
	__u64 frames_dropped;     /* Frames dropped (backpressure) */
	__u64 frames_consumed;    /* Frames consumed */
	__u64 bytes_produced;     /* Total bytes transmitted */
	__u64 bytes_consumed;     /* Total bytes consumed */
	__u32 desc_completed;     /* Descriptors completed */
	__u32 errors;             /* Error counter */
	__u32 crc_errors;         /* CRC validation failures */
	__u32 irq_count;          /* Total IRQ count */
	__u32 desc_head;          /* Current head index */
	__u32 desc_tail;          /* Current tail index */
	__u32 current_frame;      /* Current frame index (0-249) */
	__u32 status;             /* Device status register */
	__u32 reserved[4];        /* Reserved for future use */
};

/* ------------------------------------------------------------------------ */
/* Buffer Info Structure                                                    */
/* ------------------------------------------------------------------------ */

/*
 * struct phantomfpga_buffer_info - Buffer information for mmap
 *
 * @buffer_size:    Size of each descriptor buffer (>= frame size)
 * @buffer_count:   Number of buffers (same as descriptor count)
 * @total_size:     Total mappable size
 * @frame_size:     Size of each frame (5120 bytes)
 * @mmap_offset:    Offset to use with mmap() - always 0
 * @reserved:       Reserved for future use
 *
 * Use with PHANTOMFPGA_IOCTL_GET_BUFFER_INFO before mmap().
 */
struct phantomfpga_buffer_info {
	__u64 buffer_size;     /* Size of each buffer */
	__u64 buffer_count;    /* Number of buffers */
	__u64 total_size;      /* Total mappable size */
	__u32 frame_size;      /* Frame size (5120) */
	__u32 reserved[5];     /* Reserved for future use */
};

/* ------------------------------------------------------------------------ */
/* Fault Injection Configuration                                            */
/* For testing error handling in your code                                  */
/* ------------------------------------------------------------------------ */

/*
 * struct phantomfpga_fault_cfg - Fault injection configuration
 *
 * @inject_flags:   Which faults to enable (bitmask)
 * @fault_rate:     Probability: ~1 in N frames affected (0 = disabled)
 * @reserved:       Reserved for future use
 *
 * Fault flags:
 *   Bit 0: DROP_FRAME      - Drop frames randomly
 *   Bit 1: CORRUPT_CRC     - Corrupt CRC32 value
 *   Bit 2: CORRUPT_DATA    - Flip bits in frame data
 *   Bit 3: SKIP_SEQUENCE   - Skip sequence numbers
 *
 * Use with PHANTOMFPGA_IOCTL_SET_FAULT. Set inject_flags=0 to disable.
 */
struct phantomfpga_fault_cfg {
	__u32 inject_flags;   /* Fault enable bitmask */
	__u32 fault_rate;     /* ~1 in N frames affected */
	__u32 reserved[4];    /* Reserved for future use */
};

/* Fault injection flag bits */
#define PHANTOMFPGA_FAULT_DROP_FRAME      (1 << 0)
#define PHANTOMFPGA_FAULT_CORRUPT_CRC     (1 << 1)
#define PHANTOMFPGA_FAULT_CORRUPT_DATA    (1 << 2)
#define PHANTOMFPGA_FAULT_SKIP_SEQUENCE   (1 << 3)

/* ------------------------------------------------------------------------ */
/* IOCTL Commands                                                           */
/* ------------------------------------------------------------------------ */

/*
 * PHANTOMFPGA_IOCTL_SET_CFG - Configure device parameters
 *
 * Must be called before PHANTOMFPGA_IOCTL_START.
 * Cannot be called while device is streaming.
 *
 * Input:  struct phantomfpga_config
 * Returns: 0 on success, -EINVAL for invalid params, -EBUSY if streaming
 */
#define PHANTOMFPGA_IOCTL_SET_CFG       _IOW(PHANTOMFPGA_IOC_MAGIC, 1, \
                                             struct phantomfpga_config)

/*
 * PHANTOMFPGA_IOCTL_GET_CFG - Get current device configuration
 *
 * Output: struct phantomfpga_config
 * Returns: 0 on success
 */
#define PHANTOMFPGA_IOCTL_GET_CFG       _IOR(PHANTOMFPGA_IOC_MAGIC, 2, \
                                             struct phantomfpga_config)

/*
 * PHANTOMFPGA_IOCTL_START - Start frame transmission
 *
 * Enables frame streaming. Device will transmit frames at configured rate.
 *
 * Returns: 0 on success, -EINVAL if not configured, -EBUSY if already started
 */
#define PHANTOMFPGA_IOCTL_START         _IO(PHANTOMFPGA_IOC_MAGIC, 3)

/*
 * PHANTOMFPGA_IOCTL_STOP - Stop frame transmission
 *
 * Disables streaming. Pending frames in buffers remain valid.
 *
 * Returns: 0 on success
 */
#define PHANTOMFPGA_IOCTL_STOP          _IO(PHANTOMFPGA_IOC_MAGIC, 4)

/*
 * PHANTOMFPGA_IOCTL_GET_STATS - Get device statistics
 *
 * Output: struct phantomfpga_stats
 * Returns: 0 on success
 */
#define PHANTOMFPGA_IOCTL_GET_STATS     _IOR(PHANTOMFPGA_IOC_MAGIC, 5, \
                                             struct phantomfpga_stats)

/*
 * PHANTOMFPGA_IOCTL_RESET_STATS - Reset statistics counters
 *
 * Resets driver-side counters (frames_consumed, irq_count, crc_errors).
 *
 * Returns: 0 on success
 */
#define PHANTOMFPGA_IOCTL_RESET_STATS   _IO(PHANTOMFPGA_IOC_MAGIC, 6)

/*
 * PHANTOMFPGA_IOCTL_GET_BUFFER_INFO - Get buffer information for mmap
 *
 * Call this to get buffer parameters before mmap().
 *
 * Output: struct phantomfpga_buffer_info
 * Returns: 0 on success, -EINVAL if not configured
 */
#define PHANTOMFPGA_IOCTL_GET_BUFFER_INFO _IOR(PHANTOMFPGA_IOC_MAGIC, 7, \
                                               struct phantomfpga_buffer_info)

/*
 * PHANTOMFPGA_IOCTL_CONSUME_FRAME - Mark one frame as consumed
 *
 * Advances the consumer index and resubmits the descriptor.
 * Use after processing a frame when using mmap() access pattern.
 *
 * Returns: 0 on success, -EAGAIN if no frames available
 */
#define PHANTOMFPGA_IOCTL_CONSUME_FRAME _IO(PHANTOMFPGA_IOC_MAGIC, 8)

/*
 * PHANTOMFPGA_IOCTL_SET_FAULT - Configure fault injection
 *
 * Enables fault injection for testing error handling code.
 * Set inject_flags=0 to disable all faults.
 *
 * Input: struct phantomfpga_fault_cfg
 * Returns: 0 on success
 */
#define PHANTOMFPGA_IOCTL_SET_FAULT     _IOW(PHANTOMFPGA_IOC_MAGIC, 9, \
                                             struct phantomfpga_fault_cfg)

/* Maximum ioctl command number for validation */
#define PHANTOMFPGA_IOC_MAXNR           9

/* ------------------------------------------------------------------------ */
/* Return Codes                                                             */
/* ------------------------------------------------------------------------ */

/*
 * Standard errno values are used:
 *
 * EINVAL   - Invalid parameter value
 * EBUSY    - Device busy (already streaming, or config during stream)
 * EAGAIN   - No data available (poll or non-blocking read)
 * ENOMEM   - Memory allocation failed
 * EIO      - Hardware communication error
 * ENODEV   - Device not found
 */

/* ------------------------------------------------------------------------ */
/* Frame Header Structure (for userspace parsing)                           */
/* ------------------------------------------------------------------------ */

/*
 * Frame header at the start of each 5120-byte frame.
 * All fields are little-endian.
 */
struct phantomfpga_frame_header {
	__le32 magic;           /* 0xF00DFACE */
	__le32 sequence;        /* Frame sequence number (0-249, wraps) */
	__le64 reserved;        /* Reserved (must be 0) */
} __attribute__((packed));

/*
 * Full frame layout (5120 bytes):
 *   0x0000: Frame header (16 bytes)
 *   0x0010: ASCII frame data (4995 bytes)
 *   0x1393: Zero padding (105 bytes)
 *   0x13FC: CRC32 (4 bytes)
 */

/*
 * Completion structure (16 bytes)
 * Written at (buffer_end - 16) when a descriptor completes
 */
struct phantomfpga_completion {
	__le32 status;          /* 0=OK, else error code */
	__le32 actual_length;   /* Bytes actually transferred */
	__le64 timestamp;       /* Device timestamp */
} __attribute__((packed));

/* Completion status codes */
#define PHANTOMFPGA_COMPL_OK            0
#define PHANTOMFPGA_COMPL_ERR_DMA       1
#define PHANTOMFPGA_COMPL_ERR_OVERFLOW  2

#endif /* PHANTOMFPGA_UAPI_H */
