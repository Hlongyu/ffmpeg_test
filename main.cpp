#include <stdio.h>
extern "C" {

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avassert.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}
#include <chrono>
#include <iostream>
#include <thread>
static AVBufferRef *hw_device_ctx = NULL;
static enum AVPixelFormat hw_pix_fmt;
static FILE *output_file = NULL;
AVFrame *sw_frame[10];
AVFrame *frame[10];
int num = 0;
static int hw_decoder_init(AVCodecContext *ctx,
                           const enum AVHWDeviceType type) {
  int err = 0;

  if ((err = av_hwdevice_ctx_create(&hw_device_ctx, type, NULL, NULL, 0)) < 0) {
    fprintf(stderr, "Failed to create specified HW device.\n");
    return err;
  }
  ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);

  return err;
}

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                        const enum AVPixelFormat *pix_fmts) {
  const enum AVPixelFormat *p;

  for (p = pix_fmts; *p != -1; p++) {
    if (*p == hw_pix_fmt)
      return *p;
  }

  fprintf(stderr, "Failed to get HW surface format.\n");
  return AV_PIX_FMT_NONE;
}

static int decode_write(AVCodecContext *avctx, AVPacket *packet) {

  AVFrame *tmp_frame = NULL;
  uint8_t *buffer = NULL;
  int size;
  int ret = 0;

  ret = avcodec_send_packet(avctx, packet);
  if (ret < 0) {
    fprintf(stderr, "Error during decoding\n");
    return ret;
  }

  while (1) {
    // if (!(frame = av_frame_alloc())) {
    //   fprintf(stderr, "Can not alloc frame\n");
    //   ret = AVERROR(ENOMEM);
    //   goto fail;
    // }
    num++;
    num = num % 10;
    ret = avcodec_receive_frame(avctx, frame[num]);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      // av_frame_free(&frame);
      // av_frame_free(&sw_frame);
      return 0;
    } else if (ret < 0) {
      fprintf(stderr, "Error while decoding\n");
      goto fail;
    }

    if (frame[num]->format == hw_pix_fmt) {
      /* retrieve data from GPU to CPU */
      auto start = std::chrono::high_resolution_clock::now();
      if ((ret = av_hwframe_transfer_data(sw_frame[num], frame[num], 0)) < 0) {
        fprintf(stderr, "Error transferring the data to system memory\n");
        goto fail;
      }
      auto end = std::chrono::high_resolution_clock::now();
      std::chrono::duration<double> duration = end - start;

      std::cout << "Execution time: " << duration.count() * 1000 << " ms"
                << std::endl;
      av_frame_copy_props(frame[num], sw_frame[num]);
      // std::this_thread::sleep_for(std::chrono::milliseconds(1));

      tmp_frame = sw_frame[num];
    } else
      tmp_frame = frame[num];

  fail:
    // av_frame_free(&frame);
    // av_frame_free(&sw_frame);
    av_freep(&buffer);
    if (ret < 0)
      return ret;
  }
}

int main(int argc, char *argv[]) {
  AVFormatContext *input_ctx = NULL;
  int video_stream, ret;
  AVStream *video = NULL;
  AVCodecContext *decoder_ctx = NULL;
  const AVCodec *decoder = NULL;
  AVPacket *packet = NULL;
  enum AVHWDeviceType type;
  int i;

  // if (argc < 4) {
  //   fprintf(stderr, "Usage: %s <device type> <input file> <output file>\n",
  //           argv[0]);
  //   return -1;
  // }

  // type = AV_HWDEVICE_TYPE_QSV;
  type = AV_HWDEVICE_TYPE_D3D11VA;
  // type = AV_HWDEVICE_TYPE_CUDA;

  packet = av_packet_alloc();
  if (!packet) {
    fprintf(stderr, "Failed to allocate AVPacket\n");
    return -1;
  }

  /* open the input file */
  if (avformat_open_input(&input_ctx, "16k_1.mkv", NULL, NULL) != 0) {
    fprintf(stderr, "Cannot open input file '%s'\n", argv[2]);
    return -1;
  }

  if (avformat_find_stream_info(input_ctx, NULL) < 0) {
    fprintf(stderr, "Cannot find input stream information.\n");
    return -1;
  }

  /* find the video stream information */
  ret = av_find_best_stream(input_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
  if (ret < 0) {
    fprintf(stderr, "Cannot find a video stream in the input file\n");
    return -1;
  }
  video_stream = ret;
  const AVCodec *codec = nullptr;
  void *iter = nullptr;
  bool find = false;
  for (int i = 0; i < 10; i++) {
    sw_frame[i] = av_frame_alloc();
    frame[i] = av_frame_alloc();
  }
  // decoder = avcodec_find_decoder_by_name("hevc_qsv");
  while (1) {
    if (find) {
      break;
    }
    codec = av_codec_iterate(&iter);
    if (!codec) {
      // m_use_hw = false;
      return -1;
      break;
    }

    if (codec->type != AVMEDIA_TYPE_VIDEO || !av_codec_is_decoder(codec) ||
        avcodec_descriptor_get(codec->id) !=
            avcodec_descriptor_get(decoder->id))
      continue;
    for (i = 0;; i++) {
      const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
      if (!config) {
        fprintf(stderr, "Decoder %s does not support device type %s.\n",
                decoder->name, av_hwdevice_get_type_name(type));
        break;
        // return -1;
      }
      if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
          config->device_type == type) {
        hw_pix_fmt = config->pix_fmt;
        find = true;
        decoder = codec;
        break;
      }
    }
  }
  if (!(decoder_ctx = avcodec_alloc_context3(decoder)))
    return AVERROR(ENOMEM);

  video = input_ctx->streams[video_stream];
  if (avcodec_parameters_to_context(decoder_ctx, video->codecpar) < 0)
    return -1;
  decoder_ctx->thread_count = 0;
  decoder_ctx->get_format = get_hw_format;

  if (hw_decoder_init(decoder_ctx, type) < 0)
    return -1;

  if ((ret = avcodec_open2(decoder_ctx, decoder, NULL)) < 0) {
    fprintf(stderr, "Failed to open codec for stream #%u\n", video_stream);
    return -1;
  }

  /* open the file to dump raw data */
  // output_file = fopen(argv[3], "w+b");

  /* actual decoding and dump the raw data */
  while (ret >= 0) {
    if ((ret = av_read_frame(input_ctx, packet)) < 0)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (video_stream == packet->stream_index)
      ret = decode_write(decoder_ctx, packet);

    av_packet_unref(packet);
  }

  /* flush the decoder */
  ret = decode_write(decoder_ctx, NULL);

  if (output_file)
    fclose(output_file);
  av_packet_free(&packet);
  avcodec_free_context(&decoder_ctx);
  avformat_close_input(&input_ctx);
  av_buffer_unref(&hw_device_ctx);

  return 0;
}
