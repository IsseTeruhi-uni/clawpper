#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <notification/notification_messages.h>
#include <applications/services/cli/cli.h>

#define VIEW_IDLE    0
#define VIEW_CONFIRM 1
#define MAX_OPTIONS  6
#define ANIM_FRAMES  4
#define ANIM_MS      350
#define DONE_FRAMES  10  // auto-reset to Waiting after ~3.5s

// Character and monitor anchor coordinates
#define CX 82  // Clawd X
#define CY 20  // Clawd Y (legs near desk level)
#define MX 12  // Monitor X
#define MY  9  // Monitor Y

// ====== State ======

typedef enum {
    StateWaiting   = 0,
    StateComplete  = 1,
    StateConfirm   = 2,
    StateRunning   = 3,
    StateEditing   = 4,
    StateSearching = 5,
} IdleState;

typedef struct {
    uint8_t   frame;
    uint8_t   state_frames;
    IdleState state;
    char      msg[48];
} IdleModel;

typedef struct {
    Gui*             gui;
    ViewDispatcher*  view_dispatcher;
    View*            idle_view;
    Submenu*         confirm_menu;
    NotificationApp* notifications;
    CliRegistry*     cli;
    FuriTimer*       anim_timer;
    FuriSemaphore*   confirm_sem;
    FuriMutex*       confirm_mutex;
    char             confirm_title[64];
    char             confirm_opts[MAX_OPTIONS][48];
    uint8_t          confirm_opt_count;
    uint8_t          confirm_choice;
    bool             confirm_active;
} ClaudeNotifyApp;

// ====== Clawd character ======

