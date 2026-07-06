/* SPDX-License-Identifier: GPL-2.0-only */
// Copyright (c) 2026 Šerif Rami <ramiserifpersia@gmail.com>

#ifndef __US1800_H
#define __US1800_H

#include <linux/usb.h>
#include <sound/core.h>

#define DRIVER_NAME "us1800"

#define USB_VID_TASCAM 0x0644
#define USB_PID_TASCAM_US1800 0x8030

#define EP_PLAYBACK_FEEDBACK 0x81
#define EP_AUDIO_OUT 0x02
#define EP_MIDI_IN 0x83
#define EP_MIDI_OUT 0x04
#define EP_AUDIO_IN 0x86

#define RT_H2D_CLASS_EP (USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_ENDPOINT)
#define RT_D2H_CLASS_EP (USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_ENDPOINT)
#define RT_H2D_VENDOR_DEV (USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE)
#define RT_D2H_VENDOR_DEV (USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE)

enum uac_request {
	UAC_SET_CUR = 0x01,
	UAC_GET_CUR = 0x81,
};

enum uac_control_selector {
	UAC_SAMPLING_FREQ_CONTROL = 0x0100,
};

enum tascam_vendor_request {
	VENDOR_REQ_REGISTER_WRITE = 0x41,
	VENDOR_REQ_MODE_CONTROL = 0x49,
	VENDOR_REQ_FIRMWARE_READ = 0x56,
};

enum tascam_mode_value {
	MODE_VAL_HANDSHAKE_READ = 0x0000,
	MODE_VAL_CONFIG = 0x0010,
	MODE_VAL_STREAM_START_US1800 = 0x0032,
};

enum tascam_register {
	REG_ADDR_INIT_0D = 0x0d04,
	REG_ADDR_INIT_0E = 0x0e00,
	REG_ADDR_INIT_0F = 0x0f00,
	REG_ADDR_RATE_44100 = 0x1000,
	REG_ADDR_RATE_48000 = 0x1002,
	REG_ADDR_RATE_88200 = 0x1008,
	REG_ADDR_RATE_96000 = 0x100a,
	REG_ADDR_INIT_11 = 0x110b,
};

#define REG_VAL_ENABLE 0x0101
#define USB_CTRL_TIMEOUT_MS 1000

struct us1800_card {
	struct usb_device *dev;
	struct usb_interface *iface0;
	struct snd_card *card;
	u8 *scratch_buf;
	int current_rate;
};

/* Function Prototypes */
int us1800_configure_device_for_rate(struct us1800_card *us1800, int rate);

#endif /* __US1800_H */
