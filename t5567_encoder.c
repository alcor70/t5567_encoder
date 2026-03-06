#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define TAG "T5567_Encoder"
#define CSV_FILE_PATH "/ext/apps_data/t5567_encoder/anagrafica.csv"
#define RFID_SAVE_PATH "/ext/lfrfid/badge_%s.rfid"

uint8_t reverse_nibble(uint8_t n) {
    uint8_t reversed = 0;
    if (n & 0x01) reversed |= 0x08;
    if (n & 0x02) reversed |= 0x04;
    if (n & 0x04) reversed |= 0x02;
    if (n & 0x08) reversed |= 0x01;
    return reversed;
}

uint16_t calculate_t5567_code(const char* matricola_str) {
    uint16_t matricola_val = atoi(matricola_str);
    uint8_t d3 = (matricola_val / 1000) % 10;
    uint8_t d2 = (matricola_val / 100) % 10;
    uint8_t d1 = (matricola_val / 10) % 10;
    uint8_t d0 = matricola_val % 10;

    uint8_t r3 = reverse_nibble(d3);
    uint8_t r2 = reverse_nibble(d2);
    uint8_t r1 = reverse_nibble(d1);
    uint8_t r0 = reverse_nibble(d0);

    uint8_t byte4 = (r3 << 4) | r2;
    uint8_t byte5 = (r1 << 4) | r0;

    return (byte4 << 8) | byte5;
}

bool check_if_matricola_exists(const char* matricola) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool found = false;

    if(storage_file_open(file, CSV_FILE_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char c;
        char line_start[16] = {0};
        uint8_t ls_idx = 0;
        bool at_line_start = true;
        
        while(storage_file_read(file, &c, 1) == 1) {
            if (c == '\n' || c == '\r') {
                at_line_start = true;
                ls_idx = 0;
                memset(line_start, 0, 16);
            } else if (at_line_start) {
                if (c == ',') {
                    at_line_start = false;
                    if (strcmp(line_start, matricola) == 0) {
                        found = true;
                        break;
                    }
                } else if (ls_idx < 15) {
                    line_start[ls_idx++] = c;
                }
            }
        }
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return found;
}

bool generate_rfid_file(const char* matricola) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool success = false;

    char file_path[128];
    snprintf(file_path, sizeof(file_path), RFID_SAVE_PATH, matricola);

    if(storage_file_open(file, file_path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        uint16_t code = calculate_t5567_code(matricola);
        uint8_t byte4 = (code >> 8) & 0xFF;
        uint8_t byte5 = code & 0xFF;
        
        char file_content[256];
        snprintf(file_content, sizeof(file_content),
            "Filetype: Flipper RFID key\n"
            "Version: 1\n"
            "Key type: EM4100\n"
            "Data: 00 00 00 %02X %02X\n", byte4, byte5);
            
        success = storage_file_write(file, file_content, strlen(file_content)) == strlen(file_content);
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    
    return success;
}

typedef enum { AppStateInput, AppStateWarning, AppStateDone, AppStateError } AppState;

typedef struct {
    uint8_t digits[4];
    uint8_t cur_idx;
    char buffer_matricola[5];
    AppState state;
    char status_msg[64];
    FuriMutex* mutex;
} T5567EncoderApp;

static void draw_callback(Canvas* canvas, void* ctx) {
    T5567EncoderApp* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 5, 12, "T5567 Encoder");
    
    if (app->state == AppStateInput) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 5, 27, "Inserisci Matricola:");
        
        canvas_set_font(canvas, FontBigNumbers);
        for(uint8_t i = 0; i < 4; i++) {
            char d[4];
            snprintf(d, sizeof(d), "%u", app->digits[i]);
            canvas_draw_str(canvas, 20 + (i * 12), 52, d);
            
            if (i == app->cur_idx) {
                // Sottolinea la cifra selezionata
                canvas_draw_line(canvas, 20 + (i * 12), 54, 20 + (i * 12) + 9, 54);
            }
        }
        
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 5, 62, "Usa frecce. Tieni OK x Creare");
        
    } else if (app->state == AppStateWarning) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 5, 25, "Attenzione: Matricola");
        canvas_draw_str(canvas, 5, 35, "gia' in anagrafica!");
        canvas_draw_str(canvas, 5, 55, "[BACK] Annulla  [OK] Forza");
    } else if (app->state == AppStateDone) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 5, 30, "Creato con Successo!");
        canvas_draw_str(canvas, 5, 45, app->status_msg);
        canvas_draw_str(canvas, 5, 60, "Premi BACK per uscire");
    } else if (app->state == AppStateError) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 5, 30, "Errore SD Card.");
        canvas_draw_str(canvas, 5, 55, "Premi BACK per uscire");
    }
    furi_mutex_release(app->mutex);
}

