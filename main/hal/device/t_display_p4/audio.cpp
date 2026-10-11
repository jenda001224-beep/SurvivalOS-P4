/*
 * @Description: T-Display-P4 音频硬件实现
 * @Author: LILYGO_L
 * @Date: 2026-08-28 00:00:00
 * @LastEditTime: 2026-09-02 17:52:50
 * @License: GPL 3.0
 */
#include <strings.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cstdio>

#include "audio/new_notification_010_c2_b16_s44100.h"
#include "audio/wav_decoder.h"
#include "base/logger.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/device/common/device_utils.h"
#include "hal/device/t_display_p4/device.h"

namespace lilygo_box::hal {
namespace device = lilygo_device_driver::t_display_p4::device;
namespace {

// SurvivalOS: ES8311 DAC volume register, 0.5 dB per step with 191 = 0 dB.
// The slider used to run linearly from the bottom of the register, which put
// three quarters of its travel below what the small speaker makes audible.
// It now spans -30 dB to +6 dB; the 6 dB above full scale buy loudness for
// quiet material at the cost of clipping the peaks of loud one.
constexpr int kAudioVolumeRegisterAtOnePercent = 131;
constexpr int kAudioVolumeRegisterAtFull = 203;
// SurvivalOS: digital microphone gain on top of the 30 dB analogue stage. The
// vendor's 18 dB left speech peaking near -18 dBFS.
constexpr auto kMicrophoneDigitalGain =
    cpp_bus_driver::Es8311::AdcGain::kGain30db;
constexpr size_t kSpeakerPlaybackChunkBytes = 4096;
constexpr uint32_t kSpeakerPlaybackTaskStackBytes = 4 * 1024;
constexpr uint32_t kAudioFilePlaybackTaskStackBytes = 8 * 1024;
constexpr UBaseType_t kSpeakerPlaybackTaskPriority = 3;
constexpr uint32_t kPausedAudioReadyTimeoutMs = 1000;
constexpr uint32_t kPausedAudioReadyPollMs = 10;
constexpr uint32_t kSpeakerPlaybackSampleRateHz = 44100;
constexpr uint8_t kSpeakerPlaybackChannelCount = 2;
constexpr uint8_t kSpeakerPlaybackBitsPerSample = 16;
constexpr uint32_t kMicrophoneCaptureTaskStackBytes = 4 * 1024;
constexpr UBaseType_t kMicrophoneCaptureTaskPriority = 3;
constexpr size_t kMicrophoneReadSampleCount = 128;
constexpr uint32_t kMicrophoneReadRetryDelayMs = 10;
// SurvivalOS: four times the vendor's values, matching the 12 dB more gain.
constexpr int kMicrophoneAverageFullScale = 4000;
constexpr int kMicrophonePeakFullScale = 16000;
constexpr int kMicrophoneLevelRiseDivisor = 4;
constexpr int kMicrophoneLevelFallDivisor = 8;
// The codec runs the capture path with the same slot layout as playback, so a
// recording is written as 16-bit stereo at whatever rate the codec is clocked
// at. Once the wiring is confirmed on hardware this can drop to mono.
constexpr uint8_t kRecordingChannelCount = 2;
constexpr uint8_t kRecordingBitsPerSample = 16;
constexpr size_t kWavHeaderBytes = 44;

/**
 * @brief Writes a 44-byte canonical WAV header.
 * @param file Open file positioned at the start.
 * @param sample_rate_hz Sample rate in hertz.
 * @param channel_count Number of interleaved channels.
 * @param bits_per_sample Bits in one sample.
 * @param payload_bytes Size of the PCM payload; may be zero for a placeholder.
 * @return true when the whole header was written.
 */
bool WriteWavHeader(FILE* file, uint32_t sample_rate_hz, uint8_t channel_count,
    uint8_t bits_per_sample, uint32_t payload_bytes) {
  const uint32_t byte_rate =
      sample_rate_hz * channel_count * (bits_per_sample / 8);
  const uint16_t block_align = channel_count * (bits_per_sample / 8);
  uint8_t header[kWavHeaderBytes] = {};
  auto put32 = [&header](size_t offset, uint32_t value) {
    header[offset] = static_cast<uint8_t>(value);
    header[offset + 1] = static_cast<uint8_t>(value >> 8);
    header[offset + 2] = static_cast<uint8_t>(value >> 16);
    header[offset + 3] = static_cast<uint8_t>(value >> 24);
  };
  auto put16 = [&header](size_t offset, uint16_t value) {
    header[offset] = static_cast<uint8_t>(value);
    header[offset + 1] = static_cast<uint8_t>(value >> 8);
  };
  std::memcpy(header, "RIFF", 4);
  put32(4, payload_bytes + 36);
  std::memcpy(header + 8, "WAVEfmt ", 8);
  put32(16, 16);
  put16(20, 1);
  put16(22, channel_count);
  put32(24, sample_rate_hz);
  put32(28, byte_rate);
  put16(32, block_align);
  put16(34, bits_per_sample);
  std::memcpy(header + 36, "data", 4);
  put32(40, payload_bytes);
  return std::fwrite(header, 1, sizeof(header), file) == sizeof(header);
}

}  // namespace

bool TDisplayP4Device::PlaySpeakerTone(size_t* bytes_written) {
  if (bytes_written != nullptr) {
    *bytes_written = 0;
  }

  if (!Configure(kSpeakerPlaybackSampleRateHz, kSpeakerPlaybackChannelCount,
          kSpeakerPlaybackBitsPerSample)) {
    LogMessage(
        LogLevel::kWarning, __FILE__, __LINE__, "Es8311 init retry failed\n");
    return false;
  }

  const auto* audio_data = reinterpret_cast<const uint8_t*>(c2_b16_s44100);
  const size_t audio_size = sizeof(c2_b16_s44100);
  speaker_.total_bytes.store(audio_size);
  const size_t frame_size =
      (kSpeakerPlaybackBitsPerSample / 8) * kSpeakerPlaybackChannelCount;
  const size_t duration_ms =
      ((audio_size / frame_size) * 1000U) / kSpeakerPlaybackSampleRateHz;

  LogMessage(LogLevel::kDebug, __FILE__, __LINE__,
      "ES8311 speaker playback: bytes=%u, sample_rate=%u, channels=%u, "
      "duration=%u ms\n",
      static_cast<unsigned int>(audio_size),
      static_cast<unsigned int>(kSpeakerPlaybackSampleRateHz),
      static_cast<unsigned int>(kSpeakerPlaybackChannelCount),
      static_cast<unsigned int>(duration_ms));

  size_t total_written = 0;
  while (total_written < audio_size) {
    const size_t write_size =
        std::min(kSpeakerPlaybackChunkBytes, audio_size - total_written);
    const size_t written =
        driver_.chip().es8311->WriteI2s(audio_data + total_written, write_size);
    if (written == 0) {
      LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
          "ES8311 WriteI2s failed, written=%u/%u\n",
          static_cast<unsigned int>(total_written),
          static_cast<unsigned int>(audio_size));
      return false;
    }
    total_written += written;
    if (bytes_written != nullptr) {
      *bytes_written = total_written;
    }
    speaker_.bytes_written.store(total_written);
  }

