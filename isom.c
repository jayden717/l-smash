/*****************************************************************************
 * isom.c:
 *****************************************************************************
 * Copyright (C) 2010-2014 L-SMASH project
 *
 * Authors: Yusuke Nakamura <muken.the.vfrmaniac@gmail.com>
 * Contributors: Takashi Hirata <silverfilain@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *****************************************************************************/

/* This file is available under an ISC license. */

#include "internal.h" /* must be placed first */

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "box.h"
#include "mp4a.h"
#include "mp4sys.h"
#include "write.h"
#include "description.h"
#ifdef LSMASH_DEMUXER_ENABLED
#include "read.h"
#endif

/*---- ----*/
char *isom_4cc2str( uint32_t fourcc )
{
    static char str[5];
    str[0] = (fourcc >> 24) & 0xff;
    str[1] = (fourcc >> 16) & 0xff;
    str[2] = (fourcc >>  8) & 0xff;
    str[3] =  fourcc        & 0xff;
    str[4] = 0;
    return str;
}

isom_trak_t *isom_get_trak( lsmash_file_t *file, uint32_t track_ID )
{
    if( track_ID == 0
     || !file
     || !file->moov )
        return NULL;
    for( lsmash_entry_t *entry = file->moov->trak_list.head; entry; entry = entry->next )
    {
        isom_trak_t *trak = (isom_trak_t *)entry->data;
        if( !trak
         || !trak->tkhd )
            return NULL;
        if( trak->tkhd->track_ID == track_ID )
            return trak;
    }
    return NULL;
}

isom_trex_t *isom_get_trex( isom_mvex_t *mvex, uint32_t track_ID )
{
    if( track_ID == 0 || !mvex )
        return NULL;
    for( lsmash_entry_t *entry = mvex->trex_list.head; entry; entry = entry->next )
    {
        isom_trex_t *trex = (isom_trex_t *)entry->data;
        if( !trex )
            return NULL;
        if( trex->track_ID == track_ID )
            return trex;
    }
    return NULL;
}

static isom_traf_t *isom_get_traf( isom_moof_t *moof, uint32_t track_ID )
{
    if( track_ID == 0 || !moof )
        return NULL;
    for( lsmash_entry_t *entry = moof->traf_list.head; entry; entry = entry->next )
    {
        isom_traf_t *traf = (isom_traf_t *)entry->data;
        if( !traf
         || !traf->tfhd )
            return NULL;
        if( traf->tfhd->track_ID == track_ID )
            return traf;
    }
    return NULL;
}

isom_tfra_t *isom_get_tfra( isom_mfra_t *mfra, uint32_t track_ID )
{
    if( track_ID == 0 || !mfra )
        return NULL;
    for( lsmash_entry_t *entry = mfra->tfra_list.head; entry; entry = entry->next )
    {
        isom_tfra_t *tfra = (isom_tfra_t *)entry->data;
        if( !tfra )
            return NULL;
        if( tfra->track_ID == track_ID )
            return tfra;
    }
    return NULL;
}

static isom_sidx_t *isom_get_sidx( lsmash_file_t *file, uint32_t reference_ID )
{
    if( reference_ID == 0 || !file )
        return NULL;
    for( lsmash_entry_t *entry = file->sidx_list.head; entry; entry = entry->next )
    {
        isom_sidx_t *sidx = (isom_sidx_t *)entry->data;
        if( !sidx )
            return NULL;
        if( sidx->reference_ID == reference_ID )
            return sidx;
    }
    return NULL;
}

static int isom_add_elst_entry( isom_elst_t *elst, uint64_t segment_duration, int64_t media_time, int32_t media_rate )
{
    assert( elst->file );
    isom_elst_entry_t *data = lsmash_malloc( sizeof(isom_elst_entry_t) );
    if( !data )
        return -1;
    data->segment_duration = segment_duration;
    data->media_time       = media_time;
    data->media_rate       = media_rate;
    if( lsmash_add_entry( elst->list, data ) )
    {
        lsmash_free( data );
        return -1;
    }
    if( !elst->file->undefined_64_ver
     && (data->segment_duration > UINT32_MAX
      || data->media_time       >  INT32_MAX
      || data->media_time       <  INT32_MIN) )
        elst->version = 1;
    return 0;
}

isom_dcr_ps_entry_t *isom_create_ps_entry( uint8_t *ps, uint32_t ps_size )
{
    isom_dcr_ps_entry_t *entry = lsmash_malloc( sizeof(isom_dcr_ps_entry_t) );
    if( !entry )
        return NULL;
    entry->nalUnit = lsmash_memdup( ps, ps_size );
    if( !entry->nalUnit )
    {
        lsmash_free( entry );
        return NULL;
    }
    entry->nalUnitLength = ps_size;
    entry->unused        = 0;
    return entry;
}

void isom_remove_dcr_ps( isom_dcr_ps_entry_t *ps )
{
    if( !ps )
        return;
    if( ps->nalUnit )
        lsmash_free( ps->nalUnit );
    lsmash_free( ps );
}

/* This function returns 0 if failed, sample_entry_number if succeeded. */
int lsmash_add_sample_entry( lsmash_root_t *root, uint32_t track_ID, void *summary )
{
    if( !root || !summary )
        return 0;
    isom_trak_t *trak = isom_get_trak( root->file, track_ID );
    if( !trak
     || !trak->file
     || !trak->mdia
     || !trak->mdia->minf
     || !trak->mdia->minf->stbl
     || !trak->mdia->minf->stbl->stsd )
        return 0;
    isom_stsd_t *stsd = trak->mdia->minf->stbl->stsd;
    lsmash_entry_list_t *list = &stsd->list;
    int ret = -1;
    lsmash_codec_type_t sample_type = ((lsmash_summary_t *)summary)->sample_type;
    if( lsmash_check_codec_type_identical( sample_type, LSMASH_CODEC_TYPE_RAW ) )
    {
        if( trak->mdia->minf->vmhd )
            ret = isom_setup_visual_description( stsd, sample_type, (lsmash_video_summary_t *)summary );
        else if( trak->mdia->minf->smhd )
            ret = isom_setup_audio_description( stsd, sample_type, (lsmash_audio_summary_t *)summary );
        return ret < 0 ? 0 : list->entry_count;
    }
    static struct description_setup_table_tag
    {
        lsmash_codec_type_t type;
        void *func;
    } description_setup_table[160] = { { LSMASH_CODEC_TYPE_INITIALIZER, NULL } };
    if( !description_setup_table[0].func )
    {
        int i = 0;
#define ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( type, func ) \
    description_setup_table[i++] = (struct description_setup_table_tag){ type, func }
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_AVC1_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_AVC2_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_AVC3_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_AVC4_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_HVC1_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_HEV1_VIDEO, isom_setup_visual_description );
#if 0
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_AVCP_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_SVC1_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_MVC1_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_MVC2_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_MP4V_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_DRAC_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_ENCV_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_MJP2_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_S263_VIDEO, isom_setup_visual_description );
#endif
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_VC_1_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_2VUY_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_APCH_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_APCN_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_APCS_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_APCO_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_AP4H_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_DVC_VIDEO,  isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_DVCP_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_DVPP_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_DV5N_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_DV5P_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_DVH2_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_DVH3_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_DVH5_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_DVH6_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_DVHP_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_DVHQ_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_ULRA_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_ULRG_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_ULY2_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_ULY0_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_ULH2_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_ULH0_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_V210_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_V216_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_V308_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_V408_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_V410_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_YUV2_VIDEO, isom_setup_visual_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_MP4A_AUDIO, isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_AC_3_AUDIO, isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_ALAC_AUDIO, isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_EC_3_AUDIO, isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_SAMR_AUDIO, isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_SAWB_AUDIO, isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_DTSC_AUDIO, isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_DTSE_AUDIO, isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_DTSH_AUDIO, isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_DTSL_AUDIO, isom_setup_audio_description );
#if 0
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_DRA1_AUDIO, isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_ENCA_AUDIO, isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_G719_AUDIO, isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_G726_AUDIO, isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_M4AE_AUDIO, isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_MLPA_AUDIO, isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_RAW_AUDIO,  isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_SAWP_AUDIO, isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_SEVC_AUDIO, isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_SQCP_AUDIO, isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_SSMV_AUDIO, isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_TWOS_AUDIO, isom_setup_audio_description );
#endif
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_MP4A_AUDIO,    isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_MAC3_AUDIO,    isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_MAC6_AUDIO,    isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_AGSM_AUDIO,    isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_ALAW_AUDIO,    isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_ULAW_AUDIO,    isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_FULLMP3_AUDIO, isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_ADPCM2_AUDIO,  isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_ADPCM17_AUDIO, isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_GSM49_AUDIO,   isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_NONE_AUDIO,    isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_LPCM_AUDIO,    isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_SOWT_AUDIO,    isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_TWOS_AUDIO,    isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_FL32_AUDIO,    isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_FL64_AUDIO,    isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_IN24_AUDIO,    isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_IN32_AUDIO,    isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_23NI_AUDIO,    isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_NOT_SPECIFIED, isom_setup_audio_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_TX3G_TEXT, isom_add_tx3g_description );
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( QT_CODEC_TYPE_TEXT_TEXT,   isom_add_qt_text_description );
#if 0
        ADD_DESCRIPTION_SETUP_TABLE_ELEMENT( ISOM_CODEC_TYPE_MP4S_SYSTEM, isom_add_mp4s_entry );
#endif
    }
    for( int i = 0; description_setup_table[i].func; i++ )
        if( lsmash_check_codec_type_identical( sample_type, description_setup_table[i].type ) )
        {
            if( isom_setup_visual_description == description_setup_table[i].func )
                ret = isom_setup_visual_description( stsd, sample_type, (lsmash_video_summary_t *)summary );
            else if( isom_setup_audio_description == description_setup_table[i].func )
                ret = isom_setup_audio_description( stsd, sample_type, (lsmash_audio_summary_t *)summary );
            else if( isom_add_tx3g_description == description_setup_table[i].func )
            {
                isom_tx3g_entry_t *tx3g = isom_add_tx3g_description( stsd );
                if( tx3g )
                {
                    tx3g->data_reference_index = 1;
                    ret = isom_add_ftab( tx3g );
                    if( ret < 0 )
                        isom_remove_box_by_itself( tx3g );
                }
            }
            else if( isom_add_qt_text_description == description_setup_table[i].func )
            {
                isom_qt_text_entry_t *text = isom_add_qt_text_description( stsd );
                if( text )
                {
                    text->data_reference_index = 1;
                    ret = 0;
                }
            }
            break;
        }
    return ret < 0 ? 0 : list->entry_count;
}

static int isom_add_stts_entry( isom_stbl_t *stbl, uint32_t sample_delta )
{
    if( !stbl
     || !stbl->stts
     || !stbl->stts->list )
        return -1;
    isom_stts_entry_t *data = lsmash_malloc( sizeof(isom_stts_entry_t) );
    if( !data )
        return -1;
    data->sample_count = 1;
    data->sample_delta = sample_delta;
    if( lsmash_add_entry( stbl->stts->list, data ) )
    {
        lsmash_free( data );
        return -1;
    }
    return 0;
}

static int isom_add_ctts_entry( isom_stbl_t *stbl, uint32_t sample_offset )
{
    if( !stbl
     || !stbl->ctts
     || !stbl->ctts->list )
        return -1;
    isom_ctts_entry_t *data = lsmash_malloc( sizeof(isom_ctts_entry_t) );
    if( !data )
        return -1;
    data->sample_count  = 1;
    data->sample_offset = sample_offset;
    if( lsmash_add_entry( stbl->ctts->list, data ) )
    {
        lsmash_free( data );
        return -1;
    }
    return 0;
}

static int isom_add_stsc_entry( isom_stbl_t *stbl, uint32_t first_chunk, uint32_t samples_per_chunk, uint32_t sample_description_index )
{
    if( !stbl
     || !stbl->stsc
     || !stbl->stsc->list )
        return -1;
    isom_stsc_entry_t *data = lsmash_malloc( sizeof(isom_stsc_entry_t) );
    if( !data )
        return -1;
    data->first_chunk              = first_chunk;
    data->samples_per_chunk        = samples_per_chunk;
    data->sample_description_index = sample_description_index;
    if( lsmash_add_entry( stbl->stsc->list, data ) )
    {
        lsmash_free( data );
        return -1;
    }
    return 0;
}

static int isom_add_stsz_entry( isom_stbl_t *stbl, uint32_t entry_size )
{
    if( !stbl
     || !stbl->stsz )
        return -1;
    isom_stsz_t *stsz = stbl->stsz;
    /* retrieve initial sample_size */
    if( stsz->sample_count == 0 )
        stsz->sample_size = entry_size;
    /* if it seems constant access_unit size at present, update sample_count only */
    if( !stsz->list && stsz->sample_size == entry_size )
    {
        ++ stsz->sample_count;
        return 0;
    }
    /* found sample_size varies, create sample_size list */
    if( !stsz->list )
    {
        stsz->list = lsmash_create_entry_list();
        if( !stsz->list )
            return -1;
        for( uint32_t i = 0; i < stsz->sample_count; i++ )
        {
            isom_stsz_entry_t *data = lsmash_malloc( sizeof(isom_stsz_entry_t) );
            if( !data )
                return -1;
            data->entry_size = stsz->sample_size;
            if( lsmash_add_entry( stsz->list, data ) )
            {
                lsmash_free( data );
                return -1;
            }
        }
        stsz->sample_size = 0;
    }
    isom_stsz_entry_t *data = lsmash_malloc( sizeof(isom_stsz_entry_t) );
    if( !data )
        return -1;
    data->entry_size = entry_size;
    if( lsmash_add_entry( stsz->list, data ) )
    {
        lsmash_free( data );
        return -1;
    }
    ++ stsz->sample_count;
    return 0;
}

static int isom_add_stss_entry( isom_stbl_t *stbl, uint32_t sample_number )
{
    if( !stbl
     || !stbl->stss
     || !stbl->stss->list )
        return -1;
    isom_stss_entry_t *data = lsmash_malloc( sizeof(isom_stss_entry_t) );
    if( !data )
        return -1;
    data->sample_number = sample_number;
    if( lsmash_add_entry( stbl->stss->list, data ) )
    {
        lsmash_free( data );
        return -1;
    }
    return 0;
}

static int isom_add_stps_entry( isom_stbl_t *stbl, uint32_t sample_number )
{
    if( !stbl
     || !stbl->stps
     || !stbl->stps->list )
        return -1;
    isom_stps_entry_t *data = lsmash_malloc( sizeof(isom_stps_entry_t) );
    if( !data )
        return -1;
    data->sample_number = sample_number;
    if( lsmash_add_entry( stbl->stps->list, data ) )
    {
        lsmash_free( data );
        return -1;
    }
    return 0;
}

static int isom_add_sdtp_entry( isom_box_t *parent, lsmash_sample_property_t *prop, uint8_t avc_extensions )
{
    if( !prop || !parent )
        return -1;
    isom_sdtp_t *sdtp = NULL;
    if( lsmash_check_box_type_identical( parent->type, ISOM_BOX_TYPE_STBL ) )
        sdtp = ((isom_stbl_t *)parent)->sdtp;
    else if( lsmash_check_box_type_identical( parent->type, ISOM_BOX_TYPE_TRAF ) )
        sdtp = ((isom_traf_t *)parent)->sdtp;
    else
        assert( 0 );
    if( !sdtp
     || !sdtp->list )
        return -1;
    isom_sdtp_entry_t *data = lsmash_malloc( sizeof(isom_sdtp_entry_t) );
    if( !data )
        return -1;
    /* isom_sdtp_entry_t is smaller than lsmash_sample_property_t. */
    data->is_leading            = (avc_extensions ? prop->leading : prop->allow_earlier) & 0x03;
    data->sample_depends_on     = prop->independent & 0x03;
    data->sample_is_depended_on = prop->disposable  & 0x03;
    data->sample_has_redundancy = prop->redundant   & 0x03;
    if( lsmash_add_entry( sdtp->list, data ) )
    {
        lsmash_free( data );
        return -1;
    }
    return 0;
}

static int isom_add_co64_entry( isom_stbl_t *stbl, uint64_t chunk_offset )
{
    if( !stbl
     || !stbl->stco
     || !stbl->stco->list )
        return -1;
    isom_co64_entry_t *data = lsmash_malloc( sizeof(isom_co64_entry_t) );
    if( !data )
        return -1;
    data->chunk_offset = chunk_offset;
    if( lsmash_add_entry( stbl->stco->list, data ) )
    {
        lsmash_free( data );
        return -1;
    }
    return 0;
}

static int isom_convert_stco_to_co64( isom_stbl_t* stbl )
{
    /* backup stco */
    int ret = 0;
    isom_stco_t *stco = stbl->stco;
    stbl->stco = NULL;
    if( isom_add_co64( stbl ) )
    {
        ret = -1;
        goto fail;
    }
    /* move chunk_offset to co64 from stco */
    for( lsmash_entry_t *entry = stco->list->head; entry; entry = entry->next )
    {
        isom_stco_entry_t *data = (isom_stco_entry_t*)entry->data;
        if( isom_add_co64_entry( stbl, data->chunk_offset ) )
        {
            ret = -1;
            goto fail;
        }
    }
fail:
    isom_remove_box_by_itself( stco );
    return ret;
}

static int isom_add_stco_entry( isom_stbl_t *stbl, uint64_t chunk_offset )
{
    if( !stbl
     || !stbl->stco
     || !stbl->stco->list )
        return -1;
    if( stbl->stco->large_presentation )
        return isom_add_co64_entry( stbl, chunk_offset );
    if( chunk_offset > UINT32_MAX )
    {
        if( isom_convert_stco_to_co64( stbl ) )
            return -1;
        return isom_add_co64_entry( stbl, chunk_offset );
    }
    isom_stco_entry_t *data = lsmash_malloc( sizeof(isom_stco_entry_t) );
    if( !data )
        return -1;
    data->chunk_offset = (uint32_t)chunk_offset;
    if( lsmash_add_entry( stbl->stco->list, data ) )
    {
        lsmash_free( data );
        return -1;
    }
    return 0;
}

static isom_sgpd_t *isom_get_sample_group_description_common( lsmash_entry_list_t *list, uint32_t grouping_type )
{
    for( lsmash_entry_t *entry = list->head; entry; entry = entry->next )
    {
        isom_sgpd_t *sgpd = (isom_sgpd_t *)entry->data;
        if( !sgpd
         || !sgpd->list )
            return NULL;
        if( sgpd->grouping_type == grouping_type )
            return sgpd;
    }
    return NULL;
}

static isom_sbgp_t *isom_get_sample_to_group_common( lsmash_entry_list_t *list, uint32_t grouping_type )
{
    for( lsmash_entry_t *entry = list->head; entry; entry = entry->next )
    {
        isom_sbgp_t *sbgp = (isom_sbgp_t *)entry->data;
        if( !sbgp
         || !sbgp->list )
            return NULL;
        if( sbgp->grouping_type == grouping_type )
            return sbgp;
    }
    return NULL;
}

isom_sgpd_t *isom_get_sample_group_description( isom_stbl_t *stbl, uint32_t grouping_type )
{
    return isom_get_sample_group_description_common( &stbl->sgpd_list, grouping_type );
}

isom_sbgp_t *isom_get_sample_to_group( isom_stbl_t *stbl, uint32_t grouping_type )
{
    return isom_get_sample_to_group_common( &stbl->sbgp_list, grouping_type );
}

isom_sgpd_t *isom_get_fragment_sample_group_description( isom_traf_t *traf, uint32_t grouping_type )
{
    return isom_get_sample_group_description_common( &traf->sgpd_list, grouping_type );
}

isom_sbgp_t *isom_get_fragment_sample_to_group( isom_traf_t *traf, uint32_t grouping_type )
{
    return isom_get_sample_to_group_common( &traf->sbgp_list, grouping_type );
}

static isom_rap_entry_t *isom_add_rap_group_entry( isom_sgpd_t *sgpd )
{
    if( !sgpd )
        return NULL;
    isom_rap_entry_t *data = lsmash_malloc( sizeof(isom_rap_entry_t) );
     if( !data )
        return NULL;
    data->description_length        = 0;
    data->num_leading_samples_known = 0;
    data->num_leading_samples       = 0;
    if( lsmash_add_entry( sgpd->list, data ) )
    {
        lsmash_free( data );
        return NULL;
    }
    return data;
}

static isom_roll_entry_t *isom_add_roll_group_entry( isom_sgpd_t *sgpd, int16_t roll_distance )
{
    if( !sgpd )
        return NULL;
    isom_roll_entry_t *data = lsmash_malloc( sizeof(isom_roll_entry_t) );
     if( !data )
        return NULL;
    data->description_length = 0;
    data->roll_distance      = roll_distance;
    if( lsmash_add_entry( sgpd->list, data ) )
    {
        lsmash_free( data );
        return NULL;
    }
    return data;
}

static isom_group_assignment_entry_t *isom_add_group_assignment_entry( isom_sbgp_t *sbgp, uint32_t sample_count, uint32_t group_description_index )
{
    if( !sbgp )
        return NULL;
    isom_group_assignment_entry_t *data = lsmash_malloc( sizeof(isom_group_assignment_entry_t) );
    if( !data )
        return NULL;
    data->sample_count            = sample_count;
    data->group_description_index = group_description_index;
    if( lsmash_add_entry( sbgp->list, data ) )
    {
        lsmash_free( data );
        return NULL;
    }
    return data;
}

int isom_check_compatibility( lsmash_file_t *file )
{
    if( !file )
        return -1;
    /* Clear flags for compatibility. */
    ptrdiff_t compat_offset = offsetof( lsmash_file_t, qt_compatible );
    memset( (int8_t *)file + compat_offset, 0, sizeof(lsmash_file_t) - compat_offset );
    file->min_isom_version = UINT8_MAX; /* undefined value */
    /* Check brand to decide mandatory boxes. */
    if( (!file->ftyp || file->ftyp->brand_count == 0)
     && (!file->styp_list.head || !file->styp_list.head->data) )
    {
        /* No brand declaration means this file is a MP4 version 1 or QuickTime file format. */
        if( file->moov
         && file->moov->iods )
        {
            file->mp4_version1    = 1;
            file->isom_compatible = 1;
        }
        else
        {
            file->qt_compatible    = 1;
            file->undefined_64_ver = 1;
        }
        return 0;
    }
    for( uint32_t i = 0; i <= file->ftyp->brand_count; i++ )
    {
        uint32_t brand = (i == file->ftyp->brand_count ? file->ftyp->major_brand : file->ftyp->compatible_brands[i]);
        switch( brand )
        {
            case ISOM_BRAND_TYPE_QT :
                file->qt_compatible = 1;
                break;
            case ISOM_BRAND_TYPE_MP41 :
                file->mp4_version1 = 1;
                break;
            case ISOM_BRAND_TYPE_MP42 :
                file->mp4_version2 = 1;
                break;
            case ISOM_BRAND_TYPE_AVC1 :
            case ISOM_BRAND_TYPE_ISOM :
                file->max_isom_version = LSMASH_MAX( file->max_isom_version, 1 );
                file->min_isom_version = LSMASH_MIN( file->min_isom_version, 1 );
                break;
            case ISOM_BRAND_TYPE_ISO2 :
                file->max_isom_version = LSMASH_MAX( file->max_isom_version, 2 );
                file->min_isom_version = LSMASH_MIN( file->min_isom_version, 2 );
                break;
            case ISOM_BRAND_TYPE_ISO3 :
                file->max_isom_version = LSMASH_MAX( file->max_isom_version, 3 );
                file->min_isom_version = LSMASH_MIN( file->min_isom_version, 3 );
                break;
            case ISOM_BRAND_TYPE_ISO4 :
                file->max_isom_version = LSMASH_MAX( file->max_isom_version, 4 );
                file->min_isom_version = LSMASH_MIN( file->min_isom_version, 4 );
                break;
            case ISOM_BRAND_TYPE_ISO5 :
                file->max_isom_version = LSMASH_MAX( file->max_isom_version, 5 );
                file->min_isom_version = LSMASH_MIN( file->min_isom_version, 5 );
                break;
            case ISOM_BRAND_TYPE_ISO6 :
                file->max_isom_version = LSMASH_MAX( file->max_isom_version, 6 );
                file->min_isom_version = LSMASH_MIN( file->min_isom_version, 6 );
                break;
            case ISOM_BRAND_TYPE_ISO7 :
                file->max_isom_version = LSMASH_MAX( file->max_isom_version, 7 );
                file->min_isom_version = LSMASH_MIN( file->min_isom_version, 7 );
                break;
            case ISOM_BRAND_TYPE_M4A :
            case ISOM_BRAND_TYPE_M4B :
            case ISOM_BRAND_TYPE_M4P :
            case ISOM_BRAND_TYPE_M4V :
                file->itunes_movie = 1;
                break;
            case ISOM_BRAND_TYPE_3GP4 :
                file->max_3gpp_version = LSMASH_MAX( file->max_3gpp_version, 4 );
                break;
            case ISOM_BRAND_TYPE_3GP5 :
                file->max_3gpp_version = LSMASH_MAX( file->max_3gpp_version, 5 );
                break;
            case ISOM_BRAND_TYPE_3GE6 :
            case ISOM_BRAND_TYPE_3GG6 :
            case ISOM_BRAND_TYPE_3GP6 :
            case ISOM_BRAND_TYPE_3GR6 :
            case ISOM_BRAND_TYPE_3GS6 :
                file->max_3gpp_version = LSMASH_MAX( file->max_3gpp_version, 6 );
                break;
            case ISOM_BRAND_TYPE_3GP7 :
                file->max_3gpp_version = LSMASH_MAX( file->max_3gpp_version, 7 );
                break;
            case ISOM_BRAND_TYPE_3GP8 :
                file->max_3gpp_version = LSMASH_MAX( file->max_3gpp_version, 8 );
                break;
            case ISOM_BRAND_TYPE_3GE9 :
            case ISOM_BRAND_TYPE_3GF9 :
            case ISOM_BRAND_TYPE_3GG9 :
            case ISOM_BRAND_TYPE_3GH9 :
            case ISOM_BRAND_TYPE_3GM9 :
            case ISOM_BRAND_TYPE_3GP9 :
            case ISOM_BRAND_TYPE_3GR9 :
            case ISOM_BRAND_TYPE_3GS9 :
            case ISOM_BRAND_TYPE_3GT9 :
                file->max_3gpp_version = LSMASH_MAX( file->max_3gpp_version, 9 );
                break;
            default :
                break;
        }
        switch( brand )
        {
            case ISOM_BRAND_TYPE_AVC1 :
            case ISOM_BRAND_TYPE_ISO2 :
            case ISOM_BRAND_TYPE_ISO3 :
            case ISOM_BRAND_TYPE_ISO4 :
            case ISOM_BRAND_TYPE_ISO5 :
            case ISOM_BRAND_TYPE_ISO6 :
                file->avc_extensions = 1;
                break;
            case ISOM_BRAND_TYPE_3GP4 :
            case ISOM_BRAND_TYPE_3GP5 :
            case ISOM_BRAND_TYPE_3GP6 :
            case ISOM_BRAND_TYPE_3GP7 :
            case ISOM_BRAND_TYPE_3GP8 :
            case ISOM_BRAND_TYPE_3GP9 :
                file->forbid_tref = 1;
                break;
            case ISOM_BRAND_TYPE_3GH9 :
            case ISOM_BRAND_TYPE_3GM9 :
            case ISOM_BRAND_TYPE_DASH :
            case ISOM_BRAND_TYPE_DSMS :
            case ISOM_BRAND_TYPE_LMSG :
            case ISOM_BRAND_TYPE_MSDH :
            case ISOM_BRAND_TYPE_MSIX :
            case ISOM_BRAND_TYPE_SIMS :
                file->media_segment = 1;
                break;
            default :
                break;
        }
    }
    file->isom_compatible = !file->qt_compatible
                          || file->mp4_version1
                          || file->mp4_version2
                          || file->itunes_movie
                          || file->max_3gpp_version;
    file->undefined_64_ver = file->qt_compatible || file->itunes_movie;
    if( file->flags & LSMASH_FILE_MODE_WRITE )
    {
        /* Media Segment is incompatible with ISO Base Media File Format version 4 or former must be compatible with
         * version 6 or later since it requires default-base-is-moof and Track Fragment Base Media Decode Time Box. */
        if( file->media_segment && (file->min_isom_version < 5 || (file->max_isom_version && file->max_isom_version < 6)) )
            return -1;
        file->allow_moof_base = (file->max_isom_version >= 5 && file->min_isom_version >= 5)
                             || (file->max_isom_version == 0 && file->min_isom_version == UINT8_MAX && file->media_segment);
    }
    return 0;
}

static uint32_t isom_get_sample_count( isom_trak_t *trak )
{
    if( !trak
     || !trak->mdia
     || !trak->mdia->minf
     || !trak->mdia->minf->stbl
     || !trak->mdia->minf->stbl->stsz )
        return 0;
    return trak->mdia->minf->stbl->stsz->sample_count;
}

static uint64_t isom_get_dts( isom_stts_t *stts, uint32_t sample_number )
{
    if( !stts
     || !stts->list )
        return 0;
    uint64_t dts = 0;
    uint32_t i   = 1;
    lsmash_entry_t    *entry;
    isom_stts_entry_t *data;
    for( entry = stts->list->head; entry; entry = entry->next )
    {
        data = (isom_stts_entry_t *)entry->data;
        if( !data )
            return 0;
        if( i + data->sample_count > sample_number )
            break;
        dts += (uint64_t)data->sample_delta * data->sample_count;
        i   += data->sample_count;
    }
    if( !entry )
        return 0;
    dts += (uint64_t)data->sample_delta * (sample_number - i);
    return dts;
}

#if 0
static uint64_t isom_get_cts( isom_stts_t *stts, isom_ctts_t *ctts, uint32_t sample_number )
{
    if( !stts
     || !stts->list )
        return 0;
    if( !ctts )
        return isom_get_dts( stts, sample_number );
    uint32_t i = 1;     /* This can be 0 (and then condition below shall be changed) but I dare use same algorithm with isom_get_dts. */
    lsmash_entry_t    *entry;
    isom_ctts_entry_t *data;
    if( sample_number == 0 )
        return 0;
    for( entry = ctts->list->head; entry; entry = entry->next )
    {
        data = (isom_ctts_entry_t *)entry->data;
        if( !data )
            return 0;
        if( i + data->sample_count > sample_number )
            break;
        i += data->sample_count;
    }
    if( !entry )
        return 0;
    return isom_get_dts( stts, sample_number ) + data->sample_offset;
}
#endif

