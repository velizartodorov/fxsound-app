/*
FxSound
Copyright (C) 2025  FxSound LLC

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <string.h>
#include <new>

#include "codedefs.h"
#include "FiltPartConv.h"
#include "../pffft/pffft.h"

/*
 * FUNCTION: filtPartConvAllocFloats()  [internal]
 * DESCRIPTION:
 *   Zeroed, SIMD-aligned float buffer (PFFFT requires 16-byte alignment).
 */
static float *filtPartConvAllocFloats(size_t count)
{
	float *buffer = (float *)pffft_aligned_malloc(sizeof(float) * count);

	if (buffer != NULL)
	{
		memset(buffer, 0, sizeof(float) * count);
	}

	return buffer;
}

static void filtPartConvFreeFloats(float *buffer)
{
	if (buffer != NULL)
	{
		pffft_aligned_free(buffer);
	}
}

/*
 * FUNCTION: filtPartConvTransformFilter()  [internal]
 * DESCRIPTION:
 *   Splits rp_coeffs into P partitions of B taps, zero-pads each to 2B and
 *   stores its forward FFT (PFFFT's internal z-domain order, which is what
 *   pffft_zconvolve_accumulate() expects) into fp_slot. Taps beyond
 *   i_num_taps are zero. Uses only the publishing-thread scratch buffers.
 */
static void filtPartConvTransformFilter(FiltPartConv *sp_conv, const realtype *rp_coeffs, int i_num_taps, float *fp_slot)
{
	int partition, k;

	for (partition = 0; partition < sp_conv->num_partitions; partition++)
	{
		memset(sp_conv->publish_time, 0, sizeof(float) * (size_t)sp_conv->fft_size);

		for (k = 0; k < sp_conv->block_size; k++)
		{
			int tap = partition * sp_conv->block_size + k;

			if (tap < i_num_taps)
			{
				sp_conv->publish_time[k] = (float)rp_coeffs[tap];
			}
		}

		pffft_transform(sp_conv->fft_setup, sp_conv->publish_time,
			fp_slot + (size_t)partition * sp_conv->fft_size, sp_conv->publish_work, PFFFT_FORWARD);
	}
}

/*
 * FUNCTION: filtPartConvDestroy()
 */
void PT_DECLSPEC filtPartConvDestroy(FiltPartConv *sp_conv)
{
	int i;

	if (sp_conv == NULL)
	{
		return;
	}

	for (i = 0; i < FILT_PART_CONV_MAX_CHANNELS; i++)
	{
		filtPartConvFreeFloats(sp_conv->channels[i].input_block);
		filtPartConvFreeFloats(sp_conv->channels[i].output_block);
		filtPartConvFreeFloats(sp_conv->channels[i].fdl);
		filtPartConvFreeFloats(sp_conv->channels[i].dry_delay);
	}
	for (i = 0; i < FILT_PART_CONV_NUM_SLOTS; i++)
	{
		filtPartConvFreeFloats(sp_conv->slots[i]);
	}
	filtPartConvFreeFloats(sp_conv->work);
	filtPartConvFreeFloats(sp_conv->accum);
	filtPartConvFreeFloats(sp_conv->time_out);
	filtPartConvFreeFloats(sp_conv->publish_work);
	filtPartConvFreeFloats(sp_conv->publish_time);

	if (sp_conv->fft_setup != NULL)
	{
		pffft_destroy_setup(sp_conv->fft_setup);
	}

	delete sp_conv;
}

/*
 * FUNCTION: filtPartConvCreate()
 * DESCRIPTION:
 *   Allocates every buffer the engine will ever use and loads rp_coeffs as the
 *   initial filter (slot 0). i_block_size must make 2B a valid PFFFT real
 *   transform size (a multiple of 32; 128 and 512 are used). Returns NULL if
 *   the arguments are invalid or any allocation fails. Never call this from
 *   the audio thread.
 */
