/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PhantomFPGA Register Definitions v3.0
 *
 * This header mirrors the QEMU device register layout. The trainee driver
 * uses these definitions to communicate with the virtual FPGA device.
 *
 * v3.0 streams pre-built data frames via scatter-gather DMA.
 * Build a driver, stream frames over TCP, display them on the host.
 *
 * Matches: platform/qemu/src/hw/misc/phantomfpga.h
 */

#ifndef PHANTOMFPGA_REGS_H
#define PHANTOMFPGA_REGS_H

#include <linux/types.h>
#include "phantomfpga_uapi.h"

/* ------------------------------------------------------------------------ */
/* PCI Device Identification                                                */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_VENDOR_ID       0x0DAD   /* Oh DAD - unassigned vendor ID */
#define PHANTOMFPGA_DEVICE_ID       0xF00D   /* What every programmer needs */
#define PHANTOMFPGA_SUBSYS_VENDOR   0x0DAD
#define PHANTOMFPGA_SUBSYS_ID       0x0003   /* v3.0 edition */
#define PHANTOMFPGA_REVISION        0x03

/* ------------------------------------------------------------------------ */
/* Device Constants                                                         */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_BAR0_SIZE       4096
#define PHANTOMFPGA_DEV_ID_VAL      0xF00DFACE  /* Expected in DEV_ID register */
#define PHANTOMFPGA_DEV_VER         0x00030000  /* v3.0.0 */
#define PHANTOMFPGA_MSIX_VECTORS    3           /* Complete, error, no_desc */

/* Default values */
#define PHANTOMFPGA_DEFAULT_FRAME_RATE  25      /* 25 fps */
#define PHANTOMFPGA_DEFAULT_DESC_COUNT  256     /* Descriptor ring entries */
#define PHANTOMFPGA_DEFAULT_IRQ_COUNT   8       /* IRQ after N completions */
#define PHANTOMFPGA_DEFAULT_IRQ_TIMEOUT 40000   /* 40ms = 1 frame at 25fps */
#define PHANTOMFPGA_DEFAULT_FAULT_RATE  1000    /* 1 in N frames affected */

/* Limits */
#define PHANTOMFPGA_MIN_FRAME_RATE      1       /* 1 fps */
#define PHANTOMFPGA_MAX_FRAME_RATE      60      /* 60 fps */
#define PHANTOMFPGA_MIN_DESC_COUNT      4
#define PHANTOMFPGA_MAX_DESC_COUNT      4096

/* ------------------------------------------------------------------------ */
/* Register Offsets (from BAR0 base)                                        */
/* ------------------------------------------------------------------------ */

/* Identification & Control */
#define PHANTOMFPGA_REG_DEV_ID          0x000   /* R   - Device ID (0xF00DFACE) */
#define PHANTOMFPGA_REG_DEV_VER         0x004   /* R   - Device Version */
#define PHANTOMFPGA_REG_CTRL            0x008   /* R/W - Control Register */
#define PHANTOMFPGA_REG_STATUS          0x00C   /* R   - Status Register */

/* Frame Configuration */
#define PHANTOMFPGA_REG_FRAME_SIZE      0x010   /* R   - Frame size in bytes (5120) */
#define PHANTOMFPGA_REG_FRAME_COUNT     0x014   /* R   - Total frames (250) */
#define PHANTOMFPGA_REG_FRAME_RATE      0x018   /* R/W - Frames per second (1-60) */
#define PHANTOMFPGA_REG_CURRENT_FRAME   0x01C   /* R   - Current frame index */

/* Descriptor Ring Configuration */
#define PHANTOMFPGA_REG_DESC_RING_LO    0x020   /* R/W - Descriptor ring base [31:0] */
#define PHANTOMFPGA_REG_DESC_RING_HI    0x024   /* R/W - Descriptor ring base [63:32] */
#define PHANTOMFPGA_REG_DESC_RING_SIZE  0x028   /* R/W - Number of descriptors */
#define PHANTOMFPGA_REG_DESC_HEAD       0x02C   /* R/W - Head (driver submits) */
#define PHANTOMFPGA_REG_DESC_TAIL       0x030   /* R   - Tail (device completes) */

