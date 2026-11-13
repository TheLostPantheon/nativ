/*
 * SceAudiodecUser: decode ATRAC9 streams supplied directly by a game. NGS has
 * its own ATRAC9 player; this API is used by middleware such as FMOD.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "third_party/libatrac9/libatrac9.h"

#define SCE_AUDIODEC_ERROR_API_FAIL 0x807F0000u
#define SCE_AUDIODEC_ERROR_INVALID_TYPE 0x807F0001u
#define SCE_AUDIODEC_ERROR_INVALID_INIT_PARAM 0x807F0002u
#define SCE_AUDIODEC_ERROR_ALREADY_INITIALIZED 0x807F0003u
#define SCE_AUDIODEC_ERROR_NOT_INITIALIZED 0x807F0005u
#define SCE_AUDIODEC_ERROR_A_HANDLE_IN_USE 0x807F0006u
#define SCE_AUDIODEC_ERROR_ALL_HANDLES_IN_USE 0x807F0007u
#define SCE_AUDIODEC_ERROR_INVALID_PTR 0x807F0008u
#define SCE_AUDIODEC_ERROR_INVALID_HANDLE 0x807F0009u
#define SCE_AUDIODEC_ERROR_NOT_HANDLE_IN_USE 0x807F000au
#define SCE_AUDIODEC_ERROR_INVALID_WORD_LENGTH 0x807F000cu
#define SCE_AUDIODEC_ERROR_INVALID_SIZE 0x807F000du
#define SCE_AUDIODEC_ERROR_INVALID_NFRAMES 0x807F000fu
#define SCE_AUDIODEC_AT9_ERROR_INVALID_CONFIG 0x807F2000u
#define SCE_AUDIODEC_TYPE_AT9 0x1003u
#define SCE_AUDIODEC_INIT_PARAM_SIZE 8u
#define SCE_AUDIODEC_AT9_INFO_SIZE 0x1cu
#define SCE_AUDIODEC_AT9_MAX_CHANNELS 16u
#define SCE_AUDIODEC_AT9_MAX_ES_SIZE 1024u
#define SCE_AUDIODEC_AT9_MAX_NFRAMES 8u
#define SCE_AUDIODEC_WORD_LENGTH_16BITS 16u

enum {
	C_SIZE = 0,
	C_HANDLE = 4,
	C_ES = 8,
	C_ES_USED = 12,
	C_ES_MAX = 16,
	C_PCM = 20,
	C_PCM_GIVEN = 24,
	C_PCM_MAX = 28,
	C_WORD_LENGTH = 32,
	C_INFO = 36,
	CTRL_SIZE = 0x28,
};
enum {
	I_CONFIG = 4,
	I_CHANNELS = 8,
	I_BIT_RATE = 12,
	I_SAMPLE_RATE = 16,
	I_SUPERFRAME_SIZE = 20,
	I_FRAMES_IN_SUPERFRAME = 24,
};

#define MAX_DECODERS 32
typedef struct {
	int used;
	void *at9;
	uint8_t config[ATRAC9_CONFIG_DATA_SIZE];
	Atrac9CodecInfo info;
	int superframe_frame;
	int superframe_bytes_left;
	int16_t pcm[256 * 8];
} Decoder;

static Decoder decoders[MAX_DECODERS];
static pthread_mutex_t decoder_lock = PTHREAD_MUTEX_INITIALIZER;
static int at9_initialized;

static int audiodec_log(void) {
	static int enabled = -1;
	if (enabled < 0) enabled = getenv("RECOMP_AUDIODEC_LOG") != 0;
	return enabled;
}

static uint32_t check_ctrl(uint32_t ctrl) {
	if (!ctrl) return SCE_AUDIODEC_ERROR_INVALID_PTR;
	if (RD32(ctrl + C_SIZE) != CTRL_SIZE) return SCE_AUDIODEC_ERROR_INVALID_SIZE;
	return 0;
}

static Decoder *decoder_for_ctrl(uint32_t ctrl) {
	uint32_t handle = RD32(ctrl + C_HANDLE);
	return handle >= 1 && handle <= MAX_DECODERS && decoders[handle - 1].used ? &decoders[handle - 1] : 0;
}

static int open_at9(Decoder *decoder) {
	decoder->at9 = Atrac9GetHandle();
	if (!decoder->at9) return -1;
	if (Atrac9InitDecoder(decoder->at9, decoder->config) ||
	    Atrac9GetCodecInfo(decoder->at9, &decoder->info) ||
	    decoder->info.channels < 1 || decoder->info.channels > 8 ||
	    decoder->info.frameSamples < 1 || decoder->info.frameSamples > 256 ||
	    decoder->info.framesInSuperframe < 1 || decoder->info.superframeSize < 1) {
		Atrac9ReleaseHandle(decoder->at9);
		decoder->at9 = 0;
		return -1;
	}
	decoder->superframe_frame = 0;
	decoder->superframe_bytes_left = decoder->info.superframeSize;
	return 0;
}

static void free_decoder(Decoder *decoder) {
	if (decoder->at9) Atrac9ReleaseHandle(decoder->at9);
	memset(decoder, 0, sizeof *decoder);
}

static void h_init(Cpu *c) {
	if (A0 != SCE_AUDIODEC_TYPE_AT9) {
		fprintf(stderr, "    audiodec: codec type %#x is not implemented\n", A0);
		RET(SCE_AUDIODEC_ERROR_INVALID_TYPE);
		return;
	}
	if (!A1) {
		RET(SCE_AUDIODEC_ERROR_INVALID_PTR);
		return;
	}
	if (RD32(A1) != SCE_AUDIODEC_INIT_PARAM_SIZE || !RD32(A1 + 4) || RD32(A1 + 4) > SCE_AUDIODEC_AT9_MAX_CHANNELS) {
		RET(SCE_AUDIODEC_ERROR_INVALID_INIT_PARAM);
		return;
	}
	pthread_mutex_lock(&decoder_lock);
	if (at9_initialized) {
		pthread_mutex_unlock(&decoder_lock);
		RET(SCE_AUDIODEC_ERROR_ALREADY_INITIALIZED);
		return;
	}
	at9_initialized = 1;
	pthread_mutex_unlock(&decoder_lock);
	RET(0);
}

static void h_term(Cpu *c) {
	if (A0 != SCE_AUDIODEC_TYPE_AT9) {
		RET(SCE_AUDIODEC_ERROR_INVALID_TYPE);
		return;
	}
	pthread_mutex_lock(&decoder_lock);
	if (!at9_initialized) {
		pthread_mutex_unlock(&decoder_lock);
		RET(SCE_AUDIODEC_ERROR_NOT_INITIALIZED);
		return;
	}
	for (int index = 0; index < MAX_DECODERS; index++) {
		if (decoders[index].used) {
			pthread_mutex_unlock(&decoder_lock);
			RET(SCE_AUDIODEC_ERROR_A_HANDLE_IN_USE);
			return;
		}
	}
	at9_initialized = 0;
	pthread_mutex_unlock(&decoder_lock);
	RET(0);
}

static void h_create(Cpu *c) {
	uint32_t ctrl = A0;
	uint32_t error = check_ctrl(ctrl);
	if (error) {
		RET(error);
		return;
	}
	if (A1 != SCE_AUDIODEC_TYPE_AT9) {
		fprintf(stderr, "    audiodec: decoder type %#x is not implemented\n", A1);
		RET(SCE_AUDIODEC_ERROR_INVALID_TYPE);
		return;
	}
	uint32_t info = RD32(ctrl + C_INFO);
	if (!info) {
		RET(SCE_AUDIODEC_ERROR_INVALID_PTR);
		return;
	}
	if (RD32(info) != SCE_AUDIODEC_AT9_INFO_SIZE) {
		RET(SCE_AUDIODEC_ERROR_INVALID_SIZE);
		return;
	}
	if (RD32(ctrl + C_WORD_LENGTH) != SCE_AUDIODEC_WORD_LENGTH_16BITS) {
		RET(SCE_AUDIODEC_ERROR_INVALID_WORD_LENGTH);
		return;
	}

	pthread_mutex_lock(&decoder_lock);
	if (!at9_initialized) {
		pthread_mutex_unlock(&decoder_lock);
		RET(SCE_AUDIODEC_ERROR_NOT_INITIALIZED);
		return;
	}
	int index = 0;
	while (index < MAX_DECODERS && decoders[index].used) index++;
	if (index == MAX_DECODERS) {
		pthread_mutex_unlock(&decoder_lock);
		RET(SCE_AUDIODEC_ERROR_ALL_HANDLES_IN_USE);
		return;
	}

	Decoder *decoder = &decoders[index];
	memset(decoder, 0, sizeof *decoder);
	memcpy(decoder->config, G(info + I_CONFIG), sizeof decoder->config);
	if (open_at9(decoder)) {
		pthread_mutex_unlock(&decoder_lock);
		fprintf(stderr, "    audiodec: invalid ATRAC9 configuration %02x %02x %02x %02x\n",
		        decoder->config[0], decoder->config[1], decoder->config[2], decoder->config[3]);
		RET(SCE_AUDIODEC_AT9_ERROR_INVALID_CONFIG);
		return;
	}
	decoder->used = 1;
	Atrac9CodecInfo codec = decoder->info;
	pthread_mutex_unlock(&decoder_lock);

	uint32_t bit_rate = (uint32_t)((uint64_t)codec.superframeSize * 8u * (uint32_t)codec.samplingRate /
	                               ((uint32_t)codec.frameSamples * (uint32_t)codec.framesInSuperframe));
	WR32(info + I_CHANNELS, (uint32_t)codec.channels);
	WR32(info + I_BIT_RATE, bit_rate);
	WR32(info + I_SAMPLE_RATE, (uint32_t)codec.samplingRate);
	WR32(info + I_SUPERFRAME_SIZE, (uint32_t)codec.superframeSize);
	WR32(info + I_FRAMES_IN_SUPERFRAME, (uint32_t)codec.framesInSuperframe);
	WR32(ctrl + C_HANDLE, (uint32_t)index + 1);
	WR32(ctrl + C_ES_MAX, (uint32_t)codec.superframeSize < SCE_AUDIODEC_AT9_MAX_ES_SIZE
	                          ? (uint32_t)codec.superframeSize
	                          : SCE_AUDIODEC_AT9_MAX_ES_SIZE);
	WR32(ctrl + C_PCM_MAX, (uint32_t)(codec.frameSamples * codec.channels * 2));
	if (audiodec_log()) {
		fprintf(stderr, "    audiodec: decoder %d: %d channels, %d Hz, %d-byte superframes, %d frames, %d-bit PCM\n",
		        index + 1, codec.channels, codec.samplingRate, codec.superframeSize, codec.framesInSuperframe,
		        RD32(ctrl + C_WORD_LENGTH));
	}
	RET(0);
}

static void h_delete(Cpu *c) {
	uint32_t error = check_ctrl(A0);
	if (error) {
		RET(error);
		return;
	}
	pthread_mutex_lock(&decoder_lock);
	Decoder *decoder = decoder_for_ctrl(A0);
	if (!decoder) {
		pthread_mutex_unlock(&decoder_lock);
		RET(SCE_AUDIODEC_ERROR_NOT_HANDLE_IN_USE);
		return;
	}
	free_decoder(decoder);
	pthread_mutex_unlock(&decoder_lock);
	RET(0);
}

static void h_clear(Cpu *c) {
	uint32_t error = check_ctrl(A0);
	if (error) {
		RET(error);
		return;
	}
	if (!RD32(A0 + C_HANDLE)) {
		RET(SCE_AUDIODEC_ERROR_INVALID_HANDLE);
		return;
	}
	pthread_mutex_lock(&decoder_lock);
	Decoder *decoder = decoder_for_ctrl(A0);
	if (!decoder) {
		pthread_mutex_unlock(&decoder_lock);
		RET(SCE_AUDIODEC_ERROR_NOT_HANDLE_IN_USE);
		return;
	}
	Atrac9ReleaseHandle(decoder->at9);
	decoder->at9 = 0;
	int failed = open_at9(decoder);
	pthread_mutex_unlock(&decoder_lock);
	RET(failed ? SCE_AUDIODEC_ERROR_API_FAIL : 0);
}

static uint32_t decode_frames(uint32_t ctrl, uint32_t frame_count) {
	uint32_t error = check_ctrl(ctrl);
	if (error) return error;
	if (!frame_count || frame_count > SCE_AUDIODEC_AT9_MAX_NFRAMES) return SCE_AUDIODEC_ERROR_INVALID_NFRAMES;
	uint32_t es = RD32(ctrl + C_ES);
	uint32_t pcm = RD32(ctrl + C_PCM);
	if (!es || !pcm) return SCE_AUDIODEC_ERROR_INVALID_PTR;

	pthread_mutex_lock(&decoder_lock);
	Decoder *decoder = decoder_for_ctrl(ctrl);
	if (!decoder) {
		pthread_mutex_unlock(&decoder_lock);
		return SCE_AUDIODEC_ERROR_NOT_HANDLE_IN_USE;
	}
	uint32_t es_used = 0;
	uint32_t pcm_written = 0;
	uint32_t frame_bytes = (uint32_t)(decoder->info.frameSamples * decoder->info.channels * 2);
	for (uint32_t frame = 0; frame < frame_count; frame++) {
		int used = 0;
		int result = Atrac9Decode(decoder->at9, G(es + es_used), decoder->pcm, &used);
		if (result || used <= 0) {
			pthread_mutex_unlock(&decoder_lock);
			WR32(ctrl + C_ES_USED, es_used);
			WR32(ctrl + C_PCM_GIVEN, pcm_written);
			fprintf(stderr, "    audiodec: ATRAC9 frame at %#x failed (%d, %d bytes)\n", es + es_used, result, used);
			return SCE_AUDIODEC_ERROR_API_FAIL;
		}

		if (used > decoder->superframe_bytes_left) {
			pthread_mutex_unlock(&decoder_lock);
			WR32(ctrl + C_ES_USED, es_used);
			WR32(ctrl + C_PCM_GIVEN, pcm_written);
			fprintf(stderr, "    audiodec: ATRAC9 frame consumed %d bytes with %d left in its superframe\n",
			        used, decoder->superframe_bytes_left);
			return SCE_AUDIODEC_ERROR_API_FAIL;
		}
		uint32_t consumed = (uint32_t)used;
		decoder->superframe_bytes_left -= used;
		if (++decoder->superframe_frame == decoder->info.framesInSuperframe) {
			if (decoder->superframe_bytes_left > 0) consumed += (uint32_t)decoder->superframe_bytes_left;
			decoder->superframe_frame = 0;
			decoder->superframe_bytes_left = decoder->info.superframeSize;
		}
		memcpy(G(pcm + pcm_written), decoder->pcm, frame_bytes);
		es_used += consumed;
		pcm_written += frame_bytes;
	}
	pthread_mutex_unlock(&decoder_lock);

	WR32(ctrl + C_ES_USED, es_used);
	WR32(ctrl + C_PCM_GIVEN, pcm_written);
	return 0;
}

static void h_decode(Cpu *c) {
	RET(decode_frames(A0, 1));
}

static void h_decode_n(Cpu *c) {
	RET(decode_frames(A0, A1));
}

static void h_internal_error(Cpu *c) {
	uint32_t error = check_ctrl(A0);
	if (error) {
		RET(error);
		return;
	}
	if (!A1) {
		RET(SCE_AUDIODEC_ERROR_INVALID_PTR);
		return;
	}
	WR32(A1, 0);
	RET(0);
}

const HleEntry hle_audiodec[] = {
	{"sceAudiodecInitLibrary", h_init},
	{"sceAudiodecTermLibrary", h_term},
	{"sceAudiodecCreateDecoder", h_create},
	{"sceAudiodecDeleteDecoder", h_delete},
	{"sceAudiodecDecode", h_decode},
	{"sceAudiodecDecodeNFrames", h_decode_n},
	{"sceAudiodecClearContext", h_clear},
	{"sceAudiodecGetInternalError", h_internal_error},
	{0, 0},
};