static int isom_replace_last_sample_delta( isom_stbl_t *stbl, uint32_t sample_delta )
{
    if( !stbl
     || !stbl->stts
     || !stbl->stts->list
     || !stbl->stts->list->tail
     || !stbl->stts->list->tail->data )
        return -1;
    isom_stts_entry_t *last_stts_data = (isom_stts_entry_t *)stbl->stts->list->tail->data;
    if( sample_delta != last_stts_data->sample_delta )
    {
        if( last_stts_data->sample_count > 1 )
        {
            last_stts_data->sample_count -= 1;
            if( isom_add_stts_entry( stbl, sample_delta ) )
                return -1;
        }
        else
            last_stts_data->sample_delta = sample_delta;
    }
    return 0;
}

static int isom_update_mdhd_duration( isom_trak_t *trak, uint32_t last_sample_delta )
{
    if( !trak
     || !trak->file
     || !trak->cache
     || !trak->mdia
     || !trak->mdia->mdhd
     || !trak->mdia->minf
     || !trak->mdia->minf->stbl
     || !trak->mdia->minf->stbl->stts
     || !trak->mdia->minf->stbl->stts->list )
        return -1;
    lsmash_file_t *file = trak->file;
    isom_mdhd_t *mdhd = trak->mdia->mdhd;
    isom_stbl_t *stbl = trak->mdia->minf->stbl;
    isom_stts_t *stts = stbl->stts;
    isom_ctts_t *ctts = stbl->ctts;
    isom_cslg_t *cslg = stbl->cslg;
    mdhd->duration = 0;
    uint32_t sample_count = isom_get_sample_count( trak );
    if( sample_count == 0 )
    {
        /* Return error if non-fragmented movie has no samples. */
        if( !file->fragment && !stts->list->entry_count )
            return -1;
        return 0;
    }
    /* Now we have at least 1 sample, so do stts_entry. */
    lsmash_entry_t    *last_stts      = stts->list->tail;
    isom_stts_entry_t *last_stts_data = (isom_stts_entry_t *)last_stts->data;
    if( sample_count == 1 )
        mdhd->duration = last_stts_data->sample_delta;
    /* Now we have at least 2 samples,
     * but dunno whether 1 stts_entry which has 2 samples or 2 stts_entry which has 1 samle each. */
    else if( !ctts )
    {
        /* use dts instead of cts */
        mdhd->duration = isom_get_dts( stts, sample_count );
        if( last_sample_delta )
        {
            mdhd->duration += last_sample_delta;
            if( isom_replace_last_sample_delta( stbl, last_sample_delta ) )
                return -1;
        }
        else if( last_stts_data->sample_count > 1 )
            mdhd->duration += last_stts_data->sample_delta; /* no need to update last_stts_data->sample_delta */
        else
        {
            /* Remove the last entry. */
            if( lsmash_remove_entry_tail( stts->list, NULL ) )
                return -1;
            /* copy the previous sample_delta. */
            ++ ((isom_stts_entry_t *)stts->list->tail->data)->sample_count;
            mdhd->duration += ((isom_stts_entry_t *)stts->list->tail->data)->sample_delta;
        }
    }
    else
    {
        if( !ctts->list
         ||  ctts->list->entry_count == 0 )
            return -1;
        uint64_t dts        = 0;
        uint64_t max_cts    = 0;
        uint64_t max2_cts   = 0;
        uint64_t min_cts    = UINT64_MAX;
        uint32_t max_offset = 0;
        uint32_t min_offset = UINT32_MAX;
        int32_t  ctd_shift  = trak->cache->timestamp.ctd_shift;
        uint32_t j = 0;
        uint32_t k = 0;
        lsmash_entry_t *stts_entry = stts->list->head;
        lsmash_entry_t *ctts_entry = ctts->list->head;
        for( uint32_t i = 0; i < sample_count; i++ )
        {
            if( !ctts_entry || !stts_entry )
                return -1;
            isom_stts_entry_t *stts_data = (isom_stts_entry_t *)stts_entry->data;
            isom_ctts_entry_t *ctts_data = (isom_ctts_entry_t *)ctts_entry->data;
            if( !stts_data || !ctts_data )
                return -1;
            uint64_t cts;
            if( ctd_shift )
            {
                /* Anyway, add composition to decode timeline shift for calculating maximum and minimum CTS correctly. */
                int32_t sample_offset = (int32_t)ctts_data->sample_offset;
                cts = dts + sample_offset + ctd_shift;
                max_offset = LSMASH_MAX( (int32_t)max_offset, sample_offset );
                min_offset = LSMASH_MIN( (int32_t)min_offset, sample_offset );
            }
            else
            {
                cts = dts + ctts_data->sample_offset;
                max_offset = LSMASH_MAX( max_offset, ctts_data->sample_offset );
                min_offset = LSMASH_MIN( min_offset, ctts_data->sample_offset );
            }
            min_cts = LSMASH_MIN( min_cts, cts );
            if( max_cts < cts )
            {
                max2_cts = max_cts;
                max_cts  = cts;
            }
            else if( max2_cts < cts )
                max2_cts = cts;
            dts += stts_data->sample_delta;
            /* If finished sample_count of current entry, move to next. */
            if( ++j == ctts_data->sample_count )
            {
                ctts_entry = ctts_entry->next;
                j = 0;
            }
            if( ++k == stts_data->sample_count )
            {
                stts_entry = stts_entry->next;
                k = 0;
            }
        }
        dts -= last_stts_data->sample_delta;
        if( file->fragment )
            /* Overall presentation is extended exceeding this initial movie.
             * So, any players shall display the movie exceeding the durations
             * indicated in Movie Header Box, Track Header Boxes and Media Header Boxes.
             * Samples up to the duration indicated in Movie Extends Header Box shall be displayed.
             * In the absence of Movie Extends Header Box, all samples shall be displayed. */
            mdhd->duration += dts + last_sample_delta;
        else
        {
            if( !last_sample_delta )
            {
                /* The spec allows an arbitrary value for the duration of the last sample. So, we pick last-1 sample's. */
                last_sample_delta = max_cts - max2_cts;
            }
            mdhd->duration = max_cts - min_cts + last_sample_delta;
            /* To match dts and media duration, update stts and mdhd relatively. */
            if( mdhd->duration > dts )
                last_sample_delta = mdhd->duration - dts;
            else
                mdhd->duration = dts + last_sample_delta;   /* media duration must not less than last dts. */
        }
        if( isom_replace_last_sample_delta( stbl, last_sample_delta ) )
            return -1;
        /* Explicit composition information and timeline shifting  */
        if( cslg || file->qt_compatible || file->max_isom_version >= 4 )
        {
            if( ctd_shift )
            {
                /* Remove composition to decode timeline shift. */
                max_cts  -= ctd_shift;
                max2_cts -= ctd_shift;
                min_cts  -= ctd_shift;
            }
            int64_t composition_end_time = max_cts + (max_cts - max2_cts);
            if( !file->fragment
             && ((int32_t)min_offset <= INT32_MAX) && ((int32_t)max_offset  <= INT32_MAX)
             && ((int64_t)min_cts    <= INT32_MAX) && (composition_end_time <= INT32_MAX) )
            {
                if( !cslg )
                {
                    if( isom_add_cslg( trak->mdia->minf->stbl ) )
                        return -1;
                    cslg = stbl->cslg;
                }
                cslg->compositionToDTSShift        = ctd_shift;
                cslg->leastDecodeToDisplayDelta    = min_offset;
                cslg->greatestDecodeToDisplayDelta = max_offset;
                cslg->compositionStartTime         = min_cts;
                cslg->compositionEndTime           = composition_end_time;
            }
            else
            {
                if( cslg )
                    lsmash_free( cslg );
                stbl->cslg = NULL;
            }
        }
    }
    if( mdhd->duration > UINT32_MAX && !file->undefined_64_ver )
        mdhd->version = 1;
    return 0;
}

static int isom_update_mvhd_duration( isom_moov_t *moov )
{
    if( !moov
     || !moov->mvhd
     || !moov->mvhd->file )
        return -1;
    isom_mvhd_t *mvhd = moov->mvhd;
    mvhd->duration = 0;
    for( lsmash_entry_t *entry = moov->trak_list.head; entry; entry = entry->next )
    {
        /* We pick maximum track duration as movie duration. */
        isom_trak_t *data = (isom_trak_t *)entry->data;
        if( !data
         || !data->tkhd )
            return -1;
        mvhd->duration = entry != moov->trak_list.head
                       ? LSMASH_MAX( mvhd->duration, data->tkhd->duration )
                       : data->tkhd->duration;
    }
    if( mvhd->duration > UINT32_MAX && !mvhd->file->undefined_64_ver )
        mvhd->version = 1;
    return 0;
}

static int isom_update_tkhd_duration( isom_trak_t *trak )
{
    if( !trak
     || !trak->tkhd
     || !trak->file
     || !trak->file->moov )
        return -1;
    lsmash_file_t *file = trak->file;
    isom_tkhd_t   *tkhd = trak->tkhd;
    tkhd->duration = 0;
    if( file->fragment
     || !trak->edts
     || !trak->edts->elst )
    {
        /* If this presentation might be extended or this track doesn't have edit list, calculate track duration from media duration. */
        if( !trak->mdia
         || !trak->mdia->mdhd
         || !file->moov->mvhd
         ||  trak->mdia->mdhd->timescale == 0 )
            return -1;
        if( trak->mdia->mdhd->duration == 0 && isom_update_mdhd_duration( trak, 0 ) )
            return -1;
        tkhd->duration = trak->mdia->mdhd->duration * ((double)file->moov->mvhd->timescale / trak->mdia->mdhd->timescale);
    }
    else
    {
        /* If the presentation won't be extended and this track has any edit, then track duration is just the sum of the segment_duartions. */
        for( lsmash_entry_t *entry = trak->edts->elst->list->head; entry; entry = entry->next )
        {
            isom_elst_entry_t *data = (isom_elst_entry_t *)entry->data;
            if( !data )
                return -1;
            tkhd->duration += data->segment_duration;
        }
    }
    if( tkhd->duration > UINT32_MAX && !file->undefined_64_ver )
        tkhd->version = 1;
    if( !file->fragment && tkhd->duration == 0 )
        tkhd->duration = tkhd->version == 1 ? 0xffffffffffffffff : 0xffffffff;
    return isom_update_mvhd_duration( file->moov );
}

int lsmash_update_track_duration( lsmash_root_t *root, uint32_t track_ID, uint32_t last_sample_delta )
{
    if( !root )
        return -1;
    lsmash_file_t *file = root->file;
    isom_trak_t *trak = isom_get_trak( file, track_ID );
    if( !trak )
        return -1;
    if( isom_update_mdhd_duration( trak, last_sample_delta ) )
        return -1;
    /* If the presentation won't be extended and this track has any edit, we don't change or update duration in tkhd. */
    return (!file->fragment && trak->edts && trak->edts->elst)
         ? isom_update_mvhd_duration( file->moov )  /* Only update movie duration. */
         : isom_update_tkhd_duration( trak );       /* Also update movie duration internally. */
}

static inline int isom_increment_sample_number_in_entry( uint32_t *sample_number_in_entry, uint32_t sample_count_in_entry, lsmash_entry_t **entry )
{
    if( *sample_number_in_entry != sample_count_in_entry )
    {
        *sample_number_in_entry += 1;
        return 0;
    }
    /* Precede the next entry. */
    *sample_number_in_entry = 1;
    if( *entry )
    {
        *entry = (*entry)->next;
        if( *entry && !(*entry)->data )
            return -1;
    }
    return 0;
}

static int isom_calculate_bitrate_description( isom_mdia_t *mdia, uint32_t *bufferSizeDB, uint32_t *maxBitrate, uint32_t *avgBitrate, uint32_t sample_description_index )
{
    isom_stsz_t *stsz               = mdia->minf->stbl->stsz;
    lsmash_entry_t *stsz_entry      = stsz->list ? stsz->list->head : NULL;
    lsmash_entry_t *stts_entry      = mdia->minf->stbl->stts->list->head;
    lsmash_entry_t *stsc_entry      = NULL;
    lsmash_entry_t *next_stsc_entry = mdia->minf->stbl->stsc->list->head;
    isom_stts_entry_t *stts_data    = NULL;
    isom_stsc_entry_t *stsc_data    = NULL;
    if( next_stsc_entry && !next_stsc_entry->data )
        return -1;
    uint32_t rate                   = 0;
    uint64_t dts                    = 0;
    uint32_t time_wnd               = 0;
    uint32_t timescale              = mdia->mdhd->timescale;
    uint32_t chunk_number           = 0;
    uint32_t sample_number_in_stts  = 1;
    uint32_t sample_number_in_chunk = 1;
    *bufferSizeDB = 0;
    *maxBitrate   = 0;
    *avgBitrate   = 0;
    while( stts_entry )
    {
        if( !stsc_data || sample_number_in_chunk == stsc_data->samples_per_chunk )
        {
            /* Move the next chunk. */
            sample_number_in_chunk = 1;
            ++chunk_number;
            /* Check if the next entry is broken. */
            while( next_stsc_entry && ((isom_stsc_entry_t *)next_stsc_entry->data)->first_chunk < chunk_number )
            {
                /* Just skip broken next entry. */
                next_stsc_entry = next_stsc_entry->next;
                if( next_stsc_entry && !next_stsc_entry->data )
                    return -1;
            }
            /* Check if the next chunk belongs to the next sequence of chunks. */
            if( next_stsc_entry && ((isom_stsc_entry_t *)next_stsc_entry->data)->first_chunk == chunk_number )
            {
                stsc_entry = next_stsc_entry;
                next_stsc_entry = next_stsc_entry->next;
                if( next_stsc_entry && !next_stsc_entry->data )
                    return -1;
                stsc_data = (isom_stsc_entry_t *)stsc_entry->data;
                /* Check if the next contiguous chunks belong to given sample description. */
                if( stsc_data->sample_description_index != sample_description_index )
                {
                    /* Skip chunks which don't belong to given sample description. */
                    uint32_t number_of_skips   = 0;
                    uint32_t first_chunk       = stsc_data->first_chunk;
                    uint32_t samples_per_chunk = stsc_data->samples_per_chunk;
                    while( next_stsc_entry )
                    {
                        if( ((isom_stsc_entry_t *)next_stsc_entry->data)->sample_description_index != sample_description_index )
                        {
                            stsc_data = (isom_stsc_entry_t *)next_stsc_entry->data;
                            number_of_skips  += (stsc_data->first_chunk - first_chunk) * samples_per_chunk;
                            first_chunk       = stsc_data->first_chunk;
                            samples_per_chunk = stsc_data->samples_per_chunk;
                        }
                        else if( ((isom_stsc_entry_t *)next_stsc_entry->data)->first_chunk <= first_chunk )
                            ;   /* broken entry */
                        else
                            break;
                        /* Just skip the next entry. */
                        next_stsc_entry = next_stsc_entry->next;
                        if( next_stsc_entry && !next_stsc_entry->data )
                            return -1;
                    }
                    if( !next_stsc_entry )
                        break;      /* There is no more chunks which don't belong to given sample description. */
                    number_of_skips += (((isom_stsc_entry_t *)next_stsc_entry->data)->first_chunk - first_chunk) * samples_per_chunk;
                    for( uint32_t i = 0; i < number_of_skips; i++ )
                    {
                        if( stsz->list )
                        {
                            if( !stsz_entry )
                                break;
                            stsz_entry = stsz_entry->next;
                        }
                        if( !stts_entry )
                            break;
                        if( isom_increment_sample_number_in_entry( &sample_number_in_stts, ((isom_stts_entry_t *)stts_entry->data)->sample_count, &stts_entry ) )
                            return -1;
                    }
                    if( (stsz->list && !stsz_entry) || !stts_entry )
                        break;
                    chunk_number = stsc_data->first_chunk;
                }
            }
        }
        else
            ++sample_number_in_chunk;
        /* Get current sample's size. */
        uint32_t size;
        if( stsz->list )
        {
            if( !stsz_entry )
                break;
            isom_stsz_entry_t *stsz_data = (isom_stsz_entry_t *)stsz_entry->data;
            if( !stsz_data )
                return -1;
            size = stsz_data->entry_size;
            stsz_entry = stsz_entry->next;
        }
        else
            size = stsz->sample_size;
        /* Get current sample's DTS. */
        if( stts_data )
            dts += stts_data->sample_delta;
        stts_data = (isom_stts_entry_t *)stts_entry->data;
        if( !stts_data )
            return -1;
        isom_increment_sample_number_in_entry( &sample_number_in_stts, stts_data->sample_count, &stts_entry );
        /* Calculate bitrate description. */
        if( *bufferSizeDB < size )
            *bufferSizeDB = size;
        *avgBitrate += size;
        rate += size;
        if( dts > time_wnd + timescale )
        {
            if( rate > *maxBitrate )
                *maxBitrate = rate;
            time_wnd = dts;
            rate = 0;
        }
    }
    double duration = (double)mdia->mdhd->duration / timescale;
    *avgBitrate = (uint32_t)(*avgBitrate / duration);
    if( *maxBitrate == 0 )
        *maxBitrate = *avgBitrate;
    /* Convert to bits per second. */
    *maxBitrate *= 8;
    *avgBitrate *= 8;
    return 0;
}

static int isom_update_bitrate_description( isom_mdia_t *mdia )
{
    if( !mdia
     || !mdia->mdhd
     || !mdia->minf
     || !mdia->minf->stbl )
        return -1;
    isom_stbl_t *stbl = mdia->minf->stbl;
    if( !stbl->stsd
     || !stbl->stsz
     || !stbl->stsc || !stbl->stsc->list
     || !stbl->stts || !stbl->stts->list )
        return -1;
    uint32_t sample_description_index = 0;
    for( lsmash_entry_t *entry = stbl->stsd->list.head; entry; entry = entry->next )
    {
        isom_sample_entry_t *sample_entry = (isom_sample_entry_t *)entry->data;
        if( !sample_entry )
            return -1;
        ++sample_description_index;
        uint32_t bufferSizeDB;
        uint32_t maxBitrate;
        uint32_t avgBitrate;
        /* set bitrate info */
        lsmash_codec_type_t sample_type = (lsmash_codec_type_t)sample_entry->type;
        if( lsmash_check_codec_type_identical( sample_type, ISOM_CODEC_TYPE_AVC1_VIDEO )
         || lsmash_check_codec_type_identical( sample_type, ISOM_CODEC_TYPE_AVC2_VIDEO )
         || lsmash_check_codec_type_identical( sample_type, ISOM_CODEC_TYPE_AVC3_VIDEO )
         || lsmash_check_codec_type_identical( sample_type, ISOM_CODEC_TYPE_AVC4_VIDEO )
         || lsmash_check_codec_type_identical( sample_type, ISOM_CODEC_TYPE_HVC1_VIDEO )
         || lsmash_check_codec_type_identical( sample_type, ISOM_CODEC_TYPE_HEV1_VIDEO ) )
        {
            isom_visual_entry_t *stsd_data = (isom_visual_entry_t *)sample_entry;
            if( !stsd_data )
                return -1;
            isom_btrt_t *btrt = (isom_btrt_t *)isom_get_extension_box_format( &stsd_data->extensions, ISOM_BOX_TYPE_BTRT );
            if( btrt )
            {
                if( isom_calculate_bitrate_description( mdia, &bufferSizeDB, &maxBitrate, &avgBitrate, sample_description_index ) )
                    return -1;
                btrt->bufferSizeDB = bufferSizeDB;
                btrt->maxBitrate   = maxBitrate;
                btrt->avgBitrate   = avgBitrate;
            }
        }
        else if( lsmash_check_codec_type_identical( sample_type, ISOM_CODEC_TYPE_MP4V_VIDEO ) )
        {
            isom_visual_entry_t *stsd_data = (isom_visual_entry_t *)sample_entry;
            if( !stsd_data )
                return -1;
            isom_esds_t *esds = (isom_esds_t *)isom_get_extension_box_format( &stsd_data->extensions, ISOM_BOX_TYPE_ESDS );
            if( !esds || !esds->ES )
                return -1;
            if( isom_calculate_bitrate_description( mdia, &bufferSizeDB, &maxBitrate, &avgBitrate, sample_description_index ) )
                return -1;
            /* FIXME: avgBitrate is 0 only if VBR in proper. */
            if( mp4sys_update_DecoderConfigDescriptor( esds->ES, bufferSizeDB, maxBitrate, 0 ) )
                return -1;
        }
        else if( lsmash_check_codec_type_identical( sample_type, ISOM_CODEC_TYPE_MP4A_AUDIO ) )
        {
            isom_audio_entry_t *stsd_data = (isom_audio_entry_t *)sample_entry;
            if( !stsd_data )
                return -1;
            isom_esds_t *esds = NULL;
            if( ((isom_audio_entry_t *)sample_entry)->version )
            {
                /* MPEG-4 Audio in QTFF */
                isom_wave_t *wave = (isom_wave_t *)isom_get_extension_box_format( &stsd_data->extensions, QT_BOX_TYPE_WAVE );
                if( !wave )
                    return -1;
                esds = (isom_esds_t *)isom_get_extension_box_format( &wave->extensions, ISOM_BOX_TYPE_ESDS );
            }
            else
                esds = (isom_esds_t *)isom_get_extension_box_format( &stsd_data->extensions, ISOM_BOX_TYPE_ESDS );
            if( !esds || !esds->ES )
                return -1;
            if( isom_calculate_bitrate_description( mdia, &bufferSizeDB, &maxBitrate, &avgBitrate, sample_description_index ) )
                return -1;
            /* FIXME: avgBitrate is 0 only if VBR in proper. */
            if( mp4sys_update_DecoderConfigDescriptor( esds->ES, bufferSizeDB, maxBitrate, 0 ) )
                return -1;
        }
        else if( lsmash_check_codec_type_identical( sample_type, ISOM_CODEC_TYPE_ALAC_AUDIO )
              || lsmash_check_codec_type_identical( sample_type,   QT_CODEC_TYPE_ALAC_AUDIO ) )
        {
            isom_audio_entry_t *alac = (isom_audio_entry_t *)sample_entry;
            if( !alac )
                return -1;
            uint8_t *exdata      = NULL;
            uint32_t exdata_size = 0;
            isom_box_t *alac_ext = isom_get_extension_box( &alac->extensions, QT_BOX_TYPE_WAVE );
            if( alac_ext )
            {
                /* Apple Lossless Audio inside QuickTime file format
                 * Though average bitrate field we found is always set to 0 apparently,
                 * we set up maxFrameBytes and avgBitRate fields. */
                if( alac_ext->manager & LSMASH_BINARY_CODED_BOX )
                    exdata = isom_get_child_box_position( alac_ext->binary, alac_ext->size, QT_BOX_TYPE_ALAC, &exdata_size );
                else
                {
                    isom_wave_t *wave     = (isom_wave_t *)alac_ext;
                    isom_box_t  *wave_ext = isom_get_extension_box( &wave->extensions, QT_BOX_TYPE_ALAC );
                    if( !wave_ext || !(wave_ext->manager & LSMASH_BINARY_CODED_BOX) )
                        return -1;
                    exdata      = wave_ext->binary;
                    exdata_size = wave_ext->size;
                }
            }
            else
            {
                /* Apple Lossless Audio inside ISO Base Media file format */
                isom_box_t *ext = isom_get_extension_box( &alac->extensions, ISOM_BOX_TYPE_ALAC );
                if( !ext || !(ext->manager & LSMASH_BINARY_CODED_BOX) )
                    return -1;
                exdata      = ext->binary;
                exdata_size = ext->size;
            }
            if( !exdata || exdata_size < 36 )
                return -1;
            if( isom_calculate_bitrate_description( mdia, &bufferSizeDB, &maxBitrate, &avgBitrate, sample_description_index ) )
                return -1;
            exdata += 24;
            /* maxFrameBytes */
            exdata[0] = (bufferSizeDB >> 24) & 0xff;
            exdata[1] = (bufferSizeDB >> 16) & 0xff;
            exdata[2] = (bufferSizeDB >>  8) & 0xff;
            exdata[3] =  bufferSizeDB        & 0xff;
            /* avgBitRate */
            exdata[4] = (avgBitrate   >> 24) & 0xff;
            exdata[5] = (avgBitrate   >> 16) & 0xff;
            exdata[6] = (avgBitrate   >>  8) & 0xff;
            exdata[7] =  avgBitrate          & 0xff;
        }
        else if( isom_is_waveform_audio( sample_type ) )
        {
            isom_box_t *ext = isom_get_extension_box( &sample_entry->extensions, QT_BOX_TYPE_WAVE );
            if( !ext )
                return -1;
            uint8_t *exdata      = NULL;
            uint32_t exdata_size = 0;
            if( ext->manager & LSMASH_BINARY_CODED_BOX )
                exdata = isom_get_child_box_position( ext->binary, ext->size, sample_type, &exdata_size );
            else
            {
                isom_wave_t *wave     = (isom_wave_t *)ext;
                isom_box_t  *wave_ext = isom_get_extension_box( &wave->extensions, sample_type );
                if( !wave_ext || !(wave_ext->manager & LSMASH_BINARY_CODED_BOX) )
                    return -1;
                exdata      = wave_ext->binary;
                exdata_size = wave_ext->size;
            }
            /* Check whether exdata is valid or not. */
            if( !exdata || exdata_size < ISOM_BASEBOX_COMMON_SIZE + 18 )
                return -1;
            exdata += ISOM_BASEBOX_COMMON_SIZE;
            uint16_t cbSize = exdata[16] | (exdata[17] << 8);
            if( exdata_size < ISOM_BASEBOX_COMMON_SIZE + 18 + cbSize )
                return -1;
            /* WAVEFORMATEX.nAvgBytesPerSec */
            if( isom_calculate_bitrate_description( mdia, &bufferSizeDB, &maxBitrate, &avgBitrate, sample_description_index ) < 0 )
                return -1;
            uint32_t nAvgBytesPerSec = avgBitrate / 8;
            exdata[ 8] =  nAvgBytesPerSec        & 0xff;
            exdata[ 9] = (nAvgBytesPerSec >>  8) & 0xff;
            exdata[10] = (nAvgBytesPerSec >> 16) & 0xff;
            exdata[11] = (nAvgBytesPerSec >> 24) & 0xff;
            if( lsmash_check_codec_type_identical( sample_type, QT_CODEC_TYPE_FULLMP3_AUDIO )
             || lsmash_check_codec_type_identical( sample_type, QT_CODEC_TYPE_MP3_AUDIO ) )
            {
                /* MPEGLAYER3WAVEFORMAT.nBlockSize */
                uint32_t nSamplesPerSec  = exdata[ 4] | (exdata[ 5] << 8) | (exdata[6] << 16) | (exdata[7] << 24);
                uint16_t nFramesPerBlock = exdata[26] | (exdata[27] << 8);
                uint16_t padding         = 0;   /* FIXME? */
                uint16_t nBlockSize      = (144 * (avgBitrate / nSamplesPerSec) + padding) * nFramesPerBlock;
                exdata[24] =  nBlockSize        & 0xff;
                exdata[25] = (nBlockSize >>  8) & 0xff;
            }
        }
        else if( lsmash_check_codec_type_identical( sample_type, ISOM_CODEC_TYPE_DTSC_AUDIO )
              || lsmash_check_codec_type_identical( sample_type, ISOM_CODEC_TYPE_DTSE_AUDIO )
              || lsmash_check_codec_type_identical( sample_type, ISOM_CODEC_TYPE_DTSH_AUDIO )
              || lsmash_check_codec_type_identical( sample_type, ISOM_CODEC_TYPE_DTSL_AUDIO ) )
        {
            isom_audio_entry_t *dts_audio = (isom_audio_entry_t *)sample_entry;
            if( !dts_audio )
                return -1;
            isom_box_t *ext = isom_get_extension_box( &dts_audio->extensions, ISOM_BOX_TYPE_DDTS );
            if( !(ext && (ext->manager & LSMASH_BINARY_CODED_BOX) && ext->binary && ext->size >= 28) )
                return -1;
            if( isom_calculate_bitrate_description( mdia, &bufferSizeDB, &maxBitrate, &avgBitrate, sample_description_index ) )
                return -1;
            if( !stbl->stsz->list )
                maxBitrate = avgBitrate;
            uint8_t *exdata = ext->binary + 12;
            exdata[0] = (maxBitrate >> 24) & 0xff;
            exdata[1] = (maxBitrate >> 16) & 0xff;
            exdata[2] = (maxBitrate >>  8) & 0xff;
            exdata[3] =  maxBitrate        & 0xff;
            exdata[4] = (avgBitrate >> 24) & 0xff;
            exdata[5] = (avgBitrate >> 16) & 0xff;
            exdata[6] = (avgBitrate >>  8) & 0xff;
            exdata[7] =  avgBitrate        & 0xff;
        }
        else if( lsmash_check_codec_type_identical( sample_type, ISOM_CODEC_TYPE_EC_3_AUDIO ) )
        {
            isom_audio_entry_t *eac3 = (isom_audio_entry_t *)sample_entry;
            if( !eac3 )
                return -1;
            isom_box_t *ext = isom_get_extension_box( &eac3->extensions, ISOM_BOX_TYPE_DEC3 );
            if( !(ext && (ext->manager & LSMASH_BINARY_CODED_BOX) && ext->binary && ext->size >= 10) )
                return -1;
            uint16_t bitrate;
            if( stbl->stsz->list )
            {
                if( isom_calculate_bitrate_description( mdia, &bufferSizeDB, &maxBitrate, &avgBitrate, sample_description_index ) )
                    return -1;
                bitrate = maxBitrate / 1000;    /* Use maximum bitrate if VBR. */
            }
            else
                bitrate = stbl->stsz->sample_size * (eac3->samplerate >> 16) / 192000;      /* 192000 == 1536 * 1000 / 8 */
            uint8_t *exdata = ext->binary + 8;
            exdata[0] = (bitrate >> 5) & 0xff;
            exdata[1] = (bitrate & 0x1f) << 3;
        }
    }
    return sample_description_index ? 0 : -1;
}

