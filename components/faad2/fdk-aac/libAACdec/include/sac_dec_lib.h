/* Stub for libSAC_dec (MPEG Surround / MPS spatial audio decoder).
 * This project only streams stereo internet radio; MPS is never active.
 * All functions are no-ops that signal "not available / disabled". */

#pragma once

#include "machine_type.h"
#include "FDK_audio.h"
#include "genericStds.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Error codes ─────────────────────────────────────────────────────── */
typedef INT SACDEC_ERROR;

#define MPS_OK                       ((SACDEC_ERROR)0)
#define MPS_UNSUPPORTED_CONFIG       ((SACDEC_ERROR)1)
#define MPS_PARSE_ERROR              ((SACDEC_ERROR)2)
#define MPS_OUTPUT_BUFFER_TOO_SMALL  ((SACDEC_ERROR)3)
#define SAC_INSTANCE_NOT_FULL_AVAILABLE 1

/* ── Parameter IDs ───────────────────────────────────────────────────── */
typedef INT SACDEC_PARAM;
#define SACDEC_OUT_MODE_NORMAL   0
#define SACDEC_BS_INTERRUPTION   1
#define SACDEC_CLEAR_HISTORY     2
#define SACDEC_INTERFACE         3
#define SACDEC_PARTIALLY_COMPLEX 4

/* ── Interface selector ──────────────────────────────────────────────── */
typedef INT SAC_INPUT_CONFIG;
#define SAC_INTERFACE_TIME 0
#define SAC_INTERFACE_QMF  1

/* ── Opaque decoder handle ───────────────────────────────────────────── */
typedef struct CMpegSurroundDecoder CMpegSurroundDecoder;

/* Forward-declared types used in function signatures */
struct FDK_QMF_DOMAIN;
struct LIB_INFO;

/* ── No-op function stubs ────────────────────────────────────────────── */

static inline INT mpegSurroundDecoder_Open(
    CMpegSurroundDecoder **pSelf, INT stereoConfigIndex,
    struct FDK_QMF_DOMAIN *pQmfDomain)
{
    (void)pSelf; (void)stereoConfigIndex; (void)pQmfDomain;
    if (pSelf) *pSelf = NULL;
    return 0; /* success — NULL handle means "disabled" */
}

static inline INT mpegSurroundDecoder_IsFullMpegSurroundDecoderInstanceAvailable(
    CMpegSurroundDecoder *self)
{
    (void)self;
    return SAC_INSTANCE_NOT_FULL_AVAILABLE;
}

static inline SACDEC_ERROR mpegSurroundDecoder_Config(
    CMpegSurroundDecoder *self, HANDLE_FDK_BITSTREAM hBs,
    AUDIO_OBJECT_TYPE coreCodec, INT samplingRate, INT frameSize,
    INT numChannels, INT stereoConfigIndex, INT coreSbrFrameLengthIndex,
    INT configBytes, UCHAR configMode, UCHAR *configChanged)
{
    (void)self; (void)hBs; (void)coreCodec; (void)samplingRate;
    (void)frameSize; (void)numChannels; (void)stereoConfigIndex;
    (void)coreSbrFrameLengthIndex; (void)configBytes;
    (void)configMode; (void)configChanged;
    return MPS_UNSUPPORTED_CONFIG;
}

static inline SACDEC_ERROR mpegSurroundDecoder_SetParam(
    CMpegSurroundDecoder *self, SACDEC_PARAM param, INT value)
{
    (void)self; (void)param; (void)value;
    return MPS_OK;
}

static inline SACDEC_ERROR mpegSurroundDecoder_Parse(
    CMpegSurroundDecoder *self, HANDLE_FDK_BITSTREAM hBs, INT *pCount,
    AUDIO_OBJECT_TYPE coreCodec, INT samplingRate, INT frameSize, INT indepFlag)
{
    (void)self; (void)hBs; (void)pCount; (void)coreCodec;
    (void)samplingRate; (void)frameSize; (void)indepFlag;
    return MPS_PARSE_ERROR;
}

static inline SACDEC_ERROR mpegSurroundDecoder_ParseNoHeader(
    CMpegSurroundDecoder *self, HANDLE_FDK_BITSTREAM hBs,
    INT *pCount, INT indepFlag)
{
    (void)self; (void)hBs; (void)pCount; (void)indepFlag;
    return MPS_PARSE_ERROR;
}

static inline INT mpegSurroundDecoder_Apply(
    CMpegSurroundDecoder *self,
    PCM_AAC *input, PCM_DEC *pTimeData, INT timeDataSize,
    INT nrSamplesPerFrame, INT *nChannels, INT *frameSize,
    INT sampleRate, AUDIO_OBJECT_TYPE coreCodec,
    AUDIO_CHANNEL_TYPE *channelType, UCHAR *channelIndices,
    const FDK_channelMapDescr *mapDescr, INT inDataHeadroom,
    INT *outDataHeadroom)
{
    (void)self; (void)input; (void)pTimeData; (void)timeDataSize;
    (void)nrSamplesPerFrame; (void)nChannels; (void)frameSize;
    (void)sampleRate; (void)coreCodec; (void)channelType;
    (void)channelIndices; (void)mapDescr; (void)inDataHeadroom;
    (void)outDataHeadroom;
    return MPS_UNSUPPORTED_CONFIG;
}

static inline INT mpegSurroundDecoder_GetDelay(CMpegSurroundDecoder *self)
{
    (void)self;
    return 0;
}

static inline void mpegSurroundDecoder_ConfigureQmfDomain(
    CMpegSurroundDecoder *self, SAC_INPUT_CONFIG sacInputConfig,
    UINT sampleRate, AUDIO_OBJECT_TYPE coreCodec)
{
    (void)self; (void)sacInputConfig; (void)sampleRate; (void)coreCodec;
}

static inline INT mpegSurroundDecoder_FreeMem(CMpegSurroundDecoder *self)
{
    (void)self;
    return MPS_OK;
}

static inline void mpegSurroundDecoder_Close(CMpegSurroundDecoder *self)
{
    (void)self;
}

static inline INT mpegSurroundDecoder_GetLibInfo(LIB_INFO *info)
{
    (void)info;
    return 0;
}

static inline INT mpegSurroundDecoder_IsPseudoLR(
    CMpegSurroundDecoder *self, int *bsPseudoLr)
{
    (void)self;
    if (bsPseudoLr) *bsPseudoLr = 0;
    return 0;
}

#ifdef __cplusplus
}
#endif
