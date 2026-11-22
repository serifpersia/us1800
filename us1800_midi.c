// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Šerif Rami <ramiserifpersia@gmail.com>

#include <linux/delay.h>

#include "us1800.h"

static void us1800_midi_out_complete(struct urb *urb)
{
	struct us1800_card *us1800 = urb->context;
	struct snd_rawmidi_substream *substream;
	unsigned long flags;
	int count;
	bool submit = false;
	bool active;

	atomic_set(&us1800->midi_out_in_flight, 0);

	spin_lock_irqsave(&us1800->midi_lock, flags);

	substream = us1800->midi_output;
	active = us1800->midi_out_active;

	if (urb->status || !substream) {
		us1800->midi_out_active = false;
		spin_unlock_irqrestore(&us1800->midi_lock, flags);
		return;
	}

	if (!active) {
		spin_unlock_irqrestore(&us1800->midi_lock, flags);
		return;
	}

	count = snd_rawmidi_transmit(substream, us1800->midi_out_buf, MIDI_PAYLOAD_SIZE);
	if (count > 0) {
		if (count < MIDI_PAYLOAD_SIZE)
			memset(us1800->midi_out_buf + count, 0xFD, MIDI_PAYLOAD_SIZE - count);

		us1800->midi_out_buf[8] = 0xE0;
		urb->transfer_buffer_length = MIDI_PACKET_SIZE;
		submit = true;
	} else {
		us1800->midi_out_active = false;
	}

	if (submit) {
		atomic_set(&us1800->midi_out_in_flight, 1);
		usb_anchor_urb(urb, &us1800->midi_anchor);
		if (usb_submit_urb(urb, GFP_ATOMIC) < 0) {
			usb_unanchor_urb(urb);
			atomic_set(&us1800->midi_out_in_flight, 0);
			us1800->midi_out_active = false;
		}
	}

	spin_unlock_irqrestore(&us1800->midi_lock, flags);
}

static void us1800_midi_output_trigger(struct snd_rawmidi_substream *substream, int up)
{
	struct us1800_card *us1800 = substream->rmidi->private_data;
	unsigned long flags;
	int count;

	spin_lock_irqsave(&us1800->midi_lock, flags);

	if (up) {
		us1800->midi_output = substream;
		if (!us1800->midi_out_active) {
			us1800->midi_out_active = true;

			count = snd_rawmidi_transmit(substream, us1800->midi_out_buf, MIDI_PAYLOAD_SIZE);
			if (count > 0) {
				if (count < MIDI_PAYLOAD_SIZE)
					memset(us1800->midi_out_buf + count, 0xFD, MIDI_PAYLOAD_SIZE - count);

				us1800->midi_out_buf[8] = 0xE0;
				us1800->midi_out_urb->transfer_buffer_length = MIDI_PACKET_SIZE;

				atomic_set(&us1800->midi_out_in_flight, 1);
				usb_anchor_urb(us1800->midi_out_urb, &us1800->midi_anchor);
				if (usb_submit_urb(us1800->midi_out_urb, GFP_ATOMIC) < 0) {
					usb_unanchor_urb(us1800->midi_out_urb);
					atomic_set(&us1800->midi_out_in_flight, 0);
					us1800->midi_out_active = false;
				}
			} else {
				us1800->midi_out_active = false;
			}
		}
	} else {
		us1800->midi_output = NULL;
	}

	spin_unlock_irqrestore(&us1800->midi_lock, flags);
}

static void us1800_midi_output_drain(struct snd_rawmidi_substream *substream)
{
	struct us1800_card *us1800 = substream->rmidi->private_data;
	int i;

	for (i = 0; i < 1000; i++) {
		if (!atomic_read(&us1800->midi_out_in_flight) &&
		    !READ_ONCE(us1800->midi_out_active))
			break;
		msleep(1);
	}
}

static void us1800_midi_in_complete(struct urb *urb)
{
	struct us1800_card *us1800 = urb->context;
	struct snd_rawmidi_substream *substream;
	unsigned long flags;
	int i;

	if (urb->status)
		return;

	spin_lock_irqsave(&us1800->midi_lock, flags);
	substream = us1800->midi_input;
	spin_unlock_irqrestore(&us1800->midi_lock, flags);

	if (urb->actual_length == MIDI_PACKET_SIZE && substream) {
		u8 *data = urb->transfer_buffer;

		for (i = 0; i < MIDI_PAYLOAD_SIZE; i++) {
			if (data[i] == 0xFD)
				break;
			snd_rawmidi_receive(substream, &data[i], 1);
		}
	}

	usb_anchor_urb(urb, &us1800->midi_anchor);
	if (usb_submit_urb(urb, GFP_ATOMIC) < 0)
		usb_unanchor_urb(urb);
}