static int isom_check_mandatory_boxes( lsmash_file_t *file )
{
    if( !file
     || !file->moov
     || !file->moov->mvhd )
        return -1;
    /* A movie requires at least one track. */
    if( !file->moov->trak_list.head )
        return -1;
    for( lsmash_entry_t *entry = file->moov->trak_list.head; entry; entry = entry->next )
    {
        isom_trak_t *trak = (isom_trak_t *)entry->data;
        if( !trak
         || !trak->tkhd
         || !trak->mdia
         || !trak->mdia->mdhd
         || !trak->mdia->hdlr
         || !trak->mdia->minf
         || !trak->mdia->minf->dinf
         || !trak->mdia->minf->dinf->dref
         || !trak->mdia->minf->stbl
         || !trak->mdia->minf->stbl->stsd
         || !trak->mdia->minf->stbl->stsz
         || !trak->mdia->minf->stbl->stts
         || !trak->mdia->minf->stbl->stsc
         || !trak->mdia->minf->stbl->stco )
            return -1;
        if( file->qt_compatible && !trak->mdia->minf->hdlr )
            return -1;
        isom_stbl_t *stbl = trak->mdia->minf->stbl;
        if( !stbl->stsd->list.head )
            return -1;
        if( !file->fragment
         && (!stbl->stsd->list.head
          || !stbl->stts->list || !stbl->stts->list->head
          || !stbl->stsc->list || !stbl->stsc->list->head
          || !stbl->stco->list || !stbl->stco->list->head) )
            return -1;
    }
    if( !file->fragment )
        return 0;
    if( !file->moov->mvex )
        return -1;
    for( lsmash_entry_t *entry = file->moov->mvex->trex_list.head; entry; entry = entry->next )
        if( !entry->data )  /* trex */
            return -1;
    return 0;
}

static inline uint64_t isom_get_current_mp4time( void )
{
    return (uint64_t)time( NULL ) + ISOM_MAC_EPOCH_OFFSET;
}

static int isom_set_media_creation_time( isom_trak_t *trak, uint64_t current_mp4time )
{
    if( !trak->mdia
     || !trak->mdia->mdhd )
        return -1;
    isom_mdhd_t *mdhd = trak->mdia->mdhd;
    if( mdhd->creation_time == 0 )
        mdhd->creation_time = mdhd->modification_time = current_mp4time;
    return 0;
}

static int isom_set_track_creation_time( isom_trak_t *trak, uint64_t current_mp4time )
{
    if( !trak
     || !trak->tkhd )
        return -1;
    isom_tkhd_t *tkhd = trak->tkhd;
    if( tkhd->creation_time == 0 )
        tkhd->creation_time = tkhd->modification_time = current_mp4time;
    if( isom_set_media_creation_time( trak, current_mp4time ) )
        return -1;
    return 0;
}

static int isom_set_movie_creation_time( lsmash_file_t *file )
{
    if( !file
     || !file->moov
     || !file->moov->mvhd )
        return -1;
    uint64_t current_mp4time = isom_get_current_mp4time();
    for( uint32_t i = 1; i <= file->moov->trak_list.entry_count; i++ )
        if( isom_set_track_creation_time( isom_get_trak( file, i ), current_mp4time ) )
            return -1;
    isom_mvhd_t *mvhd = file->moov->mvhd;
    if( mvhd->creation_time == 0 )
        mvhd->creation_time = mvhd->modification_time = current_mp4time;
    return 0;
}

int isom_setup_handler_reference( isom_hdlr_t *hdlr, uint32_t media_type )
{
    isom_box_t    *parent = hdlr->parent;
    lsmash_file_t *file   = hdlr->file;
    if( !parent || !file )
        return -1;
    isom_mdia_t *mdia = lsmash_check_box_type_identical( parent->type, ISOM_BOX_TYPE_MDIA ) ? (isom_mdia_t *)parent : NULL;
    isom_meta_t *meta = lsmash_check_box_type_identical( parent->type, ISOM_BOX_TYPE_META ) ? (isom_meta_t *)parent
                      : lsmash_check_box_type_identical( parent->type,   QT_BOX_TYPE_META ) ? (isom_meta_t *)parent : NULL;
    uint32_t type    = mdia ? (file->qt_compatible ? QT_HANDLER_TYPE_MEDIA : 0) : (meta ? 0 : QT_HANDLER_TYPE_DATA);
    uint32_t subtype = media_type;
    hdlr->componentType    = type;
    hdlr->componentSubtype = subtype;
    char *type_name    = NULL;
    char *subtype_name = NULL;
    uint8_t type_name_length    = 0;
    uint8_t subtype_name_length = 0;
    if( mdia )
        type_name = "Media ";
    else if( meta )
        type_name = "Metadata ";
    else    /* if( minf ) */
        type_name = "Data ";
    type_name_length = strlen( type_name );
    struct
    {
        uint32_t subtype;
        char    *subtype_name;
        uint8_t  subtype_name_length;
    } subtype_table[] =
        {
            { ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK,          "Sound ",    6 },
            { ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK,          "Video ",    6 },
            { ISOM_MEDIA_HANDLER_TYPE_HINT_TRACK,           "Hint ",     5 },
            { ISOM_MEDIA_HANDLER_TYPE_TIMED_METADATA_TRACK, "Metadata ", 9 },
            { ISOM_MEDIA_HANDLER_TYPE_TEXT_TRACK,           "Text ",     5 },
            { ISOM_META_HANDLER_TYPE_ITUNES_METADATA,       "iTunes ",   7 },
            { QT_REFERENCE_HANDLER_TYPE_ALIAS,              "Alias ",    6 },
            { QT_REFERENCE_HANDLER_TYPE_RESOURCE,           "Resource ", 9 },
            { QT_REFERENCE_HANDLER_TYPE_URL,                "URL ",      4 },
            { subtype,                                      "Unknown ",  8 }
        };
    for( int i = 0; subtype_table[i].subtype; i++ )
        if( subtype == subtype_table[i].subtype )
        {
            subtype_name        = subtype_table[i].subtype_name;
            subtype_name_length = subtype_table[i].subtype_name_length;
            break;
        }
    uint32_t name_length = 15 + subtype_name_length + type_name_length + file->isom_compatible + file->qt_compatible;
    uint8_t *name = lsmash_malloc( name_length );
    if( !name )
        return -1;
    if( file->qt_compatible )
        name[0] = name_length & 0xff;
    memcpy( name + file->qt_compatible, "L-SMASH ", 8 );
    memcpy( name + file->qt_compatible + 8, subtype_name, subtype_name_length );
    memcpy( name + file->qt_compatible + 8 + subtype_name_length, type_name, type_name_length );
    memcpy( name + file->qt_compatible + 8 + subtype_name_length + type_name_length, "Handler", 7 );
    if( file->isom_compatible )
        name[name_length - 1] = 0;
    hdlr->componentName        = name;
    hdlr->componentName_length = name_length;
    return 0;
}

/*******************************
    public interfaces
*******************************/

/*---- track manipulators ----*/

void lsmash_delete_track( lsmash_root_t *root, uint32_t track_ID )
{
    if( !root
     || !root->file
     || !root->file->moov )
        return;
    for( lsmash_entry_t *entry = root->file->moov->trak_list.head; entry; entry = entry->next )
    {
        isom_trak_t *trak = (isom_trak_t *)entry->data;
        if( !trak
         || !trak->tkhd )
            return;
        if( trak->tkhd->track_ID == track_ID )
        {
            isom_remove_box_by_itself( trak );
            return;
        }
    }
}

uint32_t lsmash_create_track( lsmash_root_t *root, lsmash_media_type media_type )
{
    if( !root )
        return 0;
    lsmash_file_t *file = root->file;
    isom_trak_t *trak = isom_add_trak( file->moov );
    if( !trak
     || !trak->file
     || !trak->file->moov
     || !trak->file->moov->mvhd )
        goto fail;
    if( isom_add_tkhd( trak )
     || isom_add_mdia( trak )
     || isom_add_mdhd( trak->mdia )
     || isom_add_minf( trak->mdia )
     || isom_add_dinf( trak->mdia->minf )
     || isom_add_dref( trak->mdia->minf->dinf )
     || isom_add_stbl( trak->mdia->minf )
     || isom_add_stsd( trak->mdia->minf->stbl )
     || isom_add_stts( trak->mdia->minf->stbl )
     || isom_add_stsc( trak->mdia->minf->stbl )
     || isom_add_stco( trak->mdia->minf->stbl )
     || isom_add_stsz( trak->mdia->minf->stbl ) )
        goto fail;
    if( isom_add_hdlr( trak->mdia )
     || isom_setup_handler_reference( trak->mdia->hdlr, media_type ) )
        goto fail;
    if( file->qt_compatible )
    {
        if( isom_add_hdlr( trak->mdia->minf )
         || isom_setup_handler_reference( trak->mdia->minf->hdlr, QT_REFERENCE_HANDLER_TYPE_URL ) )
            goto fail;
    }
    switch( media_type )
    {
        case ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK :
            if( isom_add_vmhd( trak->mdia->minf ) )
                goto fail;
            trak->mdia->minf->vmhd->flags = 0x000001;
            break;
        case ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK :
            if( isom_add_smhd( trak->mdia->minf ) )
                goto fail;
            break;
        case ISOM_MEDIA_HANDLER_TYPE_HINT_TRACK :
            if( isom_add_hmhd( trak->mdia->minf ) )
                goto fail;
            break;
        case ISOM_MEDIA_HANDLER_TYPE_TEXT_TRACK :
            if( file->qt_compatible || file->itunes_movie )
            {
                if( isom_add_gmhd( trak->mdia->minf )
                 || isom_add_gmin( trak->mdia->minf->gmhd )
                 || isom_add_text( trak->mdia->minf->gmhd ) )
                    return 0;
                /* Default Text Media Information Box. */
                {
                    isom_text_t *text = trak->mdia->minf->gmhd->text;
                    text->matrix[0] = 0x00010000;
                    text->matrix[4] = 0x00010000;
                    text->matrix[8] = 0x40000000;
                }
            }
            else
                goto fail;  /* We support only reference text media track for chapter yet. */
            break;
        default :
            if( isom_add_nmhd( trak->mdia->minf ) )
                goto fail;
            break;
    }
    /* Default Track Header Box. */
    {
        isom_tkhd_t *tkhd = trak->tkhd;
        if( media_type == ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK )
            tkhd->volume = 0x0100;
        tkhd->matrix[0] = 0x00010000;
        tkhd->matrix[4] = 0x00010000;
        tkhd->matrix[8] = 0x40000000;
        tkhd->duration  = 0xffffffff;
        tkhd->track_ID  = trak->file->moov->mvhd->next_track_ID ++;
    }
    trak->mdia->mdhd->language = file->qt_compatible ? 0 : ISOM_LANGUAGE_CODE_UNDEFINED;
    return trak->tkhd->track_ID;
fail:
    isom_remove_box_by_itself( trak );
    return 0;
}

uint32_t lsmash_get_track_ID( lsmash_root_t *root, uint32_t track_number )
{
    if( !root
     || !root->file
     || !root->file->moov )
        return 0;
    isom_trak_t *trak = (isom_trak_t *)lsmash_get_entry_data( &root->file->moov->trak_list, track_number );
    if( !trak
     || !trak->tkhd )
        return 0;
    return trak->tkhd->track_ID;
}

void lsmash_initialize_track_parameters( lsmash_track_parameters_t *param )
{
    memset( param, 0, sizeof(lsmash_track_parameters_t) );
    param->audio_volume = 0x0100;
    param->matrix[0]    = 0x00010000;
    param->matrix[4]    = 0x00010000;
    param->matrix[8]    = 0x40000000;
}

int lsmash_set_track_parameters( lsmash_root_t *root, uint32_t track_ID, lsmash_track_parameters_t *param )
{
    if( !root )
        return -1;
    lsmash_file_t *file = root->file;
    isom_trak_t *trak = isom_get_trak( file, track_ID );
    if( !trak
     || !trak->mdia
     || !trak->mdia->hdlr
     || !file->moov->mvhd )
        return -1;
    /* Prepare Track Aperture Modes if required. */
    if( file->qt_compatible && param->aperture_modes )
    {
        if( !trak->tapt && isom_add_tapt( trak ) )
            return -1;
        isom_tapt_t *tapt = trak->tapt;
        if( (!tapt->clef && isom_add_clef( tapt ))
         || (!tapt->prof && isom_add_prof( tapt ))
         || (!tapt->enof && isom_add_enof( tapt )) )
            return -1;
    }
    else
        isom_remove_box_by_itself( trak->tapt );
    /* Set up Track Header. */
    uint32_t media_type = trak->mdia->hdlr->componentSubtype;
    isom_tkhd_t *tkhd = trak->tkhd;
    tkhd->flags    = param->mode;
    tkhd->track_ID = param->track_ID ? param->track_ID : tkhd->track_ID;
    tkhd->duration = !trak->edts || !trak->edts->elst ? param->duration : tkhd->duration;
    /* Template fields
     *   alternate_group, layer, volume and matrix
     * According to 14496-14, these value are all set to defaut values in 14496-12.
     * And when a file is read as an MPEG-4 file, these values shall be ignored.
     * If a file complies with other specifications, then those fields may have non-default values
     * as required by those other specifications. */
    if( param->alternate_group )
    {
        if( file->qt_compatible || file->itunes_movie || file->max_3gpp_version >= 4 )
            tkhd->alternate_group = param->alternate_group;
        else
        {
            tkhd->alternate_group = 0;
            lsmash_log( NULL, LSMASH_LOG_WARNING,
                        "alternate_group is specified but not compatible with any of the brands. It won't be set.\n" );
        }
    }
    else
        tkhd->alternate_group = 0;
    if( file->qt_compatible || file->itunes_movie )
    {
        tkhd->layer  = media_type == ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK ? param->video_layer  : 0;
        tkhd->volume = media_type == ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK ? param->audio_volume : 0;
        if( media_type == ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK )
            for( int i = 0; i < 9; i++ )
                tkhd->matrix[i] = param->matrix[i];
        else
            for( int i = 0; i < 9; i++ )
                tkhd->matrix[i] = 0;
    }
    else
    {
        tkhd->layer     = 0;
        tkhd->volume    = media_type == ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK ? 0x0100 : 0;
        tkhd->matrix[0] = 0x00010000;
        tkhd->matrix[1] = 0;
        tkhd->matrix[2] = 0;
        tkhd->matrix[3] = 0;
        tkhd->matrix[4] = 0x00010000;
        tkhd->matrix[5] = 0;
        tkhd->matrix[6] = 0;
        tkhd->matrix[7] = 0;
        tkhd->matrix[8] = 0x40000000;
    }
    /* visual presentation size */
    tkhd->width  = media_type == ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK ? param->display_width  : 0;
    tkhd->height = media_type == ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK ? param->display_height : 0;
    /* Update next_track_ID if needed. */
    if( file->moov->mvhd->next_track_ID <= tkhd->track_ID )
        file->moov->mvhd->next_track_ID = tkhd->track_ID + 1;
    return 0;
}

int lsmash_get_track_parameters( lsmash_root_t *root, uint32_t track_ID, lsmash_track_parameters_t *param )
{
    if( !root )
        return -1;
    isom_trak_t *trak = isom_get_trak( root->file, track_ID );
    if( !trak )
        return -1;
    isom_tkhd_t *tkhd = trak->tkhd;
    param->mode            = tkhd->flags;
    param->track_ID        = tkhd->track_ID;
    param->duration        = tkhd->duration;
    param->video_layer     = tkhd->layer;
    param->alternate_group = tkhd->alternate_group;
    param->audio_volume    = tkhd->volume;
    for( int i = 0; i < 9; i++ )
        param->matrix[i]   = tkhd->matrix[i];
    param->display_width   = tkhd->width;
    param->display_height  = tkhd->height;
    param->aperture_modes  = !!trak->tapt;
    return 0;
}

static int isom_set_media_handler_name( lsmash_file_t *file, uint32_t track_ID, char *handler_name )
{
    isom_trak_t *trak = isom_get_trak( file, track_ID );
    if( !trak
     || !trak->mdia
     || !trak->mdia->hdlr )
        return -1;
    isom_hdlr_t *hdlr = trak->mdia->hdlr;
    uint8_t *name        = NULL;
    uint32_t name_length = strlen( handler_name ) + file->isom_compatible + file->qt_compatible;
    if( file->qt_compatible )
        name_length = LSMASH_MIN( name_length, 255 );
    if( name_length > hdlr->componentName_length && hdlr->componentName )
        name = lsmash_realloc( hdlr->componentName, name_length );
    else if( !hdlr->componentName )
        name = lsmash_malloc( name_length );
    else
        name = hdlr->componentName;
    if( !name )
        return -1;
    if( file->qt_compatible )
        name[0] = name_length & 0xff;
    memcpy( name + file->qt_compatible, handler_name, strlen( handler_name ) );
    if( file->isom_compatible )
        name[name_length - 1] = 0;
    hdlr->componentName        = name;
    hdlr->componentName_length = name_length;
    return 0;
}

static int isom_set_data_handler_name( lsmash_file_t *file, uint32_t track_ID, char *handler_name )
{
    isom_trak_t *trak = isom_get_trak( file, track_ID );
    if( !trak
     || !trak->mdia
     || !trak->mdia->minf
     || !trak->mdia->minf->hdlr )
        return -1;
    isom_hdlr_t *hdlr = trak->mdia->minf->hdlr;
    uint8_t *name        = NULL;
    uint32_t name_length = strlen( handler_name ) + file->isom_compatible + file->qt_compatible;
    if( file->qt_compatible )
        name_length = LSMASH_MIN( name_length, 255 );
    if( name_length > hdlr->componentName_length && hdlr->componentName )
        name = lsmash_realloc( hdlr->componentName, name_length );
    else if( !hdlr->componentName )
        name = lsmash_malloc( name_length );
    else
        name = hdlr->componentName;
    if( !name )
        return -1;
    if( file->qt_compatible )
        name[0] = name_length & 0xff;
    memcpy( name + file->qt_compatible, handler_name, strlen( handler_name ) );
    if( file->isom_compatible )
        name[name_length - 1] = 0;
    hdlr->componentName        = name;
    hdlr->componentName_length = name_length;
    return 0;
}

uint32_t lsmash_get_media_timescale( lsmash_root_t *root, uint32_t track_ID )
{
    if( !root )
        return 0;
    isom_trak_t *trak = isom_get_trak( root->file, track_ID );
    if( !trak
     || !trak->mdia
     || !trak->mdia->mdhd )
        return 0;
    return trak->mdia->mdhd->timescale;
}

uint64_t lsmash_get_media_duration( lsmash_root_t *root, uint32_t track_ID )
{
    if( !root )
        return 0;
    isom_trak_t *trak = isom_get_trak( root->file, track_ID );
    if( !trak
     || !trak->mdia
     || !trak->mdia->mdhd )
        return 0;
    return trak->mdia->mdhd->duration;
}

uint64_t lsmash_get_track_duration( lsmash_root_t *root, uint32_t track_ID )
{
    if( !root )
        return 0;
    isom_trak_t *trak = isom_get_trak( root->file, track_ID );
    if( !trak
     || !trak->tkhd )
        return 0;
    return trak->tkhd->duration;
}

uint32_t lsmash_get_last_sample_delta( lsmash_root_t *root, uint32_t track_ID )
{
    if( !root )
        return 0;
    isom_trak_t *trak = isom_get_trak( root->file, track_ID );
    if( !trak
     || !trak->mdia
     || !trak->mdia->minf
     || !trak->mdia->minf->stbl
     || !trak->mdia->minf->stbl->stts
     || !trak->mdia->minf->stbl->stts->list
     || !trak->mdia->minf->stbl->stts->list->tail
     || !trak->mdia->minf->stbl->stts->list->tail->data )
        return 0;
    return ((isom_stts_entry_t *)trak->mdia->minf->stbl->stts->list->tail->data)->sample_delta;
}

uint32_t lsmash_get_start_time_offset( lsmash_root_t *root, uint32_t track_ID )
{
    if( !root )
        return 0;
    isom_trak_t *trak = isom_get_trak( root->file, track_ID );
    if( !trak
     || !trak->mdia
     || !trak->mdia->minf
     || !trak->mdia->minf->stbl
     || !trak->mdia->minf->stbl->ctts
     || !trak->mdia->minf->stbl->ctts->list
     || !trak->mdia->minf->stbl->ctts->list->head
     || !trak->mdia->minf->stbl->ctts->list->head->data )
        return 0;
    return ((isom_ctts_entry_t *)trak->mdia->minf->stbl->ctts->list->head->data)->sample_offset;
}

uint32_t lsmash_get_composition_to_decode_shift( lsmash_root_t *root, uint32_t track_ID )
{
    if( !root )
        return 0;
    lsmash_file_t *file = root->file;
    isom_trak_t *trak = isom_get_trak( file, track_ID );
    if( !trak
     || !trak->mdia
     || !trak->mdia->minf
     || !trak->mdia->minf->stbl )
        return 0;
    uint32_t sample_count = isom_get_sample_count( trak );
    if( sample_count == 0 )
        return 0;
    isom_stbl_t *stbl = trak->mdia->minf->stbl;
    if( !stbl->stts || !stbl->stts->list
     || !stbl->ctts || !stbl->ctts->list )
        return 0;
    if( !(file->max_isom_version >= 4 && stbl->ctts->version == 1) && !file->qt_compatible )
        return 0;   /* This movie shall not have composition to decode timeline shift. */
    lsmash_entry_t *stts_entry = stbl->stts->list->head;
    lsmash_entry_t *ctts_entry = stbl->ctts->list->head;
    if( !stts_entry || !ctts_entry )
        return 0;
    uint64_t dts       = 0;
    uint64_t cts       = 0;
    uint32_t ctd_shift = 0;
    uint32_t i         = 0;
    uint32_t j         = 0;
    for( uint32_t k = 0; k < sample_count; i++ )
    {
        isom_stts_entry_t *stts_data = (isom_stts_entry_t *)stts_entry->data;
        isom_ctts_entry_t *ctts_data = (isom_ctts_entry_t *)ctts_entry->data;
        if( !stts_data || !ctts_data )
            return 0;
        cts = dts + (int32_t)ctts_data->sample_offset;
        if( dts > cts + ctd_shift )
            ctd_shift = dts - cts;
        dts += stts_data->sample_delta;
        if( ++i == stts_data->sample_count )
        {
            stts_entry = stts_entry->next;
            if( !stts_entry )
                return 0;
            i = 0;
        }
        if( ++j == ctts_data->sample_count )
        {
            ctts_entry = ctts_entry->next;
            if( !ctts_entry )
                return 0;
            j = 0;
        }
    }
    return ctd_shift;
}

uint16_t lsmash_pack_iso_language( char *iso_language )
{
    if( !iso_language || strlen( iso_language ) != 3 )
        return 0;
    return (uint16_t)LSMASH_PACK_ISO_LANGUAGE( iso_language[0], iso_language[1], iso_language[2] );
}

static int isom_iso2mac_language( uint16_t ISO_language, uint16_t *MAC_language )
{
    if( !MAC_language )
        return -1;
    int i = 0;
    for( ; isom_languages[i].iso_name; i++ )
        if( ISO_language == isom_languages[i].iso_name )
            break;
    if( !isom_languages[i].iso_name )
        return -1;
    *MAC_language = isom_languages[i].mac_value;
    return 0;
}

static int isom_mac2iso_language( uint16_t MAC_language, uint16_t *ISO_language )
{
    if( !ISO_language )
        return -1;
    int i = 0;
    for( ; isom_languages[i].iso_name; i++ )
        if( MAC_language == isom_languages[i].mac_value )
            break;
    *ISO_language = isom_languages[i].iso_name ? isom_languages[i].iso_name : ISOM_LANGUAGE_CODE_UNDEFINED;
    return 0;
}

static int isom_set_media_language( lsmash_file_t *file, uint32_t track_ID, uint16_t ISO_language, uint16_t MAC_language )
{
    isom_trak_t *trak = isom_get_trak( file, track_ID );
    if( !trak
     || !trak->mdia
     || !trak->mdia->mdhd )
        return -1;
    uint16_t language = 0;
    if( file->isom_compatible )
    {
        if( ISO_language )
            language = ISO_language;
        else if( MAC_language )
        {
            if( isom_mac2iso_language( MAC_language, &language ) )
                return -1;
        }
        else
            language = ISOM_LANGUAGE_CODE_UNDEFINED;
    }
    else if( file->qt_compatible )
    {
        if( ISO_language )
        {
            if( isom_iso2mac_language( ISO_language, &language ) )
                return -1;
        }
        else
            language = MAC_language;
    }
    else
        return -1;
    trak->mdia->mdhd->language = language;
    return 0;
}

static int isom_add_sample_grouping( isom_box_t *parent, isom_grouping_type grouping_type )
{
    isom_sgpd_t *sgpd;
    isom_sbgp_t *sbgp;
    if( NULL == (sgpd = isom_add_sgpd( parent ))
     || NULL == (sbgp = isom_add_sbgp( parent )) )
        return -1;
    sbgp->grouping_type = grouping_type;
    sgpd->grouping_type = grouping_type;
    sgpd->version       = 1;    /* We use version 1 for Sample Group Description Box because it is recommended in the spec. */
    switch( grouping_type )
    {
        case ISOM_GROUP_TYPE_RAP :
            sgpd->default_length = 1;
            break;
        case ISOM_GROUP_TYPE_ROLL :
            sgpd->default_length = 2;
            break;
        default :
            /* We don't consider other grouping types currently. */
            break;
    }
    return 0;
}

static int isom_create_sample_grouping( isom_trak_t *trak, isom_grouping_type grouping_type )
{
    lsmash_file_t *file = trak->file;
    switch( grouping_type )
    {
        case ISOM_GROUP_TYPE_RAP :
            assert( file->max_isom_version >= 6 );
            break;
        case ISOM_GROUP_TYPE_ROLL :
            assert( file->avc_extensions || file->qt_compatible );
            break;
        default :
            assert( 0 );
            break;
    }
    if( isom_add_sample_grouping( (isom_box_t *)trak->mdia->minf->stbl, grouping_type ) < 0 )
        return -1;
    if( trak->cache->fragment && file->max_isom_version >= 6 )
        switch( grouping_type )
        {
            case ISOM_GROUP_TYPE_RAP :
                trak->cache->fragment->rap_grouping = 1;
                break;
            case ISOM_GROUP_TYPE_ROLL :
                trak->cache->fragment->roll_grouping = 1;
                break;
            default :
                /* We don't consider other grouping types currently. */
                break;
        }
    return 0;
}

void lsmash_initialize_media_parameters( lsmash_media_parameters_t *param )
{
    memset( param, 0, sizeof(lsmash_media_parameters_t) );
    param->timescale = 1;
}

int lsmash_set_media_parameters( lsmash_root_t *root, uint32_t track_ID, lsmash_media_parameters_t *param )
{
    if( !root )
        return -1;
    lsmash_file_t *file = root->file;
    isom_trak_t *trak = isom_get_trak( file, track_ID );
    if( !trak
     || !trak->mdia
     || !trak->mdia->mdhd
     || !trak->mdia->minf
     || !trak->mdia->minf->stbl )
        return -1;
    trak->mdia->mdhd->timescale = param->timescale;
    if( isom_set_media_language( file, track_ID, param->ISO_language, param->MAC_language ) )
        return -1;
    if( param->media_handler_name
     && isom_set_media_handler_name( file, track_ID, param->media_handler_name ) )
        return -1;
    if( file->qt_compatible && param->data_handler_name
     && isom_set_data_handler_name( file, track_ID, param->data_handler_name ) )
        return -1;
    if( (file->avc_extensions || file->qt_compatible) && param->roll_grouping
     && isom_create_sample_grouping( trak, ISOM_GROUP_TYPE_ROLL ) )
        return -1;
    if( (file->max_isom_version >= 6) && param->rap_grouping
     && isom_create_sample_grouping( trak, ISOM_GROUP_TYPE_RAP ) )
        return -1;
    return 0;
}

int lsmash_get_media_parameters( lsmash_root_t *root, uint32_t track_ID, lsmash_media_parameters_t *param )
{
    if( !root )
        return -1;
    lsmash_file_t *file = root->file;
    isom_trak_t *trak = isom_get_trak( file, track_ID );
    if( !trak
     || !trak->mdia
     || !trak->mdia->mdhd
     || !trak->mdia->hdlr
     || !trak->mdia->minf
     || !trak->mdia->minf->stbl )
        return -1;
    isom_mdhd_t *mdhd = trak->mdia->mdhd;
    isom_stbl_t *stbl = trak->mdia->minf->stbl;
    param->timescale    = mdhd->timescale;
    param->handler_type = trak->mdia->hdlr->componentSubtype;
    param->duration     = mdhd->duration;
    /* Whether sample grouping present. */
    {
        isom_sbgp_t *sbgp;
        isom_sgpd_t *sgpd;
        sbgp = isom_get_sample_to_group         ( stbl, ISOM_GROUP_TYPE_ROLL );
        sgpd = isom_get_sample_group_description( stbl, ISOM_GROUP_TYPE_ROLL );
        param->roll_grouping = sbgp && sgpd;
        sbgp = isom_get_sample_to_group         ( stbl, ISOM_GROUP_TYPE_RAP );
        sgpd = isom_get_sample_group_description( stbl, ISOM_GROUP_TYPE_RAP );
        param->rap_grouping = sbgp && sgpd;
    }
    /* Get media language. */
    if( mdhd->language >= 0x800 )
    {
        param->MAC_language = 0;
        param->ISO_language = mdhd->language;
    }
    else
    {
        param->MAC_language = mdhd->language;
        param->ISO_language = 0;
    }
    /* Get handler name(s). */
    isom_hdlr_t *hdlr = trak->mdia->hdlr;
    int length = LSMASH_MIN( 255, hdlr->componentName_length );
    if( length )
    {
        memcpy( param->media_handler_name_shadow, hdlr->componentName + file->qt_compatible, length );
        param->media_handler_name_shadow[length - 2 + file->isom_compatible + file->qt_compatible] = '\0';
        param->media_handler_name = param->media_handler_name_shadow;
    }
    else
    {
        param->media_handler_name = NULL;
        memset( param->media_handler_name_shadow, 0, sizeof(param->media_handler_name_shadow) );
    }
    if( trak->mdia->minf->hdlr )
    {
        hdlr = trak->mdia->minf->hdlr;
        length = LSMASH_MIN( 255, hdlr->componentName_length );
        if( length )
        {
            memcpy( param->data_handler_name_shadow, hdlr->componentName + file->qt_compatible, length );
            param->data_handler_name_shadow[length - 2 + file->isom_compatible + file->qt_compatible] = '\0';
            param->data_handler_name = param->data_handler_name_shadow;
        }
        else
        {
            param->data_handler_name = NULL;
            memset( param->data_handler_name_shadow, 0, sizeof(param->data_handler_name_shadow) );
        }
    }
    else
    {
        param->data_handler_name = NULL;
        memset( param->data_handler_name_shadow, 0, sizeof(param->data_handler_name_shadow) );
    }
    return 0;
}

/*---- movie manipulators ----*/

void lsmash_discard_boxes( lsmash_root_t *root )
{
    if( !root || !root->file )
        return;
    isom_remove_all_extension_boxes( &root->file->extensions );
}

