/*
Copyright (c) 2012, Broadcom Europe Ltd
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "bcm_host.h"
#include "interface/mmal/mmal.h"
#include "interface/mmal/util/mmal_util.h"
#include "interface/mmal/util/mmal_util_params.h"
#include "interface/mmal/util/mmal_default_components.h"
#include "interface/vcos/vcos.h"
#include <stdio.h>

#define WIDTH 720
#define HEIGHT 540
#define NUM_OUTPUT_BUFFERS 500

#define CHECK_STATUS(status, msg) if (status != MMAL_SUCCESS) { fprintf(stderr, msg"\n"); goto error; }

/** Context for our application */
static struct CONTEXT_T {
   VCOS_SEMAPHORE_T semaphore;
   MMAL_QUEUE_T *queue;
} context;

/** Callback from the input port.
 * Buffer has been consumed and is available to be used again. */
static void input_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
   struct CONTEXT_T *ctx = (struct CONTEXT_T *)port->userdata;

   /* The encoder is done with the data, just recycle the buffer header into its pool */
   mmal_buffer_header_release(buffer);

   /* Kick the processing thread */
   vcos_semaphore_post(&ctx->semaphore);
}

/** Callback from the output port.
 * Buffer has been produced by the port and is available for processing. */
static void output_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
   struct CONTEXT_T *ctx = (struct CONTEXT_T *)port->userdata;

   /* Queue the encoded video frame */
   mmal_queue_put(ctx->queue, buffer);

   /* Kick the processing thread */
   vcos_semaphore_post(&ctx->semaphore);
}