// Faithful SVG reproduction: rectangular body + 4 legs + side arms + white eye holes
// Scale ~1.5x from the 24-unit SVG: body 27x18px, arms 5x4px (sides), legs 2x4px (x4)
static void draw_clawd(Canvas* canvas, uint8_t f, IdleState state) {
    // Main body (filled rectangle)
    canvas_draw_box(canvas, CX, CY, 27, 18);

    // Eyes (white cutout holes) — SVG: left x=6-7.5,y=8.1-10.9 / right x=16.5-18,y=8.1-10.9
    canvas_set_color(canvas, ColorWhite);
    if(state == StateComplete && f % 2 == 0) {
        // Happy eyes (^^)
        canvas_draw_line(canvas, CX + 4, CY + 7, CX + 5, CY + 5);
        canvas_draw_line(canvas, CX + 5, CY + 5, CX + 6, CY + 7);
        canvas_draw_line(canvas, CX+21, CY + 7, CX+22, CY + 5);
        canvas_draw_line(canvas, CX+22, CY + 5, CX+23, CY + 7);
    } else {
        canvas_draw_box(canvas, CX + 4, CY + 5, 3, 4);   // left eye (3x4px)
        canvas_draw_box(canvas, CX + 20, CY + 5, 3, 4);  // right eye (3x4px)
    }
    canvas_set_color(canvas, ColorBlack);

    // 4 legs at bottom of body (2x4px each) — symmetric SVG placement
    canvas_draw_box(canvas, CX + 2,  CY + 18, 2, 4);  // leg 1 (outer left)
    canvas_draw_box(canvas, CX + 7,  CY + 18, 2, 4);  // leg 2 (inner left)
    canvas_draw_box(canvas, CX + 18, CY + 18, 2, 4);  // leg 3 (inner right)
    canvas_draw_box(canvas, CX + 23, CY + 18, 2, 4);  // leg 4 (outer right)

    // Arms (state + frame dependent, rectangular protrusions on sides)
    switch(state) {
    case StateComplete:
        // Arms raised high
        canvas_draw_line(canvas, CX - 1,  CY + 9, CX - 6,  CY + 3);
        canvas_draw_line(canvas, CX + 28, CY + 9, CX + 33, CY + 3);
        if(f % 2 == 0) {
            canvas_draw_dot(canvas, CX - 8, CY + 1);
            canvas_draw_dot(canvas, CX + 35, CY + 1);
        } else {
            canvas_draw_dot(canvas, CX - 7, CY);
            canvas_draw_dot(canvas, CX + 34, CY);
        }
        break;
    case StateConfirm:
        // Thinking pose: left arm sideways, right arm angled toward head
        canvas_draw_box(canvas, CX - 5, CY + 10, 5, 4);              // left arm (horizontal)
        canvas_draw_line(canvas, CX + 27, CY + 11, CX + 33, CY + 6); // right arm (angled up)
        canvas_draw_dot(canvas, CX + 34, CY + 5);
        break;
    case StateRunning:
        // Alternating arms (running motion)
        if(f % 2 == 0) {
            canvas_draw_box(canvas, CX - 5, CY + 5,  5, 4);  // left arm up
            canvas_draw_box(canvas, CX + 27, CY + 11, 5, 4); // right arm down
        } else {
            canvas_draw_box(canvas, CX - 5,  CY + 11, 5, 4); // left arm down
            canvas_draw_box(canvas, CX + 27, CY + 5,  5, 4); // right arm up
        }
        break;
    case StateEditing:
        // Left arm at side, right arm raised (holding pen)
        canvas_draw_box(canvas, CX - 5, CY + 11, 5, 4);              // left arm
        canvas_draw_line(canvas, CX + 27, CY + 9, CX + 33, CY + 3); // right arm raised
        canvas_draw_dot(canvas, CX + 34, CY + 2);                    // pen tip
        break;
    case StateSearching:
        // Both arms raised (binoculars pose)
        canvas_draw_line(canvas, CX - 1,  CY + 10, CX - 5, CY + 5);
        canvas_draw_line(canvas, CX + 28, CY + 10, CX + 32, CY + 5);
        // Binoculars lenses (frames over eyes)
        canvas_draw_frame(canvas, CX + 2,  CY + 3, 5, 5);
        canvas_draw_frame(canvas, CX + 20, CY + 3, 5, 5);
        break;
    default: // StateWaiting: typing animation (arms alternate up/down)
        if(f == 0 || f == 2) {
            // Arms down (pressing keys)
            canvas_draw_box(canvas, CX - 5, CY + 11, 5, 4);
            canvas_draw_box(canvas, CX + 27, CY + 11, 5, 4);
        } else {
            // Arms up (lifting off keys)
            canvas_draw_box(canvas, CX - 5, CY + 8, 5, 4);
            canvas_draw_box(canvas, CX + 27, CY + 8, 5, 4);
        }
        break;
    }
}

// ====== Monitor ======

static void draw_monitor(Canvas* canvas, uint8_t f, IdleState state) {
    // Outer frame (52px wide)
    canvas_draw_frame(canvas, MX, MY, 52, 26);
    // Screen area (white background, 48px inner width)
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_box(canvas, MX + 2, MY + 2, 48, 22);
    canvas_set_color(canvas, ColorBlack);

    switch(state) {
    case StateComplete:
        // Blank — state is indicated by "Complete!" text at the bottom of screen
        break;
    case StateConfirm:
        // Question mark (centered)
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, MX + 22, MY + 18, "?");
        break;
    case StateRunning: {
        // Spinner animation
        static const char* const spin[] = {"-", "/", "|", "\\"};
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, MX + 24, MY + 17, spin[f]);
        break;
    }
    case StateEditing: {
        // Static top line + growing second line with blinking cursor
        uint8_t len = (uint8_t)(10 + f * 10);
        if(len > 42) len = 42;
        canvas_draw_line(canvas, MX + 4, MY + 8,  MX + 44, MY + 8);
        canvas_draw_line(canvas, MX + 4, MY + 14, MX + 4 + len, MY + 14);
        if(f % 2 == 0) canvas_draw_box(canvas, MX + 5 + len, MY + 12, 2, 4);
        break;
    }
    case StateSearching:
        // Magnifying glass
        canvas_draw_circle(canvas, MX + 22, MY + 12, 7);
        canvas_draw_line(canvas, MX + 27, MY + 17, MX + 33, MY + 22);
        canvas_draw_line(canvas, MX + 28, MY + 17, MX + 34, MY + 22);
        break;
    default: { // StateWaiting
        // Animated code lines (max 44px to fill wider screen)
        uint8_t l1 = 16 + (f >= 1 ? 10 : 0) + (f >= 3 ? 6 : 0);
        uint8_t l2 = 40;
        uint8_t l3 = 10 + (f >= 2 ? 12 : 0);
        if(l1 > 44) l1 = 44;
        if(l3 > 44) l3 = 44;
        canvas_draw_line(canvas, MX + 4, MY + 6,  MX + 4 + l1, MY + 6);
        canvas_draw_line(canvas, MX + 4, MY + 12, MX + 4 + l2, MY + 12);
        canvas_draw_line(canvas, MX + 4, MY + 18, MX + 4 + l3, MY + 18);
        // Blinking cursor
        if(f % 2 == 0) {
            canvas_draw_box(canvas, MX + 4 + l3 + 1, MY + 16, 2, 3);
        }
        break;
    }
    }

    // Stand (centered in 52px frame: center=26, stand width=6 → MX+23)
    canvas_draw_box(canvas, MX + 23, MY + 26, 6, 5);
    canvas_draw_line(canvas, MX + 13, MY + 31, MX + 39, MY + 31);
}

