/*
 * Copyright (c) 2014-2016 Cesanta Software Limited
 * All rights reserved
 *
 * Spiffy flasher. Implements strong checksums (MD5) and can use higher
 * baud rates. Actual max baud rate will differe from device to device,
 * but 921K seems to be common.
 *
 * SLIP protocol is used for communication.
 * First packet is a single byte - command number.
 * After that, a packet with a variable number of 32-bit (LE) arguments,
 * depending on command.
 *
 * Then command produces variable number of packets of output, but first
 * packet of length 1 is the response code: 0 for success, non-zero - error.
 *
 * See individual command description below.
 */

#include "stub_flasher.h"

#include <stdint.h>
#include <string.h>

#include "rom_functions.h"

#if defined(ESP8266)
#include "eagle_soc.h"
#include "ets_sys.h"
#elif defined(ESP32)
#include "soc/uart_reg.h"
#endif

#include "slip.h"
#include "uart.h"

/* Param: baud rate. */
uint32_t params[1] __attribute__((section(".params")));

#define FLASH_BLOCK_SIZE 65536
#define FLASH_SECTOR_SIZE 4096
#define FLASH_PAGE_SIZE 256

#define UART_BUF_SIZE (8 * FLASH_SECTOR_SIZE)
#define FLASH_WRITE_SIZE 512

#define UART_RX_INTS (UART_RXFIFO_FULL_INT_ENA | UART_RXFIFO_TOUT_INT_ENA)

extern uint32_t _bss_start, _bss_end;

#ifdef ESP8266
#define REG_SPI_BASE(i) (0x60000200 - i * 0x100)

#define SPI_CMD_REG(i) (REG_SPI_BASE(i) + 0x0)
#define SPI_FLASH_RDID (BIT(28))

#define SPI_W0_REG(i) (REG_SPI_BASE(i) + 0x40)
#endif

int do_flash_erase(uint32_t addr, uint32_t len) {
  if (addr % FLASH_SECTOR_SIZE != 0) return 0x32;
  if (len % FLASH_SECTOR_SIZE != 0) return 0x33;
  if (SPIUnlock() != 0) return 0x34;

  while (len > 0 && (addr % FLASH_BLOCK_SIZE != 0)) {
    if (SPIEraseSector(addr / FLASH_SECTOR_SIZE) != 0) return 0x35;
    len -= FLASH_SECTOR_SIZE;
    addr += FLASH_SECTOR_SIZE;
  }

  while (len > FLASH_BLOCK_SIZE) {
    if (SPIEraseBlock(addr / FLASH_BLOCK_SIZE) != 0) return 0x36;
    len -= FLASH_BLOCK_SIZE;
    addr += FLASH_BLOCK_SIZE;
  }

  while (len > 0) {
    if (SPIEraseSector(addr / FLASH_SECTOR_SIZE) != 0) return 0x37;
    len -= FLASH_SECTOR_SIZE;
    addr += FLASH_SECTOR_SIZE;
  }

  return 0;
}

struct uart_buf {
  uint8_t data[UART_BUF_SIZE];
  uint32_t nr;
  uint8_t *pr, *pw;
};

static struct uart_buf ub;

void uart_isr(void *arg) {
  uint32_t int_st = READ_PERI_REG(UART_INT_ST_REG(0));
  while (1) {
    uint32_t fifo_len = READ_PERI_REG(UART_STATUS_REG(0)) & 0xff;
    if (fifo_len == 0) break;
    while (fifo_len-- > 0) {
      uint8_t byte = READ_PERI_REG(UART_FIFO_REG(0)) & 0xff;
      *ub.pw++ = byte;
      ub.nr++;
      if (ub.pw >= ub.data + UART_BUF_SIZE) ub.pw = ub.data;
    }
  }
  WRITE_PERI_REG(UART_INT_CLR_REG(0), int_st);
  (void) arg;
}