/* Interrupt Configuration */
#define PHANTOMFPGA_REG_IRQ_STATUS      0x034   /* R/W1C - IRQ status */
#define PHANTOMFPGA_REG_IRQ_MASK        0x038   /* R/W   - IRQ enable mask */
#define PHANTOMFPGA_REG_IRQ_COALESCE    0x03C   /* R/W   - Coalesce settings */

/* Statistics */
#define PHANTOMFPGA_REG_STAT_FRAMES_TX  0x040   /* R   - Frames transmitted */
#define PHANTOMFPGA_REG_STAT_FRAMES_DROP 0x044  /* R   - Frames dropped (backpressure) */
#define PHANTOMFPGA_REG_STAT_BYTES_LO   0x048   /* R   - Total bytes [31:0] */
#define PHANTOMFPGA_REG_STAT_BYTES_HI   0x04C   /* R   - Total bytes [63:32] */
#define PHANTOMFPGA_REG_STAT_DESC_COMPL 0x050   /* R   - Descriptors completed */
#define PHANTOMFPGA_REG_STAT_ERRORS     0x054   /* R   - Error count */

/* Fault Injection */
#define PHANTOMFPGA_REG_FAULT_INJECT    0x058   /* R/W - Fault injection control */
#define PHANTOMFPGA_REG_FAULT_RATE      0x05C   /* R/W - Fault probability (1/N) */

#define PHANTOMFPGA_REG_MAX             0x060   /* First invalid register offset */

/* ------------------------------------------------------------------------ */
/* Control Register (CTRL @ 0x008) Bits                                     */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_CTRL_RUN            BIT(0)  /* Enable frame transmission */
#define PHANTOMFPGA_CTRL_RESET          BIT(1)  /* Soft reset (self-clearing) */
#define PHANTOMFPGA_CTRL_IRQ_EN         BIT(2)  /* Global interrupt enable */

/* ------------------------------------------------------------------------ */
/* Status Register (STATUS @ 0x00C) Bits                                    */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_STATUS_RUNNING      BIT(0)  /* Device is transmitting */
#define PHANTOMFPGA_STATUS_DESC_EMPTY   BIT(1)  /* No descriptors available */
#define PHANTOMFPGA_STATUS_ERROR        BIT(2)  /* Error condition */

/* ------------------------------------------------------------------------ */
/* IRQ Status/Mask (@ 0x034, 0x038) Bits                                    */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_IRQ_COMPLETE        BIT(0)  /* Descriptor(s) completed */
#define PHANTOMFPGA_IRQ_ERROR           BIT(1)  /* Error occurred */
#define PHANTOMFPGA_IRQ_NO_DESC         BIT(2)  /* No descriptors available */

#define PHANTOMFPGA_IRQ_ALL             (PHANTOMFPGA_IRQ_COMPLETE | \
                                         PHANTOMFPGA_IRQ_ERROR | \
                                         PHANTOMFPGA_IRQ_NO_DESC)

/* MSI-X vector assignments */
#define PHANTOMFPGA_MSIX_VEC_COMPLETE   0
#define PHANTOMFPGA_MSIX_VEC_ERROR      1
#define PHANTOMFPGA_MSIX_VEC_NO_DESC    2

/* ------------------------------------------------------------------------ */
/* IRQ Coalesce Register (@ 0x03C) Fields                                   */
/* [15:0] = count threshold, [31:16] = timeout in microseconds              */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_IRQ_COAL_COUNT_MASK     0x0000FFFF
#define PHANTOMFPGA_IRQ_COAL_COUNT_SHIFT    0
#define PHANTOMFPGA_IRQ_COAL_TIMEOUT_MASK   0xFFFF0000
#define PHANTOMFPGA_IRQ_COAL_TIMEOUT_SHIFT  16

static inline u32 phantomfpga_irq_coalesce_pack(u16 count, u16 timeout_us)
{
	return ((u32)timeout_us << 16) | count;
}

static inline void phantomfpga_irq_coalesce_unpack(u32 val, u16 *count, u16 *timeout_us)
{
	*count = val & 0xFFFF;
	*timeout_us = val >> 16;
}