  return true;
}

bool TDisplayP4Device::StartSpeakerTone() {
  if (speaker_.running.load()) {
    return StartPausedAudioSpeakerTone(false);
  }

  if (!TryAcquireAuxiliaryAudioOutput(AuxiliaryAudioOutput::kSpeakerTone)) {
    return false;
  }

  bool expected = false;
  if (!speaker_.running.compare_exchange_strong(expected, true)) {
    ReleaseAuxiliaryAudioOutput(AuxiliaryAudioOutput::kSpeakerTone);
    return false;
  }

  speaker_.completed.store(false);
  speaker_.success.store(false);
  speaker_.bytes_written.store(0);
  speaker_.total_bytes.store(sizeof(c2_b16_s44100));
  speaker_.loop_enabled.store(false);
  speaker_.stop_requested.store(false);
  speaker_.paused.store(false);
  speaker_.playback_kind.store(SpeakerState::PlaybackKind::kTone);

  const BaseType_t result = xTaskCreate(SpeakerPlaybackTaskEntry,
      "speaker_play", kSpeakerPlaybackTaskStackBytes, this,
      kSpeakerPlaybackTaskPriority, nullptr);
  if (result != pdPASS) {
    speaker_.running.store(false);
    speaker_.completed.store(true);
    speaker_.playback_kind.store(SpeakerState::PlaybackKind::kNone);
    ReleaseAuxiliaryAudioOutput(AuxiliaryAudioOutput::kSpeakerTone);
    return false;
  }

  return true;
}

bool TDisplayP4Device::StartSpeakerToneLoop() {
  if (speaker_.tone_overlay_running.load()) {
    return speaker_.tone_overlay_loop_enabled.load();
  }
  if (speaker_.running.load()) {
    if (speaker_.playback_kind.load() ==
        SpeakerState::PlaybackKind::kToneLoop) {
      return true;
    }
    return StartPausedAudioSpeakerTone(true);
  }
  if (!TryAcquireAuxiliaryAudioOutput(AuxiliaryAudioOutput::kSpeakerTone)) {
    return false;
  }
  speaker_.loop_enabled.store(true);
  speaker_.stop_requested.store(false);

  bool expected = false;
  if (!speaker_.running.compare_exchange_strong(expected, true)) {
    ReleaseAuxiliaryAudioOutput(AuxiliaryAudioOutput::kSpeakerTone);
    return speaker_.playback_kind.load() ==
           SpeakerState::PlaybackKind::kToneLoop;
  }

  speaker_.completed.store(false);
  speaker_.success.store(false);
  speaker_.bytes_written.store(0);
  speaker_.total_bytes.store(sizeof(c2_b16_s44100));
  speaker_.paused.store(false);
  speaker_.playback_kind.store(SpeakerState::PlaybackKind::kToneLoop);

  const BaseType_t result = xTaskCreate(SpeakerPlaybackTaskEntry,
      "speaker_loop", kSpeakerPlaybackTaskStackBytes, this,
      kSpeakerPlaybackTaskPriority, nullptr);
  if (result != pdPASS) {
    speaker_.running.store(false);
    speaker_.completed.store(true);
    speaker_.loop_enabled.store(false);
    speaker_.playback_kind.store(SpeakerState::PlaybackKind::kNone);
    ReleaseAuxiliaryAudioOutput(AuxiliaryAudioOutput::kSpeakerTone);
    return false;
  }

  return true;
}

