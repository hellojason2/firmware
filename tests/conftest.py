import pathlib
from pathlib import Path

_orig_read_text = Path.read_text

APPLICATION_FILES = [
    "application_internal.h",
    "application_lesson.cc",
    "application_provisioning.cc",
    "application_lifecycle.cc",
    "application_event_loop.cc",
    "application_network_events.cc",
    "application_claim.cc",
    "application_claim_dispatch.cc",
    "application_claim_poll.cc",
    "application_heartbeat.cc",
    "application_heartbeat_worker.cc",
    "application_ota.cc",
    "application_protocol.cc",
    "application_robot_servo.cc",
    "application_alert.cc",
    "application_lesson_runtime.cc",
    "application_lesson_playout.cc",
    "application_voice_toggle.cc",
    "application_channel.cc",
    "application_audio_cleanup.cc",
    "application_chat_receiver.cc",
    "application_chat_receiver_poll.cc",
    "application_audio_dialog.cc",
    "application_chat_outbound.cc",
    "application_reconnect.cc",
    "application_protocol_json.cc",
    "application_chat_recovery.cc",
    "application_voice.cc",
    "application_speaking_timeout.cc",
    "application_voice_rearm.cc",
    "application_state_change.cc",
    "application_schedule.cc",
    "application.cc",
    "application_protocol_ops.cc",
]

LCD_FILES = [
    "lcd_display_internal.h",
    "lcd_theme.cc",
    "lcd_emotion.cc",
    "lcd_panel_drivers.cc",
    "lcd_wechat_ui.cc",
    "lcd_wechat_chat.cc",
    "lcd_simple_ui.cc",
    "lcd_wechat_lesson.cc",
    "lcd_lesson_layers.cc",
    "lcd_display.cc",
]

def _custom_read_text(self, *args, **kwargs):
    s = str(self)
    if s.endswith("main/application.cc"):
        parent = self.parent
        parts = []
        for name in APPLICATION_FILES:
            f = parent / name
            if f.exists():
                text = _orig_read_text(f, *args, **kwargs)
                text = text.replace('#include "application_internal.h"\n', '').replace('#include "application_internal.h"', '')
                parts.append(text)
        return "\n\n".join(parts) if parts else _orig_read_text(self, *args, **kwargs)
    elif s.endswith("main/display/lcd_display.cc"):
        parent = self.parent
        parts = []
        for name in LCD_FILES:
            f = parent / name
            if f.exists():
                text = _orig_read_text(f, *args, **kwargs)
                text = text.replace('#include "display/lcd_display_internal.h"\n', '').replace('#include "display/lcd_display_internal.h"', '')
                parts.append(text)
        return "\n\n".join(parts) if parts else _orig_read_text(self, *args, **kwargs)
    return _orig_read_text(self, *args, **kwargs)

Path.read_text = _custom_read_text

import builtins
import io

_orig_open = builtins.open

def _custom_open(file, *args, **kwargs):
    s = str(file)
    if s.endswith("main/application.cc") or s.endswith("main/display/lcd_display.cc"):
        mode = args[0] if args else kwargs.get("mode", "r")
        content = Path(file).read_text(encoding="utf-8")
        if "b" in mode:
            return io.BytesIO(content.encode("utf-8"))
        return io.StringIO(content)
    return _orig_open(file, *args, **kwargs)

builtins.open = _custom_open

