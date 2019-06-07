#if 0

// TyrannoData.c
//

#include "stdafx.h"

// FFMPEG
#define __STDC_CONSTANT_MACROS

#include "libavutil\imgutils.h"
#include "libavutil\samplefmt.h"
#include "libavutil\timestamp.h"
#include "libavcodec\avcodec.h"
#include "libavformat\avformat.h"
#include "libswscale\swscale.h"
#include "libavutil\avconfig.h"
#include "libavutil\avstring.h"

static AVFormatContext *fmt_ctx_av = NULL, *fmt_ctx_sub = NULL;
static AVCodecContext *video_dec_ctx = NULL, *audio_dec_ctx = NULL, *subtitle_dec_ctx = NULL;
static int width, height;
static enum AVPixelFormat pix_fmt;
static AVStream *video_stream = NULL, *audio_stream = NULL, *subtitle_stream = NULL;
static const char *src_filename_av = NULL, *src_filename_sub = NULL;
static const char *video_dst_filename = NULL;
static const char *audio_dst_filename = NULL;
static FILE *video_dst_file = NULL;
static FILE *audio_dst_file = NULL;

static uint8_t *video_dst_data[4] = { NULL };
static int video_dst_linesize[4];
static int video_dst_bufsize;

static int video_stream_idx = -1, audio_stream_idx = -1, subtitle_stream_idx = -1;
static AVFrame *video_frame = NULL, *audio_frame = NULL;
static AVPacket pkt;
static int video_frame_count = 0;
static int audio_frame_count = 0;

/* Enable or disable frame reference counting. You are not supposed to support
* both paths in your application but pick the one most appropriate to your
* needs. Look for the use of refcount in this example to see what are the
* differences of API usage between them. */
static int refcount = 0;

struct SwsContext *sws_ctx = NULL;

// SDL
#include "SDL.h"
#undef main
//#include "SDL_thread.h"

#define SDL_AUDIO_BUFFER_SIZE   1024

SDL_Window* window = NULL;
SDL_Renderer* renderer = NULL;
SDL_Texture* texture = NULL;
Uint8 *yPlane = NULL, *uPlane = NULL, *vPlane = NULL;
size_t yPlaneSz, uvPlaneSz;
int uvPitch;

SDL_AudioSpec wanted_spec, spec;

typedef struct PacketQueue {
	AVPacketList *first_pkt, *last_pkt;
	int nb_packets;
	int size;
	SDL_mutex *mutex;
	SDL_cond *cond;
} PacketQueue;

PacketQueue audioq;

void packet_queue_init(PacketQueue *q) {
	memset(q, 0, sizeof(PacketQueue));
	q->mutex = SDL_CreateMutex();
	q->cond = SDL_CreateCond();
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt) {

	AVPacketList *pkt1;
	if (av_dup_packet(pkt) < 0) {
		return -1;
	}
	pkt1 = av_malloc(sizeof(AVPacketList));
	if (!pkt1)
		return -1;
	pkt1->pkt = *pkt;
	pkt1->next = NULL;


	SDL_LockMutex(q->mutex);

	if (!q->last_pkt)
		q->first_pkt = pkt1;
	else
		q->last_pkt->next = pkt1;
	q->last_pkt = pkt1;
	q->nb_packets++;
	q->size += pkt1->pkt.size;
	SDL_CondSignal(q->cond);

	SDL_UnlockMutex(q->mutex);
	return 0;
}

int quit = 0;

static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block) {
	AVPacketList *pkt1;
	int ret;

	SDL_LockMutex(q->mutex);

	for (;;) {

		if (quit) {
			ret = -1;
			break;
		}

		pkt1 = q->first_pkt;
		if (pkt1) {
			q->first_pkt = pkt1->next;
			if (!q->first_pkt)
				q->last_pkt = NULL;
			q->nb_packets--;
			q->size -= pkt1->pkt.size;
			*pkt = pkt1->pkt;
			av_free(pkt1);
			ret = 1;
			break;
		}
		else if (!block) {
			ret = 0;
			break;
		}
		else {
			SDL_CondWait(q->cond, q->mutex);
		}
	}
	SDL_UnlockMutex(q->mutex);
	return ret;
}