bool TDisplayP4Device::StopSpeakerToneLoop() {
  if (speaker_.tone_overlay_running.load() &&
      speaker_.tone_overlay_loop_enabled.load()) {
    speaker_.tone_overlay_stop_requested.store(true);
    speaker_.tone_overlay_loop_enabled.store(false);
    return true;
  }
  if (speaker_.playback_kind.load() != SpeakerState::PlaybackKind::kToneLoop) {
    return false;
  }
  speaker_.stop_requested.store(true);
  speaker_.loop_enabled.store(false);
  return true;
}

bool TDisplayP4Device::SetSpeakerVolumePercent(int percent) {
  if (!driver_.IsEs8311Ready() && !driver_.InitEs8311()) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__, "Es8311 init failed\n");
    return false;
  }

  const int clamped_percent = std::clamp(percent, 0, 100);
  const uint8_t volume = clamped_percent == 0
      ? 0
      : static_cast<uint8_t>(kAudioVolumeRegisterAtOnePercent +
            (kAudioVolumeRegisterAtFull - kAudioVolumeRegisterAtOnePercent) *
                (clamped_percent - 1) / 99);
  const bool result = driver_.chip().es8311->SetDacVolume(volume);
  UpdateAudioCodecOperatingMode();
  return result;
}

bool TDisplayP4Device::ReadSpeakerToneStatus(SpeakerStatus* status) {
  if (status == nullptr) {
    return false;
  }

  const SpeakerState::PlaybackKind playback_kind =
      speaker_.playback_kind.load();
  const bool audio_file_running =
      playback_kind == SpeakerState::PlaybackKind::kAudioFile;
  status->running = speaker_.tone_overlay_running.load() ||
                    (speaker_.running.load() && !audio_file_running);
  status->completed = speaker_.completed.load();
  status->success = speaker_.success.load();
  status->bytes_written = speaker_.bytes_written.load();
  status->total_bytes = speaker_.total_bytes.load();
  return true;
}

bool TDisplayP4Device::StartAudioFile(const char* path, uint32_t duration_ms) {
  if (path == nullptr || path[0] == '\0') {
    return false;
  }
  const AuxiliaryAudioOutput auxiliary_output =
      speaker_.auxiliary_output.load();
  if (auxiliary_output == AuxiliaryAudioOutput::kMicrophoneLoopback ||
      speaker_.tone_overlay_running.load()) {
    return false;
  }
  if (speaker_.running.load()) {
    speaker_.stop_requested.store(true);
    speaker_.paused.store(false);
    for (int retry = 0; retry < 100 && speaker_.running.load(); ++retry) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (speaker_.running.load()) {
      return false;
    }
  }
  if (speaker_.auxiliary_output.load() != AuxiliaryAudioOutput::kNone) {
    return false;
  }

  bool expected = false;
  if (!speaker_.running.compare_exchange_strong(expected, true)) {
    return false;
  }
  if (speaker_.auxiliary_output.load() != AuxiliaryAudioOutput::kNone) {
    speaker_.running.store(false);
    return false;
  }
  std::snprintf(
      speaker_.audio_file_path, sizeof(speaker_.audio_file_path), "%s", path);
  speaker_.loop_enabled.store(false);
  speaker_.stop_requested.store(false);
  speaker_.paused.store(false);
  speaker_.elapsed_ms.store(0);
  speaker_.duration_ms.store(duration_ms);
  speaker_.seek_requested.store(false);
  speaker_.seek_position_ms.store(0);
  speaker_.file_state.store(AudioFilePlaybackState::kPlaying);
  speaker_.playback_kind.store(SpeakerState::PlaybackKind::kAudioFile);

  const BaseType_t result = xTaskCreate(SpeakerPlaybackTaskEntry, "audio_file",
      kAudioFilePlaybackTaskStackBytes, this, kSpeakerPlaybackTaskPriority,
      nullptr);
  if (result != pdPASS) {
    speaker_.running.store(false);
    speaker_.file_state.store(AudioFilePlaybackState::kError);
    speaker_.playback_kind.store(SpeakerState::PlaybackKind::kNone);
    return false;
  }
  return true;
}

