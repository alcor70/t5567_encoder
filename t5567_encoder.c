#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>
#include <dialogs/dialogs.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define TAG "T5567_Encoder"
#define CSV_FILE_PATH EXT_PATH("apps_data/t5567_encoder/anagrafica.csv")
#define RFID_SAVE_PATH EXT_PATH("lfrfid/badge_%s.rfid")

// ----- Logic Functions -----

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

// Check if a string exists at the beginning of any line in the CSV file
bool check_if_matricola_exists(const char* matricola) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool found = false;

    if(storage_file_open(file, CSV_FILE_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char buffer[64];
        uint16_t read_bytes;
        uint16_t search_len = strlen(matricola);
        
        // Simple line by line reading (naive approach for smallish files)
        // A robust implementation would handle cross-buffer line breaks
        // But for Flipper, we read chunks and look for "\n" + matricola + ","
        
        // For simplicity in this example we will read the whole file line by line
        // assuming standard Flipper stream functions or manual buffer parsing.
        
        // Very basic approach: read char by char to find lines
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
                    // end of first column
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
    } else {
        FURI_LOG_E(TAG, "Failed to open CSV file for reading");
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    
    return found;
}

// Generate the Flipper RFID file format
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
        
        // Flipper specific RFID file header for T5577 raw or EM4100
        // A standard 125kHz EM4100 file format looks like this:
        /*
            Filetype: Flipper RFID device
            Version: 1
            Key type: EM4100
            Data: 00 00 00 A2 44
        */
        char file_content[256];
        snprintf(file_content, sizeof(file_content),
            "Filetype: Flipper RFID device\n"
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

// ----- UI & App State -----

typedef enum {
    AppStateInput,
    AppStateWarning,
    AppStateDone,
    AppStateError
} AppState;

typedef struct {
    char input_buffer[10];
    uint8_t input_index;
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
        canvas_draw_str(canvas, 5, 27, "Insert Matricola:");
        
        canvas_set_font(canvas, FontBigNumbers);
        
        // draw a cursor
        char display_str[12];
        snprintf(display_str, sizeof(display_str), "%s_", app->input_buffer);
        canvas_draw_str(canvas, 5, 50, display_str);

        if(app->input_index > 0) {
            canvas_set_font(canvas, FontSecondary);
            canvas_draw_str(canvas, 90, 60, "[OK] Write");
        }
    } else if (app->state == AppStateWarning) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 5, 25, "Warning: Matricola");
        canvas_draw_str(canvas, 5, 35, "already in CSV!");
        canvas_draw_str(canvas, 5, 55, "[BACK] Cancel   [OK] Write");
    } else if (app->state == AppStateDone) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 5, 25, "Success!");
        canvas_draw_str(canvas, 5, 40, app->status_msg);
        canvas_draw_str(canvas, 5, 55, "Press BACK to exit");
    } else if (app->state == AppStateError) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 5, 30, "Error generating file.");
        canvas_draw_str(canvas, 5, 55, "Press BACK to exit");
    }
    
    furi_mutex_release(app->mutex);
}

static void input_callback(InputEvent* input_event, void* ctx) {
    FuriMessageQueue* event_queue = ctx;
    furi_message_queue_put(event_queue, input_event, FuriWaitForever);
}

// ----- Entry Point -----

int32_t t5567_encoder_app_entry(void* p) {
    UNUSED(p);
    
    T5567EncoderApp* app = malloc(sizeof(T5567EncoderApp));
    memset(app, 0, sizeof(T5567EncoderApp));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->state = AppStateInput;
    
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
            if(event.type == InputTypeShort) {
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                
                if (app->state == AppStateInput) {
                    if (event.key == InputKeyBack) {
                        if (app->input_index > 0) {
                            app->input_index--;
                            app->input_buffer[app->input_index] = '\0';
                        } else {
                            running = false;
                        }
                    } else if (event.key == InputKeyUp && app->input_index < 4) {
                        // Just an example, ideally use a virtual keyboard or directional input for digits
                        // For a real FAP, we would use a Dialogs text input
                        // Here we just mock inputs
                    } else if (event.key == InputKeyOk && app->input_index > 0) {
                        // Check if exists
                        if (check_if_matricola_exists(app->input_buffer)) {
                            app->state = AppStateWarning;
                        } else {
                            if (generate_rfid_file(app->input_buffer)) {
                                snprintf(app->status_msg, sizeof(app->status_msg), "badge_%s.rfid created", app->input_buffer);
                                app->state = AppStateDone;
                            } else {
                                app->state = AppStateError;
                            }
                        }
                    }
                } else if (app->state == AppStateWarning) {
                    if (event.key == InputKeyBack) {
                        app->state = AppStateInput;
                    } else if (event.key == InputKeyOk) {
                        if (generate_rfid_file(app->input_buffer)) {
                            snprintf(app->status_msg, sizeof(app->status_msg), "badge_%s.rfid overwritten", app->input_buffer);
                            app->state = AppStateDone;
                        } else {
                            app->state = AppStateError;
                        }
                    }
                } else if (app->state == AppStateDone || app->state == AppStateError) {
                    if (event.key == InputKeyBack) {
                        running = false;
                    }
                }
                
                furi_mutex_release(app->mutex);
            }
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
