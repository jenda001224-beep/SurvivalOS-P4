/*
 * @Description: 振动反馈播放与强度控制接口
 * @Author: LILYGO_L
 * @Date: 2026-05-14 00:20:00
 * @LastEditTime: 2026-05-14 00:20:00
 * @License: GPL 3.0
 */
#pragma once

#include <cstdint>

namespace lilygo_box::hal {

class HapticProvider {
 public:
  virtual ~HapticProvider() = default;

  /**
   * @brief 读取可用 RAM 触觉反馈波形数量
   * @param waveform_count 波形数量输出地址
   * @return 读取成功返回 true，否则返回 false
   */
  virtual bool ReadHapticWaveformCount(uint8_t* waveform_count) = 0;

  /**
   * @brief 播放指定触觉反馈波形
   * @param waveform_sequence_number RAM 波形 sequence 编号
   * @param loop_count 播放循环次数，范围 1~16
   * @param gain 振动增益，范围 0~255
   * @param auto_brake true 表示启用自动制动，false 表示关闭自动制动
   * @return 播放任务启动成功返回 true，否则返回 false
   */
  virtual bool PlayHapticWaveform(uint8_t waveform_sequence_number,
      uint8_t loop_count, uint8_t gain, bool auto_brake) = 0;

  /**
   * @brief Plays a waveform for a chosen time instead of the default short tick.
   *
   * SurvivalOS addition. PlayHapticWaveform always cuts the motor after a few
   * milliseconds, which makes every effect a click; this lets an effect run
   * long enough to be felt as a vibration. Boards that do not override it fall
   * back to the plain tick.
   *
   * @param waveform_sequence_number RAM waveform sequence number.
   * @param loop_count Loop count, 1..16 (16 repeats until the time is up).
   * @param gain Vibration gain, 0..255.
   * @param auto_brake Whether to brake the motor when the waveform ends.
   * @param play_ms How long to let the motor run before it is stopped.
   * @return true when playback was started.
   */
  virtual bool PlayHapticWaveformFor(uint8_t waveform_sequence_number,
      uint8_t loop_count, uint8_t gain, bool auto_brake, uint32_t play_ms) {
    (void)play_ms;
    return PlayHapticWaveform(
        waveform_sequence_number, loop_count, gain, auto_brake);
  }
};

}  // namespace lilygo_box::hal