static void isom_remove_root( lsmash_root_t *root )
{
    if( !root )
        return;
    isom_remove_all_extension_boxes( &root->extensions );
    lsmash_free( root );
}

lsmash_root_t *lsmash_create_root( void )
{
    lsmash_root_t *root = lsmash_malloc_zero( sizeof(lsmash_root_t) );
    if( !root )
        return NULL;
    root->destruct = (isom_extension_destructor_t)isom_remove_root;
    root->root     = root;
    return root;
}

void lsmash_destroy_root( lsmash_root_t *root )
{
    isom_remove_box_by_itself( root );
}

static int fread_wrapper( void *opaque, uint8_t *buf, int size )
{
    int read_size = fread( buf, 1, size, (FILE *)opaque );
    if( read_size != size && !feof( (FILE *)opaque ) )
        return -1;
    return read_size;
}

static int fwrite_wrapper( void *opaque, uint8_t *buf, int size )
{
    return fwrite( buf, 1, size, (FILE *)opaque );
}

static int64_t fseek_wrapper( void *opaque, int64_t offset, int whence )
{
    if( lsmash_fseek( (FILE *)opaque, offset, whence ) != 0 )
        return -1;
    return lsmash_ftell( (FILE *)opaque );
}

int lsmash_open_file
(
    const char               *filename,
    int                       open_mode,
    lsmash_file_parameters_t *param
)
{
    if( !filename || !param )
        return -1;
    char mode[4] = { 0 };
    lsmash_file_mode file_mode;
    if( open_mode == 0 )
    {
        memcpy( mode, "w+b", 4 );
        file_mode = LSMASH_FILE_MODE_WRITE
                  | LSMASH_FILE_MODE_BOX
                  | LSMASH_FILE_MODE_INITIALIZATION
                  | LSMASH_FILE_MODE_MEDIA;
    }
#ifdef LSMASH_DEMUXER_ENABLED
    else if( open_mode == 1 )
    {
        memcpy( mode, "rb", 3 );
        file_mode = LSMASH_FILE_MODE_READ;
    }
#endif
    if( !mode[0] )
        return -1;
    FILE *stream   = NULL;
    int   seekable = 1;
    if( !strcmp( filename, "-" ) )
    {
        if( file_mode & LSMASH_FILE_MODE_READ )
            stream = stdin;
        else if( file_mode & LSMASH_FILE_MODE_WRITE )
        {
            stream     = stdout;
            seekable   = 0;
            file_mode |= LSMASH_FILE_MODE_FRAGMENTED;
        }
    }
    else
        stream = lsmash_fopen( filename, mode );
    if( !stream )
        return -1;
    memset( param, 0, sizeof(lsmash_file_parameters_t) );
    param->mode                = file_mode;
    param->opaque              = (void *)stream;
    param->read                = fread_wrapper;
    param->write               = fwrite_wrapper;
    param->seek                = seekable ? fseek_wrapper : NULL;
    param->major_brand         = 0;
    param->brands              = NULL;
    param->brand_count         = 0;
    param->minor_version       = 0;
    param->max_chunk_duration  = 0.5;
    param->max_async_tolerance = 2.0;
    param->max_chunk_size      = 4 * 1024 * 1024;
    param->max_read_size       = 4 * 1024 * 1024;
    return 0;
}

int lsmash_close_file
(
    lsmash_file_parameters_t *param
)
{
    if( !param )
        return -1;
    if( !param->opaque )
        return 0;
    int ret = fclose( (FILE *)param->opaque );
    param->opaque = NULL;
    return ret;
}

static int isom_set_brands
(
    lsmash_file_t     *file,
    lsmash_brand_type  major_brand,
    uint32_t           minor_version,
    lsmash_brand_type *brands,
    uint32_t           brand_count
)
{
    if( brand_count > 50 )
        return -1;      /* We support setting brands up to 50. */
    if( major_brand == 0 || brand_count == 0 )
    {
        /* Absence of File Type Box means this file is a QuickTime or MP4 version 1 format file. */
        isom_remove_box_by_itself( file->ftyp );
        /* Anyway we use QTFF as a default file format. */
        file->qt_compatible = 1;
        return 0;
    }
    isom_ftyp_t *ftyp;
    if( file->flags & LSMASH_FILE_MODE_INITIALIZATION )
    {
        /* Add File Type Box if absent yet. */
        if( !file->ftyp && isom_add_ftyp( file ) < 0 )
            return -1;
        ftyp = file->ftyp;
    }
    else
    {
        /* Add Segment Type Box if absent yet. */
        ftyp = file->styp_list.head && file->styp_list.head->data
             ? (isom_styp_t *)file->styp_list.head->data
             : isom_add_styp( file );
        if( !ftyp )
            return -1;
    }
    /* Allocate an array of compatible brands.
     * ISO/IEC 14496-12 doesn't forbid the absence of brands in the compatible brand list.
     * For a reason of safety, however, we set at least one brand in the list. */
    size_t alloc_size = (brand_count ? brand_count : 1) * sizeof(uint32_t);
    lsmash_brand_type *compatible_brands;
    if( !file->compatible_brands )
        compatible_brands = lsmash_malloc( alloc_size );
    else
        compatible_brands = lsmash_realloc( file->compatible_brands, alloc_size );
    if( !compatible_brands )
        return -1;
    /* Set compatible brands. */
    if( brand_count )
        for( uint32_t i = 0; i < brand_count; i++ )
            compatible_brands[i] = brands[i];
    else
    {
        /* At least one compatible brand. */
        compatible_brands[0] = major_brand;
        brand_count = 1;
    }
    file->compatible_brands = compatible_brands;
    /* Duplicate an array of compatible brands. */
    lsmash_free( ftyp->compatible_brands );
    ftyp->compatible_brands = lsmash_memdup( compatible_brands, alloc_size );
    if( !ftyp->compatible_brands )
    {
        lsmash_freep( &file->compatible_brands );
        return -1;
    }
    ftyp->size          = ISOM_BASEBOX_COMMON_SIZE + 8 + brand_count * 4;
    ftyp->major_brand   = major_brand;
    ftyp->minor_version = minor_version;
    ftyp->brand_count   = brand_count;
    file->brand_count   = brand_count;
    return isom_check_compatibility( file );
}

lsmash_file_t *lsmash_set_file
(
    lsmash_root_t            *root,
    lsmash_file_parameters_t *param
)
{
    if( !root || !param )
        return NULL;
    if( root->file )
        /* Handling of multiple files is not supported yet. */
        return NULL;
    lsmash_file_t *file = isom_add_file( root );
    if( !file )
        return NULL;
    lsmash_bs_t *bs = lsmash_malloc_zero( sizeof(lsmash_bs_t) );
    if( !bs )
        goto fail;
    file->bs             = bs;
    file->flags          = param->mode;
    file->bs->stream     = param->opaque;
    file->bs->read       = param->read;
    file->bs->write      = param->write;
    file->bs->seek       = param->seek;
    file->bs->unseekable = (param->seek == NULL);
    file->max_chunk_duration  = param->max_chunk_duration;
    file->max_async_tolerance = LSMASH_MAX( param->max_async_tolerance, 2 * param->max_chunk_duration );
    file->max_chunk_size      = param->max_chunk_size;
    file->max_read_size       = param->max_read_size;
    if( (file->flags & LSMASH_FILE_MODE_WRITE)
     && (file->flags & LSMASH_FILE_MODE_BOX) )
    {
        /* Establish the fragment handler if required. */
        if( file->flags & LSMASH_FILE_MODE_FRAGMENTED )
        {
            file->fragment = lsmash_malloc_zero( sizeof(isom_fragment_manager_t) );
            if( !file->fragment )
                goto fail;
            file->fragment->pool = lsmash_create_entry_list();
            if( !file->fragment->pool )
                goto fail;
        }
        else if( file->bs->unseekable )
            /* For unseekable output operations, LSMASH_FILE_MODE_FRAGMENTED shall be set. */
            goto fail;
        /* Establish file types. */
        if( isom_set_brands( file, param->major_brand,
                                   param->minor_version,
                                   param->brands, param->brand_count ) < 0 )
            goto fail;
        /* Create the movie header if the initialization of the streams is required. */
        if( file->flags & LSMASH_FILE_MODE_INITIALIZATION )
        {
            if( isom_add_moov( file )       < 0
             || isom_add_mvhd( file->moov ) < 0 )
                goto fail;
            /* Default Movie Header Box. */
            isom_mvhd_t *mvhd = file->moov->mvhd;
            mvhd->rate          = 0x00010000;
            mvhd->volume        = 0x0100;
            mvhd->matrix[0]     = 0x00010000;
            mvhd->matrix[4]     = 0x00010000;
            mvhd->matrix[8]     = 0x40000000;
            mvhd->next_track_ID = 1;
        }
    }
    root->file = file;
    return file;
fail:
    isom_remove_box_by_itself( file );
    return NULL;
}

int64_t lsmash_read_file
(
    lsmash_file_t            *file,
    lsmash_file_parameters_t *param
)
{
#ifdef LSMASH_DEMUXER_ENABLED
    if( !file || !file->bs )
        return -1LL;
    int64_t ret = -1;
    if( file->flags & (LSMASH_FILE_MODE_READ | LSMASH_FILE_MODE_DUMP) )
    {
        /* Get the file size if seekable when reading. */
        if( !file->bs->unseekable )
        {
            ret = lsmash_bs_seek( file->bs, 0, SEEK_END );
            if( ret < 0 )
                return ret;
            file->bs->written = ret;
            lsmash_bs_seek( file->bs, 0, SEEK_SET );
        }
        else
            ret = 0;
        /* Read whole boxes. */
        if( isom_read_file( file ) < 0 )
            return -1;
        if( param )
        {
            if( file->ftyp )
            {
                /* file types */
                isom_ftyp_t *ftyp = file->ftyp;
                param->major_brand   = ftyp->major_brand ? ftyp->major_brand : ISOM_BRAND_TYPE_QT;
                param->minor_version = ftyp->minor_version;
                param->brands        = file->compatible_brands;
                param->brand_count   = file->brand_count;
            }
            else if( file->styp_list.head && file->styp_list.head->data )
            {
                /* segment types */
                isom_styp_t *styp = (isom_styp_t *)file->styp_list.head->data;
                param->major_brand   = styp->major_brand ? styp->major_brand : ISOM_BRAND_TYPE_QT;
                param->minor_version = styp->minor_version;
                param->brands        = file->compatible_brands;
                param->brand_count   = file->brand_count;
            }
            else
            {
                param->major_brand   = file->mp4_version1 ? ISOM_BRAND_TYPE_MP41 : ISOM_BRAND_TYPE_QT;
                param->minor_version = 0;
                param->brands        = NULL;
                param->brand_count   = 0;
            }
        }
    }
    return ret;
#else
    return -1LL;
#endif
}

lsmash_root_t *lsmash_open_movie( const char *filename, lsmash_file_mode mode )
{
    if( !filename || ((mode & LSMASH_FILE_MODE_WRITE) && (mode & LSMASH_FILE_MODE_READ)) )
        return NULL;
    int open_mode = -1;
    if( mode & LSMASH_FILE_MODE_WRITE )
        open_mode = 0;
#ifdef LSMASH_DEMUXER_ENABLED
    else if( mode & LSMASH_FILE_MODE_READ )
        open_mode = 1;
#endif
    if( open_mode < 0 )
        return NULL;
    lsmash_root_t *root = lsmash_create_root();
    if( !root )
        return NULL;
    lsmash_file_parameters_t param;
    if( lsmash_open_file( filename, open_mode, &param ) < 0 )
    {
        lsmash_destroy_root( root );
        return NULL;
    }
    param.mode |= mode;
    lsmash_file_t *file = lsmash_set_file( root, &param );
    if( !file || (open_mode == 1 && lsmash_read_file( file, &param ) < 0) )
    {
        lsmash_close_file( &param );
        lsmash_destroy_root( root );
        return NULL;
    }
    /* Don't fclose() here. */
    param.opaque = NULL;
    lsmash_close_file( &param );
    /* Call fclose() when calling lsmash_destroy_root(). */
    file->bc_fclose = 1;
    return root;
}

static int isom_finish_fragment_movie( lsmash_file_t *file );

/* A movie fragment cannot switch a sample description to another.
 * So you must call this function before switching sample descriptions. */
int lsmash_create_fragment_movie( lsmash_root_t *root )
{
    if( !root )
        return -1;
    lsmash_file_t *file = root->file;
    if( !file
     || !file->bs
     || !file->fragment
     || !file->moov )
        return -1;
    /* Finish and write the current movie fragment before starting a new one. */
    if( isom_finish_fragment_movie( file ) < 0 )
        return -1;
    /* Add a new movie fragment if the current one is not present or not written. */
    if( !file->fragment->movie || (file->fragment->movie->manager & LSMASH_WRITTEN_BOX) )
    {
        /* We always hold only one movie fragment except for the initial movie (a pair of moov and mdat). */
        if( file->fragment->movie && file->moof_list.entry_count != 1 )
            return -1;
        isom_moof_t *moof = isom_add_moof( file );
        if( isom_add_mfhd( moof ) < 0 )
            return -1;
        file->fragment->movie = moof;
        moof->mfhd->sequence_number = ++ file->fragment->fragment_count;
        if( file->moof_list.entry_count == 1 )
            return 0;
        /* Remove the previous movie fragment. */
        if( file->moof_list.head )
            isom_remove_box_by_itself( file->moof_list.head->data );
    }
    return 0;
}

void lsmash_initialize_movie_parameters( lsmash_movie_parameters_t *param )
{
    memset( param, 0, sizeof(lsmash_movie_parameters_t) );
    param->timescale           = 600;
    param->playback_rate       = 0x00010000;
    param->playback_volume     = 0x0100;
}

int lsmash_set_movie_parameters( lsmash_root_t *root, lsmash_movie_parameters_t *param )
{
    if( !root )
        return -1;
    lsmash_file_t *file = root->file;
    if( !file
     || !file->moov
     || !file->moov->mvhd
     || ((param->major_brand || (param->brands && param->number_of_brands))
      && isom_set_brands( file, param->major_brand, param->minor_version, param->brands, param->number_of_brands ) < 0) )
        return -1;
    isom_mvhd_t *mvhd = file->moov->mvhd;
    if( param->max_chunk_duration > 0 )
        file->max_chunk_duration  = param->max_chunk_duration;
    if( param->max_async_tolerance > 0 )
        file->max_async_tolerance = LSMASH_MAX( param->max_async_tolerance, 2 * param->max_chunk_duration );
    if( param->max_chunk_size )
        file->max_chunk_size = param->max_chunk_size;
    if( param->max_read_size )
        file->max_read_size = param->max_read_size;
    mvhd->timescale           = param->timescale;
    if( file->qt_compatible || file->itunes_movie )
    {
        mvhd->rate            = param->playback_rate;
        mvhd->volume          = param->playback_volume;
        mvhd->previewTime     = param->preview_time;
        mvhd->previewDuration = param->preview_duration;
        mvhd->posterTime      = param->poster_time;
    }
    else
    {
        mvhd->rate            = 0x00010000;
        mvhd->volume          = 0x0100;
        mvhd->previewTime     = 0;
        mvhd->previewDuration = 0;
        mvhd->posterTime      = 0;
    }
    return 0;
}

int lsmash_get_movie_parameters( lsmash_root_t *root, lsmash_movie_parameters_t *param )
{
    if( !root )
        return -1;
    lsmash_file_t *file = root->file;
    if( !file
     || !file->moov
     || !file->moov->mvhd )
        return -1;
    isom_mvhd_t *mvhd = file->moov->mvhd;
    if( file->ftyp )
    {
        isom_ftyp_t *ftyp = file->ftyp;
        uint32_t brand_count = LSMASH_MIN( ftyp->brand_count, 50 );     /* brands up to 50 */
        for( uint32_t i = 0; i < brand_count; i++ )
            param->brands_shadow[i] = ftyp->compatible_brands[i];
        param->major_brand      = ftyp->major_brand;
        param->brands           = param->brands_shadow;
        param->number_of_brands = brand_count;
        param->minor_version    = ftyp->minor_version;
    }
    param->max_chunk_duration  = file->max_chunk_duration;
    param->max_async_tolerance = file->max_async_tolerance;
    param->max_chunk_size      = file->max_chunk_size;
    param->max_read_size       = file->max_read_size;
    param->timescale           = mvhd->timescale;
    param->duration            = mvhd->duration;
    param->playback_rate       = mvhd->rate;
    param->playback_volume     = mvhd->volume;
    param->preview_time        = mvhd->previewTime;
    param->preview_duration    = mvhd->previewDuration;
    param->poster_time         = mvhd->posterTime;
    param->number_of_tracks    = file->moov->trak_list.entry_count;
    return 0;
}

uint32_t lsmash_get_movie_timescale( lsmash_root_t *root )
{
    if( !root
     || !root->file
     || !root->file->moov
     || !root->file->moov->mvhd )
        return 0;
    return root->file->moov->mvhd->timescale;
}

static int isom_scan_trak_profileLevelIndication( isom_trak_t *trak, mp4a_audioProfileLevelIndication *audio_pli, mp4sys_visualProfileLevelIndication *visual_pli )
{
    if( !trak
     || !trak->mdia
     || !trak->mdia->minf
     || !trak->mdia->minf->stbl )
        return -1;
    isom_stsd_t *stsd = trak->mdia->minf->stbl->stsd;
    if( !stsd
     || !stsd->list.head )
        return -1;
    for( lsmash_entry_t *entry = stsd->list.head; entry; entry = entry->next )
    {
        isom_sample_entry_t *sample_entry = (isom_sample_entry_t *)entry->data;
        if( !sample_entry )
            return -1;
        lsmash_codec_type_t sample_type = (lsmash_codec_type_t)sample_entry->type;
        if( trak->mdia->minf->vmhd )
        {
            if( lsmash_check_codec_type_identical( sample_type, ISOM_CODEC_TYPE_AVC1_VIDEO )
             || lsmash_check_codec_type_identical( sample_type, ISOM_CODEC_TYPE_AVC2_VIDEO )
             || lsmash_check_codec_type_identical( sample_type, ISOM_CODEC_TYPE_AVC3_VIDEO )
             || lsmash_check_codec_type_identical( sample_type, ISOM_CODEC_TYPE_AVC4_VIDEO )
             || lsmash_check_codec_type_identical( sample_type, ISOM_CODEC_TYPE_AVCP_VIDEO )
             || lsmash_check_codec_type_identical( sample_type, ISOM_CODEC_TYPE_SVC1_VIDEO )
             || lsmash_check_codec_type_identical( sample_type, ISOM_CODEC_TYPE_MVC1_VIDEO )
             || lsmash_check_codec_type_identical( sample_type, ISOM_CODEC_TYPE_MVC2_VIDEO ) )
            {
                /* FIXME: Do we have to arbitrate like audio? */
                if( *visual_pli == MP4SYS_VISUAL_PLI_NONE_REQUIRED )
                    *visual_pli = lsmash_check_codec_type_identical( sample_type, ISOM_CODEC_TYPE_AVCP_VIDEO )
                                ? MP4SYS_OBJECT_TYPE_Parameter_Sets_H_264_ISO_14496_10
                                : MP4SYS_VISUAL_PLI_H264_AVC;
            }
            else
                *visual_pli = MP4SYS_VISUAL_PLI_NOT_SPECIFIED;
        }
        else if( trak->mdia->minf->smhd )
        {
            if( lsmash_check_codec_type_identical( sample_type, ISOM_CODEC_TYPE_MP4A_AUDIO ) )
            {
                isom_audio_entry_t *audio = (isom_audio_entry_t *)sample_entry;
#ifdef LSMASH_DEMUXER_ENABLED
                isom_esds_t *esds = (isom_esds_t *)isom_get_extension_box_format( &audio->extensions, ISOM_BOX_TYPE_ESDS );
                if( !esds || !esds->ES )
                    return -1;
                if( !lsmash_check_codec_type_identical( audio->summary.sample_type, ISOM_CODEC_TYPE_MP4A_AUDIO ) )
                    /* This is needed when copying descriptions. */
                    mp4sys_setup_summary_from_DecoderSpecificInfo( &audio->summary, esds->ES );
#endif
                *audio_pli = mp4a_max_audioProfileLevelIndication( *audio_pli, mp4a_get_audioProfileLevelIndication( &audio->summary ) );
            }
            else
                /* NOTE: Audio CODECs other than 'mp4a' does not have appropriate pli. */
                *audio_pli = MP4A_AUDIO_PLI_NOT_SPECIFIED;
        }
        else
            ;   /* FIXME: Do we have to set OD_profileLevelIndication? */
    }
    return 0;
}

static int isom_setup_iods( isom_moov_t *moov )
{
    if( !moov->iods && isom_add_iods( moov ) )
        return -1;
    isom_iods_t *iods = moov->iods;
    iods->OD = mp4sys_create_ObjectDescriptor( 1 ); /* NOTE: Use 1 for ObjectDescriptorID of IOD. */
    if( !iods->OD )
        goto fail;
    mp4a_audioProfileLevelIndication     audio_pli = MP4A_AUDIO_PLI_NONE_REQUIRED;
    mp4sys_visualProfileLevelIndication visual_pli = MP4SYS_VISUAL_PLI_NONE_REQUIRED;
    for( lsmash_entry_t *entry = moov->trak_list.head; entry; entry = entry->next )
    {
        isom_trak_t *trak = (isom_trak_t *)entry->data;
        if( !trak
         || !trak->tkhd )
            goto fail;
        if( isom_scan_trak_profileLevelIndication( trak, &audio_pli, &visual_pli ) )
            goto fail;
        if( mp4sys_add_ES_ID_Inc( iods->OD, trak->tkhd->track_ID ) )
            goto fail;
    }
    if( mp4sys_to_InitialObjectDescriptor( iods->OD,
                                           0, /* FIXME: I'm not quite sure what the spec says. */
                                           MP4SYS_OD_PLI_NONE_REQUIRED, MP4SYS_SCENE_PLI_NONE_REQUIRED,
                                           audio_pli, visual_pli,
                                           MP4SYS_GRAPHICS_PLI_NONE_REQUIRED ) )
        goto fail;
    return 0;
fail:
    isom_remove_box_by_itself( iods );
    return -1;
}

int lsmash_create_object_descriptor( lsmash_root_t *root )
{
    if( !root || !root->file )
        return -1;
    lsmash_file_t *file = root->file;
    /* Return error if this file is not compatible with MP4 file format. */
    if( !file->mp4_version1 && !file->mp4_version2 )
        return -1;
    return isom_setup_iods( file->moov );
}

/*---- finishing functions ----*/

static int isom_set_fragment_overall_duration( lsmash_file_t *file )
{
    /* Get the longest duration of the tracks. */
    uint64_t longest_duration = 0;
    for( lsmash_entry_t *entry = file->moov->trak_list.head; entry; entry = entry->next )
    {
        isom_trak_t *trak = (isom_trak_t *)entry->data;
        if( !trak
         || !trak->cache
         || !trak->cache->fragment
         || !trak->mdia
         || !trak->mdia->mdhd
         || !trak->mdia->mdhd->timescale )
            return -1;
        uint64_t duration;
        if( !trak->edts
         || !trak->edts->elst
         || !trak->edts->elst->list )
        {
            duration = trak->cache->fragment->largest_cts + trak->cache->fragment->last_duration;
            duration = (uint64_t)(((double)duration / trak->mdia->mdhd->timescale) * file->moov->mvhd->timescale);
        }
        else
        {
            duration = 0;
            for( lsmash_entry_t *elst_entry = trak->edts->elst->list->head; elst_entry; elst_entry = elst_entry->next )
            {
                isom_elst_entry_t *data = (isom_elst_entry_t *)elst_entry->data;
                if( !data )
                    return -1;
                if( data->segment_duration == ISOM_EDIT_DURATION_IMPLICIT )
                {
                    uint64_t segment_duration = trak->cache->fragment->largest_cts + trak->cache->fragment->last_duration;
                    duration += (uint64_t)(((double)segment_duration / trak->mdia->mdhd->timescale) * file->moov->mvhd->timescale);
                }
                else
                    duration += data->segment_duration;
            }
        }
        longest_duration = LSMASH_MAX( duration, longest_duration );
    }
    isom_mehd_t *mehd = file->moov->mvex->mehd;
    mehd->fragment_duration = longest_duration;
    mehd->version           = 1;
    mehd->manager          &= ~LSMASH_PLACEHOLDER;
    isom_update_box_size( mehd );
    /* Write Movie Extends Header Box here. */
    lsmash_bs_t *bs = file->bs;
    uint64_t current_pos = bs->offset;
    lsmash_bs_seek( bs, mehd->pos, SEEK_SET );
    int ret = isom_write_box( bs, (isom_box_t *)mehd );
    lsmash_bs_seek( bs, current_pos, SEEK_SET );
    return ret;
}

static int isom_write_fragment_random_access_info( lsmash_file_t *file )
{
    if( !file->moov->mvex )
        return 0;
    /* Reconstruct the Movie Fragment Random Access Box.
     * All 'time' field in the Track Fragment Random Access Boxes shall reflect edit list. */
    uint32_t movie_timescale = lsmash_get_movie_timescale( file->root );
    if( movie_timescale == 0 )
        return -1;  /* Division by zero will occur. */
    for( lsmash_entry_t *trex_entry = file->moov->mvex->trex_list.head; trex_entry; trex_entry = trex_entry->next )
    {
        isom_trex_t *trex = (isom_trex_t *)trex_entry->data;
        if( !trex )
            return -1;
        /* Get the edit list of the track associated with the trex->track_ID.
         * If failed or absent, implicit timeline mapping edit is used, and skip this operation for the track. */
        isom_trak_t *trak = isom_get_trak( file, trex->track_ID );
        if( !trak )
            return -1;
        if( !trak->edts
         || !trak->edts->elst
         || !trak->edts->elst->list
         || !trak->edts->elst->list->head
         || !trak->edts->elst->list->head->data )
            continue;
        isom_elst_t *elst = trak->edts->elst;
        /* Get the Track Fragment Random Access Boxes of the track associated with the trex->track_ID.
         * If failed or absent, skip reconstructing the Track Fragment Random Access Box of the track. */
        isom_tfra_t *tfra = isom_get_tfra( file->mfra, trex->track_ID );
        if( !tfra )
            continue;
        /* Reconstruct the Track Fragment Random Access Box. */
        lsmash_entry_t    *edit_entry      = elst->list->head;
        isom_elst_entry_t *edit            = edit_entry->data;
        uint64_t           edit_offset     = 0;     /* units in media timescale */
        uint32_t           media_timescale = lsmash_get_media_timescale( file->root, trex->track_ID );
        for( lsmash_entry_t *rap_entry = tfra->list->head; rap_entry; )
        {
            isom_tfra_location_time_entry_t *rap = (isom_tfra_location_time_entry_t *)rap_entry->data;
            if( !rap )
            {
                /* Irregular case. Drop this entry. */
                lsmash_entry_t *next = rap_entry->next;
                lsmash_remove_entry_direct( tfra->list, rap_entry, NULL );
                rap_entry = next;
                continue;
            }
            uint64_t composition_time = rap->time;
            /* Skip edits that doesn't need the current sync sample indicated in the Track Fragment Random Access Box. */
            while( edit )
            {
                uint64_t segment_duration = ((edit->segment_duration - 1) / movie_timescale + 1) * media_timescale;
                if( edit->media_time != ISOM_EDIT_MODE_EMPTY
                 && composition_time < edit->media_time + segment_duration )
                    break;  /* This Timeline Mapping Edit might require the current sync sample.
                             * Note: this condition doesn't cover all cases.
                             *       For instance, matching the both following conditions
                             *         1. A sync sample isn't in the presentation.
                             *         2. The other samples, which precede it in the composition timeline, is in the presentation. */
                edit_offset += segment_duration;
                edit_entry   = edit_entry->next;
                if( !edit_entry )
                {
                    /* No more presentation. */
                    edit = NULL;
                    break;
                }
                edit = edit_entry->data;
            }
            if( !edit )
            {
                /* No more presentation.
                 * Drop the rest of sync samples since they are generally absent in the whole presentation.
                 * Though the exceptions are sync samples with earlier composition time, we ignore them. (SAP type 2: TEPT = TDEC = TSAP < TPTF)
                 * To support this exception, we need sorting entries of the list by composition times. */
                while( rap_entry )
                {
                    lsmash_entry_t *next = rap_entry->next;
                    lsmash_remove_entry_direct( tfra->list, rap_entry, NULL );
                    rap_entry = next;
                }
                break;
            }
            /* If the sync sample isn't in the presentation,
             * we pick the earliest presentation time of the current edit as its presentation time. */
            rap->time = edit_offset;
            if( composition_time >= edit->media_time )
                rap->time += composition_time - edit->media_time;
            rap_entry = rap_entry->next;
        }
    }
    /* Decide the size of the Movie Fragment Random Access Box. */
    if( isom_update_box_size( file->mfra ) == 0 )
        return -1;
    /* Write the Movie Fragment Random Access Box. */
    return isom_write_box( file->bs, (isom_box_t *)file->mfra );
}

static int isom_complement_data_reference( isom_minf_t *minf )
{
    if( !minf->dinf
     || !minf->dinf->dref )
        return -1;
    /* Complement data referece if absent. */
    if( !minf->dinf->dref->list.head )
    {
        isom_dref_entry_t *url = isom_add_dref_entry( minf->dinf->dref );
        if( !url )
            return -1;
        url->flags = 0x000001;  /* Media data is in the same file. */
    }
    return 0;
}

static int isom_update_indexed_material_offset
(
    lsmash_file_t *file,
    isom_sidx_t   *last_sidx
)
{
    /* Update the size of each Segment Index Box. */
    for( lsmash_entry_t *entry = file->sidx_list.head; entry; entry = entry->next )
    {
        isom_sidx_t *sidx = (isom_sidx_t *)entry->data;
        if( !sidx )
            continue;
        if( isom_update_box_size( sidx ) == 0 )
            return -1;
    }
    /* first_offset: the sum of the size of subsequent Segment Index Boxes
     * Be careful about changing the size of them. */
    last_sidx->first_offset = 0;
    for( lsmash_entry_t *a_entry = file->sidx_list.head; a_entry && a_entry->data != last_sidx; a_entry = a_entry->next )
    {
        isom_sidx_t *a = (isom_sidx_t *)a_entry->data;
        a->first_offset = 0;
        for( lsmash_entry_t *b_entry = a_entry->next; b_entry; b_entry = b_entry->next )
        {
            isom_sidx_t *b = (isom_sidx_t *)b_entry->data;
            a->first_offset += b->size;
        }
    }
    return 0;
}

