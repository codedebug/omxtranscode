#include <stdlib.h>

#include "omx.h"
#include "packet_queue.h"
#include "video.h"

void *
encode_thread(void *ctx) {

    OMX_BUFFERHEADERTYPE *buf; // buffer taken from the OMX decoder 
    OMX_PARAM_PORTDEFINITIONTYPE resizer_config;
    OMX_PARAM_PORTDEFINITIONTYPE generic_config;
    OMX_VIDEO_PORTDEFINITIONTYPE decoder_config;
    struct decode_ctx_t *decoder_ctx = (struct decode_ctx_t *) ctx;
    struct packet_t *current_packet;
    int bytes_left;
    uint8_t *p; // points to currently copied buffer 

    // set common stuff 
    OMX_INIT_STRUCTURE(resizer_config);
    OMX_INIT_STRUCTURE(decoder_config);
    
    omx_setup_pipeline(&decoder_ctx->pipeline, OMX_VIDEO_CodingAVC);
    
    // main loop that will poll packets and render
    while (decoder_ctx->video_queue->queue_count != 0 || decoder_ctx->video_queue->queue_finished != 1) {
        current_packet = packet_queue_get_next_item(decoder_ctx->video_queue);
        p = current_packet->data;
        bytes_left = current_packet->data_length;

        while (bytes_left > 0) {

            fprintf(stderr, "OMX buffers: v: %02d/20, vcodec queue: %4d\r", omx_get_free_buffer_count(&decoder_ctx->pipeline.video_decode), decoder_ctx->video_queue->queue_count);
            buf = get_next_buffer(&decoder_ctx->pipeline.video_decode); // This will block if there are no empty buffers

            // copy at most the length of the OMX buf
            int copy_length = OMX_MIN(bytes_left, buf->nAllocLen);

            memcpy(buf->pBuffer, p, copy_length);
            p += copy_length;
            bytes_left -= copy_length;

            buf->nFilledLen = copy_length;
            
            if (decoder_ctx->first_packet) {
                buf->nFlags = OMX_BUFFERFLAG_STARTTIME;
                decoder_ctx->first_packet = 0;
            } else {
                //taken from ilclient
                buf->nFlags = OMX_BUFFERFLAG_TIME_UNKNOWN;
            }

            if (decoder_ctx->pipeline.video_decode.port_settings_changed == 1) {
                int x = 640;
                int y = 480;
                
                // rounding the dimensions up to the next multiple of 16.
                x += 0x0f;
                x &= ~0x0f;
                y += 0x0f;
                y &= ~0x0f;
                decoder_ctx->pipeline.video_decode.port_settings_changed = 0;
                
                // configure the resize stuff here 
                
                //get information about the decoded video
                generic_config.nPortIndex = 131;
                OERR(OMX_GetParameter(decoder_ctx->pipeline.video_decode.h, OMX_IndexParamPortDefinition, &generic_config));
                
                //configure input of resizer
                resizer_config.nPortIndex = 60;
                OERR(OMX_GetParameter(decoder_ctx->pipeline.resize.h, OMX_IndexParamPortDefinition, &resizer_config));
                
                resizer_config.format.image.nFrameWidth = generic_config.format.video.nFrameWidth;
                resizer_config.format.image.nFrameHeight = generic_config.format.video.nFrameHeight;
                resizer_config.format.image.nStride = generic_config.format.video.nStride;
                resizer_config.format.image.nSliceHeight = generic_config.format.video.nSliceHeight;
                resizer_config.format.image.bFlagErrorConcealment = generic_config.format.video.bFlagErrorConcealment;
                resizer_config.format.image.eCompressionFormat = generic_config.format.video.eCompressionFormat;
                resizer_config.format.image.eColorFormat = generic_config.format.video.eColorFormat;
                resizer_config.format.image.pNativeWindow = generic_config.format.video.pNativeWindow;
                OERR(OMX_SetParameter(decoder_ctx->pipeline.resize.h, OMX_IndexParamPortDefinition, &resizer_config));
                
                //configure output of resizer 
                resizer_config.nPortIndex = 61; //output port 
                resizer_config.format.image.nStride = 0;
                resizer_config.format.image.nSliceHeight = 0;
                resizer_config.format.image.nFrameWidth = 640;
                resizer_config.format.image.nFrameHeight = 480;
                
                OERR(OMX_SetParameter(decoder_ctx->pipeline.resize.h, OMX_IndexParamPortDefinition, &resizer_config));
                
                
                
                fprintf(stderr, "video_decode port_settings_changed = 1\n");
                OERR(OMX_SetupTunnel(decoder_ctx->pipeline.video_decode.h, 131, decoder_ctx->pipeline.video_scheduler.h, 10));
                omx_send_command_and_wait(&decoder_ctx->pipeline.video_decode, OMX_CommandPortEnable, 131, NULL);

                omx_send_command_and_wait(&decoder_ctx->pipeline.video_scheduler, OMX_CommandPortEnable, 10, NULL);
                omx_send_command_and_wait(&decoder_ctx->pipeline.video_scheduler, OMX_CommandStateSet, OMX_StateExecuting, NULL);
                omx_send_command_and_wait(&decoder_ctx->pipeline.video_render, OMX_CommandStateSet, OMX_StateIdle, NULL);
            }

            if (decoder_ctx->pipeline.video_scheduler.port_settings_changed == 1) {
                decoder_ctx->pipeline.video_scheduler.port_settings_changed = 0;
                fprintf(stderr, "video_scheduler port_settings_changed = 1\n");

                OERR(OMX_SetupTunnel(decoder_ctx->pipeline.video_scheduler.h, 11, decoder_ctx->pipeline.video_render.h, 90));
                omx_send_command_and_wait(&decoder_ctx->pipeline.video_scheduler, OMX_CommandPortEnable, 11, NULL);
                omx_send_command_and_wait(&decoder_ctx->pipeline.video_render, OMX_CommandPortEnable, 90, NULL);
                omx_send_command_and_wait(&decoder_ctx->pipeline.video_render, OMX_CommandStateSet, OMX_StateExecuting, NULL);
            }

            OERR(OMX_EmptyThisBuffer(decoder_ctx->pipeline.video_decode.h, buf));
        }

        packet_queue_free_item(current_packet);
        current_packet = NULL;
    }

    printf("Finishing stream \n");
    /* Indicate end of video stream */
    buf = get_next_buffer(&decoder_ctx->pipeline.video_decode);

    buf->nFilledLen = 0;
    buf->nFlags = OMX_BUFFERFLAG_TIME_UNKNOWN | OMX_BUFFERFLAG_EOS;

    OERR(OMX_EmptyThisBuffer(decoder_ctx->pipeline.video_decode.h, buf));

    omx_teardown_pipeline(&decoder_ctx->pipeline);
}
