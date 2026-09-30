#ifndef APP_MANAGER_H_
#define APP_MANAGER_H_

#include <cstdint>
#include <functional>
#include <vector>

// Ba che do UI tren main. Chatbox la mac dinh (giu nguyen hanh vi voice/cloud cu).
// Menu va Game ve tren lop overlay LVGL (lv_layer_top) nen KHONG dung UI chatbox.
enum class AppMode {
    Chatbox = 0,
    Menu,
    Game,
    Speed,   // chinh toc do tay+dau cua slave (dance nhac + gesture cham nut)
};

// Goi 1 lan sau khi display SetupUI() xong (tren main task).
void AppManagerInit();

// Dang ky ham gui dong dieu khien toi slave (vd "MODE:GAME"). Best-effort, co the null.
void AppManagerSetSlaveSender(std::function<void(const char*)> sender);

// Dang ky ham phat SFX game (PCM 16-bit mono 24kHz). Goi tren main task. Co the null.
void AppManagerSetSoundPlayer(std::function<void(const std::vector<int16_t>&)> player);

AppMode AppGetMode();

// System flows such as Wi-Fi setup must reveal the chatbox/status layer.
void AppExitToChatboxForSystemFlow();

// Cac ham xu ly nay PHAI chay tren main task (UART listener marshal qua Application::Schedule).
// Chung tu lock display (DisplayLockGuard) khi dung LVGL.
void AppHandleInputLeft();
void AppHandleInputRight();
void AppHandleInputBothClick();
void AppHandleMenuHold();   // giữ hai nút 3 giây: về menu
void AppHandleRightHold();  // giữ nút phải 3 giây: đổi Wi-Fi (giữ claim)
void AppOnSlaveReady();

#endif  // APP_MANAGER_H_