// ====== Main scene ======

static void idle_draw_callback(Canvas* canvas, void* _model) {
    IdleModel* m = (IdleModel*)_model;
    canvas_clear(canvas);

    // Desk surface
    canvas_draw_line(canvas, 6, 44, 122, 44);

    // Keyboard (expanded to match 52px monitor width)
    for(int kx = MX + 2; kx < MX + 50; kx += 6) {
        canvas_draw_line(canvas, kx, 46, kx + 4, 46);
    }
    canvas_draw_line(canvas, MX + 4, 48, MX + 46, 48);

    // Monitor
    draw_monitor(canvas, m->frame, m->state);

    // Clawd
    draw_clawd(canvas, m->frame, m->state);

    // Status text (bottom of screen)
    canvas_set_font(canvas, FontSecondary);
    switch(m->state) {
    case StateComplete:
        canvas_draw_str(canvas, 2, 62, "Complete!");
        break;
    case StateConfirm:
        canvas_draw_str(canvas, 2, 62, "Confirm?");
        break;
    case StateRunning: {
        static const char* const dots[] = {"Running.", "Running..", "Running...", "Running.."};
        canvas_draw_str(canvas, 2, 62, dots[m->frame]);
        break;
    }
    case StateEditing: {
        static const char* const dots[] = {"Editing.", "Editing..", "Editing...", "Editing.."};
        canvas_draw_str(canvas, 2, 62, dots[m->frame]);
        break;
    }
    case StateSearching: {
        static const char* const dots[] = {"Searching.", "Searching..", "Searching...", "Searching.."};
        canvas_draw_str(canvas, 2, 62, dots[m->frame]);
        break;
    }
    default: {
        static const char* const dots[] = {"Waiting.", "Waiting..", "Waiting...", "Waiting.."};
        canvas_draw_str(canvas, 2, 62, dots[m->frame]);
        break;
    }
    }
}

// ====== Animation timer (also handles auto-reset from Complete to Waiting) ======

static void anim_timer_cb(void* context) {
    ClaudeNotifyApp* app = context;
    with_view_model(
        app->idle_view,
        IdleModel* model,
        {
            model->frame = (model->frame + 1) % ANIM_FRAMES;
            if(model->state == StateComplete) {
                model->state_frames++;
                if(model->state_frames >= DONE_FRAMES) {
                    model->state        = StateWaiting;
                    model->state_frames = 0;
                    memset(model->msg, 0, sizeof(model->msg));
                }
            }
        },
        true);
}

// ====== Confirm submenu ======

