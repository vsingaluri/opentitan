// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
#include <assert.h>

#include "sw/device/lib/arch/device.h"
#include "sw/device/lib/base/macros.h"
#include "sw/device/lib/base/memory.h"
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/dif/dif_i2c.h"
#include "sw/device/lib/runtime/hart.h"
#include "sw/device/lib/runtime/log.h"
#include "sw/device/lib/runtime/print.h"
#include "sw/device/lib/testing/i2c_testutils.h"
#include "sw/device/lib/testing/rv_core_ibex_testutils.h"
#include "sw/device/lib/testing/test_framework/check.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "i2c_regs.h"  // Generated.

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "This test assumes the target platform is little endian.");

OTTF_DEFINE_TEST_CONFIG();

enum {
  kDeviceAddr = 0x52,
  // Default timeout for i2c reads.
  kDefaultTimeoutMicros = 5000,
  // "Acknowledgement polling" timeout - used for waiting until EEPROM has
  // finished writing.
  kAckPollTimeoutMicros = 80000,
};

static dif_rv_core_ibex_t rv_core_ibex;
static dif_pinmux_t pinmux;
static dif_i2c_t i2c;

static status_t write_byte(const uint8_t addr[2], uint8_t byte) {
  uint8_t data[3] = {addr[0], addr[1], byte};
  return i2c_testutils_write(&i2c, kDeviceAddr, sizeof(data), data, false);
}

static status_t read_byte(const uint8_t addr[2], uint8_t *byte) {
  TRY(i2c_testutils_write(&i2c, kDeviceAddr, 2, addr, true));
  return i2c_testutils_read(&i2c, kDeviceAddr, 1, byte, kDefaultTimeoutMicros);
}

/**
 * Poll the EEPROM device until it is no longer busy writing.
 *
 * This function performs a zero-byte read with a `kAckPollTimeoutMicros`
 * microsecond timeout. The EEPROM device will not acknowledge the device
 * address until it is finished writing.
 */
static status_t poll_while_busy(void) {
  return i2c_testutils_read(&i2c, kDeviceAddr, 0, NULL, kAckPollTimeoutMicros);
}

static status_t write_read_random(void) {
  // Write a byte to some random address.
  const uint8_t kAddr[2] = {0x03, 0x21};
  TRY(write_byte(kAddr, 0xAB));

  // Wait for the write to finish.
  TRY(poll_while_busy());

  // Read back the data at that address.
  uint8_t read_data = 0x00;
  TRY(read_byte(kAddr, &read_data));

  // Check the written byte matches what we wrote.
  TRY_CHECK(read_data == 0xAB, "Unexpected value: 0x%02x", read_data);

  // Erase the value we just wrote to prevent the success state persisting to
  // subsequent runs of the test.
  TRY(write_byte(kAddr, 0xFF));
  TRY(poll_while_busy());
  TRY(read_byte(kAddr, &read_data));

  // Check the over-written byte matches the new value.
  TRY_CHECK(read_data == 0xFF, "Unexpected value: 0x%02x", read_data);

  return OK_STATUS();
}

static status_t write_read_page(void) {
  // Write multiple bytes to the 9th page (of 64 bytes each).
  const uint8_t kAddr[2] = {0x02, 0x01};
  uint8_t data[5] = {kAddr[0], kAddr[1], 0xAB, 0xCD, 0xEF};
  TRY(i2c_testutils_write(&i2c, kDeviceAddr, sizeof(data), data, false));

  // Wait for the write to finish.
  TRY(poll_while_busy());

  // Read back the data at that address.
  uint8_t read_data[3] = {0x00, 0x00, 0x00};
  TRY(i2c_testutils_write(&i2c, kDeviceAddr, sizeof(kAddr), kAddr, true));
  TRY(i2c_testutils_read(&i2c, kDeviceAddr, sizeof(read_data), read_data,
                         kDefaultTimeoutMicros));

  // Check the written bytes match what we wrote.
  TRY_CHECK_ARRAYS_EQ(read_data, data + 2, sizeof(read_data));

  // Erase the values we just wrote to prevent the success state persisting to
  // subsequent runs of the test.
  data[2] = data[3] = data[4] = 0xFF;
  TRY(i2c_testutils_write(&i2c, kDeviceAddr, sizeof(data), data, false));
  TRY(poll_while_busy());
  TRY(i2c_testutils_write(&i2c, kDeviceAddr, sizeof(kAddr), kAddr, true));
  TRY(i2c_testutils_read(&i2c, kDeviceAddr, sizeof(read_data), read_data,
                         kDefaultTimeoutMicros));

  // Check the over-written bytes match the new erased value.
  TRY_CHECK_ARRAYS_EQ(read_data, data + 2, sizeof(read_data));

  return OK_STATUS();
}

static status_t test_init(void) {
  mmio_region_t base_addr =
      mmio_region_from_addr(TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR);

  TRY(dif_rv_core_ibex_init(base_addr, &rv_core_ibex));

  base_addr = mmio_region_from_addr(TOP_EARLGREY_I2C2_BASE_ADDR);
  TRY(dif_i2c_init(base_addr, &i2c));

  base_addr = mmio_region_from_addr(TOP_EARLGREY_PINMUX_AON_BASE_ADDR);
  TRY(dif_pinmux_init(base_addr, &pinmux));

  TRY(i2c_testutils_select_pinmux(&pinmux, 2));

  TRY(dif_i2c_host_set_enabled(&i2c, kDifToggleEnabled));

  return OK_STATUS();
}

bool test_main(void) {
  status_t test_result;
  CHECK_STATUS_OK(test_init());

  dif_i2c_speed_t speeds[] = {kDifI2cSpeedStandard, kDifI2cSpeedFast,
                              kDifI2cSpeedFastPlus};

  test_result = OK_STATUS();
  for (size_t i = 0; i < ARRAYSIZE(speeds); ++i) {
    CHECK_STATUS_OK(i2c_testutils_set_speed(&i2c, speeds[i]));
    EXECUTE_TEST(test_result, write_read_random);
    EXECUTE_TEST(test_result, write_read_page);
  }

  return status_ok(test_result);
}