bool TDisplayP4Device::PauseAudioFile() {
  if (!speaker_.running.load() ||
      speaker_.playback_kind.load() != SpeakerState::PlaybackKind::kAudioFile ||
      speaker_.file_state.load() != AudioFilePlaybackState::kPlaying) {
    return false;
  }
  speaker_.pause_acknowledged.store(false);
  speaker_.paused.store(true);
  speaker_.file_state.store(AudioFilePlaybackState::kPaused);
  return true;
}

bool TDisplayP4Device::ResumeAudioFile() {
  if (!speaker_.running.load() ||
      speaker_.playback_kind.load() != SpeakerState::PlaybackKind::kAudioFile ||
      speaker_.auxiliary_output.load() != AuxiliaryAudioOutput::kNone ||
      speaker_.file_state.load() != AudioFilePlaybackState::kPaused) {
    return false;
  }
  speaker_.paused.store(false);
  speaker_.file_state.store(AudioFilePlaybackState::kPlaying);
  return true;
}

bool TDisplayP4Device::SeekAudioFile(uint32_t position_ms) {
  if (!speaker_.running.load() ||
      speaker_.playback_kind.load() != SpeakerState::PlaybackKind::kAudioFile) {
    return false;
  }
  const uint32_t duration_ms = speaker_.duration_ms.load();
  if (duration_ms == 0) {
    return false;
  }
  const uint32_t clamped_position_ms = std::min(position_ms, duration_ms);
  speaker_.seek_position_ms.store(clamped_position_ms);
  speaker_.seek_requested.store(true);
  speaker_.elapsed_ms.store(clamped_position_ms);
  return true;
}

bool TDisplayP4Device::StopAudioFile() {
  if (speaker_.playback_kind.load() != SpeakerState::PlaybackKind::kAudioFile) {
    return false;
  }
  speaker_.stop_requested.store(true);
  if (!speaker_.tone_overlay_running.load()) {
    speaker_.paused.store(false);
  }
  speaker_.file_state.store(AudioFilePlaybackState::kStopped);
  return true;
}

bool TDisplayP4Device::ReadAudioFileStatus(AudioFilePlaybackStatus* status) {
  if (status == nullptr) {
    return false;
  }
  status->state = speaker_.file_state.load();
  status->elapsed_ms = speaker_.elapsed_ms.load();
  status->duration_ms = speaker_.duration_ms.load();
  return true;
}

void TDisplayP4Device::SpeakerPlaybackTaskEntry(void* context) {
  auto* self = static_cast<TDisplayP4Device*>(context);
  if (self != nullptr) {
    self->RunSpeakerPlaybackTask();
  }
  vTaskDelete(nullptr);
}

void TDisplayP4Device::PausedAudioSpeakerToneTaskEntry(void* context) {
  auto* self = static_cast<TDisplayP4Device*>(context);
  if (self != nullptr) {
    self->RunPausedAudioSpeakerToneTask();
  }
  vTaskDelete(nullptr);
}

void TDisplayP4Device::RunSpeakerPlaybackTask() {
  if (speaker_.playback_kind.load() ==
      SpeakerState::PlaybackKind::kAudioFile) {
    // Pick the decoder by extension: voice memos are WAV, music is MP3.
    const size_t path_length = std::strlen(speaker_.audio_file_path);
    const bool is_wav = path_length >= 4 &&
        strcasecmp(speaker_.audio_file_path + path_length - 4, ".wav") == 0;
    const audio::PcmPlaybackResult result = is_wav
        ? audio::PlayWavFile(speaker_.audio_file_path, this)
        : audio::PlayMp3File(speaker_.audio_file_path, this);
    const bool completed = result == audio::PcmPlaybackResult::kCompleted;
    const bool stopped = result == audio::PcmPlaybackResult::kStopped;
    speaker_.paused.store(false);
    speaker_.stop_requested.store(false);
    speaker_.seek_requested.store(false);
    speaker_.file_state.store(completed
                                  ? AudioFilePlaybackState::kCompleted
                                  : (stopped ? AudioFilePlaybackState::kStopped
                                             : AudioFilePlaybackState::kError));
    speaker_.playback_kind.store(SpeakerState::PlaybackKind::kNone);
    speaker_.running.store(false);
    UpdateAudioCodecOperatingMode();
    return;
  }

  size_t bytes_written = 0;
  bool played = false;
  do {
    size_t current_written = 0;
    played = PlaySpeakerTone(&current_written) || played;
    bytes_written += current_written;
    speaker_.bytes_written.store(bytes_written);
  } while (speaker_.loop_enabled.load() && !speaker_.stop_requested.load());
  speaker_.success.store(played);
  speaker_.completed.store(true);
  speaker_.loop_enabled.store(false);
  speaker_.stop_requested.store(false);
  speaker_.playback_kind.store(SpeakerState::PlaybackKind::kNone);
  ReleaseAuxiliaryAudioOutput(AuxiliaryAudioOutput::kSpeakerTone);
  speaker_.running.store(false);
  UpdateAudioCodecOperatingMode();
}