/* ------------------------------------------------------------------------ */
/* Fault Injection (@ 0x058) Bits - Simplified for Frames                   */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_FAULT_ALL               (PHANTOMFPGA_FAULT_DROP_FRAME | \
                                             PHANTOMFPGA_FAULT_CORRUPT_CRC | \
                                             PHANTOMFPGA_FAULT_CORRUPT_DATA | \
                                             PHANTOMFPGA_FAULT_SKIP_SEQUENCE)

/* ------------------------------------------------------------------------ */
/* Scatter-Gather Descriptor (32 bytes)                                     */
/* ------------------------------------------------------------------------ */

/*
 * Descriptor control flags
 */
#define PHANTOMFPGA_DESC_CTRL_COMPLETED  BIT(0)  /* Device sets when done */
#define PHANTOMFPGA_DESC_CTRL_EOP        BIT(1)  /* End of packet */
#define PHANTOMFPGA_DESC_CTRL_SOP        BIT(2)  /* Start of packet */
#define PHANTOMFPGA_DESC_CTRL_IRQ        BIT(3)  /* Generate IRQ on completion */
#define PHANTOMFPGA_DESC_CTRL_STOP       BIT(4)  /* Stop after this descriptor */

/*
 * Scatter-gather descriptor structure.
 * Must match the QEMU device's PhantomFPGASGDesc structure exactly.
 */
struct phantomfpga_sg_desc {
	__le32 control;        /* Flags: COMPLETED, EOP, SOP, IRQ, STOP */
	__le32 length;         /* Buffer length in bytes */
	__le64 dst_addr;       /* Host destination address */
	__le64 next_desc;      /* Next descriptor address (0 = end of chain) */
	__le64 reserved;       /* Alignment / future use */
} __packed;

#define PHANTOMFPGA_DESC_SIZE   sizeof(struct phantomfpga_sg_desc)

/* ------------------------------------------------------------------------ */
/* Derived sizes and offsets                                                */
/* Structs live in phantomfpga_uapi.h - these are convenience macros        */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_COMPL_SIZE          sizeof(struct phantomfpga_completion)
#define PHANTOMFPGA_FRAME_HDR_SIZE      sizeof(struct phantomfpga_frame_header)
#define PHANTOMFPGA_FRAME_CRC_OFFSET    (PHANTOMFPGA_FRAME_SIZE - 4)
#define PHANTOMFPGA_FRAME_DATA_OFFSET   PHANTOMFPGA_FRAME_HDR_SIZE
#define PHANTOMFPGA_FRAME_PADDING_SIZE  105

/* ------------------------------------------------------------------------ */
/* Helper Functions                                                         */
/* ------------------------------------------------------------------------ */

/*
 * Calculate number of available descriptors in the ring.
 * Ring size must be a power of 2.
 */
static inline u32 phantomfpga_desc_pending(u32 head, u32 tail, u32 ring_size)
{
	return (head - tail) & (ring_size - 1);
}

/*
 * Calculate number of free descriptor slots.
 * We keep one slot empty to distinguish full from empty.
 */
static inline u32 phantomfpga_desc_free(u32 head, u32 tail, u32 ring_size)
{
	return ring_size - 1 - phantomfpga_desc_pending(head, tail, ring_size);
}

/*
 * Check if descriptor ring is empty.
 */
static inline bool phantomfpga_desc_ring_empty(u32 head, u32 tail)
{
	return head == tail;
}

/*
 * Check if descriptor ring is full.
 */
static inline bool phantomfpga_desc_ring_full(u32 head, u32 tail, u32 ring_size)
{
	u32 next_head = (head + 1) & (ring_size - 1);
	return next_head == tail;
}

/*
 * Get completion struct location within a buffer.
 */
static inline void *phantomfpga_completion_ptr(void *buffer, u32 buffer_size)
{
	return (u8 *)buffer + buffer_size - PHANTOMFPGA_COMPL_SIZE;
}

/*
 * Get CRC32 location within a frame.
 */
static inline __le32 *phantomfpga_frame_crc_ptr(void *frame)
{
	return (__le32 *)((u8 *)frame + PHANTOMFPGA_FRAME_CRC_OFFSET);
}

/*
 * CRC32 polynomial for verification.
 * IEEE 802.3 (Ethernet) - same as the device uses.
 */
#define PHANTOMFPGA_CRC32_POLY  0xEDB88320

#endif /* PHANTOMFPGA_REGS_H */