static void confirm_submenu_callback(void* context, uint32_t index) {
    ClaudeNotifyApp* app = context;
    app->confirm_choice = (uint8_t)index;
    app->confirm_active = false;
    furi_semaphore_release(app->confirm_sem);
    // Return to idle (waiting) after selection
    with_view_model(
        app->idle_view,
        IdleModel* model,
        { model->state = StateWaiting; },
        false);
    view_dispatcher_switch_to_view(app->view_dispatcher, VIEW_IDLE);
}

static bool navigation_event_callback(void* context) {
    ClaudeNotifyApp* app = context;
    if(app->confirm_active) {
        // Back button = select last option (Deny)
        app->confirm_choice = app->confirm_opt_count > 0 ? app->confirm_opt_count - 1 : 0;
        app->confirm_active = false;
        furi_semaphore_release(app->confirm_sem);
        with_view_model(
            app->idle_view,
            IdleModel* model,
            { model->state = StateWaiting; },
            false);
        view_dispatcher_switch_to_view(app->view_dispatcher, VIEW_IDLE);
        return true;
    }
    return false;
}

static bool custom_event_callback(void* context, uint32_t event) {
    UNUSED(context);
    UNUSED(event);
    return true;
}

// ====== CLI handlers ======

static void cli_notify_handler(PipeSide* pipe, FuriString* args, void* context) {
    UNUSED(pipe);
    ClaudeNotifyApp* app = context;
    with_view_model(
        app->idle_view,
        IdleModel* model,
        {
            model->state       = StateComplete;
            model->state_frames = 0;
            strncpy(model->msg, furi_string_get_cstr(args), sizeof(model->msg) - 1);
            model->msg[sizeof(model->msg) - 1] = '\0';
        },
        true);
    notification_message(app->notifications, &sequence_single_vibro);
}

static void cli_state_handler(PipeSide* pipe, FuriString* args, void* context) {
    UNUSED(pipe);
    ClaudeNotifyApp* app = context;
    const char* s = furi_string_get_cstr(args);
    IdleState new_state = StateWaiting;
    if(strcmp(s, "running") == 0)        new_state = StateRunning;
    else if(strcmp(s, "editing") == 0)   new_state = StateEditing;
    else if(strcmp(s, "searching") == 0) new_state = StateSearching;
    else if(strcmp(s, "complete") == 0)  new_state = StateComplete;
    with_view_model(
        app->idle_view,
        IdleModel* model,
        {
            model->state        = new_state;
            model->state_frames = 0;
        },
        true);
}

static void cli_confirm_handler(PipeSide* pipe, FuriString* args, void* context) {
    ClaudeNotifyApp* app = context;

    furi_mutex_acquire(app->confirm_mutex, FuriWaitForever);

    app->confirm_choice    = 0;
    app->confirm_opt_count = 0;
    memset(app->confirm_title, 0, sizeof(app->confirm_title));

    // Reflect StateConfirm in the model (triggers animation change)
    with_view_model(
        app->idle_view,
        IdleModel* model,
        { model->state = StateConfirm; },
        false);

    // Parse "title%1Foption1%1Foption2..." — %1F is URL-encoded unit separator
    static const char SEP[] = "%1F";
    const char*       raw   = furi_string_get_cstr(args);
    const char*       p     = raw;
    const char*       sep   = strstr(p, SEP);

    if(sep) {
        size_t len = (size_t)(sep - p);
        if(len >= sizeof(app->confirm_title)) len = sizeof(app->confirm_title) - 1;
        strncpy(app->confirm_title, p, len);
        app->confirm_title[len] = '\0';
        p = sep + sizeof(SEP) - 1;
        while(*p && app->confirm_opt_count < MAX_OPTIONS) {
            sep        = strstr(p, SEP);
            char*  opt = app->confirm_opts[app->confirm_opt_count];
            size_t ol  = sep ? (size_t)(sep - p) : strlen(p);
            if(ol >= sizeof(app->confirm_opts[0])) ol = sizeof(app->confirm_opts[0]) - 1;
            strncpy(opt, p, ol);
            opt[ol] = '\0';
            app->confirm_opt_count++;
            if(sep) p = sep + sizeof(SEP) - 1;
            else break;
        }
    } else {
        // Fallback: no separator found, use raw string as title with default options
        strncpy(app->confirm_title, raw, sizeof(app->confirm_title) - 1);
        strncpy(app->confirm_opts[0], "1. Allow", sizeof(app->confirm_opts[0]) - 1);
        strncpy(app->confirm_opts[1], "2. Deny",  sizeof(app->confirm_opts[0]) - 1);
        app->confirm_opt_count = 2;
    }

    // Build submenu and switch to confirm view
    submenu_reset(app->confirm_menu);
    submenu_set_header(app->confirm_menu, app->confirm_title);
    for(uint8_t i = 0; i < app->confirm_opt_count; i++) {
        submenu_add_item(
            app->confirm_menu,
            app->confirm_opts[i],
            i,
            confirm_submenu_callback,
            app);
    }
    app->confirm_active = true;
    notification_message(app->notifications, &sequence_double_vibro);
    view_dispatcher_switch_to_view(app->view_dispatcher, VIEW_CONFIRM);

    // Block until user makes a selection
    furi_semaphore_acquire(app->confirm_sem, FuriWaitForever);
    char response[32];
    int rlen = snprintf(response, sizeof(response), "CHOICE:%u\r\n", (unsigned)app->confirm_choice);
    pipe_send(pipe, response, (size_t)rlen);
    furi_mutex_release(app->confirm_mutex);
}