static void us1800_midi_input_trigger(struct snd_rawmidi_substream *substream, int up)
{
	struct us1800_card *us1800 = substream->rmidi->private_data;
	unsigned long flags;

	spin_lock_irqsave(&us1800->midi_lock, flags);
	if (up)
		us1800->midi_input = substream;
	else
		us1800->midi_input = NULL;
	spin_unlock_irqrestore(&us1800->midi_lock, flags);
}

static int us1800_midi_open(struct snd_rawmidi_substream *substream)
{
	struct us1800_card *us1800 = substream->rmidi->private_data;

	if (substream->stream == SNDRV_RAWMIDI_STREAM_OUTPUT) {
		unsigned long flags;

		spin_lock_irqsave(&us1800->midi_lock, flags);
		us1800->midi_out_active = false;
		spin_unlock_irqrestore(&us1800->midi_lock, flags);
	} else if (substream->stream == SNDRV_RAWMIDI_STREAM_INPUT) {
		usb_anchor_urb(us1800->midi_in_urb, &us1800->midi_anchor);
		if (usb_submit_urb(us1800->midi_in_urb, GFP_KERNEL) < 0) {
			usb_unanchor_urb(us1800->midi_in_urb);
			return -EIO;
		}
	}
	return 0;
}

static int us1800_midi_close(struct snd_rawmidi_substream *substream)
{
	struct us1800_card *us1800 = substream->rmidi->private_data;

	if (substream->stream == SNDRV_RAWMIDI_STREAM_INPUT)
		usb_kill_urb(us1800->midi_in_urb);
	return 0;
}

static const struct snd_rawmidi_ops midi_output_ops = {
	.open = us1800_midi_open,
	.close = us1800_midi_close,
	.trigger = us1800_midi_output_trigger,
	.drain = us1800_midi_output_drain,
};

static const struct snd_rawmidi_ops midi_input_ops = {
	.open = us1800_midi_open,
	.close = us1800_midi_close,
	.trigger = us1800_midi_input_trigger,
};

int us1800_create_midi(struct us1800_card *us1800)
{
	int err;
	struct snd_rawmidi *rmidi;

	err = snd_rawmidi_new(us1800->card, "TASCAM MIDI", 0, 1, 1, &rmidi);
	if (err < 0)
		return err;

	rmidi->private_data = us1800;
	strscpy(rmidi->name, "TASCAM US-1800 MIDI", sizeof(rmidi->name));

	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_OUTPUT, &midi_output_ops);
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_INPUT, &midi_input_ops);

	rmidi->info_flags = SNDRV_RAWMIDI_INFO_OUTPUT |
	SNDRV_RAWMIDI_INFO_INPUT |
	SNDRV_RAWMIDI_INFO_DUPLEX;
	us1800->rmidi = rmidi;

	us1800->midi_out_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!us1800->midi_out_urb) {
		err = -ENOMEM;
		goto err_out_urb;
	}

	us1800->midi_in_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!us1800->midi_in_urb) {
		err = -ENOMEM;
		goto err_in_urb;
	}

	us1800->midi_out_buf = usb_alloc_coherent(us1800->dev, MIDI_PACKET_SIZE,
						  GFP_KERNEL, &us1800->midi_out_urb->transfer_dma);
	if (!us1800->midi_out_buf) {
		err = -ENOMEM;
		goto err_out_buf;
	}

	us1800->midi_in_buf = usb_alloc_coherent(us1800->dev, MIDI_PACKET_SIZE,
						 GFP_KERNEL, &us1800->midi_in_urb->transfer_dma);
	if (!us1800->midi_in_buf) {
		err = -ENOMEM;
		goto err_in_buf;
	}

	usb_fill_bulk_urb(us1800->midi_out_urb, us1800->dev,
			  usb_sndbulkpipe(us1800->dev, EP_MIDI_OUT),
			  us1800->midi_out_buf, MIDI_PACKET_SIZE,
			  us1800_midi_out_complete, us1800);
	us1800->midi_out_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	usb_fill_bulk_urb(us1800->midi_in_urb, us1800->dev,
			  usb_rcvbulkpipe(us1800->dev, EP_MIDI_IN),
			  us1800->midi_in_buf, MIDI_PACKET_SIZE,
			  us1800_midi_in_complete, us1800);
	us1800->midi_in_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	atomic_set(&us1800->midi_out_in_flight, 0);
	spin_lock_init(&us1800->midi_lock);
	init_usb_anchor(&us1800->midi_anchor);

	return 0;

err_in_buf:
	usb_free_coherent(us1800->dev, MIDI_PACKET_SIZE,
			  us1800->midi_out_buf, us1800->midi_out_urb->transfer_dma);
err_out_buf:
	usb_free_urb(us1800->midi_in_urb);
	us1800->midi_in_urb = NULL;
err_in_urb:
	usb_free_urb(us1800->midi_out_urb);
	us1800->midi_out_urb = NULL;
err_out_urb:
	return err;
}
