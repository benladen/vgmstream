#include "../vgmstream.h"

#ifdef VGM_USE_VORBIS
#include <stdio.h>
#include <string.h>
#include "meta.h"
#include <vorbis/vorbisfile.h>

#define OGG_DEFAULT_BITSTREAM 0

static size_t ov_read_func(void *ptr, size_t size, size_t nmemb, void * datasource) {
    ogg_vorbis_streamfile * const ov_streamfile = datasource;
    size_t bytes_read, items_read;

    off_t real_offset = ov_streamfile->start + ov_streamfile->offset;
    size_t max_bytes = size * nmemb;

    /* clamp for virtual filesize */
    if (max_bytes > ov_streamfile->size - ov_streamfile->offset)
        max_bytes = ov_streamfile->size - ov_streamfile->offset;

    bytes_read = read_streamfile(ptr, real_offset, max_bytes, ov_streamfile->streamfile);
    items_read = bytes_read / size;

    /* may be encrypted */
    if (ov_streamfile->decryption_callback) {
        ov_streamfile->decryption_callback(ptr, size, items_read, ov_streamfile);
    }

    ov_streamfile->offset += items_read * size;

    return items_read;
}

static int ov_seek_func(void *datasource, ogg_int64_t offset, int whence) {
    ogg_vorbis_streamfile * const ov_streamfile = datasource;
    ogg_int64_t base_offset, new_offset;

    switch (whence) {
        case SEEK_SET:
            base_offset = 0;
            break;
        case SEEK_CUR:
            base_offset = ov_streamfile->offset;
            break;
        case SEEK_END:
            base_offset = ov_streamfile->size;
            break;
        default:
            return -1;
            break;
    }


    new_offset = base_offset + offset;
    if (new_offset < 0 || new_offset > ov_streamfile->size) {
        return -1; /* *must* return -1 if stream is unseekable */
    } else {
        ov_streamfile->offset = new_offset;
        return 0;
    }
}

static long ov_tell_func(void * datasource) {
    ogg_vorbis_streamfile * const ov_streamfile = datasource;
    return ov_streamfile->offset;
}

static int ov_close_func(void * datasource) {
    /* needed as setting ov_close_func in ov_callbacks to NULL doesn't seem to work
     * (closing the streamfile is done in free_ogg_vorbis) */
    return 0;
}


static void um3_ogg_decryption_callback(void *ptr, size_t size, size_t nmemb, void *datasource) {
    size_t bytes_read = size*nmemb;
    ogg_vorbis_streamfile * const ov_streamfile = datasource;
    int i;

    /* first 0x800 bytes are xor'd with 0xff */
    if (ov_streamfile->offset < 0x800) {
        int num_crypt = 0x800 - ov_streamfile->offset;
        if (num_crypt > bytes_read)
            num_crypt = bytes_read;

        for (i = 0; i < num_crypt; i++)
            ((uint8_t*)ptr)[i] ^= 0xff;
    }
}

static void kovs_ogg_decryption_callback(void *ptr, size_t size, size_t nmemb, void *datasource) {
    size_t bytes_read = size*nmemb;
    ogg_vorbis_streamfile * const ov_streamfile = datasource;
    int i;

    /* first 0x100 bytes are xor'd with offset */
    if (ov_streamfile->offset < 0x100) {
        int max_offset = ov_streamfile->offset + bytes_read;
        if (max_offset > 0x100)
            max_offset = 0x100;

        for (i = ov_streamfile->offset; i < max_offset; i++) {
            ((uint8_t*)ptr)[i-ov_streamfile->offset] ^= i;
        }
    }
}

static void psychic_ogg_decryption_callback(void *ptr, size_t size, size_t nmemb, void *datasource) {
    size_t bytes_read = size*nmemb;
    int i;

    /* add 0x23 ('#') */
    {
        for (i = 0; i < bytes_read; i++)
            ((uint8_t*)ptr)[i] += 0x23;
    }
}

static void sngw_ogg_decryption_callback(void *ptr, size_t size, size_t nmemb, void *datasource) {
    size_t bytes_read = size*nmemb;
    ogg_vorbis_streamfile * const ov_streamfile = datasource;
    int i;
    char *header_id = "OggS";
    uint8_t key[4];

    put_32bitBE(key, ov_streamfile->sngw_xor);

    /* bytes are xor'd with key and nibble-swapped */
    {
        for (i = 0; i < bytes_read; i++) {
            if (ov_streamfile->offset+i < 0x04) {
                /* replace key in the first 4 bytes with "OggS" */
                ((uint8_t*)ptr)[i] = (uint8_t)header_id[(ov_streamfile->offset + i) % 4];
            }
            else {
                uint8_t val = ((uint8_t*)ptr)[i] ^ key[(ov_streamfile->offset + i) % 4];
                ((uint8_t*)ptr)[i] = ((val << 4) & 0xf0) | ((val >> 4) & 0x0f);
            }
        }
    }
}