bool TDisplayP4Device::StartPausedAudioSpeakerTone(bool loop_enabled) {
  if (speaker_.playback_kind.load() != SpeakerState::PlaybackKind::kAudioFile ||
      speaker_.file_state.load() != AudioFilePlaybackState::kPaused) {
    return false;
  }

  if (!TryAcquireAuxiliaryAudioOutput(AuxiliaryAudioOutput::kSpeakerTone)) {
    return false;
  }

  bool expected = false;
  if (!speaker_.tone_overlay_running.compare_exchange_strong(expected, true)) {
    ReleaseAuxiliaryAudioOutput(AuxiliaryAudioOutput::kSpeakerTone);
    return false;
  }
  speaker_.tone_overlay_loop_enabled.store(loop_enabled);
  speaker_.tone_overlay_stop_requested.store(false);
  speaker_.completed.store(false);
  speaker_.success.store(false);
  speaker_.bytes_written.store(0);
  speaker_.total_bytes.store(sizeof(c2_b16_s44100));

  const BaseType_t result = xTaskCreate(PausedAudioSpeakerToneTaskEntry,
      "speaker_overlay", kSpeakerPlaybackTaskStackBytes, this,
      kSpeakerPlaybackTaskPriority, nullptr);
  if (result != pdPASS) {
    speaker_.tone_overlay_loop_enabled.store(false);
    speaker_.tone_overlay_running.store(false);
    speaker_.completed.store(true);
    ReleaseAuxiliaryAudioOutput(AuxiliaryAudioOutput::kSpeakerTone);
    return false;
  }
  return true;
}

void TDisplayP4Device::RunPausedAudioSpeakerToneTask() {
  const bool pause_ready = WaitForPausedAudioFile();

  const uint32_t paused_sample_rate_hz = speaker_.sample_rate_hz.load();
  size_t total_bytes_written = 0;
  bool played = false;
  if (pause_ready && !speaker_.tone_overlay_stop_requested.load() &&
      !speaker_.stop_requested.load()) {
    do {
      size_t bytes_written = 0;
      played = PlaySpeakerTone(&bytes_written) || played;
      total_bytes_written += bytes_written;
      speaker_.bytes_written.store(total_bytes_written);
    } while (speaker_.tone_overlay_loop_enabled.load() &&
             !speaker_.tone_overlay_stop_requested.load() &&
             !speaker_.stop_requested.load());
  }
  const bool output_restored =
      !pause_ready ||
      Configure(paused_sample_rate_hz, kSpeakerPlaybackChannelCount,
          kSpeakerPlaybackBitsPerSample);
  speaker_.bytes_written.store(total_bytes_written);
  speaker_.success.store(played && output_restored);
  speaker_.completed.store(true);
  speaker_.tone_overlay_loop_enabled.store(false);
  speaker_.tone_overlay_stop_requested.store(false);
  speaker_.tone_overlay_running.store(false);
  ReleaseAuxiliaryAudioOutput(AuxiliaryAudioOutput::kSpeakerTone);
  if (speaker_.stop_requested.load()) {
    speaker_.paused.store(false);
  }
}

bool TDisplayP4Device::TryAcquireAuxiliaryAudioOutput(
    AuxiliaryAudioOutput output) {
  AuxiliaryAudioOutput expected = AuxiliaryAudioOutput::kNone;
  return output != AuxiliaryAudioOutput::kNone &&
         speaker_.auxiliary_output.compare_exchange_strong(expected, output);
}

void TDisplayP4Device::ReleaseAuxiliaryAudioOutput(
    AuxiliaryAudioOutput output) {
  speaker_.auxiliary_output.compare_exchange_strong(
      output, AuxiliaryAudioOutput::kNone);
}

bool TDisplayP4Device::WaitForPausedAudioFile() {
  for (uint32_t elapsed_ms = 0; elapsed_ms < kPausedAudioReadyTimeoutMs;
      elapsed_ms += kPausedAudioReadyPollMs) {
    if (!speaker_.running.load() || !speaker_.paused.load() ||
        speaker_.stop_requested.load() ||
        speaker_.playback_kind.load() !=
            SpeakerState::PlaybackKind::kAudioFile ||
        speaker_.file_state.load() != AudioFilePlaybackState::kPaused) {
      return false;
    }
    if (speaker_.pause_acknowledged.load()) {
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(kPausedAudioReadyPollMs));
  }
  return speaker_.running.load() && speaker_.paused.load() &&
         !speaker_.stop_requested.load() &&
         speaker_.playback_kind.load() ==
             SpeakerState::PlaybackKind::kAudioFile &&
         speaker_.file_state.load() == AudioFilePlaybackState::kPaused &&
         speaker_.pause_acknowledged.load();
}