FiltPartConv PT_DECLSPEC *filtPartConvCreate(int i_block_size, int i_num_taps, int i_num_channels, const realtype *rp_coeffs)
{
	FiltPartConv *conv;
	size_t fft_size, spectra_size;
	int i;
	bool ok = true;

	if (i_block_size < 16 || (i_block_size % 16) != 0 || i_num_taps < 1 ||
		i_num_channels < 1 || i_num_channels > FILT_PART_CONV_MAX_CHANNELS || rp_coeffs == NULL)
	{
		return NULL;
	}

	conv = new (std::nothrow) FiltPartConv();
	if (conv == NULL)
	{
		return NULL;
	}

	conv->block_size = i_block_size;
	conv->fft_size = 2 * i_block_size;
	conv->num_partitions = (i_num_taps + i_block_size - 1) / i_block_size;
	conv->num_taps = i_num_taps;
	conv->num_channels = i_num_channels;
	conv->dry_delay_length = (i_num_taps - 1) / 2 + i_block_size;

	fft_size = (size_t)conv->fft_size;
	spectra_size = (size_t)conv->num_partitions * fft_size;

	conv->fft_setup = pffft_new_setup(conv->fft_size, PFFFT_REAL);
	ok = ok && (conv->fft_setup != NULL);

	conv->work = filtPartConvAllocFloats(fft_size);
	conv->accum = filtPartConvAllocFloats(fft_size);
	conv->time_out = filtPartConvAllocFloats(fft_size);
	conv->publish_work = filtPartConvAllocFloats(fft_size);
	conv->publish_time = filtPartConvAllocFloats(fft_size);
	ok = ok && conv->work && conv->accum && conv->time_out && conv->publish_work && conv->publish_time;

	for (i = 0; i < FILT_PART_CONV_NUM_SLOTS; i++)
	{
		conv->slots[i] = filtPartConvAllocFloats(spectra_size);
		ok = ok && (conv->slots[i] != NULL);
	}

	for (i = 0; i < i_num_channels; i++)
	{
		conv->channels[i].input_block = filtPartConvAllocFloats(fft_size);
		conv->channels[i].output_block = filtPartConvAllocFloats((size_t)i_block_size);
		conv->channels[i].fdl = filtPartConvAllocFloats(spectra_size);
		conv->channels[i].dry_delay = filtPartConvAllocFloats((size_t)conv->dry_delay_length);
		ok = ok && conv->channels[i].input_block && conv->channels[i].output_block &&
			conv->channels[i].fdl && conv->channels[i].dry_delay;
	}

	if (!ok)
	{
		filtPartConvDestroy(conv);
		return NULL;
	}

	filtPartConvTransformFilter(conv, rp_coeffs, i_num_taps, conv->slots[0]);
	conv->front_slot = 0;
	conv->middle_slot.store(1, std::memory_order_release);
	conv->back_slot = 2;

	return conv;
}

/*
 * FUNCTION: filtPartConvPublishFilter()
 * DESCRIPTION:
 *   Loads a new filter into the back slot and publishes it. The audio thread
 *   picks it up at its next block boundary and crossfades to it over that
 *   block; the input history (FDL) is kept, so audio continues without a gap. If the previous published filter was
 *   never picked up, it is simply replaced. Must always be called from the
 *   same single thread. Taps beyond the engine's capacity are ignored.
 */
void PT_DECLSPEC filtPartConvPublishFilter(FiltPartConv *sp_conv, const realtype *rp_coeffs, int i_num_taps)
{
	int previous_middle;

	if (i_num_taps > sp_conv->num_taps)
	{
		i_num_taps = sp_conv->num_taps;
	}

	filtPartConvTransformFilter(sp_conv, rp_coeffs, i_num_taps, sp_conv->slots[sp_conv->back_slot]);

	previous_middle = sp_conv->middle_slot.exchange(sp_conv->back_slot | FILT_PART_CONV_SLOT_DIRTY, std::memory_order_acq_rel);
	sp_conv->back_slot = previous_middle & FILT_PART_CONV_SLOT_MASK;
}

/*
 * FUNCTION: filtPartConvGetLatencySamples()
 */
int PT_DECLSPEC filtPartConvGetLatencySamples(const FiltPartConv *sp_conv)
{
	return sp_conv->dry_delay_length;
}

/*
 * FUNCTION: filtPartConvConvolveBlock()  [internal]
 * DESCRIPTION:
 *   For one channel whose newest input spectrum is already in its FDL:
 *   multiply-accumulates the FDL against the P partitions of fp_filter
 *   (partition p with the spectrum from p blocks ago), inverse FFT, and
 *   returns a pointer to the block's B output samples (the last B of the
 *   2B inverse transform: overlap-save) inside sp_conv->time_out. PFFFT's
 *   inverse transform is unscaled, hence the 1/2B scaling. Audio thread only.
 */
