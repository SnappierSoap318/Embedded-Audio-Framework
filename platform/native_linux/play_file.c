#include <eaf/eaf_file.h>
#include <eaf/eaf_player.h>
#include <eaf/eaf_sink_null.h>
#include <eaf/eaf_wav.h>
#include <stdio.h>
#ifdef EAF_HAVE_ALSA
#include <eaf/eaf_sink_alsa.h>
static eaf_alsa_sink_ctx_t alsa;
#endif
static eaf_file_t file;
static eaf_wav_t wav;
static eaf_source_t source;
static eaf_player_t player;
static eaf_pipeline_t pipeline;
static eaf_reservoir_t reservoir;
static int32_t storage[16384u * 2u];
static eaf_null_sink_ctx_t sink_ctx;
static eaf_sink_t sink = {&eaf_null_sink_ops, &sink_ctx};
static eaf_volume_ctx_t volume = {{INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX}};
static eaf_node_t master = {"volume", EAF_NODE_STAGE_POST_PROCESS, &eaf_volume_ops, &volume};
static eaf_node_t *const nodes[] = {&master};
int main(int argc, char **argv) {
    if (argc != 2 && argc != 3) {
        fprintf(stderr, "Usage: %s file.wav [ALSA_DEVICE] (default: timed null)\n", argv[0]);
        return 2;
    }
    if (argc == 3) {
#ifdef EAF_HAVE_ALSA
        alsa.device = argv[2];
        sink = (eaf_sink_t){&eaf_alsa_sink_ops, &alsa};
#else
        fprintf(stderr, "ALSA support was not built\n");
        return 2;
#endif
    }
    eaf_reader_t reader;
    eaf_pipeline_config_t config = {&reservoir, nodes, 1, &sink};
    int rc = hal_file_open(&file, &reader, argv[1]);
    if (!rc)
        rc = eaf_wav_open(&wav, &source, &reader);
    if (!rc)
        rc = eaf_reservoir_init(&reservoir, storage, 16384, source.format, 8192);
    if (!rc)
        rc = eaf_pipeline_init(&pipeline, &config);
    if (!rc)
        rc = eaf_pipeline_configure(&pipeline, 128);
    if (!rc)
        rc = eaf_player_init(&player, &pipeline, &source, &volume);
    if (!rc)
        rc = eaf_player_start(&player);
    while (!rc)
        rc = eaf_player_step(&player);
    if (rc == EAF_EOF) {
        printf("Read %llu/%llu source frames at %u Hz, channels=%u, underruns=%u\n",
               (unsigned long long)reservoir.frames_read, (unsigned long long)source.total_frames,
               source.format.sample_rate, (unsigned)source.format.num_channels,
               reservoir.underruns);
        rc = EAF_OK;
    }
    if (player.initialized) {
        int cleanup = eaf_player_deinit(&player);
        if (!rc)
            rc = cleanup;
    }
    {
        int cleanup = eaf_pipeline_deinit(&pipeline);
        if (!rc)
            rc = cleanup;
    }
    hal_file_close(&file);
    if (rc)
        fprintf(stderr, "Playback failed: %d\n", rc);
    return rc ? 1 : 0;
}