static void input_callback(InputEvent* input_event, void* ctx) {
    FuriMessageQueue* event_queue = ctx;
    furi_message_queue_put(event_queue, input_event, FuriWaitForever);
}

int32_t t5567_encoder_app(void* p) {
    UNUSED(p);
    T5567EncoderApp* app = malloc(sizeof(T5567EncoderApp));
    memset(app, 0, sizeof(T5567EncoderApp));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->state = AppStateInput;
    app->cur_idx = 0;
    
    // Inizializza tutto a 0
    app->digits[0] = 0; app->digits[1] = 0; app->digits[2] = 0; app->digits[3] = 0;
    
    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, draw_callback, app);
    view_port_input_callback_set(view_port, input_callback, event_queue);
    
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    InputEvent event;
    bool running = true;
    
    while(running) {
        if(furi_message_queue_get(event_queue, &event, 100) == FuriStatusOk) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            
            if (app->state == AppStateInput) {
                if (event.type == InputTypeShort) {
                    if (event.key == InputKeyBack) {
                        running = false;
                    } else if (event.key == InputKeyUp) {
                        if (app->digits[app->cur_idx] == 9) app->digits[app->cur_idx] = 0;
                        else app->digits[app->cur_idx]++;
                    } else if (event.key == InputKeyDown) {
                        if (app->digits[app->cur_idx] == 0) app->digits[app->cur_idx] = 9;
                        else app->digits[app->cur_idx]--;
                    } else if (event.key == InputKeyRight) {
                        if (app->cur_idx < 3) app->cur_idx++;
                    } else if (event.key == InputKeyLeft) {
                        if (app->cur_idx > 0) app->cur_idx--;
                    }
                } else if (event.type == InputTypeLong && event.key == InputKeyOk) {
                    // Costruisci la stringa matricola quando si tiene premuto OK
                    app->buffer_matricola[0] = '0' + app->digits[0];
                    app->buffer_matricola[1] = '0' + app->digits[1];
                    app->buffer_matricola[2] = '0' + app->digits[2];
                    app->buffer_matricola[3] = '0' + app->digits[3];
                    app->buffer_matricola[4] = '\0';
                    
                    if (check_if_matricola_exists(app->buffer_matricola)) {
                        app->state = AppStateWarning;
                    } else {
                        if (generate_rfid_file(app->buffer_matricola)) {
                            snprintf(app->status_msg, sizeof(app->status_msg), "SD/lfrfid/badge_%s.rfid", app->buffer_matricola);
                            app->state = AppStateDone;
                        } else {
                            app->state = AppStateError;
                        }
                    }
                }
            } else if (app->state == AppStateWarning) {
                if (event.type == InputTypeShort) {
                    if (event.key == InputKeyBack) {
                        app->state = AppStateInput;
                    } else if (event.key == InputKeyOk) {
                        if (generate_rfid_file(app->buffer_matricola)) {
                            snprintf(app->status_msg, sizeof(app->status_msg), "Sovrascritto badge_%s.rfid", app->buffer_matricola);
                            app->state = AppStateDone;
                        } else {
                            app->state = AppStateError;
                        }
                    }
                }
            } else if (app->state == AppStateDone || app->state == AppStateError) {
                if (event.type == InputTypeShort && (event.key == InputKeyBack || event.key == InputKeyOk)) {
                    running = false;
                }
            }
            furi_mutex_release(app->mutex);
        }
        view_port_update(view_port);
    }
    
    view_port_enabled_set(view_port, false);
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_message_queue_free(event_queue);
    furi_mutex_free(app->mutex);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