static const float *filtPartConvConvolveBlock(FiltPartConv *sp_conv, FiltPartConvChannel *sp_chan, const float *fp_filter)
{
	const int fft_size = sp_conv->fft_size;
	const int num_partitions = sp_conv->num_partitions;
	const float scaling = 1.0f / (float)fft_size;
	int partition;

	memset(sp_conv->accum, 0, sizeof(float) * (size_t)fft_size);
	for (partition = 0; partition < num_partitions; partition++)
	{
		int spectrum_index = sp_conv->fdl_head - partition;

		if (spectrum_index < 0)
		{
			spectrum_index += num_partitions;
		}

		pffft_zconvolve_accumulate(sp_conv->fft_setup,
			sp_chan->fdl + (size_t)spectrum_index * fft_size,
			fp_filter + (size_t)partition * fft_size,
			sp_conv->accum, scaling);
	}

	pffft_transform(sp_conv->fft_setup, sp_conv->accum, sp_conv->time_out, sp_conv->work, PFFFT_BACKWARD);
	return sp_conv->time_out + sp_conv->block_size;
}

/*
 * FUNCTION: filtPartConvProcessBlock()  [internal]
 * DESCRIPTION:
 *   Runs once every B frames, on the audio thread. For each channel: FFT of
 *   the last 2B input samples into the newest FDL entry, then the block's
 *   output with the current (front) filter.
 *
 *   If a newly published filter is waiting, the block is then computed a
 *   second time with it and the two outputs are crossfaded linearly across
 *   the block's B samples (old -> new), so a filter swap never steps. The
 *   order matters: every use of the current front slot comes first, and only
 *   then is it handed back to the publisher (the exchange), so the audio
 *   thread never reads a slot the publisher may already be overwriting. The
 *   FDL holds input spectra, not filtered ones, so from the following block
 *   on the output is exactly the new filter's. The swap costs one extra
 *   accumulate + inverse FFT per channel, in that block only.
 */
static void filtPartConvProcessBlock(FiltPartConv *sp_conv)
{
	const int block_size = sp_conv->block_size;
	const int fft_size = sp_conv->fft_size;
	const float *filter = sp_conv->slots[sp_conv->front_slot];
	const bool swap_pending = (sp_conv->middle_slot.load(std::memory_order_acquire) & FILT_PART_CONV_SLOT_DIRTY) != 0;
	int channel, k;

	sp_conv->fdl_head = (sp_conv->fdl_head + 1) % sp_conv->num_partitions;

	for (channel = 0; channel < sp_conv->num_channels; channel++)
	{
		FiltPartConvChannel *chan = &sp_conv->channels[channel];

		pffft_transform(sp_conv->fft_setup, chan->input_block,
			chan->fdl + (size_t)sp_conv->fdl_head * fft_size, sp_conv->work, PFFFT_FORWARD);

		memcpy(chan->output_block, filtPartConvConvolveBlock(sp_conv, chan, filter), sizeof(float) * (size_t)block_size);
		memmove(chan->input_block, chan->input_block + block_size, sizeof(float) * (size_t)block_size);
	}

	if (swap_pending)
	{
		const float step = 1.0f / (float)block_size;

		/* The old front slot is not touched again after this exchange. */
		sp_conv->front_slot = sp_conv->middle_slot.exchange(sp_conv->front_slot, std::memory_order_acq_rel) & FILT_PART_CONV_SLOT_MASK;
		filter = sp_conv->slots[sp_conv->front_slot];

		for (channel = 0; channel < sp_conv->num_channels; channel++)
		{
			FiltPartConvChannel *chan = &sp_conv->channels[channel];
			const float *new_output = filtPartConvConvolveBlock(sp_conv, chan, filter);

			for (k = 0; k < block_size; k++)
			{
				float weight = (float)(k + 1) * step;

				chan->output_block[k] += weight * (new_output[k] - chan->output_block[k]);
			}
		}
	}
}

/*
 * FUNCTION: filtPartConvProcessFrame()
 */
void PT_DECLSPEC filtPartConvProcessFrame(FiltPartConv *sp_conv, float *fp_frame, float *fp_dry_out)
{
	int channel;

	for (channel = 0; channel < sp_conv->num_channels; channel++)
	{
		FiltPartConvChannel *chan = &sp_conv->channels[channel];
		float input = fp_frame[channel];

		chan->input_block[sp_conv->block_size + sp_conv->fill] = input;
		fp_frame[channel] = chan->output_block[sp_conv->fill];

		if (fp_dry_out != NULL)
		{
			fp_dry_out[channel] = chan->dry_delay[sp_conv->dry_index];
		}
		chan->dry_delay[sp_conv->dry_index] = input;
	}

	sp_conv->dry_index++;
	if (sp_conv->dry_index >= sp_conv->dry_delay_length)
	{
		sp_conv->dry_index = 0;
	}

	sp_conv->fill++;
	if (sp_conv->fill >= sp_conv->block_size)
	{
		filtPartConvProcessBlock(sp_conv);
		sp_conv->fill = 0;
	}
}