static int isom_rearrange_boxes
(
    lsmash_file_t        *file,
    lsmash_adhoc_remux_t *remux,
    uint8_t              *buf[2],
    size_t                read_num,
    size_t                size,
    uint64_t              read_pos,
    uint64_t              write_pos,
    uint64_t              file_size
)
{
    /* Copy-pastan */
    int buf_switch = 1;
    lsmash_bs_t *bs = file->bs;
    while( read_num == size )
    {
        if( lsmash_bs_seek( bs, read_pos, SEEK_SET ) < 0 )
            return -1;
        lsmash_bs_read_data( bs, buf[buf_switch], &read_num );
        read_pos    = bs->offset;
        buf_switch ^= 0x1;
        if( lsmash_bs_seek( bs, write_pos, SEEK_SET ) < 0 )
            return -1;
        if( lsmash_bs_write_data( bs, buf[buf_switch], size ) < 0 )
            return -1;
        write_pos = bs->offset;
        if( remux && remux->func )
            remux->func( remux->param, write_pos, file_size ); // FIXME:
    }
    if( lsmash_bs_write_data( bs, buf[buf_switch ^ 0x1], read_num ) < 0 )
        return -1;
    if( remux && remux->func )
        remux->func( remux->param, file_size, file_size ); // FIXME:
    return 0;
}

int lsmash_finish_movie( lsmash_root_t *root, lsmash_adhoc_remux_t *remux )
{
    if( !root )
        return -1;
    lsmash_file_t *file = root->file;
    if( !file
     || !file->bs
     || !file->moov )
        return -1;
    uint8_t *buf[2] = { NULL, NULL };
    if( file->fragment )
    {
        /* Output the final movie fragment. */
        if( isom_finish_fragment_movie( file ) < 0 )
            return -1;
        if( file->bs->unseekable )
            return 0;
        /* Write Segment Index Boxes.
         * This occurs only when the initial movie has no samples.
         * We don't consider updating of chunk offsets within initial movie sample table here.
         * This is reasonable since DASH requires no samples in the initial movie.
         * This implementation is not suitable for live-streaming.
         + To support live-streaming, it is good to use daisy-chained index.  */
        if( file->flags & LSMASH_FILE_MODE_INDEX )
        {
            /* Update the size of each Segment Index Box and establish the offset from the anchor point to the indexed material. */
            if( isom_update_indexed_material_offset( file, (isom_sidx_t *)file->sidx_list.tail->data ) < 0 )
                return -1;
            /* Get the total size of all Segment Index Boxes. */
            uint64_t total_sidx_size = 0;
            for( lsmash_entry_t *entry = file->sidx_list.head; entry; entry = entry->next )
            {
                isom_sidx_t *sidx = (isom_sidx_t *)entry->data;
                if( !sidx )
                    continue;
                total_sidx_size += sidx->size;
            }
            /* buffer size must be at least total_sidx_size * 2 */
            size_t buffer_size = total_sidx_size * 2;
            if( remux && remux->buffer_size > buffer_size )
                buffer_size = remux->buffer_size;
            /* Split to 2 buffers. */
            if( (buf[0] = (uint8_t *)lsmash_malloc( buffer_size )) == NULL )
                return -1;
            size_t size = buffer_size / 2;
            buf[1] = buf[0] + size;
            /* Seek to the beginning of the first Movie Fragment Box. */
            lsmash_bs_t *bs = file->bs;
            if( lsmash_bs_seek( bs, file->fragment->first_moof_pos, SEEK_SET ) < 0 )
                goto fail;
            size_t read_num = size;
            lsmash_bs_read_data( bs, buf[0], &read_num );
            uint64_t read_pos = bs->offset;
            /* */
            if( lsmash_bs_seek( bs, file->fragment->first_moof_pos, SEEK_SET ) < 0 )
                goto fail;
            for( lsmash_entry_t *entry = file->sidx_list.head; entry; entry = entry->next )
            {
                isom_sidx_t *sidx = (isom_sidx_t *)entry->data;
                if( !sidx )
                    continue;
                if( isom_write_box( file->bs, (isom_box_t *)sidx ) < 0 )
                    return -1;
            }
            uint64_t write_pos = bs->offset;
            uint64_t total     = file->size + total_sidx_size;
            if( isom_rearrange_boxes( file, remux, buf, read_num, size, read_pos, write_pos, total ) < 0 )
                goto fail;
            file->size += total_sidx_size;
            lsmash_freep( &buf[0] );
            /* Update moof_offset. */
            if( file->mfra )
                for( lsmash_entry_t *entry = file->mfra->tfra_list.head; entry; entry = entry->next )
                {
                    isom_tfra_t *tfra = (isom_tfra_t *)entry->data;
                    if( !tfra )
                        continue;
                    for( lsmash_entry_t *rap_entry = tfra->list->head; rap_entry; rap_entry = rap_entry->next )
                    {
                        isom_tfra_location_time_entry_t *rap = (isom_tfra_location_time_entry_t *)rap_entry->data;
                        if( !rap )
                            continue;
                        rap->moof_offset += total_sidx_size;
                    }
                }
        }
        /* Write the overall random access information at the tail of the movie. */
        if( isom_write_fragment_random_access_info( file ) )
            return -1;
        /* Set overall duration of the movie. */
        return isom_set_fragment_overall_duration( file );
    }
    isom_moov_t *moov = file->moov;
    for( lsmash_entry_t *entry = moov->trak_list.head; entry; entry = entry->next )
    {
        isom_trak_t *trak = (isom_trak_t *)entry->data;
        if( !trak
         || !trak->cache
         || !trak->tkhd
         || !trak->mdia
         || !trak->mdia->minf
         || !trak->mdia->minf->stbl
         || isom_complement_data_reference( trak->mdia->minf ) < 0 )
            return -1;
        uint32_t track_ID         = trak->tkhd->track_ID;
        uint32_t related_track_ID = trak->related_track_ID;
        /* Disable the track if the track is a track reference chapter. */
        if( trak->is_chapter )
            trak->tkhd->flags &= ~ISOM_TRACK_ENABLED;
        if( trak->is_chapter && related_track_ID )
        {
            /* In order that the track duration of the chapter track doesn't exceed that of the related track. */
            lsmash_edit_t edit;
            edit.duration   = LSMASH_MIN( trak->tkhd->duration, lsmash_get_track_duration( root, related_track_ID ) );
            edit.start_time = 0;
            edit.rate       = ISOM_EDIT_MODE_NORMAL;
            if( lsmash_create_explicit_timeline_map( root, track_ID, edit ) )
                return -1;
        }
        /* Add stss box if any samples aren't sync sample. */
        isom_stbl_t *stbl = trak->mdia->minf->stbl;
        if( !trak->cache->all_sync && !stbl->stss && isom_add_stss( stbl ) )
            return -1;
        if( isom_update_tkhd_duration( trak ) )
            return -1;
        if( isom_update_bitrate_description( trak->mdia ) )
            return -1;
    }
    if( file->mp4_version1 == 1 && isom_setup_iods( moov ) )
        return -1;
    if( isom_check_mandatory_boxes( file )
     || isom_set_movie_creation_time( file )
     || isom_update_box_size( moov ) == 0 )
        return -1;
    /* Write the size of Media Data Box here. */
    lsmash_bs_t *bs = file->bs;
    file->mdat->manager &= ~LSMASH_INCOMPLETE_BOX;
    if( isom_write_box( bs, (isom_box_t *)file->mdat ) < 0 )
        return -1;
    /* Write the Movie Box and a Meta Box if no optimization for progressive download. */
    uint64_t meta_size = file->meta ? file->meta->size : 0;
    if( !remux )
    {
        if( isom_write_box( bs, (isom_box_t *)file->moov )
         || isom_write_box( bs, (isom_box_t *)file->meta ) )
            return -1;
        file->size += moov->size + meta_size;
        return 0;
    }
    /* stco->co64 conversion, depending on last chunk's offset */
    for( lsmash_entry_t *entry = moov->trak_list.head; entry; )
    {
        isom_trak_t *trak = (isom_trak_t *)entry->data;
        isom_stco_t *stco = trak->mdia->minf->stbl->stco;
        if( !stco->list->tail )
            return -1;
        if( stco->large_presentation
         || (((isom_stco_entry_t *)stco->list->tail->data)->chunk_offset + moov->size + meta_size) <= UINT32_MAX )
        {
            entry = entry->next;
            continue;   /* no need to convert stco into co64 */
        }
        /* stco->co64 conversion */
        if( isom_convert_stco_to_co64( trak->mdia->minf->stbl )
         || isom_update_box_size( moov ) == 0 )
            return -1;
        entry = moov->trak_list.head;   /* whenever any conversion, re-check all traks */
    }
    /* now the amount of offset is fixed. */
    uint64_t mtf_size = moov->size + meta_size;     /* sum of size of boxes moved to front */
    /* buffer size must be at least mtf_size * 2 */
    remux->buffer_size = LSMASH_MAX( remux->buffer_size, mtf_size * 2 );
    /* Split to 2 buffers. */
    if( (buf[0] = (uint8_t*)lsmash_malloc( remux->buffer_size )) == NULL )
        return -1; /* NOTE: i think we still can fallback to "return isom_write_moov();" here. */
    size_t size = remux->buffer_size / 2;
    buf[1] = buf[0] + size;
    /* now the amount of offset is fixed. apply that to stco/co64 */
    for( lsmash_entry_t *entry = moov->trak_list.head; entry; entry = entry->next )
    {
        isom_stco_t *stco = ((isom_trak_t *)entry->data)->mdia->minf->stbl->stco;
        if( stco->large_presentation )
            for( lsmash_entry_t *co64_entry = stco->list->head ; co64_entry ; co64_entry = co64_entry->next )
                ((isom_co64_entry_t *)co64_entry->data)->chunk_offset += mtf_size;
        else
            for( lsmash_entry_t *stco_entry = stco->list->head ; stco_entry ; stco_entry = stco_entry->next )
                ((isom_stco_entry_t *)stco_entry->data)->chunk_offset += mtf_size;
    }
    /* Backup starting area of mdat and write moov + meta there instead. */
    isom_mdat_t *mdat            = file->mdat;
    uint64_t     total           = file->size + mtf_size;
    uint64_t     placeholder_pos = file->free ? file->free->pos : mdat->pos;
    if( lsmash_bs_seek( bs, placeholder_pos, SEEK_SET ) < 0 )
        goto fail;
    size_t read_num = size;
    lsmash_bs_read_data( bs, buf[0], &read_num );
    uint64_t read_pos = bs->offset;
    /* Write moov + meta there instead. */
    if( lsmash_bs_seek( bs, placeholder_pos, SEEK_SET ) < 0
     || isom_write_box( bs, (isom_box_t *)file->moov )  < 0
     || isom_write_box( bs, (isom_box_t *)file->meta )  < 0 )
        goto fail;
    uint64_t write_pos = bs->offset;
    /* Update the positions */
    mdat->pos += mtf_size;
    if( file->free )
        file->free->pos += mtf_size;
    /* Move Media Data Box. */
    if( isom_rearrange_boxes( file, remux, buf, read_num, size, read_pos, write_pos, total ) < 0 )
        goto fail;
    file->size += mtf_size;
    lsmash_free( buf[0] );
    return 0;
fail:
    lsmash_free( buf[0] );
    return -1;
}

#define GET_MOST_USED( box_name, index, flag_name ) \
    if( most_used[index] < stats.flag_name[i] ) \
    { \
        most_used[index] = stats.flag_name[i]; \
        box_name->default_sample_flags.flag_name = i; \
    }

static int isom_create_fragment_overall_default_settings( lsmash_file_t *file )
{
    if( isom_add_mvex( file->moov ) )
        return -1;
    if( !file->bs->unseekable )
    {
        if( isom_add_mehd( file->moov->mvex ) )
            return -1;
        file->moov->mvex->mehd->manager |= LSMASH_PLACEHOLDER;
    }
    for( lsmash_entry_t *trak_entry = file->moov->trak_list.head; trak_entry; trak_entry = trak_entry->next )
    {
        isom_trak_t *trak = (isom_trak_t *)trak_entry->data;
        if( !trak
         || !trak->cache
         || !trak->tkhd
         || !trak->mdia
         || !trak->mdia->minf
         || !trak->mdia->minf->stbl )
            return -1;
        isom_stbl_t *stbl = trak->mdia->minf->stbl;
        if( !stbl->stts || !stbl->stts->list
         || !stbl->stsz
         || (stbl->stts->list->tail && !stbl->stts->list->tail->data)
         || (stbl->stsz->list && stbl->stsz->list->head && !stbl->stsz->list->head->data) )
            return -1;
        isom_trex_t *trex = isom_add_trex( file->moov->mvex );
        if( !trex )
            return -1;
        trex->track_ID = trak->tkhd->track_ID;
        /* Set up defaults. */
        trex->default_sample_description_index = trak->cache->chunk.sample_description_index
                                               ? trak->cache->chunk.sample_description_index
                                               : 1;
        trex->default_sample_duration          = stbl->stts->list->tail
                                               ? ((isom_stts_entry_t *)stbl->stts->list->tail->data)->sample_delta
                                               : 1;
        trex->default_sample_size              = !stbl->stsz->list
                                               ? stbl->stsz->sample_size : stbl->stsz->list->head
                                               ? ((isom_stsz_entry_t *)stbl->stsz->list->head->data)->entry_size : 0;
        if( stbl->sdtp
         && stbl->sdtp->list )
        {
            struct sample_flags_stats_t
            {
                uint32_t is_leading           [4];
                uint32_t sample_depends_on    [4];
                uint32_t sample_is_depended_on[4];
                uint32_t sample_has_redundancy[4];
            } stats = { { 0 }, { 0 }, { 0 }, { 0 } };
            for( lsmash_entry_t *sdtp_entry = stbl->sdtp->list->head; sdtp_entry; sdtp_entry = sdtp_entry->next )
            {
                isom_sdtp_entry_t *data = (isom_sdtp_entry_t *)sdtp_entry->data;
                if( !data )
                    return -1;
                ++ stats.is_leading           [ data->is_leading            ];
                ++ stats.sample_depends_on    [ data->sample_depends_on     ];
                ++ stats.sample_is_depended_on[ data->sample_is_depended_on ];
                ++ stats.sample_has_redundancy[ data->sample_has_redundancy ];
            }
            uint32_t most_used[4] = { 0, 0, 0, 0 };
            for( int i = 0; i < 4; i++ )
            {
                GET_MOST_USED( trex, 0, is_leading            );
                GET_MOST_USED( trex, 1, sample_depends_on     );
                GET_MOST_USED( trex, 2, sample_is_depended_on );
                GET_MOST_USED( trex, 3, sample_has_redundancy );
            }
        }
        trex->default_sample_flags.sample_is_non_sync_sample = !trak->cache->all_sync;
    }
    return 0;
}

static int isom_prepare_random_access_info( lsmash_file_t *file )
{
    if( file->bs->unseekable )
        return 0;
    if( isom_add_mfra( file )
     || isom_add_mfro( file->mfra ) )
        return -1;
    return 0;
}

static int isom_output_fragment_media_data( lsmash_file_t *file )
{
    isom_fragment_manager_t *fragment = file->fragment;
    /* If there is no available Media Data Box to write samples, add and write a new one. */
    if( fragment->sample_count )
    {
        if( !file->mdat && isom_add_mdat( file ) < 0 )
            return -1;
        file->mdat->manager &= ~(LSMASH_INCOMPLETE_BOX | LSMASH_WRITTEN_BOX);
        if( isom_write_box( file->bs, (isom_box_t *)file->mdat ) < 0 )
            return -1;
        file->size += file->mdat->size;
        file->mdat->size       = 0;
        file->mdat->media_size = 0;
    }
    lsmash_remove_entries( fragment->pool, isom_remove_sample_pool );
    fragment->pool_size    = 0;
    fragment->sample_count = 0;
    return 0;
}

static int isom_rap_grouping_established( isom_rap_group_t *group, int num_leading_samples_known, isom_sgpd_t *sgpd, int is_fragment );
static int isom_all_recovery_completed( isom_sbgp_t *sbgp, lsmash_entry_list_t *pool );

static int isom_finish_fragment_initial_movie( lsmash_file_t *file )
{
    if( !file->moov )
        return -1;
    isom_moov_t *moov = file->moov;
    for( lsmash_entry_t *entry = moov->trak_list.head; entry; entry = entry->next )
    {
        isom_trak_t *trak = (isom_trak_t *)entry->data;
        if( !trak
         || !trak->cache
         || !trak->tkhd
         || !trak->mdia
         || !trak->mdia->mdhd
         || !trak->mdia->minf
         || !trak->mdia->minf->stbl
         || isom_complement_data_reference( trak->mdia->minf ) < 0 )
            return -1;
        isom_stbl_t *stbl = trak->mdia->minf->stbl;
        if( isom_get_sample_count( trak ) )
        {
            /* Add stss box if any samples aren't sync sample. */
            if( !trak->cache->all_sync && !stbl->stss && isom_add_stss( stbl ) )
                return -1;
            if( isom_update_tkhd_duration( trak ) )
                return -1;
        }
        else
            trak->tkhd->duration = 0;
        if( isom_update_bitrate_description( trak->mdia ) )
            return -1;
        /* Complete the last sample groups within tracks in the initial movie. */
        if( trak->cache->rap )
        {
            isom_sgpd_t *sgpd = isom_get_sample_group_description( stbl, ISOM_GROUP_TYPE_RAP );
            if( !sgpd || isom_rap_grouping_established( trak->cache->rap, 1, sgpd, 0 ) < 0 )
                return -1;
            lsmash_freep( &trak->cache->rap );
        }
        if( trak->cache->roll.pool )
        {
            isom_sbgp_t *sbgp = isom_get_sample_to_group( stbl, ISOM_GROUP_TYPE_ROLL );
            if( !sbgp || isom_all_recovery_completed( sbgp, trak->cache->roll.pool ) < 0 )
                return -1;
        }
    }
    if( file->mp4_version1 == 1 && isom_setup_iods( moov ) )
        return -1;
    if( isom_create_fragment_overall_default_settings( file )
     || isom_prepare_random_access_info( file )
     || isom_check_mandatory_boxes( file->file )
     || isom_set_movie_creation_time( file->file )
     || isom_update_box_size( moov ) == 0 )
        return -1;
    /* stco->co64 conversion, depending on last chunk's offset */
    uint64_t meta_size = file->meta ? file->meta->size : 0;
    for( lsmash_entry_t *entry = moov->trak_list.head; entry; )
    {
        isom_trak_t *trak = (isom_trak_t *)entry->data;
        isom_stco_t *stco = trak->mdia->minf->stbl->stco;
        if( !stco->list->tail   /* no samples */
         || stco->large_presentation
         || (((isom_stco_entry_t *)stco->list->tail->data)->chunk_offset + moov->size + meta_size) <= UINT32_MAX )
        {
            entry = entry->next;
            continue;   /* no need to convert stco into co64 */
        }
        /* stco->co64 conversion */
        if( isom_convert_stco_to_co64( trak->mdia->minf->stbl )
         || isom_update_box_size( moov ) == 0 )
            return -1;
        entry = moov->trak_list.head;   /* whenever any conversion, re-check all traks */
    }
    /* Now, the amount of offset is fixed. Apply that to stco/co64. */
    uint64_t preceding_size = moov->size + meta_size;
    for( lsmash_entry_t *entry = moov->trak_list.head; entry; entry = entry->next )
    {
        isom_stco_t *stco = ((isom_trak_t *)entry->data)->mdia->minf->stbl->stco;
        if( stco->large_presentation )
            for( lsmash_entry_t *co64_entry = stco->list->head ; co64_entry ; co64_entry = co64_entry->next )
                ((isom_co64_entry_t *)co64_entry->data)->chunk_offset += preceding_size;
        else
            for( lsmash_entry_t *stco_entry = stco->list->head ; stco_entry ; stco_entry = stco_entry->next )
                ((isom_stco_entry_t *)stco_entry->data)->chunk_offset += preceding_size;
    }
    /* Write File Type Box here if it was not written yet. */
    if( file->ftyp && !(file->ftyp->manager & LSMASH_WRITTEN_BOX) )
    {
        if( isom_write_box( file->bs, (isom_box_t *)file->ftyp ) )
            return -1;
        file->size += file->ftyp->size;
    }
    /* Write Movie Box. */
    if( isom_write_box( file->bs, (isom_box_t *)file->moov ) < 0
     || isom_write_box( file->bs, (isom_box_t *)file->meta ) < 0 )
        return -1;
    file->size += preceding_size;
    /* Output samples. */
    if( isom_output_fragment_media_data( file ) < 0 )
        return -1;
    /* Revert the number of samples in tracks to 0. */
    for( lsmash_entry_t *entry = moov->trak_list.head; entry; entry = entry->next )
    {
        isom_trak_t *trak = (isom_trak_t *)entry->data;
        if( trak->cache->fragment )
            trak->cache->fragment->sample_count = 0;
    }
    return 0;
}

/* Return 1 if there is diffrence, otherwise return 0. */
static int isom_compare_sample_flags( isom_sample_flags_t *a, isom_sample_flags_t *b )
{
    return (a->reserved                    != b->reserved)
        || (a->is_leading                  != b->is_leading)
        || (a->sample_depends_on           != b->sample_depends_on)
        || (a->sample_is_depended_on       != b->sample_is_depended_on)
        || (a->sample_has_redundancy       != b->sample_has_redundancy)
        || (a->sample_padding_value        != b->sample_padding_value)
        || (a->sample_is_non_sync_sample   != b->sample_is_non_sync_sample)
        || (a->sample_degradation_priority != b->sample_degradation_priority);
}