int decode_interrupt_cb(void) {
	return quit;
}

int audio_decode_frame(AVCodecContext *aCodecCtx, uint8_t *audio_buf, int buf_size) {

	static uint8_t *audio_pkt_data = NULL;
	static int audio_pkt_size = 0;

	int len1, data_size = 0;

	for (;;) {

		if (quit) {
			return -1;
		}

		if (packet_queue_get(&audioq, &pkt, 1) < 0) {
			return -1;
		}

		audio_pkt_data = pkt.data;
		audio_pkt_size = pkt.size;

		/* decode audio frame */
		/* send the packet with the compressed data to the decoder */
		int ret = avcodec_send_packet(audio_dec_ctx, &pkt);
		if (ret < 0) {
			fprintf(stderr, "Error submitting the packet to the decoder\n");
			exit(1);
		}

		/* read all the output frames (in general there may be any number of them */
		//while (ret >= 0) {
			ret = avcodec_receive_frame(audio_dec_ctx, audio_frame);
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
				return;
			else if (ret < 0) {
				fprintf(stderr, "Error during decoding\n");
				exit(1);
			}
			data_size = av_samples_get_buffer_size(NULL,
				audio_dec_ctx->channels,
				audio_frame->nb_samples,
				audio_dec_ctx->sample_fmt,
				1);

			//Out Audio Param
			uint64_t out_channel_layout = AV_CH_LAYOUT_STEREO;
			//nb_samples: AAC-1024 MP3-1152
			int out_nb_samples = audio_dec_ctx->frame_size;
			int out_sample_rate = 44100;
			int out_channels = av_get_channel_layout_nb_channels(out_channel_layout);
			//Out Buffer Size
			int out_buffer_size = av_samples_get_buffer_size(NULL, out_channels, out_nb_samples, AV_SAMPLE_FMT_S16, 1);

			int64_t in_channel_layout = av_get_default_channel_layout(audio_dec_ctx->channels);
			struct SwrContext *au_convert_ctx = swr_alloc_set_opts(NULL, out_channel_layout, AV_SAMPLE_FMT_S16, out_sample_rate,
				in_channel_layout, audio_dec_ctx->sample_fmt, audio_dec_ctx->sample_rate, 0, NULL);

			swr_convert(au_convert_ctx, audio_buf, audio_frame->nb_samples, (const uint8_t **)audio_frame->data, audio_frame->nb_samples);
			//assert(data_size <= buf_size);
			//memcpy(audio_buf, audio_frame->data[0], data_size);
			return data_size;
			/*
			int i, ch;
			for (i = 0; i < frame->nb_samples; i++)
			for (ch = 0; ch < dec_ctx->channels; ch++)
			fwrite(frame->data[ch] + data_size*i, 1, data_size, outfile);
			*/
			//packet_queue_put(&audioq, &pkt);
		//}

#if 0
			int got_frame = 0;
			len1 = avcodec_decode_audio4(aCodecCtx, &audio_frame, &got_frame, &pkt);
			if (len1 < 0) {
				/* if error, skip frame */
				audio_pkt_size = 0;
				break;
			}
			audio_pkt_data += len1;
			audio_pkt_size -= len1;
			data_size = 0;
			if (got_frame) {
				data_size = av_samples_get_buffer_size(NULL,
					aCodecCtx->channels,
					audio_frame->nb_samples,
					aCodecCtx->sample_fmt,
					1);
				//assert(data_size <= buf_size);
				memcpy(audio_buf, audio_frame->data[0], data_size);
			}
			if (data_size <= 0) {
				/* No data yet, get more frames */
				continue;
			}
			/* We have data, return it and come back for more later */
			return data_size;
		}
		if (pkt.data)
			av_free_packet(&pkt);

		if (quit) {
			return -1;
		}

		if (packet_queue_get(&audioq, &pkt, 1) < 0) {
			return -1;
		}
		audio_pkt_data = pkt.data;
		audio_pkt_size = pkt.size;
#endif
	}

