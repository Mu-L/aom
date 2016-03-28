/*
 * Copyright (c) 2016, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
*/


#include <string>

#include "third_party/googletest/src/include/gtest/gtest.h"

#include "./aom_config.h"
#include "test/codec_factory.h"
#include "test/decode_test_driver.h"
#include "test/encode_test_driver.h"
#include "test/register_state_check.h"
#include "test/video_source.h"

namespace libaom_test {
void Encoder::InitEncoder(VideoSource *video) {
  aom_codec_err_t res;
  const aom_image_t *img = video->img();

  if (video->img() && !encoder_.priv) {
    cfg_.g_w = img->d_w;
    cfg_.g_h = img->d_h;
    cfg_.g_timebase = video->timebase();
    cfg_.rc_twopass_stats_in = stats_->buf();

    res = aom_codec_enc_init(&encoder_, CodecInterface(), &cfg_, init_flags_);
    ASSERT_EQ(AOM_CODEC_OK, res) << EncoderError();

#if CONFIG_AV1_ENCODER
    if (CodecInterface() == &aom_codec_av1_cx_algo) {
      // Default to 1 tile column for AV1.
      const int log2_tile_columns = 0;
      res = aom_codec_control_(&encoder_, AV1E_SET_TILE_COLUMNS,
                               log2_tile_columns);
      ASSERT_EQ(AOM_CODEC_OK, res) << EncoderError();
    }
#endif
  }
}

void Encoder::EncodeFrame(VideoSource *video, const unsigned long frame_flags) {
  if (video->img())
    EncodeFrameInternal(*video, frame_flags);
  else
    Flush();

  // Handle twopass stats
  CxDataIterator iter = GetCxData();

  while (const aom_codec_cx_pkt_t *pkt = iter.Next()) {
    if (pkt->kind != AOM_CODEC_STATS_PKT) continue;

    stats_->Append(*pkt);
  }
}

void Encoder::EncodeFrameInternal(const VideoSource &video,
                                  const unsigned long frame_flags) {
  aom_codec_err_t res;
  const aom_image_t *img = video.img();

  // Handle frame resizing
  if (cfg_.g_w != img->d_w || cfg_.g_h != img->d_h) {
    cfg_.g_w = img->d_w;
    cfg_.g_h = img->d_h;
    res = aom_codec_enc_config_set(&encoder_, &cfg_);
    ASSERT_EQ(AOM_CODEC_OK, res) << EncoderError();
  }

  // Encode the frame
  API_REGISTER_STATE_CHECK(res = aom_codec_encode(&encoder_, img, video.pts(),
                                                  video.duration(), frame_flags,
                                                  deadline_));
  ASSERT_EQ(AOM_CODEC_OK, res) << EncoderError();
}

void Encoder::Flush() {
  const aom_codec_err_t res =
      aom_codec_encode(&encoder_, NULL, 0, 0, 0, deadline_);
  if (!encoder_.priv)
    ASSERT_EQ(AOM_CODEC_ERROR, res) << EncoderError();
  else
    ASSERT_EQ(AOM_CODEC_OK, res) << EncoderError();
}

void EncoderTest::InitializeConfig() {
  const aom_codec_err_t res = codec_->DefaultEncoderConfig(&cfg_, 0);
  dec_cfg_ = aom_codec_dec_cfg_t();
  ASSERT_EQ(AOM_CODEC_OK, res);
}

void EncoderTest::SetMode(TestMode mode) {
  switch (mode) {
    case kRealTime: deadline_ = AOM_DL_REALTIME; break;

    case kOnePassGood:
    case kTwoPassGood: deadline_ = AOM_DL_GOOD_QUALITY; break;

    case kOnePassBest:
    case kTwoPassBest: deadline_ = AOM_DL_BEST_QUALITY; break;

    default: ASSERT_TRUE(false) << "Unexpected mode " << mode;
  }

  if (mode == kTwoPassGood || mode == kTwoPassBest)
    passes_ = 2;
  else
    passes_ = 1;
}
// The function should return "true" most of the time, therefore no early
// break-out is implemented within the match checking process.
static bool compare_img(const aom_image_t *img1, const aom_image_t *img2) {
  bool match = (img1->fmt == img2->fmt) && (img1->cs == img2->cs) &&
               (img1->d_w == img2->d_w) && (img1->d_h == img2->d_h);

  const unsigned int width_y = img1->d_w;
  const unsigned int height_y = img1->d_h;
  unsigned int i;
  for (i = 0; i < height_y; ++i)
    match = (memcmp(img1->planes[AOM_PLANE_Y] + i * img1->stride[AOM_PLANE_Y],
                    img2->planes[AOM_PLANE_Y] + i * img2->stride[AOM_PLANE_Y],
                    width_y) == 0) &&
            match;
  const unsigned int width_uv = (img1->d_w + 1) >> 1;
  const unsigned int height_uv = (img1->d_h + 1) >> 1;
  for (i = 0; i < height_uv; ++i)
    match = (memcmp(img1->planes[AOM_PLANE_U] + i * img1->stride[AOM_PLANE_U],
                    img2->planes[AOM_PLANE_U] + i * img2->stride[AOM_PLANE_U],
                    width_uv) == 0) &&
            match;
  for (i = 0; i < height_uv; ++i)
    match = (memcmp(img1->planes[AOM_PLANE_V] + i * img1->stride[AOM_PLANE_V],
                    img2->planes[AOM_PLANE_V] + i * img2->stride[AOM_PLANE_V],
                    width_uv) == 0) &&
            match;
  return match;
}

void EncoderTest::MismatchHook(const aom_image_t * /*img1*/,
                               const aom_image_t * /*img2*/) {
  ASSERT_TRUE(0) << "Encode/Decode mismatch found";
}

void EncoderTest::RunLoop(VideoSource *video) {
  aom_codec_dec_cfg_t dec_cfg = aom_codec_dec_cfg_t();

  stats_.Reset();

  ASSERT_TRUE(passes_ == 1 || passes_ == 2);
  for (unsigned int pass = 0; pass < passes_; pass++) {
    last_pts_ = 0;

    if (passes_ == 1)
      cfg_.g_pass = AOM_RC_ONE_PASS;
    else if (pass == 0)
      cfg_.g_pass = AOM_RC_FIRST_PASS;
    else
      cfg_.g_pass = AOM_RC_LAST_PASS;

    BeginPassHook(pass);
    Encoder *const encoder =
        codec_->CreateEncoder(cfg_, deadline_, init_flags_, &stats_);
    ASSERT_TRUE(encoder != NULL);

    video->Begin();
    encoder->InitEncoder(video);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());

    unsigned long dec_init_flags = 0;  // NOLINT
    // Use fragment decoder if encoder outputs partitions.
    // NOTE: fragment decoder and partition encoder are only supported by AOM.
    if (init_flags_ & AOM_CODEC_USE_OUTPUT_PARTITION)
      dec_init_flags |= AOM_CODEC_USE_INPUT_FRAGMENTS;
    Decoder *const decoder = codec_->CreateDecoder(dec_cfg, dec_init_flags, 0);
    bool again;
    for (again = true; again; video->Next()) {
      again = (video->img() != NULL);

      PreEncodeFrameHook(video);
      PreEncodeFrameHook(video, encoder);
      encoder->EncodeFrame(video, frame_flags_);

      CxDataIterator iter = encoder->GetCxData();

      bool has_cxdata = false;
      bool has_dxdata = false;
      while (const aom_codec_cx_pkt_t *pkt = iter.Next()) {
        pkt = MutateEncoderOutputHook(pkt);
        again = true;
        switch (pkt->kind) {
          case AOM_CODEC_CX_FRAME_PKT:
            has_cxdata = true;
            if (decoder && DoDecode()) {
              aom_codec_err_t res_dec = decoder->DecodeFrame(
                  (const uint8_t *)pkt->data.frame.buf, pkt->data.frame.sz);

              if (!HandleDecodeResult(res_dec, *video, decoder)) break;

              has_dxdata = true;
            }
            ASSERT_GE(pkt->data.frame.pts, last_pts_);
            last_pts_ = pkt->data.frame.pts;
            FramePktHook(pkt);
            break;

          case AOM_CODEC_PSNR_PKT: PSNRPktHook(pkt); break;

          default: break;
        }
      }

      // Flush the decoder when there are no more fragments.
      if ((init_flags_ & AOM_CODEC_USE_OUTPUT_PARTITION) && has_dxdata) {
        const aom_codec_err_t res_dec = decoder->DecodeFrame(NULL, 0);
        if (!HandleDecodeResult(res_dec, *video, decoder)) break;
      }

      if (has_dxdata && has_cxdata) {
        const aom_image_t *img_enc = encoder->GetPreviewFrame();
        DxDataIterator dec_iter = decoder->GetDxData();
        const aom_image_t *img_dec = dec_iter.Next();
        if (img_enc && img_dec) {
          const bool res = compare_img(img_enc, img_dec);
          if (!res) {  // Mismatch
            MismatchHook(img_enc, img_dec);
          }
        }
        if (img_dec) DecompressedFrameHook(*img_dec, video->pts());
      }
      if (!Continue()) break;
    }

    EndPassHook();

    if (decoder) delete decoder;
    delete encoder;

    if (!Continue()) break;
  }
}

}  // namespace libaom_test
