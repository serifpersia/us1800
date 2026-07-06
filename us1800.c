// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Šerif Rami <ramiserifpersia@gmail.com>

#include <linux/init.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/initval.h>
#include "us1800.h"

MODULE_AUTHOR("Šerif Rami <ramiserifpersia@gmail.com>");
MODULE_DESCRIPTION("ALSA Driver for TASCAM US-1800");
MODULE_LICENSE("GPL");

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static bool enable[SNDRV_CARDS] = { 1, [1 ...(SNDRV_CARDS - 1)] = 0 };
static atomic_t dev_idx = ATOMIC_INIT(0);

static int us1800_write_regs(struct us1800_card *us1800, const u16 *regs, size_t count)
{
	int i, err = 0;
	struct usb_device *dev = us1800->dev;

	for (i = 0; i < count; i++) {
		dev_info(&dev->dev, "Writing vendor register 0x%04x...\n", regs[i]);
		err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
							  VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV,
						regs[i], REG_VAL_ENABLE, NULL, 0, USB_CTRL_TIMEOUT_MS);
		if (err < 0) {
			dev_err(&dev->dev, "Failed to write register 0x%04x: %d\n", regs[i], err);
			return err;
		}
	}
	return 0;
}

int us1800_configure_device_for_rate(struct us1800_card *us1800, int rate)
{
	struct usb_device *dev = us1800->dev;
	u8 *rate_payload;
	int err = 0;
	const u8 *current_payload_src;
	u16 rate_reg;
	static const u8 payload_44100[] = { 0x44, 0xac, 0x00 };
	static const u8 payload_48000[] = { 0x80, 0xbb, 0x00 };
	static const u8 payload_88200[] = { 0x88, 0x58, 0x01 };
	static const u8 payload_96000[] = { 0x00, 0x77, 0x01 };

	if (!dev)
		return -ENODEV;

	dev_info(&dev->dev, "Starting configuration sequence for rate: %d Hz\n", rate);

	switch (rate) {
		case 44100:
			current_payload_src = payload_44100;
			rate_reg = REG_ADDR_RATE_44100;
			break;
		case 48000:
			current_payload_src = payload_48000;
			rate_reg = REG_ADDR_RATE_48000;
			break;
		case 88200:
			current_payload_src = payload_88200;
			rate_reg = REG_ADDR_RATE_88200;
			break;
		case 96000:
			current_payload_src = payload_96000;
			rate_reg = REG_ADDR_RATE_96000;
			break;
		default:
			dev_err(&dev->dev, "Requested unsupported rate: %d Hz\n", rate);
			return -EINVAL;
	}

	rate_payload = kmemdup(current_payload_src, 3, GFP_KERNEL);
	if (!rate_payload)
		return -ENOMEM;

	/* Step 1: Mode Change to CONFIG with wIndex = 0x0002 */
	dev_info(&dev->dev, "[1/8] Setting mode to CONFIG (0x0010) on Interface 0 (wIndex=2)\n");
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
						  VENDOR_REQ_MODE_CONTROL, RT_H2D_VENDOR_DEV,
					   MODE_VAL_CONFIG, 0x0002, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0) {
		dev_err(&dev->dev, "CONFIG mode switch failed: %d\n", err);
		goto out;
	}

	/* Step 2: Set Sample Rate on Capture Endpoint 0x86 */
	dev_info(&dev->dev, "[2/8] Setting rate payload on Capture EP 0x86\n");
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), UAC_SET_CUR,
						  RT_H2D_CLASS_EP, UAC_SAMPLING_FREQ_CONTROL,
					   EP_AUDIO_IN, rate_payload, 3, USB_CTRL_TIMEOUT_MS);
	if (err < 0) {
		dev_err(&dev->dev, "Capture rate configuration failed: %d\n", err);
		goto out;
	}

	/* Step 3: Set Sample Rate on Playback Endpoint 0x02 */
	dev_info(&dev->dev, "[3/8] Setting rate payload on Playback EP 0x02\n");
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0), UAC_SET_CUR,
						  RT_H2D_CLASS_EP, UAC_SAMPLING_FREQ_CONTROL,
						  EP_AUDIO_OUT, rate_payload, 3, USB_CTRL_TIMEOUT_MS);
	if (err < 0) {
		dev_err(&dev->dev, "Playback rate configuration failed: %d\n", err);
		goto out;
	}

	/* Step 4: Register state query */
	dev_info(&dev->dev, "[4/8] Reading diagnostic status from register 0x0d00\n");
	err = usb_control_msg(dev, usb_rcvctrlpipe(dev, 0), VENDOR_REQ_REGISTER_WRITE,
						  RT_D2H_VENDOR_DEV, 0x0d00, REG_VAL_ENABLE, us1800->scratch_buf,
						  5, USB_CTRL_TIMEOUT_MS);
	if (err < 0) {
		dev_err(&dev->dev, "Diagnostic status read failed: %d\n", err);
		goto out;
	}
	dev_info(&dev->dev, "Diagnostic payload returned: %02x %02x %02x %02x %02x\n",
			 us1800->scratch_buf[0], us1800->scratch_buf[1], us1800->scratch_buf[2],
			 us1800->scratch_buf[3], us1800->scratch_buf[4]);

	/* Step 5: Hardware register series writes */
	dev_info(&dev->dev, "[5/8] Initiating sequential register writes\n");
	{
		const u16 regs_to_write[] = {
			REG_ADDR_INIT_0D, REG_ADDR_INIT_0E,
			REG_ADDR_INIT_0F, rate_reg, REG_ADDR_INIT_11
		};
		err = us1800_write_regs(us1800, regs_to_write, ARRAY_SIZE(regs_to_write));
		if (err < 0)
			goto out;
	}

	/* Step 6: Verify sample rate on Capture Endpoint */
	dev_info(&dev->dev, "[6/8] Verifying active rate on Capture Endpoint 0x86\n");
	err = usb_control_msg(dev, usb_rcvctrlpipe(dev, 0), UAC_GET_CUR,
						  RT_D2H_CLASS_EP, UAC_SAMPLING_FREQ_CONTROL,
						  EP_AUDIO_IN, us1800->scratch_buf, 3, USB_CTRL_TIMEOUT_MS);
	if (err < 0) {
		dev_err(&dev->dev, "Active rate verification failed: %d\n", err);
		goto out;
	}
	dev_info(&dev->dev, "Active Endpoint Rate: %d Hz\n",
			 us1800->scratch_buf[0] | (us1800->scratch_buf[1] << 8) | (us1800->scratch_buf[2] << 16));

	/* Step 7: Post-config handshake check */
	dev_info(&dev->dev, "[7/8] Reading final state validation handshake\n");
	err = usb_control_msg(dev, usb_rcvctrlpipe(dev, 0), VENDOR_REQ_MODE_CONTROL,
						  RT_D2H_VENDOR_DEV, MODE_VAL_HANDSHAKE_READ, 0x0000,
						  us1800->scratch_buf, 1, USB_CTRL_TIMEOUT_MS);
	if (err < 0) {
		dev_err(&dev->dev, "State validation handshake failed: %d\n", err);
		goto out;
	}
	dev_info(&dev->dev, "State validation response: 0x%02x\n", us1800->scratch_buf[0]);

	/* Step 8: Mode Change to STREAM START with wValue = 0x0032 */
	dev_info(&dev->dev, "[8/8] Activating streaming mode with command STREAM START (0x0032)\n");
	err = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
						  VENDOR_REQ_MODE_CONTROL, RT_H2D_VENDOR_DEV,
						  MODE_VAL_STREAM_START_US1800, 0x0000, NULL, 0, USB_CTRL_TIMEOUT_MS);
	if (err < 0) {
		dev_err(&dev->dev, "STREAM START transition failed: %d\n", err);
		goto out;
	}

	dev_info(&dev->dev, "US-1800 configuration and handshake sequence verified successfully.\n");

	out:
	kfree(rate_payload);
	return err;
}