bool TDisplayP4Device::UpdateAudioCodecOperatingMode() {
  using OperatingMode =
      lilygo_device_driver::TDisplayP4Driver::Es8311OperatingMode;
  const bool playback_active = speaker_.running.load();
  const bool capture_active = microphone_.running.load();
  OperatingMode mode = OperatingMode::kSleep;
  if (playback_active && capture_active) {
    mode = OperatingMode::kDuplex;
  } else if (playback_active) {
    mode = OperatingMode::kPlayback;
  } else if (capture_active) {
    mode = microphone_.adc_to_dac_enabled.load() ? OperatingMode::kDuplex
                                                 : OperatingMode::kCapture;
  }
  if (!driver_.SetEs8311OperatingMode(mode)) {
    return false;
  }
  if (capture_active && driver_.IsEs8311Ready()) {
    // Set again on every mode change in case the driver restored its default.
    driver_.chip().es8311->SetAdcGain(kMicrophoneDigitalGain);
  }
  return true;
}

bool TDisplayP4Device::Configure(
    uint32_t sample_rate_hz, uint8_t channel_count, uint8_t bits_per_sample) {
  if ((channel_count != 1 && channel_count != 2) ||
      bits_per_sample != kSpeakerPlaybackBitsPerSample) {
    return false;
  }
  const bool codec_was_ready = driver_.IsEs8311Ready();
  if (!UpdateAudioCodecOperatingMode()) {
    return false;
  }
  if (codec_was_ready && speaker_.sample_rate_hz.load() == sample_rate_hz) {
    return true;
  }

  // ESP-IDF 只允许在 I2S 通道禁用时重配标准模式时钟。
  if (!driver_.chip().es8311->SetI2sChannelEnable(false)) {
    return false;
  }
  const bool clock_reconfigured = driver_.chip().es8311->SetClockReconfig(
      device::es8311::kMclkMultiple, sample_rate_hz);
  const bool channels_restored =
      driver_.chip().es8311->SetI2sChannelEnable(true);
  if (!clock_reconfigured || !channels_restored) {
    return false;
  }
  speaker_.sample_rate_hz.store(sample_rate_hz);
  return true;
}

bool TDisplayP4Device::WaitUntilReady() {
  if (speaker_.paused.load() && !speaker_.stop_requested.load()) {
    speaker_.pause_acknowledged.store(true);
  }
  while (speaker_.paused.load() && !speaker_.stop_requested.load()) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  speaker_.pause_acknowledged.store(false);
  return !speaker_.stop_requested.load();
}

bool TDisplayP4Device::TakeSeekRequest(uint32_t* position_ms) {
  if (position_ms == nullptr || !speaker_.seek_requested.exchange(false)) {
    return false;
  }
  *position_ms = speaker_.seek_position_ms.load();
  return true;
}

bool TDisplayP4Device::Write(const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0 || !driver_.IsEs8311Ready()) {
    return false;
  }
  size_t total_written = 0;
  while (total_written < size) {
    if (!WaitUntilReady()) {
      return false;
    }
    const size_t write_size =
        std::min(kSpeakerPlaybackChunkBytes, size - total_written);
    const size_t written =
        driver_.chip().es8311->WriteI2s(data + total_written, write_size);
    if (written == 0) {
      return false;
    }
    total_written += written;
  }
  return true;
}

void TDisplayP4Device::UpdateProgress(uint32_t elapsed_ms) {
  const uint32_t duration_ms = speaker_.duration_ms.load();
  speaker_.elapsed_ms.store(
      duration_ms == 0 ? elapsed_ms : std::min(elapsed_ms, duration_ms));
}

bool TDisplayP4Device::StartMicrophone() {
  bool expected = false;
  if (!microphone_.running.compare_exchange_strong(expected, true)) {
    return !microphone_.stop_requested.load();
  }

  microphone_.stop_requested.store(false);
  microphone_.level_percent.store(0);
  microphone_.peak_sample.store(0);
  microphone_.bytes_read.store(0);
  if (!SetAudioAdcToDac(false)) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Failed to activate the ES8311 microphone capture path\n");
    microphone_.running.store(false);
    UpdateAudioCodecOperatingMode();
    return false;
  }

  const BaseType_t result = xTaskCreate(MicrophoneCaptureTaskEntry,
      "mic_capture", kMicrophoneCaptureTaskStackBytes, this,
      kMicrophoneCaptureTaskPriority, nullptr);
  if (result != pdPASS) {
    microphone_.running.store(false);
    microphone_.stop_requested.store(true);
    UpdateAudioCodecOperatingMode();
    return false;
  }

  return true;
}

bool TDisplayP4Device::StopMicrophone() {
  microphone_.stop_requested.store(true);
  microphone_.level_percent.store(0);
  microphone_.peak_sample.store(0);
  if (!driver_.IsEs8311Ready()) {
    microphone_.adc_to_dac_enabled.store(false);
    ReleaseAuxiliaryAudioOutput(AuxiliaryAudioOutput::kMicrophoneLoopback);
    return true;
  }
  return SetAudioAdcToDac(false);
}

