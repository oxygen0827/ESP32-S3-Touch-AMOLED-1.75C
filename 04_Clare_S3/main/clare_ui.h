#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLARE_UI_HOME = 0,
    CLARE_UI_CLARE,
    CLARE_UI_DEMO,
} clare_ui_page_t;

typedef struct {
    void (*open_clare)(void *ctx);
    void (*close_clare)(void *ctx);
    void (*start_meeting)(void *ctx);
    void (*stop_meeting)(void *ctx);
    void (*toggle_host)(void *ctx);
    void (*refresh_summary)(void *ctx);
    void *ctx;
} clare_ui_callbacks_t;

void clare_ui_init(const clare_ui_callbacks_t *callbacks);
void clare_ui_set_page(clare_ui_page_t page);
void clare_ui_set_status(const char *text);
void clare_ui_set_transcript(const char *text);
void clare_ui_reset_transcript(void);
void clare_ui_append_transcript(const char *text, bool is_final);
void clare_ui_set_answer(const char *text);
void clare_ui_reset_answer(void);
void clare_ui_append_answer(const char *text, bool is_final);
void clare_ui_append_answer_delta(const char *text, bool is_final);
void clare_ui_set_wifi(const char *text);
void clare_ui_set_meeting_active(bool active);
void clare_ui_set_host_active(bool active);

#ifdef __cplusplus
}
#endif
