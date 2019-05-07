// TyrannoData.cpp : 콘솔 응용 프로그램에 대한 진입점을 정의합니다.
//

#include "stdafx.h"

extern "C" {
#include "libavcodec\avcodec.h"
#include "libavformat\avformat.h"
}

// Audio & Video
static AVFormatContext *fmt_ctx_av = NULL;
static const char *src_filename_av = NULL;

// Subtitle
static AVFormatContext *fmt_ctx_sub = NULL;
static const char *src_filename_sub = NULL;

int main(int argc, char **argv)
{
	src_filename_av = argv[1];
	src_filename_sub = argv[2];

	/* open input file, and allocate format context */
	if (avformat_open_input(&fmt_ctx_av, src_filename_av, NULL, NULL) < 0) {
		fprintf(stderr, "Could not open source file %s\n", src_filename_av);
		exit(1);
	}

	if (avformat_open_input(&fmt_ctx_sub, src_filename_sub, NULL, NULL) < 0) {
		fprintf(stderr, "Could not open source file %s\n", src_filename_sub);
		exit(1);
	}

	return 0;
}

