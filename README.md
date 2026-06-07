# 小智 AI 固件改造 — ES3C28P / Freenove 2.8寸 触摸版

## 只需要改的文件

| 文件 | 说明 |
|------|------|
| `main/display/lcd_display.h` | 加空闲主页控件 |
| `main/display/lcd_display.cc` | 实现空闲主页 + 天气API |
| `main/application.cc` | 加 4 行 `SetIdleMode` 调用 |

> **为什么没有板子定义文件？** 因为 Freenove 2.8 LCD 和你的 ES3C28P 硬件一模一样（ILI9341+ES8311+FT6336G），直接用它的板型就行。

## 功能

- **空闲时** → 显示大时钟 + 日期 + 天气（深色星空背景，沈阳于洪区）
- **对话时** → 正常聊天界面，说完自动回主页
- **触摸** → 单点切换对话，长按3秒进WiFi配置
- **天气** → 每30分钟自动刷新，免费 Open-Meteo API

## 怎么拿到 .bin 文件（GitHub 云编译）

### 第一步：注册 GitHub
去 [github.com](https://github.com) 注册账号（已有则跳过）

### 第二步：创建仓库
1. 点右上角 `+` → `New repository`
2. 仓库名填 `xiaozhi-es3c28p`
3. 选 **Public**，不要勾任何选项
4. 点 `Create repository`

### 第三步：上传文件

在电脑上打开 PowerShell，复制粘贴运行：

```powershell
# 1. 克隆你的空仓库
git clone https://github.com/你的用户名/xiaozhi-es3c28p.git
cd xiaozhi-es3c28p

# 2. 把我们改好的文件放进去
# 从 modified_firmware 文件夹把 .github/ 和 main/display/ 复制过来
# 然后：
git add .
git commit -m "添加空闲主页 + 沈阳天气"
git push
```

### 第四步：等编译完成
1. 进你的 GitHub 仓库页面
2. 点顶部的 `Actions` 标签
3. 左边点 `Build XiaoZhi ES3C28P Firmware`
4. 点右边 `Run workflow` → 绿色 `Run workflow` 按钮
5. 等大约 5-8 分钟（那个咖啡杯图标转完）
6. 刷新页面，点 `xiaozhi-es3c28p-firmware` 下载 zip
7. 里面有 `xiaozhi_es3c28p_v2.0.2.bin`

### 第五步：烧录
用你目录里的 `Flash_download\flash_download_tool_3.9.4\flash_download_tool_3.9.4.exe`
- 选 `ESP32-S3` 芯片
- 选固件文件 `xiaozhi_es3c28p_v2.0.2.bin`，地址填 `0x0`
- 勾上 `Erase Flash`（第一次烧录需要擦除）
- 点 `START`

## 改城市坐标

编辑 `main/display/lcd_display.cc`，找 `FetchWeatherFromApi()`：

```cpp
float lat = 41.77f;   // 沈阳于洪
float lon = 123.32f;
std::string city = "沈阳于洪";
```
