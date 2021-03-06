#include "meta.h"
#include "../coding/coding.h"

/* AL" - headerless a-law, found in Conquest of Elysium 3 (PC) */
VGMSTREAM * init_vgmstream_pc_al2(STREAMFILE *streamFile) {
    VGMSTREAM * vgmstream = NULL;
    off_t start_offset;
    int loop_flag = 0, channel_count;


    if ( !check_extensions(streamFile,"al2"))
        goto fail;

    channel_count = 2;

    /* build the VGMSTREAM */
    vgmstream = allocate_vgmstream(channel_count,loop_flag);
    if (!vgmstream) goto fail;

    vgmstream->sample_rate = 22050;
    vgmstream->coding_type = coding_ALAW;
    vgmstream->layout_type = layout_interleave;
    vgmstream->interleave_block_size = 0x01;
    vgmstream->meta_type = meta_PC_AL2;
    vgmstream->num_samples = pcm_bytes_to_samples(get_streamfile_size(streamFile), channel_count, 8);
    if (loop_flag) {
        vgmstream->loop_start_sample = 0;
        vgmstream->loop_end_sample = vgmstream->num_samples;
    }

    start_offset = 0;

    if ( !vgmstream_open_stream(vgmstream, streamFile, start_offset) )
        goto fail;
    return vgmstream;

fail:
    close_vgmstream(vgmstream);
    return NULL;
}