int do_flash_write(uint32_t addr, uint32_t len, uint32_t erase) {
  uint8_t digest[16];
  uint32_t num_written = 0, num_erased = 0;
  struct MD5Context ctx;
  MD5Init(&ctx);

  if (addr % FLASH_SECTOR_SIZE != 0) return 0x32;
  if (len % FLASH_SECTOR_SIZE != 0) return 0x33;
  if (SPIUnlock() != 0) return 0x34;

  ub.nr = 0;
  ub.pr = ub.pw = ub.data;
  ets_isr_attach(ETS_UART0_INUM, uart_isr, &ub);
  SET_PERI_REG_MASK(UART_INT_ENA_REG(0), UART_RX_INTS);
  ets_isr_unmask(1 << ETS_UART0_INUM);

  SLIP_send(&num_written, 4);

  while (num_written < len) {
    volatile uint32_t *nr = &ub.nr;
    /* Prepare the space ahead. */
    while (erase && num_erased < num_written + FLASH_WRITE_SIZE) {
      const uint32_t num_left = (len - num_erased);
      if (num_left > FLASH_BLOCK_SIZE && addr % FLASH_BLOCK_SIZE == 0) {
        if (SPIEraseBlock(addr / FLASH_BLOCK_SIZE) != 0) return 0x35;
        num_erased += FLASH_BLOCK_SIZE;
      } else {
        /* len % FLASH_SECTOR_SIZE == 0 is enforced, no further checks needed */
        if (SPIEraseSector(addr / FLASH_SECTOR_SIZE) != 0) return 0x36;
        num_erased += FLASH_SECTOR_SIZE;
      }
    }
    /* Wait for data to arrive. */
    while (*nr < FLASH_WRITE_SIZE) {
    }
    MD5Update(&ctx, ub.pr, FLASH_WRITE_SIZE);
    if (SPIWrite(addr, (uint32_t *) ub.pr, FLASH_WRITE_SIZE) != 0) return 0x37;
    ets_intr_lock();
    *nr -= FLASH_WRITE_SIZE;
    ets_intr_unlock();
    num_written += FLASH_WRITE_SIZE;
    addr += FLASH_WRITE_SIZE;
    ub.pr += FLASH_WRITE_SIZE;
    if (ub.pr >= ub.data + UART_BUF_SIZE) ub.pr = ub.data;
    SLIP_send(&num_written, 4);
  }

  ets_isr_mask(1 << ETS_UART0_INUM);

  MD5Final(digest, &ctx);
  SLIP_send(digest, 16);

  return 0;
}

int do_flash_read(uint32_t addr, uint32_t len, uint32_t block_size,
                  uint32_t max_in_flight) {
  uint8_t buf[FLASH_SECTOR_SIZE];
  uint8_t digest[16];
  struct MD5Context ctx;
  uint32_t num_sent = 0, num_acked = 0;
  if (block_size > sizeof(buf)) return 0x52;
  MD5Init(&ctx);
  while (num_acked < len) {
    while (num_sent < len && num_sent - num_acked < max_in_flight) {
      uint32_t n = len - num_sent;
      if (n > block_size) n = block_size;
      if (SPIRead(addr, (uint32_t *) buf, n) != 0) return 0x53;
      send_packet(buf, n);
      MD5Update(&ctx, buf, n);
      addr += n;
      num_sent += n;
    }
    {
      if (SLIP_recv(&num_acked, sizeof(num_acked)) != 4) return 0x54;
      if (num_acked > num_sent) return 0x55;
    }
  }
  MD5Final(digest, &ctx);
  send_packet(digest, sizeof(digest));
  return 0;
}

int do_flash_digest(uint32_t addr, uint32_t len, uint32_t digest_block_size) {
  uint8_t buf[FLASH_SECTOR_SIZE];
  uint8_t digest[16];
  uint32_t read_block_size =
      digest_block_size ? digest_block_size : sizeof(buf);
  struct MD5Context ctx;
  if (digest_block_size > sizeof(buf)) return 0x62;
  MD5Init(&ctx);
  while (len > 0) {
    uint32_t n = len;
    struct MD5Context block_ctx;
    MD5Init(&block_ctx);
    if (n > read_block_size) n = read_block_size;
    if (SPIRead(addr, (uint32_t *) buf, n) != 0) return 0x63;
    MD5Update(&ctx, buf, n);
    if (digest_block_size > 0) {
      MD5Update(&block_ctx, buf, n);
      MD5Final(digest, &block_ctx);
      send_packet(digest, sizeof(digest));
    }
    addr += n;
    len -= n;
  }
  MD5Final(digest, &ctx);
  send_packet(digest, sizeof(digest));
  return 0;
}