bool TDisplayP4Device::SetAudioAdcToDac(bool enable) {
  if (enable && microphone_.adc_to_dac_enabled.load()) {
    return true;
  }
  if (enable && !microphone_.running.load()) {
    return false;
  }

  if (enable) {
    if (!TryAcquireAuxiliaryAudioOutput(
            AuxiliaryAudioOutput::kMicrophoneLoopback)) {
      return false;
    }
    if (speaker_.running.load() && !WaitForPausedAudioFile()) {
      ReleaseAuxiliaryAudioOutput(AuxiliaryAudioOutput::kMicrophoneLoopback);
      return false;
    }
  }

  const bool previous_enabled = microphone_.adc_to_dac_enabled.exchange(enable);
  if (!UpdateAudioCodecOperatingMode() || !driver_.IsEs8311Ready()) {
    microphone_.adc_to_dac_enabled.store(previous_enabled);
    if (enable) {
      ReleaseAuxiliaryAudioOutput(AuxiliaryAudioOutput::kMicrophoneLoopback);
    }
    UpdateAudioCodecOperatingMode();
    return false;
  }

  if (!enable) {
    ReleaseAuxiliaryAudioOutput(AuxiliaryAudioOutput::kMicrophoneLoopback);
  }

  return true;
}

bool TDisplayP4Device::ReadMicrophoneStatus(MicrophoneStatus* status) {
  if (status == nullptr) {
    return false;
  }

  status->running = microphone_.running.load();
  status->adc_to_dac_enabled = microphone_.adc_to_dac_enabled.load();
  status->level_percent = microphone_.level_percent.load();
  status->peak_sample = microphone_.peak_sample.load();
  status->bytes_read = microphone_.bytes_read.load();
  return true;
}

void TDisplayP4Device::MicrophoneCaptureTaskEntry(void* context) {
  auto* self = static_cast<TDisplayP4Device*>(context);
  if (self != nullptr) {
    self->RunMicrophoneCaptureTask();
  }
  vTaskDelete(nullptr);
}

void TDisplayP4Device::RunMicrophoneCaptureTask() {
  std::array<int16_t, kMicrophoneReadSampleCount> samples = {};
  // The capture task is the only owner of the recording file, so no locking is
  // needed between it and the UI thread.
  FILE* recording_file = nullptr;
  size_t recording_payload_bytes = 0;
  const uint32_t recording_sample_rate = speaker_.sample_rate_hz.load();

  if (microphone_.recording_requested.load()) {
    recording_file = std::fopen(microphone_.recording_path, "wb");
    if (recording_file == nullptr ||
        !WriteWavHeader(recording_file, recording_sample_rate,
            kRecordingChannelCount, kRecordingBitsPerSample, 0)) {
      LogMessage(LogLevel::kError, __FILE__, __LINE__,
          "Open recording file failed: %s\n", microphone_.recording_path);
      if (recording_file != nullptr) {
        std::fclose(recording_file);
        recording_file = nullptr;
      }
      microphone_.recording_failed.store(true);
      microphone_.recording_requested.store(false);
    } else {
      microphone_.recorded_bytes.store(0);
      microphone_.recording_failed.store(false);
      microphone_.recording_sample_rate_hz.store(recording_sample_rate);
      microphone_.recording_started_ms.store(
          static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS));
      microphone_.recording_active.store(true);
    }
  }

  while (!microphone_.stop_requested.load()) {
    const size_t read_bytes = driver_.chip().es8311->ReadI2s(
        samples.data(), samples.size() * sizeof(samples[0]));
    if (read_bytes > 0) {
      microphone_.bytes_read.fetch_add(read_bytes);

      int peak_sample = 0;
      int64_t absolute_sum = 0;
      const size_t sample_count = read_bytes / sizeof(samples[0]);
      for (size_t i = 0; i < sample_count && i < samples.size(); ++i) {
        const int sample = samples[i];
        const int absolute_sample = sample < 0 ? -sample : sample;
        absolute_sum += absolute_sample;
        peak_sample = std::max(peak_sample, absolute_sample);
      }

      const int average_sample =
          sample_count == 0 ? 0 : absolute_sum / static_cast<int>(sample_count);
      const int average_level_percent =
          std::min(100, (average_sample * 100) / kMicrophoneAverageFullScale);
      const int peak_level_percent =
          std::min(100, (peak_sample * 100) / kMicrophonePeakFullScale);
      const int target_level_percent =
          std::max(average_level_percent, peak_level_percent);
      const int current_level_percent = microphone_.level_percent.load();
      const int difference = target_level_percent - current_level_percent;
      const int divisor = difference > 0 ? kMicrophoneLevelRiseDivisor
                                         : kMicrophoneLevelFallDivisor;
      int level_percent = current_level_percent + difference / divisor;
      if (level_percent == current_level_percent && difference != 0) {
        level_percent += difference > 0 ? 1 : -1;
      }
      microphone_.peak_sample.store(peak_sample);
      microphone_.level_percent.store(level_percent);

      if (recording_file != nullptr) {
        if (std::fwrite(samples.data(), 1, read_bytes, recording_file) !=
            read_bytes) {
          LogMessage(LogLevel::kError, __FILE__, __LINE__,
              "Write recording payload failed\n");
          microphone_.recording_failed.store(true);
          microphone_.recording_active.store(false);
          std::fclose(recording_file);
          recording_file = nullptr;
        } else {
          recording_payload_bytes += read_bytes;
          microphone_.recorded_bytes.store(recording_payload_bytes);
          microphone_.recording_elapsed_ms.store(
              static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS) -
              microphone_.recording_started_ms.load());
        }
      }

      if (microphone_.adc_to_dac_enabled.load()) {
        const auto* pcm_data = reinterpret_cast<const uint8_t*>(samples.data());
        size_t written_bytes = 0;
        while (written_bytes < read_bytes &&
               microphone_.adc_to_dac_enabled.load() &&
               !microphone_.stop_requested.load()) {
          const size_t written = driver_.chip().es8311->WriteI2s(
              pcm_data + written_bytes, read_bytes - written_bytes);
          if (written == 0) {
            LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
                "ES8311 microphone PCM loopback write failed\n");
            microphone_.adc_to_dac_enabled.store(false);
            ReleaseAuxiliaryAudioOutput(
                AuxiliaryAudioOutput::kMicrophoneLoopback);
            UpdateAudioCodecOperatingMode();
            break;
          }
          written_bytes += written;
        }
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(kMicrophoneReadRetryDelayMs));
    }
  }

  if (recording_file != nullptr) {
    // Patch the header now that the payload length is known.
    if (std::fseek(recording_file, 0, SEEK_SET) != 0 ||
        !WriteWavHeader(recording_file, recording_sample_rate,
            kRecordingChannelCount, kRecordingBitsPerSample,
            static_cast<uint32_t>(recording_payload_bytes))) {
      LogMessage(LogLevel::kError, __FILE__, __LINE__,
          "Finalise recording header failed\n");
      microphone_.recording_failed.store(true);
    }
    std::fclose(recording_file);
    recording_file = nullptr;
  }
  microphone_.recording_active.store(false);
  microphone_.recording_requested.store(false);

  microphone_.adc_to_dac_enabled.store(false);
  ReleaseAuxiliaryAudioOutput(AuxiliaryAudioOutput::kMicrophoneLoopback);
  microphone_.level_percent.store(0);
  microphone_.peak_sample.store(0);
  microphone_.running.store(false);
  UpdateAudioCodecOperatingMode();
}

