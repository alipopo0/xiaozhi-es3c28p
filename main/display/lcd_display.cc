#include "lcd_display.h"
#include "gif/lvgl_gif.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "assets/lang_config.h"

#include <vector>
#include <algorithm>
#include <cstring>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <esp_http_client.h>
#include <cJSON.h>

#include <src/misc/cache/lv_cache.h>

#include "board.h"

#define TAG "LcdDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_awesome_30_4);

// 天气刷新间隔 (30分钟)
#define WEATHER_REFRESH_INTERVAL_MS (30 * 60 * 1000)

/* ========== WMO 天气代码 → 中文描述 ========== */
static const char* wmo_code_to_desc(int code) {
    switch (code) {
        case 0: return "晴朗";
        case 1: return "大部晴";
        case 2: return "多云";
        case 3: return "阴天";
        case 45: case 48: return "雾";
        case 51: return "小毛毛雨";
        case 53: return "中毛毛雨";
        case 55: return "大毛毛雨";
        case 61: return "小雨";
        case 63: return "中雨";
        case 65: return "大雨";
        case 71: return "小雪";
        case 73: return "中雪";
        case 75: return "大雪";
        case 80: return "阵雨";
        case 81: return "中阵雨";
        case 82: return "大阵雨";
        case 95: return "雷暴";
        case 96: case 99: return "雷暴+冰雹";
        default: return "未知";
    }
}

/* WMO 天气代码 → 图标 */
static const char* wmo_code_to_icon(int code) {
    switch (code) {
        case 0: return "☀️";
        case 1: return "🌤️";
        case 2: return "⛅";
        case 3: return "☁️";
        case 45: case 48: return "🌫️";
        case 51: case 53: case 55: return "🌦️";
        case 61: case 63: case 65: return "🌧️";
        case 71: case 73: case 75: return "🌨️";
        case 80: case 81: case 82: return "🌦️";
        case 95: case 96: case 99: return "⛈️";
        default: return "❓";
    }
}

/* ========== 从 Open-Meteo API 获取天气 ========== */
static bool fetch_weather_from_openmeteo(WeatherData& weather, float lat, float lon) {
    char url[256];
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast?latitude=%.2f&longitude=%.2f"
        "&current_weather=true&timezone=Asia%%2FShanghai",
        lat, lon);

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 10000;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return false;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    // 读取响应
    std::string response;
    char buffer[128];
    int read_len;
    while ((read_len = esp_http_client_read(client, buffer, sizeof(buffer))) > 0) {
        response.append(buffer, read_len);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (response.empty()) return false;

    // 解析 JSON
    cJSON* root = cJSON_Parse(response.c_str());
    if (!root) return false;

    cJSON* current = cJSON_GetObjectItem(root, "current_weather");
    if (!current) {
        cJSON_Delete(root);
        return false;
    }

    cJSON* temp = cJSON_GetObjectItem(current, "temperature");
    cJSON* wmo_code = cJSON_GetObjectItem(current, "weathercode");
    cJSON* wind = cJSON_GetObjectItem(current, "windspeed");

    if (cJSON_IsNumber(temp)) weather.temperature = temp->valuedouble;
    if (cJSON_IsNumber(wmo_code)) weather.code = wmo_code->valueint;
    weather.description = wmo_code_to_desc(weather.code);
    weather.icon = wmo_code_to_icon(weather.code);
    weather.valid = true;

    // 获取湿度（在另一层）
    cJSON* hourly = cJSON_GetObjectItem(root, "hourly");
    if (hourly) {
        cJSON* rh = cJSON_GetObjectItem(hourly, "relativehumidity_2m");
        if (rh && cJSON_IsArray(rh) && cJSON_GetArraySize(rh) > 0) {
            cJSON* first = cJSON_GetArrayItem(rh, 0);
            if (cJSON_IsNumber(first)) weather.humidity = first->valueint;
        }
    }

    cJSON_Delete(root);
    return true;
}

/* ========== 主题初始化 ========== */
void LcdDisplay::InitializeLcdThemes() {
    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_awesome_30_4);

    auto light_theme = new LvglTheme("light");
    light_theme->set_background_color(lv_color_hex(0xFFFFFF));
    light_theme->set_text_color(lv_color_hex(0x000000));
    // ... themes same as original

    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_background_color(lv_color_hex(0x000000));
    dark_theme->set_text_color(lv_color_hex(0xFFFFFF));
    // ... themes same as original
}

LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    InitializeLcdThemes();

    Settings settings("display", false);
    std::string theme_name = settings.GetString("theme", "light");
    current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);

    // 预览图片定时器
    esp_timer_create_args_t preview_timer_args = {
        .callback = [](void* arg) { static_cast<LcdDisplay*>(arg)->SetPreviewImage(nullptr); },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "preview_timer",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&preview_timer_args, &preview_timer_);

    // 天气刷新定时器
    esp_timer_create_args_t weather_timer_args = {
        .callback = [](void* arg) {
            auto display = static_cast<LcdDisplay*>(arg);
            display->FetchWeatherFromApi();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "weather_timer",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&weather_timer_args, &weather_timer_);
}

/* ========== SPI Display 构造函数 ========== */
SpiLcdDisplay::SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y,
                           bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

    // 白色填充
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    ESP_LOGI(TAG, "Turning display on");
    esp_lcd_panel_disp_on_off(panel_, true);

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

#if CONFIG_SPIRAM
    size_t psram_size_mb = esp_psram_get_size() / 1024 / 1024;
    if (psram_size_mb >= 8) {
        lv_image_cache_resize(2 * 1024 * 1024, true);
    } else if (psram_size_mb >= 2) {
        lv_image_cache_resize(512 * 1024, true);
    }
#endif

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation = { .swap_xy = swap_xy, .mirror_x = mirror_x, .mirror_y = mirror_y },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 1,
            .buff_spiram = 0,
            .sw_rotate = 0,
            .swap_bytes = 1,
            .full_refresh = 0,
            .direct_mode = 0,
        },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

RgbLcdDisplay::RgbLcdDisplay(...) : LcdDisplay(...) { /* same as original */ }
MipiLcdDisplay::MipiLcdDisplay(...) : LcdDisplay(...) { /* same as original */ }

LcdDisplay::~LcdDisplay() {
    SetPreviewImage(nullptr);
    if (weather_timer_ != nullptr) {
        esp_timer_stop(weather_timer_);
        esp_timer_delete(weather_timer_);
    }
    if (preview_timer_ != nullptr) {
        esp_timer_stop(preview_timer_);
        esp_timer_delete(preview_timer_);
    }
    // ... cleanup objects same as original
}

bool LcdDisplay::Lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }
void LcdDisplay::Unlock() { lvgl_port_unlock(); }

/* =====================================================================
 * 创建空闲主页 (大时钟 + 日期 + 天气)
 * 240x320 竖屏布局
 * ===================================================================== */