static int isom_finish_fragment_movie( lsmash_file_t *file )
{
    if( !file->moov
     || !file->fragment
     || !file->fragment->pool )
        return -1;
    isom_moof_t *moof = file->fragment->movie;
    if( !moof )
        return isom_finish_fragment_initial_movie( file );
    /* Don't write the current movie fragment if containing no track fragments.
     * This is a requirement of DASH Media Segment. */
    if( !moof->traf_list.head
     || !moof->traf_list.head->data )
        return 0;
    /* Calculate appropriate default_sample_flags of each Track Fragment Header Box.
     * And check whether that default_sample_flags is useful or not. */
    for( lsmash_entry_t *entry = moof->traf_list.head; entry; entry = entry->next )
    {
        isom_traf_t *traf = (isom_traf_t *)entry->data;
        if( !traf
         || !traf->tfhd
         || !traf->file
         || !traf->file->moov
         || !traf->file->moov->mvex )
            return -1;
        isom_tfhd_t *tfhd = traf->tfhd;
        isom_trex_t *trex = isom_get_trex( file->moov->mvex, tfhd->track_ID );
        if( !trex )
            return -1;
        struct sample_flags_stats_t
        {
            uint32_t is_leading               [4];
            uint32_t sample_depends_on        [4];
            uint32_t sample_is_depended_on    [4];
            uint32_t sample_has_redundancy    [4];
            uint32_t sample_is_non_sync_sample[2];
        } stats = { { 0 }, { 0 }, { 0 }, { 0 }, { 0 } };
        for( lsmash_entry_t *trun_entry = traf->trun_list.head; trun_entry; trun_entry = trun_entry->next )
        {
            isom_trun_t *trun = (isom_trun_t *)trun_entry->data;
            if( !trun || trun->sample_count == 0 )
                return -1;
            isom_sample_flags_t *sample_flags;
            if( trun->flags & ISOM_TR_FLAGS_SAMPLE_FLAGS_PRESENT )
            {
                if( !trun->optional )
                    return -1;
                for( lsmash_entry_t *optional_entry = trun->optional->head; optional_entry; optional_entry = optional_entry->next )
                {
                    isom_trun_optional_row_t *row = (isom_trun_optional_row_t *)optional_entry->data;
                    if( !row )
                        return -1;
                    sample_flags = &row->sample_flags;
                    ++ stats.is_leading               [ sample_flags->is_leading                ];
                    ++ stats.sample_depends_on        [ sample_flags->sample_depends_on         ];
                    ++ stats.sample_is_depended_on    [ sample_flags->sample_is_depended_on     ];
                    ++ stats.sample_has_redundancy    [ sample_flags->sample_has_redundancy     ];
                    ++ stats.sample_is_non_sync_sample[ sample_flags->sample_is_non_sync_sample ];
                }
            }
            else
            {
                sample_flags = &tfhd->default_sample_flags;
                stats.is_leading               [ sample_flags->is_leading                ] += trun->sample_count;
                stats.sample_depends_on        [ sample_flags->sample_depends_on         ] += trun->sample_count;
                stats.sample_is_depended_on    [ sample_flags->sample_is_depended_on     ] += trun->sample_count;
                stats.sample_has_redundancy    [ sample_flags->sample_has_redundancy     ] += trun->sample_count;
                stats.sample_is_non_sync_sample[ sample_flags->sample_is_non_sync_sample ] += trun->sample_count;
            }
        }
        uint32_t most_used[5] = { 0, 0, 0, 0, 0 };
        for( int i = 0; i < 4; i++ )
        {
            GET_MOST_USED( tfhd, 0, is_leading            );
            GET_MOST_USED( tfhd, 1, sample_depends_on     );
            GET_MOST_USED( tfhd, 2, sample_is_depended_on );
            GET_MOST_USED( tfhd, 3, sample_has_redundancy );
            if( i < 2 )
                GET_MOST_USED( tfhd, 4, sample_is_non_sync_sample );
        }
        int useful_default_sample_duration = 0;
        int useful_default_sample_size = 0;
        for( lsmash_entry_t *trun_entry = traf->trun_list.head; trun_entry; trun_entry = trun_entry->next )
        {
            isom_trun_t *trun = (isom_trun_t *)trun_entry->data;
            if( !(trun->flags & ISOM_TR_FLAGS_SAMPLE_DURATION_PRESENT) )
                useful_default_sample_duration = 1;
            if( !(trun->flags & ISOM_TR_FLAGS_SAMPLE_SIZE_PRESENT) )
                useful_default_sample_size = 1;
            int useful_first_sample_flags   = 1;
            int useful_default_sample_flags = 1;
            if( trun->sample_count == 1 )
            {
                /* It is enough to check only if first_sample_flags equals default_sample_flags or not.
                 * If it is equal, just use default_sample_flags.
                 * If not, just use first_sample_flags of this run. */
                if( !isom_compare_sample_flags( &trun->first_sample_flags, &tfhd->default_sample_flags ) )
                    useful_first_sample_flags = 0;
            }
            else if( trun->optional
                  && trun->optional->head )
            {
                lsmash_entry_t           *optional_entry = trun->optional->head->next;
                isom_trun_optional_row_t *row            = (isom_trun_optional_row_t *)optional_entry->data;
                isom_sample_flags_t representative_sample_flags = row->sample_flags;
                if( isom_compare_sample_flags( &tfhd->default_sample_flags, &representative_sample_flags ) )
                    useful_default_sample_flags = 0;
                if( !isom_compare_sample_flags( &trun->first_sample_flags, &representative_sample_flags ) )
                    useful_first_sample_flags = 0;
                if( useful_default_sample_flags )
                    for( optional_entry = optional_entry->next; optional_entry; optional_entry = optional_entry->next )
                    {
                        row = (isom_trun_optional_row_t *)optional_entry->data;
                        if( isom_compare_sample_flags( &representative_sample_flags, &row->sample_flags ) )
                        {
                            useful_default_sample_flags = 0;
                            break;
                        }
                    }
            }
            if( useful_default_sample_flags )
            {
                tfhd->flags |=  ISOM_TF_FLAGS_DEFAULT_SAMPLE_FLAGS_PRESENT;
                trun->flags &= ~ISOM_TR_FLAGS_SAMPLE_FLAGS_PRESENT;
            }
            else
            {
                useful_first_sample_flags = 0;
                trun->flags |= ISOM_TR_FLAGS_SAMPLE_FLAGS_PRESENT;
            }
            if( useful_first_sample_flags )
                trun->flags |= ISOM_TR_FLAGS_FIRST_SAMPLE_FLAGS_PRESENT;
        }
        if( useful_default_sample_duration && tfhd->default_sample_duration != trex->default_sample_duration )
            tfhd->flags |= ISOM_TF_FLAGS_DEFAULT_SAMPLE_DURATION_PRESENT;
        else
            tfhd->default_sample_duration = trex->default_sample_duration;      /* This might be redundant, but is to be more natural. */
        if( useful_default_sample_size && tfhd->default_sample_size != trex->default_sample_size )
            tfhd->flags |= ISOM_TF_FLAGS_DEFAULT_SAMPLE_SIZE_PRESENT;
        else
            tfhd->default_sample_size = trex->default_sample_size;              /* This might be redundant, but is to be more natural. */
        if( !(tfhd->flags & ISOM_TF_FLAGS_DEFAULT_SAMPLE_FLAGS_PRESENT) )
            tfhd->default_sample_flags = trex->default_sample_flags;            /* This might be redundant, but is to be more natural. */
        else if( !isom_compare_sample_flags( &tfhd->default_sample_flags, &trex->default_sample_flags ) )
            tfhd->flags &= ~ISOM_TF_FLAGS_DEFAULT_SAMPLE_FLAGS_PRESENT;
    }
    /* Complete the last sample groups in the previous track fragments. */
    for( lsmash_entry_t *entry = moof->traf_list.head; entry; entry = entry->next )
    {
        isom_traf_t *traf = (isom_traf_t *)entry->data;
        if( traf->cache->rap )
        {
            isom_sgpd_t *sgpd = isom_get_fragment_sample_group_description( traf, ISOM_GROUP_TYPE_RAP );
            if( !sgpd || isom_rap_grouping_established( traf->cache->rap, 1, sgpd, 1 ) < 0 )
                return -1;
            lsmash_freep( &traf->cache->rap );
        }
        if( traf->cache->roll.pool )
        {
            isom_sbgp_t *sbgp = isom_get_fragment_sample_to_group( traf, ISOM_GROUP_TYPE_ROLL );
            if( !sbgp || isom_all_recovery_completed( sbgp, traf->cache->roll.pool ) < 0 )
                return -1;
        }
    }
    /* Establish Movie Fragment Box.
     * We write exactly one Media Data Box starting immediately after the corresponding Movie Fragment Box. */
    if( file->allow_moof_base )
    {
        /* In this branch, we use default-base-is-moof flag, which indicates implicit base_data_offsets originate in the
         * first byte of each enclosing Movie Fragment Box.
         * We use the sum of the size of the Movie Fragment Box and the offset from the size field of the Media Data Box to
         * the type field of it as the data_offset of the first track run like the following.
         *
         *  _____________ _ offset := 0
         * |   |         |
         * | m | s i z e |
         * |   |_________|
         * | o |         |
         * |   | t y p e |
         * | o |_________|
         * |   |         |
         * | f | d a t a |
         * |___|_________|_ offset := the size of the Movie Fragment Box
         * |   |         |
         * | m | s i z e |
         * |   |_________|
         * | d |         |
         * |   | t y p e |
         * | a |_________|_ offset := the data_offset of the first track run
         * |   |         |
         * | t | d a t a |
         * |___|_________|_ offset := the size of a subsegment containing exactly one movie fragment
         *
         * For a pair of one Movie Fragment Box and one Media Data Box, placed in this order, implicit base_data_offsets
         * indicated by the absence of both base-data-offset-present and default-base-is-moof are somewhat complicated
         * since the implicit base_data_offset of the current track fragment is defined by the end of the data of the
         * previous track fragment and the data_offset of the track runs could be negative value because of interleaving
         * track runs or something other reasons.
         * In contrast, implicit base_data_offsets indicated by default-base-is-moof are simple since the base_data_offset
         * of each track fragment is always constant for that pair and has no dependency on other track fragments.
         */
        for( lsmash_entry_t *entry = moof->traf_list.head; entry; entry = entry->next )
        {
            isom_traf_t *traf = (isom_traf_t *)entry->data;
            traf->tfhd->flags           |= ISOM_TF_FLAGS_DEFAULT_BASE_IS_MOOF;
            traf->tfhd->base_data_offset = file->size;  /* not written actually though */
            for( lsmash_entry_t *trun_entry = traf->trun_list.head; trun_entry; trun_entry = trun_entry->next )
            {
                /* Here, data_offset is always greater than zero. */
                isom_trun_t *trun = trun_entry->data;
                trun->flags |= ISOM_TR_FLAGS_DATA_OFFSET_PRESENT;
            }
        }
        /* Consider the update of tr_flags here. */
        if( isom_update_box_size( moof ) == 0 )
            return -1;
        /* Now, we can calculate offsets in the current movie fragment, so do it. */
        for( lsmash_entry_t *entry = moof->traf_list.head; entry; entry = entry->next )
        {
            isom_traf_t *traf = (isom_traf_t *)entry->data;
            for( lsmash_entry_t *trun_entry = traf->trun_list.head; trun_entry; trun_entry = trun_entry->next )
            {
                isom_trun_t *trun = trun_entry->data;
                trun->data_offset += moof->size + ISOM_BASEBOX_COMMON_SIZE;
            }
        }
    }
    else
    {
        /* In this branch, we use explicit base_data_offset. */
        for( lsmash_entry_t *entry = moof->traf_list.head; entry; entry = entry->next )
        {
            isom_traf_t *traf = (isom_traf_t *)entry->data;
            traf->tfhd->flags |= ISOM_TF_FLAGS_BASE_DATA_OFFSET_PRESENT;
        }
        /* Consider the update of tf_flags here. */
        if( isom_update_box_size( moof ) == 0 )
            return -1;
        /* Now, we can calculate offsets in the current movie fragment, so do it. */
        for( lsmash_entry_t *entry = moof->traf_list.head; entry; entry = entry->next )
        {
            isom_traf_t *traf = (isom_traf_t *)entry->data;
            traf->tfhd->base_data_offset = file->size + moof->size + ISOM_BASEBOX_COMMON_SIZE;
        }
    }
    /* Write Movie Fragment Box and its children. */
    moof->pos = file->size;
    if( isom_write_box( file->bs, (isom_box_t *)moof ) )
        return -1;
    if( file->fragment->fragment_count == 1 )
        file->fragment->first_moof_pos = moof->pos;
    file->size += moof->size;
    /* Output samples. */
    if( isom_output_fragment_media_data( file ) < 0 )
        return -1;
    /* Revert the number of samples in track fragments to 0. */
    for( lsmash_entry_t *entry = moof->traf_list.head; entry; entry = entry->next )
    {
        isom_traf_t *traf = (isom_traf_t *)entry->data;
        if( traf->cache->fragment )
            traf->cache->fragment->sample_count = 0;
    }
    if( !(file->flags & LSMASH_FILE_MODE_INDEX) || file->max_isom_version < 6 )
        return 0;
    /* Make the index of this subsegment. */
    for( lsmash_entry_t *entry = moof->traf_list.head; entry; entry = entry->next )
    {
        isom_traf_t       *traf           = (isom_traf_t *)entry->data;
        isom_tfhd_t       *tfhd           = traf->tfhd;
        isom_fragment_t   *track_fragment = traf->cache->fragment;
        isom_subsegment_t *subsegment     = &track_fragment->subsegment;
        isom_sidx_t       *sidx           = isom_get_sidx( file, tfhd->track_ID );
        isom_trak_t       *trak           = isom_get_trak( file, tfhd->track_ID );
        if( !trak
         || !trak->mdia
         || !trak->mdia->mdhd )
            return -1;
        assert( traf->tfdt );
        if( !sidx )
        {
            sidx = isom_add_sidx( file );
            if( !sidx )
                return -1;
            sidx->reference_ID    = tfhd->track_ID;
            sidx->timescale       = trak->mdia->mdhd->timescale;
            sidx->reserved        = 0;
            sidx->reference_count = 0;
            if( isom_update_indexed_material_offset( file, sidx ) < 0 )
                return -1;
        }
        /* One pair of a Movie Fragment Box with an associated Media Box per subsegment. */
        isom_sidx_referenced_item_t *data = lsmash_malloc( sizeof(isom_sidx_referenced_item_t) );
        if( !data )
            return -1;
        if( lsmash_add_entry( sidx->list, data ) < 0 )
        {
            lsmash_free( data );
            return -1;
        }
        sidx->reference_count = sidx->list->entry_count;
        data->reference_type = 0;  /* media */
        data->reference_size = file->size - moof->pos;
        /* presentation */
        uint64_t TSAP;
        uint64_t TDEC;
        uint64_t TEPT;
        uint64_t TPTF;
        uint64_t composition_duration = subsegment->largest_cts - subsegment->smallest_cts + track_fragment->last_duration;
        int subsegment_in_presentation;
        int first_rp_in_presentation;
        int first_sample_in_presentation;
        isom_elst_entry_t *edit = NULL;
        if( trak->edts && trak->edts->elst && trak->edts->elst->list )
        {
            /* Explicit edits */
            isom_elst_t *elst = trak->edts->elst;
            uint32_t movie_timescale = file->moov->mvhd->timescale;
            uint64_t pts             = subsegment->segment_duration;
            subsegment_in_presentation   = 0;
            first_rp_in_presentation     = 0;
            first_sample_in_presentation = 0;
            for( lsmash_entry_t *elst_entry = elst->list->head; elst_entry; elst_entry = elst_entry->next )
            {
                edit = (isom_elst_entry_t *)elst_entry->data;
                if( !edit )
                    continue;
                uint64_t edit_end_pts;
                uint64_t edit_end_cts;
                if( edit->segment_duration == ISOM_EDIT_DURATION_IMPLICIT
                 || (elst->version == 0 && edit->segment_duration == ISOM_EDIT_DURATION_UNKNOWN32)
                 || (elst->version == 1 && edit->segment_duration == ISOM_EDIT_DURATION_UNKNOWN64) )
                {
                    edit_end_cts = UINT64_MAX;
                    edit_end_pts = UINT64_MAX;
                }
                else
                {
                    if( edit->segment_duration )
                    {
                        double segment_duration = edit->segment_duration * ((double)sidx->timescale / movie_timescale);
                        edit_end_cts = edit->media_time + (uint64_t)(segment_duration * ((double)edit->media_rate / (1 << 16)));
                        edit_end_pts = pts + (uint64_t)segment_duration;
                    }
                    else
                    {
                        uint64_t segment_duration = composition_duration;
                        if( edit->media_time > subsegment->smallest_cts )
                        {
                            if( subsegment->largest_cts + track_fragment->last_duration > edit->media_time )
                                segment_duration -= edit->media_time - subsegment->smallest_cts;
                            else
                                segment_duration = 0;
                        }
                        edit_end_cts = edit->media_time + (uint64_t)(segment_duration * ((double)edit->media_rate / (1 << 16)));
                        edit_end_pts = pts + (uint64_t)segment_duration;
                    }
                }
                if( edit->media_time == ISOM_EDIT_MODE_EMPTY )
                {
                    pts = edit_end_pts;
                    continue;
                }
                if( (subsegment->smallest_cts >= edit->media_time && subsegment->smallest_cts < edit_end_cts)
                 || (subsegment->largest_cts  >= edit->media_time && subsegment->largest_cts  < edit_end_cts) )
                {
                    /* This subsegment is present in this edit. */
                    double rate = (double)edit->media_rate / (1 << 16);
                    uint64_t start_time = LSMASH_MAX( subsegment->smallest_cts, edit->media_time );
                    if( sidx->reference_count == 1 )
                        sidx->earliest_presentation_time = pts;
                    if( subsegment_in_presentation == 0 )
                    {
                        subsegment_in_presentation = 1;
                        if( subsegment->smallest_cts >= edit->media_time )
                            TEPT = pts + (uint64_t)((subsegment->smallest_cts - start_time) / rate);
                        else
                            TEPT = pts;
                    }
                    if( first_rp_in_presentation == 0
                     && ((subsegment->first_ed_cts >= edit->media_time && subsegment->first_ed_cts < edit_end_cts)
                      || (subsegment->first_rp_cts >= edit->media_time && subsegment->first_rp_cts < edit_end_cts)) )
                    {
                        /* FIXME: to distinguish TSAP and TDEC, need something to indicate incorrectly decodable sample. */
                        first_rp_in_presentation = 1;
                        if( subsegment->first_ed_cts >= edit->media_time && subsegment->first_ed_cts < edit_end_cts )
                            TSAP = pts + (uint64_t)((subsegment->first_ed_cts - start_time) / rate);
                        else
                            TSAP = pts;
                        TDEC = TSAP;
                    }
                    if( first_sample_in_presentation == 0
                     && subsegment->first_cts >= edit->media_time && subsegment->first_cts < edit_end_cts )
                    {
                        first_sample_in_presentation = 1;
                        TPTF = pts + (uint64_t)((subsegment->first_cts - start_time) / rate);
                    }
                    uint64_t subsegment_end_pts = pts + (uint64_t)(composition_duration / rate);
                    pts = LSMASH_MIN( edit_end_pts, subsegment_end_pts );
                    /* Update subsegment_duration. */
                    data->subsegment_duration = pts - subsegment->segment_duration;
                }
                else
                    /* This subsegment is not present in this edit. */
                    pts = edit_end_pts;
            }
        }
        else
        {
            /* Implicit edit */
            if( sidx->reference_count == 1 )
                sidx->earliest_presentation_time = subsegment->smallest_cts;
            data->subsegment_duration = composition_duration;
            /* FIXME: to distinguish TSAP and TDEC, need something to indicate incorrectly decodable sample. */
            TSAP = subsegment->first_rp_cts;
            TDEC = subsegment->first_rp_cts;
            TEPT = subsegment->smallest_cts;
            TPTF = subsegment->first_cts;
            subsegment_in_presentation   = 1;
            first_rp_in_presentation     = 1;
            first_sample_in_presentation = 1;
        }
        if( subsegment->first_ra_flags  == ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE
         || subsegment->first_ra_number == 0
         || subsegment->first_rp_number == 0
         || subsegment_in_presentation  == 0
         || first_rp_in_presentation    == 0 )
        {
            /* No SAP in this subsegment. */
            data->starts_with_SAP = 0;
            data->SAP_type        = 0;
            data->SAP_delta_time  = 0;
        }
        else
        {
            data->starts_with_SAP = (subsegment->first_ra_number == 1);
            data->SAP_type        = 0;
            data->SAP_delta_time  = TSAP - TEPT;
            /* Decide SAP_type. */
            if( first_sample_in_presentation )
            {
                if( TEPT == TDEC && TDEC == TSAP && TSAP == TPTF )
                    data->SAP_type = 1;
                else if( TEPT == TDEC && TDEC == TSAP && TSAP < TPTF )
                    data->SAP_type = 2;
                else if( TEPT < TDEC && TDEC == TSAP && TSAP <= TPTF )
                    data->SAP_type = 3;
                else if( TEPT <= TPTF && TPTF < TDEC && TDEC == TSAP )
                    data->SAP_type = 4;
            }
            if( data->SAP_type == 0 )
            {
                if( TEPT == TDEC && TDEC < TSAP )
                    data->SAP_type = 5;
                else if( TEPT < TDEC && TDEC < TSAP )
                    data->SAP_type = 6;
            }
        }
        subsegment->segment_duration += data->subsegment_duration;
        subsegment->first_ed_cts      = UINT64_MAX;
        subsegment->first_rp_cts      = UINT64_MAX;
        subsegment->first_rp_number   = 0;
        subsegment->first_ra_number   = 0;
        subsegment->first_ra_flags    = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE;
        subsegment->decodable         = 0;
    }
    return 0;
}

#undef GET_MOST_USED

static isom_trun_optional_row_t *isom_request_trun_optional_row( isom_trun_t *trun, isom_tfhd_t *tfhd, uint32_t sample_number )
{
    isom_trun_optional_row_t *row = NULL;
    if( !trun->optional )
    {
        trun->optional = lsmash_create_entry_list();
        if( !trun->optional )
            return NULL;
    }
    if( trun->optional->entry_count < sample_number )
    {
        while( trun->optional->entry_count < sample_number )
        {
            row = lsmash_malloc( sizeof(isom_trun_optional_row_t) );
            if( !row )
                return NULL;
            /* Copy from default. */
            row->sample_duration                = tfhd->default_sample_duration;
            row->sample_size                    = tfhd->default_sample_size;
            row->sample_flags                   = tfhd->default_sample_flags;
            row->sample_composition_time_offset = 0;
            if( lsmash_add_entry( trun->optional, row ) )
            {
                lsmash_free( row );
                return NULL;
            }
        }
        return row;
    }
    uint32_t i = 0;
    for( lsmash_entry_t *entry = trun->optional->head; entry; entry = entry->next )
    {
        row = (isom_trun_optional_row_t *)entry->data;
        if( !row )
            return NULL;
        if( ++i == sample_number )
            return row;
    }
    return NULL;
}

int lsmash_create_fragment_empty_duration( lsmash_root_t *root, uint32_t track_ID, uint32_t duration )
{
    if( !root )
        return -1;
    lsmash_file_t *file = root->file;
    if( !file
     || !file->fragment
     || !file->fragment->movie
     || !file->moov )
        return -1;
    isom_trak_t *trak = isom_get_trak( file, track_ID );
    if( !trak
     || !trak->tkhd )
        return -1;
    isom_trex_t *trex = isom_get_trex( file->moov->mvex, track_ID );
    if( !trex )
        return -1;
    isom_moof_t *moof = file->fragment->movie;
    isom_traf_t *traf = isom_get_traf( moof, track_ID );
    if( traf )
        return -1;
    traf = isom_add_traf( moof );
    if( isom_add_tfhd( traf ) )
        return -1;
    isom_tfhd_t *tfhd = traf->tfhd;
    tfhd->flags                   = ISOM_TF_FLAGS_DURATION_IS_EMPTY;    /* no samples for this track fragment yet */
    tfhd->track_ID                = trak->tkhd->track_ID;
    tfhd->default_sample_duration = duration;
    if( duration != trex->default_sample_duration )
        tfhd->flags |= ISOM_TF_FLAGS_DEFAULT_SAMPLE_DURATION_PRESENT;
    traf->cache = trak->cache;
    traf->cache->fragment->traf_number    = moof->traf_list.entry_count;
    traf->cache->fragment->last_duration += duration;       /* The duration of the last sample includes this empty-duration. */
    return 0;
}

static int isom_set_fragment_last_duration( isom_traf_t *traf, uint32_t last_duration )
{
    isom_tfhd_t *tfhd = traf->tfhd;
    if( !traf->trun_list.tail
     || !traf->trun_list.tail->data )
    {
        /* There are no track runs in this track fragment, so it is a empty-duration. */
        isom_trex_t *trex = isom_get_trex( traf->file->moov->mvex, tfhd->track_ID );
        if( !trex )
            return -1;
        tfhd->flags |= ISOM_TF_FLAGS_DURATION_IS_EMPTY;
        if( last_duration != trex->default_sample_duration )
            tfhd->flags |= ISOM_TF_FLAGS_DEFAULT_SAMPLE_DURATION_PRESENT;
        tfhd->default_sample_duration        = last_duration;
        traf->cache->fragment->last_duration = last_duration;
        return 0;
    }
    /* Update the last sample_duration if needed. */
    isom_trun_t *trun = (isom_trun_t *)traf->trun_list.tail->data;
    if( trun->sample_count           == 1
     && traf->trun_list.entry_count == 1 )
    {
        isom_trex_t *trex = isom_get_trex( traf->file->moov->mvex, tfhd->track_ID );
        if( !trex )
            return -1;
        if( last_duration != trex->default_sample_duration )
            tfhd->flags |= ISOM_TF_FLAGS_DEFAULT_SAMPLE_DURATION_PRESENT;
        tfhd->default_sample_duration = last_duration;
    }
    else if( last_duration != tfhd->default_sample_duration )
        trun->flags |= ISOM_TR_FLAGS_SAMPLE_DURATION_PRESENT;
    if( trun->flags )
    {
        isom_trun_optional_row_t *row = isom_request_trun_optional_row( trun, tfhd, trun->sample_count );
        if( !row )
            return -1;
        row->sample_duration = last_duration;
    }
    traf->cache->fragment->last_duration = last_duration;
    return 0;
}

int lsmash_set_last_sample_delta( lsmash_root_t *root, uint32_t track_ID, uint32_t sample_delta )
{
    if( !root || track_ID == 0 )
        return -1;
    lsmash_file_t *file = root->file;
    if( file->fragment
     && file->fragment->movie )
    {
        isom_traf_t *traf = isom_get_traf( file->fragment->movie, track_ID );
        if( !traf
         || !traf->cache
         || !traf->tfhd )
            return -1;
        return isom_set_fragment_last_duration( traf, sample_delta );
    }
    isom_trak_t *trak = isom_get_trak( file, track_ID );
    if( !trak
     || !trak->cache
     || !trak->mdia
     || !trak->mdia->mdhd
     || !trak->mdia->minf
     || !trak->mdia->minf->stbl
     || !trak->mdia->minf->stbl->stsd
     || !trak->mdia->minf->stbl->stsz
     || !trak->mdia->minf->stbl->stts
     || !trak->mdia->minf->stbl->stts->list )
        return -1;
    isom_stbl_t *stbl = trak->mdia->minf->stbl;
    isom_stts_t *stts = stbl->stts;
    uint32_t sample_count = isom_get_sample_count( trak );
    if( !stts->list->tail )
    {
        if( !sample_count )
            return 0;       /* no samples */
        if( sample_count > 1 )
            return -1;      /* irregular sample_count */
        /* Set the duration of the first sample.
         * This duration is also the duration of the last sample. */
        if( isom_add_stts_entry( stbl, sample_delta ) )
            return -1;
        return lsmash_update_track_duration( root, track_ID, 0 );
    }
    uint32_t i = 0;
    for( lsmash_entry_t *entry = stts->list->head; entry; entry = entry->next )
        i += ((isom_stts_entry_t *)entry->data)->sample_count;
    if( sample_count < i )
        return -1;
    int no_last = (sample_count > i);
    isom_stts_entry_t *last_stts_data = (isom_stts_entry_t *)stts->list->tail->data;
    if( !last_stts_data )
        return -1;
    /* Consider QuikcTime fixed compression audio. */
    isom_audio_entry_t *audio = (isom_audio_entry_t *)lsmash_get_entry_data( &trak->mdia->minf->stbl->stsd->list,
                                                                              trak->cache->chunk.sample_description_index );
    if( !audio )
        return -1;
    if( (audio->manager & LSMASH_AUDIO_DESCRIPTION)
     && (audio->manager & LSMASH_QTFF_BASE)
     && (audio->version == 1)
     && (audio->compression_ID != QT_AUDIO_COMPRESSION_ID_VARIABLE_COMPRESSION) )
    {
        if( audio->samplesPerPacket == 0 )
            return -1;
        int exclude_last_sample = no_last ? 0 : 1;
        uint32_t j = audio->samplesPerPacket;
        for( lsmash_entry_t *entry = stts->list->tail; entry && j > 1; entry = entry->prev )
        {
            isom_stts_entry_t *stts_data = (isom_stts_entry_t *)entry->data;
            if( !stts_data )
                return -1;
            for( uint32_t k = exclude_last_sample; k < stts_data->sample_count && j > 1; k++ )
            {
                sample_delta -= stts_data->sample_delta;
                --j;
            }
            exclude_last_sample = 0;
        }
    }
    /* Set sample_delta. */
    if( no_last )
    {
        /* The duration of the last sample is not set yet. */
        if( sample_count - i > 1 )
            return -1;
        /* Add a sample_delta. */
        if( sample_delta == last_stts_data->sample_delta )
            ++ last_stts_data->sample_count;
        else if( isom_add_stts_entry( stbl, sample_delta ) < 0 )
            return -1;
    }
    /* The duration of the last sample is already set. Replace it with a new one. */
    else if( isom_replace_last_sample_delta( stbl, sample_delta ) < 0 )
        return -1;
    return lsmash_update_track_duration( root, track_ID, sample_delta );
}

/*---- timeline manipulator ----*/

int lsmash_modify_explicit_timeline_map( lsmash_root_t *root, uint32_t track_ID, uint32_t edit_number, lsmash_edit_t edit )
{
    if( !root || edit.duration == 0 || edit.start_time < -1 )
        return -1;
    lsmash_file_t *file = root->file;
    isom_trak_t *trak = isom_get_trak( file, track_ID );
    if( !trak
     || !trak->edts
     || !trak->edts->elst
     || !trak->edts->elst->list )
        return -1;
    isom_elst_t       *elst = trak->edts->elst;
    isom_elst_entry_t *data = (isom_elst_entry_t *)lsmash_get_entry_data( elst->list, edit_number );
    if( !data )
        return -1;
    data->segment_duration = edit.duration;
    data->media_time       = edit.start_time;
    data->media_rate       = edit.rate;
    if( elst->pos == 0 || !file->fragment || file->bs->unseekable )
        return isom_update_tkhd_duration( trak );
    /* Rewrite the specified entry.
     * Note: we don't update the version of the Edit List Box. */
    lsmash_bs_t *bs = file->bs;
    uint64_t current_pos = bs->offset;
    uint64_t entry_pos = elst->pos + ISOM_LIST_FULLBOX_COMMON_SIZE + ((uint64_t)edit_number - 1) * (elst->version == 1 ? 20 : 12);
    lsmash_bs_seek( bs, entry_pos, SEEK_SET );
    if( elst->version )
    {
        lsmash_bs_put_be64( bs, data->segment_duration );
        lsmash_bs_put_be64( bs, data->media_time );
    }
    else
    {
        lsmash_bs_put_be32( bs, (uint32_t)LSMASH_MIN( data->segment_duration, UINT32_MAX ) );
        lsmash_bs_put_be32( bs, (uint32_t)data->media_time );
    }
    lsmash_bs_put_be32( bs, data->media_rate );
    int ret = lsmash_bs_flush_buffer( bs );
    lsmash_bs_seek( bs, current_pos, SEEK_SET );
    return ret;
}

int lsmash_create_explicit_timeline_map( lsmash_root_t *root, uint32_t track_ID, lsmash_edit_t edit )
{
    if( !root || edit.start_time < -1 )
        return -1;
    isom_trak_t *trak = isom_get_trak( root->file, track_ID );
    if( !trak
     || !trak->tkhd )
        return -1;
    edit.duration = (edit.duration || root->file->fragment) ? edit.duration
                  : trak->tkhd->duration ? trak->tkhd->duration
                  : isom_update_tkhd_duration( trak ) ? 0
                  : trak->tkhd->duration;
    if( (!trak->edts       && isom_add_edts( trak ))
     || (!trak->edts->elst && isom_add_elst( trak->edts ))
     || isom_add_elst_entry( trak->edts->elst, edit.duration, edit.start_time, edit.rate ) )
        return -1;
    return isom_update_tkhd_duration( trak );
}

int lsmash_get_explicit_timeline_map( lsmash_root_t *root, uint32_t track_ID, uint32_t edit_number, lsmash_edit_t *edit )
{
    if( !root || !edit )
        return -1;
    isom_trak_t *trak = isom_get_trak( root->file, track_ID );
    if( !trak )
        return -1;
    if( !trak->edts
     || !trak->edts->elst )
    {
        /* no edits */
        edit->duration   = 0;
        edit->start_time = 0;
        edit->rate       = 0;
        return 0;
    }
    isom_elst_entry_t *elst = (isom_elst_entry_t *)lsmash_get_entry_data( trak->edts->elst->list, edit_number );
    if( !elst )
        return -1;
    edit->duration   = elst->segment_duration;
    edit->start_time = elst->media_time;
    edit->rate       = elst->media_rate;
    return 0;
}

uint32_t lsmash_count_explicit_timeline_map( lsmash_root_t *root, uint32_t track_ID )
{
    if( !root )
        return -1;
    isom_trak_t *trak = isom_get_trak( root->file, track_ID );
    if( !trak
     || !trak->edts
     || !trak->edts->elst
     || !trak->edts->elst->list )
        return 0;
    return trak->edts->elst->list->entry_count;
}

/*---- create / modification time fields manipulators ----*/

int lsmash_update_media_modification_time( lsmash_root_t *root, uint32_t track_ID )
{
    if( !root )
        return -1;
    isom_trak_t *trak = isom_get_trak( root->file, track_ID );
    if( !trak
     || !trak->mdia
     || !trak->mdia->mdhd )
        return -1;
    isom_mdhd_t *mdhd = trak->mdia->mdhd;
    mdhd->modification_time = isom_get_current_mp4time();
    /* overwrite strange creation_time */
    if( mdhd->creation_time > mdhd->modification_time )
        mdhd->creation_time = mdhd->modification_time;
    return 0;
}

int lsmash_update_track_modification_time( lsmash_root_t *root, uint32_t track_ID )
{
    if( !root )
        return -1;
    isom_trak_t *trak = isom_get_trak( root->file, track_ID );
    if( !trak
     || !trak->tkhd )
        return -1;
    isom_tkhd_t *tkhd = trak->tkhd;
    tkhd->modification_time = isom_get_current_mp4time();
    /* overwrite strange creation_time */
    if( tkhd->creation_time > tkhd->modification_time )
        tkhd->creation_time = tkhd->modification_time;
    return 0;
}

int lsmash_update_movie_modification_time( lsmash_root_t *root )
{
    if( !root )
        return -1;
    lsmash_file_t *file = root->file;
    if( !file
     || !file->moov
     || !file->moov->mvhd )
        return -1;
    isom_mvhd_t *mvhd = file->moov->mvhd;
    mvhd->modification_time = isom_get_current_mp4time();
    /* overwrite strange creation_time */
    if( mvhd->creation_time > mvhd->modification_time )
        mvhd->creation_time = mvhd->modification_time;
    return 0;
}

/*---- sample manipulators ----*/
lsmash_sample_t *lsmash_create_sample( uint32_t size )
{
    lsmash_sample_t *sample = lsmash_malloc_zero( sizeof(lsmash_sample_t) );
    if( !sample )
        return NULL;
    if( size == 0 )
        return sample;
    sample->data = lsmash_malloc( size );
    if( !sample->data )
    {
        lsmash_free( sample );
        return NULL;
    }
    sample->length = size;
    return sample;
}

int lsmash_sample_alloc( lsmash_sample_t *sample, uint32_t size )
{
    if( !sample )
        return -1;
    if( size == 0 )
    {
        if( sample->data )
            lsmash_free( sample->data );
        sample->data   = NULL;
        sample->length = 0;
        return 0;
    }
    if( size == sample->length )
        return 0;
    uint8_t *data;
    if( !sample->data )
        data = lsmash_malloc( size );
    else
        data = lsmash_realloc( sample->data, size );
    if( !data )
        return -1;
    sample->data   = data;
    sample->length = size;
    return 0;
}

void lsmash_delete_sample( lsmash_sample_t *sample )
{
    if( !sample )
        return;
    if( sample->data )
        lsmash_free( sample->data );
    lsmash_free( sample );
}

isom_sample_pool_t *isom_create_sample_pool( uint64_t size )
{
    isom_sample_pool_t *pool = lsmash_malloc_zero( sizeof(isom_sample_pool_t) );
    if( !pool )
        return NULL;
    if( size == 0 )
        return pool;
    pool->data = lsmash_malloc( size );
    if( !pool->data )
    {
        lsmash_free( pool );
        return NULL;
    }
    pool->alloc = size;
    return pool;
}

void isom_remove_sample_pool( isom_sample_pool_t *pool )
{
    if( !pool )
        return;
    if( pool->data )
        lsmash_free( pool->data );
    lsmash_free( pool );
}

static uint32_t isom_add_size( isom_trak_t *trak, uint32_t sample_size )
{
    if( isom_add_stsz_entry( trak->mdia->minf->stbl, sample_size ) )
        return 0;
    return isom_get_sample_count( trak );
}

static uint32_t isom_add_dts( isom_stbl_t *stbl, isom_timestamp_t *cache, uint64_t dts )
{
    isom_stts_t *stts = stbl->stts;
    if( stts->list->entry_count == 0 )
    {
        if( isom_add_stts_entry( stbl, dts ) )
            return 0;
        cache->dts = dts;
        return dts;
    }
    if( dts <= cache->dts )
        return 0;
    uint32_t sample_delta = dts - cache->dts;
    isom_stts_entry_t *data = (isom_stts_entry_t *)stts->list->tail->data;
    if( data->sample_delta == sample_delta )
        ++ data->sample_count;
    else if( isom_add_stts_entry( stbl, sample_delta ) )
        return 0;
    cache->dts = dts;
    return sample_delta;
}