static void isd_ogg_decryption_callback(void *ptr, size_t size, size_t nmemb, void *datasource) {
    static const uint8_t key[16] = {
            0xe0,0x00,0xe0,0x00,0xa0,0x00,0x00,0x00,0xe0,0x00,0xe0,0x80,0x40,0x40,0x40,0x00
    };
    size_t bytes_read = size*nmemb;
    ogg_vorbis_streamfile * const ov_streamfile = datasource;
    int i;

    /* bytes are xor'd with key */
    {
        for (i = 0; i < bytes_read; i++)
            ((uint8_t*)ptr)[i] ^= key[(ov_streamfile->offset + i) % 16];
    }
}


/* Ogg Vorbis, by way of libvorbisfile; may contain loop comments */
VGMSTREAM * init_vgmstream_ogg_vorbis(STREAMFILE *streamFile) {
    char filename[PATH_LIMIT];
    vgm_vorbis_info_t inf = {0};
    off_t start_offset = 0;

    int is_ogg = 0;
    int is_um3 = 0;
    int is_kovs = 0;
    int is_psychic = 0;
    int is_sngw = 0;
    int is_isd = 0;


    /* check extension */
    if (check_extensions(streamFile,"ogg,logg")) { /* .ogg: standard/psychic, .logg: renamed for plugins */
        is_ogg = 1;
    } else if (check_extensions(streamFile,"um3")) {
        is_um3 = 1;
    } else if (check_extensions(streamFile,"kvs,kovs")) { /* .kvs: Atelier Sophie (PC), kovs: header id only? */
        is_kovs = 1;
    } else if (check_extensions(streamFile,"sngw")) { /* .sngw: Devil May Cry 4 SE (PC), Biohazard 6 (PC) */
        is_sngw = 1;
    } else if (check_extensions(streamFile,"isd")) { /* .isd: Azure Striker Gunvolt (PC) */
        is_isd = 1;
    } else {
        goto fail;
    }
    streamFile->get_name(streamFile,filename,sizeof(filename));

    /* check standard Ogg Vorbis */
    if (is_ogg) {

        /* check Psychic Software obfuscation (Darkwind: War on Wheels PC) */
        if (read_32bitBE(0x00,streamFile) == 0x2c444430) {
            is_psychic = 1;
            inf.decryption_callback = psychic_ogg_decryption_callback;
        }
        else if (read_32bitBE(0x00,streamFile) != 0x4f676753) { /* "OggS" */
            goto fail; /* not known (ex. Wwise) */
        }
    }

    /* check "Ultramarine3" (???), may be encrypted */
    if (is_um3) {
        if (read_32bitBE(0x00,streamFile) != 0x4f676753) { /* "OggS" */
            inf.decryption_callback = um3_ogg_decryption_callback;
        }
    }

    /* check KOVS (Koei Tecmo games), encrypted and has an actual header */
    if (is_kovs) {
        if (read_32bitBE(0x00,streamFile) != 0x4b4f5653) { /* "KOVS" */
            goto fail;
        }
        inf.loop_start = read_32bitLE(0x08,streamFile);
        inf.loop_flag = (inf.loop_start != 0);
        inf.decryption_callback = kovs_ogg_decryption_callback;

        start_offset = 0x20;
    }

    /* check SNGW (Capcom's MT Framework PC games), may be encrypted */
    if (is_sngw) {
        if (read_32bitBE(0x00,streamFile) != 0x4f676753) { /* "OggS" */
            inf.sngw_xor = read_32bitBE(0x00,streamFile);
            inf.decryption_callback = sngw_ogg_decryption_callback;
        }
    }

    /* check ISD (Gunvolt PC) */
    if (is_isd) {
        inf.decryption_callback = isd_ogg_decryption_callback;

        //todo looping unknown, not in Ogg comments
        // game has sound/GV_steam.* files with info about sound/stream/*.isd
        //- .ish: constant id/names
        //- .isl: unknown table, maybe looping?
        //- .isf: format table, ordered like file numbers, 0x18 header with:
        //   0x00(2): ?, 0x02(2): channels, 0x04: sample rate, 0x08: skip samples (in PCM bytes), always 32000
        //   0x0c(2): PCM block size, 0x0e(2): PCM bps, 0x10: null, 0x18: samples (in PCM bytes)
    }


    if (is_um3) {
        inf.meta_type = meta_OGG_UM3;
    } else if (is_kovs) {
        inf.meta_type = meta_OGG_KOVS;
    } else if (is_psychic) {
        inf.meta_type = meta_OGG_PSYCHIC;
    } else if (is_sngw) {
        inf.meta_type = meta_OGG_SNGW;
    } else if (is_isd) {
        inf.meta_type = meta_OGG_ISD;
    } else {
        inf.meta_type = meta_OGG_VORBIS;
    }
    inf.layout_type = layout_ogg_vorbis;

    return init_vgmstream_ogg_vorbis_callbacks(streamFile, filename, NULL, start_offset, &inf);

fail:
    return NULL;
}