// ====== Entry point ======

int32_t clawpper_app(void* p) {
    UNUSED(p);
    ClaudeNotifyApp* app = malloc(sizeof(ClaudeNotifyApp));
    memset(app, 0, sizeof(ClaudeNotifyApp));

    app->gui           = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->cli           = furi_record_open(RECORD_CLI);
    app->confirm_sem   = furi_semaphore_alloc(1, 0);
    app->confirm_mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, navigation_event_callback);
    view_dispatcher_attach_to_gui(
        app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    // Idle animation view
    app->idle_view = view_alloc();
    view_set_draw_callback(app->idle_view, idle_draw_callback);
    view_allocate_model(app->idle_view, ViewModelTypeLocking, sizeof(IdleModel));

    // Confirm submenu
    app->confirm_menu = submenu_alloc();

    view_dispatcher_add_view(app->view_dispatcher, VIEW_IDLE,    app->idle_view);
    view_dispatcher_add_view(app->view_dispatcher, VIEW_CONFIRM, submenu_get_view(app->confirm_menu));
    view_dispatcher_switch_to_view(app->view_dispatcher, VIEW_IDLE);

    // Animation timer
    app->anim_timer = furi_timer_alloc(anim_timer_cb, FuriTimerTypePeriodic, app);
    furi_timer_start(app->anim_timer, ANIM_MS);

    cli_registry_add_command(
        app->cli, "claude_notify",  CliCommandFlagParallelSafe, cli_notify_handler,  app);
    cli_registry_add_command(
        app->cli, "claude_confirm", CliCommandFlagParallelSafe, cli_confirm_handler, app);
    cli_registry_add_command(
        app->cli, "claude_state",   CliCommandFlagParallelSafe, cli_state_handler,   app);

    view_dispatcher_run(app->view_dispatcher);

    cli_registry_delete_command(app->cli, "claude_notify");
    cli_registry_delete_command(app->cli, "claude_confirm");
    cli_registry_delete_command(app->cli, "claude_state");

    furi_timer_stop(app->anim_timer);
    furi_timer_free(app->anim_timer);

    view_dispatcher_remove_view(app->view_dispatcher, VIEW_CONFIRM);
    view_dispatcher_remove_view(app->view_dispatcher, VIEW_IDLE);
    submenu_free(app->confirm_menu);
    view_free(app->idle_view);
    view_dispatcher_free(app->view_dispatcher);

    furi_mutex_free(app->confirm_mutex);
    furi_semaphore_free(app->confirm_sem);
    furi_record_close(RECORD_CLI);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