static int isom_add_cts( isom_stbl_t *stbl, isom_timestamp_t *cache, uint64_t cts )
{
    isom_ctts_t *ctts = stbl->ctts;
    if( !ctts )
    {
        if( cts == cache->dts )
        {
            cache->cts = cts;
            return 0;
        }
        /* Add ctts box and the first ctts entry. */
        if( isom_add_ctts( stbl ) || isom_add_ctts_entry( stbl, 0 ) )
            return -1;
        ctts = stbl->ctts;
        isom_ctts_entry_t *data = (isom_ctts_entry_t *)ctts->list->head->data;
        uint32_t sample_count = stbl->stsz->sample_count;
        if( sample_count != 1 )
        {
            data->sample_count = sample_count - 1;
            if( isom_add_ctts_entry( stbl, cts - cache->dts ) )
                return -1;
        }
        else
            data->sample_offset = cts;
        cache->cts = cts;
        return 0;
    }
    if( !ctts->list )
        return -1;
    isom_ctts_entry_t *data = (isom_ctts_entry_t *)ctts->list->tail->data;
    uint32_t sample_offset = cts - cache->dts;
    if( data->sample_offset == sample_offset )
        ++ data->sample_count;
    else if( isom_add_ctts_entry( stbl, sample_offset ) )
        return -1;
    cache->cts = cts;
    return 0;
}

static int isom_add_timestamp( isom_trak_t *trak, uint64_t dts, uint64_t cts )
{
    if( !trak->cache
     || !trak->mdia->minf->stbl->stts
     || !trak->mdia->minf->stbl->stts->list )
        return -1;
    lsmash_file_t *file = trak->file;
    if( file->isom_compatible && file->qt_compatible && (cts - dts) > INT32_MAX )
        return -1;      /* sample_offset is not compatible. */
    isom_stbl_t      *stbl     = trak->mdia->minf->stbl;
    isom_timestamp_t *ts_cache = &trak->cache->timestamp;
    uint32_t sample_count = isom_get_sample_count( trak );
    uint32_t sample_delta = sample_count > 1 ? isom_add_dts( stbl, ts_cache, dts ) : 0;
    if( sample_count > 1 && !sample_delta )
        return -1;
    if( isom_add_cts( stbl, ts_cache, cts ) )
        return -1;
    if( (cts + ts_cache->ctd_shift) < dts )
    {
        if( (file->max_isom_version <  4 && !file->qt_compatible)   /* Negative sample offset is not supported. */
         || (file->max_isom_version >= 4 &&  file->qt_compatible)   /* ctts version 1 is not defined in QTFF. */
         || ((dts - cts) > INT32_MAX) )                             /* Overflow */
            return -1;
        ts_cache->ctd_shift = dts - cts;
        if( stbl->ctts->version == 0 && !file->qt_compatible )
            stbl->ctts->version = 1;
    }
    if( trak->cache->fragment )
    {
        isom_fragment_t *fragment_cache = trak->cache->fragment;
        fragment_cache->last_duration = sample_delta;
        fragment_cache->largest_cts   = LSMASH_MAX( ts_cache->cts, fragment_cache->largest_cts );
    }
    return 0;
}

static int isom_add_sync_point( isom_trak_t *trak, uint32_t sample_number, lsmash_sample_property_t *prop )
{
    isom_stbl_t  *stbl  = trak->mdia->minf->stbl;
    isom_cache_t *cache = trak->cache;
    if( !(prop->ra_flags & ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC) )   /* no null check for prop */
    {
        if( !cache->all_sync )
            return 0;
        if( !stbl->stss && isom_add_stss( stbl ) )
            return -1;
        if( isom_add_stss_entry( stbl, 1 ) )    /* Declare here the first sample is a sync sample. */
            return -1;
        cache->all_sync = 0;
        return 0;
    }
    if( cache->all_sync )     /* We don't need stss box if all samples are sync sample. */
        return 0;
    if( !stbl->stss )
    {
        if( isom_get_sample_count( trak ) == 1 )
        {
            cache->all_sync = 1;    /* Also the first sample is a sync sample. */
            return 0;
        }
        if( isom_add_stss( stbl ) )
            return -1;
    }
    return isom_add_stss_entry( stbl, sample_number );
}

static int isom_add_partial_sync( isom_trak_t *trak, uint32_t sample_number, lsmash_sample_property_t *prop )
{
    if( !trak->file->qt_compatible )
        return 0;
    if( !(prop->ra_flags & QT_SAMPLE_RANDOM_ACCESS_FLAG_PARTIAL_SYNC) )
        return 0;
    /* This sample is a partial sync sample. */
    isom_stbl_t *stbl = trak->mdia->minf->stbl;
    if( !stbl->stps && isom_add_stps( stbl ) )
        return -1;
    return isom_add_stps_entry( stbl, sample_number );
}

static int isom_add_dependency_type( isom_trak_t *trak, lsmash_sample_property_t *prop )
{
    if( !trak->file->qt_compatible && !trak->file->avc_extensions )
        return 0;
    uint8_t avc_extensions = trak->file->avc_extensions;
    isom_stbl_t *stbl = trak->mdia->minf->stbl;
    if( stbl->sdtp )
        return isom_add_sdtp_entry( (isom_box_t *)stbl, prop, avc_extensions );
    if( !prop->allow_earlier && !prop->leading && !prop->independent && !prop->disposable && !prop->redundant )  /* no null check for prop */
        return 0;
    if( isom_add_sdtp( (isom_box_t *)stbl ) )
        return -1;
    uint32_t count = isom_get_sample_count( trak );
    /* fill past samples with ISOM_SAMPLE_*_UNKNOWN */
    lsmash_sample_property_t null_prop = { 0 };
    for( uint32_t i = 1; i < count; i++ )
        if( isom_add_sdtp_entry( (isom_box_t *)stbl, &null_prop, avc_extensions ) )
            return -1;
    return isom_add_sdtp_entry( (isom_box_t *)stbl, prop, avc_extensions );
}

static int isom_rap_grouping_established( isom_rap_group_t *group, int num_leading_samples_known, isom_sgpd_t *sgpd, int is_fragment )
{
    isom_rap_entry_t *rap = group->random_access;
    if( !rap )
        return 0;
    assert( rap == (isom_rap_entry_t *)sgpd->list->tail->data );
    rap->num_leading_samples_known = num_leading_samples_known;
    /* Avoid duplication of sample group descriptions. */
    uint32_t group_description_index = is_fragment ? 0x10001 : 1;
    for( lsmash_entry_t *entry = sgpd->list->head; entry != sgpd->list->tail; entry = entry->next )
    {
        isom_rap_entry_t *data = (isom_rap_entry_t *)entry->data;
        if( !data )
            return -1;
        if( rap->num_leading_samples_known == data->num_leading_samples_known
         && rap->num_leading_samples       == data->num_leading_samples )
        {
            /* The same description already exists.
             * Remove the latest random access entry. */
            lsmash_remove_entry_tail( sgpd->list, NULL );
            /* Replace assigned group_description_index with the one corresponding the same description. */
            if( group->assignment->group_description_index == 0 )
            {
                /* We don't create consecutive sample groups not assigned to 'rap '.
                 * So the previous sample group shall be a group of 'rap ' if any. */
                if( group->prev_assignment )
                {
                    assert( group->prev_assignment->group_description_index );
                    group->prev_assignment->group_description_index = group_description_index;
                }
            }
            else
                group->assignment->group_description_index = group_description_index;
            break;
        }
        ++group_description_index;
    }
    group->random_access = NULL;
    return 0;
}

static int isom_group_random_access( isom_box_t *parent, lsmash_sample_t *sample )
{
    if( parent->file->max_isom_version < 6 )
        return 0;
    isom_cache_t *cache;
    isom_sbgp_t  *sbgp;
    isom_sgpd_t  *sgpd;
    uint32_t      sample_count;
    int           is_fragment;
    if( lsmash_check_box_type_identical( parent->type, ISOM_BOX_TYPE_TRAK ) )
    {
        isom_trak_t *trak = (isom_trak_t *)parent;
        cache = trak->cache;
        sbgp  = isom_get_sample_to_group         ( trak->mdia->minf->stbl, ISOM_GROUP_TYPE_RAP );
        sgpd  = isom_get_sample_group_description( trak->mdia->minf->stbl, ISOM_GROUP_TYPE_RAP );
        sample_count = isom_get_sample_count( trak );
        is_fragment  = 0;
    }
    else if( lsmash_check_box_type_identical( parent->type, ISOM_BOX_TYPE_TRAF ) )
    {
        isom_traf_t *traf = (isom_traf_t *)parent;
        cache = traf->cache;
        sbgp  = isom_get_fragment_sample_to_group         ( traf, ISOM_GROUP_TYPE_RAP );
        sgpd  = isom_get_fragment_sample_group_description( traf, ISOM_GROUP_TYPE_RAP );
        sample_count = cache->fragment->sample_count + 1;
        is_fragment  = 1;
    }
    else
    {
        assert( 0 );
        sbgp = NULL;
        sgpd = NULL;
    }
    if( !sbgp || !sgpd )
        return 0;
    lsmash_sample_property_t *prop = &sample->prop;
    uint8_t is_rap = (prop->ra_flags & ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC)
                  || (prop->ra_flags & QT_SAMPLE_RANDOM_ACCESS_FLAG_PARTIAL_SYNC)
                  || (prop->ra_flags & ISOM_SAMPLE_RANDOM_ACCESS_FLAG_RAP)
                  || (LSMASH_IS_POST_ROLL_START( prop->ra_flags ) && prop->post_roll.identifier == prop->post_roll.complete);
    isom_rap_group_t *group = cache->rap;
    if( !group )
    {
        /* This sample is the first sample, create a grouping cache. */
        assert( sample_count == 1 );
        group = lsmash_malloc( sizeof(isom_rap_group_t) );
        if( !group )
            return -1;
        if( is_rap )
        {
            group->random_access = isom_add_rap_group_entry( sgpd );
            group->assignment    = isom_add_group_assignment_entry( sbgp, 1, sgpd->list->entry_count + (is_fragment ? 0x10000 : 0) );
        }
        else
        {
            /* The first sample is not always a random access point. */
            group->random_access = NULL;
            group->assignment    = isom_add_group_assignment_entry( sbgp, 1, 0 );
        }
        if( !group->assignment )
        {
            lsmash_free( group );
            return -1;
        }
        group->prev_assignment = NULL;
        group->is_prev_rap     = is_rap;
        cache->rap             = group;
        return 0;
    }
    if( group->is_prev_rap )
    {
        /* OK. here, the previous sample is a menber of 'rap '. */
        if( !is_rap )
        {
            /* This sample isn't a member of 'rap ' and the previous sample is.
             * So we create a new group and set 0 on its group_description_index. */
            group->prev_assignment = group->assignment;
            group->assignment      = isom_add_group_assignment_entry( sbgp, 1, 0 );
            if( !group->assignment )
            {
                lsmash_free( group );
                return -1;
            }
        }
        else if( !LSMASH_IS_CLOSED_RAP( prop->ra_flags ) )
        {
            /* Create a new group since there is the possibility the next sample is a leading sample.
             * This sample is a member of 'rap ', so we set appropriate value on its group_description_index. */
            if( isom_rap_grouping_established( group, 1, sgpd, is_fragment ) < 0 )
                return -1;
            group->random_access   = isom_add_rap_group_entry( sgpd );
            group->prev_assignment = group->assignment;
            group->assignment      = isom_add_group_assignment_entry( sbgp, 1, sgpd->list->entry_count + (is_fragment ? 0x10000 : 0) );
            if( !group->assignment )
            {
                lsmash_free( group );
                return -1;
            }
        }
        else    /* The previous and current sample are a member of 'rap ', and the next sample must not be a leading sample. */
            ++ group->assignment->sample_count;
    }
    else if( is_rap )
    {
        /* This sample is a member of 'rap ' and the previous sample isn't.
         * So we create a new group and set appropriate value on its group_description_index. */
        if( isom_rap_grouping_established( group, 1, sgpd, is_fragment ) < 0 )
            return -1;
        group->random_access   = isom_add_rap_group_entry( sgpd );
        group->prev_assignment = group->assignment;
        group->assignment      = isom_add_group_assignment_entry( sbgp, 1, sgpd->list->entry_count + (is_fragment ? 0x10000 : 0) );
        if( !group->assignment )
        {
            lsmash_free( group );
            return -1;
        }
    }
    else    /* The previous and current sample aren't a member of 'rap '. */
        ++ group->assignment->sample_count;
    /* Obtain the property of the latest random access point group. */
    if( !is_rap && group->random_access )
    {
        if( prop->leading == ISOM_SAMPLE_LEADING_UNKNOWN )
        {
            /* We can no longer know num_leading_samples in this group. */
            if( isom_rap_grouping_established( group, 0, sgpd, is_fragment ) < 0 )
                return -1;
        }
        else
        {
            if( prop->leading == ISOM_SAMPLE_IS_UNDECODABLE_LEADING
             || prop->leading == ISOM_SAMPLE_IS_DECODABLE_LEADING )
                ++ group->random_access->num_leading_samples;
            /* no more consecutive leading samples in this group */
            else if( isom_rap_grouping_established( group, 1, sgpd, is_fragment ) < 0 )
                return -1;
        }
    }
    group->is_prev_rap = is_rap;
    return 0;
}

static int isom_roll_grouping_established( isom_roll_group_t *group, int16_t roll_distance, isom_sgpd_t *sgpd, int is_fragment )
{
    /* Avoid duplication of sample group descriptions. */
    uint32_t group_description_index = is_fragment ? 0x10001 : 1;
    for( lsmash_entry_t *entry = sgpd->list->head; entry; entry = entry->next )
    {
        isom_roll_entry_t *data = (isom_roll_entry_t *)entry->data;
        if( !data )
            return -1;
        if( roll_distance == data->roll_distance )
        {
            /* The same description already exists.
             * Set the group_description_index corresponding the same description. */
            group->assignment->group_description_index = group_description_index;
            group->described = 1;
            return 0;
        }
        ++group_description_index;
    }
    /* Add a new roll recovery description. */
    if( !isom_add_roll_group_entry( sgpd, roll_distance ) )
        return -1;
    group->assignment->group_description_index = sgpd->list->entry_count + (is_fragment ? 0x10000 : 0);
    group->described = 1;
    return 0;
}

static int isom_deduplicate_roll_group( isom_sbgp_t *sbgp, lsmash_entry_list_t *pool )
{
    /* Deduplication */
    uint32_t current_group_number = sbgp->list->entry_count - pool->entry_count + 1;
    isom_group_assignment_entry_t *prev_assignment = (isom_group_assignment_entry_t *)lsmash_get_entry_data( sbgp->list, current_group_number - 1 );
    for( lsmash_entry_t *entry = pool->head; entry; )
    {
        isom_roll_group_t *group = (isom_roll_group_t *)entry->data;
        if( !group
         || !group->assignment )
            return -1;
        if( !group->delimited
         || !group->described )
            return 0;
        if( prev_assignment && prev_assignment->group_description_index == group->assignment->group_description_index )
        {
            /* Merge the current group with the previous. */
            lsmash_entry_t *next_entry = entry->next;
            prev_assignment->sample_count += group->assignment->sample_count;
            if( lsmash_remove_entry( sbgp->list, current_group_number, NULL )
             || lsmash_remove_entry_direct( pool, entry, NULL ) )
                return -1;
            entry = next_entry;
        }
        else
        {
            entry = entry->next;
            prev_assignment = group->assignment;
            ++current_group_number;
        }
    }
    return 0;
}

/* Remove pooled caches that has become unnecessary. */
static int isom_clean_roll_pool( lsmash_entry_list_t *pool )
{
    for( lsmash_entry_t *entry = pool->head; entry; entry = pool->head )
    {
        isom_roll_group_t *group = (isom_roll_group_t *)entry->data;
        if( !group )
            return -1;
        if( !group->delimited
         || !group->described )
            return 0;
        if( lsmash_remove_entry_direct( pool, entry, NULL ) )
            return -1;
    }
    return 0;
}

static int isom_flush_roll_pool( isom_sbgp_t *sbgp, lsmash_entry_list_t *pool )
{
    if( isom_deduplicate_roll_group( sbgp, pool ) < 0 )
        return -1;
    return isom_clean_roll_pool( pool );
}

static int isom_all_recovery_described( isom_sbgp_t *sbgp, lsmash_entry_list_t *pool )
{
    for( lsmash_entry_t *entry = pool->head; entry; entry = entry->next )
    {
        isom_roll_group_t *group = (isom_roll_group_t *)entry->data;
        if( !group )
            return -1;
        group->described = 1;
    }
    return isom_flush_roll_pool( sbgp, pool );
}

static int isom_all_recovery_completed( isom_sbgp_t *sbgp, lsmash_entry_list_t *pool )
{
    for( lsmash_entry_t *entry = pool->head; entry; entry = entry->next )
    {
        isom_roll_group_t *group = (isom_roll_group_t *)entry->data;
        if( !group )
            return -1;
        group->described = 1;
        group->delimited = 1;
    }
    return isom_flush_roll_pool( sbgp, pool );
}

static int isom_group_roll_recovery( isom_box_t *parent, lsmash_sample_t *sample )
{
    if( !parent->file->avc_extensions
     && !parent->file->qt_compatible )
        return 0;
    isom_cache_t *cache;
    isom_sbgp_t  *sbgp;
    isom_sgpd_t  *sgpd;
    uint32_t      sample_count;
    int           is_fragment;
    if( lsmash_check_box_type_identical( parent->type, ISOM_BOX_TYPE_TRAK ) )
    {
        isom_trak_t *trak = (isom_trak_t *)parent;
        cache = trak->cache;
        sbgp  = isom_get_sample_to_group         ( trak->mdia->minf->stbl, ISOM_GROUP_TYPE_ROLL );
        sgpd  = isom_get_sample_group_description( trak->mdia->minf->stbl, ISOM_GROUP_TYPE_ROLL );
        sample_count = isom_get_sample_count( trak );
        is_fragment  = 0;
    }
    else if( lsmash_check_box_type_identical( parent->type, ISOM_BOX_TYPE_TRAF ) )
    {
        if( parent->file->max_isom_version < 6 )
            return 0;
        isom_traf_t *traf = (isom_traf_t *)parent;
        cache = traf->cache;
        sbgp  = isom_get_fragment_sample_to_group         ( traf, ISOM_GROUP_TYPE_ROLL );
        sgpd  = isom_get_fragment_sample_group_description( traf, ISOM_GROUP_TYPE_ROLL );
        sample_count = cache->fragment->sample_count + 1;
        is_fragment  = 1;
    }
    else
    {
        assert( 0 );
        sbgp = NULL;
        sgpd = NULL;
    }
    if( !sbgp || !sgpd )
        return 0;
    lsmash_entry_list_t *pool = cache->roll.pool;
    if( !pool )
    {
        pool = lsmash_create_entry_list();
        if( !pool )
            return -1;
        cache->roll.pool = pool;
    }
    lsmash_sample_property_t *prop  = &sample->prop;
    isom_roll_group_t        *group = (isom_roll_group_t *)lsmash_get_entry_data( pool, pool->entry_count );
    int is_recovery_start = LSMASH_IS_POST_ROLL_START( prop->ra_flags );
    int valid_pre_roll = !is_recovery_start
                      && (prop->ra_flags != ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE)
                      && (prop->pre_roll.distance > 0)
                      && (prop->pre_roll.distance <= -INT16_MIN);
    int new_group = !group || is_recovery_start || (group->prev_is_recovery_start != is_recovery_start);
    if( !new_group )
    {
        /* Check pre-roll distance. */
        if( !group->assignment )
            return -1;
        uint32_t group_description_index = group->assignment->group_description_index;
        if( group_description_index && is_fragment )
        {
            assert( group_description_index > 0x10000 );
            group_description_index -= 0x10000;
        }
        isom_roll_entry_t *prev_roll = (isom_roll_entry_t *)lsmash_get_entry_data( sgpd->list, group_description_index );
        if( !prev_roll )
            new_group = valid_pre_roll;
        else if( !valid_pre_roll || (prop->pre_roll.distance != -prev_roll->roll_distance) )
            /* Pre-roll distance is different from the previous. */
            new_group = 1;
    }
    if( new_group )
    {
        if( group )
            group->delimited = 1;
        else
            assert( sample_count == 1 );
        /* Create a new group. */
        group = lsmash_malloc_zero( sizeof(isom_roll_group_t) );
        if( !group )
            return -1;
        group->prev_is_recovery_start = is_recovery_start;
        group->assignment             = isom_add_group_assignment_entry( sbgp, 1, 0 );
        if( !group->assignment || lsmash_add_entry( pool, group ) )
        {
            lsmash_free( group );
            return -1;
        }
        if( is_recovery_start )
        {
            /* a member of non-roll or post-roll group */
            group->first_sample   = sample_count;
            group->recovery_point = prop->post_roll.complete;
        }
        else
        {
            if( valid_pre_roll )
            {
                /* a member of pre-roll group */
                if( isom_roll_grouping_established( group, -prop->pre_roll.distance, sgpd, is_fragment ) < 0 )
                    return -1;
            }
            else
                /* a member of non-roll group */
                group->described = 1;
        }
    }
    else
    {
        group->prev_is_recovery_start    = is_recovery_start;
        group->assignment->sample_count += 1;
    }
    /* If encountered a sync sample, all recovery is completed here. */
    if( prop->ra_flags & ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC )
        return isom_all_recovery_described( sbgp, pool );
    /* Check whether this sample is a random access recovery point or not. */
    for( lsmash_entry_t *entry = pool->head; entry; entry = entry->next )
    {
        group = (isom_roll_group_t *)entry->data;
        if( !group )
            return -1;
        if( group->described )
            continue;
        if( prop->post_roll.identifier == group->recovery_point )
        {
            int16_t distance = sample_count - group->first_sample;
            /* Add a roll recovery entry only when roll_distance isn't zero since roll_distance = 0 must not be used. */
            if( distance )
            {
                /* Now, this group is a 'roll'. */
                if( isom_roll_grouping_established( group, distance, sgpd, is_fragment ) < 0 )
                    return -1;
                /* All groups before the current group are described. */
                lsmash_entry_t *current = entry;
                for( entry = pool->head; entry != current; entry = entry->next )
                {
                    group = (isom_roll_group_t *)entry->data;
                    if( !group )
                        return -1;
                    group->described = 1;
                }
                /* Cache the CTS of the first recovery point in a subsegment. */
                if( cache->fragment
                 && cache->fragment->subsegment.first_rp_number == 0 )
                {
                    cache->fragment->subsegment.first_rp_number = sample_count;
                    cache->fragment->subsegment.first_rp_cts    = sample->cts;
                    cache->fragment->subsegment.first_ed_cts    = sample->cts;
                    cache->fragment->subsegment.decodable       = 1;
                }
            }
            else
                group->described = 1;
            break;      /* Avoid evaluating groups, in the pool, having the same identifier for recovery point again. */
        }
    }
    return isom_flush_roll_pool( sbgp, pool );
}

/* returns 1 if pooled samples must be flushed. */
/* FIXME: I wonder if this function should have a extra argument which indicates force_to_flush_cached_chunk.
   see lsmash_append_sample for detail. */
static int isom_add_chunk( isom_trak_t *trak, lsmash_sample_t *sample )
{
    if( !trak->file
     || !trak->cache
     || !trak->mdia->mdhd
     ||  trak->mdia->mdhd->timescale == 0
     || !trak->mdia->minf->stbl->stsc
     || !trak->mdia->minf->stbl->stsc->list )
        return -1;
    lsmash_file_t *file    = trak->file;
    isom_chunk_t  *current = &trak->cache->chunk;
    if( !current->pool )
    {
        /* Very initial settings, just once per track */
        current->pool = isom_create_sample_pool( 0 );
        if( !current->pool )
            return -1;
    }
    if( !current->pool->sample_count )
    {
        /* Cannot decide whether we should flush the current sample or not here yet. */
        current->chunk_number            += 1;
        current->sample_description_index = sample->index;
        current->first_dts                = sample->dts;
        return 0;
    }
    if( sample->dts < current->first_dts )
        return -1;  /* easy error check. */
    if( (file->max_chunk_duration >= ((double)(sample->dts - current->first_dts) / trak->mdia->mdhd->timescale))
     && (file->max_chunk_size     >= current->pool->size + sample->length)
     && (current->sample_description_index == sample->index) )
        return 0;   /* No need to flush current cached chunk, the current sample must be put into that. */
    /* NOTE: chunk relative stuff must be pushed into file after a chunk is fully determined with its contents. */
    /* Now the current cached chunk is fixed, actually add the chunk relative properties to its file accordingly. */
    isom_stbl_t       *stbl           = trak->mdia->minf->stbl;
    isom_stsc_entry_t *last_stsc_data = stbl->stsc->list->tail ? (isom_stsc_entry_t *)stbl->stsc->list->tail->data : NULL;
    /* Create a new chunk sequence in this track if needed. */
    if( (!last_stsc_data
      || current->pool->sample_count       != last_stsc_data->samples_per_chunk
      || current->sample_description_index != last_stsc_data->sample_description_index)
     && isom_add_stsc_entry( stbl, current->chunk_number,
                                   current->pool->sample_count,
                                   current->sample_description_index ) )
        return -1;
    /* Add a new chunk offset in this track. */
    uint64_t offset = file->size;
    if( file->fragment )
        offset += ISOM_BASEBOX_COMMON_SIZE + file->fragment->pool_size;
    if( isom_add_stco_entry( stbl, offset ) )
        return -1;
    /* Update and re-initialize cache, using the current sample */
    current->chunk_number            += 1;
    current->sample_description_index = sample->index;
    current->first_dts                = sample->dts;
    /* current->pool must be flushed in isom_append_sample_internal() */
    return 1;
}

static int isom_write_pooled_samples( lsmash_file_t *file, isom_sample_pool_t *pool )
{
    if( !file
     || !file->mdat
     || !file->bs
     || !file->bs->stream )
        return -1;
    lsmash_bs_put_bytes( file->bs, pool->size, pool->data );
    if( lsmash_bs_flush_buffer( file->bs ) )
        return -1;
    file->mdat->media_size  += pool->size;
    file->size              += pool->size;
    pool->sample_count = 0;
    pool->size         = 0;
    return 0;
}

static int isom_update_sample_tables( isom_trak_t *trak, lsmash_sample_t *sample, uint32_t *samples_per_packet )
{
    isom_audio_entry_t *audio = (isom_audio_entry_t *)lsmash_get_entry_data( &trak->mdia->minf->stbl->stsd->list, sample->index );
    if( (audio->manager & LSMASH_AUDIO_DESCRIPTION)
     && (audio->manager & LSMASH_QTFF_BASE)
     && (audio->version == 1)
     && (audio->compression_ID != QT_AUDIO_COMPRESSION_ID_VARIABLE_COMPRESSION) )
    {
        /* Add entries of the sample table for each uncompressed sample. */
        uint64_t sample_duration = trak->mdia->mdhd->timescale / (audio->samplerate >> 16);
        if( audio->samplesPerPacket == 0 || sample_duration == 0 )
            return -1;
        uint64_t sample_dts = sample->dts;
        uint64_t sample_cts = sample->cts;
        for( uint32_t i = 0; i < audio->samplesPerPacket; i++ )
        {
            /* Add a size of uncomressed audio and increment sample_count.
             * This points to individual uncompressed audio samples, each one byte in size, within the compressed frames. */
            uint32_t sample_count = isom_add_size( trak, 1 );
            if( sample_count == 0 )
                return -1;
            /* Add a decoding timestamp and a composition timestamp. */
            if( isom_add_timestamp( trak, sample_dts, sample_cts ) < 0 )
                return -1;
            sample_dts += sample_duration;
            sample_cts += sample_duration;
        }
        *samples_per_packet = audio->samplesPerPacket;
    }
    else
    {
        /* Add a sample_size and increment sample_count. */
        uint32_t sample_count = isom_add_size( trak, sample->length );
        if( sample_count == 0 )
            return -1;
        /* Add a decoding timestamp and a composition timestamp. */
        if( isom_add_timestamp( trak, sample->dts, sample->cts ) < 0 )
            return -1;
        /* Add a sync point if needed. */
        if( isom_add_sync_point( trak, sample_count, &sample->prop ) < 0 )
            return -1;
        /* Add a partial sync point if needed. */
        if( isom_add_partial_sync( trak, sample_count, &sample->prop ) < 0 )
            return -1;
        /* Add leading, independent, disposable and redundant information if needed. */
        if( isom_add_dependency_type( trak, &sample->prop ) < 0 )
            return -1;
        /* Group samples into random access point type if needed. */
        if( isom_group_random_access( (isom_box_t *)trak, sample ) < 0 )
            return -1;
        /* Group samples into random access recovery point type if needed. */
        if( isom_group_roll_recovery( (isom_box_t *)trak, sample ) < 0 )
            return -1;
        *samples_per_packet = 1;
    }
    /* Add a chunk if needed. */
    return isom_add_chunk( trak, sample );
}

static int isom_append_fragment_track_run( lsmash_file_t *file, isom_chunk_t *chunk )
{
    if( !chunk->pool || chunk->pool->size == 0 )
        return 0;
    isom_fragment_manager_t *fragment = file->fragment;
    /* Move data in the pool of the current track fragment to the pool of the current movie fragment.
     * Empty the pool of current track. We don't delete data of samples here. */
    if( lsmash_add_entry( fragment->pool, chunk->pool ) < 0 )
        return -1;
    fragment->sample_count += chunk->pool->sample_count;
    fragment->pool_size    += chunk->pool->size;
    chunk->pool = isom_create_sample_pool( chunk->pool->size );
    return chunk->pool ? 0 : -1;
}

static int isom_output_cached_chunk( isom_trak_t *trak )
{
    isom_chunk_t      *chunk          = &trak->cache->chunk;
    isom_stbl_t       *stbl           = trak->mdia->minf->stbl;
    isom_stsc_entry_t *last_stsc_data = stbl->stsc->list->tail ? (isom_stsc_entry_t *)stbl->stsc->list->tail->data : NULL;
    /* Create a new chunk sequence in this track if needed. */
    if( (!last_stsc_data
      || chunk->pool->sample_count       != last_stsc_data->samples_per_chunk
      || chunk->sample_description_index != last_stsc_data->sample_description_index)
     && isom_add_stsc_entry( stbl, chunk->chunk_number,
                                   chunk->pool->sample_count,
                                   chunk->sample_description_index ) )
        return -1;
    lsmash_file_t *file = trak->file;
    if( file->fragment )
    {
        /* Add a new chunk offset in this track. */
        if( isom_add_stco_entry( stbl, file->size + ISOM_BASEBOX_COMMON_SIZE + file->fragment->pool_size ) )
            return -1;
        return isom_append_fragment_track_run( file, chunk );
    }
    /* Add a new chunk offset in this track. */
    if( isom_add_stco_entry( stbl, file->size ) )
        return -1;
    /* Output pooled samples in this track. */
    return isom_write_pooled_samples( file, chunk->pool );
}