VGMSTREAM * init_vgmstream_ogg_vorbis_callbacks(STREAMFILE *streamFile, const char * filename, ov_callbacks *callbacks_p, off_t start, const vgm_vorbis_info_t *vgm_inf) {
    VGMSTREAM * vgmstream = NULL;
    ogg_vorbis_codec_data * data = NULL;
    OggVorbis_File *ovf = NULL;
    vorbis_info *vi;

    int loop_flag = vgm_inf->loop_flag;
    int32_t loop_start = vgm_inf->loop_start;
    int loop_length_found = vgm_inf->loop_length_found;
    int32_t loop_length = vgm_inf->loop_length;
    int loop_end_found = vgm_inf->loop_end_found;
    int32_t loop_end = vgm_inf->loop_end;
    size_t stream_size = vgm_inf->stream_size ?
            vgm_inf->stream_size :
            get_streamfile_size(streamFile) - start;

    ov_callbacks default_callbacks;

    if (!callbacks_p) {
        default_callbacks.read_func = ov_read_func;
        default_callbacks.seek_func = ov_seek_func;
        default_callbacks.close_func = ov_close_func;
        default_callbacks.tell_func = ov_tell_func;

        callbacks_p = &default_callbacks;
    }

    /* test if this is a proper Ogg Vorbis file, with the current (from init_x) STREAMFILE */
    {
        OggVorbis_File temp_ovf;
        ogg_vorbis_streamfile temp_streamfile;

        temp_streamfile.streamfile = streamFile;

        temp_streamfile.start = start;
        temp_streamfile.offset = 0;
        temp_streamfile.size = stream_size;

        temp_streamfile.decryption_callback = vgm_inf->decryption_callback;
        temp_streamfile.scd_xor = vgm_inf->scd_xor;
        temp_streamfile.scd_xor_length = vgm_inf->scd_xor_length;
        temp_streamfile.sngw_xor = vgm_inf->sngw_xor;

        /* open the ogg vorbis file for testing */
        memset(&temp_ovf, 0, sizeof(temp_ovf));
        if (ov_test_callbacks(&temp_streamfile, &temp_ovf, NULL, 0, *callbacks_p))
            goto fail;

        /* we have to close this as it has the init_vgmstream meta-reading STREAMFILE */
        ov_clear(&temp_ovf);
    }


    /* proceed to init codec_data and reopen a STREAMFILE for this stream */
    {
        data = calloc(1,sizeof(ogg_vorbis_codec_data));
        if (!data) goto fail;

        data->ov_streamfile.streamfile = streamFile->open(streamFile,filename, STREAMFILE_DEFAULT_BUFFER_SIZE);
        if (!data->ov_streamfile.streamfile) goto fail;

        data->ov_streamfile.start = start;
        data->ov_streamfile.offset = 0;
        data->ov_streamfile.size = stream_size;

        data->ov_streamfile.decryption_callback = vgm_inf->decryption_callback;
        data->ov_streamfile.scd_xor = vgm_inf->scd_xor;
        data->ov_streamfile.scd_xor_length = vgm_inf->scd_xor_length;
        data->ov_streamfile.sngw_xor = vgm_inf->sngw_xor;

        /* open the ogg vorbis file for real */
        if (ov_open_callbacks(&data->ov_streamfile, &data->ogg_vorbis_file, NULL, 0, *callbacks_p))
            goto fail;
        ovf = &data->ogg_vorbis_file;
    }

    /* get info from bitstream 0 */
    data->bitstream = OGG_DEFAULT_BITSTREAM;
    vi = ov_info(ovf,OGG_DEFAULT_BITSTREAM);

    /* search for loop comments */
    {
        int i;
        vorbis_comment *comment = ov_comment(ovf,OGG_DEFAULT_BITSTREAM);

        for (i = 0; i < comment->comments; i++) {
            const char * user_comment = comment->user_comments[i];
            if (strstr(user_comment,"loop_start=")==user_comment || /* PSO4 */
                strstr(user_comment,"LOOP_START=")==user_comment || /* PSO4 */
                strstr(user_comment,"COMMENT=LOOPPOINT=")==user_comment ||
                strstr(user_comment,"LOOPSTART=")==user_comment ||
                strstr(user_comment,"um3.stream.looppoint.start=")==user_comment ||
                strstr(user_comment,"LOOP_BEGIN=")==user_comment || /* Hatsune Miku: Project Diva F (PS3) */
                strstr(user_comment,"LoopStart=")==user_comment) {  /* Devil May Cry 4 (PC) */
                loop_start = atol(strrchr(user_comment,'=')+1);
                loop_flag = (loop_start >= 0);
            }
            else if (strstr(user_comment,"LOOPLENGTH=")==user_comment) {/* (LOOPSTART pair) */
                loop_length = atol(strrchr(user_comment,'=')+1);
                loop_length_found = 1;
            }
            else if (strstr(user_comment,"title=-lps")==user_comment) { /* Memories Off #5 (PC) */
                loop_start = atol(user_comment+10);
                loop_flag = (loop_start >= 0);
            }
            else if (strstr(user_comment,"album=-lpe")==user_comment) { /* (title=-lps pair) */
                loop_end = atol(user_comment+10);
                loop_flag = 1;
                loop_end_found = 1;
            }
            else if (strstr(user_comment,"LoopEnd=")==user_comment) { /* (LoopStart pair) */
                if(loop_flag) {
                    loop_length = atol(strrchr(user_comment,'=')+1)-loop_start;
                    loop_length_found = 1;
                }
            }
            else if (strstr(user_comment,"LOOP_END=")==user_comment) { /* (LOOP_BEGIN pair) */
                if(loop_flag) {
                    loop_length = atol(strrchr(user_comment,'=')+1)-loop_start;
                    loop_length_found = 1;
                }
            }
            else if (strstr(user_comment,"lp=")==user_comment) {
                sscanf(strrchr(user_comment,'=')+1,"%d,%d", &loop_start,&loop_end);
                loop_flag = 1;
                loop_end_found = 1;
            }
            else if (strstr(user_comment,"LOOPDEFS=")==user_comment) { /* Fairy Fencer F: Advent Dark Force */
                sscanf(strrchr(user_comment,'=')+1,"%d,%d", &loop_start,&loop_end);
                loop_flag = 1;
                loop_end_found = 1;
            }
            else if (strstr(user_comment,"COMMENT=loop(")==user_comment) { /* Zero Time Dilemma (PC) */
                sscanf(strrchr(user_comment,'(')+1,"%d,%d", &loop_start,&loop_end);
                loop_flag = 1;
                loop_end_found = 1;
            }
        }
    }


    /* build the VGMSTREAM */
    vgmstream = allocate_vgmstream(vi->channels,loop_flag);
    if (!vgmstream) goto fail;

    vgmstream->codec_data = data; /* store our fun extra datas */
    vgmstream->channels = vi->channels;
    vgmstream->sample_rate = vi->rate;
    vgmstream->num_streams = vgm_inf->total_subsongs;
    vgmstream->stream_size = stream_size;

    vgmstream->num_samples = ov_pcm_total(ovf,-1); /* let libvorbisfile find total samples */
    if (loop_flag) {
        vgmstream->loop_start_sample = loop_start;
        if (loop_length_found)
            vgmstream->loop_end_sample = loop_start+loop_length;
        else if (loop_end_found)
            vgmstream->loop_end_sample = loop_end;
        else
            vgmstream->loop_end_sample = vgmstream->num_samples;
        vgmstream->loop_flag = loop_flag;

        if (vgmstream->loop_end_sample > vgmstream->num_samples)
            vgmstream->loop_end_sample = vgmstream->num_samples;
    }

    vgmstream->coding_type = coding_ogg_vorbis;
    vgmstream->layout_type = vgm_inf->layout_type;
    vgmstream->meta_type = vgm_inf->meta_type;

    return vgmstream;

fail:
    /* clean up anything we may have opened */
    if (data) {
        if (ovf)
            ov_clear(&data->ogg_vorbis_file);//same as ovf
        if (data->ov_streamfile.streamfile)
            close_streamfile(data->ov_streamfile.streamfile);
        free(data);
    }
    if (vgmstream) {
        vgmstream->codec_data = NULL;
        close_vgmstream(vgmstream);
    }
    return NULL;
}

#endif