void LcdDisplay::CreateIdleScreen() {
    auto screen = lv_screen_active();
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto icon_font = lvgl_theme->icon_font()->font();

    /* 空闲主页容器 — 初始隐藏 */
    idle_container_ = lv_obj_create(screen);
    lv_obj_set_size(idle_container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(idle_container_, 0, 0);
    lv_obj_set_style_border_width(idle_container_, 0, 0);
    lv_obj_set_style_pad_all(idle_container_, 0, 0);
    lv_obj_set_style_bg_color(idle_container_, lv_color_hex(0x0F0C29));  // 深色背景
    lv_obj_set_style_bg_opa(idle_container_, LV_OPA_COVER, 0);
    lv_obj_add_flag(idle_container_, LV_OBJ_FLAG_HIDDEN);  // 初始隐藏

    /* ---- 大时钟 72px ---- */
    clock_time_label_ = lv_label_create(idle_container_);
    lv_label_set_text(clock_time_label_, "12:00");
    lv_obj_set_style_text_font(clock_time_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(clock_time_label_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(clock_time_label_, lvgl_theme->text_font()->font(), 0);
    lv_obj_align(clock_time_label_, LV_ALIGN_CENTER, 0, -30);
    
    // 加大字号 — LVGL9 用 lv_style_set_text_font/lv_font_t
    // 这里使用更大号的系统字体或默认字体
    lv_obj_set_style_text_letter_spacing(clock_time_label_, 2, 0);

    /* ---- 日期 ---- */
    clock_date_label_ = lv_label_create(idle_container_);
    lv_label_set_text(clock_date_label_, "2025年7月4日 星期五");
    lv_obj_set_style_text_color(clock_date_label_, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(clock_date_label_, LV_ALIGN_CENTER, 0, 20);

    /* ---- 天气区域 ---- */
    // 天气图标
    weather_icon_label_ = lv_label_create(idle_container_);
    lv_label_set_text(weather_icon_label_, "☀️");
    lv_obj_set_style_text_color(weather_icon_label_, lv_color_hex(0xFFDD55), 0);
    lv_obj_align(weather_icon_label_, LV_ALIGN_CENTER, -30, 70);

    // 温度
    weather_temp_label_ = lv_label_create(idle_container_);
    lv_label_set_text(weather_temp_label_, "28°C");
    lv_obj_set_style_text_color(weather_temp_label_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(weather_temp_label_, LV_ALIGN_CENTER, 20, 70);

    // 天气描述
    weather_desc_label_ = lv_label_create(idle_container_);
    lv_label_set_text(weather_desc_label_, "晴朗 · 深圳");
    lv_obj_set_style_text_color(weather_desc_label_, lv_color_hex(0x888888), 0);
    lv_obj_align(weather_desc_label_, LV_ALIGN_CENTER, 0, 100);

    ESP_LOGI(TAG, "Idle screen created");
}

/* =====================================================================
 * 设置空闲/对话模式
 * ===================================================================== */
void LcdDisplay::SetIdleMode(bool idle) {
    DisplayLockGuard lock(this);
    if (idle == show_idle_screen_) return;

    show_idle_screen_ = idle;

    if (idle) {
        // 显示空闲主页，隐藏对话内容
        if (idle_container_) lv_obj_remove_flag(idle_container_, LV_OBJ_FLAG_HIDDEN);
        if (content_) lv_obj_add_flag(content_, LV_OBJ_FLAG_HIDDEN);
        if (emoji_label_) lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        if (emoji_image_) lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        if (status_bar_) lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
        
        // 更新显示
        UpdateClockDisplay();
        UpdateWeatherDisplay();

        // 启动天气定时器
        if (weather_timer_ && !weather_.valid) {
            FetchWeatherFromApi();
        }
        esp_timer_start_periodic(weather_timer_, WEATHER_REFRESH_INTERVAL_MS);

        ESP_LOGI(TAG, "Switched to IDLE screen");
    } else {
        // 显示对话内容，隐藏空闲主页
        if (idle_container_) lv_obj_add_flag(idle_container_, LV_OBJ_FLAG_HIDDEN);
        if (content_) lv_obj_remove_flag(content_, LV_OBJ_FLAG_HIDDEN);
        if (emoji_label_) lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        if (emoji_image_) lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        if (status_bar_) lv_obj_remove_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);

        // 停止天气定时器
        esp_timer_stop(weather_timer_);

        ESP_LOGI(TAG, "Switched to CHAT screen");
    }
}

/* =====================================================================
 * 更新时钟显示 (每秒调用)
 * ===================================================================== */
void LcdDisplay::UpdateClockDisplay() {
    if (!clock_time_label_ || !clock_date_label_) return;

    time_t now = time(nullptr);
    struct tm* tm = localtime(&now);
    if (tm->tm_year < 2025 - 1900) return;  // 时间未同步

    char time_str[16];
    strftime(time_str, sizeof(time_str), "%H:%M", tm);
    lv_label_set_text(clock_time_label_, time_str);

    char date_str[32];
    strftime(date_str, sizeof(date_str), "%Y年%m月%d日 %A", tm);
    // 替换英文星期为中文
    const char* week_cn[] = {"星期日","星期一","星期二","星期三","星期四","星期五","星期六"};
    char date_cn[32];
    snprintf(date_cn, sizeof(date_cn), "%04d年%02d月%02d日 %s",
        tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, week_cn[tm->tm_wday]);
    lv_label_set_text(clock_date_label_, date_cn);
}

/* =====================================================================
 * 更新天气显示
 * ===================================================================== */
void LcdDisplay::UpdateWeatherDisplay() {
    if (!weather_icon_label_ || !weather_temp_label_ || !weather_desc_label_) return;

    if (weather_.valid) {
        char temp_str[16];
        snprintf(temp_str, sizeof(temp_str), "%.0f°C", weather_.temperature);
        lv_label_set_text(weather_icon_label_, weather_.icon.c_str());
        lv_label_set_text(weather_temp_label_, temp_str);

        char desc_str[64];
        snprintf(desc_str, sizeof(desc_str), "%s · %s", 
            weather_.description.c_str(), weather_.city.c_str());
        lv_label_set_text(weather_desc_label_, desc_str);
    } else {
        lv_label_set_text(weather_icon_label_, "🌤️");
        lv_label_set_text(weather_temp_label_, "--°C");
        lv_label_set_text(weather_desc_label_, "获取天气中...");
    }
}

/* =====================================================================
 * 从 API 获取天气 (沈阳于洪区 41.77, 123.32)
 * ===================================================================== */
void LcdDisplay::FetchWeatherFromApi() {
    // 默认深圳坐标，可在 settings 中配置
    Settings settings("weather", true);
    float lat = settings.GetFloat("latitude", 41.77f);
    float lon = settings.GetFloat("longitude", 123.32f);
    std::string city = settings.GetString("city", "沈阳于洪");

    WeatherData new_weather;
    new_weather.city = city;

    bool ok = fetch_weather_from_openmeteo(new_weather, lat, lon);
    if (ok) {
        weather_ = new_weather;
        ESP_LOGI(TAG, "Weather updated: %.1f°C, %s", weather_.temperature, weather_.description.c_str());

        DisplayLockGuard lock(this);
        UpdateWeatherDisplay();
    } else {
        ESP_LOGW(TAG, "Weather fetch failed, will retry later");
    }
}

/* =====================================================================
 * 外部更新天气 (例如从 MCP 服务器推送)
 * ===================================================================== */
void LcdDisplay::UpdateWeather(const WeatherData& weather) {
    weather_ = weather;
    weather_.valid = true;
    DisplayLockGuard lock(this);
    UpdateWeatherDisplay();
}

/* =====================================================================
 * SetupUI — 创建用户界面
 * ===================================================================== */
void LcdDisplay::SetupUI() {
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times");
        return;
    }
    Display::SetupUI();
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0F0C29), 0);

    /* ===== 对话界面 (与原始一致) ===== */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lv_color_hex(0x0D0D1A), 0);

    /* Top bar */
    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(top_bar_, lv_color_hex(0x0D0D1A), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 6, 0);
    lv_obj_set_style_pad_left(top_bar_, 12, 0);
    lv_obj_set_style_pad_right(top_bar_, 12, 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lv_color_hex(0xFFFFFF), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lv_color_hex(0x81C784), 0);

    /* Status bar */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.8);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(status_label_, "初始化中");
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.8);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    /* Content - chat area */
    content_ = lv_obj_create(container_);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_style_pad_all(content_, 8, 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_bg_color(content_, lv_color_hex(0x0D0D1A), 0);
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* Emoji */
    emoji_label_ = lv_label_create(screen);
    lv_obj_center(emoji_label_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lv_color_hex(0x6666EE), 0);
    lv_label_set_text(emoji_label_, "◆");  // AI icon placeholder

    /* ===== 创建空闲主页 ===== */
    CreateIdleScreen();

    /* 初始状态：显示对话界面 */
    SetIdleMode(false);

    ESP_LOGI(TAG, "SetupUI complete");
}

/* =====================================================================
 * 状态栏更新 (每秒调用)
 * ===================================================================== */
void LcdDisplay::UpdateStatusBar(bool update_all) {
    // 如果显示空闲主页，每秒更新时钟
    if (show_idle_screen_) {
        UpdateClockDisplay();
    }

    // 调用父类更新网络、电池等
    LvglDisplay::UpdateStatusBar(update_all);
}

/* =====================================================================
 * 以下方法与原始一致
 * ===================================================================== */
void LcdDisplay::SetEmotion(const char* emotion) {
    // ... same as original
}

void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    // ... same as original
}

void LcdDisplay::ClearChatMessages() {
    // ... same as original
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    // ... same as original
}

void LcdDisplay::SetTheme(Theme* theme) {
    // ... same as original
}

void LcdDisplay::SetHideSubtitle(bool hide) {
    hide_subtitle_ = hide;
}
