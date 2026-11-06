/*
FxSound
Copyright (C) 2025  FxSound LLC
Contributors:
	www.theremino.com (2025)
	
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
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include "codedefs.h"
#include "u_GraphicEq.h"

/*
 * The dial range of band i_band_num (1-based) when num_bands (> 1) bands span r_min_band_freq..r_max_band_freq:
 * from halfway (on a log scale) to the previous band's position, to halfway to the next one; the first and last
 * bands are pinned to the span's edges.
 */
static void BandRangeForSpan(realtype r_min_band_freq, realtype r_max_band_freq, int num_bands, int i_band_num,
    float *fp_min_freq, float *fp_max_freq)
{
    double d_ratio = (double)r_max_band_freq / (double)r_min_band_freq;
    double d_power, d_factor;

    if (i_band_num == 1)
    {
        /* Pin to the fixed ISO spectrum edge, not the band's own (possibly preset-shifted) frequency */
        *fp_min_freq = r_min_band_freq;
    }
    else
    {
        d_power = (double)((i_band_num-1)*2 - 1) / (double)(num_bands*2 - 2);

        d_factor = pow(d_ratio, d_power);

        *fp_min_freq = roundf((realtype)(r_min_band_freq * d_factor));
        if (*fp_min_freq < 1000)
            (*fp_min_freq)++;
        else
            (*fp_min_freq)+=10;
    }

    if (i_band_num == num_bands)
    {
        /* Pin to the fixed ISO spectrum edge, not the band's own (possibly preset-shifted) frequency */
        *fp_max_freq = r_max_band_freq;
    }
    else
    {
        d_power = (double)(i_band_num*2 - 1) / (double)(num_bands*2 - 2);

        d_factor = pow(d_ratio, d_power);

        *fp_max_freq = roundf((realtype)(r_min_band_freq * d_factor));
    }
}

/*
 * FUNCTION: GraphicEqGetNumBands()
 * DESCRIPTION:
 *  Gets the number of bands.
 */
int PT_DECLSPEC GraphicEqGetNumBands(PT_HANDLE *hp_GraphicEq, int *ip_num_bands)
{
	struct GraphicEqHdlType *cast_handle;

	cast_handle = (struct GraphicEqHdlType *)(hp_GraphicEq);
 
	if (cast_handle == NULL)
		return(NOT_OKAY);

	*ip_num_bands = cast_handle->num_bands;

	return(OKAY);
}

/*
 * FUNCTION: GraphicEqGetBandBoostCut()
 * DESCRIPTION:
 *  Gets the boost/cut setting for the passed band.  
 *
 *  Note; The band_num starts at 1. (i.e. to get the first band use i_band_num = 1)
 */
int PT_DECLSPEC GraphicEqGetBandBoostCut(PT_HANDLE *hp_GraphicEq, int i_band_num, realtype *rp_boost_cut)
{
	struct GraphicEqHdlType *cast_handle;
	realtype *rp_boost_array;

	cast_handle = (struct GraphicEqHdlType *)(hp_GraphicEq);
 
	if (cast_handle == NULL)
		return(NOT_OKAY);

   /* Make sure the band_num is legal */
	if ((i_band_num <= 0) || (i_band_num > cast_handle->num_bands)) 
		return(NOT_OKAY);

	if( sosGetCenterFreqResponseArray((PT_HANDLE *)(cast_handle->sos_hdl), &rp_boost_array) != OKAY)
		return(NOT_OKAY);

	*rp_boost_cut = rp_boost_array[i_band_num - 1];

	return(OKAY);
}

/*
 * FUNCTION: GraphicEqGetBandCenterFrequency()
 * DESCRIPTION:
 *  Gets the center frequency setting for the passed band.  
 *
 *  Note; The band_num starts at 1. (i.e. to get the first band use i_band_num = 1)
 */
int PT_DECLSPEC GraphicEqGetBandCenterFrequency(PT_HANDLE *hp_GraphicEq, int i_band_num, realtype *rp_center_freq)
{
	struct GraphicEqHdlType *cast_handle;
	realtype *rp_freq_array;

	cast_handle = (struct GraphicEqHdlType *)(hp_GraphicEq);
 
	if (cast_handle == NULL)
		return(NOT_OKAY);

   /* Make sure the band_num is legal */
	if ((i_band_num <= 0) || (i_band_num > cast_handle->num_bands)) 
		return(NOT_OKAY);

		/* Get the section response array */
	if( sosGetCenterFreqArray((PT_HANDLE *)(cast_handle->sos_hdl), &rp_freq_array) != OKAY)
		return(NOT_OKAY);

	*rp_center_freq = rp_freq_array[i_band_num - 1];

	return(OKAY);
}