static int isom_pool_sample( isom_sample_pool_t *pool, lsmash_sample_t *sample, uint32_t samples_per_packet )
{
    uint64_t pool_size = pool->size + sample->length;
    if( pool->alloc < pool_size )
    {
        uint8_t *data;
        uint64_t alloc = pool_size + (1<<16);
        if( !pool->data )
            data = lsmash_malloc( alloc );
        else
            data = lsmash_realloc( pool->data, alloc );
        if( !data )
            return -1;
        pool->data  = data;
        pool->alloc = alloc;
    }
    memcpy( pool->data + pool->size, sample->data, sample->length );
    pool->size          = pool_size;
    pool->sample_count += samples_per_packet;
    lsmash_delete_sample( sample );
    return 0;
}

static int isom_append_sample_internal( isom_trak_t *trak, lsmash_sample_t *sample )
{
    uint32_t samples_per_packet;
    int flush = isom_update_sample_tables( trak, sample, &samples_per_packet );
    if( flush < 0 )
        return -1;
    /* flush == 1 means pooled samples must be flushed. */
    lsmash_file_t      *file         = trak->file;
    isom_sample_pool_t *current_pool = trak->cache->chunk.pool;
    if( flush == 1 && isom_write_pooled_samples( file, current_pool ) )
        return -1;
    /* Arbitration system between tracks with extremely scattering dts.
     * Here, we check whether asynchronization between the tracks exceeds the tolerance.
     * If a track has too old "first DTS" in its cached chunk than current sample's DTS, then its pooled samples must be flushed.
     * We don't consider presentation of media since any edit can pick an arbitrary portion of media in track.
     * Note: you needn't read this loop until you grasp the basic handling of chunks. */
    double tolerance = file->max_async_tolerance;
    for( lsmash_entry_t *entry = file->moov->trak_list.head; entry; entry = entry->next )
    {
        isom_trak_t *other = (isom_trak_t *)entry->data;
        if( trak == other )
            continue;
        if( !other
         || !other->cache
         || !other->mdia
         || !other->mdia->mdhd
         ||  other->mdia->mdhd->timescale == 0
         || !other->mdia->minf
         || !other->mdia->minf->stbl
         || !other->mdia->minf->stbl->stsc
         || !other->mdia->minf->stbl->stsc->list )
            return -1;
        isom_chunk_t *chunk = &other->cache->chunk;
        if( !chunk->pool || chunk->pool->sample_count == 0 )
            continue;
        double diff = ((double)sample->dts      /  trak->mdia->mdhd->timescale)
                    - ((double)chunk->first_dts / other->mdia->mdhd->timescale);
        if( diff > tolerance && isom_output_cached_chunk( other ) )
            return -1;
        /* Note: we don't flush the cached chunk in the current track and the current sample here
         * even if the conditional expression of '-diff > tolerance' meets.
         * That's useless because appending a sample to another track would be a good equivalent.
         * It's even harmful because it causes excess chunk division by calling
         * isom_output_cached_chunk() which always generates a new chunk.
         * Anyway some excess chunk division will be there, but rather less without it.
         * To completely avoid this, we need to observe at least whether the current sample will be placed
         * right next to the previous chunk of the same track or not. */
    }
    /* anyway the current sample must be pooled. */
    return isom_pool_sample( current_pool, sample, samples_per_packet );
}

/* This function is for non-fragmented movie. */
static int isom_append_sample( lsmash_file_t *file, uint32_t track_ID, lsmash_sample_t *sample )
{
    isom_trak_t *trak = isom_get_trak( file, track_ID );
    if( !trak
     || !trak->file
     || !trak->cache
     || !trak->mdia
     || !trak->mdia->mdhd
     ||  trak->mdia->mdhd->timescale == 0
     || !trak->mdia->minf
     || !trak->mdia->minf->stbl
     || !trak->mdia->minf->stbl->stsd
     || !trak->mdia->minf->stbl->stsc || !trak->mdia->minf->stbl->stsc->list )
        return -1;
    /* If there is no available Media Data Box to write samples, add and write a new one before any chunk offset is decided. */
    if( !file->mdat )
    {
        if( isom_add_mdat( file ) )
            return -1;
        file->mdat->manager |= LSMASH_PLACEHOLDER;
        if( isom_write_box( file->bs, (isom_box_t *)file->mdat ) < 0 )
            return -1;
        assert( file->free );
        file->size += file->free->size + file->mdat->size;
    }
    isom_sample_entry_t *sample_entry = (isom_sample_entry_t *)lsmash_get_entry_data( &trak->mdia->minf->stbl->stsd->list, sample->index );
    if( !sample_entry )
        return -1;
    if( isom_is_lpcm_audio( sample_entry ) )
    {
        uint32_t frame_size = ((isom_audio_entry_t *)sample_entry)->constBytesPerAudioPacket;
        if( sample->length == frame_size )
            return isom_append_sample_internal( trak, sample );
        else if( sample->length < frame_size )
            return -1;
        /* Append samples splitted into each LPCMFrame. */
        uint64_t dts = sample->dts;
        uint64_t cts = sample->cts;
        for( uint32_t offset = 0; offset < sample->length; offset += frame_size )
        {
            lsmash_sample_t *lpcm_sample = lsmash_create_sample( frame_size );
            if( !lpcm_sample )
                return -1;
            memcpy( lpcm_sample->data, sample->data + offset, frame_size );
            lpcm_sample->dts   = dts++;
            lpcm_sample->cts   = cts++;
            lpcm_sample->prop  = sample->prop;
            lpcm_sample->index = sample->index;
            if( isom_append_sample_internal( trak, lpcm_sample ) )
            {
                lsmash_delete_sample( lpcm_sample );
                return -1;
            }
        }
        lsmash_delete_sample( sample );
        return 0;
    }
    return isom_append_sample_internal( trak, sample );
}

static int isom_output_cache( isom_trak_t *trak )
{
    isom_cache_t *cache = trak->cache;
    if( cache->chunk.pool
     && cache->chunk.pool->sample_count
     && isom_output_cached_chunk( trak ) < 0 )
        return -1;
    isom_stbl_t *stbl = trak->mdia->minf->stbl;
    for( lsmash_entry_t *entry = stbl->sgpd_list.head; entry; entry = entry->next )
    {
        isom_sgpd_t *sgpd = (isom_sgpd_t *)entry->data;
        if( !sgpd )
            return -1;
        switch( sgpd->grouping_type )
        {
            case ISOM_GROUP_TYPE_RAP :
            {
                isom_rap_group_t *group = cache->rap;
                if( !group )
                {
                    if( stbl->file->fragment )
                        continue;
                    else
                        return -1;
                }
                if( !group->random_access )
                    continue;
                group->random_access->num_leading_samples_known = 1;
                break;
            }
            case ISOM_GROUP_TYPE_ROLL :
                if( !cache->roll.pool )
                {
                    if( stbl->file->fragment )
                        continue;
                    else
                        return -1;
                }
                isom_sbgp_t *sbgp = isom_get_sample_to_group( stbl, ISOM_GROUP_TYPE_ROLL );
                if( !sbgp || isom_all_recovery_completed( sbgp, cache->roll.pool ) < 0 )
                    return -1;
                break;
            default :
                break;
        }
    }
    return 0;
}

static int isom_output_fragment_cache( isom_traf_t *traf )
{
    isom_cache_t *cache = traf->cache;
    if( isom_append_fragment_track_run( traf->file, &cache->chunk ) < 0 )
        return -1;
    for( lsmash_entry_t *entry = traf->sgpd_list.head; entry; entry = entry->next )
    {
        isom_sgpd_t *sgpd = (isom_sgpd_t *)entry->data;
        if( !sgpd )
            return -1;
        switch( sgpd->grouping_type )
        {
            case ISOM_GROUP_TYPE_RAP :
            {
                isom_rap_group_t *group = cache->rap;
                if( !group )
                {
                    if( traf->file->fragment )
                        continue;
                    else
                        return -1;
                }
                if( !group->random_access )
                    continue;
                group->random_access->num_leading_samples_known = 1;
                break;
            }
            case ISOM_GROUP_TYPE_ROLL :
                if( !cache->roll.pool )
                {
                    if( traf->file->fragment )
                        continue;
                    else
                        return -1;
                }
                isom_sbgp_t *sbgp = isom_get_fragment_sample_to_group( traf, ISOM_GROUP_TYPE_ROLL );
                if( !sbgp || isom_all_recovery_completed( sbgp, cache->roll.pool ) < 0 )
                    return -1;
                break;
            default :
                break;
        }
    }
    return 0;
}

static int isom_flush_fragment_pooled_samples( lsmash_file_t *file, uint32_t track_ID, uint32_t last_sample_duration )
{
    isom_traf_t *traf = isom_get_traf( file->fragment->movie, track_ID );
    if( !traf )
        return 0;   /* no samples */
    if( !traf->cache
     || !traf->cache->fragment )
        return -1;
    if( traf->trun_list.entry_count
     && traf->trun_list.tail
     && traf->trun_list.tail->data )
    {
        /* Media Data Box preceded by Movie Fragment Box could change base_data_offsets in each track fragments later.
         * We can't consider this here because the length of Movie Fragment Box is unknown at this step yet. */
        isom_trun_t *trun = (isom_trun_t *)traf->trun_list.tail->data;
        if( file->fragment->pool_size )
            trun->flags |= ISOM_TR_FLAGS_DATA_OFFSET_PRESENT;
        trun->data_offset = file->fragment->pool_size;
    }
    if( isom_output_fragment_cache( traf ) < 0 )
        return -1;
    return isom_set_fragment_last_duration( traf, last_sample_duration );
}

int lsmash_flush_pooled_samples( lsmash_root_t *root, uint32_t track_ID, uint32_t last_sample_delta )
{
    if( !root || !root->file )
        return -1;
    lsmash_file_t *file = root->file;
    if( file->fragment
     && file->fragment->movie )
        return isom_flush_fragment_pooled_samples( file, track_ID, last_sample_delta );
    isom_trak_t *trak = isom_get_trak( file, track_ID );
    if( !trak
     || !trak->cache
     || !trak->mdia
     || !trak->mdia->minf
     || !trak->mdia->minf->stbl
     || !trak->mdia->minf->stbl->stsc
     || !trak->mdia->minf->stbl->stsc->list )
        return -1;
    if( isom_output_cache( trak ) < 0 )
        return -1;
    return lsmash_set_last_sample_delta( root, track_ID, last_sample_delta );
}

/* This function doesn't update sample_duration of the last sample in the previous movie fragment.
 * Instead of this, isom_finish_movie_fragment undertakes this task. */
static int isom_update_fragment_previous_sample_duration( isom_traf_t *traf, isom_trex_t *trex, uint32_t duration )
{
    isom_tfhd_t *tfhd = traf->tfhd;
    isom_trun_t *trun = (isom_trun_t *)traf->trun_list.tail->data;
    int previous_run_has_previous_sample = 0;
    if( trun->sample_count == 1 )
    {
        if( traf->trun_list.entry_count == 1 )
            return 0;       /* The previous track run belongs to the previous movie fragment if it exists. */
        if( !traf->trun_list.tail->prev
         || !traf->trun_list.tail->prev->data )
            return -1;
        /* OK. The previous sample exists in the previous track run in the same track fragment. */
        trun = (isom_trun_t *)traf->trun_list.tail->prev->data;
        previous_run_has_previous_sample = 1;
    }
    /* Update default_sample_duration of the Track Fragment Header Box
     * if this duration is what the first sample in the current track fragment owns. */
    if( (trun->sample_count == 2 && traf->trun_list.entry_count == 1)
     || (trun->sample_count == 1 && traf->trun_list.entry_count == 2) )
    {
        if( duration != trex->default_sample_duration )
            tfhd->flags |= ISOM_TF_FLAGS_DEFAULT_SAMPLE_DURATION_PRESENT;
        tfhd->default_sample_duration = duration;
    }
    /* Update the previous sample_duration if needed. */
    if( duration != tfhd->default_sample_duration )
        trun->flags |= ISOM_TR_FLAGS_SAMPLE_DURATION_PRESENT;
    if( trun->flags )
    {
        uint32_t sample_number = trun->sample_count - !previous_run_has_previous_sample;
        isom_trun_optional_row_t *row = isom_request_trun_optional_row( trun, tfhd, sample_number );
        if( !row )
            return -1;
        row->sample_duration = duration;
    }
    traf->cache->fragment->last_duration = duration;
    return 0;
}

static isom_sample_flags_t isom_generate_fragment_sample_flags( lsmash_sample_t *sample )
{
    isom_sample_flags_t flags;
    flags.reserved                    = 0;
    flags.is_leading                  = sample->prop.leading     & 0x3;
    flags.sample_depends_on           = sample->prop.independent & 0x3;
    flags.sample_is_depended_on       = sample->prop.disposable  & 0x3;
    flags.sample_has_redundancy       = sample->prop.redundant   & 0x3;
    flags.sample_padding_value        = 0;
    flags.sample_is_non_sync_sample   = !(sample->prop.ra_flags & ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC);
    flags.sample_degradation_priority = 0;
    return flags;
}

static int isom_update_fragment_sample_tables( isom_traf_t *traf, lsmash_sample_t *sample )
{
    isom_tfhd_t *tfhd = traf->tfhd;
    isom_trex_t *trex = isom_get_trex( traf->file->moov->mvex, tfhd->track_ID );
    if( !trex )
        return -1;
    lsmash_file_t *file    = traf->file;
    isom_cache_t  *cache   = traf->cache;
    isom_chunk_t  *current = &cache->chunk;
    if( !current->pool )
    {
        /* Very initial settings, just once per track */
        current->pool = isom_create_sample_pool( 0 );
        if( !current->pool )
            return -1;
    }
    /* Create a new track run if the duration exceeds max_chunk_duration.
     * Old one will be appended to the pool of this movie fragment. */
    uint32_t media_timescale = lsmash_get_media_timescale( file->root, tfhd->track_ID );
    if( !media_timescale )
        return -1;
    int delimit = (file->max_chunk_duration < ((double)(sample->dts - current->first_dts) / media_timescale))
               || (file->max_chunk_size < (current->pool->size + sample->length));
    isom_trun_t *trun = NULL;
    if( !traf->trun_list.entry_count || delimit )
    {
        if( delimit
         && traf->trun_list.entry_count
         && traf->trun_list.tail
         && traf->trun_list.tail->data )
        {
            /* Media Data Box preceded by Movie Fragment Box could change base data offsets in each track fragments later.
             * We can't consider this here because the length of Movie Fragment Box is unknown at this step yet. */
            trun = (isom_trun_t *)traf->trun_list.tail->data;
            if( file->fragment->pool_size )
                trun->flags |= ISOM_TR_FLAGS_DATA_OFFSET_PRESENT;
            trun->data_offset = file->fragment->pool_size;
        }
        trun = isom_add_trun( traf );
        if( !trun )
            return -1;
    }
    else
    {
        if( !traf->trun_list.tail
         || !traf->trun_list.tail->data )
            return -1;
        trun = (isom_trun_t *)traf->trun_list.tail->data;
    }
    isom_sample_flags_t sample_flags = isom_generate_fragment_sample_flags( sample );
    if( ++trun->sample_count == 1 )
    {
        if( traf->trun_list.entry_count == 1 )
        {
            /* This track fragment isn't empty-duration-fragment any more. */
            tfhd->flags &= ~ISOM_TF_FLAGS_DURATION_IS_EMPTY;
            /* Set up sample_description_index in this track fragment. */
            if( sample->index != trex->default_sample_description_index )
                tfhd->flags |= ISOM_TF_FLAGS_SAMPLE_DESCRIPTION_INDEX_PRESENT;
            tfhd->sample_description_index = current->sample_description_index = sample->index;
            /* Set up default_sample_size used in this track fragment. */
            tfhd->default_sample_size = sample->length;
            /* Set up default_sample_flags used in this track fragment.
             * Note: we decide an appropriate default value at the end of this movie fragment. */
            tfhd->default_sample_flags = sample_flags;
            /* Set up random access information if this sample is a sync sample.
             * We inform only the first sample in each movie fragment. */
            if( !file->bs->unseekable && (sample->prop.ra_flags & ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC) )
            {
                isom_tfra_t *tfra = isom_get_tfra( file->mfra, tfhd->track_ID );
                if( !tfra )
                {
                    tfra = isom_add_tfra( file->mfra );
                    if( !tfra )
                        return -1;
                    tfra->track_ID = tfhd->track_ID;
                }
                if( !tfra->list )
                {
                    tfra->list = lsmash_create_entry_list();
                    if( !tfra->list )
                        return -1;
                }
                isom_tfra_location_time_entry_t *rap = lsmash_malloc( sizeof(isom_tfra_location_time_entry_t) );
                if( !rap )
                    return -1;
                rap->time          = sample->cts;   /* Set composition timestamp temporally.
                                                     * At the end of the whole movie, this will be reset as presentation time. */
                rap->moof_offset   = file->size;    /* We place Movie Fragment Box in the head of each movie fragment. */
                rap->traf_number   = cache->fragment->traf_number;
                rap->trun_number   = traf->trun_list.entry_count;
                rap->sample_number = trun->sample_count;
                if( lsmash_add_entry( tfra->list, rap ) )
                {
                    lsmash_free( rap );
                    return -1;
                }
                tfra->number_of_entry = tfra->list->entry_count;
                int length;
                for( length = 1; rap->traf_number >> (length * 8); length++ );
                tfra->length_size_of_traf_num = LSMASH_MAX( length - 1, tfra->length_size_of_traf_num );
                for( length = 1; rap->traf_number >> (length * 8); length++ );
                tfra->length_size_of_trun_num = LSMASH_MAX( length - 1, tfra->length_size_of_trun_num );
                for( length = 1; rap->sample_number >> (length * 8); length++ );
                tfra->length_size_of_sample_num = LSMASH_MAX( length - 1, tfra->length_size_of_sample_num );
            }
            /* Set up the base media decode time of this track fragment.
             * This feature is available under ISO Base Media version 6 or later.
             * For DASH Media Segment, each Track Fragment Box shall contain a Track Fragment Base Media Decode Time Box. */
            if( file->max_isom_version >= 6 || file->media_segment )
            {
                assert( !traf->tfdt );
                if( isom_add_tfdt( traf ) )
                    return -1;
                if( sample->dts > UINT32_MAX )
                    traf->tfdt->version = 1;
                traf->tfdt->baseMediaDecodeTime = sample->dts;
            }
        }
        trun->first_sample_flags = sample_flags;
        current->first_dts = sample->dts;
    }
    /* Update the optional rows in the current track run except for sample_duration if needed. */
    if( sample->length != tfhd->default_sample_size )
        trun->flags |= ISOM_TR_FLAGS_SAMPLE_SIZE_PRESENT;
    if( isom_compare_sample_flags( &sample_flags, &tfhd->default_sample_flags ) )
        trun->flags |= ISOM_TR_FLAGS_SAMPLE_FLAGS_PRESENT;
    uint32_t sample_composition_time_offset = sample->cts - sample->dts;
    if( sample_composition_time_offset )
    {
        trun->flags |= ISOM_TR_FLAGS_SAMPLE_COMPOSITION_TIME_OFFSET_PRESENT;
        /* Check if negative composition time offset is present. */
        isom_timestamp_t *ts_cache = &cache->timestamp;
        if( (sample->cts + ts_cache->ctd_shift) < sample->dts )
        {
            if( file->max_isom_version < 6 )
                return -1;  /* Negative composition time offset is not supported. */
            if( (sample->dts - sample->cts) > INT32_MAX )
                return -1;  /* Overflow */
            ts_cache->ctd_shift = sample->dts - sample->cts;
            if( trun->version == 0 && file->max_isom_version >= 6 )
                trun->version = 1;
        }
    }
    if( trun->flags )
    {
        isom_trun_optional_row_t *row = isom_request_trun_optional_row( trun, tfhd, trun->sample_count );
        if( !row )
            return -1;
        row->sample_size                    = sample->length;
        row->sample_flags                   = sample_flags;
        row->sample_composition_time_offset = sample_composition_time_offset;
    }
    if( isom_group_random_access( (isom_box_t *)traf, sample ) < 0
     || isom_group_roll_recovery( (isom_box_t *)traf, sample ) < 0 )
        return -1;
    /* Set up the previous sample_duration if this sample is not the first sample in the overall movie. */
    if( cache->fragment->has_samples )
    {
        /* Note: when using for live streaming, it is not good idea to return error (-1) by sample->dts < prev_dts
         * since that's trivial for such semi-permanent presentation. */
        uint64_t prev_dts = cache->timestamp.dts;
        if( sample->dts <= prev_dts
         || sample->dts >  prev_dts + UINT32_MAX )
            return -1;
        uint32_t sample_duration = sample->dts - prev_dts;
        if( isom_update_fragment_previous_sample_duration( traf, trex, sample_duration ) )
            return -1;
    }
    /* Cache */
    cache->timestamp.dts         = sample->dts;
    cache->timestamp.cts         = sample->cts;
    cache->fragment->largest_cts = LSMASH_MAX( sample->cts, cache->fragment->largest_cts );
    isom_subsegment_t *subsegment = &cache->fragment->subsegment;
    if( trun->sample_count == 1 && traf->trun_list.entry_count == 1 )
    {
        subsegment->first_cts    = sample->cts;
        subsegment->largest_cts  = sample->cts;
        subsegment->smallest_cts = sample->cts;
    }
    else
    {
        subsegment->largest_cts  = LSMASH_MAX( sample->cts, subsegment->largest_cts );
        subsegment->smallest_cts = LSMASH_MIN( sample->cts, subsegment->smallest_cts );
    }
    if( subsegment->first_ra_flags == ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE )
    {
        subsegment->first_ra_flags  = sample->prop.ra_flags;
        subsegment->first_ra_number = cache->fragment->sample_count + 1;
        if( sample->prop.ra_flags & (ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC | ISOM_SAMPLE_RANDOM_ACCESS_FLAG_RAP) )
        {
            subsegment->first_rp_number = subsegment->first_ra_number;
            subsegment->first_rp_cts    = sample->cts;
            subsegment->first_ed_cts    = sample->cts;
            subsegment->decodable       = 1;
        }
    }
    else if( subsegment->decodable )
    {
        if( (subsegment->first_ra_flags & (ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC | ISOM_SAMPLE_RANDOM_ACCESS_FLAG_RAP))
          ? (sample->prop.leading == ISOM_SAMPLE_IS_DECODABLE_LEADING)
          : (subsegment->first_ra_flags & ISOM_SAMPLE_RANDOM_ACCESS_FLAG_POST_ROLL_START) )
            subsegment->first_ed_cts = LSMASH_MIN( sample->cts, subsegment->first_ed_cts );
        else
            subsegment->decodable = 0;
    }
    return delimit;
}

static int isom_append_fragment_sample_internal_initial( isom_trak_t *trak, lsmash_sample_t *sample )
{
    /* Update the sample tables of this track fragment.
     * If a new chunk was created, append the previous one to the pool of this movie fragment. */
    uint32_t samples_per_packet;
    int delimit = isom_update_sample_tables( trak, sample, &samples_per_packet );
    if( delimit < 0 )
        return -1;
    else if( delimit == 1 )
        isom_append_fragment_track_run( trak->file, &trak->cache->chunk );
    /* Add a new sample into the pool of this track fragment. */
    if( isom_pool_sample( trak->cache->chunk.pool, sample, samples_per_packet ) )
        return -1;
    trak->cache->fragment->has_samples   = 1;
    trak->cache->fragment->sample_count += 1;
    return 0;
}

static int isom_append_fragment_sample_internal( isom_traf_t *traf, lsmash_sample_t *sample )
{
    /* Update the sample tables of this track fragment.
     * If a new track run was created, append the previous one to the pool of this movie fragment. */
    int delimit = isom_update_fragment_sample_tables( traf, sample );
    if( delimit < 0 )
        return -1;
    else if( delimit == 1 )
        isom_append_fragment_track_run( traf->file, &traf->cache->chunk );
    /* Add a new sample into the pool of this track fragment. */
    if( isom_pool_sample( traf->cache->chunk.pool, sample, 1 ) )
        return -1;
    traf->cache->fragment->has_samples   = 1;
    traf->cache->fragment->sample_count += 1;
    return 0;
}

static int isom_append_fragment_sample( lsmash_file_t *file, uint32_t track_ID, lsmash_sample_t *sample )
{
    isom_fragment_manager_t *fragment = file->fragment;
    if( !fragment || !fragment->pool )
        return -1;
    isom_trak_t *trak = isom_get_trak( file, track_ID );
    if( !trak
     || !trak->file
     || !trak->cache
     || !trak->cache->fragment
     || !trak->tkhd
     || !trak->mdia
     || !trak->mdia->mdhd
     ||  trak->mdia->mdhd->timescale == 0
     || !trak->mdia->minf
     || !trak->mdia->minf->stbl
     || !trak->mdia->minf->stbl->stsd
     || !trak->mdia->minf->stbl->stsc || !trak->mdia->minf->stbl->stsc->list )
        return -1;
    int (*append_sample_func)( void *, lsmash_sample_t * ) = NULL;
    void *track_fragment = NULL;
    if( !fragment->movie )
    {
        append_sample_func = (int (*)( void *, lsmash_sample_t * ))isom_append_fragment_sample_internal_initial;
        track_fragment = trak;
    }
    else
    {
        isom_traf_t *traf = isom_get_traf( fragment->movie, track_ID );
        if( !traf )
        {
            traf = isom_add_traf( fragment->movie );
            if( isom_add_tfhd( traf ) )
                return -1;
            traf->tfhd->flags                  = ISOM_TF_FLAGS_DURATION_IS_EMPTY; /* no samples for this track fragment yet */
            traf->tfhd->track_ID               = trak->tkhd->track_ID;
            traf->cache                        = trak->cache;
            traf->cache->fragment->traf_number = fragment->movie->traf_list.entry_count;
            if( (traf->cache->fragment->rap_grouping  && isom_add_sample_grouping( (isom_box_t *)traf, ISOM_GROUP_TYPE_RAP  ) < 0)
             || (traf->cache->fragment->roll_grouping && isom_add_sample_grouping( (isom_box_t *)traf, ISOM_GROUP_TYPE_ROLL ) < 0) )
                return -1;
        }
        else if( !traf->file
              || !traf->file->moov
              || !traf->file->moov->mvex
              || !traf->cache
              || !traf->tfhd )
            return -1;
        append_sample_func = (int (*)( void *, lsmash_sample_t * ))isom_append_fragment_sample_internal;
        track_fragment = traf;
    }
    isom_sample_entry_t *sample_entry = (isom_sample_entry_t *)lsmash_get_entry_data( &trak->mdia->minf->stbl->stsd->list, sample->index );
    if( !sample_entry )
        return -1;
    if( isom_is_lpcm_audio( sample_entry ) )
    {
        uint32_t frame_size = ((isom_audio_entry_t *)sample_entry)->constBytesPerAudioPacket;
        if( sample->length == frame_size )
            return append_sample_func( track_fragment, sample );
        else if( sample->length < frame_size )
            return -1;
        /* Append samples splitted into each LPCMFrame. */
        uint64_t dts = sample->dts;
        uint64_t cts = sample->cts;
        for( uint32_t offset = 0; offset < sample->length; offset += frame_size )
        {
            lsmash_sample_t *lpcm_sample = lsmash_create_sample( frame_size );
            if( !lpcm_sample )
                return -1;
            memcpy( lpcm_sample->data, sample->data + offset, frame_size );
            lpcm_sample->dts   = dts++;
            lpcm_sample->cts   = cts++;
            lpcm_sample->prop  = sample->prop;
            lpcm_sample->index = sample->index;
            if( append_sample_func( track_fragment, lpcm_sample ) )
            {
                lsmash_delete_sample( lpcm_sample );
                return -1;
            }
        }
        lsmash_delete_sample( sample );
        return 0;
    }
    return append_sample_func( track_fragment, sample );
}

int lsmash_append_sample( lsmash_root_t *root, uint32_t track_ID, lsmash_sample_t *sample )
{
    if( !root )
        return -1;
    lsmash_file_t *file = root->file;
    /* We think max_chunk_duration == 0, which means all samples will be cached on memory, should be prevented.
     * This means removal of a feature that we used to have, but anyway very alone chunk does not make sense. */
    if( !file
     || !file->bs
     || !sample
     || !sample->data
     || track_ID                  == 0
     || file->max_chunk_duration  == 0
     || file->max_async_tolerance == 0 )
        return -1;
    /* Write File Type Box here if it was not written yet. */
    if( file->ftyp && !(file->ftyp->manager & LSMASH_WRITTEN_BOX) )
    {
        if( isom_write_box( file->bs, (isom_box_t *)file->ftyp ) )
            return -1;
        file->size += file->ftyp->size;
    }
    if( file->fragment
     && file->fragment->pool )
        return isom_append_fragment_sample( file, track_ID, sample );
    return isom_append_sample( file, track_ID, sample );
}

/*---- misc functions ----*/

int lsmash_delete_explicit_timeline_map( lsmash_root_t *root, uint32_t track_ID )
{
    if( !root )
        return -1;
    isom_trak_t *trak = isom_get_trak( root->file, track_ID );
    if( !trak )
        return -1;
    isom_remove_box_by_itself( trak->edts );
    return isom_update_tkhd_duration( trak );
}

void lsmash_delete_tyrant_chapter( lsmash_root_t *root )
{
    if( !root
     || !root->file
     || !root->file->moov
     || !root->file->moov->udta )
        return;
    isom_remove_box_by_itself( root->file->moov->udta->chpl );
}

int lsmash_set_copyright( lsmash_root_t *root, uint32_t track_ID, uint16_t ISO_language, char *notice )
{
    if( !root )
        return -1;
    lsmash_file_t *file = root->file;
    if( !file
     || !file->moov
     || !file->isom_compatible
     || (ISO_language && ISO_language < 0x800)
     || !notice )
        return -1;
    isom_udta_t *udta;
    if( track_ID )
    {
        isom_trak_t *trak = isom_get_trak( file, track_ID );
        if( !trak || (!trak->udta && isom_add_udta( trak )) )
            return -1;
        udta = trak->udta;
    }
    else
    {
        if( !file->moov->udta && isom_add_udta( file->moov ) )
            return -1;
        udta = file->moov->udta;
    }
    assert( udta );
    for( lsmash_entry_t *entry = udta->cprt_list.head; entry; entry = entry->next )
    {
        isom_cprt_t *cprt = (isom_cprt_t *)entry->data;
        if( !cprt || cprt->language == ISO_language )
            return -1;
    }
    if( isom_add_cprt( udta ) )
        return -1;
    isom_cprt_t *cprt = (isom_cprt_t *)udta->cprt_list.tail->data;
    cprt->language      = ISO_language;
    cprt->notice_length = strlen( notice ) + 1;
    cprt->notice        = lsmash_memdup( notice, cprt->notice_length );
    return 0;
}