int do_flash_read_chip_id(void) {
  uint32_t chip_id = 0;
  WRITE_PERI_REG(SPI_CMD_REG(0), SPI_FLASH_RDID);
  while (READ_PERI_REG(SPI_CMD_REG(0)) & SPI_FLASH_RDID) {
  }
  chip_id = READ_PERI_REG(SPI_W0_REG(0)) & 0xFFFFFF;
  send_packet((uint8_t *) &chip_id, sizeof(chip_id));
  return 0;
}

uint8_t cmd_loop(void) {
  uint8_t cmd;
  do {
    uint32_t args[4];
    uint32_t len = SLIP_recv(&cmd, 1);
    if (len != 1) {
      continue;
    }
    uint8_t resp = 0xff;
    switch (cmd) {
      case CMD_FLASH_ERASE: {
        len = SLIP_recv(args, sizeof(args));
        if (len == 8) {
          resp = do_flash_erase(args[0] /* addr */, args[1] /* len */);
        } else {
          resp = 0x31;
        }
        break;
      }
      case CMD_FLASH_WRITE: {
        len = SLIP_recv(args, sizeof(args));
        if (len == 12) {
          resp = do_flash_write(args[0] /* addr */, args[1] /* len */,
                                args[2] /* erase */);
        } else {
          resp = 0x41;
        }
        break;
      }
      case CMD_FLASH_READ: {
        len = SLIP_recv(args, sizeof(args));
        if (len == 16) {
          resp = do_flash_read(args[0] /* addr */, args[1], /* len */
                               args[2] /* block_size */,
                               args[3] /* max_in_flight */);
        } else {
          resp = 0x51;
        }
        break;
      }
      case CMD_FLASH_DIGEST: {
        len = SLIP_recv(args, sizeof(args));
        if (len == 12) {
          resp = do_flash_digest(args[0] /* addr */, args[1], /* len */
                                 args[2] /* digest_block_size */);
        } else {
          resp = 0x61;
        }
        break;
      }
      case CMD_FLASH_READ_CHIP_ID: {
        resp = do_flash_read_chip_id();
        break;
      }
      case CMD_FLASH_ERASE_CHIP: {
        resp = SPIEraseChip();
        break;
      }
      case CMD_BOOT_FW:
      case CMD_REBOOT: {
        resp = 0;
        SLIP_send(&resp, 1);
        return cmd;
      }
    }
    SLIP_send(&resp, 1);
  } while (cmd != CMD_BOOT_FW && cmd != CMD_REBOOT);
  return cmd;
}

void stub_main(void) {
  uint32_t baud_rate = params[0];
  uint32_t greeting = 0x4941484f; /* OHAI */
  uint8_t last_cmd;

  /* This points at us right now, reset for next boot. */
  ets_set_user_start(0);

  memset(&_bss_start, 0, (&_bss_end - &_bss_start));

/* Selects SPI functions for flash pins. */
#if defined(ESP8266)
  SelectSpiFunction();
#elif defined(ESP32)
  spi_flash_attach(0, 0);
#endif

  SPIParamCfg(0, 16 * 1024 * 1024, FLASH_BLOCK_SIZE, FLASH_SECTOR_SIZE,
              FLASH_PAGE_SIZE, 0xffff);

  if (baud_rate > 0) {
    ets_delay_us(10000);
    set_baud_rate(0, baud_rate);
  }

  /* Give host time to get ready too. */
  ets_delay_us(50000);

  SLIP_send(&greeting, 4);

  last_cmd = cmd_loop();

  ets_delay_us(10000);

  if (last_cmd == CMD_BOOT_FW) {
#if defined(ESP8266)
    /*
     * Find the return address in our own stack and change it.
     * "flash_finish" it gets to the same point, except it doesn't need to
     * patch up its RA: it returns from UartDwnLdProc, then from f_400011ac,
     * then jumps to 0x4000108a, then checks strapping bits again (which will
     * not have changed), and then proceeds to 0x400010a8.
     */
    volatile uint32_t *sp = &baud_rate;
    while (*sp != (uint32_t) 0x40001100) sp++;
    *sp = 0x400010a8;
    /*
     * The following dummy asm fragment acts as a barrier, to make sure function
     * epilogue, including return address loading, is added after our stack
     * patching.
     */
    __asm volatile("nop.n");
    return; /* To 0x400010a8 */
#elif defined(ESP32)
/* TODO(rojer) */
#endif
  } else {
    software_reset();
  }
  /* Not reached */
}