#if 0
	//int ret = 1;


#endif
}

void audio_callback(void *userdata, Uint8 *stream, int len)
{
	AVCodecContext *aCodecCtx = (AVCodecContext *)userdata;
	int len1, audio_size;

	static uint8_t audio_buf[(192000 * 3) / 2];
	static unsigned int audio_buf_size = 0;
	static unsigned int audio_buf_index = 0;

#if 1
	while (len > 0) {
		if (audio_buf_index >= audio_buf_size) {
			/* We have already sent all our data; get more */
			audio_size = audio_decode_frame(aCodecCtx, audio_buf, sizeof(audio_buf));
			if (audio_size < 0) {
				/* If error, output silence */
				audio_buf_size = 1024;
				memset(audio_buf, 0, audio_buf_size);
			}
			else {
				audio_buf_size = audio_size;
			}
			audio_buf_index = 0;
		}
		len1 = audio_buf_size - audio_buf_index;
		if (len1 > len)
			len1 = len;
		memcpy(stream, (uint8_t *)audio_buf + audio_buf_index, len1);
		len -= len1;
		stream += len1;
		audio_buf_index += len1;
	}
#endif
}

int decode_packet()
{
	int ret = 0;
	int decoded = pkt.size;

	if (pkt.stream_index == video_stream_idx) {
		/* decode video frame */
		//ret = avcodec_decode_video2(video_dec_ctx, frame, got_frame, &pkt);
		ret = avcodec_send_packet(video_dec_ctx, &pkt);
		if (ret < 0) {
			fprintf(stderr, "Error sending a packet for decoding\n");
			exit(1);
		}

		while (ret >= 0) {
			ret = avcodec_receive_frame(video_dec_ctx, video_frame);
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
				return 0;
			else if (ret < 0) {
				fprintf(stderr, "Error during decoding\n");
				exit(1);
			}

#if 0
			printf("video_frame n:%d coded_n:%d\n",
				video_frame_count++, frame->coded_picture_number);

			/* copy decoded frame to destination buffer:
			* this is required since rawvideo expects non aligned data */
			av_image_copy(video_dst_data, video_dst_linesize,
				(const uint8_t **)(frame->data), frame->linesize,
				pix_fmt, width, height);

			/* write to rawvideo file */
			fwrite(video_dst_data[0], 1, video_dec_ctx->width * video_dec_ctx->height, video_dst_file);
			fwrite(video_dst_data[1], 1, video_dec_ctx->width * video_dec_ctx->height / 4, video_dst_file);
			fwrite(video_dst_data[2], 1, video_dec_ctx->width * video_dec_ctx->height / 4, video_dst_file);
#endif

#if 1
			AVPicture pict;
			pict.data[0] = yPlane;
			pict.data[1] = uPlane;
			pict.data[2] = vPlane;
			pict.linesize[0] = video_dec_ctx->width;
			pict.linesize[1] = uvPitch;
			pict.linesize[2] = uvPitch;

			// Convert the image into YUV format that SDL uses
			sws_scale(sws_ctx, (uint8_t const * const *)video_frame->data,
				video_frame->linesize, 0, video_dec_ctx->height, pict.data,
				pict.linesize);

			SDL_UpdateYUVTexture(
				texture,
				NULL,
				yPlane,
				video_dec_ctx->width,
				uPlane,
				uvPitch,
				vPlane,
				uvPitch
			);

			SDL_RenderClear(renderer);
			SDL_RenderCopy(renderer, texture, NULL, NULL);
			SDL_RenderPresent(renderer);
#endif
		}
	}
	else if (pkt.stream_index == audio_stream_idx) {

		packet_queue_put(&audioq, &pkt);
#if 0
		/* decode audio frame */
		/* send the packet with the compressed data to the decoder */
		int ret = avcodec_send_packet(audio_dec_ctx, &pkt);
		if (ret < 0) {
			fprintf(stderr, "Error submitting the packet to the decoder\n");
			exit(1);
		}

		/* read all the output frames (in general there may be any number of them */
		while (ret >= 0) {
			ret = avcodec_receive_frame(audio_dec_ctx, audio_frame);
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
				return;
			else if (ret < 0) {
				fprintf(stderr, "Error during decoding\n");
				exit(1);
			}
			int data_size = av_get_bytes_per_sample(audio_dec_ctx->sample_fmt);
			if (data_size < 0) {
				/* This should not occur, checking just for paranoia */
				fprintf(stderr, "Failed to calculate data size\n");
				exit(1);
			}
			/*
			int i, ch;
			for (i = 0; i < frame->nb_samples; i++)
			for (ch = 0; ch < dec_ctx->channels; ch++)
			fwrite(frame->data[ch] + data_size*i, 1, data_size, outfile);
			*/
			//packet_queue_put(&audioq, &pkt);
		}
#endif
	}
	else {
		av_free_packet(&pkt);
	}

	return decoded;
}

