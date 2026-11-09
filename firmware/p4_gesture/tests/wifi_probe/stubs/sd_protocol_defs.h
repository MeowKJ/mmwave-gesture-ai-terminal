#pragma once
/* v6.1 sd_protocol_defs.h constants; CMD5 is an R4 query with arg == 0. */
#define SD_IO_SEND_OP_COND 5
#define MMC_R4(resp) ((resp)[0])
#define SD_OCR_VOL_MASK 0x00ff8000
#define SD_IO_OCR_MEM_READY (1U << 31)
#define SD_IO_OCR_NUM_FUNCTIONS(ocr) (((ocr) >> 28) & 0x7)
