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
#ifndef _FILT_PART_CONV_H_
#define _FILT_PART_CONV_H_

#include <atomic>
#include "codedefs.h"

struct PFFFT_Setup;

/* Maximum channels one engine processes; further channels pass through. */
#define FILT_PART_CONV_MAX_CHANNELS  8
#define FILT_PART_CONV_NUM_SLOTS     3
/* Set in middle_slot when it holds a filter the audio thread hasn't taken yet. */
#define FILT_PART_CONV_SLOT_DIRTY    0x4
#define FILT_PART_CONV_SLOT_MASK     0x3

struct FiltPartConvChannel {
	float *input_block;   /* 2B samples: previous block, then the block being filled */
	float *output_block;  /* B filtered samples, emitted during the following block */
	float *fdl;           /* frequency-domain delay line: P input spectra of 2B floats (ring) */
	float *dry_delay;     /* dry_delay_length samples (ring), for time-aligned dry output */
};

/*
 * Uniformly-partitioned overlap-save (UPOLS) FFT convolution engine. Generic:
 * it knows nothing about brickwall policy. All buffers are allocated by
 * filtPartConvCreate(); the audio-thread entry point,
 * filtPartConvProcessFrame(), never allocates or locks.
 *
 * Filter swaps use a lock-free triple buffer of filter spectra: the audio
 * thread owns front_slot, the single publishing thread owns back_slot, and
 * middle_slot is exchanged atomically between them.
 */
struct FiltPartConv {
	int block_size;        /* B: partition size, also the added latency */
	int fft_size;          /* 2B */
	int num_partitions;    /* P = ceil(num_taps / B) */
	int num_taps;          /* filter length capacity */
	int num_channels;
	int dry_delay_length;  /* total latency in samples: (num_taps - 1) / 2 + B */
	int tag;               /* caller-defined; not used by the convolver */
	unsigned int serial;   /* caller-defined; not used by the convolver */

	int fill;              /* frames written into the current block, 0..B-1 */
	int fdl_head;          /* index of the newest spectrum in each channel's fdl */
	int dry_index;

	PFFFT_Setup *fft_setup;
	float *work;           /* audio-thread FFT scratch (2B) */
	float *accum;          /* audio-thread spectrum accumulator (2B) */
	float *time_out;       /* audio-thread inverse FFT output (2B) */
	float *publish_work;   /* publishing-thread FFT scratch (2B) */
	float *publish_time;   /* publishing-thread zero-padded partition (2B) */

	float *slots[FILT_PART_CONV_NUM_SLOTS]; /* filter spectra, P x 2B floats each */
	int front_slot;                         /* audio thread only */
	int back_slot;                          /* publishing thread only */
	std::atomic<int> middle_slot;

	FiltPartConvChannel channels[FILT_PART_CONV_MAX_CHANNELS];
};

FiltPartConv PT_DECLSPEC *filtPartConvCreate(int i_block_size, int i_num_taps, int i_num_channels, const realtype *rp_coeffs);
void PT_DECLSPEC filtPartConvDestroy(FiltPartConv *sp_conv);
void PT_DECLSPEC filtPartConvPublishFilter(FiltPartConv *sp_conv, const realtype *rp_coeffs, int i_num_taps);
int PT_DECLSPEC filtPartConvGetLatencySamples(const FiltPartConv *sp_conv);

/*
 * Processes one interleaved frame (the first num_channels samples of fp_frame)
 * in place: each sample is replaced by the filtered output, delayed by
 * filtPartConvGetLatencySamples() for a symmetric (linear-phase) filter. If
 * fp_dry_out is non-NULL, it receives the input delayed by the same latency.
 * Audio thread only. Real-time safe.
 */
void PT_DECLSPEC filtPartConvProcessFrame(FiltPartConv *sp_conv, float *fp_frame, float *fp_dry_out);

#endif
