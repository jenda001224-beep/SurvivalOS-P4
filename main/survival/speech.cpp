/*
 * @Description: Offline text to speech through eSpeak NG
 * @License: GPL 3.0
 */
#include "survival/speech.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "base/logger.h"
#include "esp_heap_caps.h"
#include "espeak-ng/espeak_ng.h"
#include "espeak-ng/speak_lib.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace lilygo_box::survival {
namespace {

// eSpeak keeps deep call chains with sizeable local arrays; its task gets a
// generous stack, from PSRAM so internal memory is not touched.
constexpr uint32_t kSpeechStackBytes = 48 * 1024;
constexpr UBaseType_t kSpeechPriority = 3;
constexpr TickType_t kSpeechTimeout = pdMS_TO_TICKS(30000);

// eSpeak's state is global, so is the job handed to its task.
struct SpeechJob {
  enum class Kind { kOpen, kClose, kSynthesize } kind = Kind::kOpen;
  const char* text = nullptr;
  const char* voice = nullptr;
  const char* path = nullptr;
  FILE* file = nullptr;
  uint32_t samples = 0;
  bool ok = false;
  SemaphoreHandle_t done = nullptr;
};

SemaphoreHandle_t g_lock = nullptr;
SpeechJob* g_current = nullptr;

void WriteLe32(uint8_t* out, uint32_t value) {
  out[0] = value & 0xFF;
  out[1] = (value >> 8) & 0xFF;
  out[2] = (value >> 16) & 0xFF;
  out[3] = (value >> 24) & 0xFF;
}

void WriteWavHeader(FILE* file, uint32_t sample_rate, uint32_t samples) {
  uint8_t header[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E',
      'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0,
      16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
  WriteLe32(header + 4, 36 + samples * 2);
  WriteLe32(header + 24, sample_rate);
  WriteLe32(header + 28, sample_rate * 2);
  WriteLe32(header + 40, samples * 2);
  std::fseek(file, 0, SEEK_SET);
  std::fwrite(header, 1, sizeof(header), file);
}

int SynthCallback(short* samples, int count, espeak_EVENT* /*events*/) {
  SpeechJob* job = g_current;
  if (job == nullptr || job->file == nullptr) {
    return 1;  // abort
  }
  if (samples != nullptr && count > 0) {
    if (std::fwrite(samples, sizeof(short), static_cast<size_t>(count),
            job->file) != static_cast<size_t>(count)) {
      return 1;
    }
    job->samples += static_cast<uint32_t>(count);
  }
  return 0;
}

void RunJob(SpeechJob* job) {
  switch (job->kind) {
    case SpeechJob::Kind::kOpen: {
      espeak_ng_InitializePath(job->path);
      espeak_ng_ERROR_CONTEXT context = nullptr;
      espeak_ng_STATUS status = espeak_ng_Initialize(&context);
      if (status != ENS_OK) {
        char message[96] = {};
        espeak_ng_GetStatusCodeMessage(status, message, sizeof(message));
        LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
            "eSpeak init failed: %s\n", message);
        espeak_ng_ClearErrorContext(&context);
        job->ok = false;
        return;
      }
      status = espeak_ng_InitializeOutput(ENOUTPUT_MODE_SYNCHRONOUS, 0, nullptr);
      espeak_SetSynthCallback(SynthCallback);
      job->ok = status == ENS_OK;
      return;
    }
    case SpeechJob::Kind::kClose:
      espeak_ng_Terminate();
      job->ok = true;
      return;
    case SpeechJob::Kind::kSynthesize: {
      if (espeak_ng_SetVoiceByName(job->voice) != ENS_OK) {
        LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
            "eSpeak has no voice '%s'\n", job->voice);
        job->ok = false;
        return;
      }
      job->file = std::fopen(job->path, "wb");
      if (job->file == nullptr) {
        job->ok = false;
        return;
      }
      const int sample_rate = espeak_ng_GetSampleRate();
      WriteWavHeader(job->file, static_cast<uint32_t>(sample_rate), 0);
      job->samples = 0;
      g_current = job;
      const espeak_ng_STATUS status = espeak_ng_Synthesize(job->text,
          std::strlen(job->text) + 1, 0, POS_CHARACTER, 0,
          espeakCHARS_UTF8 | espeakENDPAUSE, nullptr, nullptr);
      espeak_ng_Synchronize();
      g_current = nullptr;
      WriteWavHeader(job->file, static_cast<uint32_t>(sample_rate), job->samples);
      const bool closed = std::fclose(job->file) == 0;
      job->file = nullptr;
      job->ok = status == ENS_OK && closed && job->samples > 0;
      return;
    }
  }
}

void SpeechTask(void* argument) {
  auto* job = static_cast<SpeechJob*>(argument);
  RunJob(job);
  xSemaphoreGive(job->done);
  vTaskDeleteWithCaps(nullptr);
}

bool Run(SpeechJob* job) {
  if (g_lock == nullptr) {
    g_lock = xSemaphoreCreateMutex();
    if (g_lock == nullptr) {
      return false;
    }
  }
  if (xSemaphoreTake(g_lock, kSpeechTimeout) != pdTRUE) {
    return false;
  }
  job->done = xSemaphoreCreateBinary();
  bool ok = false;
  if (job->done != nullptr &&
      xTaskCreatePinnedToCoreWithCaps(SpeechTask, "speech", kSpeechStackBytes,
          job, kSpeechPriority, nullptr, tskNO_AFFINITY,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) {
    // The task always finishes; waiting forever keeps the job alive for it.
    xSemaphoreTake(job->done, portMAX_DELAY);
    ok = job->ok;
  }
  if (job->done != nullptr) {
    vSemaphoreDelete(job->done);
    job->done = nullptr;
  }
  xSemaphoreGive(g_lock);
  return ok;
}

}  // namespace

Speech::~Speech() { Close(); }

bool Speech::Open(const char* data_directory) {
  if (open_) {
    return true;
  }
  if (data_directory == nullptr) {
    return false;
  }
  SpeechJob job;
  job.kind = SpeechJob::Kind::kOpen;
  job.path = data_directory;
  open_ = Run(&job);
  return open_;
}

void Speech::Close() {
  if (!open_) {
    return;
  }
  SpeechJob job;
  job.kind = SpeechJob::Kind::kClose;
  Run(&job);
  open_ = false;
}

bool Speech::SynthesizeToFile(const char* text, const char* voice,
    const char* wav_path, unsigned int* duration_ms) {
  if (!open_ || text == nullptr || text[0] == '\0' || voice == nullptr ||
      wav_path == nullptr) {
    return false;
  }
  SpeechJob job;
  job.kind = SpeechJob::Kind::kSynthesize;
  job.text = text;
  job.voice = voice;
  job.path = wav_path;
  const bool ok = Run(&job);
  if (duration_ms != nullptr) {
    const int rate = espeak_ng_GetSampleRate();
    *duration_ms = rate > 0 ? static_cast<unsigned int>(
        static_cast<uint64_t>(job.samples) * 1000U / static_cast<unsigned>(rate)) : 0;
  }
  return ok;
}

}  // namespace lilygo_box::survival