bool TDisplayP4Device::StartAudioRecording(const char* path) {
  if (path == nullptr || path[0] == '\0') {
    return false;
  }
  if (microphone_.running.load() || microphone_.recording_active.load()) {
    return false;
  }
  if (std::strlen(path) >= sizeof(microphone_.recording_path)) {
    LogMessage(LogLevel::kError, __FILE__, __LINE__,
        "Recording path is too long: %s\n", path);
    return false;
  }

  // The codec has to be clocked before the capture task reads its rate.
  if (!Configure(kSpeakerPlaybackSampleRateHz, kRecordingChannelCount,
          kRecordingBitsPerSample)) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Configure codec for recording failed\n");
    return false;
  }

  std::snprintf(microphone_.recording_path,
      sizeof(microphone_.recording_path), "%s", path);
  microphone_.recording_failed.store(false);
  microphone_.recorded_bytes.store(0);
  microphone_.recording_elapsed_ms.store(0);
  microphone_.recording_requested.store(true);

  if (!StartMicrophone()) {
    microphone_.recording_requested.store(false);
    return false;
  }
  return true;
}

bool TDisplayP4Device::StopAudioRecording() {
  if (!microphone_.recording_active.load() &&
      !microphone_.recording_requested.load()) {
    return true;
  }
  return StopMicrophone();
}

bool TDisplayP4Device::ReadAudioRecordingStatus(AudioRecordingStatus* status) {
  if (status == nullptr) {
    return false;
  }

  *status = AudioRecordingStatus();
  if (microphone_.recording_failed.load()) {
    status->state = AudioRecordingState::kError;
  } else if (microphone_.recording_active.load()) {
    status->state = AudioRecordingState::kRecording;
  } else if (microphone_.recorded_bytes.load() > 0) {
    status->state = AudioRecordingState::kCompleted;
  } else {
    status->state = AudioRecordingState::kIdle;
  }
  status->elapsed_ms = microphone_.recording_elapsed_ms.load();
  status->bytes_written = microphone_.recorded_bytes.load();
  status->level_percent = microphone_.level_percent.load();
  status->sample_rate_hz = microphone_.recording_sample_rate_hz.load();
  status->channel_count = kRecordingChannelCount;
  status->bits_per_sample = kRecordingBitsPerSample;
  return true;
}

}  // namespace lilygo_box::hal