int main(int argc, char **argv)
{
   MMAL_STATUS_T status = MMAL_EINVAL;
   MMAL_COMPONENT_T *encoder = 0;
   MMAL_POOL_T *pool_in = 0, *pool_out = 0;
   unsigned int count;
   uint32_t start, end;
   FILE *out_file;
   int i;

   bcm_host_init();

   /* Create the encoder component.
    * This specific component exposes 2 ports (1 input and 1 output). Like most components
    * its expects the format of its input port to be set by the client in order for it to
    * know what kind of data it will be fed. */
   status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_ENCODER, &encoder);
   CHECK_STATUS(status, "failed to create encoder");

   /* Set format of video encoder input port */
   MMAL_ES_FORMAT_T *format_in = encoder->input[0]->format;
   format_in->type = MMAL_ES_TYPE_VIDEO;
   format_in->encoding = MMAL_ENCODING_YUYV;
   format_in->es->video.width = VCOS_ALIGN_UP(WIDTH, 32);
   format_in->es->video.height = VCOS_ALIGN_UP(HEIGHT, 16);
   format_in->es->video.crop.width = WIDTH;
   format_in->es->video.crop.height = HEIGHT;
   format_in->es->video.frame_rate.num = 120;
   format_in->es->video.frame_rate.den = 1;
   format_in->es->video.par.num = 1;
   format_in->es->video.par.den = 1;
   /* If the data is known to be framed then the following flag should be set:
    * format_in->flags |= MMAL_ES_FORMAT_FLAG_FRAMED; */

   status = mmal_port_format_commit(encoder->input[0]);
   CHECK_STATUS(status, "failed to commit format");

   /* Set & Display the output port format */

   MMAL_ES_FORMAT_T *format_out = encoder->output[0]->format;
   format_out->type = MMAL_ES_TYPE_VIDEO;
   format_out->encoding = MMAL_ENCODING_H264;
   format_out->es->video.width = WIDTH;
   format_out->es->video.height = HEIGHT;
   format_out->es->video.frame_rate.num = 120;
   format_out->es->video.frame_rate.den = 1;
   format_out->bitrate = 10000000;

   fprintf(stderr, "%s\n", encoder->output[0]->name);
   fprintf(stderr, " type: %i, fourcc: %4.4s\n", format_out->type, (char *)&format_out->encoding);
   fprintf(stderr, " bitrate: %i, framed: %i\n", format_out->bitrate,
           !!(format_out->flags & MMAL_ES_FORMAT_FLAG_FRAMED));
   fprintf(stderr, " extra data: %i, %p\n", format_out->extradata_size, format_out->extradata);
   fprintf(stderr, " width: %i, height: %i, (%i,%i,%i,%i)\n",
           format_out->es->video.width, format_out->es->video.height,
           format_out->es->video.crop.x, format_out->es->video.crop.y,
           format_out->es->video.crop.width, format_out->es->video.crop.height);

   /* The format of both ports is now set so we can get their buffer requirements and create
    * our buffer headers. We use the buffer pool API to create these. */
   encoder->input[0]->buffer_num = 6;
   encoder->input[0]->buffer_size = encoder->input[0]->buffer_size_min;
   encoder->output[0]->buffer_num = 6;
   encoder->output[0]->buffer_size = encoder->output[0]->buffer_size_min;
   pool_in = mmal_port_pool_create(encoder->input[0], encoder->input[0]->buffer_num,
                              encoder->input[0]->buffer_size);
   pool_out = mmal_port_pool_create(encoder->output[0], encoder->output[0]->buffer_num,
                               encoder->output[0]->buffer_size);

   for (i = 0; i < encoder->input[0]->buffer_num; i++)
   {
     memset(pool_in->header[i]->data, (i*160)&0xFF, pool_in->header[i]->alloc_size);
   }

   /* Create a queue to store our encoded video frames. The callback we will get when
    * a frame has been encoded will put the frame into this queue. */
   context.queue = mmal_queue_create();

   out_file = fopen("out.h264", "wb");
   if (!out_file)
   {
      fprintf(stderr, "Failed to open output file\n");
      exit(1);
   }
   /* Store a reference to our context in each port (will be used during callbacks) */
   encoder->input[0]->userdata = (void *)&context;
   encoder->output[0]->userdata = (void *)&context;

   vcos_semaphore_create(&context.semaphore, "example", 1);

   /* Enable all the input port and the output port.
    * The callback specified here is the function which will be called when the buffer header
    * we sent to the component has been processed. */
   status = mmal_port_enable(encoder->input[0], input_callback);
   CHECK_STATUS(status, "failed to enable input port");
   status = mmal_port_enable(encoder->output[0], output_callback);
   CHECK_STATUS(status, "failed to enable output port");

   /* Component won't start processing data until it is enabled. */
   status = mmal_component_enable(encoder);
   CHECK_STATUS(status, "failed to enable component");

   /* Start decoding */
   fprintf(stderr, "start encoding\n");
   start = vcos_getmicrosecs();

   /* This is the main processing loop */
   for (count = 0; count < NUM_OUTPUT_BUFFERS; )
   {
      MMAL_BUFFER_HEADER_T *buffer;

      /* Wait for buffer headers to be available on either of the encoder ports */
      vcos_semaphore_wait(&context.semaphore);

      /* Send data to encode to the input port of the video encoder */
      if ((buffer = mmal_queue_get(pool_in->queue)) != NULL)
      {
      fprintf(stderr, "Got buffer %p\n", buffer);
         buffer->length = buffer->alloc_size;
         status = mmal_port_send_buffer(encoder->input[0], buffer);
         CHECK_STATUS(status, "failed to send buffer");
      }

      /* Get our encoded frames */
      while ((buffer = mmal_queue_get(context.queue)) != NULL)
      {
         /* We have a frame, do something with it (why not display it for instance?).
          * Once we're done with it, we release it. It will automatically go back
          * to its original pool so it can be reused for a new video frame.
          */
         count++;
         fprintf(stderr, "encoded frame\n");
         fwrite(buffer->data, buffer->length, 1, out_file);
         mmal_buffer_header_release(buffer);
      }

      /* Send empty buffers to the output port of the encoder */
      while ((buffer = mmal_queue_get(pool_out->queue)) != NULL)
      {
         status = mmal_port_send_buffer(encoder->output[0], buffer);
         CHECK_STATUS(status, "failed to send buffer");
      }
   }

   /* Stop encoding */
   end = vcos_getmicrosecs();
   fprintf(stderr, "stop encoding %u frames took %u usecs or %u fps\n",
           NUM_OUTPUT_BUFFERS, end - start, (1000000 * NUM_OUTPUT_BUFFERS) / (end - start));

   /* Stop everything. Not strictly necessary since mmal_component_destroy()
    * will do that anyway */
   mmal_port_disable(encoder->input[0]);
   mmal_port_disable(encoder->output[0]);
   mmal_component_disable(encoder);

   fclose(out_file);

 error:
   /* Cleanup everything */
   if (encoder)
      mmal_component_destroy(encoder);
   if (pool_in)
      mmal_pool_destroy(pool_in);
   if (pool_out)
      mmal_pool_destroy(pool_out);
   if (context.queue)
      mmal_queue_destroy(context.queue);

   vcos_semaphore_delete(&context.semaphore);
   return status == MMAL_SUCCESS ? 0 : -1;
}