int open_codec_context(int *stream_idx,
	AVCodecContext **dec_ctx, AVFormatContext *fmt_ctx, enum AVMediaType type)
{
	int ret, stream_index;
	AVStream *st;
	AVCodec *dec = NULL;
	AVDictionary *opts = NULL;

	ret = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);
	if (ret < 0) {
		fprintf(stderr, "Could not find %s stream in input file '%s'\n",
			av_get_media_type_string(type), src_filename_av);
		return ret;
	}
	else {
		stream_index = ret;
		st = fmt_ctx->streams[stream_index];

		/* find decoder for the stream */
		dec = avcodec_find_decoder(st->codecpar->codec_id);
		if (!dec) {
			fprintf(stderr, "Failed to find %s codec\n",
				av_get_media_type_string(type));
			return AVERROR(EINVAL);
		}

		/* Allocate a codec context for the decoder */
		*dec_ctx = avcodec_alloc_context3(dec);
		if (!*dec_ctx) {
			fprintf(stderr, "Failed to allocate the %s codec context\n",
				av_get_media_type_string(type));
			return AVERROR(ENOMEM);
		}

		/* Copy codec parameters from input stream to output codec context */
		if ((ret = avcodec_parameters_to_context(*dec_ctx, st->codecpar)) < 0) {
			fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
				av_get_media_type_string(type));
			return ret;
		}

		/* Init the decoders, with or without reference counting */
		av_dict_set(&opts, "refcounted_frames", refcount ? "1" : "0", 0);
		if ((ret = avcodec_open2(*dec_ctx, dec, &opts)) < 0) {
			fprintf(stderr, "Failed to open %s codec\n",
				av_get_media_type_string(type));
			return ret;
		}
		*stream_idx = stream_index;
	}

	return 0;
}

int get_format_from_sample_fmt(const char **fmt,
	enum AVSampleFormat sample_fmt)
{
	int i;
	struct sample_fmt_entry {
		enum AVSampleFormat sample_fmt; const char *fmt_be, *fmt_le;
	} sample_fmt_entries[] = {
		{ AV_SAMPLE_FMT_U8,  "u8",    "u8" },
		{ AV_SAMPLE_FMT_S16, "s16be", "s16le" },
		{ AV_SAMPLE_FMT_S32, "s32be", "s32le" },
		{ AV_SAMPLE_FMT_FLT, "f32be", "f32le" },
		{ AV_SAMPLE_FMT_DBL, "f64be", "f64le" },
	};
	*fmt = NULL;

	for (i = 0; i < FF_ARRAY_ELEMS(sample_fmt_entries); i++) {
		struct sample_fmt_entry *entry = &sample_fmt_entries[i];
		if (sample_fmt == entry->sample_fmt) {
			*fmt = AV_NE(entry->fmt_be, entry->fmt_le);
			return 0;
		}
	}

	fprintf(stderr,
		"sample format %s is not supported as output format\n",
		av_get_sample_fmt_name(sample_fmt));
	return -1;
}