static void us1800_card_private_free(struct snd_card *card)
{
	struct us1800_card *us1800 = card->private_data;

	if (us1800) {
		if (us1800->dev) {
			usb_put_dev(us1800->dev);
			us1800->dev = NULL;
		}
	}
}

static int us1800_probe(struct usb_interface *intf, const struct usb_device_id *usb_id)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	struct snd_card *card;
	struct us1800_card *us1800;
	int err;
	int idx;

	/* Interface 1 is reserved for endpoint streaming.
	 * Driver configuration binds solely to standard Interface 0.
	 */
	if (intf->cur_altsetting->desc.bInterfaceNumber == 1)
		return 0;

	idx = atomic_fetch_inc(&dev_idx);
	if (idx >= SNDRV_CARDS) {
		atomic_dec(&dev_idx);
		return -ENODEV;
	}
	if (!enable[idx]) {
		atomic_dec(&dev_idx);
		return -ENOENT;
	}

	dev_info(&dev->dev, "TASCAM US-1800 driver probe started.\n");

	err = snd_card_new(&dev->dev, index[idx], id[idx], THIS_MODULE,
					   sizeof(struct us1800_card), &card);
	if (err < 0) {
		atomic_dec(&dev_idx);
		return err;
	}

	us1800 = card->private_data;
	card->private_free = us1800_card_private_free;
	us1800->dev = usb_get_dev(dev);
	us1800->card = card;
	us1800->iface0 = intf;

	strscpy(card->driver, DRIVER_NAME, sizeof(card->driver));
	strscpy(card->shortname, "US-1800", sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname), "%s (%04x:%04x) at %s",
			 card->shortname, USB_VID_TASCAM, dev->descriptor.idProduct,
		  dev_name(&dev->dev));

	us1800->scratch_buf = devm_kzalloc(&dev->dev, 16, GFP_KERNEL);
	if (!us1800->scratch_buf) {
		err = -ENOMEM;
		goto free_card;
	}

	/* Query and log firmware string from hardware */
	dev_info(&dev->dev, "Querying firmware revisions...\n");
	err = usb_control_msg(dev, usb_rcvctrlpipe(dev, 0),
						  VENDOR_REQ_FIRMWARE_READ, RT_D2H_VENDOR_DEV,
					   0x0000, 0x0000, us1800->scratch_buf, 15,
					   USB_CTRL_TIMEOUT_MS);
	if (err < 0) {
		dev_err(&dev->dev, "Firmware lookup failed: %d\n", err);
		goto free_card;
	}
	dev_info(&dev->dev, "Firmware reports version string: %s\n", us1800->scratch_buf + 4);

	/* Prepare configuration endpoints on Interfaces 0 & 1 */
	err = usb_set_interface(dev, 0, 1);
	if (err < 0) {
		dev_err(&dev->dev, "Alternate setting selection for Interface 0 failed: %d\n", err);
		goto free_card;
	}

	err = usb_set_interface(dev, 1, 1);
	if (err < 0) {
		dev_err(&dev->dev, "Alternate setting selection for Interface 1 failed: %d\n", err);
		goto free_card;
	}

	/* Read initial boot control state */
	dev_info(&dev->dev, "Reading initial boot state...\n");
	err = usb_control_msg(dev, usb_rcvctrlpipe(dev, 0),
						  VENDOR_REQ_MODE_CONTROL, RT_D2H_VENDOR_DEV,
					   MODE_VAL_HANDSHAKE_READ, 0x0000, us1800->scratch_buf, 1,
					   USB_CTRL_TIMEOUT_MS);
	if (err < 0) {
		dev_err(&dev->dev, "Boot state verification failed: %d\n", err);
		goto free_card;
	}
	dev_info(&dev->dev, "Boot state handshake: 0x%02x\n", us1800->scratch_buf[0]);

	/* Set device clock configuration to a default 48000 Hz */
	err = us1800_configure_device_for_rate(us1800, 48000);
	if (err < 0) {
		dev_err(&dev->dev, "Initial target clock initialization failed.\n");
		goto free_card;
	}
	us1800->current_rate = 48000;

	err = snd_card_register(card);
	if (err < 0)
		goto free_card;

	usb_set_intfdata(intf, us1800);
	dev_info(&dev->dev, "TASCAM US-1800 card probed and registered successfully.\n");
	return 0;

	free_card:
	snd_card_free(card);
	atomic_dec(&dev_idx);
	return err;
}

static void us1800_disconnect(struct usb_interface *intf)
{
	struct us1800_card *us1800 = usb_get_intfdata(intf);

	if (!us1800)
		return;

	if (intf->cur_altsetting->desc.bInterfaceNumber == 0) {
		dev_info(&us1800->dev->dev, "Removing TASCAM US-1800 from system.\n");
		snd_card_disconnect(us1800->card);
		usb_set_intfdata(intf, NULL);
		snd_card_free(us1800->card);
		atomic_dec(&dev_idx);
	}
}

static const struct usb_device_id us1800_usb_ids[] = {
	{ USB_DEVICE(USB_VID_TASCAM, USB_PID_TASCAM_US1800) },
	{ }
};
MODULE_DEVICE_TABLE(usb, us1800_usb_ids);

static struct usb_driver us1800_alsa_driver = {
	.name = DRIVER_NAME,
	.probe = us1800_probe,
	.disconnect = us1800_disconnect,
	.id_table = us1800_usb_ids,
};

module_usb_driver(us1800_alsa_driver);
