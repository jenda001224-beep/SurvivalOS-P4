/*
 * @Description: WiFi 扫描、连接配置与状态管理接口
 * @Author: LILYGO_L
 * @Date: 2026-05-15 13:20:00
 * @LastEditTime: 2026-05-15 13:20:00
 * @License: GPL 3.0
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace lilygo_box::hal {

inline constexpr size_t kWifiSsidMaxLength = 32;
inline constexpr size_t kWifiPasswordMaxLength = 64;
inline constexpr size_t kMaxWifiScanNetworkCount = 16;
// 单次 SNTP 入网检测允许包含随机启动延时和网络重试的最长时间。
inline constexpr uint32_t kWifiInternetCheckTimeoutMs = 30 * 1000;

struct WifiNetworkInfo {
  // 热点 SSID，末尾保留字符串终止符。
  char ssid[kWifiSsidMaxLength + 1] = {};
  // 热点信号强度，单位为 dBm。
  int rssi = 0;
  // 热点所在信道。
  int channel = 0;
  // 热点是否需要密码。
  bool secure = false;
  // 热点是否位于 5 GHz 频段。
  bool is_5g = false;
};

struct WifiScanStatus {
  // WiFi 扫描是否正在进行。
  bool scan_running = false;
  // 最近一次扫描是否失败。
  bool scan_failed = false;
  // 最近一次 WiFi 扫描或连接错误码。
  int last_error = 0;
  // 扫描结果版本号，扫描完成后递增。
  uint32_t generation = 0;
  // 当前有效扫描结果数量。
  size_t network_count = 0;
  // 最近一次扫描得到的热点列表。
  WifiNetworkInfo networks[kMaxWifiScanNetworkCount] = {};
};

struct WifiStatus {
  // WiFi 初始化任务是否正在运行
  bool init_task_running = false;
  // WiFi 连接命令、热点关联或 DHCP 获取是否仍在进行。
  bool connect_task_running = false;
  // WiFi 驱动是否已经初始化完成
  bool driver_initialized = false;
  // WiFi 驱动是否已经启动
  bool running = false;
  // STA 是否已经关联热点
  bool connected = false;
  // 是否已经通过 DHCP 获取地址
  bool got_ip = false;
  // 启动或连接是否失败
  bool start_failed = false;
  // 获取时间测试是否正在运行
  bool time_test_running = false;
  // SNTP 时间同步是否已经启动
  bool time_sync_started = false;
  // SNTP 是否已经同步到有效时间
  bool time_synced = false;
  // 当前重试次数
  int retry_count = 0;
  // 最近一次错误码
  int last_error = 0;
  // 最近一次断开原因
  int disconnect_reason = 0;
  // 当前热点信号强度
  int rssi = 0;
  // 当前连接信道
  int channel = 0;
  // STA MAC 地址打包值
  uint64_t mac_address = 0;
  // DHCP IP 地址
  uint32_t ip_address = 0;
  // DHCP 子网掩码
  uint32_t netmask = 0;
  // DHCP 网关
  uint32_t gateway = 0;
  // 每次取得 DHCP 地址后递增，用于识别快速重连产生的新连接。
  uint32_t connection_generation = 0;
  // SNTP 获取到的 UTC Unix 时间戳
  int64_t unix_time = 0;
  // SNTP 最新一次网络同步距当前的秒数
  uint32_t time_sync_age_s = 0;
  // 当前连接或测试使用的 SSID
  char ssid[33] = {};
};

// SurvivalOS: which firmware answers on the wireless co-processor. Early
// T-Display-P4 units carry ESP-AT there, which ESP-Hosted cannot talk to; the
// phone can replace it with the ESP-Hosted firmware it carries.
enum class WifiCoprocessorFirmware : uint8_t {
  // Not looked at yet.
  kUnknown,
  // ESP-Hosted network adapter: Wi-Fi works.
  kHosted,
  // Factory ESP-AT: needs the one-time update.
  kEspAt,
  // The chip did not answer at all.
  kSilent,
};

enum class WifiCoprocessorUpdateStage : uint8_t {
  kIdle,
  kChecking,
  kEnteringDownloadMode,
  // Waiting for the user to hold the co-processor's BOOT button.
  kNeedsBootButton,
  kConnecting,
  kWriting,
  kVerifying,
  kDone,
  kFailed,
};

enum class WifiCoprocessorUpdateMode : uint8_t {
  // Only find out which firmware is there.
  kIdentify,
  // Go all the way into the co-processor's download mode and read its flash
  // size, but write nothing.
  kRehearse,
  // Write the bundled firmware.
  kInstall,
};

struct WifiCoprocessorStatus {
  WifiCoprocessorFirmware firmware = WifiCoprocessorFirmware::kUnknown;
  WifiCoprocessorUpdateStage stage = WifiCoprocessorUpdateStage::kIdle;
  // Whether this build carries a firmware image for the co-processor.
  bool update_available = false;
  // Progress of the write, 0-100.
  uint8_t progress_percent = 0;
  // Firmware version of the co-processor, or what went wrong.
  char detail[120] = {};
};

class WifiProvider {
 public:
  virtual ~WifiProvider() = default;

  /**
   * @brief 启用或停止 hosted WiFi STA
   * @param enabled true 异步启用，false 立即请求停止
   * @return 状态切换请求成功返回 true，否则返回 false
   */
  virtual bool SetWifiEnabled(bool enabled) = 0;

  /**
   * @brief 启动 hosted WiFi STA 并异步扫描附近热点
   * @return 扫描命令发送成功返回 true，否则返回 false
   */
  virtual bool StartWifiScan() = 0;

  /**
   * @brief 读取最近一次 hosted WiFi 扫描状态与热点列表
   * @param status WiFi 扫描状态输出地址
   * @return 读取成功返回 true，否则返回 false
   */
  virtual bool ReadWifiScanStatus(WifiScanStatus* status) = 0;

  /**
   * @brief 连接指定 hosted WiFi 热点
   * @param ssid 热点 SSID
   * @param password 热点密码，开放网络可传空字符串或 nullptr
   * @return 连接命令发送成功返回 true，否则返回 false
   */
  virtual bool ConnectWifi(const char* ssid, const char* password) = 0;

  /**
   * @brief 取消当前 WiFi 连接等待状态并保持 WiFi 驱动开启
   * @return 取消成功或当前无需取消返回 true，否则返回 false
   */
  virtual bool CancelWifiConnection() = 0;

  /**
   * @brief 对当前已连接 WLAN 主动发起一次 SNTP 入网复检
   * @return 复检请求发送成功返回 true，否则返回 false
   */
  virtual bool RequestWifiInternetCheck() = 0;

  /**
   * @brief 临时打开 WiFi 并连接工厂测试热点获取网络时间
   * @return 启动命令发送成功返回 true，否则返回 false
   */
  virtual bool StartWifiTimeTest() = 0;

  /**
   * @brief 停止 WiFi 获取时间测试并恢复进入测试前的 WiFi 状态
   * @return 恢复命令发送成功返回 true，否则返回 false
   */
  virtual bool StopWifiTimeTest() = 0;

  /**
   * @brief 读取 hosted WiFi 连接、DHCP 和时间同步状态
   * @param status WiFi 状态输出地址
   * @return 读取成功返回 true，否则返回 false
   */
  virtual bool ReadWifiStatus(WifiStatus* status) = 0;

  /**
   * @brief Reads what is known about the co-processor's firmware and the
   *        progress of an update. Never touches the hardware.
   * @return false when the board has no such service
   */
  virtual bool ReadWifiCoprocessorStatus(WifiCoprocessorStatus* status) {
    (void)status;
    return false;
  }

  /**
   * @brief Identifies the co-processor's firmware and, unless it is only a
   *        check, replaces ESP-AT with the bundled ESP-Hosted firmware.
   * @param mode What to do; see WifiCoprocessorUpdateMode
   * @return true when the background task was started
   */
  virtual bool StartWifiCoprocessorUpdate(WifiCoprocessorUpdateMode mode) {
    (void)mode;
    return false;
  }

  /**
   * @brief Tells an update waiting in kNeedsBootButton that the button is
   *        being held, so it can reset the co-processor.
   */
  virtual bool ConfirmWifiCoprocessorBootButton() { return false; }

  /**
   * @brief Diagnostics: sends AT commands to a co-processor that runs ESP-AT
   *        and logs the replies.
   * @param script Commands separated by ';'. "!connect" tries to reach the
   *        ROM download mode at that point, "!wait" pauses half a second.
   * @return true when the background task was started
   */
  virtual bool StartWifiCoprocessorExperiment(const char* script) {
    (void)script;
    return false;
  }
};

}  // namespace lilygo_box::hal