int PT_DECLSPEC GraphicEqGetBandFrequencyRange(PT_HANDLE *hp_GraphicEq, int i_band_num, float *fp_min_freq, float* fp_max_freq)
{
    struct GraphicEqHdlType *cast_handle;
    realtype *rp_freq_array;

    cast_handle = (struct GraphicEqHdlType *)(hp_GraphicEq);

    if (cast_handle == NULL)
        return(NOT_OKAY);

    /* Make sure the band_num is legal */
    if ((i_band_num <= 0) || (i_band_num > cast_handle->num_bands))
        return(NOT_OKAY);

    if (sosGetCenterFreqArray((PT_HANDLE *)(cast_handle->sos_hdl), &rp_freq_array) != OKAY)
        return(NOT_OKAY);

    if (cast_handle->num_bands == 1)
    {
        cast_handle->Q = (realtype)1.0;

        *fp_min_freq = rp_freq_array[i_band_num - 1];
        *fp_max_freq = rp_freq_array[i_band_num - 1];
    }
    else
    {
        BandRangeForSpan(cast_handle->min_band_freq, cast_handle->max_band_freq, cast_handle->num_bands, i_band_num,
            fp_min_freq, fp_max_freq);
    }

    return(OKAY);
}

int PT_DECLSPEC GraphicEqGetIsoLayout(PT_HANDLE *hp_GraphicEq, int *ip_iso)
{
    struct GraphicEqHdlType *cast_handle;

    cast_handle = (struct GraphicEqHdlType *)(hp_GraphicEq);

    if (cast_handle == NULL || ip_iso == NULL)
        return(NOT_OKAY);

    *ip_iso = cast_handle->iso_layout;

    return(OKAY);
}

int PT_DECLSPEC GraphicEqFreqsFitIsoLayoutOnly(PT_HANDLE *hp_GraphicEq, int *ip_iso_only)
{
    struct GraphicEqHdlType *cast_handle;
    realtype *rp_freq_array;
    int num_bands;
    bool fits_standard = true;
    bool fits_iso = true;

    cast_handle = (struct GraphicEqHdlType *)(hp_GraphicEq);

    if (cast_handle == NULL || ip_iso_only == NULL)
        return(NOT_OKAY);

    *ip_iso_only = 0;

    num_bands = cast_handle->num_bands;
    if (num_bands != GRAPHIC_EQ_10_BAND_NUM_BANDS)
        return(OKAY);

    if (sosGetCenterFreqArray((PT_HANDLE *)(cast_handle->sos_hdl), &rp_freq_array) != OKAY)
        return(NOT_OKAY);

    for (int band = 1; band <= num_bands; band++)
    {
        float freq = (float)rp_freq_array[band - 1];
        float lo, hi;

        BandRangeForSpan(GRAPHIC_EQ_10_BAND_STANDARD_MIN_FREQ, GRAPHIC_EQ_10_BAND_MAX_FREQ, num_bands, band, &lo, &hi);
        if (freq < lo || freq > hi)
            fits_standard = false;

        BandRangeForSpan(GRAPHIC_EQ_10_BAND_ISO_MIN_FREQ, GRAPHIC_EQ_10_BAND_MAX_FREQ, num_bands, band, &lo, &hi);
        if (freq < lo || freq > hi)
            fits_iso = false;
    }

    *ip_iso_only = (fits_iso && !fits_standard) ? 1 : 0;

    return(OKAY);
}

int PT_DECLSPEC GraphicEqGetMasterGain(PT_HANDLE* hp_GraphicEq, float* gain_db)
{
	struct GraphicEqHdlType* cast_handle;

	cast_handle = (struct GraphicEqHdlType*)(hp_GraphicEq);

	if (cast_handle == NULL)
		return(NOT_OKAY);

	*gain_db = cast_handle->master_gain;

	return(OKAY);
}

int PT_DECLSPEC GraphicEqGetNormalization(PT_HANDLE* hp_GraphicEq, float* gain_db)
{
	struct GraphicEqHdlType* cast_handle;

	cast_handle = (struct GraphicEqHdlType*)(hp_GraphicEq);

	if (cast_handle == NULL)
		return(NOT_OKAY);

	*gain_db = cast_handle->normalization_gain;

	return(OKAY);
}

int PT_DECLSPEC GraphicEqGetVolumeLeveling(PT_HANDLE* hp_GraphicEq, float* gain_db)
{
	struct GraphicEqHdlType* cast_handle;

	cast_handle = (struct GraphicEqHdlType*)(hp_GraphicEq);

	if (cast_handle == NULL)
		return(NOT_OKAY);

	*gain_db = cast_handle->volume_leveling_gain_db;

	return(OKAY);
}


int PT_DECLSPEC GraphicEqGetFilterQ(PT_HANDLE* hp_GraphicEq, float* q_multiplier)
{
	struct GraphicEqHdlType* cast_handle;

	cast_handle = (struct GraphicEqHdlType*)(hp_GraphicEq);

	if (cast_handle == NULL)
		return(NOT_OKAY);

	*q_multiplier = cast_handle->Q_multiplier;

	return(OKAY);
}

int PT_DECLSPEC GraphicEqGetBalance(PT_HANDLE* hp_GraphicEq, float* balance_db)
{
	struct GraphicEqHdlType* cast_handle;

	cast_handle = (struct GraphicEqHdlType*)(hp_GraphicEq);

	if (cast_handle == NULL)
		return(NOT_OKAY);

	*balance_db = cast_handle->balance;

	return(OKAY);
}
