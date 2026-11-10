/* Standalone diagnosis only. Never use these registers alongside Hosted. */
#pragma once
#define MEO_C5_DIAG_MAGIC "M5D1"
#define MEO_C5_DIAG_VERSION 1
#define MEO_C5_DIAG_READY 3
#define MEO_C5_DIAG_BUFFER_SIZE 256
#define MEO_C5_DIAG_PACKET_SIZE 64
#define MEO_C5_DIAG_PACKETS 16
#define MEO_C5_DIAG_SAMPLES 20
/* Shared registers: 0..3 signature, 4 version, 5 stage, 6 reset reason,
 * 7 heartbeat seqlock, 8..11 little-endian heartbeat, 32..63 ELF SHA-256.
 * 12..31 are deliberately untouched (includes reserved/interrupt fields).
 * Host CMD52 mapping is 0x6c+pos for pos<28, 0x9c+(pos-32) for pos>=32. */
#define MEO_C5_DIAG_REG_ADDR(pos) ((pos) < 28 ? 0x6c + (pos) : 0x9c + (pos) - 32)