int main(int argc, char **argv)
{
	int ret = 0, got_frame;

	src_filename_av = argv[1];
	src_filename_sub = argv[2];

	/* register all codecs, demux and protocols */
	avdevice_register_all();
	avformat_network_init();

	//init_opts();

	//url_set_interrupt_cb(decode_interrupt_cb);

	/* open input file, and allocate format context */
	if (avformat_open_input(&fmt_ctx_av, src_filename_av, NULL, NULL) < 0) {
		fprintf(stderr, "Could not open source file %s\n", src_filename_av);
		exit(1);
	}
	if (avformat_open_input(&fmt_ctx_sub, src_filename_sub, NULL, NULL) < 0) {
		fprintf(stderr, "Could not open source file %s\n", src_filename_sub);
		exit(1);
	}

	/* retrieve stream information */
	if (avformat_find_stream_info(fmt_ctx_av, NULL) < 0) {
		fprintf(stderr, "Could not find stream information\n");
		exit(1);
	}
	if (avformat_find_stream_info(fmt_ctx_sub, NULL) < 0) {
		fprintf(stderr, "Could not find stream information\n");
		exit(1);
	}

	/* open codec context */
	if (open_codec_context(&video_stream_idx, &video_dec_ctx, fmt_ctx_av, AVMEDIA_TYPE_VIDEO) >= 0) {
		video_stream = fmt_ctx_av->streams[video_stream_idx];

		video_dst_file = fopen("sample.out.yuv", "wb");
		if (!video_dst_file) {
			fprintf(stderr, "Could not open destination file %s\n", video_dst_filename);
			ret = 1;
			goto end;
		}

		/* allocate image where the decoded image will be put */
		width = video_dec_ctx->width;
		height = video_dec_ctx->height;
		pix_fmt = video_dec_ctx->pix_fmt;
		ret = av_image_alloc(video_dst_data, video_dst_linesize,
			width, height, pix_fmt, 1);
		if (ret < 0) {
			fprintf(stderr, "Could not allocate raw video buffer\n");
			goto end;
		}
		video_dst_bufsize = ret;
	}

	if (open_codec_context(&audio_stream_idx, &audio_dec_ctx, fmt_ctx_av, AVMEDIA_TYPE_AUDIO) >= 0) {
		audio_stream = fmt_ctx_av->streams[audio_stream_idx];
		audio_dst_file = fopen("sample.out.wav", "wb");
		if (!audio_dst_file) {
			fprintf(stderr, "Could not open destination file %s\n", audio_dst_filename);
			ret = 1;
			goto end;
		}

		wanted_spec.freq = audio_dec_ctx->sample_rate;
		//wanted_spec.format = AV_SAMPLE_FMT_FLTP; // audio_dec_ctx->sample_fmt;
		wanted_spec.format = AUDIO_F32SYS; // AUDIO_S16SYS;
		wanted_spec.channels = audio_dec_ctx->channels;
		wanted_spec.silence = 0;
		wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
		wanted_spec.callback = audio_callback;
		wanted_spec.userdata = audio_dec_ctx;

		if (SDL_OpenAudio(&wanted_spec, &spec) < 0) {
			fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
			return -1;
		}
	}

	if (open_codec_context(&subtitle_stream_idx, &subtitle_dec_ctx, fmt_ctx_sub, AVMEDIA_TYPE_SUBTITLE) >= 0) {
		subtitle_stream = fmt_ctx_sub->streams[subtitle_stream_idx];
	}

	/* dump input information to stderr */
	av_dump_format(fmt_ctx_av, 0, src_filename_av, 0);
	av_dump_format(fmt_ctx_sub, 0, src_filename_sub, 0);

	if (!audio_stream && !video_stream) {
		fprintf(stderr, "Could not find audio or video stream in the input, aborting\n");
		ret = 1;
		goto end;
	}

#if 1
	/* Initialize SDL. */
	if (SDL_Init(SDL_INIT_EVERYTHING)) {
		fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
		exit(1);
	}

	/* Initialize SDL Audio. */
	packet_queue_init(&audioq);
	SDL_PauseAudio(0);

	/* Create the window where we will draw. */
	window = SDL_CreateWindow("SDL_RenderClear",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		video_dec_ctx->width,
		video_dec_ctx->height,
		0);

	/* We must call SDL_CreateRenderer in order for draw calls to affect this window. */
	renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

	/* Select the color for drawing. It is set to red here. */
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);

	/* Clear the entire screen to our selected color. */
	SDL_RenderClear(renderer);

	/* Up until now everything was drawn behind the scenes.
	This will show the new, red contents of the window. */
	SDL_RenderPresent(renderer);

	/* Give us time to see the window. */
	SDL_Delay(100);

	// Allocate a place to put our YUV image on that screen
	texture = SDL_CreateTexture(
		renderer,
		SDL_PIXELFORMAT_YV12,
		SDL_TEXTUREACCESS_STREAMING,
		video_dec_ctx->width,
		video_dec_ctx->height
	);
	if (!texture) {
		fprintf(stderr, "SDL: could not create texture - exiting\n");
		exit(1);
	}

	// initialize SWS context for software scaling
	sws_ctx = sws_getContext(video_dec_ctx->width, video_dec_ctx->height,
		video_dec_ctx->pix_fmt, video_dec_ctx->width, video_dec_ctx->height,
		AV_PIX_FMT_YUV420P,
		SWS_BILINEAR,
		NULL,
		NULL,
		NULL);

	// set up YV12 pixel array (12 bits per pixel)
	yPlaneSz = video_dec_ctx->width * video_dec_ctx->height;
	uvPlaneSz = video_dec_ctx->width * video_dec_ctx->height / 4;
	yPlane = (Uint8*)malloc(yPlaneSz);
	uPlane = (Uint8*)malloc(uvPlaneSz);
	vPlane = (Uint8*)malloc(uvPlaneSz);
	if (!yPlane || !uPlane || !vPlane) {
		fprintf(stderr, "Could not allocate pixel buffers - exiting\n");
		exit(1);
	}

	uvPitch = video_dec_ctx->width / 2;
