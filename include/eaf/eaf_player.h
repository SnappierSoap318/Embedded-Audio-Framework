#pragma once
#include <eaf/eaf_control.h>
#include <eaf/eaf_dsp.h>
#include <eaf/eaf_source.h>
#define EAF_PLAYER_DECODE_FRAMES 256u
typedef struct {
    eaf_pipeline_t *pipeline;
    eaf_source_t *source;
    eaf_volume_ctx_t *volume;
    eaf_control_queue_t commands;
    eaf_thread_t decoder;
    eaf_sem_t wake;
    eaf_atomic_u32_t gate, quit, failed;
    int source_error; /* Read only after acquiring failed, or joining decoder. */
    int32_t decode_buffer[EAF_PLAYER_DECODE_FRAMES * EAF_MAX_CHANNELS];
    bool initialized;
} eaf_player_t;
/* Pass a configured pipeline and initialized seekable source with matching formats.
   Optional volume must belong to a node in that pipeline. No other producer. */
int eaf_player_init(eaf_player_t *player, eaf_pipeline_t *pipeline, eaf_source_t *source,
                    eaf_volume_ctx_t *volume);
int eaf_player_start(eaf_player_t *player);
/* Owner thread; one command maximum per step, then one audio block.
   EOF commits the final padded block and stops. Seek/stop are discontinuities:
   stop the sink before joining the producer (file reads may block on storage). */
int eaf_player_step(eaf_player_t *player);
int eaf_player_stop(eaf_player_t *player);
int eaf_player_seek(eaf_player_t *player, uint64_t frame);
int eaf_player_deinit(eaf_player_t *player);
/* Single control producer. Rejected/full commands return false, never overwrite.
   Commands persist across stop/start; owner may drain them while stopped. */
bool eaf_player_command(eaf_player_t *player, const eaf_command_t *command);