#endif

	video_frame = av_frame_alloc();
	audio_frame = av_frame_alloc();
	if (!video_frame || !audio_frame) {
		fprintf(stderr, "Could not allocate frame\n");
		ret = AVERROR(ENOMEM);
		goto end;
	}

	/* initialize packet, set data to NULL, let the demuxer fill it */
	av_init_packet(&pkt);
	pkt.data = NULL;
	pkt.size = 0;

	if (video_stream)
		printf("Demuxing video from file '%s'\n", src_filename_av);
	if (audio_stream)
		printf("Demuxing audio from file '%s'\n", src_filename_av);
	if (subtitle_stream)
		printf("Demuxing subtitle from file '%s'\n", src_filename_sub);


	int isRunning = 1;
	SDL_Event ev;

	while (isRunning == 1)
	{
		if (SDL_PollEvent(&ev) != 0)
		{
			// Getting the events
			if (ev.type == SDL_QUIT)
			{
				quit = 1;
				isRunning = -1;
			}
		}
		else if (ev.type == SDL_KEYDOWN)
		{
			switch (ev.key.keysym.sym)
			{
			case SDLK_p:
				isRunning = -1;
				break;
			}
		}
		if (av_read_frame(fmt_ctx_av, &pkt) >= 0) {
			ret = decode_packet();
		}
	}

end:
	avcodec_free_context(&video_dec_ctx);
	avcodec_free_context(&audio_dec_ctx);
	avformat_close_input(&fmt_ctx_av);
	avformat_close_input(&fmt_ctx_sub);
	if (video_dst_file)
		fclose(video_dst_file);
	if (audio_dst_file)
		fclose(audio_dst_file);
	av_frame_free(&video_frame);
	av_frame_free(&audio_frame);
	av_free(video_dst_data[0]);
	
#if 1
	/* Always be sure to clean up */
	SDL_DestroyTexture(texture);
	//SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	//window = NULL;
	//texture = NULL;
	//renderer = NULL;
	SDL_Quit();

	// Free the YUV frame
	free(yPlane);
	free(uPlane);
	free(vPlane);
#endif

	return 0;
}

#endif