#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdarg.h>
#include <SDL.h>

// --- Z80 Flag Register Bits ---
#define FLAG_C  (1 << 0) // Carry Flag
#define FLAG_N  (1 << 1) // Add/Subtract Flag
#define FLAG_PV (1 << 2) // Parity/Overflow Flag
#define FLAG_H  (1 << 4) // Half Carry Flag
#define FLAG_Z  (1 << 6) // Zero Flag
#define FLAG_S  (1 << 7) // Sign Flag

// --- Global Memory ---
uint8_t memory[0x10000]; // 65536 bytes

// --- ZX Spectrum Constants ---
#define SCREEN_WIDTH 256
#define SCREEN_HEIGHT 192
#define BORDER_SIZE 48
#define TOTAL_WIDTH (SCREEN_WIDTH + BORDER_SIZE * 2)
#define TOTAL_HEIGHT (SCREEN_HEIGHT + BORDER_SIZE * 2)
#define DISPLAY_SCALE 3
#define VRAM_START 0x4000
#define ATTR_START 0x5800
#define T_STATES_PER_FRAME 69888 // 3.5MHz / 50Hz (Spectrum CPU speed)

const double CPU_CLOCK_HZ = 3500000.0;

// --- SDL Globals ---
SDL_Window* window = NULL;
SDL_Renderer* renderer = NULL;
SDL_Texture* texture = NULL;
uint32_t pixels[ TOTAL_WIDTH * TOTAL_HEIGHT ];

// --- Audio Globals ---
volatile int beeper_state = 0; // 0 = low, 1 = high
const int AUDIO_AMPLITUDE = 2000;
static const double BEEPER_IDLE_RESET_SAMPLES = 512.0;
static const double BEEPER_REWIND_TOLERANCE_SAMPLES = 8.0;
int audio_sample_rate = 44100;
int audio_available = 0;

static double beeper_max_latency_samples = 256.0;
static double beeper_latency_throttle_samples = 320.0;
static double beeper_latency_release_samples = 256.0;
static double beeper_latency_trim_samples = 512.0;
static const double BEEPER_HP_ALPHA = 0.995;

static const char* audio_dump_path = NULL;

static const int16_t TAPE_WAV_AMPLITUDE = 20000;

// --- Tape Constants ---
static const int TAPE_PILOT_PULSE_TSTATES = 2168;
static const int TAPE_SYNC_FIRST_PULSE_TSTATES = 667;
static const int TAPE_SYNC_SECOND_PULSE_TSTATES = 735;
static const int TAPE_BIT0_PULSE_TSTATES = 855;
static const int TAPE_BIT1_PULSE_TSTATES = 1710;
static const int TAPE_HEADER_PILOT_COUNT = 8063;
static const int TAPE_DATA_PILOT_COUNT = 3223;
static const uint64_t TAPE_SILENCE_THRESHOLD_TSTATES = 350000ULL; // 0.1 second

typedef struct {
    uint8_t* data;
    uint32_t length;
    uint32_t pause_ms;
} TapeBlock;

typedef struct {
    TapeBlock* blocks;
    size_t count;
    size_t capacity;
} TapeImage;

typedef struct {
    uint32_t duration;
} TapePulse;

typedef struct {
    TapePulse* pulses;
    size_t count;
    size_t capacity;
    int initial_level;
    uint32_t sample_rate;
} TapeWaveform;

typedef enum {
    TAPE_FORMAT_NONE,
    TAPE_FORMAT_TAP,
    TAPE_FORMAT_TZX,
    TAPE_FORMAT_WAV
} TapeFormat;

typedef enum {
    TAPE_PHASE_IDLE,
    TAPE_PHASE_PILOT,
    TAPE_PHASE_SYNC1,
    TAPE_PHASE_SYNC2,
    TAPE_PHASE_DATA,
    TAPE_PHASE_PAUSE,
    TAPE_PHASE_DONE
} TapePhase;

typedef struct {
    TapeImage image;
    TapeWaveform waveform;
    TapeFormat format;
    int use_waveform_playback;
    size_t current_block;
    TapePhase phase;
    int pilot_pulses_remaining;
    size_t data_byte_index;
    uint8_t data_bit_mask;
    int data_pulse_half;
    uint64_t next_transition_tstate;
    uint64_t pause_end_tstate;
    int level;
    int playing;
    size_t waveform_index;
    uint64_t paused_transition_remaining;
    uint64_t paused_pause_remaining;
    uint64_t position_tstates;
    uint64_t position_start_tstate;
    uint64_t last_transition_tstate;
} TapePlaybackState;

typedef enum {
    TAPE_OUTPUT_NONE,
    TAPE_OUTPUT_TAP,
    TAPE_OUTPUT_WAV
} TapeOutputFormat;

typedef struct {
    TapeImage recorded;
    TapePulse* pulses;
    size_t pulse_count;
    size_t pulse_capacity;
    uint64_t last_transition_tstate;
    int last_level;
    int block_active;
    int enabled;
    const char* output_path;
    int block_start_level;
    uint32_t sample_rate;
    int16_t* audio_samples;
    size_t audio_sample_count;
    size_t audio_sample_capacity;
    int16_t* wav_prefix_samples;
    size_t wav_prefix_sample_count;
    TapeOutputFormat output_format;
    int recording;
    int session_dirty;
    uint64_t position_tstates;
    uint64_t position_start_tstate;
    int append_mode;
    uint32_t append_data_chunk_offset;
    uint32_t append_existing_data_bytes;
    uint64_t wav_existing_samples;
    uint64_t wav_head_samples;
    int wav_requires_truncate;
} TapeRecorder;

static TapePlaybackState tape_playback = {0};
static TapeRecorder tape_recorder = {0};
static int tape_ear_state = 1;
static int tape_input_enabled = 0;

typedef enum {
    TAPE_DECK_STATUS_IDLE,
    TAPE_DECK_STATUS_PLAY,
    TAPE_DECK_STATUS_STOP,
    TAPE_DECK_STATUS_REWIND,
    TAPE_DECK_STATUS_RECORD
} TapeDeckStatus;

static TapeDeckStatus tape_deck_status = TAPE_DECK_STATUS_IDLE;
static int tape_debug_logging = 0;

static void tape_log(const char* fmt, ...) {
    if (!tape_debug_logging || !fmt) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    fputs("[TAPE] ", stderr);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

static uint64_t tape_wav_shared_position_tstates = 0;

typedef enum {
    TAPE_CONTROL_ACTION_NONE = 0,
    TAPE_CONTROL_ACTION_PLAY,
    TAPE_CONTROL_ACTION_STOP,
    TAPE_CONTROL_ACTION_REWIND,
    TAPE_CONTROL_ACTION_RECORD
} TapeControlAction;

#define TAPE_CONTROL_BUTTON_MAX 4
#define TAPE_CONTROL_ICON_WIDTH 7
#define TAPE_CONTROL_ICON_HEIGHT 7

typedef struct {
    TapeControlAction action;
    SDL_Rect rect;
    int enabled;
    int visible;
} TapeControlButton;

typedef struct {
    TapeControlAction action;
    uint8_t rows[TAPE_CONTROL_ICON_HEIGHT];
} TapeControlIcon;

static TapeControlButton tape_control_buttons[TAPE_CONTROL_BUTTON_MAX];
static int tape_control_button_count = 0;

#define TAPE_OVERLAY_FONT_WIDTH 5
#define TAPE_OVERLAY_FONT_HEIGHT 7

typedef struct {
    char ch;
    uint8_t rows[TAPE_OVERLAY_FONT_HEIGHT];
} TapeOverlayGlyph;

static const TapeOverlayGlyph tape_overlay_font[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'0', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
    {':', {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x04, 0x04, 0x00}}
};

static const TapeControlIcon tape_control_icons[] = {
    {
        TAPE_CONTROL_ACTION_PLAY,
        {0x08, 0x0C, 0x0E, 0x0F, 0x0E, 0x0C, 0x08}
    },
    {
        TAPE_CONTROL_ACTION_STOP,
        {0x00, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x00}
    },
    {
        TAPE_CONTROL_ACTION_REWIND,
        {0x48, 0x6C, 0x7E, 0x7F, 0x7E, 0x6C, 0x48}
    },
    {
        TAPE_CONTROL_ACTION_RECORD,
        {0x00, 0x1C, 0x3E, 0x3E, 0x3E, 0x1C, 0x00}
    }
};

typedef struct {
    uint8_t value;
    uint64_t t_state;
} UlaWriteEvent;

static UlaWriteEvent ula_write_queue[64];
static size_t ula_write_count = 0;
static uint64_t ula_instruction_base_tstate = 0;
static int* ula_instruction_progress_ptr = NULL;

static TapeFormat tape_input_format = TAPE_FORMAT_NONE;
static const char* tape_input_path = NULL;

static int string_ends_with_case_insensitive(const char* str, const char* suffix);
static TapeFormat tape_format_from_extension(const char* path);
static int tape_load_image(const char* path, TapeFormat format, TapeImage* image);
static void tape_free_image(TapeImage* image);
static int tape_image_add_block(TapeImage* image, const uint8_t* data, uint32_t length, uint32_t pause_ms);
static void tape_reset_playback(TapePlaybackState* state);
static void tape_start_playback(TapePlaybackState* state, uint64_t start_time);
static void tape_pause_playback(TapePlaybackState* state, uint64_t current_t_state);
static int tape_resume_playback(TapePlaybackState* state, uint64_t current_t_state);
static void tape_rewind_playback(TapePlaybackState* state);
static int tape_begin_block(TapePlaybackState* state, size_t block_index, uint64_t start_time);
static void tape_update(uint64_t current_t_state);
static int tape_current_block_pilot_count(const TapePlaybackState* state);
static void tape_waveform_reset(TapeWaveform* waveform);
static int tape_waveform_add_pulse(TapeWaveform* waveform, uint64_t duration);
static int tape_generate_waveform_from_image(const TapeImage* image, TapeWaveform* waveform);
static int tape_load_wav(const char* path, TapePlaybackState* state);
static int tape_create_blank_wav(const char* path, uint32_t sample_rate);
static void tape_recorder_enable(const char* path, TapeOutputFormat format);
static int tape_recorder_start_session(uint64_t current_t_state, int append_mode);
static void tape_recorder_stop_session(uint64_t current_t_state, int finalize_output);
static void tape_recorder_handle_mic(uint64_t t_state, int level);
static void tape_recorder_update(uint64_t current_t_state, int force_flush);
static int tape_recorder_write_output(void);
static void tape_recorder_reset_audio(void);
static void tape_recorder_reset_wav_prefix(void);
static size_t tape_recorder_samples_from_tstates(uint64_t duration);
static uint64_t tape_recorder_tstates_from_samples(uint64_t sample_count);
static int tape_recorder_append_audio_samples(int level, size_t sample_count);
static void tape_recorder_append_block_audio(uint64_t idle_cycles);
static int tape_recorder_write_wav(void);
static int tape_recorder_prepare_append_wav(uint32_t* data_chunk_offset,
                                            uint32_t* existing_bytes,
                                            uint32_t* sample_rate_out);
static int tape_recorder_prepare_wav_session(uint64_t head_tstates);
static void tape_shutdown(void);
static void tape_deck_play(uint64_t current_t_state);
static void tape_deck_stop(uint64_t current_t_state);
static void tape_deck_rewind(uint64_t current_t_state);
static void tape_deck_record(uint64_t current_t_state, int append_mode);
static int tape_handle_control_key(const SDL_Event* event);
static int tape_handle_mouse_button(const SDL_Event* event);
static void tape_playback_accumulate_elapsed(TapePlaybackState* state, uint64_t stop_t_state);
static uint64_t tape_playback_elapsed_tstates(const TapePlaybackState* state, uint64_t current_t_state);
static uint64_t tape_recorder_elapsed_tstates(uint64_t current_t_state);
static void tape_render_overlay(void);
static void tape_wav_seek_playback(TapePlaybackState* state, uint64_t position_tstates);
static void ula_queue_port_value(uint8_t value);
static void ula_process_port_events(uint64_t current_t_state);

static FILE* audio_dump_file = NULL;
static uint32_t audio_dump_data_bytes = 0;

#define BEEPER_EVENT_CAPACITY 8192

typedef struct {
    uint64_t t_state;
    uint8_t level;
} BeeperEvent;

static BeeperEvent beeper_events[BEEPER_EVENT_CAPACITY];
static size_t beeper_event_head = 0;
static size_t beeper_event_tail = 0;
static uint64_t beeper_last_event_t_state = 0;
static double beeper_cycles_per_sample = 0.0;
static double beeper_playback_position = 0.0;
static double beeper_writer_cursor = 0.0;
static double beeper_hp_last_input = 0.0;
static double beeper_hp_last_output = 0.0;
static int beeper_playback_level = 0;
static int beeper_latency_warning_active = 0;
static int beeper_idle_log_active = 0;
static uint64_t beeper_idle_reset_count = 0;
static int beeper_logging_enabled = 0;

#define BEEPER_LOG(...)                                              \
    do {                                                             \
        if (beeper_logging_enabled) {                                \
            fprintf(stderr, __VA_ARGS__);                            \
        }                                                            \
    } while (0)

static size_t beeper_pending_event_count(void);
static void beeper_force_resync(uint64_t sync_t_state);

// --- Timing Globals ---
uint64_t total_t_states = 0; // A global clock for the entire CPU

// --- ZX Spectrum Colours ---
const uint32_t spectrum_colors[8] = {0x000000FF,0x0000CDFF,0xCD0000FF,0xCD00CDFF,0x00CD00FF,0x00CDCDFF,0xCDCD00FF,0xCFCFCFFF};
const uint32_t spectrum_bright_colors[8] = {0x000000FF,0x0000FFFF,0xFF0000FF,0xFF00FFFF,0x00FF00FF,0x00FFFFFF,0xFFFF00FF,0xFFFFFFFF};

// --- Keyboard State ---
uint8_t keyboard_matrix[8] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// --- ROM Handling ---
static const char *default_rom_filename = "48.rom";

static char *build_executable_relative_path(const char *executable_path, const char *filename);
// --- Z80 CPU State ---
typedef struct Z80 {
    // 8-bit Main Registers
    uint8_t reg_A; uint8_t reg_F;
    uint8_t reg_B; uint8_t reg_C;
    uint8_t reg_D; uint8_t reg_E;
    uint8_t reg_H; uint8_t reg_L;

    // 8-bit Alternate Registers
    uint8_t alt_reg_A; uint8_t alt_reg_F;
    uint8_t alt_reg_B; uint8_t alt_reg_C;
    uint8_t alt_reg_D; uint8_t alt_reg_E;
    uint8_t alt_reg_H; uint8_t alt_reg_L;

    // 8-bit Special Registers
    uint8_t reg_I; // Interrupt Vector
    uint8_t reg_R; // Memory Refresh

    // 16-bit Index Registers
    uint16_t reg_IX;
    uint16_t reg_IY;

    // 16-bit Special Registers
    uint16_t reg_SP; // Stack Pointer
    uint16_t reg_PC; // Program Counter

    // Interrupt Flip-Flops
    int iff1; // Main interrupt enable flag
    int iff2; // Temp storage for iff1 (used by NMI)
    int interruptMode; // IM 0, 1, or 2
    int ei_delay; // Flag to handle EI's delayed effect
    int halted; // Flag for HALT instruction

} Z80;


// --- Function Prototypes ---
uint8_t readByte(uint16_t addr); void writeByte(uint16_t addr, uint8_t val);
uint16_t readWord(uint16_t addr); void writeWord(uint16_t addr, uint16_t val);
uint8_t io_read(uint16_t port); void io_write(uint16_t port, uint8_t value);
int cpu_step(Z80* cpu); int init_sdl(void); void cleanup_sdl(void); void render_screen(void);
int map_sdl_key_to_spectrum(SDL_Keycode sdl_key, int* row_ptr, uint8_t* mask_ptr);
int cpu_interrupt(Z80* cpu, uint8_t data_bus);
int cpu_ddfd_cb_step(Z80* cpu, uint16_t* index_reg, int is_ix);
void audio_callback(void* userdata, Uint8* stream, int len);
static void beeper_reset_audio_state(uint64_t current_t_state, int current_level);
static void beeper_set_latency_limit(double sample_limit);
static void beeper_push_event(uint64_t t_state, int level);
static size_t beeper_catch_up_to(double catch_up_position, double playback_position_snapshot);
static double beeper_current_latency_samples(void);
static double beeper_latency_threshold(void);
static Uint32 beeper_recommended_throttle_delay(double latency_samples);
static int audio_dump_start(const char* path, uint32_t sample_rate);
static void audio_dump_write_samples(const Sint16* samples, size_t count);
static void audio_dump_finish(void);
static void audio_dump_abort(void);


// --- ROM Utilities ---
static char *build_executable_relative_path(const char *executable_path, const char *filename) {
    if (!executable_path || !filename) {
        return NULL;
    }

    const char *last_sep = strrchr(executable_path, '/');
#ifdef _WIN32
    const char *last_backslash = strrchr(executable_path, '\\');
    if (!last_sep || (last_backslash && last_backslash > last_sep)) {
        last_sep = last_backslash;
    }
#endif
    if (!last_sep) {
        return NULL;
    }

    size_t dir_len = (size_t)(last_sep - executable_path + 1);
    size_t name_len = strlen(filename);
    char *joined = (char *)malloc(dir_len + name_len + 1);
    if (!joined) {
        return NULL;
    }
    memcpy(joined, executable_path, dir_len);
    memcpy(joined + dir_len, filename, name_len + 1);
    return joined;
}


// --- Memory Access Helpers ---
uint8_t readByte(uint16_t addr) {
    if (addr < 0x4000) { return memory[addr]; } // ROM Read
    return memory[addr];                        // RAM Read
}
void writeByte(uint16_t addr, uint8_t val) {
    if (addr < 0x4000) { return; } // No Write to ROM
    memory[addr] = val;            // RAM Write
}
uint16_t readWord(uint16_t addr) { uint8_t lo = readByte(addr); uint8_t hi = readByte(addr+1); return (hi << 8) | lo; }
void writeWord(uint16_t addr, uint16_t val) { uint8_t lo=val&0xFF; uint8_t hi=(val>>8)&0xFF; writeByte(addr, lo); writeByte(addr+1, hi); }

// --- I/O Port Access Helpers ---
uint8_t border_color_idx = 0;

static void beeper_reset_audio_state(uint64_t current_t_state, int current_level) {
    beeper_event_head = 0;
    beeper_event_tail = 0;
    beeper_last_event_t_state = current_t_state;
    beeper_playback_position = (double)current_t_state;
    beeper_writer_cursor = (double)current_t_state;
    beeper_playback_level = current_level ? 1 : 0;
    double baseline = (current_level ? 1.0 : -1.0) * (double)AUDIO_AMPLITUDE;
    beeper_hp_last_input = baseline;
    beeper_hp_last_output = 0.0;
    beeper_state = current_level ? 1 : 0;
    beeper_idle_log_active = 0;
}

static void beeper_force_resync(uint64_t sync_t_state) {
    double baseline = (beeper_playback_level ? 1.0 : -1.0) * (double)AUDIO_AMPLITUDE;
    beeper_event_head = 0;
    beeper_event_tail = 0;
    beeper_playback_position = (double)sync_t_state;
    beeper_writer_cursor = (double)sync_t_state;
    beeper_last_event_t_state = sync_t_state;
    beeper_hp_last_input = baseline;
    beeper_hp_last_output = 0.0;
    beeper_idle_log_active = 0;
}

static size_t beeper_pending_event_count(void) {
    size_t head = beeper_event_head;
    size_t tail = beeper_event_tail;

    if (tail >= head) {
        return tail - head;
    }

    return (size_t)BEEPER_EVENT_CAPACITY - head + tail;
}

static double beeper_latency_threshold(void) {
    double threshold = beeper_latency_throttle_samples;
    if (threshold < beeper_max_latency_samples) {
        threshold = beeper_max_latency_samples;
    }
    return threshold;
}

static Uint32 beeper_recommended_throttle_delay(double latency_samples) {
    double threshold = beeper_latency_threshold();
    if (latency_samples <= threshold || audio_sample_rate <= 0) {
        return 0;
    }

    double over = latency_samples - threshold;
    double limit = beeper_max_latency_samples;
    if (limit <= 0.0) {
        limit = 256.0;
    }

    if (over <= limit * 0.1) {
        return 0;
    }

    if (over <= limit * 0.5) {
        return 1;
    }

    double estimated_ms = ceil((over * 1000.0) / (double)audio_sample_rate);
    if (estimated_ms < 2.0) {
        estimated_ms = 2.0;
    } else if (estimated_ms > 8.0) {
        estimated_ms = 8.0;
    }

    return (Uint32)estimated_ms;
}

static double beeper_current_latency_samples(void) {
    if (!audio_available || beeper_cycles_per_sample <= 0.0) {
        beeper_latency_warning_active = 0;
        return 0.0;
    }

    double writer_cursor;
    double playback_position;

    SDL_LockAudio();
    writer_cursor = beeper_writer_cursor;
    playback_position = beeper_playback_position;
    SDL_UnlockAudio();

    double latency_cycles = writer_cursor - playback_position;
    if (latency_cycles <= 0.0) {
        if (beeper_latency_warning_active) {
            beeper_latency_warning_active = 0;
        }
        return 0.0;
    }

    double latency_samples = latency_cycles / beeper_cycles_per_sample;
    if (latency_samples < 0.0) {
        latency_samples = 0.0;
    }

    double throttle_threshold = beeper_latency_threshold();
    if (latency_samples >= throttle_threshold) {
        if (!beeper_latency_warning_active) {
            BEEPER_LOG(
                "[BEEPER] latency %.2f samples exceeds throttle %.2f (clamp %.2f); throttling CPU until audio catches up\n",
                latency_samples,
                throttle_threshold,
                beeper_max_latency_samples);
            beeper_latency_warning_active = 1;
        }
    } else {
        double release_threshold = beeper_latency_release_samples;
        if (release_threshold < beeper_max_latency_samples) {
            release_threshold = beeper_max_latency_samples;
        }
        if (latency_samples < release_threshold && beeper_latency_warning_active) {
            beeper_latency_warning_active = 0;
        }
    }

    return latency_samples;
}

static void beeper_set_latency_limit(double sample_limit) {
    if (sample_limit < 64.0) {
        sample_limit = 64.0;
    }
    beeper_max_latency_samples = sample_limit;

    double headroom = sample_limit * 0.5;
    if (headroom < 128.0) {
        headroom = 128.0;
    } else if (headroom > 2048.0) {
        headroom = 2048.0;
    }

    beeper_latency_throttle_samples = beeper_max_latency_samples + headroom;

    double release = beeper_latency_throttle_samples - headroom * 0.5;
    if (release < beeper_max_latency_samples) {
        release = beeper_max_latency_samples;
    }
    beeper_latency_release_samples = release;

    double trim_margin = headroom;
    if (trim_margin < beeper_max_latency_samples) {
        trim_margin = beeper_max_latency_samples;
    }
    beeper_latency_trim_samples = beeper_latency_throttle_samples + trim_margin;
}

static void audio_dump_write_uint16(uint8_t* dst, uint16_t value) {
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static void audio_dump_write_uint32(uint8_t* dst, uint32_t value) {
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
    dst[2] = (uint8_t)((value >> 16) & 0xFFu);
    dst[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static int audio_dump_start(const char* path, uint32_t sample_rate) {
    if (!path) {
        return 0;
    }

    audio_dump_file = fopen(path, "wb");
    if (!audio_dump_file) {
        fprintf(stderr, "[BEEPER] failed to open audio dump '%s': %s\n", path, strerror(errno));
        return 0;
    }

    audio_dump_data_bytes = 0;

    uint8_t header[44];
    memset(header, 0, sizeof(header));

    memcpy(header, "RIFF", 4);
    audio_dump_write_uint32(header + 4, 36u);
    memcpy(header + 8, "WAVE", 4);
    memcpy(header + 12, "fmt ", 4);
    audio_dump_write_uint32(header + 16, 16u); // PCM chunk size
    audio_dump_write_uint16(header + 20, 1u);  // PCM format
    audio_dump_write_uint16(header + 22, 1u);  // mono
    audio_dump_write_uint32(header + 24, sample_rate);
    uint32_t byte_rate = sample_rate * 2u; // mono, 16-bit
    audio_dump_write_uint32(header + 28, byte_rate);
    audio_dump_write_uint16(header + 32, 2u); // block align
    audio_dump_write_uint16(header + 34, 16u); // bits per sample
    memcpy(header + 36, "data", 4);
    audio_dump_write_uint32(header + 40, 0u);

    if (fwrite(header, sizeof(header), 1, audio_dump_file) != 1) {
        fprintf(stderr, "[BEEPER] failed to write WAV header to '%s'\n", path);
        audio_dump_abort();
        return 0;
    }

    BEEPER_LOG("[BEEPER] dumping audio to %s\n", path);
    return 1;
}

static void audio_dump_abort(void) {
    if (audio_dump_file) {
        fclose(audio_dump_file);
        audio_dump_file = NULL;
    }
    audio_dump_data_bytes = 0;
}

static void audio_dump_write_samples(const Sint16* samples, size_t count) {
    if (!audio_dump_file || !samples || count == 0) {
        return;
    }

    size_t written = fwrite(samples, sizeof(Sint16), count, audio_dump_file);
    if (written != count) {
        fprintf(stderr,
                "[BEEPER] audio dump write failed after %zu samples\n",
                (size_t)(audio_dump_data_bytes / 2u));
        audio_dump_abort();
        return;
    }

    audio_dump_data_bytes += (uint32_t)(written * sizeof(Sint16));
}

static void audio_dump_finish(void) {
    if (!audio_dump_file) {
        return;
    }

    uint32_t riff_size = 36u + audio_dump_data_bytes;
    uint32_t data_size = audio_dump_data_bytes;

    if (fseek(audio_dump_file, 4L, SEEK_SET) == 0) {
        uint8_t size_bytes[4];
        audio_dump_write_uint32(size_bytes, riff_size);
        fwrite(size_bytes, sizeof(size_bytes), 1, audio_dump_file);
    }

    if (fseek(audio_dump_file, 40L, SEEK_SET) == 0) {
        uint8_t data_bytes_buf[4];
        audio_dump_write_uint32(data_bytes_buf, data_size);
        fwrite(data_bytes_buf, sizeof(data_bytes_buf), 1, audio_dump_file);
    }

    fclose(audio_dump_file);
    audio_dump_file = NULL;
    audio_dump_data_bytes = 0;
}

static uint64_t tape_pause_to_tstates(uint32_t pause_ms) {
    if (pause_ms == 0) {
        return 0;
    }
    double tstates = ((double)pause_ms / 1000.0) * CPU_CLOCK_HZ;
    if (tstates <= 0.0) {
        return 0;
    }
    return (uint64_t)(tstates + 0.5);
}

static const char* tape_header_type_name(uint8_t header_type) {
    switch (header_type) {
        case 0x00:
            return "Program";
        case 0x01:
            return "Number array";
        case 0x02:
            return "Character array";
        case 0x03:
            return "Bytes";
        default:
            return "Unknown";
    }
}

static void tape_log_block_summary(const TapeBlock* block, size_t index) {
    if (!tape_debug_logging) {
        return;
    }

    if (!block) {
        tape_log("Block %zu: <null>\n", index);
        return;
    }

    tape_log("Block %zu: length=%u pause=%u", index, block->length, block->pause_ms);
    if (!block->data || block->length == 0) {
        tape_log(" (empty)\n");
        return;
    }

    uint8_t flag = block->data[0];
    tape_log(" flag=0x%02X", flag);

    if (flag == 0x00 && block->length >= 19) {
        uint8_t header_type = block->data[1];
        char name[11];
        size_t copy_len = 10u;
        size_t available = 0u;
        if (block->length > 2u) {
            available = block->length - 2u;
        }
        if (available < copy_len) {
            copy_len = available;
        }
        memset(name, '\0', sizeof(name));
        if (copy_len > 0u) {
            memcpy(name, &block->data[2], copy_len);
            for (size_t i = 0; i < copy_len; ++i) {
                if ((unsigned char)name[i] < 32u || (unsigned char)name[i] > 126u) {
                    name[i] = '?';
                }
            }
            for (int i = (int)copy_len - 1; i >= 0; --i) {
                if (name[i] == ' ') {
                    name[i] = '\0';
                } else {
                    break;
                }
            }
        }
        uint16_t data_length = (uint16_t)block->data[12] | ((uint16_t)block->data[13] << 8);
        uint16_t param1 = (uint16_t)block->data[14] | ((uint16_t)block->data[15] << 8);
        uint16_t param2 = (uint16_t)block->data[16] | ((uint16_t)block->data[17] << 8);
        tape_log(" header=%s name='%s' data_len=%u param1=%u param2=%u\n",
                 tape_header_type_name(header_type),
                 name,
                 data_length,
                 param1,
                 param2);
        return;
    }

    if (flag == 0xFF && block->length >= 2) {
        uint32_t payload_length = block->length - 2u;
        uint8_t checksum = block->data[block->length - 1];
        tape_log(" data payload_len=%u checksum=0x%02X\n", payload_length, checksum);
        return;
    }

    tape_log("\n");
}

static void tape_free_image(TapeImage* image) {
    if (!image) {
        return;
    }
    if (image->blocks) {
        for (size_t i = 0; i < image->count; ++i) {
            free(image->blocks[i].data);
            image->blocks[i].data = NULL;
        }
        free(image->blocks);
    }
    image->blocks = NULL;
    image->count = 0;
    image->capacity = 0;
}

static int tape_image_add_block(TapeImage* image, const uint8_t* data, uint32_t length, uint32_t pause_ms) {
    if (!image) {
        return 0;
    }

    if (image->count == image->capacity) {
        size_t new_capacity = image->capacity ? image->capacity * 2 : 8;
        TapeBlock* new_blocks = (TapeBlock*)realloc(image->blocks, new_capacity * sizeof(TapeBlock));
        if (!new_blocks) {
            return 0;
        }
        image->blocks = new_blocks;
        image->capacity = new_capacity;
    }

    TapeBlock* block = &image->blocks[image->count];
    block->length = length;
    block->pause_ms = pause_ms;
    block->data = NULL;

    if (length > 0) {
        block->data = (uint8_t*)malloc(length);
        if (!block->data) {
            return 0;
        }
        memcpy(block->data, data, length);
    }

    tape_log_block_summary(block, image->count);
    image->count++;
    return 1;
}

static void tape_waveform_reset(TapeWaveform* waveform) {
    if (!waveform) {
        return;
    }
    if (waveform->pulses) {
        free(waveform->pulses);
        waveform->pulses = NULL;
    }
    waveform->count = 0;
    waveform->capacity = 0;
    waveform->initial_level = 1;
    waveform->sample_rate = 0u;
}

static int tape_waveform_add_pulse(TapeWaveform* waveform, uint64_t duration) {
    if (!waveform || duration == 0) {
        return 1;
    }
    if (duration > UINT32_MAX) {
        duration = UINT32_MAX;
    }
    if (waveform->count == waveform->capacity) {
        size_t new_capacity = waveform->capacity ? waveform->capacity * 2 : 512;
        TapePulse* new_pulses = (TapePulse*)realloc(waveform->pulses, new_capacity * sizeof(TapePulse));
        if (!new_pulses) {
            return 0;
        }
        waveform->pulses = new_pulses;
        waveform->capacity = new_capacity;
    }
    waveform->pulses[waveform->count].duration = (uint32_t)duration;
    waveform->count++;
    return 1;
}

static int tape_generate_waveform_from_image(const TapeImage* image, TapeWaveform* waveform) {
    if (!image || !waveform) {
        return 0;
    }

    tape_waveform_reset(waveform);
    waveform->initial_level = 1;
    waveform->sample_rate = 0u;

    if (image->count == 0) {
        return 1;
    }

    uint64_t pending_silence = 0;
    for (size_t block_index = 0; block_index < image->count; ++block_index) {
        const TapeBlock* block = &image->blocks[block_index];
        int pilot_count = TAPE_DATA_PILOT_COUNT;
        if (block->length > 0 && block->data && block->data[0] == 0x00) {
            pilot_count = TAPE_HEADER_PILOT_COUNT;
        }

        for (int pulse = 0; pulse < pilot_count; ++pulse) {
            uint64_t duration = (uint64_t)TAPE_PILOT_PULSE_TSTATES;
            if (pending_silence) {
                duration += pending_silence;
                pending_silence = 0;
            }
            if (!tape_waveform_add_pulse(waveform, duration)) {
                return 0;
            }
        }

        uint64_t duration = (uint64_t)TAPE_SYNC_FIRST_PULSE_TSTATES;
        if (pending_silence) {
            duration += pending_silence;
            pending_silence = 0;
        }
        if (!tape_waveform_add_pulse(waveform, duration)) {
            return 0;
        }

        duration = (uint64_t)TAPE_SYNC_SECOND_PULSE_TSTATES;
        if (!tape_waveform_add_pulse(waveform, duration)) {
            return 0;
        }

        if (block->length > 0 && block->data) {
            for (uint32_t byte_index = 0; byte_index < block->length; ++byte_index) {
                uint8_t value = block->data[byte_index];
                uint8_t mask = 0x80u;
                for (int bit = 0; bit < 8; ++bit) {
                    int is_one = (value & mask) ? 1 : 0;
                    uint64_t pulse = (uint64_t)(is_one ? TAPE_BIT1_PULSE_TSTATES : TAPE_BIT0_PULSE_TSTATES);
                    if (pending_silence) {
                        pulse += pending_silence;
                        pending_silence = 0;
                    }
                    if (!tape_waveform_add_pulse(waveform, pulse)) {
                        return 0;
                    }
                    if (!tape_waveform_add_pulse(waveform, pulse)) {
                        return 0;
                    }
                    mask >>= 1;
                }
            }
        }

        pending_silence += tape_pause_to_tstates(block->pause_ms);
    }

    return 1;
}

static int tape_load_tap(const char* path, TapeImage* image) {
    FILE* tf = fopen(path, "rb");
    if (!tf) {
        fprintf(stderr, "Failed to open TAP file '%s': %s\n", path, strerror(errno));
        return 0;
    }

    uint8_t length_bytes[2];
    while (fread(length_bytes, sizeof(length_bytes), 1, tf) == 1) {
        uint32_t block_length = (uint32_t)length_bytes[0] | ((uint32_t)length_bytes[1] << 8);
        uint8_t* buffer = NULL;
        if (block_length > 0) {
            buffer = (uint8_t*)malloc(block_length);
            if (!buffer) {
                fprintf(stderr, "Out of memory while reading TAP block\n");
                fclose(tf);
                return 0;
            }
            if (fread(buffer, block_length, 1, tf) != 1) {
                fprintf(stderr, "Failed to read TAP block payload\n");
                free(buffer);
                fclose(tf);
                return 0;
            }
        }

        uint32_t pause_ms = 1000u;
        if (buffer) {
            if (!tape_image_add_block(image, buffer, block_length, pause_ms)) {
                fprintf(stderr, "Failed to store TAP block\n");
                free(buffer);
                fclose(tf);
                return 0;
            }
            free(buffer);
        } else {
            uint8_t empty = 0;
            if (!tape_image_add_block(image, &empty, 0u, pause_ms)) {
                fprintf(stderr, "Failed to store empty TAP block\n");
                fclose(tf);
                return 0;
            }
        }
    }

    if (!feof(tf)) {
        fprintf(stderr, "Failed to read TAP file '%s' completely\n", path);
        fclose(tf);
        return 0;
    }

    fclose(tf);
    if (tape_debug_logging) {
        tape_log("Loaded TAP '%s' with %zu blocks\n", path, image->count);
    }
    return 1;
}

static int tape_load_tzx(const char* path, TapeImage* image) {
    FILE* tf = fopen(path, "rb");
    if (!tf) {
        fprintf(stderr, "Failed to open TZX file '%s': %s\n", path, strerror(errno));
        return 0;
    }

    uint8_t header[10];
    if (fread(header, sizeof(header), 1, tf) != 1) {
        fprintf(stderr, "Failed to read TZX header from '%s'\n", path);
        fclose(tf);
        return 0;
    }

    if (memcmp(header, "ZXTape!\x1A", 8) != 0) {
        fprintf(stderr, "File '%s' is not a valid TZX image\n", path);
        fclose(tf);
        return 0;
    }

    int block_id = 0;
    while ((block_id = fgetc(tf)) != EOF) {
        if (block_id == 0x10) {
            uint8_t pause_bytes[2];
            uint8_t length_bytes[2];
            if (fread(pause_bytes, sizeof(pause_bytes), 1, tf) != 1 ||
                fread(length_bytes, sizeof(length_bytes), 1, tf) != 1) {
                fprintf(stderr, "Truncated TZX block\n");
                fclose(tf);
                return 0;
            }

            uint32_t pause_ms = (uint32_t)pause_bytes[0] | ((uint32_t)pause_bytes[1] << 8);
            uint32_t block_length = (uint32_t)length_bytes[0] | ((uint32_t)length_bytes[1] << 8);

            uint8_t* buffer = NULL;
            if (block_length > 0) {
                buffer = (uint8_t*)malloc(block_length);
                if (!buffer) {
                    fprintf(stderr, "Out of memory while reading TZX block\n");
                    fclose(tf);
                    return 0;
                }
                if (fread(buffer, block_length, 1, tf) != 1) {
                    fprintf(stderr, "Failed to read TZX block payload\n");
                    free(buffer);
                    fclose(tf);
                    return 0;
                }
            }

            if (buffer) {
                if (!tape_image_add_block(image, buffer, block_length, pause_ms)) {
                    fprintf(stderr, "Failed to store TZX block\n");
                    free(buffer);
                    fclose(tf);
                    return 0;
                }
                free(buffer);
            } else {
                uint8_t empty = 0;
                if (!tape_image_add_block(image, &empty, 0u, pause_ms)) {
                    fprintf(stderr, "Failed to store empty TZX block\n");
                    fclose(tf);
                    return 0;
                }
            }
        } else {
            fprintf(stderr, "Unsupported TZX block type 0x%02X in '%s'\n", block_id, path);
            fclose(tf);
            return 0;
        }
    }

    fclose(tf);
    return 1;
}

static int tape_create_blank_wav(const char* path, uint32_t sample_rate) {
    if (!path) {
        return 0;
    }

    if (sample_rate == 0u) {
        sample_rate = 44100u;
    }

    FILE* wf = fopen(path, "wb");
    if (!wf) {
        fprintf(stderr, "Failed to create WAV file '%s': %s\n", path, strerror(errno));
        return 0;
    }

    uint8_t header[44];
    memset(header, 0, sizeof(header));
    memcpy(header + 0, "RIFF", 4);
    uint32_t chunk_size = 36u;
    header[4] = (uint8_t)(chunk_size & 0xFFu);
    header[5] = (uint8_t)((chunk_size >> 8) & 0xFFu);
    header[6] = (uint8_t)((chunk_size >> 16) & 0xFFu);
    header[7] = (uint8_t)((chunk_size >> 24) & 0xFFu);
    memcpy(header + 8, "WAVE", 4);
    memcpy(header + 12, "fmt ", 4);
    header[16] = 16;
    header[20] = 1;
    header[22] = 1;
    header[24] = (uint8_t)(sample_rate & 0xFFu);
    header[25] = (uint8_t)((sample_rate >> 8) & 0xFFu);
    header[26] = (uint8_t)((sample_rate >> 16) & 0xFFu);
    header[27] = (uint8_t)((sample_rate >> 24) & 0xFFu);

    uint32_t byte_rate = sample_rate * sizeof(int16_t);
    header[28] = (uint8_t)(byte_rate & 0xFFu);
    header[29] = (uint8_t)((byte_rate >> 8) & 0xFFu);
    header[30] = (uint8_t)((byte_rate >> 16) & 0xFFu);
    header[31] = (uint8_t)((byte_rate >> 24) & 0xFFu);
    header[32] = (uint8_t)(sizeof(int16_t));
    header[34] = 16;
    memcpy(header + 36, "data", 4);

    if (fwrite(header, sizeof(header), 1, wf) != 1) {
        fprintf(stderr, "Failed to write WAV header to '%s'\n", path);
        fclose(wf);
        return 0;
    }

    if (fclose(wf) != 0) {
        fprintf(stderr, "Failed to finalize WAV file '%s': %s\n", path, strerror(errno));
        return 0;
    }

    return 1;
}

static int tape_load_wav(const char* path, TapePlaybackState* state) {
    if (!path || !state) {
        return 0;
    }

    FILE* wf = fopen(path, "rb");
    if (!wf) {
        if (errno != ENOENT) {
            fprintf(stderr, "Failed to open WAV file '%s': %s\n", path, strerror(errno));
            return 0;
        }

        uint32_t sample_rate = (audio_sample_rate > 0) ? (uint32_t)audio_sample_rate : 44100u;
        if (!tape_create_blank_wav(path, sample_rate)) {
            return 0;
        }
        printf("Created empty WAV tape %s\n", path);
        tape_free_image(&state->image);
        tape_waveform_reset(&state->waveform);
        state->waveform.sample_rate = sample_rate;
        state->format = TAPE_FORMAT_WAV;
        tape_wav_shared_position_tstates = 0;
        return 1;
    }

    uint8_t riff_header[12];
    if (fread(riff_header, sizeof(riff_header), 1, wf) != 1) {
        fprintf(stderr, "Failed to read WAV header from '%s'\n", path);
        fclose(wf);
        return 0;
    }

    if (memcmp(riff_header, "RIFF", 4) != 0 || memcmp(riff_header + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "File '%s' is not a valid WAV image\n", path);
        fclose(wf);
        return 0;
    }

    uint16_t audio_format = 0;
    uint16_t num_channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;
    uint8_t* data_buffer = NULL;
    uint32_t data_size = 0;
    int have_fmt = 0;
    int have_data = 0;

    for (;;) {
        uint8_t chunk_header[8];
        if (fread(chunk_header, sizeof(chunk_header), 1, wf) != 1) {
            break;
        }

        uint32_t chunk_size = (uint32_t)chunk_header[4] |
                               ((uint32_t)chunk_header[5] << 8) |
                               ((uint32_t)chunk_header[6] << 16) |
                               ((uint32_t)chunk_header[7] << 24);

        if (memcmp(chunk_header, "fmt ", 4) == 0) {
            if (chunk_size < 16) {
                fprintf(stderr, "Invalid WAV fmt chunk in '%s'\n", path);
                fclose(wf);
                free(data_buffer);
                return 0;
            }
            uint8_t* fmt_data = (uint8_t*)malloc(chunk_size);
            if (!fmt_data) {
                fprintf(stderr, "Out of memory while reading WAV fmt chunk\n");
                fclose(wf);
                free(data_buffer);
                return 0;
            }
            if (fread(fmt_data, chunk_size, 1, wf) != 1) {
                fprintf(stderr, "Failed to read WAV fmt chunk\n");
                free(fmt_data);
                fclose(wf);
                free(data_buffer);
                return 0;
            }
            audio_format = (uint16_t)fmt_data[0] | ((uint16_t)fmt_data[1] << 8);
            num_channels = (uint16_t)fmt_data[2] | ((uint16_t)fmt_data[3] << 8);
            sample_rate = (uint32_t)fmt_data[4] |
                          ((uint32_t)fmt_data[5] << 8) |
                          ((uint32_t)fmt_data[6] << 16) |
                          ((uint32_t)fmt_data[7] << 24);
            bits_per_sample = (uint16_t)fmt_data[14] | ((uint16_t)fmt_data[15] << 8);
            free(fmt_data);
            have_fmt = 1;
        } else if (memcmp(chunk_header, "data", 4) == 0) {
            if (chunk_size == 0) {
                free(data_buffer);
                data_buffer = NULL;
                data_size = 0;
                have_data = 1;
            } else {
                uint8_t* buffer = (uint8_t*)malloc(chunk_size);
                if (!buffer) {
                    fprintf(stderr, "Out of memory while reading WAV data chunk\n");
                    fclose(wf);
                    free(data_buffer);
                    return 0;
                }
                if (fread(buffer, chunk_size, 1, wf) != 1) {
                    fprintf(stderr, "Failed to read WAV data chunk\n");
                    free(buffer);
                    fclose(wf);
                    free(data_buffer);
                    return 0;
                }
                free(data_buffer);
                data_buffer = buffer;
                data_size = chunk_size;
                have_data = 1;
            }
        } else {
            if (fseek(wf, chunk_size, SEEK_CUR) != 0) {
                fprintf(stderr, "Failed to skip WAV chunk in '%s'\n", path);
                fclose(wf);
                free(data_buffer);
                return 0;
            }
        }

        if (chunk_size & 1u) {
            if (fseek(wf, 1, SEEK_CUR) != 0) {
                fprintf(stderr, "Failed to align WAV chunk in '%s'\n", path);
                fclose(wf);
                free(data_buffer);
                return 0;
            }
        }

        if (have_fmt && have_data) {
            break;
        }
    }

    fclose(wf);

    if (!have_fmt || !have_data) {
        fprintf(stderr, "WAV file '%s' is missing required chunks\n", path);
        free(data_buffer);
        return 0;
    }

    if (audio_format != 1) {
        fprintf(stderr, "WAV file '%s' is not PCM encoded\n", path);
        free(data_buffer);
        return 0;
    }

    if (num_channels != 1) {
        fprintf(stderr, "WAV file '%s' must be mono for tape loading\n", path);
        free(data_buffer);
        return 0;
    }

    if (bits_per_sample != 8 && bits_per_sample != 16) {
        fprintf(stderr, "Unsupported WAV bit depth (%u) in '%s'\n", (unsigned)bits_per_sample, path);
        free(data_buffer);
        return 0;
    }

    if (sample_rate == 0) {
        fprintf(stderr, "Invalid WAV sample rate in '%s'\n", path);
        free(data_buffer);
        return 0;
    }

    int bytes_per_sample = bits_per_sample / 8;
    if (bytes_per_sample <= 0) {
        free(data_buffer);
        return 0;
    }

    if (data_size % (uint32_t)bytes_per_sample != 0u) {
        fprintf(stderr, "Corrupt WAV data chunk in '%s'\n", path);
        free(data_buffer);
        return 0;
    }

    size_t total_samples = data_size / (size_t)bytes_per_sample;

    tape_free_image(&state->image);
    tape_waveform_reset(&state->waveform);
    state->waveform.sample_rate = sample_rate;
    state->format = TAPE_FORMAT_WAV;

    if (total_samples == 0) {
        free(data_buffer);
        return 1;
    }

    size_t offset = 0;
    int32_t first_value = 0;
    if (bits_per_sample == 16) {
        first_value = (int16_t)((uint16_t)data_buffer[offset] | ((uint16_t)data_buffer[offset + 1] << 8));
    } else {
        first_value = (int32_t)data_buffer[offset] - 128;
    }
    int previous_level = (first_value >= 0) ? 1 : 0;
    state->waveform.initial_level = previous_level;
    size_t run_length = 1;
    offset += (size_t)bytes_per_sample;

    double tstates_per_sample = CPU_CLOCK_HZ / (double)sample_rate;

    for (size_t sample_index = 1; sample_index < total_samples; ++sample_index) {
        int32_t sample_value = 0;
        if (bits_per_sample == 16) {
            sample_value = (int16_t)((uint16_t)data_buffer[offset] | ((uint16_t)data_buffer[offset + 1] << 8));
        } else {
            sample_value = (int32_t)data_buffer[offset] - 128;
        }
        int level = (sample_value >= 0) ? 1 : 0;

        if (level == previous_level) {
            run_length++;
        } else {
            uint64_t duration = (uint64_t)(tstates_per_sample * (double)run_length + 0.5);
            if (duration == 0 && run_length > 0) {
                duration = 1;
            }
            if (!tape_waveform_add_pulse(&state->waveform, duration)) {
                fprintf(stderr, "Out of memory while decoding WAV tape\n");
                free(data_buffer);
                tape_waveform_reset(&state->waveform);
                state->format = TAPE_FORMAT_NONE;
                return 0;
            }
            previous_level = level;
            run_length = 1;
        }

        offset += (size_t)bytes_per_sample;
    }

    free(data_buffer);
    if (state == &tape_playback) {
        tape_wav_shared_position_tstates = 0;
    }
    return 1;
}

static int string_ends_with_case_insensitive(const char* str, const char* suffix) {
    if (!str || !suffix) {
        return 0;
    }

    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);
    if (suffix_len == 0 || suffix_len > str_len) {
        return 0;
    }

    const char* str_tail = str + (str_len - suffix_len);
    for (size_t i = 0; i < suffix_len; ++i) {
        char a = str_tail[i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b) {
            return 0;
        }
    }

    return 1;
}

static TapeFormat tape_format_from_extension(const char* path) {
    if (!path) {
        return TAPE_FORMAT_NONE;
    }

    if (string_ends_with_case_insensitive(path, ".tap")) {
        return TAPE_FORMAT_TAP;
    }
    if (string_ends_with_case_insensitive(path, ".tzx")) {
        return TAPE_FORMAT_TZX;
    }
    if (string_ends_with_case_insensitive(path, ".wav")) {
        return TAPE_FORMAT_WAV;
    }

    return TAPE_FORMAT_NONE;
}

static int tape_load_image(const char* path, TapeFormat format, TapeImage* image) {
    if (!path || !image || format == TAPE_FORMAT_NONE) {
        return 0;
    }

    tape_free_image(image);

    switch (format) {
        case TAPE_FORMAT_TAP:
            return tape_load_tap(path, image);
        case TAPE_FORMAT_TZX:
            return tape_load_tzx(path, image);
        default:
            break;
    }

    return 0;
}

static void tape_reset_playback(TapePlaybackState* state) {
    if (!state) {
        return;
    }
    state->current_block = 0;
    state->phase = TAPE_PHASE_IDLE;
    state->pilot_pulses_remaining = 0;
    state->data_byte_index = 0;
    state->data_bit_mask = 0x80u;
    state->data_pulse_half = 0;
    state->next_transition_tstate = 0;
    state->pause_end_tstate = 0;
    state->playing = 0;
    state->waveform_index = 0;
    state->paused_transition_remaining = 0;
    state->paused_pause_remaining = 0;
    state->position_tstates = 0;
    state->position_start_tstate = 0;
    state->last_transition_tstate = 0;
    if (state == &tape_playback && state->format == TAPE_FORMAT_WAV) {
        tape_wav_shared_position_tstates = 0;
    }
    if ((state->format == TAPE_FORMAT_WAV) ||
        (state->use_waveform_playback && state->waveform.count > 0)) {
        state->level = state->waveform.initial_level ? 1 : 0;
        tape_ear_state = state->level;
    } else {
        state->level = 1;
        tape_ear_state = 1;
    }
}

static int tape_current_block_pilot_count(const TapePlaybackState* state) {
    if (!state || state->current_block >= state->image.count) {
        return TAPE_DATA_PILOT_COUNT;
    }
    const TapeBlock* block = &state->image.blocks[state->current_block];
    if (block->length > 0 && block->data && block->data[0] == 0x00) {
        return TAPE_HEADER_PILOT_COUNT;
    }
    return TAPE_DATA_PILOT_COUNT;
}

static int tape_begin_block(TapePlaybackState* state, size_t block_index, uint64_t start_time) {
    if (!state || block_index >= state->image.count) {
        return 0;
    }

    state->current_block = block_index;
    if (tape_debug_logging) {
        tape_log("Starting playback of block %zu at t=%llu\n",
                 block_index,
                 (unsigned long long)start_time);
        tape_log_block_summary(&state->image.blocks[block_index], block_index);
    }
    state->pilot_pulses_remaining = tape_current_block_pilot_count(state);
    state->data_byte_index = 0;
    state->data_bit_mask = 0x80u;
    state->data_pulse_half = 0;
    state->phase = TAPE_PHASE_PILOT;
    state->level = 1;
    tape_ear_state = state->level;
    state->next_transition_tstate = start_time + (uint64_t)TAPE_PILOT_PULSE_TSTATES;
    state->playing = 1;
    state->last_transition_tstate = start_time;
    state->paused_transition_remaining = 0;
    state->paused_pause_remaining = 0;
    return 1;
}

static void tape_start_playback(TapePlaybackState* state, uint64_t start_time) {
    if (!state) {
        return;
    }
    tape_reset_playback(state);
    state->position_start_tstate = start_time;
    state->last_transition_tstate = start_time;
    int use_waveform = (state->format == TAPE_FORMAT_WAV) ||
                       (state->use_waveform_playback && state->waveform.count > 0);
    if (use_waveform) {
        if (state->waveform.count == 0) {
            return;
        }
        state->waveform_index = 0;
        state->level = state->waveform.initial_level ? 1 : 0;
        tape_ear_state = state->level;
        state->playing = 1;
        state->next_transition_tstate = start_time + (uint64_t)state->waveform.pulses[0].duration;
        state->paused_transition_remaining = 0;
        state->paused_pause_remaining = 0;
        return;
    }
    if (state->image.count == 0) {
        return;
    }
    (void)tape_begin_block(state, 0u, start_time);
}

static void tape_pause_playback(TapePlaybackState* state, uint64_t current_t_state) {
    if (!state || !state->playing) {
        return;
    }

    if (state->next_transition_tstate > current_t_state) {
        state->paused_transition_remaining = state->next_transition_tstate - current_t_state;
    } else {
        state->paused_transition_remaining = 0;
    }

    if (state->phase == TAPE_PHASE_PAUSE && state->pause_end_tstate > current_t_state) {
        state->paused_pause_remaining = state->pause_end_tstate - current_t_state;
    } else {
        state->paused_pause_remaining = 0;
    }

    tape_playback_accumulate_elapsed(state, current_t_state);
    state->last_transition_tstate = current_t_state;
    state->playing = 0;
    int use_waveform = (state->format == TAPE_FORMAT_WAV) ||
                       (state->use_waveform_playback && state->waveform.count > 0);
    if (state == &tape_playback && use_waveform) {
        tape_wav_shared_position_tstates = state->position_tstates;
    }
}

static int tape_resume_playback(TapePlaybackState* state, uint64_t current_t_state) {
    if (!state || state->playing) {
        return 0;
    }

    int use_waveform = (state->format == TAPE_FORMAT_WAV) ||
                       (state->use_waveform_playback && state->waveform.count > 0);
    if (use_waveform) {
        if (state->waveform.count == 0 || state->waveform_index >= state->waveform.count) {
            return 0;
        }
        uint64_t delay = state->paused_transition_remaining;
        state->next_transition_tstate = current_t_state + delay;
        if (delay == 0 && state->next_transition_tstate < current_t_state) {
            state->next_transition_tstate = current_t_state;
        }
        state->playing = 1;
    } else {
        if (state->phase == TAPE_PHASE_IDLE) {
            tape_start_playback(state, current_t_state);
            return state->playing;
        }
        if (state->phase == TAPE_PHASE_DONE) {
            return 0;
        }
        uint64_t delay = state->paused_transition_remaining;
        state->next_transition_tstate = current_t_state + delay;
        if (delay == 0 && state->next_transition_tstate < current_t_state) {
            state->next_transition_tstate = current_t_state;
        }
        if (state->phase == TAPE_PHASE_PAUSE) {
            uint64_t pause_delay = state->paused_pause_remaining;
            state->pause_end_tstate = current_t_state + pause_delay;
            if (pause_delay == 0 && state->pause_end_tstate < current_t_state) {
                state->pause_end_tstate = current_t_state;
            }
        }
        state->playing = 1;
    }

    if (state->playing) {
        state->position_start_tstate = current_t_state;
        state->last_transition_tstate = current_t_state;
    }

    state->paused_transition_remaining = 0;
    state->paused_pause_remaining = 0;
    tape_ear_state = state->level;
    return 1;
}

static void tape_rewind_playback(TapePlaybackState* state) {
    if (!state) {
        return;
    }
    tape_reset_playback(state);
}

static void tape_wav_seek_playback(TapePlaybackState* state, uint64_t position_tstates) {
    if (!state || state->format != TAPE_FORMAT_WAV) {
        return;
    }

    state->playing = 0;
    state->paused_transition_remaining = 0;
    state->paused_pause_remaining = 0;
    state->waveform_index = 0;
    state->next_transition_tstate = 0;
    state->last_transition_tstate = 0;
    state->position_tstates = 0;
    state->position_start_tstate = 0;

    int initial_level = state->waveform.initial_level ? 1 : 0;
    state->level = initial_level;
    tape_ear_state = state->level;

    if (state->waveform.count == 0) {
        tape_wav_shared_position_tstates = 0;
        return;
    }

    uint64_t total_duration = 0;
    for (size_t i = 0; i < state->waveform.count; ++i) {
        total_duration += (uint64_t)state->waveform.pulses[i].duration;
    }

    uint64_t target = position_tstates;
    if (target > total_duration) {
        target = total_duration;
    }

    uint64_t accumulated = 0;
    size_t index = 0;
    while (index < state->waveform.count) {
        uint64_t duration = (uint64_t)state->waveform.pulses[index].duration;
        if (duration == 0) {
            index++;
            continue;
        }
        if (target < accumulated + duration) {
            break;
        }
        accumulated += duration;
        index++;
    }

    state->waveform_index = index;
    if ((index & 1u) != 0u) {
        state->level = initial_level ? 0 : 1;
        tape_ear_state = state->level;
    }

    state->position_tstates = target;
    state->position_start_tstate = target;
    state->last_transition_tstate = target;
    tape_wav_shared_position_tstates = target;

    if (index < state->waveform.count) {
        uint64_t duration = (uint64_t)state->waveform.pulses[index].duration;
        uint64_t consumed = target - accumulated;
        if (consumed > duration) {
            consumed = duration;
        }
        uint64_t remaining = duration - consumed;
        if (remaining == 0) {
            state->waveform_index = index + 1;
            state->level = state->level ? 0 : 1;
            tape_ear_state = state->level;
            if (state->waveform_index < state->waveform.count) {
                state->paused_transition_remaining = (uint64_t)state->waveform.pulses[state->waveform_index].duration;
            } else {
                state->paused_transition_remaining = 0;
            }
        } else {
            state->paused_transition_remaining = remaining;
        }
    } else {
        state->paused_transition_remaining = 0;
    }
}

static void tape_playback_accumulate_elapsed(TapePlaybackState* state, uint64_t stop_t_state) {
    if (!state) {
        return;
    }

    if (stop_t_state < state->position_start_tstate) {
        stop_t_state = state->position_start_tstate;
    }

    if (stop_t_state > state->position_start_tstate) {
        state->position_tstates += stop_t_state - state->position_start_tstate;
    }

    state->position_start_tstate = stop_t_state;
}

static uint64_t tape_playback_elapsed_tstates(const TapePlaybackState* state, uint64_t current_t_state) {
    if (!state) {
        return 0;
    }

    uint64_t elapsed = state->position_tstates;
    if (state->playing && current_t_state > state->position_start_tstate) {
        elapsed += current_t_state - state->position_start_tstate;
    }

    return elapsed;
}

static uint64_t tape_recorder_elapsed_tstates(uint64_t current_t_state) {
    if (!tape_recorder.enabled) {
        return 0;
    }

    uint64_t elapsed = tape_recorder.position_tstates;
    if (tape_recorder.recording && current_t_state > tape_recorder.position_start_tstate) {
        elapsed += current_t_state - tape_recorder.position_start_tstate;
    }

    return elapsed;
}

static const TapeOverlayGlyph* tape_overlay_find_glyph(char ch) {
    size_t glyph_count = sizeof(tape_overlay_font) / sizeof(tape_overlay_font[0]);
    for (size_t i = 0; i < glyph_count; ++i) {
        if (tape_overlay_font[i].ch == ch) {
            return &tape_overlay_font[i];
        }
    }
    return &tape_overlay_font[0];
}

static int tape_overlay_text_width(const char* text, int scale, int spacing) {
    if (!text) {
        return 0;
    }

    int width = 0;
    int first = 1;
    for (const char* c = text; *c; ++c) {
        if (!first) {
            width += spacing;
        }
        first = 0;
        width += TAPE_OVERLAY_FONT_WIDTH * scale;
    }

    return width;
}

static void tape_overlay_draw_text(int origin_x, int origin_y, const char* text, int scale, int spacing, uint32_t color) {
    if (!text) {
        return;
    }

    int cursor_x = origin_x;
    for (const char* c = text; *c; ++c) {
        char ch = *c;
        if (ch >= 'a' && ch <= 'z') {
            ch = (char)(ch - 'a' + 'A');
        }
        const TapeOverlayGlyph* glyph = tape_overlay_find_glyph(ch);
        for (int row = 0; row < TAPE_OVERLAY_FONT_HEIGHT; ++row) {
            uint8_t bits = glyph->rows[row];
            for (int col = 0; col < TAPE_OVERLAY_FONT_WIDTH; ++col) {
                int bit_index = TAPE_OVERLAY_FONT_WIDTH - 1 - col;
                if (bits & (1u << bit_index)) {
                    for (int dy = 0; dy < scale; ++dy) {
                        int py = origin_y + row * scale + dy;
                        if (py < 0 || py >= TOTAL_HEIGHT) {
                            continue;
                        }
                        for (int dx = 0; dx < scale; ++dx) {
                            int px = cursor_x + col * scale + dx;
                            if (px < 0 || px >= TOTAL_WIDTH) {
                                continue;
                            }
                            pixels[py * TOTAL_WIDTH + px] = color;
                        }
                    }
                }
            }
        }
        cursor_x += TAPE_OVERLAY_FONT_WIDTH * scale + spacing;
    }
}

static const TapeControlIcon* tape_control_find_icon(TapeControlAction action) {
    size_t icon_count = sizeof(tape_control_icons) / sizeof(tape_control_icons[0]);
    for (size_t i = 0; i < icon_count; ++i) {
        if (tape_control_icons[i].action == action) {
            return &tape_control_icons[i];
        }
    }
    return NULL;
}

static TapeControlAction tape_control_action_from_status(TapeDeckStatus status) {
    switch (status) {
        case TAPE_DECK_STATUS_PLAY:
            return TAPE_CONTROL_ACTION_PLAY;
        case TAPE_DECK_STATUS_STOP:
            return TAPE_CONTROL_ACTION_STOP;
        case TAPE_DECK_STATUS_REWIND:
            return TAPE_CONTROL_ACTION_REWIND;
        case TAPE_DECK_STATUS_RECORD:
            return TAPE_CONTROL_ACTION_RECORD;
        case TAPE_DECK_STATUS_IDLE:
        default:
            break;
    }
    return TAPE_CONTROL_ACTION_NONE;
}

static void tape_overlay_draw_icon(int origin_x, int origin_y, const TapeControlIcon* icon, int scale, uint32_t color) {
    if (!icon) {
        return;
    }

    for (int row = 0; row < TAPE_CONTROL_ICON_HEIGHT; ++row) {
        uint8_t bits = icon->rows[row];
        for (int col = 0; col < TAPE_CONTROL_ICON_WIDTH; ++col) {
            int bit_index = TAPE_CONTROL_ICON_WIDTH - 1 - col;
            if (bits & (1u << bit_index)) {
                for (int dy = 0; dy < scale; ++dy) {
                    int py = origin_y + row * scale + dy;
                    if (py < 0 || py >= TOTAL_HEIGHT) {
                        continue;
                    }
                    for (int dx = 0; dx < scale; ++dx) {
                        int px = origin_x + col * scale + dx;
                        if (px < 0 || px >= TOTAL_WIDTH) {
                            continue;
                        }
                        pixels[py * TOTAL_WIDTH + px] = color;
                    }
                }
            }
        }
    }
}

static void tape_overlay_draw_control_button(int x,
                                             int y,
                                             int size,
                                             int scale,
                                             TapeControlAction action,
                                             int enabled,
                                             int highlight) {
    if (tape_control_button_count >= TAPE_CONTROL_BUTTON_MAX) {
        return;
    }

    uint32_t border_color = 0xFFFFFFFFu;
    uint32_t background_color = enabled ? 0x383838FFu : 0x2A2A2AFFu;
    uint32_t icon_color = enabled ? 0xFFFFFFFFu : 0x7F7F7FFFu;

    if (action == TAPE_CONTROL_ACTION_RECORD) {
        icon_color = enabled ? 0xFF4444FFu : 0x803030FFu;
    }

    if (highlight && enabled) {
        if (action == TAPE_CONTROL_ACTION_RECORD) {
            background_color = 0x7F1E1EFFu;
        } else {
            background_color = 0x2E6F3FFFu;
        }
    }

    for (int yy = 0; yy < size; ++yy) {
        int py = y + yy;
        if (py < 0 || py >= TOTAL_HEIGHT) {
            continue;
        }
        for (int xx = 0; xx < size; ++xx) {
            int px = x + xx;
            if (px < 0 || px >= TOTAL_WIDTH) {
                continue;
            }
            int is_border = (yy == 0 || yy == size - 1 || xx == 0 || xx == size - 1);
            pixels[py * TOTAL_WIDTH + px] = is_border ? border_color : background_color;
        }
    }

    int icon_pixel_width = TAPE_CONTROL_ICON_WIDTH * scale;
    int icon_pixel_height = TAPE_CONTROL_ICON_HEIGHT * scale;
    int icon_origin_x = x + (size - icon_pixel_width) / 2;
    int icon_origin_y = y + (size - icon_pixel_height) / 2;
    const TapeControlIcon* icon = tape_control_find_icon(action);
    tape_overlay_draw_icon(icon_origin_x, icon_origin_y, icon, scale, icon_color);

    TapeControlButton* button = &tape_control_buttons[tape_control_button_count++];
    button->action = action;
    button->rect.x = x;
    button->rect.y = y;
    button->rect.w = size;
    button->rect.h = size;
    button->enabled = enabled ? 1 : 0;
    button->visible = 1;
}

static void tape_render_overlay(void) {
    for (int i = 0; i < TAPE_CONTROL_BUTTON_MAX; ++i) {
        tape_control_buttons[i].action = TAPE_CONTROL_ACTION_NONE;
        tape_control_buttons[i].rect.x = 0;
        tape_control_buttons[i].rect.y = 0;
        tape_control_buttons[i].rect.w = 0;
        tape_control_buttons[i].rect.h = 0;
        tape_control_buttons[i].enabled = 0;
        tape_control_buttons[i].visible = 0;
    }
    tape_control_button_count = 0;

    if (!tape_input_enabled && !tape_recorder.enabled) {
        return;
    }

    const char* mode_text = "STOP";
    int mode_is_record = 0;
    int use_recorder_time = 0;

    if (tape_recorder.recording) {
        mode_text = "REC";
        mode_is_record = 1;
        use_recorder_time = 1;
    } else if (tape_playback.playing) {
        mode_text = "PLAY";
    } else {
        switch (tape_deck_status) {
            case TAPE_DECK_STATUS_PLAY:
                mode_text = "PLAY";
                break;
            case TAPE_DECK_STATUS_REWIND:
                mode_text = "REW";
                break;
            case TAPE_DECK_STATUS_RECORD:
                mode_text = "REC";
                mode_is_record = 1;
                use_recorder_time = 1;
                break;
            case TAPE_DECK_STATUS_STOP:
            case TAPE_DECK_STATUS_IDLE:
            default:
                mode_text = "STOP";
                break;
        }
    }

    if (!use_recorder_time && !tape_input_enabled && tape_recorder.enabled) {
        use_recorder_time = 1;
    }

    uint64_t elapsed_tstates = 0;
    int shared_wav = (tape_input_format == TAPE_FORMAT_WAV) ||
                     (tape_recorder.output_format == TAPE_OUTPUT_WAV);
    if (shared_wav) {
        if (tape_recorder.recording) {
            elapsed_tstates = tape_recorder_elapsed_tstates(total_t_states);
        } else if (tape_playback.playing) {
            elapsed_tstates = tape_playback_elapsed_tstates(&tape_playback, total_t_states);
        } else if (tape_recorder.enabled && tape_recorder.output_format == TAPE_OUTPUT_WAV) {
            elapsed_tstates = tape_recorder.position_tstates;
        } else {
            elapsed_tstates = tape_wav_shared_position_tstates;
        }
    } else if (use_recorder_time) {
        elapsed_tstates = tape_recorder_elapsed_tstates(total_t_states);
    } else if (tape_input_enabled) {
        elapsed_tstates = tape_playback_elapsed_tstates(&tape_playback, total_t_states);
    }

    uint64_t clock_hz = (uint64_t)(CPU_CLOCK_HZ + 0.5);
    if (clock_hz == 0) {
        clock_hz = 1;
    }
    uint64_t total_tenths = (elapsed_tstates * 10ull + clock_hz / 2ull) / clock_hz;
    uint64_t minutes = total_tenths / 600ull;
    uint64_t seconds = (total_tenths / 10ull) % 60ull;
    uint64_t tenths = total_tenths % 10ull;
    if (minutes > 99ull) {
        minutes = 99ull;
    }

    char counter_text[16];
    (void)snprintf(counter_text, sizeof(counter_text), "%02llu:%02llu.%1llu",
                   (unsigned long long)minutes,
                   (unsigned long long)seconds,
                   (unsigned long long)tenths);

    int scale = 2;
    int spacing = scale;
    int padding = 6;
    int line_height = TAPE_OVERLAY_FONT_HEIGHT * scale;
    int line_gap = scale;
    int status_width = tape_overlay_text_width(mode_text, scale, spacing);
    int counter_width = tape_overlay_text_width(counter_text, scale, spacing);

    int button_size = line_height;
    int button_gap = scale;
    int button_area_width = 0;
    int button_count = 0;
    int record_available = (tape_recorder.enabled ||
                            (tape_input_format == TAPE_FORMAT_WAV && tape_input_path)) ? 1 : 0;
    int show_play = tape_input_enabled ? 1 : 0;
    int show_stop = (tape_input_enabled || tape_recorder.enabled) ? 1 : 0;
    int show_rewind = tape_input_enabled ? 1 : 0;
    int show_record = record_available ? 1 : 0;

    if (show_play) {
        if (button_count > 0) {
            button_area_width += button_gap;
        }
        button_area_width += button_size;
        ++button_count;
    }
    if (show_stop) {
        if (button_count > 0) {
            button_area_width += button_gap;
        }
        button_area_width += button_size;
        ++button_count;
    }
    if (show_rewind) {
        if (button_count > 0) {
            button_area_width += button_gap;
        }
        button_area_width += button_size;
        ++button_count;
    }
    if (show_record) {
        if (button_count > 0) {
            button_area_width += button_gap;
        }
        button_area_width += button_size;
        ++button_count;
    }

    int counter_button_spacing = button_count > 0 ? scale * 2 : 0;
    int counter_row_width = counter_width + counter_button_spacing + button_area_width;
    int content_width = status_width > counter_row_width ? status_width : counter_row_width;
    int panel_width = content_width + padding * 2;
    int panel_height = line_height * 2 + padding * 2 + line_gap;

    int origin_x = TOTAL_WIDTH - panel_width - 6;
    if (origin_x < 0) {
        origin_x = 0;
    }
    int origin_y = 3;
    if (origin_y + panel_height > BORDER_SIZE) {
        origin_y = BORDER_SIZE - panel_height;
        if (origin_y < 0) {
            origin_y = 0;
        }
    }

    uint32_t background_color = 0x202020FFu;
    uint32_t border_color = 0xFFFFFFFFu;
    uint32_t status_color = mode_is_record ? 0xFF5555FFu : 0xFFFFFFFFu;
    uint32_t counter_color = 0xFFFFFFFFu;

    for (int y = 0; y < panel_height; ++y) {
        int py = origin_y + y;
        if (py < 0 || py >= TOTAL_HEIGHT) {
            continue;
        }
        for (int x = 0; x < panel_width; ++x) {
            int px = origin_x + x;
            if (px < 0 || px >= TOTAL_WIDTH) {
                continue;
            }
            int is_border = (y == 0 || y == panel_height - 1 || x == 0 || x == panel_width - 1);
            pixels[py * TOTAL_WIDTH + px] = is_border ? border_color : background_color;
        }
    }

    int text_x = origin_x + padding;
    int status_y = origin_y + padding;
    int counter_y = status_y + line_height + line_gap;

    tape_overlay_draw_text(text_x, status_y, mode_text, scale, spacing, status_color);
    tape_overlay_draw_text(text_x, counter_y, counter_text, scale, spacing, counter_color);

    if (button_count > 0) {
        int button_x = text_x + counter_width + counter_button_spacing;
        int button_y = counter_y;
        TapeControlAction highlight_action = tape_control_action_from_status(tape_deck_status);

        if (show_play) {
            tape_overlay_draw_control_button(button_x,
                                             button_y,
                                             button_size,
                                             scale,
                                             TAPE_CONTROL_ACTION_PLAY,
                                             tape_input_enabled,
                                             highlight_action == TAPE_CONTROL_ACTION_PLAY);
            button_x += button_size + button_gap;
        }
        if (show_stop) {
            tape_overlay_draw_control_button(button_x,
                                             button_y,
                                             button_size,
                                             scale,
                                             TAPE_CONTROL_ACTION_STOP,
                                             show_stop,
                                             highlight_action == TAPE_CONTROL_ACTION_STOP);
            button_x += button_size + button_gap;
        }
        if (show_rewind) {
            tape_overlay_draw_control_button(button_x,
                                             button_y,
                                             button_size,
                                             scale,
                                             TAPE_CONTROL_ACTION_REWIND,
                                             tape_input_enabled,
                                             highlight_action == TAPE_CONTROL_ACTION_REWIND);
            button_x += button_size + button_gap;
        }
        if (show_record) {
            tape_overlay_draw_control_button(button_x,
                                             button_y,
                                             button_size,
                                             scale,
                                             TAPE_CONTROL_ACTION_RECORD,
                                             record_available,
                                             highlight_action == TAPE_CONTROL_ACTION_RECORD);
            button_x += button_size + button_gap;
        }
    }
}

static int tape_duration_tolerance(int reference) {
    int tolerance = reference / 4;
    if (tolerance < 200) {
        tolerance = 200;
    }
    return tolerance;
}

static int tape_duration_matches(uint32_t duration, int reference, int tolerance) {
    int diff = (int)duration - reference;
    if (diff < 0) {
        diff = -diff;
    }
    return diff <= tolerance;
}

static void tape_finish_block_playback(TapePlaybackState* state) {
    if (!state) {
        return;
    }
    if (state->current_block < state->image.count) {
        const TapeBlock* block = &state->image.blocks[state->current_block];
        if (tape_debug_logging) {
            tape_log("Finished playback of block %zu (length=%u pause=%u)\n",
                     state->current_block,
                     block ? block->length : 0u,
                     block ? block->pause_ms : 0u);
        }
        uint64_t pause = tape_pause_to_tstates(block->pause_ms);
        state->phase = TAPE_PHASE_PAUSE;
        state->pause_end_tstate = state->next_transition_tstate + pause;
        state->current_block++;
        state->data_bit_mask = 0x80u;
        if (pause == 0) {
            uint64_t start_time = state->pause_end_tstate;
            if (state->current_block < state->image.count) {
                if (!tape_begin_block(state, state->current_block, start_time)) {
                    state->phase = TAPE_PHASE_DONE;
                    state->playing = 0;
                    tape_ear_state = 1;
                }
            } else {
                state->phase = TAPE_PHASE_DONE;
                state->playing = 0;
                tape_ear_state = 1;
            }
        }
    } else {
        if (tape_debug_logging) {
            tape_log("Playback complete after block %zu\n", state->current_block);
        }
        state->phase = TAPE_PHASE_DONE;
        state->playing = 0;
        tape_ear_state = 1;
    }
}

static int tape_bit_index_from_mask(uint8_t mask) {
    for (int bit = 0; bit < 8; ++bit) {
        if ((mask >> bit) & 1u) {
            return bit;
        }
    }
    return 0;
}

static int tape_current_data_bit(const TapePlaybackState* state, const TapeBlock* block) {
    if (!state || !block || state->data_byte_index >= block->length) {
        return 0;
    }
    return (block->data[state->data_byte_index] & state->data_bit_mask) ? 1 : 0;
}

static void tape_update(uint64_t current_t_state) {
    TapePlaybackState* state = &tape_playback;
    if (!tape_input_enabled || !state->playing) {
        return;
    }

    int use_waveform = (state->format == TAPE_FORMAT_WAV) ||
                       (state->use_waveform_playback && state->waveform.count > 0);
    if (use_waveform) {
        while (state->playing && state->waveform_index < state->waveform.count &&
               current_t_state >= state->next_transition_tstate) {
            uint64_t transition_time = state->next_transition_tstate;
            if (transition_time < state->last_transition_tstate) {
                transition_time = state->last_transition_tstate;
            }
            state->level = state->level ? 0 : 1;
            tape_ear_state = state->level;
            state->waveform_index++;
            state->last_transition_tstate = transition_time;
            if (state->waveform_index < state->waveform.count) {
                uint64_t duration = (uint64_t)state->waveform.pulses[state->waveform_index].duration;
                state->next_transition_tstate = transition_time + duration;
            } else {
                state->playing = 0;
                tape_playback_accumulate_elapsed(state, transition_time);
                if (state == &tape_playback && state->format == TAPE_FORMAT_WAV) {
                    tape_wav_shared_position_tstates = state->position_tstates;
                }
                tape_deck_status = TAPE_DECK_STATUS_STOP;
                break;
            }
        }
        return;
    }

    while (state->playing) {
        if (state->phase == TAPE_PHASE_PAUSE) {
            if (current_t_state >= state->pause_end_tstate) {
                if (state->current_block >= state->image.count) {
                    state->phase = TAPE_PHASE_DONE;
                    state->playing = 0;
                    tape_ear_state = 1;
                    tape_playback_accumulate_elapsed(state, state->pause_end_tstate);
                    state->last_transition_tstate = state->pause_end_tstate;
                    if (state == &tape_playback && state->format == TAPE_FORMAT_WAV) {
                        tape_wav_shared_position_tstates = state->position_tstates;
                    }
                    tape_deck_status = TAPE_DECK_STATUS_STOP;
                    break;
                }
                if (!tape_begin_block(state, state->current_block, state->pause_end_tstate)) {
                    state->phase = TAPE_PHASE_DONE;
                    state->playing = 0;
                    tape_ear_state = 1;
                    tape_playback_accumulate_elapsed(state, state->pause_end_tstate);
                    state->last_transition_tstate = state->pause_end_tstate;
                    if (state == &tape_playback && state->format == TAPE_FORMAT_WAV) {
                        tape_wav_shared_position_tstates = state->position_tstates;
                    }
                    tape_deck_status = TAPE_DECK_STATUS_STOP;
                    break;
                }
                continue;
            }
            break;
        }

        if (state->phase == TAPE_PHASE_DONE || state->phase == TAPE_PHASE_IDLE) {
            break;
        }

        if (current_t_state < state->next_transition_tstate) {
            break;
        }

        uint64_t transition_time = state->next_transition_tstate;
        if (transition_time < state->last_transition_tstate) {
            transition_time = state->last_transition_tstate;
        }

        state->level = state->level ? 0 : 1;
        tape_ear_state = state->level;
        state->last_transition_tstate = transition_time;

        switch (state->phase) {
            case TAPE_PHASE_PILOT:
                state->pilot_pulses_remaining--;
                if (state->pilot_pulses_remaining > 0) {
                    state->next_transition_tstate = transition_time + (uint64_t)TAPE_PILOT_PULSE_TSTATES;
                } else {
                    state->phase = TAPE_PHASE_SYNC1;
                    state->next_transition_tstate = transition_time + (uint64_t)TAPE_SYNC_FIRST_PULSE_TSTATES;
                }
                break;
            case TAPE_PHASE_SYNC1:
                state->phase = TAPE_PHASE_SYNC2;
                state->next_transition_tstate = transition_time + (uint64_t)TAPE_SYNC_SECOND_PULSE_TSTATES;
                break;
            case TAPE_PHASE_SYNC2: {
                state->phase = TAPE_PHASE_DATA;
                state->data_pulse_half = 0;
                const TapeBlock* block = NULL;
                if (state->current_block < state->image.count) {
                    block = &state->image.blocks[state->current_block];
                }
                if (!block || block->length == 0 || !block->data) {
                    tape_finish_block_playback(state);
                } else {
                    int bit = tape_current_data_bit(state, block);
                    int duration = bit ? TAPE_BIT1_PULSE_TSTATES : TAPE_BIT0_PULSE_TSTATES;
                    if (tape_debug_logging && state->data_byte_index < block->length) {
                        int bit_index = tape_bit_index_from_mask(state->data_bit_mask);
                        uint8_t byte_value = block->data[state->data_byte_index];
                        tape_log("Block %zu byte %zu bit[%d]=%d (value=0x%02X mask=0x%02X)\n",
                                 state->current_block,
                                 state->data_byte_index,
                                 bit_index,
                                 bit,
                                 byte_value,
                                 state->data_bit_mask);
                    }
                    state->next_transition_tstate = transition_time + (uint64_t)duration;
                    state->data_pulse_half = 1;
                }
                break;
            }
            case TAPE_PHASE_DATA: {
                const TapeBlock* block = NULL;
                if (state->current_block < state->image.count) {
                    block = &state->image.blocks[state->current_block];
                }
                if (!block || block->length == 0 || !block->data) {
                    tape_finish_block_playback(state);
                    break;
                }

                int bit = tape_current_data_bit(state, block);
                int duration = bit ? TAPE_BIT1_PULSE_TSTATES : TAPE_BIT0_PULSE_TSTATES;
                if (tape_debug_logging && state->data_pulse_half == 0 &&
                    state->data_byte_index < block->length) {
                    int bit_index = tape_bit_index_from_mask(state->data_bit_mask);
                    uint8_t byte_value = block->data[state->data_byte_index];
                    tape_log("Block %zu byte %zu bit[%d]=%d (value=0x%02X mask=0x%02X)\n",
                             state->current_block,
                             state->data_byte_index,
                             bit_index,
                             bit,
                             byte_value,
                             state->data_bit_mask);
                }
                state->next_transition_tstate = transition_time + (uint64_t)duration;

                if (state->data_pulse_half == 0) {
                    state->data_pulse_half = 1;
                } else {
                    state->data_pulse_half = 0;
                    state->data_bit_mask >>= 1;
                    if (state->data_bit_mask == 0) {
                        state->data_bit_mask = 0x80u;
                        state->data_byte_index++;
                        if (state->data_byte_index >= block->length) {
                            tape_finish_block_playback(state);
                        }
                    }
                }
                break;
            }
            default:
                break;
        }

        if (!state->playing) {
            uint64_t stop_time = state->pause_end_tstate;
            if (stop_time < transition_time) {
                stop_time = state->next_transition_tstate;
            }
            if (stop_time < transition_time) {
                stop_time = transition_time;
            }
            tape_playback_accumulate_elapsed(state, stop_time);
            state->last_transition_tstate = stop_time;
            if (state == &tape_playback && state->format == TAPE_FORMAT_WAV) {
                tape_wav_shared_position_tstates = state->position_tstates;
            }
            tape_deck_status = TAPE_DECK_STATUS_STOP;
            break;
        }
    }
}

static void tape_recorder_reset_pulses(void) {
    if (tape_recorder.pulses) {
        free(tape_recorder.pulses);
        tape_recorder.pulses = NULL;
    }
    tape_recorder.pulse_count = 0;
    tape_recorder.pulse_capacity = 0;
}

static void tape_recorder_reset_audio(void) {
    if (tape_recorder.audio_samples) {
        free(tape_recorder.audio_samples);
        tape_recorder.audio_samples = NULL;
    }
    tape_recorder.audio_sample_count = 0;
    tape_recorder.audio_sample_capacity = 0;
}

static void tape_recorder_reset_wav_prefix(void) {
    if (tape_recorder.wav_prefix_samples) {
        free(tape_recorder.wav_prefix_samples);
        tape_recorder.wav_prefix_samples = NULL;
    }
    tape_recorder.wav_prefix_sample_count = 0;
    tape_recorder.wav_existing_samples = 0;
    tape_recorder.wav_head_samples = 0;
    tape_recorder.wav_requires_truncate = 0;
}

static int tape_recorder_prepare_append_wav(uint32_t* data_chunk_offset,
                                            uint32_t* existing_bytes,
                                            uint32_t* sample_rate_out) {
    if (!tape_recorder.output_path) {
        return 0;
    }

    FILE* wf = fopen(tape_recorder.output_path, "rb");
    if (!wf) {
        fprintf(stderr,
                "Tape RECORD append failed: unable to open '%s': %s\n",
                tape_recorder.output_path,
                strerror(errno));
        return 0;
    }

    uint8_t riff_header[12];
    if (fread(riff_header, sizeof(riff_header), 1, wf) != 1) {
        fprintf(stderr,
                "Tape RECORD append failed: '%s' is not a valid WAV file\n",
                tape_recorder.output_path);
        fclose(wf);
        return 0;
    }

    if (memcmp(riff_header, "RIFF", 4) != 0 || memcmp(riff_header + 8, "WAVE", 4) != 0) {
        fprintf(stderr,
                "Tape RECORD append failed: '%s' is not a valid WAV file\n",
                tape_recorder.output_path);
        fclose(wf);
        return 0;
    }

    uint16_t audio_format = 0;
    uint16_t num_channels = 0;
    uint16_t bits_per_sample = 0;
    uint32_t sample_rate = 0;
    uint32_t data_offset = 0;
    uint32_t data_size = 0;
    int have_fmt = 0;
    int have_data = 0;

    for (;;) {
        uint8_t chunk_header[8];
        if (fread(chunk_header, sizeof(chunk_header), 1, wf) != 1) {
            break;
        }

        long chunk_start = ftell(wf);
        if (chunk_start < 0) {
            fclose(wf);
            return 0;
        }
        chunk_start -= 8;
        if (chunk_start < 0) {
            fclose(wf);
            return 0;
        }
        uint32_t chunk_size = (uint32_t)chunk_header[4] |
                               ((uint32_t)chunk_header[5] << 8) |
                               ((uint32_t)chunk_header[6] << 16) |
                               ((uint32_t)chunk_header[7] << 24);

        if (memcmp(chunk_header, "fmt ", 4) == 0) {
            if (chunk_size < 16) {
                fprintf(stderr,
                        "Tape RECORD append failed: '%s' has an invalid WAV fmt chunk\n",
                        tape_recorder.output_path);
                fclose(wf);
                return 0;
            }
            uint8_t* fmt_data = (uint8_t*)malloc(chunk_size);
            if (!fmt_data) {
                fprintf(stderr, "Out of memory while preparing WAV append\n");
                fclose(wf);
                return 0;
            }
            if (fread(fmt_data, chunk_size, 1, wf) != 1) {
                fprintf(stderr,
                        "Tape RECORD append failed: unable to read fmt chunk from '%s'\n",
                        tape_recorder.output_path);
                free(fmt_data);
                fclose(wf);
                return 0;
            }
            audio_format = (uint16_t)fmt_data[0] | ((uint16_t)fmt_data[1] << 8);
            num_channels = (uint16_t)fmt_data[2] | ((uint16_t)fmt_data[3] << 8);
            sample_rate = (uint32_t)fmt_data[4] |
                          ((uint32_t)fmt_data[5] << 8) |
                          ((uint32_t)fmt_data[6] << 16) |
                          ((uint32_t)fmt_data[7] << 24);
            bits_per_sample = (uint16_t)fmt_data[14] | ((uint16_t)fmt_data[15] << 8);
            free(fmt_data);
            have_fmt = 1;
        } else if (memcmp(chunk_header, "data", 4) == 0) {
            if (chunk_start > (long)UINT32_MAX) {
                fclose(wf);
                return 0;
            }
            data_offset = (uint32_t)chunk_start;
            data_size = chunk_size;
            if (fseek(wf, chunk_size, SEEK_CUR) != 0) {
                fclose(wf);
                return 0;
            }
            have_data = 1;
        } else {
            if (fseek(wf, chunk_size, SEEK_CUR) != 0) {
                fclose(wf);
                return 0;
            }
        }

        if (chunk_size & 1u) {
            if (fseek(wf, 1, SEEK_CUR) != 0) {
                fclose(wf);
                return 0;
            }
        }

        if (have_fmt && have_data) {
            break;
        }
    }

    fclose(wf);

    if (!have_fmt || !have_data) {
        fprintf(stderr,
                "Tape RECORD append failed: '%s' is missing WAV metadata\n",
                tape_recorder.output_path);
        return 0;
    }

    if (audio_format != 1 || num_channels != 1 || bits_per_sample != 16) {
        fprintf(stderr,
                "Tape RECORD append failed: '%s' must be 16-bit mono PCM\n",
                tape_recorder.output_path);
        return 0;
    }

    if (sample_rate == 0) {
        fprintf(stderr,
                "Tape RECORD append failed: '%s' reports an invalid sample rate\n",
                tape_recorder.output_path);
        return 0;
    }

    if ((data_size & 1u) != 0u) {
        fprintf(stderr,
                "Tape RECORD append failed: '%s' contains incomplete 16-bit samples\n",
                tape_recorder.output_path);
        return 0;
    }

    if (data_chunk_offset) {
        *data_chunk_offset = data_offset;
    }
    if (existing_bytes) {
        *existing_bytes = data_size;
    }
    if (sample_rate_out) {
        *sample_rate_out = sample_rate;
    }

    return 1;
}

static int tape_recorder_prepare_wav_session(uint64_t head_tstates) {
    tape_recorder.wav_existing_samples = 0;
    tape_recorder.wav_head_samples = 0;
    tape_recorder.wav_requires_truncate = 0;

    if (!tape_recorder.output_path) {
        return 0;
    }

    FILE* wf = fopen(tape_recorder.output_path, "rb");
    if (!wf) {
        if (errno != ENOENT) {
            fprintf(stderr,
                    "Tape RECORD failed: unable to open '%s': %s\n",
                    tape_recorder.output_path,
                    strerror(errno));
            return 0;
        }

        uint32_t sample_rate = tape_recorder.sample_rate;
        if (sample_rate == 0u) {
            sample_rate = (audio_sample_rate > 0) ? (uint32_t)audio_sample_rate : 44100u;
        }
        if (sample_rate == 0u) {
            sample_rate = 44100u;
        }
        tape_recorder.sample_rate = sample_rate;
        return 1;
    }

    uint8_t riff_header[12];
    if (fread(riff_header, sizeof(riff_header), 1, wf) != 1) {
        fprintf(stderr,
                "Tape RECORD failed: '%s' is not a valid WAV file\n",
                tape_recorder.output_path);
        fclose(wf);
        return 0;
    }

    if (memcmp(riff_header, "RIFF", 4) != 0 || memcmp(riff_header + 8, "WAVE", 4) != 0) {
        fprintf(stderr,
                "Tape RECORD failed: '%s' is not a valid WAV file\n",
                tape_recorder.output_path);
        fclose(wf);
        return 0;
    }

    uint16_t audio_format = 0;
    uint16_t num_channels = 0;
    uint16_t bits_per_sample = 0;
    uint32_t sample_rate = 0;
    uint32_t data_offset = 0;
    uint32_t data_size = 0;
    int have_fmt = 0;
    int have_data = 0;

    for (;;) {
        uint8_t chunk_header[8];
        if (fread(chunk_header, sizeof(chunk_header), 1, wf) != 1) {
            break;
        }

        uint32_t chunk_size = (uint32_t)chunk_header[4] |
                               ((uint32_t)chunk_header[5] << 8) |
                               ((uint32_t)chunk_header[6] << 16) |
                               ((uint32_t)chunk_header[7] << 24);

        if (memcmp(chunk_header, "fmt ", 4) == 0) {
            if (chunk_size < 16) {
                fprintf(stderr,
                        "Tape RECORD failed: invalid WAV fmt chunk in '%s'\n",
                        tape_recorder.output_path);
                fclose(wf);
                return 0;
            }
            uint8_t* fmt_data = (uint8_t*)malloc(chunk_size);
            if (!fmt_data) {
                fprintf(stderr, "Tape RECORD failed: out of memory\n");
                fclose(wf);
                return 0;
            }
            if (fread(fmt_data, chunk_size, 1, wf) != 1) {
                fprintf(stderr, "Tape RECORD failed: unable to read WAV fmt chunk\n");
                free(fmt_data);
                fclose(wf);
                return 0;
            }
            audio_format = (uint16_t)fmt_data[0] | ((uint16_t)fmt_data[1] << 8);
            num_channels = (uint16_t)fmt_data[2] | ((uint16_t)fmt_data[3] << 8);
            sample_rate = (uint32_t)fmt_data[4] |
                          ((uint32_t)fmt_data[5] << 8) |
                          ((uint32_t)fmt_data[6] << 16) |
                          ((uint32_t)fmt_data[7] << 24);
            bits_per_sample = (uint16_t)fmt_data[14] | ((uint16_t)fmt_data[15] << 8);
            free(fmt_data);
            have_fmt = 1;
        } else if (memcmp(chunk_header, "data", 4) == 0) {
            data_offset = (uint32_t)(ftell(wf) - 8l);
            data_size = chunk_size;
            if (chunk_size > 0) {
                if (fseek(wf, chunk_size, SEEK_CUR) != 0) {
                    fclose(wf);
                    return 0;
                }
            }
            have_data = 1;
        } else {
            if (fseek(wf, chunk_size, SEEK_CUR) != 0) {
                fclose(wf);
                return 0;
            }
        }

        if (chunk_size & 1u) {
            if (fseek(wf, 1, SEEK_CUR) != 0) {
                fclose(wf);
                return 0;
            }
        }

        if (have_fmt && have_data) {
            break;
        }
    }

    if (!have_fmt || !have_data) {
        fprintf(stderr,
                "Tape RECORD failed: '%s' is missing required WAV chunks\n",
                tape_recorder.output_path);
        fclose(wf);
        return 0;
    }

    if (audio_format != 1u) {
        fprintf(stderr,
                "Tape RECORD failed: '%s' is not PCM encoded\n",
                tape_recorder.output_path);
        fclose(wf);
        return 0;
    }

    if (num_channels != 1u) {
        fprintf(stderr,
                "Tape RECORD failed: '%s' must be mono\n",
                tape_recorder.output_path);
        fclose(wf);
        return 0;
    }

    uint16_t bytes_per_sample = (uint16_t)(bits_per_sample / 8u);
    if (bytes_per_sample == 0u) {
        fclose(wf);
        return 0;
    }

    if (data_size % bytes_per_sample != 0u) {
        fprintf(stderr,
                "Tape RECORD failed: '%s' contains incomplete samples\n",
                tape_recorder.output_path);
        fclose(wf);
        return 0;
    }

    uint64_t existing_samples = data_size / bytes_per_sample;
    tape_recorder.wav_existing_samples = existing_samples;

    tape_recorder.sample_rate = sample_rate ? sample_rate : tape_recorder.sample_rate;
    if (tape_recorder.sample_rate == 0u) {
        tape_recorder.sample_rate = 44100u;
    }

    uint64_t requested_samples = tape_recorder_samples_from_tstates(head_tstates);
    if (requested_samples > existing_samples) {
        requested_samples = existing_samples;
    }

    tape_recorder.wav_head_samples = requested_samples;
    tape_recorder.wav_prefix_sample_count = (size_t)requested_samples;
    tape_recorder.wav_requires_truncate = (requested_samples < existing_samples);

    if (tape_recorder.wav_prefix_sample_count > 0) {
        if (fseek(wf, (long)data_offset + 8l, SEEK_SET) != 0) {
            fclose(wf);
            return 0;
        }

        size_t prefix_samples = tape_recorder.wav_prefix_sample_count;
        size_t prefix_bytes = prefix_samples * sizeof(int16_t);
        tape_recorder.wav_prefix_samples = (int16_t*)malloc(prefix_bytes);
        if (!tape_recorder.wav_prefix_samples) {
            fprintf(stderr, "Tape RECORD failed: out of memory\n");
            fclose(wf);
            return 0;
        }

        if (bytes_per_sample == sizeof(int16_t)) {
            if (fread(tape_recorder.wav_prefix_samples, sizeof(int16_t), prefix_samples, wf) != prefix_samples) {
                fprintf(stderr, "Tape RECORD failed: unable to read WAV data\n");
                free(tape_recorder.wav_prefix_samples);
                tape_recorder.wav_prefix_samples = NULL;
                tape_recorder.wav_prefix_sample_count = 0;
                fclose(wf);
                return 0;
            }
        } else {
            uint8_t* temp = (uint8_t*)malloc(prefix_samples);
            if (!temp) {
                fprintf(stderr, "Tape RECORD failed: out of memory\n");
                free(tape_recorder.wav_prefix_samples);
                tape_recorder.wav_prefix_samples = NULL;
                tape_recorder.wav_prefix_sample_count = 0;
                fclose(wf);
                return 0;
            }
            if (fread(temp, 1, prefix_samples, wf) != prefix_samples) {
                fprintf(stderr, "Tape RECORD failed: unable to read WAV data\n");
                free(temp);
                free(tape_recorder.wav_prefix_samples);
                tape_recorder.wav_prefix_samples = NULL;
                tape_recorder.wav_prefix_sample_count = 0;
                fclose(wf);
                return 0;
            }
            for (size_t i = 0; i < prefix_samples; ++i) {
                int16_t converted = (int16_t)(((int32_t)temp[i] - 128) << 8);
                tape_recorder.wav_prefix_samples[i] = converted;
            }
            free(temp);
        }
    }

    fclose(wf);
    return 1;
}

static void tape_recorder_enable(const char* path, TapeOutputFormat format) {
    tape_recorder.output_path = path;
    tape_recorder.enabled = path ? 1 : 0;
    tape_recorder.output_format = format;
    tape_recorder.block_active = 0;
    tape_recorder.last_transition_tstate = 0;
    tape_recorder.last_level = -1;
    tape_recorder.block_start_level = 0;
    tape_recorder.sample_rate = (audio_sample_rate > 0) ? (uint32_t)audio_sample_rate : 44100u;
    tape_recorder.recording = 0;
    tape_recorder.session_dirty = 0;
    tape_recorder.position_tstates = 0;
    tape_recorder.position_start_tstate = 0;
    tape_recorder.append_mode = 0;
    tape_recorder.append_data_chunk_offset = 0;
    tape_recorder.append_existing_data_bytes = 0;
    tape_recorder_reset_pulses();
    tape_free_image(&tape_recorder.recorded);
    tape_recorder_reset_audio();
    tape_recorder_reset_wav_prefix();
}

static int tape_recorder_start_session(uint64_t current_t_state, int append_mode) {
    if (!tape_recorder.enabled) {
        fprintf(stderr, "Tape RECORD ignored (no output configured)\n");
        return 0;
    }

    if (tape_recorder.recording) {
        printf("Tape recorder already active\n");
        return 0;
    }

    int use_append = (append_mode && tape_recorder.output_format == TAPE_OUTPUT_WAV) ? 1 : 0;
    tape_recorder_reset_pulses();
    tape_free_image(&tape_recorder.recorded);
    tape_recorder_reset_audio();
    tape_recorder_reset_wav_prefix();
    tape_recorder.session_dirty = 0;
    tape_recorder.append_mode = use_append;

    if (tape_recorder.output_format == TAPE_OUTPUT_WAV) {
        uint64_t head_tstates = tape_wav_shared_position_tstates;
        if (use_append) {
            uint32_t data_offset = 0;
            uint32_t existing_bytes = 0;
            uint32_t sample_rate = tape_recorder.sample_rate;
            if (!tape_recorder_prepare_append_wav(&data_offset, &existing_bytes, &sample_rate)) {
                tape_recorder.append_mode = 0;
                return 0;
            }
            tape_recorder.append_data_chunk_offset = data_offset;
            tape_recorder.append_existing_data_bytes = existing_bytes;
            tape_recorder.sample_rate = sample_rate;
            uint64_t existing_samples = existing_bytes / sizeof(int16_t);
            tape_recorder.position_tstates = tape_recorder_tstates_from_samples(existing_samples);
            tape_wav_shared_position_tstates = tape_recorder.position_tstates;
        } else {
            tape_recorder.append_data_chunk_offset = 0;
            tape_recorder.append_existing_data_bytes = 0;
            tape_recorder.position_tstates = head_tstates;
            if (!tape_recorder_prepare_wav_session(head_tstates)) {
                return 0;
            }
            if (tape_recorder.wav_requires_truncate) {
                tape_recorder.session_dirty = 1;
            }
        }
    } else {
        if (use_append) {
            printf("Tape RECORD append is only supported for WAV outputs; starting new capture\n");
            tape_recorder.append_mode = 0;
        }
        tape_recorder.append_data_chunk_offset = 0;
        tape_recorder.append_existing_data_bytes = 0;
        tape_recorder.position_tstates = 0;
        if (tape_recorder.output_path) {
            (void)remove(tape_recorder.output_path);
        }
    }

    tape_recorder.recording = 1;
    tape_recorder.block_active = 0;
    tape_recorder.last_transition_tstate = current_t_state;
    tape_recorder.last_level = -1;
    tape_recorder.block_start_level = 0;
    tape_recorder.position_start_tstate = current_t_state;
    printf("Tape RECORD%s\n", use_append ? " (append)" : "");
    return 1;
}

static void tape_recorder_stop_session(uint64_t current_t_state, int finalize_output) {
    if (!tape_recorder.enabled) {
        return;
    }

    if (tape_recorder.recording) {
        tape_recorder_update(current_t_state, 1);
        tape_recorder.recording = 0;
        tape_recorder.block_active = 0;
        tape_recorder.last_transition_tstate = current_t_state;
        tape_recorder.last_level = -1;
        if (current_t_state > tape_recorder.position_start_tstate) {
            tape_recorder.position_tstates += current_t_state - tape_recorder.position_start_tstate;
        }
        tape_recorder.position_start_tstate = current_t_state;
    }

    if (finalize_output && tape_recorder.session_dirty) {
        if (!tape_recorder_write_output()) {
            fprintf(stderr, "Failed to save tape recording\n");
        }
    }

    if (tape_recorder.output_format == TAPE_OUTPUT_WAV) {
        tape_wav_shared_position_tstates = tape_recorder.position_tstates;
        if (tape_input_format == TAPE_FORMAT_WAV &&
            tape_input_path &&
            tape_recorder.output_path &&
            strcmp(tape_input_path, tape_recorder.output_path) == 0) {
            if (!tape_load_wav(tape_input_path, &tape_playback)) {
                tape_waveform_reset(&tape_playback.waveform);
                tape_input_enabled = 0;
            } else {
                tape_reset_playback(&tape_playback);
                tape_wav_seek_playback(&tape_playback, tape_wav_shared_position_tstates);
                tape_input_enabled = 1;
            }
        }
    }

    tape_recorder_reset_wav_prefix();
}

static int tape_recorder_append_pulse(uint64_t duration) {
    if (duration == 0) {
        return 1;
    }
    if (duration > UINT32_MAX) {
        duration = UINT32_MAX;
    }
    if (tape_recorder.pulse_count == tape_recorder.pulse_capacity) {
        size_t new_capacity = tape_recorder.pulse_capacity ? tape_recorder.pulse_capacity * 2 : 512;
        TapePulse* new_pulses = (TapePulse*)realloc(tape_recorder.pulses, new_capacity * sizeof(TapePulse));
        if (!new_pulses) {
            return 0;
        }
        tape_recorder.pulses = new_pulses;
        tape_recorder.pulse_capacity = new_capacity;
    }
    tape_recorder.pulses[tape_recorder.pulse_count].duration = (uint32_t)duration;
    tape_recorder.pulse_count++;
    tape_recorder.session_dirty = 1;
    return 1;
}

static size_t tape_recorder_samples_from_tstates(uint64_t duration) {
    if (duration == 0 || tape_recorder.sample_rate == 0) {
        return 0;
    }
    double seconds = (double)duration / CPU_CLOCK_HZ;
    double samples = seconds * (double)tape_recorder.sample_rate;
    if (samples <= 0.0) {
        return 0;
    }
    if (samples >= (double)SIZE_MAX) {
        return SIZE_MAX;
    }
    size_t count = (size_t)(samples + 0.5);
    if (count == 0 && samples > 0.0) {
        count = 1;
    }
    return count;
}

static uint64_t tape_recorder_tstates_from_samples(uint64_t sample_count) {
    uint32_t sample_rate = tape_recorder.sample_rate ? tape_recorder.sample_rate : 44100u;
    if (sample_rate == 0 || sample_count == 0) {
        return 0;
    }

    double seconds = (double)sample_count / (double)sample_rate;
    double tstates = seconds * CPU_CLOCK_HZ;
    if (tstates <= 0.0) {
        return 0;
    }

    if (tstates >= (double)UINT64_MAX) {
        return UINT64_MAX;
    }

    uint64_t rounded = (uint64_t)(tstates + 0.5);
    if (rounded == 0 && tstates > 0.0) {
        rounded = 1;
    }

    return rounded;
}

static int tape_recorder_append_audio_samples(int level, size_t sample_count) {
    if (sample_count == 0) {
        return 1;
    }
    if (tape_recorder.audio_sample_count > SIZE_MAX - sample_count) {
        return 0;
    }
    size_t required = tape_recorder.audio_sample_count + sample_count;
    if (required > tape_recorder.audio_sample_capacity) {
        size_t new_capacity = tape_recorder.audio_sample_capacity ? tape_recorder.audio_sample_capacity : 4096;
        while (new_capacity < required) {
            new_capacity *= 2;
        }
        int16_t* new_samples = (int16_t*)realloc(tape_recorder.audio_samples, new_capacity * sizeof(int16_t));
        if (!new_samples) {
            return 0;
        }
        tape_recorder.audio_samples = new_samples;
        tape_recorder.audio_sample_capacity = new_capacity;
    }

    int16_t value = level ? TAPE_WAV_AMPLITUDE : (int16_t)(-TAPE_WAV_AMPLITUDE);
    int16_t* dest = tape_recorder.audio_samples + tape_recorder.audio_sample_count;
    for (size_t i = 0; i < sample_count; ++i) {
        dest[i] = value;
    }
    tape_recorder.audio_sample_count += sample_count;
    if (sample_count > 0) {
        tape_recorder.session_dirty = 1;
    }
    return 1;
}

static void tape_recorder_append_block_audio(uint64_t idle_cycles) {
    if (tape_recorder.output_format != TAPE_OUTPUT_WAV) {
        return;
    }

    if (tape_recorder.pulse_count == 0) {
        if (idle_cycles > 0 && tape_recorder.last_level >= 0) {
            size_t idle_samples = tape_recorder_samples_from_tstates(idle_cycles);
            if (!tape_recorder_append_audio_samples(tape_recorder.last_level ? 1 : 0, idle_samples)) {
                fprintf(stderr, "Warning: failed to store recorded tape audio\n");
            }
        }
        return;
    }

    int level = tape_recorder.block_start_level ? 1 : 0;
    for (size_t i = 0; i < tape_recorder.pulse_count; ++i) {
        uint32_t duration = tape_recorder.pulses[i].duration;
        size_t samples = tape_recorder_samples_from_tstates(duration);
        if (!tape_recorder_append_audio_samples(level, samples)) {
            fprintf(stderr, "Warning: failed to store recorded tape audio\n");
            return;
        }
        level = level ? 0 : 1;
    }

    if (idle_cycles > 0 && tape_recorder.last_level >= 0) {
        size_t idle_samples = tape_recorder_samples_from_tstates(idle_cycles);
        if (!tape_recorder_append_audio_samples(tape_recorder.last_level ? 1 : 0, idle_samples)) {
            fprintf(stderr, "Warning: failed to store recorded tape audio\n");
        }
    }
}

static int tape_recorder_write_wav(void) {
    if (!tape_recorder.output_path) {
        return 1;
    }

    uint32_t sample_rate = tape_recorder.sample_rate ? tape_recorder.sample_rate : 44100u;
    size_t sample_count = tape_recorder.audio_sample_count;
    size_t prefix_samples = tape_recorder.wav_prefix_sample_count;
    if (tape_recorder.append_mode) {
        if (sample_count == 0) {
            tape_recorder.session_dirty = 0;
            return 1;
        }

        uint64_t append_bytes = (uint64_t)sample_count * sizeof(int16_t);
        if (append_bytes > UINT32_MAX) {
            fprintf(stderr, "Recorded audio exceeds WAV size limits\n");
            return 0;
        }

        uint32_t data_offset = tape_recorder.append_data_chunk_offset;
        uint32_t existing_bytes = tape_recorder.append_existing_data_bytes;
        uint64_t total_bytes = (uint64_t)existing_bytes + append_bytes;
        if (total_bytes > UINT32_MAX) {
            fprintf(stderr, "Recorded audio exceeds WAV size limits\n");
            return 0;
        }

        FILE* tf = fopen(tape_recorder.output_path, "rb+");
        if (!tf) {
            fprintf(stderr,
                    "Failed to open tape output '%s': %s\n",
                    tape_recorder.output_path,
                    strerror(errno));
            return 0;
        }

        if (fseek(tf, 0, SEEK_END) != 0) {
            fclose(tf);
            return 0;
        }

        if (sample_count > 0) {
            if (fwrite(tape_recorder.audio_samples, sizeof(int16_t), sample_count, tf) != sample_count) {
                fprintf(stderr, "Failed to append WAV data\n");
                fclose(tf);
                return 0;
            }
        }

        long final_pos = ftell(tf);
        if (final_pos < 0) {
            fclose(tf);
            return 0;
        }

        uint64_t chunk_size_64 = (uint64_t)final_pos - 8u;
        if (chunk_size_64 > UINT32_MAX) {
            fprintf(stderr, "Recorded audio exceeds WAV size limits\n");
            fclose(tf);
            return 0;
        }
        uint32_t chunk_size = (uint32_t)chunk_size_64;
        uint32_t data_bytes = (uint32_t)total_bytes;

        if (fseek(tf, 4, SEEK_SET) != 0) {
            fclose(tf);
            return 0;
        }
        uint8_t chunk_size_bytes[4];
        chunk_size_bytes[0] = (uint8_t)(chunk_size & 0xFFu);
        chunk_size_bytes[1] = (uint8_t)((chunk_size >> 8) & 0xFFu);
        chunk_size_bytes[2] = (uint8_t)((chunk_size >> 16) & 0xFFu);
        chunk_size_bytes[3] = (uint8_t)((chunk_size >> 24) & 0xFFu);
        if (fwrite(chunk_size_bytes, sizeof(chunk_size_bytes), 1, tf) != 1) {
            fprintf(stderr, "Failed to update WAV header\n");
            fclose(tf);
            return 0;
        }

        if (data_offset > (uint32_t)LONG_MAX - 4u) {
            fclose(tf);
            return 0;
        }
        long data_size_pos = (long)data_offset + 4l;
        if (data_size_pos < 0 || fseek(tf, data_size_pos, SEEK_SET) != 0) {
            fclose(tf);
            return 0;
        }
        uint8_t data_bytes_array[4];
        data_bytes_array[0] = (uint8_t)(data_bytes & 0xFFu);
        data_bytes_array[1] = (uint8_t)((data_bytes >> 8) & 0xFFu);
        data_bytes_array[2] = (uint8_t)((data_bytes >> 16) & 0xFFu);
        data_bytes_array[3] = (uint8_t)((data_bytes >> 24) & 0xFFu);
        if (fwrite(data_bytes_array, sizeof(data_bytes_array), 1, tf) != 1) {
            fprintf(stderr, "Failed to update WAV header\n");
            fclose(tf);
            return 0;
        }

        if (fclose(tf) != 0) {
            fprintf(stderr,
                    "Failed to finalize tape output '%s': %s\n",
                    tape_recorder.output_path,
                    strerror(errno));
            return 0;
        }

        tape_recorder.append_existing_data_bytes = data_bytes;
        tape_recorder.session_dirty = 0;
        printf("Tape recording saved to %s\n", tape_recorder.output_path);
        return 1;
    }

    uint64_t total_samples = (uint64_t)prefix_samples + (uint64_t)sample_count;
    uint64_t data_bytes_64 = total_samples * sizeof(int16_t);
    if (data_bytes_64 > UINT32_MAX) {
        fprintf(stderr, "Recorded audio exceeds WAV size limits\n");
        return 0;
    }
    uint32_t data_bytes = (uint32_t)data_bytes_64;
    uint32_t chunk_size = 36u + data_bytes;
    if (chunk_size < data_bytes) {
        fprintf(stderr, "Recorded audio exceeds WAV size limits\n");
        return 0;
    }

    FILE* tf = fopen(tape_recorder.output_path, "wb");
    if (!tf) {
        fprintf(stderr, "Failed to open tape output '%s': %s\n", tape_recorder.output_path, strerror(errno));
        return 0;
    }

    uint8_t header[44];
    memset(header, 0, sizeof(header));
    memcpy(header + 0, "RIFF", 4);
    header[4] = (uint8_t)(chunk_size & 0xFFu);
    header[5] = (uint8_t)((chunk_size >> 8) & 0xFFu);
    header[6] = (uint8_t)((chunk_size >> 16) & 0xFFu);
    header[7] = (uint8_t)((chunk_size >> 24) & 0xFFu);
    memcpy(header + 8, "WAVE", 4);
    memcpy(header + 12, "fmt ", 4);
    header[16] = 16;
    header[20] = 1;
    header[22] = 1;
    header[24] = (uint8_t)(sample_rate & 0xFFu);
    header[25] = (uint8_t)((sample_rate >> 8) & 0xFFu);
    header[26] = (uint8_t)((sample_rate >> 16) & 0xFFu);
    header[27] = (uint8_t)((sample_rate >> 24) & 0xFFu);
    uint32_t byte_rate = sample_rate * 2u;
    header[28] = (uint8_t)(byte_rate & 0xFFu);
    header[29] = (uint8_t)((byte_rate >> 8) & 0xFFu);
    header[30] = (uint8_t)((byte_rate >> 16) & 0xFFu);
    header[31] = (uint8_t)((byte_rate >> 24) & 0xFFu);
    header[32] = 2;
    header[34] = 16;
    memcpy(header + 36, "data", 4);
    header[40] = (uint8_t)(data_bytes & 0xFFu);
    header[41] = (uint8_t)((data_bytes >> 8) & 0xFFu);
    header[42] = (uint8_t)((data_bytes >> 16) & 0xFFu);
    header[43] = (uint8_t)((data_bytes >> 24) & 0xFFu);

    if (fwrite(header, sizeof(header), 1, tf) != 1) {
        fprintf(stderr, "Failed to write WAV header\n");
        fclose(tf);
        return 0;
    }

    if (prefix_samples > 0) {
        if (!tape_recorder.wav_prefix_samples) {
            fprintf(stderr, "Failed to access recorded WAV prefix data\n");
            fclose(tf);
            return 0;
        }
        if (fwrite(tape_recorder.wav_prefix_samples, sizeof(int16_t), prefix_samples, tf) != prefix_samples) {
            fprintf(stderr, "Failed to write WAV data\n");
            fclose(tf);
            return 0;
        }
    }

    if (sample_count > 0) {
        if (fwrite(tape_recorder.audio_samples, sizeof(int16_t), sample_count, tf) != sample_count) {
            fprintf(stderr, "Failed to write WAV data\n");
            fclose(tf);
            return 0;
        }
    }

    if (fclose(tf) != 0) {
        fprintf(stderr, "Failed to finalize tape output '%s': %s\n", tape_recorder.output_path, strerror(errno));
        return 0;
    }

    tape_recorder.session_dirty = 0;
    tape_recorder.wav_prefix_sample_count = prefix_samples + sample_count;
    tape_recorder.wav_existing_samples = tape_recorder.wav_prefix_sample_count;
    tape_recorder.wav_head_samples = tape_recorder.wav_existing_samples;
    printf("Tape recording saved to %s\n", tape_recorder.output_path);

    return 1;
}

static int tape_decode_pulses_to_block(const TapePulse* pulses, size_t count, uint32_t pause_ms, TapeBlock* out_block) {
    if (!pulses || count == 0 || !out_block) {
        return 0;
    }

    size_t index = 0;
    size_t pilot_count = 0;
    size_t search_index = 0;
    size_t pilot_start = 0;
    const int pilot_tolerance = tape_duration_tolerance(TAPE_PILOT_PULSE_TSTATES);
    while (search_index < count) {
        if (!tape_duration_matches(pulses[search_index].duration, TAPE_PILOT_PULSE_TSTATES, pilot_tolerance)) {
            ++search_index;
            continue;
        }

        size_t run_start = search_index;
        while (search_index < count && tape_duration_matches(pulses[search_index].duration, TAPE_PILOT_PULSE_TSTATES, pilot_tolerance)) {
            ++search_index;
        }

        pilot_count = search_index - run_start;
        if (pilot_count >= 100) {
            pilot_start = run_start;
            index = search_index;
            break;
        }
    }

    if (pilot_count < 100) {
        return 0;
    }

    if (index + 1 >= count) {
        return 0;
    }

    double scale = 1.0;
    if (pilot_count > 0) {
        size_t sample_count = pilot_count;
        if (sample_count > 4096) {
            sample_count = 4096;
        }
        if (sample_count == 0) {
            sample_count = 1;
        }
        uint64_t pilot_sum = 0;
        for (size_t i = 0; i < sample_count; ++i) {
            pilot_sum += pulses[pilot_start + i].duration;
        }
        double pilot_average = (double)pilot_sum / (double)sample_count;
        if (pilot_average > 0.0) {
            scale = pilot_average / (double)TAPE_PILOT_PULSE_TSTATES;
            if (scale < 0.5) {
                scale = 0.5;
            } else if (scale > 2.0) {
                scale = 2.0;
            }
        }
    }

    int sync1_reference = (int)((double)TAPE_SYNC_FIRST_PULSE_TSTATES * scale + 0.5);
    int sync2_reference = (int)((double)TAPE_SYNC_SECOND_PULSE_TSTATES * scale + 0.5);
    if (sync1_reference <= 0) {
        sync1_reference = TAPE_SYNC_FIRST_PULSE_TSTATES;
    }
    if (sync2_reference <= 0) {
        sync2_reference = TAPE_SYNC_SECOND_PULSE_TSTATES;
    }

    const int sync1_tolerance = tape_duration_tolerance(sync1_reference);
    const int sync2_tolerance = tape_duration_tolerance(sync2_reference);
    if (!tape_duration_matches(pulses[index].duration, sync1_reference, sync1_tolerance) ||
        !tape_duration_matches(pulses[index + 1].duration, sync2_reference, sync2_tolerance)) {
        return 0;
    }

    index += 2;

    size_t data_limit = count;
    while (data_limit > index && ((data_limit - index) % 2u) != 0u) {
        --data_limit;
    }

    if (data_limit <= index) {
        return 0;
    }

    const size_t bits_per_byte = 8u;
    size_t bit_pairs = (data_limit - index) / 2u;
    while (bit_pairs > 0 && (bit_pairs % bits_per_byte) != 0u) {
        data_limit -= 2u;
        bit_pairs = (data_limit - index) / 2u;
    }

    if (bit_pairs == 0 || (bit_pairs % bits_per_byte) != 0u) {
        return 0;
    }

    size_t byte_count = bit_pairs / bits_per_byte;
    uint8_t* data = (uint8_t*)malloc(byte_count ? byte_count : 1u);
    if (!data) {
        return 0;
    }
    memset(data, 0, byte_count ? byte_count : 1u);

    int bit0_reference = (int)((double)TAPE_BIT0_PULSE_TSTATES * scale + 0.5);
    int bit1_reference = (int)((double)TAPE_BIT1_PULSE_TSTATES * scale + 0.5);
    if (bit0_reference <= 0) {
        bit0_reference = TAPE_BIT0_PULSE_TSTATES;
    }
    if (bit1_reference <= 0) {
        bit1_reference = TAPE_BIT1_PULSE_TSTATES;
    }

    const int bit0_tolerance = tape_duration_tolerance(bit0_reference);
    const int bit1_tolerance = tape_duration_tolerance(bit1_reference);
    const int bit0_pair_reference = bit0_reference * 2;
    const int bit1_pair_reference = bit1_reference * 2;
    const int bit0_pair_tolerance = tape_duration_tolerance(bit0_pair_reference);
    const int bit1_pair_tolerance = tape_duration_tolerance(bit1_pair_reference);

    for (size_t byte_index = 0; byte_index < byte_count; ++byte_index) {
        uint8_t value = 0;
        for (size_t bit = 0; bit < 8; ++bit) {
            if (index >= data_limit) {
                free(data);
                return 0;
            }
            uint32_t d1 = pulses[index].duration;
            uint32_t d2 = pulses[index + 1].duration;
            index += 2;
            int is_one = tape_duration_matches(d1, bit1_reference, bit1_tolerance) &&
                         tape_duration_matches(d2, bit1_reference, bit1_tolerance);
            int is_zero = tape_duration_matches(d1, bit0_reference, bit0_tolerance) &&
                          tape_duration_matches(d2, bit0_reference, bit0_tolerance);
            uint32_t pair_sum = d1 + d2;
            if (!is_one && !is_zero) {
                int sum_diff_one = abs((int)pair_sum - bit1_pair_reference);
                int sum_diff_zero = abs((int)pair_sum - bit0_pair_reference);
                if (sum_diff_one <= bit1_pair_tolerance && sum_diff_one < sum_diff_zero) {
                    is_one = 1;
                } else if (sum_diff_zero <= bit0_pair_tolerance) {
                    is_zero = 1;
                } else {
                    int score_one = abs((int)d1 - bit1_reference) + abs((int)d2 - bit1_reference);
                    int score_zero = abs((int)d1 - bit0_reference) + abs((int)d2 - bit0_reference);
                    if (score_one < score_zero && score_one <= bit1_tolerance * 4) {
                        is_one = 1;
                    } else if (score_zero <= score_one && score_zero <= bit0_tolerance * 4) {
                        is_zero = 1;
                    } else {
                        free(data);
                        return 0;
                    }
                }
            }
            if (is_one) {
                value |= (uint8_t)(1u << (7 - bit));
            }
        }

        data[byte_index] = value;
    }

    out_block->data = data;
    out_block->length = (uint32_t)byte_count;
    out_block->pause_ms = pause_ms;
    return 1;
}

static void tape_recorder_finalize_block(uint64_t current_t_state, int force_flush) {
    if (!tape_recorder.block_active || tape_recorder.pulse_count == 0) {
        if (force_flush) {
            tape_recorder.block_active = 0;
        }
        return;
    }

    uint64_t idle_cycles = 0;
    if (current_t_state > tape_recorder.last_transition_tstate) {
        idle_cycles = current_t_state - tape_recorder.last_transition_tstate;
    }

    if (!force_flush && idle_cycles < TAPE_SILENCE_THRESHOLD_TSTATES) {
        return;
    }

    uint32_t pause_ms = 1000u;
    if (idle_cycles > 0) {
        double pause = ((double)idle_cycles / CPU_CLOCK_HZ) * 1000.0;
        if (pause > 0.0) {
            if (pause > 10000.0) {
                pause = 10000.0;
            }
            pause_ms = (uint32_t)(pause + 0.5);
        }
    }

    size_t pulse_count = tape_recorder.pulse_count;
    if (tape_recorder.output_format == TAPE_OUTPUT_TAP && pulse_count >= 100) {
        TapeBlock block = {0};
        if (!tape_decode_pulses_to_block(tape_recorder.pulses, pulse_count, pause_ms, &block)) {
            fprintf(stderr, "Warning: failed to decode saved tape block (%zu pulses)\n", pulse_count);
        } else {
            if (!tape_image_add_block(&tape_recorder.recorded, block.data, block.length, block.pause_ms)) {
                fprintf(stderr, "Warning: failed to store recorded tape block\n");
            }
            free(block.data);
        }
    }

    tape_recorder_append_block_audio(idle_cycles);

    tape_recorder.block_active = 0;
    tape_recorder.pulse_count = 0;
    tape_recorder.last_transition_tstate = current_t_state;
}

static void tape_recorder_handle_mic(uint64_t t_state, int level) {
    if (!tape_recorder.enabled || !tape_recorder.recording) {
        return;
    }

    if (!tape_recorder.block_active) {
        tape_recorder.block_active = 1;
        tape_recorder.last_transition_tstate = t_state;
        tape_recorder.last_level = level;
        tape_recorder.block_start_level = level ? 1 : 0;
        return;
    }

    if (level == tape_recorder.last_level) {
        return;
    }

    uint64_t duration = 0;
    if (t_state > tape_recorder.last_transition_tstate) {
        duration = t_state - tape_recorder.last_transition_tstate;
    }
    if (!tape_recorder_append_pulse(duration)) {
        fprintf(stderr, "Warning: failed to record tape pulse\n");
    }
    tape_recorder.last_transition_tstate = t_state;
    tape_recorder.last_level = level;
}

static void tape_recorder_update(uint64_t current_t_state, int force_flush) {
    if (!tape_recorder.enabled) {
        return;
    }
    if (!tape_recorder.recording && !force_flush) {
        return;
    }
    tape_recorder_finalize_block(current_t_state, force_flush);
}

static int tape_recorder_write_output(void) {
    if (!tape_recorder.enabled || !tape_recorder.output_path) {
        return 1;
    }

    if (!tape_recorder.session_dirty) {
        return 1;
    }

    if (tape_recorder.output_format == TAPE_OUTPUT_WAV) {
        return tape_recorder_write_wav();
    }

    FILE* tf = fopen(tape_recorder.output_path, "wb");
    if (!tf) {
        fprintf(stderr, "Failed to open tape output '%s': %s\n", tape_recorder.output_path, strerror(errno));
        return 0;
    }

    int success = 1;

    if (tape_recorder.recorded.count > 0) {
        for (size_t i = 0; i < tape_recorder.recorded.count && success; ++i) {
            const TapeBlock* block = &tape_recorder.recorded.blocks[i];
            uint16_t length = (uint16_t)block->length;
            uint8_t length_bytes[2];
            length_bytes[0] = (uint8_t)(length & 0xFFu);
            length_bytes[1] = (uint8_t)((length >> 8) & 0xFFu);
            if (fwrite(length_bytes, sizeof(length_bytes), 1, tf) != 1) {
                fprintf(stderr, "Failed to write TAP block length\n");
                success = 0;
                break;
            }
            if (length > 0 && block->data) {
                if (fwrite(block->data, length, 1, tf) != 1) {
                    fprintf(stderr, "Failed to write TAP block payload\n");
                    success = 0;
                    break;
                }
            }
        }
    }

    if (fclose(tf) != 0) {
        fprintf(stderr, "Failed to finalize tape output '%s': %s\n", tape_recorder.output_path, strerror(errno));
        success = 0;
    }

    if (success) {
        tape_recorder.session_dirty = 0;
        printf("Tape recording saved to %s\n", tape_recorder.output_path);
    }

    return success;
}

static void tape_shutdown(void) {
    tape_pause_playback(&tape_playback, total_t_states);
    tape_recorder_stop_session(total_t_states, 1);
    tape_free_image(&tape_playback.image);
    tape_waveform_reset(&tape_playback.waveform);
    tape_free_image(&tape_recorder.recorded);
    tape_recorder_reset_pulses();
    tape_recorder_reset_audio();
    tape_recorder_reset_wav_prefix();
}

static void tape_deck_play(uint64_t current_t_state) {
    TapePlaybackState* state = &tape_playback;
    if (!tape_input_enabled) {
        printf("Tape PLAY ignored (no tape loaded)\n");
        return;
    }

    if (state->playing) {
        printf("Tape already playing\n");
        return;
    }

    if (state->format == TAPE_FORMAT_WAV) {
        if (state->waveform.count == 0) {
            printf("Tape PLAY ignored (empty tape)\n");
            return;
        }
        (void)tape_resume_playback(state, current_t_state);
    } else {
        if (state->image.count == 0) {
            printf("Tape PLAY ignored (empty tape)\n");
            return;
        }
        (void)tape_resume_playback(state, current_t_state);
    }

    if (state->playing) {
        printf("Tape PLAY\n");
        tape_deck_status = TAPE_DECK_STATUS_PLAY;
    } else {
        printf("Tape PLAY ignored (tape at end)\n");
    }
}

static void tape_deck_stop(uint64_t current_t_state) {
    int was_playing = tape_playback.playing;
    if (was_playing) {
        tape_pause_playback(&tape_playback, current_t_state);
    }

    int was_recording = tape_recorder.recording;
    if (was_recording || tape_recorder.session_dirty) {
        tape_recorder_stop_session(current_t_state, 1);
    }

    if (was_playing || was_recording) {
        printf("Tape STOP\n");
    } else {
        printf("Tape STOP (idle)\n");
    }
    tape_deck_status = TAPE_DECK_STATUS_STOP;
}

static void tape_deck_rewind(uint64_t current_t_state) {
    tape_pause_playback(&tape_playback, current_t_state);
    tape_rewind_playback(&tape_playback);
    tape_recorder_stop_session(current_t_state, 1);
    tape_wav_shared_position_tstates = 0;
    tape_recorder.position_tstates = 0;
    tape_recorder.position_start_tstate = current_t_state;
    printf("Tape REWIND\n");
    tape_deck_status = TAPE_DECK_STATUS_REWIND;
}

static void tape_deck_record(uint64_t current_t_state, int append_mode) {
    if (!tape_recorder.enabled) {
        if (tape_input_format == TAPE_FORMAT_WAV && tape_input_path) {
            tape_recorder_enable(tape_input_path, TAPE_OUTPUT_WAV);
            if (tape_playback.waveform.sample_rate > 0) {
                tape_recorder.sample_rate = tape_playback.waveform.sample_rate;
            }
            printf("Tape recorder destination set to %s\n", tape_recorder.output_path);
        } else {
            printf("Tape RECORD ignored (no output configured)\n");
            return;
        }
    }

    tape_pause_playback(&tape_playback, current_t_state);
    if (!tape_recorder_start_session(current_t_state, append_mode ? 1 : 0)) {
        return;
    }
    if (tape_recorder.recording) {
        tape_deck_status = TAPE_DECK_STATUS_RECORD;
    }
}

static int tape_handle_control_key(const SDL_Event* event) {
    if (!event || (event->type != SDL_KEYDOWN && event->type != SDL_KEYUP)) {
        return 0;
    }

    SDL_Keycode key = event->key.keysym.sym;
    if (key != SDLK_F5 && key != SDLK_F6 && key != SDLK_F7 && key != SDLK_F8) {
        return 0;
    }

    if (event->type == SDL_KEYDOWN) {
        if (event->key.repeat) {
            return 1;
        }
        int append_mode = (event->key.keysym.mod & KMOD_SHIFT) ? 1 : 0;
        switch (key) {
            case SDLK_F5:
                tape_deck_play(total_t_states);
                break;
            case SDLK_F6:
                tape_deck_stop(total_t_states);
                break;
            case SDLK_F7:
                tape_deck_rewind(total_t_states);
                break;
            case SDLK_F8:
                tape_deck_record(total_t_states, append_mode);
                break;
            default:
                break;
        }
    }

    return 1;
}

static int tape_handle_mouse_button(const SDL_Event* event) {
    if (!event || event->type != SDL_MOUSEBUTTONDOWN) {
        return 0;
    }

    if (event->button.button != SDL_BUTTON_LEFT) {
        return 0;
    }

    if (tape_control_button_count <= 0) {
        return 0;
    }

    int mouse_x = event->button.x;
    int mouse_y = event->button.y;

    for (int i = 0; i < tape_control_button_count; ++i) {
        TapeControlButton* button = &tape_control_buttons[i];
        if (!button->visible) {
            continue;
        }

        int left = button->rect.x;
        int top = button->rect.y;
        int right = left + button->rect.w;
        int bottom = top + button->rect.h;

        if (mouse_x < left || mouse_x >= right || mouse_y < top || mouse_y >= bottom) {
            continue;
        }

        if (!button->enabled) {
            return 1;
        }

        switch (button->action) {
            case TAPE_CONTROL_ACTION_PLAY:
                tape_deck_play(total_t_states);
                break;
            case TAPE_CONTROL_ACTION_STOP:
                tape_deck_stop(total_t_states);
                break;
            case TAPE_CONTROL_ACTION_REWIND:
                tape_deck_rewind(total_t_states);
                break;
            case TAPE_CONTROL_ACTION_RECORD:
                {
                    SDL_Keymod mods = SDL_GetModState();
                    int append_mode = (mods & KMOD_SHIFT) ? 1 : 0;
                    tape_deck_record(total_t_states, append_mode);
                }
                break;
            case TAPE_CONTROL_ACTION_NONE:
            default:
                break;
        }

        return 1;
    }

    return 0;
}

static size_t beeper_catch_up_to(double catch_up_position, double playback_position_snapshot) {
    if (beeper_cycles_per_sample <= 0.0) {
        return 0;
    }

    double playback_position = playback_position_snapshot;
    if (catch_up_position <= playback_position) {
        return 0;
    }

    double cycles_per_sample = beeper_cycles_per_sample;
    double last_input = beeper_hp_last_input;
    double last_output = beeper_hp_last_output;
    int level = beeper_playback_level;
    size_t head = beeper_event_head;
    size_t consumed = 0;

    while (playback_position + cycles_per_sample < catch_up_position) {
        double target_position = playback_position + cycles_per_sample;

        while (head != beeper_event_tail &&
               (double)beeper_events[head].t_state <= target_position) {
            level = beeper_events[head].level ? 1 : 0;
            head = (head + 1) % BEEPER_EVENT_CAPACITY;
            ++consumed;
        }

        double raw_sample = (level ? 1.0 : -1.0) * (double)AUDIO_AMPLITUDE;
        double filtered_sample = raw_sample - last_input + BEEPER_HP_ALPHA * last_output;
        last_input = raw_sample;
        last_output = filtered_sample;

        playback_position = target_position;
    }

    if (playback_position < catch_up_position) {
        double target_position = playback_position + cycles_per_sample;

        while (head != beeper_event_tail &&
               (double)beeper_events[head].t_state <= target_position) {
            level = beeper_events[head].level ? 1 : 0;
            head = (head + 1) % BEEPER_EVENT_CAPACITY;
            ++consumed;
        }

        double raw_sample = (level ? 1.0 : -1.0) * (double)AUDIO_AMPLITUDE;
        double filtered_sample = raw_sample - last_input + BEEPER_HP_ALPHA * last_output;
        last_input = raw_sample;
        last_output = filtered_sample;

        playback_position = target_position;
    }

    while (head != beeper_event_tail &&
           (double)beeper_events[head].t_state <= catch_up_position) {
        level = beeper_events[head].level ? 1 : 0;
        head = (head + 1) % BEEPER_EVENT_CAPACITY;
        ++consumed;
    }

    beeper_event_head = head;
    beeper_playback_position = playback_position;
    beeper_playback_level = level;
    beeper_hp_last_input = last_input;
    beeper_hp_last_output = last_output;
    if (beeper_writer_cursor < playback_position) {
        beeper_writer_cursor = playback_position;
    }

    return consumed;
}

static void beeper_push_event(uint64_t t_state, int level) {
    int locked_audio = 0;
    if (audio_available) {
        SDL_LockAudio();
        locked_audio = 1;
    }

    uint64_t original_t_state = t_state;
    int was_idle = beeper_idle_log_active;
    double playback_snapshot = beeper_playback_position;
    size_t pending_before = beeper_pending_event_count();
    if (beeper_cycles_per_sample > 0.0) {
        double event_offset_cycles = (double)t_state - playback_snapshot;
        double rewind_threshold_cycles =
            beeper_cycles_per_sample * BEEPER_REWIND_TOLERANCE_SAMPLES;

        if (event_offset_cycles < -rewind_threshold_cycles) {
            double rewind_samples = 0.0;
            if (beeper_cycles_per_sample > 0.0) {
                rewind_samples = -event_offset_cycles / beeper_cycles_per_sample;
            }

            BEEPER_LOG(
                "[BEEPER] timeline rewind detected: event at %llu is %.2f samples behind playback %.0f (pending %zu); resyncing audio state\n",
                (unsigned long long)t_state,
                rewind_samples,
                playback_snapshot,
                pending_before);

            beeper_force_resync(t_state);
            playback_snapshot = beeper_playback_position;
            pending_before = 0;
            was_idle = 0;
        }
    }

    if (beeper_cycles_per_sample > 0.0) {
        double playback_position_snapshot = beeper_playback_position;
        double latency_cycles = (double)t_state - playback_position_snapshot;
        double max_latency_cycles = beeper_cycles_per_sample * beeper_max_latency_samples;

        if (latency_cycles > max_latency_cycles) {
            if (!audio_available) {
                double catch_up_position = (double)t_state - max_latency_cycles;
                if (catch_up_position < 0.0) {
                    catch_up_position = 0.0;
                }
                size_t pending_before = beeper_pending_event_count();
                size_t consumed = beeper_catch_up_to(catch_up_position, playback_position_snapshot);

                double new_latency_cycles = (double)t_state - beeper_playback_position;
                double queued_samples_before = latency_cycles / beeper_cycles_per_sample;
                double queued_samples_after = new_latency_cycles / beeper_cycles_per_sample;
                size_t pending_after = beeper_pending_event_count();
                double catch_up_error_samples = 0.0;
                if (beeper_cycles_per_sample > 0.0) {
                    catch_up_error_samples = (catch_up_position - beeper_playback_position) /
                                             beeper_cycles_per_sample;
                }

                BEEPER_LOG(
                    "[BEEPER] catch-up: backlog %.2f samples -> %.2f samples (consumed %zu events, queue %zu -> %zu, catch-up err %.4f samples)\n",
                    queued_samples_before,
                    queued_samples_after,
                    consumed,
                    pending_before,
                    pending_after,
                    catch_up_error_samples);

                uint64_t catch_up_cycles = (uint64_t)catch_up_position;
                if (catch_up_cycles > beeper_last_event_t_state) {
                    beeper_last_event_t_state = catch_up_cycles;
                }
            } else {
                double throttle_cycles = beeper_cycles_per_sample * beeper_latency_throttle_samples;

                if (throttle_cycles > 0.0 && latency_cycles > throttle_cycles) {
                    double trim_cycles = beeper_cycles_per_sample * beeper_latency_trim_samples;
                    int should_trim = trim_cycles > throttle_cycles && latency_cycles > trim_cycles;

                    if (should_trim) {
                        double catch_up_position = (double)t_state - throttle_cycles;
                        if (catch_up_position < 0.0) {
                            catch_up_position = 0.0;
                        }

                        double playback_snapshot = beeper_playback_position;
                        size_t pending_before = beeper_pending_event_count();
                        size_t consumed = beeper_catch_up_to(catch_up_position, playback_snapshot);
                        double new_latency_cycles = (double)t_state - beeper_playback_position;
                        double queued_samples_before = latency_cycles / beeper_cycles_per_sample;
                        double queued_samples_after = new_latency_cycles / beeper_cycles_per_sample;
                        size_t pending_after = beeper_pending_event_count();
                        double catch_up_error_samples = 0.0;
                        if (beeper_cycles_per_sample > 0.0) {
                            catch_up_error_samples = (catch_up_position - beeper_playback_position) /
                                                     beeper_cycles_per_sample;
                        }

                        if (consumed > 0 || queued_samples_after < queued_samples_before) {
                            BEEPER_LOG(
                                "[BEEPER] trimmed backlog %.2f -> %.2f samples (consumed %zu events, queue %zu -> %zu, catch-up err %.4f samples)\n",
                                queued_samples_before,
                                queued_samples_after,
                                consumed,
                                pending_before,
                                pending_after,
                                catch_up_error_samples);
                        }

                        uint64_t catch_up_cycles = (uint64_t)catch_up_position;
                        if (catch_up_cycles > beeper_last_event_t_state) {
                            beeper_last_event_t_state = catch_up_cycles;
                        }

                        latency_cycles = new_latency_cycles;
                    }
                }
            }
        } else if (audio_available && beeper_latency_warning_active) {
            beeper_latency_warning_active = 0;
        }
    }

    if (t_state < beeper_last_event_t_state) {
        uint64_t clamped_t_state = beeper_last_event_t_state;
        double drift_samples = 0.0;
        if (beeper_cycles_per_sample > 0.0) {
            drift_samples = (double)(clamped_t_state - original_t_state) / beeper_cycles_per_sample;
        }
        BEEPER_LOG(
            "[BEEPER] event time rewind: requested %llu, clamped to %llu (drift %.2f samples, playback %.0f)\n",
            (unsigned long long)original_t_state,
            (unsigned long long)clamped_t_state,
            drift_samples,
            playback_snapshot);
        t_state = clamped_t_state;
    } else {
        beeper_last_event_t_state = t_state;
    }

    double event_cursor = (double)t_state;
    if (event_cursor > beeper_writer_cursor) {
        beeper_writer_cursor = event_cursor;
    }

    if (was_idle) {
        double delta_samples = 0.0;
        if (beeper_cycles_per_sample > 0.0) {
            delta_samples = ((double)t_state - playback_snapshot) / beeper_cycles_per_sample;
        }
        BEEPER_LOG(
            "[BEEPER] idle period cleared by event at %llu (delta %.2f samples, playback %.0f, pending %zu)\n",
            (unsigned long long)t_state,
            delta_samples,
            playback_snapshot,
            pending_before);
        beeper_idle_log_active = 0;
    }

    size_t next_tail = (beeper_event_tail + 1) % BEEPER_EVENT_CAPACITY;
    if (next_tail == beeper_event_head) {
        beeper_event_head = (beeper_event_head + 1) % BEEPER_EVENT_CAPACITY;
    }

    beeper_events[beeper_event_tail].t_state = t_state;
    beeper_events[beeper_event_tail].level = (uint8_t)(level ? 1 : 0);
    beeper_event_tail = next_tail;

    if (locked_audio) {
        SDL_UnlockAudio();
    }
}

void io_write(uint16_t port, uint8_t value) {
    if ((port & 1) == 0) { // ULA Port FE
        ula_queue_port_value(value);
    }
    (void)port;
    (void)value;
}
uint8_t io_read(uint16_t port) {
    if ((port & 1) == 0) {
        tape_update(total_t_states);
        tape_recorder_update(total_t_states, 0);

        uint8_t result = 0xFF;
        uint8_t high_byte = (port >> 8) & 0xFF;
        for (int row = 0; row < 8; ++row) { if (! (high_byte & (1 << row)) ) { result &= keyboard_matrix[row]; } }
        if (tape_ear_state) {
            result |= 0x40;
        } else {
            result &= (uint8_t)~0x40;
        }
        result |= 0xA0; // Set unused bits high
        // printf("IO Read Port 0x%04X (ULA/Keyboard): AddrHi=0x%02X -> Result=0x%02X\n", port, high_byte, result); // DEBUG
        return result;
    }
    return 0xFF;
}

// --- 16-bit Register Pair Helpers ---
static inline uint16_t get_AF(Z80* cpu){return(cpu->reg_A<<8)|cpu->reg_F;} static inline void set_AF(Z80* cpu,uint16_t v){cpu->reg_A=(v>>8)&0xFF;cpu->reg_F=v&0xFF;}
static inline uint16_t get_BC(Z80* cpu){return(cpu->reg_B<<8)|cpu->reg_C;} static inline void set_BC(Z80* cpu,uint16_t v){cpu->reg_B=(v>>8)&0xFF;cpu->reg_C=v&0xFF;}
static inline uint16_t get_DE(Z80* cpu){return(cpu->reg_D<<8)|cpu->reg_E;} static inline void set_DE(Z80* cpu,uint16_t v){cpu->reg_D=(v>>8)&0xFF;cpu->reg_E=v&0xFF;}
static inline uint16_t get_HL(Z80* cpu){return(cpu->reg_H<<8)|cpu->reg_L;} static inline void set_HL(Z80* cpu,uint16_t v){cpu->reg_H=(v>>8)&0xFF;cpu->reg_L=v&0xFF;}
static inline uint8_t get_IXh(Z80* cpu){return(cpu->reg_IX>>8)&0xFF;} static inline uint8_t get_IXl(Z80* cpu){return cpu->reg_IX&0xFF;} static inline void set_IXh(Z80* cpu,uint8_t v){cpu->reg_IX=(cpu->reg_IX&0x00FF)|(v<<8);} static inline void set_IXl(Z80* cpu,uint8_t v){cpu->reg_IX=(cpu->reg_IX&0xFF00)|v;}
static inline uint8_t get_IYh(Z80* cpu){return(cpu->reg_IY>>8)&0xFF;} static inline uint8_t get_IYl(Z80* cpu){return cpu->reg_IY&0xFF;} static inline void set_IYh(Z80* cpu,uint8_t v){cpu->reg_IY=(cpu->reg_IY&0x00FF)|(v<<8);} static inline void set_IYl(Z80* cpu,uint8_t v){cpu->reg_IY=(cpu->reg_IY&0xFF00)|v;}
static inline void set_flag(Z80* cpu,uint8_t f,int c){if(c)cpu->reg_F|=f;else cpu->reg_F&=~f;} static inline uint8_t get_flag(Z80* cpu,uint8_t f){return(cpu->reg_F&f)?1:0;}
static inline void set_xy_flags(Z80* cpu, uint8_t value) {
    cpu->reg_F = (uint8_t)((cpu->reg_F & (uint8_t)~0x28u) | (value & 0x28u));
}

static inline void set_flags_szp(Z80* cpu,uint8_t r){set_flag(cpu,FLAG_S,r&0x80);set_flag(cpu,FLAG_Z,r==0);uint8_t p=0;uint8_t t=r;for(int i=0;i<8;i++){if(t&1)p=!p;t>>=1;}set_flag(cpu,FLAG_PV,!p);set_xy_flags(cpu,r);}

// --- 8-Bit Arithmetic/Logic Helper Functions ---
void cpu_add(Z80* cpu,uint8_t v){uint16_t r=cpu->reg_A+v;uint8_t hc=((cpu->reg_A&0x0F)+(v&0x0F))>0x0F;set_flag(cpu,FLAG_S,r&0x80);set_flag(cpu,FLAG_Z,(r&0xFF)==0);set_flag(cpu,FLAG_H,hc);set_flag(cpu,FLAG_PV,((cpu->reg_A^v^0x80)&(r^v)&0x80)!=0);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,r>0xFF);cpu->reg_A=r&0xFF;set_xy_flags(cpu,cpu->reg_A);}
void cpu_adc(Z80* cpu,uint8_t v){uint8_t c=get_flag(cpu,FLAG_C);uint16_t r=cpu->reg_A+v+c;uint8_t hc=((cpu->reg_A&0x0F)+(v&0x0F)+c)>0x0F;set_flag(cpu,FLAG_S,r&0x80);set_flag(cpu,FLAG_Z,(r&0xFF)==0);set_flag(cpu,FLAG_H,hc);set_flag(cpu,FLAG_PV,((cpu->reg_A^v^0x80)&(r^v)&0x80)!=0);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,r>0xFF);cpu->reg_A=r&0xFF;set_xy_flags(cpu,cpu->reg_A);}
void cpu_sub(Z80* cpu,uint8_t v,int s){uint16_t r=cpu->reg_A-v;uint8_t hb=((cpu->reg_A&0x0F)<(v&0x0F));set_flag(cpu,FLAG_S,r&0x80);set_flag(cpu,FLAG_Z,(r&0xFF)==0);set_flag(cpu,FLAG_H,hb);set_flag(cpu,FLAG_PV,((cpu->reg_A^v)&(cpu->reg_A^r)&0x80)!=0);set_flag(cpu,FLAG_N,1);set_flag(cpu,FLAG_C,r>0xFF);set_xy_flags(cpu,(uint8_t)r);if(s)cpu->reg_A=r&0xFF;}
void cpu_sbc(Z80* cpu,uint8_t v){uint8_t c=get_flag(cpu,FLAG_C);uint16_t r=cpu->reg_A-v-c;uint8_t hb=((cpu->reg_A&0x0F)<((v&0x0F)+c));set_flag(cpu,FLAG_S,r&0x80);set_flag(cpu,FLAG_Z,(r&0xFF)==0);set_flag(cpu,FLAG_H,hb);set_flag(cpu,FLAG_PV,((cpu->reg_A^v)&(cpu->reg_A^r)&0x80)!=0);set_flag(cpu,FLAG_N,1);set_flag(cpu,FLAG_C,r>0xFF);cpu->reg_A=r&0xFF;set_xy_flags(cpu,cpu->reg_A);}
void cpu_and(Z80* cpu,uint8_t v){cpu->reg_A&=v;set_flags_szp(cpu,cpu->reg_A);set_flag(cpu,FLAG_H,1);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,0);}
void cpu_or(Z80* cpu,uint8_t v){cpu->reg_A|=v;set_flags_szp(cpu,cpu->reg_A);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,0);}
void cpu_xor(Z80* cpu,uint8_t v){cpu->reg_A^=v;set_flags_szp(cpu,cpu->reg_A);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,0);}
uint8_t cpu_inc(Z80* cpu,uint8_t v){uint8_t r=v+1;set_flag(cpu,FLAG_S,r&0x80);set_flag(cpu,FLAG_Z,r==0);set_flag(cpu,FLAG_H,(v&0x0F)==0x0F);set_flag(cpu,FLAG_PV,v==0x7F);set_flag(cpu,FLAG_N,0);set_xy_flags(cpu,r);return r;}
uint8_t cpu_dec(Z80* cpu,uint8_t v){uint8_t r=v-1;set_flag(cpu,FLAG_S,r&0x80);set_flag(cpu,FLAG_Z,r==0);set_flag(cpu,FLAG_H,(v&0x0F)==0x00);set_flag(cpu,FLAG_PV,v==0x80);set_flag(cpu,FLAG_N,1);set_xy_flags(cpu,r);return r;}
void cpu_add_hl(Z80* cpu,uint16_t v){uint16_t hl=get_HL(cpu);uint32_t r=hl+v;set_flag(cpu,FLAG_H,((hl&0x0FFF)+(v&0x0FFF))>0x0FFF);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,r>0xFFFF);set_HL(cpu,r&0xFFFF);set_xy_flags(cpu,(uint8_t)((r>>8)&0xFF));}
void cpu_add_ixiy(Z80* cpu,uint16_t* rr,uint16_t v){uint16_t ixy=*rr;uint32_t r=ixy+v;set_flag(cpu,FLAG_H,((ixy&0x0FFF)+(v&0x0FFF))>0x0FFF);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,r>0xFFFF);*rr=r&0xFFFF;set_xy_flags(cpu,(uint8_t)((r>>8)&0xFF));}
void cpu_adc_hl(Z80* cpu,uint16_t v){uint16_t hl=get_HL(cpu);uint8_t c=get_flag(cpu,FLAG_C);uint32_t r=hl+v+c;set_flag(cpu,FLAG_S,(r&0x8000)!=0);set_flag(cpu,FLAG_Z,(r&0xFFFF)==0);set_flag(cpu,FLAG_H,((hl&0x0FFF)+(v&0x0FFF)+c)>0x0FFF);set_flag(cpu,FLAG_PV,(((hl^v^0x8000)&(r^v)&0x8000))!=0);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,r>0xFFFF);set_HL(cpu,r&0xFFFF);set_xy_flags(cpu,(uint8_t)((r>>8)&0xFF));}
void cpu_sbc_hl(Z80* cpu,uint16_t v){uint16_t hl=get_HL(cpu);uint8_t c=get_flag(cpu,FLAG_C);uint32_t r=hl-v-c;set_flag(cpu,FLAG_S,(r&0x8000)!=0);set_flag(cpu,FLAG_Z,(r&0xFFFF)==0);set_flag(cpu,FLAG_H,((hl&0x0FFF)<((v&0x0FFF)+c)));set_flag(cpu,FLAG_PV,((hl^v)&(hl^(uint16_t)r)&0x8000)!=0);set_flag(cpu,FLAG_N,1);set_flag(cpu,FLAG_C,r>0xFFFF);set_HL(cpu,r&0xFFFF);set_xy_flags(cpu,(uint8_t)((r>>8)&0xFF));}
void cpu_push(Z80* cpu,uint16_t v){cpu->reg_SP--;writeByte(cpu->reg_SP,(v>>8)&0xFF);cpu->reg_SP--;writeByte(cpu->reg_SP,v&0xFF);}
uint16_t cpu_pop(Z80* cpu){uint8_t lo=readByte(cpu->reg_SP);cpu->reg_SP++;uint8_t hi=readByte(cpu->reg_SP);cpu->reg_SP++;return(hi<<8)|lo;}
uint8_t cpu_rlc(Z80* cpu,uint8_t v){uint8_t c=(v&0x80)?1:0;uint8_t r=(v<<1)|c;set_flags_szp(cpu,r);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,c);return r;}
uint8_t cpu_rrc(Z80* cpu,uint8_t v){uint8_t c=(v&0x01);uint8_t r=(v>>1)|(c<<7);set_flags_szp(cpu,r);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,c);return r;}
uint8_t cpu_rl(Z80* cpu,uint8_t v){uint8_t oc=get_flag(cpu,FLAG_C);uint8_t nc=(v&0x80)?1:0;uint8_t r=(v<<1)|oc;set_flags_szp(cpu,r);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,nc);return r;}
uint8_t cpu_rr(Z80* cpu,uint8_t v){uint8_t oc=get_flag(cpu,FLAG_C);uint8_t nc=(v&0x01);uint8_t r=(v>>1)|(oc<<7);set_flags_szp(cpu,r);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,nc);return r;}
uint8_t cpu_sla(Z80* cpu,uint8_t v){uint8_t c=(v&0x80)?1:0;uint8_t r=(v<<1);set_flags_szp(cpu,r);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,c);return r;}
uint8_t cpu_sra(Z80* cpu,uint8_t v){uint8_t c=(v&0x01);uint8_t r=(v>>1)|(v&0x80);set_flags_szp(cpu,r);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,c);return r;}
uint8_t cpu_srl(Z80* cpu,uint8_t v){uint8_t c=(v&0x01);uint8_t r=(v>>1);set_flags_szp(cpu,r);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,c);return r;}
uint8_t cpu_sll(Z80* cpu,uint8_t v){uint8_t c=(v&0x80)?1:0;uint8_t r=(uint8_t)((v<<1)|0x01);set_flags_szp(cpu,r);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,c);return r;}
void cpu_bit(Z80* cpu,uint8_t v,int b){uint8_t m=(1<<b);set_flag(cpu,FLAG_Z,(v&m)==0);set_flag(cpu,FLAG_PV,(v&m)==0);set_flag(cpu,FLAG_H,1);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_S,(b==7)&&(v&0x80));set_xy_flags(cpu,v);}

// --- 0xCB Prefix CPU Step Function ---
int cpu_cb_step(Z80* cpu) {
    uint8_t op=readByte(cpu->reg_PC++);uint8_t x=(op>>6)&3;uint8_t y=(op>>3)&7;uint8_t z=op&7;
    uint16_t hl_addr=0;int is_hl=(z==6);
    if(is_hl)hl_addr=get_HL(cpu);
    uint8_t operand;
    switch(z){case 0:operand=cpu->reg_B;break;case 1:operand=cpu->reg_C;break;case 2:operand=cpu->reg_D;break;case 3:operand=cpu->reg_E;break;case 4:operand=cpu->reg_H;break;case 5:operand=cpu->reg_L;break;case 6:operand=readByte(hl_addr);break;case 7:operand=cpu->reg_A;break;default:operand=0;}
    uint8_t result=operand;
    switch(x){case 0:switch(y){case 0:result=cpu_rlc(cpu,operand);break;case 1:result=cpu_rrc(cpu,operand);break;case 2:result=cpu_rl(cpu,operand);break;case 3:result=cpu_rr(cpu,operand);break;case 4:result=cpu_sla(cpu,operand);break;case 5:result=cpu_sra(cpu,operand);break;case 6:result=cpu_sll(cpu,operand);break;case 7:result=cpu_srl(cpu,operand);break;}break;
               case 1:cpu_bit(cpu,operand,y);return is_hl ? 8 : 4;
               case 2:result=operand&~(1<<y);break;case 3:result=operand|(1<<y);break;}
    switch(z){case 0:cpu->reg_B=result;break;case 1:cpu->reg_C=result;break;case 2:cpu->reg_D=result;break;case 3:cpu->reg_E=result;break;case 4:cpu->reg_H=result;break;case 5:cpu->reg_L=result;break;case 6:writeByte(hl_addr,result);break;case 7:cpu->reg_A=result;break;}
    return is_hl ? 11 : 4;
}

// --- 0xED Prefix CPU Step Function ---

int cpu_ed_step(Z80* cpu) {
    uint8_t op = readByte(cpu->reg_PC++);
    switch (op) {
        case 0x4A: cpu_adc_hl(cpu, get_BC(cpu)); return 11;
        case 0x5A: cpu_adc_hl(cpu, get_DE(cpu)); return 11;
        case 0x6A: cpu_adc_hl(cpu, get_HL(cpu)); return 11;
        case 0x7A: cpu_adc_hl(cpu, cpu->reg_SP); return 11;
        case 0x42: cpu_sbc_hl(cpu, get_BC(cpu)); return 11;
        case 0x52: cpu_sbc_hl(cpu, get_DE(cpu)); return 11;
        case 0x62: cpu_sbc_hl(cpu, get_HL(cpu)); return 11;
        case 0x72: cpu_sbc_hl(cpu, cpu->reg_SP); return 11;
        case 0x43: {
            uint16_t addr = readWord(cpu->reg_PC);
            cpu->reg_PC += 2;
            writeWord(addr, get_BC(cpu));
            return 16;
        }
        case 0x53: {
            uint16_t addr = readWord(cpu->reg_PC);
            cpu->reg_PC += 2;
            writeWord(addr, get_DE(cpu));
            return 16;
        }
        case 0x63: {
            uint16_t addr = readWord(cpu->reg_PC);
            cpu->reg_PC += 2;
            writeWord(addr, get_HL(cpu));
            return 16;
        }
        case 0x73: {
            uint16_t addr = readWord(cpu->reg_PC);
            cpu->reg_PC += 2;
            writeWord(addr, cpu->reg_SP);
            return 16;
        }
        case 0x4B: {
            uint16_t addr = readWord(cpu->reg_PC);
            cpu->reg_PC += 2;
            set_BC(cpu, readWord(addr));
            return 16;
        }
        case 0x5B: {
            uint16_t addr = readWord(cpu->reg_PC);
            cpu->reg_PC += 2;
            set_DE(cpu, readWord(addr));
            return 16;
        }
        case 0x6B: {
            uint16_t addr = readWord(cpu->reg_PC);
            cpu->reg_PC += 2;
            set_HL(cpu, readWord(addr));
            return 16;
        }
        case 0x7B: {
            uint16_t addr = readWord(cpu->reg_PC);
            cpu->reg_PC += 2;
            cpu->reg_SP = readWord(addr);
            return 16;
        }
        case 0xA0: {
            uint16_t hl = get_HL(cpu);
            uint8_t value = readByte(hl);
            uint16_t de = get_DE(cpu);
            writeByte(de, value);
            set_DE(cpu, (uint16_t)(de + 1));
            set_HL(cpu, (uint16_t)(hl + 1));
            uint16_t bc = (uint16_t)((get_BC(cpu) - 1) & 0xFFFF);
            set_BC(cpu, bc);
            uint8_t sum = (uint8_t)(cpu->reg_A + value);
            uint8_t preserved = cpu->reg_F & (uint8_t)(FLAG_S | FLAG_Z | FLAG_C);
            cpu->reg_F = preserved;
            set_flag(cpu, FLAG_H, 0);
            set_flag(cpu, FLAG_N, 0);
            set_flag(cpu, FLAG_PV, bc != 0);
            set_xy_flags(cpu, sum);
            return 12;
        }
        case 0xB0: {
            uint16_t hl = get_HL(cpu);
            uint8_t value = readByte(hl);
            uint16_t de = get_DE(cpu);
            writeByte(de, value);
            set_DE(cpu, (uint16_t)(de + 1));
            set_HL(cpu, (uint16_t)(hl + 1));
            uint16_t bc = (uint16_t)((get_BC(cpu) - 1) & 0xFFFF);
            set_BC(cpu, bc);
            uint8_t sum = (uint8_t)(cpu->reg_A + value);
            uint8_t preserved = cpu->reg_F & (uint8_t)(FLAG_S | FLAG_Z | FLAG_C);
            cpu->reg_F = preserved;
            set_flag(cpu, FLAG_H, 0);
            set_flag(cpu, FLAG_N, 0);
            set_flag(cpu, FLAG_PV, bc != 0);
            set_xy_flags(cpu, sum);
            if (bc != 0) {
                cpu->reg_PC -= 2;
                return 17;
            }
            return 12;
        }
        case 0xA8: {
            uint16_t hl = get_HL(cpu);
            uint8_t value = readByte(hl);
            uint16_t de = get_DE(cpu);
            writeByte(de, value);
            set_DE(cpu, (uint16_t)(de - 1));
            set_HL(cpu, (uint16_t)(hl - 1));
            uint16_t bc = (uint16_t)((get_BC(cpu) - 1) & 0xFFFF);
            set_BC(cpu, bc);
            uint8_t sum = (uint8_t)(cpu->reg_A + value);
            uint8_t preserved = cpu->reg_F & (uint8_t)(FLAG_S | FLAG_Z | FLAG_C);
            cpu->reg_F = preserved;
            set_flag(cpu, FLAG_H, 0);
            set_flag(cpu, FLAG_N, 0);
            set_flag(cpu, FLAG_PV, bc != 0);
            set_xy_flags(cpu, sum);
            return 12;
        }
        case 0xB8: {
            uint16_t hl = get_HL(cpu);
            uint8_t value = readByte(hl);
            uint16_t de = get_DE(cpu);
            writeByte(de, value);
            set_DE(cpu, (uint16_t)(de - 1));
            set_HL(cpu, (uint16_t)(hl - 1));
            uint16_t bc = (uint16_t)((get_BC(cpu) - 1) & 0xFFFF);
            set_BC(cpu, bc);
            uint8_t sum = (uint8_t)(cpu->reg_A + value);
            uint8_t preserved = cpu->reg_F & (uint8_t)(FLAG_S | FLAG_Z | FLAG_C);
            cpu->reg_F = preserved;
            set_flag(cpu, FLAG_H, 0);
            set_flag(cpu, FLAG_N, 0);
            set_flag(cpu, FLAG_PV, bc != 0);
            set_xy_flags(cpu, sum);
            if (bc != 0) {
                cpu->reg_PC -= 2;
                return 17;
            }
            return 12;
        }
        case 0xA1: {
            uint16_t hl = get_HL(cpu);
            uint8_t value = readByte(hl);
            uint16_t bc = (uint16_t)((get_BC(cpu) - 1) & 0xFFFF);
            set_BC(cpu, bc);
            set_HL(cpu, (uint16_t)(hl + 1));
            uint8_t diff = (uint8_t)(cpu->reg_A - value);
            uint8_t half = (uint8_t)((cpu->reg_A & 0x0F) < (value & 0x0F));
            set_flag(cpu, FLAG_S, diff & 0x80);
            set_flag(cpu, FLAG_Z, diff == 0);
            set_flag(cpu, FLAG_H, half);
            set_flag(cpu, FLAG_PV, bc != 0);
            set_flag(cpu, FLAG_N, 1);
            set_xy_flags(cpu, (uint8_t)(diff - (half ? 1 : 0)));
            return 12;
        }
        case 0xB1: {
            uint16_t hl = get_HL(cpu);
            uint8_t value = readByte(hl);
            uint16_t bc = (uint16_t)((get_BC(cpu) - 1) & 0xFFFF);
            set_BC(cpu, bc);
            set_HL(cpu, (uint16_t)(hl + 1));
            uint8_t diff = (uint8_t)(cpu->reg_A - value);
            uint8_t half = (uint8_t)((cpu->reg_A & 0x0F) < (value & 0x0F));
            set_flag(cpu, FLAG_S, diff & 0x80);
            set_flag(cpu, FLAG_Z, diff == 0);
            set_flag(cpu, FLAG_H, half);
            set_flag(cpu, FLAG_PV, bc != 0);
            set_flag(cpu, FLAG_N, 1);
            set_xy_flags(cpu, (uint8_t)(diff - (half ? 1 : 0)));
            if (bc != 0 && diff != 0) {
                cpu->reg_PC -= 2;
                return 17;
            }
            return 12;
        }
        case 0xA9: {
            uint16_t hl = get_HL(cpu);
            uint8_t value = readByte(hl);
            uint16_t bc = (uint16_t)((get_BC(cpu) - 1) & 0xFFFF);
            set_BC(cpu, bc);
            set_HL(cpu, (uint16_t)(hl - 1));
            uint8_t diff = (uint8_t)(cpu->reg_A - value);
            uint8_t half = (uint8_t)((cpu->reg_A & 0x0F) < (value & 0x0F));
            set_flag(cpu, FLAG_S, diff & 0x80);
            set_flag(cpu, FLAG_Z, diff == 0);
            set_flag(cpu, FLAG_H, half);
            set_flag(cpu, FLAG_PV, bc != 0);
            set_flag(cpu, FLAG_N, 1);
            set_xy_flags(cpu, (uint8_t)(diff - (half ? 1 : 0)));
            return 12;
        }
        case 0xB9: {
            uint16_t hl = get_HL(cpu);
            uint8_t value = readByte(hl);
            uint16_t bc = (uint16_t)((get_BC(cpu) - 1) & 0xFFFF);
            set_BC(cpu, bc);
            set_HL(cpu, (uint16_t)(hl - 1));
            uint8_t diff = (uint8_t)(cpu->reg_A - value);
            uint8_t half = (uint8_t)((cpu->reg_A & 0x0F) < (value & 0x0F));
            set_flag(cpu, FLAG_S, diff & 0x80);
            set_flag(cpu, FLAG_Z, diff == 0);
            set_flag(cpu, FLAG_H, half);
            set_flag(cpu, FLAG_PV, bc != 0);
            set_flag(cpu, FLAG_N, 1);
            set_xy_flags(cpu, (uint8_t)(diff - (half ? 1 : 0)));
            if (bc != 0 && diff != 0) {
                cpu->reg_PC -= 2;
                return 17;
            }
            return 12;
        }
        case 0x44: case 0x4C: case 0x54: case 0x5C: case 0x64: case 0x6C: case 0x74: case 0x7C: {
            uint8_t a = cpu->reg_A;
            cpu->reg_A = 0;
            cpu_sub(cpu, a, 1);
            return 4;
        }
        case 0x47: cpu->reg_I = cpu->reg_A; return 5;
        case 0x4F: cpu->reg_R = cpu->reg_A; return 5;
        case 0x57:
            cpu->reg_A = cpu->reg_I;
            set_flags_szp(cpu, cpu->reg_A);
            set_flag(cpu, FLAG_H, 0);
            set_flag(cpu, FLAG_N, 0);
            set_flag(cpu, FLAG_PV, cpu->iff2);
            return 5;
        case 0x5F:
            cpu->reg_A = cpu->reg_R;
            set_flags_szp(cpu, cpu->reg_A);
            set_flag(cpu, FLAG_H, 0);
            set_flag(cpu, FLAG_N, 0);
            set_flag(cpu, FLAG_PV, cpu->iff2);
            return 5;
        case 0x67: {
            uint16_t hl_addr = get_HL(cpu);
            uint8_t value = readByte(hl_addr);
            uint8_t new_mem = (uint8_t)(((cpu->reg_A & 0x0F) << 4) | (value >> 4));
            uint8_t new_a = (cpu->reg_A & 0xF0) | (value & 0x0F);
            writeByte(hl_addr, new_mem);
            cpu->reg_A = new_a;
            set_flags_szp(cpu, cpu->reg_A);
            set_flag(cpu, FLAG_H, 0);
            set_flag(cpu, FLAG_N, 0);
            return 14;
        }
        case 0x6F: {
            uint16_t hl_addr = get_HL(cpu);
            uint8_t value = readByte(hl_addr);
            uint8_t new_mem = (uint8_t)(((value << 4) & 0xF0) | (cpu->reg_A & 0x0F));
            uint8_t new_a = (cpu->reg_A & 0xF0) | ((value >> 4) & 0x0F);
            writeByte(hl_addr, new_mem);
            cpu->reg_A = new_a;
            set_flags_szp(cpu, cpu->reg_A);
            set_flag(cpu, FLAG_H, 0);
            set_flag(cpu, FLAG_N, 0);
            return 14;
        }
        case 0x45: case 0x55: case 0x5D: case 0x65: case 0x6D: case 0x75: case 0x7D:
            cpu->reg_PC = cpu_pop(cpu);
            cpu->iff1 = cpu->iff2;
            return 10;
        case 0x4D:
            cpu->reg_PC = cpu_pop(cpu);
            cpu->iff1 = cpu->iff2;
            return 10;
        case 0x46: case 0x4E: case 0x66: case 0x6E:
            cpu->interruptMode = 0;
            return 4;
        case 0x56: case 0x76:
            cpu->interruptMode = 1;
            return 4;
        case 0x5E: case 0x7E:
            cpu->interruptMode = 2;
            return 4;
        case 0x40: {
            uint8_t value = io_read(get_BC(cpu));
            cpu->reg_B = value;
            set_flags_szp(cpu, value);
            set_flag(cpu, FLAG_H, 1);
            set_flag(cpu, FLAG_N, 1);
            return 8;
        }
        case 0x48: {
            uint8_t value = io_read(get_BC(cpu));
            cpu->reg_C = value;
            set_flags_szp(cpu, value);
            set_flag(cpu, FLAG_H, 1);
            set_flag(cpu, FLAG_N, 1);
            return 8;
        }
        case 0x50: {
            uint8_t value = io_read(get_BC(cpu));
            cpu->reg_D = value;
            set_flags_szp(cpu, value);
            set_flag(cpu, FLAG_H, 1);
            set_flag(cpu, FLAG_N, 1);
            return 8;
        }
        case 0x58: {
            uint8_t value = io_read(get_BC(cpu));
            cpu->reg_E = value;
            set_flags_szp(cpu, value);
            set_flag(cpu, FLAG_H, 1);
            set_flag(cpu, FLAG_N, 1);
            return 8;
        }
        case 0x60: {
            uint8_t value = io_read(get_BC(cpu));
            cpu->reg_H = value;
            set_flags_szp(cpu, value);
            set_flag(cpu, FLAG_H, 1);
            set_flag(cpu, FLAG_N, 1);
            return 8;
        }
        case 0x68: {
            uint8_t value = io_read(get_BC(cpu));
            cpu->reg_L = value;
            set_flags_szp(cpu, value);
            set_flag(cpu, FLAG_H, 1);
            set_flag(cpu, FLAG_N, 1);
            return 8;
        }
        case 0x70: {
            uint8_t value = io_read(get_BC(cpu));
            set_flags_szp(cpu, value);
            set_flag(cpu, FLAG_H, 1);
            set_flag(cpu, FLAG_N, 1);
            return 8;
        }
        case 0x78: {
            uint8_t value = io_read(get_BC(cpu));
            cpu->reg_A = value;
            set_flags_szp(cpu, value);
            set_flag(cpu, FLAG_H, 1);
            set_flag(cpu, FLAG_N, 1);
            return 8;
        }
        case 0x41: io_write(get_BC(cpu), cpu->reg_B); return 8;
        case 0x49: io_write(get_BC(cpu), cpu->reg_C); return 8;
        case 0x51: io_write(get_BC(cpu), cpu->reg_D); return 8;
        case 0x59: io_write(get_BC(cpu), cpu->reg_E); return 8;
        case 0x61: io_write(get_BC(cpu), cpu->reg_H); return 8;
        case 0x69: io_write(get_BC(cpu), cpu->reg_L); return 8;
        case 0x71: io_write(get_BC(cpu), 0); return 8;
        case 0x79: io_write(get_BC(cpu), cpu->reg_A); return 8;
        case 0xA2: writeByte(get_HL(cpu), io_read(get_BC(cpu))); cpu->reg_B--; set_HL(cpu, get_HL(cpu) + 1); return 8;
        case 0xB2: writeByte(get_HL(cpu), io_read(get_BC(cpu))); cpu->reg_B--; set_HL(cpu, get_HL(cpu) + 1); if (cpu->reg_B != 0) { cpu->reg_PC -= 2; return 13; } return 8;
        case 0xAA: writeByte(get_HL(cpu), io_read(get_BC(cpu))); cpu->reg_B--; set_HL(cpu, get_HL(cpu) - 1); return 8;
        case 0xBA: writeByte(get_HL(cpu), io_read(get_BC(cpu))); cpu->reg_B--; set_HL(cpu, get_HL(cpu) - 1); if (cpu->reg_B != 0) { cpu->reg_PC -= 2; return 13; } return 8;
        case 0xA3: io_write(get_BC(cpu), readByte(get_HL(cpu))); cpu->reg_B--; set_HL(cpu, get_HL(cpu) + 1); return 8;
        case 0xB3: io_write(get_BC(cpu), readByte(get_HL(cpu))); cpu->reg_B--; set_HL(cpu, get_HL(cpu) + 1); if (cpu->reg_B != 0) { cpu->reg_PC -= 2; return 13; } return 8;
        case 0xAB: io_write(get_BC(cpu), readByte(get_HL(cpu))); cpu->reg_B--; set_HL(cpu, get_HL(cpu) - 1); return 8;
        case 0xBB: io_write(get_BC(cpu), readByte(get_HL(cpu))); cpu->reg_B--; set_HL(cpu, get_HL(cpu) - 1); if (cpu->reg_B != 0) { cpu->reg_PC -= 2; return 13; } return 8;
        default:
            return 4;
    }
}

int cpu_ddfd_cb_step(Z80* cpu, uint16_t* index_reg, int is_ix) {
    int8_t d=(int8_t)readByte(cpu->reg_PC++); uint8_t op=readByte(cpu->reg_PC++);
    uint16_t addr=(uint16_t)(*index_reg + d);
    uint8_t x=(op>>6)&3; uint8_t y=(op>>3)&7; uint8_t z=op&7;
    uint8_t operand=readByte(addr); uint8_t result=operand;
    switch(x){
        case 0: switch(y){ case 0:result=cpu_rlc(cpu,operand);break; case 1:result=cpu_rrc(cpu,operand);break; case 2:result=cpu_rl(cpu,operand);break; case 3:result=cpu_rr(cpu,operand);break; case 4:result=cpu_sla(cpu,operand);break; case 5:result=cpu_sra(cpu,operand);break; case 6:result=cpu_sll(cpu,operand);break; case 7:result=cpu_srl(cpu,operand);break; } break;
        case 1: cpu_bit(cpu,operand,y); return 12;
        case 2: result=(uint8_t)(operand&~(1<<y)); break; case 3: result=(uint8_t)(operand|(1<<y)); break;
    }
    if (x != 1) {
        writeByte(addr, result);
    }
    if(z==6){ return 15; }
    switch(z){
        case 0:cpu->reg_B=result;break; case 1:cpu->reg_C=result;break; case 2:cpu->reg_D=result;break; case 3:cpu->reg_E=result;break;
        case 4: if(is_ix) set_IXh(cpu,result); else set_IYh(cpu,result); break;
        case 5: if(is_ix) set_IXl(cpu,result); else set_IYl(cpu,result); break;
        case 7: cpu->reg_A=result; break;
        default: break;
    }
    return 12;
}

// --- Handle Maskable Interrupt ---
int cpu_interrupt(Z80* cpu, uint8_t data_bus) {
    if (cpu->halted) {
        cpu->reg_PC++; // Leave HALT state
        cpu->halted = 0;
    }
    cpu->iff1 = cpu->iff2 = 0; // Disable interrupts
    cpu->reg_R = (cpu->reg_R + 1) | (cpu->reg_R & 0x80);

    uint16_t vector;
    int t_states = 13;
    switch (cpu->interruptMode) {
        case 0: // Treat IM 0 like IM 1 for Spectrum ULA
        case 1:
            vector = 0x0038;
            break;
        case 2: {
            uint16_t table_addr = (uint16_t)(((uint16_t)cpu->reg_I << 8) | data_bus);
            vector = readWord(table_addr);
            t_states = 19;
            break;
        }
        default:
            vector = 0x0038;
            break;
    }

    cpu_push(cpu, cpu->reg_PC); // Push current PC
    cpu->reg_PC = vector;       // Jump to handler
    return t_states;
}

// --- The Main CPU Execution Step ---
int cpu_step(Z80* cpu) { // Returns T-states
    ula_instruction_progress_ptr = NULL;
    if (cpu->ei_delay) { cpu->iff1 = cpu->iff2 = 1; cpu->ei_delay = 0; }
    if (cpu->halted) { cpu->reg_R = (cpu->reg_R+1)|(cpu->reg_R&0x80); return 4; }

    int prefix=0;
    int t_states = 0;
    ula_instruction_base_tstate = total_t_states;
    ula_instruction_progress_ptr = &t_states;
    cpu->reg_R=(cpu->reg_R+1)|(cpu->reg_R&0x80);
    uint8_t opcode=readByte(cpu->reg_PC++);
    t_states += 4;
    
    if(opcode==0xDD){prefix=1;opcode=readByte(cpu->reg_PC++);cpu->reg_R++;t_states+=4;}
    else if(opcode==0xFD){prefix=2;opcode=readByte(cpu->reg_PC++);cpu->reg_R++;t_states+=4;}
    while(opcode==0xDD||opcode==0xFD){prefix=(opcode==0xDD)?1:2;opcode=readByte(cpu->reg_PC++);cpu->reg_R++;t_states+=4;}

    switch (opcode) {
        case 0x00: break;
        case 0x06: cpu->reg_B=readByte(cpu->reg_PC++); t_states+=3; break; case 0x0E: cpu->reg_C=readByte(cpu->reg_PC++); t_states+=3; break;
        case 0x16: cpu->reg_D=readByte(cpu->reg_PC++); t_states+=3; break; case 0x1E: cpu->reg_E=readByte(cpu->reg_PC++); t_states+=3; break;
        case 0x26: if(prefix==1){set_IXh(cpu,readByte(cpu->reg_PC++));t_states+=7;}else if(prefix==2){set_IYh(cpu,readByte(cpu->reg_PC++));t_states+=7;}else{cpu->reg_H=readByte(cpu->reg_PC++);t_states+=3;} break;
        case 0x2E: if(prefix==1){set_IXl(cpu,readByte(cpu->reg_PC++));t_states+=7;}else if(prefix==2){set_IYl(cpu,readByte(cpu->reg_PC++));t_states+=7;}else{cpu->reg_L=readByte(cpu->reg_PC++);t_states+=3;} break;
        case 0x3E: cpu->reg_A=readByte(cpu->reg_PC++); t_states+=3; break;
        case 0x44: if(prefix==1)cpu->reg_B=get_IXh(cpu);else if(prefix==2)cpu->reg_B=get_IYh(cpu);else cpu->reg_B=cpu->reg_H; if(prefix)t_states+=4; break;
        case 0x45: if(prefix==1)cpu->reg_B=get_IXl(cpu);else if(prefix==2)cpu->reg_B=get_IYl(cpu);else cpu->reg_B=cpu->reg_L; if(prefix)t_states+=4; break;
        case 0x4C: if(prefix==1)cpu->reg_C=get_IXh(cpu);else if(prefix==2)cpu->reg_C=get_IYh(cpu);else cpu->reg_C=cpu->reg_H; if(prefix)t_states+=4; break;
        case 0x4D: if(prefix==1)cpu->reg_C=get_IXl(cpu);else if(prefix==2)cpu->reg_C=get_IYl(cpu);else cpu->reg_C=cpu->reg_L; if(prefix)t_states+=4; break;
        case 0x54: if(prefix==1)cpu->reg_D=get_IXh(cpu);else if(prefix==2)cpu->reg_D=get_IYh(cpu);else cpu->reg_D=cpu->reg_H; if(prefix)t_states+=4; break;
        case 0x55: if(prefix==1)cpu->reg_D=get_IXl(cpu);else if(prefix==2)cpu->reg_D=get_IYl(cpu);else cpu->reg_D=cpu->reg_L; if(prefix)t_states+=4; break;
        case 0x5C: if(prefix==1)cpu->reg_E=get_IXh(cpu);else if(prefix==2)cpu->reg_E=get_IYh(cpu);else cpu->reg_E=cpu->reg_H; if(prefix)t_states+=4; break;
        case 0x5D: if(prefix==1)cpu->reg_E=get_IXl(cpu);else if(prefix==2)cpu->reg_E=get_IYl(cpu);else cpu->reg_E=cpu->reg_L; if(prefix)t_states+=4; break;
        case 0x60: if(prefix==1)set_IXh(cpu,cpu->reg_B);else if(prefix==2)set_IYh(cpu,cpu->reg_B);else cpu->reg_H=cpu->reg_B; if(prefix)t_states+=4; break;
        case 0x61: if(prefix==1)set_IXh(cpu,cpu->reg_C);else if(prefix==2)set_IYh(cpu,cpu->reg_C);else cpu->reg_H=cpu->reg_C; if(prefix)t_states+=4; break;
        case 0x62: if(prefix==1)set_IXh(cpu,cpu->reg_D);else if(prefix==2)set_IYh(cpu,cpu->reg_D);else cpu->reg_H=cpu->reg_D; if(prefix)t_states+=4; break;
        case 0x63: if(prefix==1)set_IXh(cpu,cpu->reg_E);else if(prefix==2)set_IYh(cpu,cpu->reg_E);else cpu->reg_H=cpu->reg_E; if(prefix)t_states+=4; break;
        case 0x64: if(prefix==1)set_IXh(cpu,get_IXh(cpu));else if(prefix==2)set_IYh(cpu,get_IYh(cpu));else cpu->reg_H=cpu->reg_H; if(prefix)t_states+=4; break;
        case 0x65: if(prefix==1)set_IXh(cpu,get_IXl(cpu));else if(prefix==2)set_IYh(cpu,get_IYl(cpu));else cpu->reg_H=cpu->reg_L; if(prefix)t_states+=4; break;
        case 0x67: if(prefix==1)set_IXh(cpu,cpu->reg_A);else if(prefix==2)set_IYh(cpu,cpu->reg_A);else cpu->reg_H=cpu->reg_A; if(prefix)t_states+=4; break;
        case 0x68: if(prefix==1)set_IXl(cpu,cpu->reg_B);else if(prefix==2)set_IYl(cpu,cpu->reg_B);else cpu->reg_L=cpu->reg_B; if(prefix)t_states+=4; break;
        case 0x69: if(prefix==1)set_IXl(cpu,cpu->reg_C);else if(prefix==2)set_IYl(cpu,cpu->reg_C);else cpu->reg_L=cpu->reg_C; if(prefix)t_states+=4; break;
        case 0x6A: if(prefix==1)set_IXl(cpu,cpu->reg_D);else if(prefix==2)set_IYl(cpu,cpu->reg_D);else cpu->reg_L=cpu->reg_D; if(prefix)t_states+=4; break;
        case 0x6B: if(prefix==1)set_IXl(cpu,cpu->reg_E);else if(prefix==2)set_IYl(cpu,cpu->reg_E);else cpu->reg_L=cpu->reg_E; if(prefix)t_states+=4; break;
        case 0x6C: if(prefix==1)set_IXl(cpu,get_IXh(cpu));else if(prefix==2)set_IYl(cpu,get_IYh(cpu));else cpu->reg_L=cpu->reg_H; if(prefix)t_states+=4; break;
        case 0x6D: if(prefix==1)set_IXl(cpu,get_IXl(cpu));else if(prefix==2)set_IYl(cpu,get_IYl(cpu));else cpu->reg_L=cpu->reg_L; if(prefix)t_states+=4; break;
        case 0x6F: if(prefix==1)set_IXl(cpu,cpu->reg_A);else if(prefix==2)set_IYl(cpu,cpu->reg_A);else cpu->reg_L=cpu->reg_A; if(prefix)t_states+=4; break;
        case 0x7C: if(prefix==1)cpu->reg_A=get_IXh(cpu);else if(prefix==2)cpu->reg_A=get_IYh(cpu);else cpu->reg_A=cpu->reg_H; if(prefix)t_states+=4; break;
        case 0x7D: if(prefix==1)cpu->reg_A=get_IXl(cpu);else if(prefix==2)cpu->reg_A=get_IYl(cpu);else cpu->reg_A=cpu->reg_L; if(prefix)t_states+=4; break;
        case 0x40:break; case 0x41:cpu->reg_B=cpu->reg_C;break; case 0x42:cpu->reg_B=cpu->reg_D;break; case 0x43:cpu->reg_B=cpu->reg_E;break; case 0x47:cpu->reg_B=cpu->reg_A;break;
        case 0x48:cpu->reg_C=cpu->reg_B;break; case 0x49:break; case 0x4A:cpu->reg_C=cpu->reg_D;break; case 0x4B:cpu->reg_C=cpu->reg_E;break; case 0x4F:cpu->reg_C=cpu->reg_A;break;
        case 0x50:cpu->reg_D=cpu->reg_B;break; case 0x51:cpu->reg_D=cpu->reg_C;break; case 0x52:break; case 0x53:cpu->reg_D=cpu->reg_E;break; case 0x57:cpu->reg_D=cpu->reg_A;break;
        case 0x58:cpu->reg_E=cpu->reg_B;break; case 0x59:cpu->reg_E=cpu->reg_C;break; case 0x5A:cpu->reg_E=cpu->reg_D;break; case 0x5B:break; case 0x5F:cpu->reg_E=cpu->reg_A;break;
        case 0x78:cpu->reg_A=cpu->reg_B;break; case 0x79:cpu->reg_A=cpu->reg_C;break; case 0x7A:cpu->reg_A=cpu->reg_D;break; case 0x7B:cpu->reg_A=cpu->reg_E;break; case 0x7F:break;
        case 0x46: { if(prefix){int8_t d=(int8_t)readByte(cpu->reg_PC++);cpu->reg_B=readByte((prefix==1?cpu->reg_IX:cpu->reg_IY)+d); t_states+=15;} else{cpu->reg_B=readByte(get_HL(cpu)); t_states+=3;} break; }
        case 0x4E: { if(prefix){int8_t d=(int8_t)readByte(cpu->reg_PC++);cpu->reg_C=readByte((prefix==1?cpu->reg_IX:cpu->reg_IY)+d); t_states+=15;} else{cpu->reg_C=readByte(get_HL(cpu)); t_states+=3;} break; }
        case 0x56: { if(prefix){int8_t d=(int8_t)readByte(cpu->reg_PC++);cpu->reg_D=readByte((prefix==1?cpu->reg_IX:cpu->reg_IY)+d); t_states+=15;} else{cpu->reg_D=readByte(get_HL(cpu)); t_states+=3;} break; }
        case 0x5E: { if(prefix){int8_t d=(int8_t)readByte(cpu->reg_PC++);cpu->reg_E=readByte((prefix==1?cpu->reg_IX:cpu->reg_IY)+d); t_states+=15;} else{cpu->reg_E=readByte(get_HL(cpu)); t_states+=3;} break; }
        case 0x66: { if(prefix){int8_t d=(int8_t)readByte(cpu->reg_PC++);cpu->reg_H=readByte((prefix==1?cpu->reg_IX:cpu->reg_IY)+d); t_states+=15;} else{cpu->reg_H=readByte(get_HL(cpu)); t_states+=3;} break; }
        case 0x6E: { if(prefix){int8_t d=(int8_t)readByte(cpu->reg_PC++);cpu->reg_L=readByte((prefix==1?cpu->reg_IX:cpu->reg_IY)+d); t_states+=15;} else{cpu->reg_L=readByte(get_HL(cpu)); t_states+=3;} break; }
        case 0x7E: { if(prefix){int8_t d=(int8_t)readByte(cpu->reg_PC++);cpu->reg_A=readByte((prefix==1?cpu->reg_IX:cpu->reg_IY)+d); t_states+=15;} else{cpu->reg_A=readByte(get_HL(cpu)); t_states+=3;} break; }
        case 0x70: { if(prefix){int8_t d=(int8_t)readByte(cpu->reg_PC++);writeByte((prefix==1?cpu->reg_IX:cpu->reg_IY)+d,cpu->reg_B); t_states+=15;} else{writeByte(get_HL(cpu),cpu->reg_B); t_states+=3;} break; }
        case 0x71: { if(prefix){int8_t d=(int8_t)readByte(cpu->reg_PC++);writeByte((prefix==1?cpu->reg_IX:cpu->reg_IY)+d,cpu->reg_C); t_states+=15;} else{writeByte(get_HL(cpu),cpu->reg_C); t_states+=3;} break; }
        case 0x72: { if(prefix){int8_t d=(int8_t)readByte(cpu->reg_PC++);writeByte((prefix==1?cpu->reg_IX:cpu->reg_IY)+d,cpu->reg_D); t_states+=15;} else{writeByte(get_HL(cpu),cpu->reg_D); t_states+=3;} break; }
        case 0x73: { if(prefix){int8_t d=(int8_t)readByte(cpu->reg_PC++);writeByte((prefix==1?cpu->reg_IX:cpu->reg_IY)+d,cpu->reg_E); t_states+=15;} else{writeByte(get_HL(cpu),cpu->reg_E); t_states+=3;} break; }
        case 0x74: { if(prefix){int8_t d=(int8_t)readByte(cpu->reg_PC++);writeByte((prefix==1?cpu->reg_IX:cpu->reg_IY)+d,cpu->reg_H); t_states+=15;} else{writeByte(get_HL(cpu),cpu->reg_H); t_states+=3;} break; }
        case 0x75: { if(prefix){int8_t d=(int8_t)readByte(cpu->reg_PC++);writeByte((prefix==1?cpu->reg_IX:cpu->reg_IY)+d,cpu->reg_L); t_states+=15;} else{writeByte(get_HL(cpu),cpu->reg_L); t_states+=3;} break; }
        case 0x77: { if(prefix){int8_t d=(int8_t)readByte(cpu->reg_PC++);writeByte((prefix==1?cpu->reg_IX:cpu->reg_IY)+d,cpu->reg_A); t_states+=15;} else{writeByte(get_HL(cpu),cpu->reg_A); t_states+=3;} break; }
        case 0x36: { if(prefix){int8_t d=(int8_t)readByte(cpu->reg_PC++);uint8_t n=readByte(cpu->reg_PC++);writeByte((prefix==1?cpu->reg_IX:cpu->reg_IY)+d,n);t_states+=15;} else{uint8_t n=readByte(cpu->reg_PC++);writeByte(get_HL(cpu),n);t_states+=6;} break; }
        case 0x0A: cpu->reg_A=readByte(get_BC(cpu)); t_states+=3; break; case 0x1A: cpu->reg_A=readByte(get_DE(cpu)); t_states+=3; break;
        case 0x02: writeByte(get_BC(cpu),cpu->reg_A); t_states+=3; break; case 0x12: writeByte(get_DE(cpu),cpu->reg_A); t_states+=3; break;
        case 0x3A: { uint16_t a=readWord(cpu->reg_PC);cpu->reg_PC+=2;cpu->reg_A=readByte(a); t_states+=9; break; }
        case 0x32: { uint16_t a=readWord(cpu->reg_PC);cpu->reg_PC+=2;writeByte(a,cpu->reg_A); t_states+=9; break; }
        case 0x84: if(prefix==1)cpu_add(cpu,get_IXh(cpu));else if(prefix==2)cpu_add(cpu,get_IYh(cpu));else cpu_add(cpu,cpu->reg_H); if(prefix)t_states+=4; break;
        case 0x85: if(prefix==1)cpu_add(cpu,get_IXl(cpu));else if(prefix==2)cpu_add(cpu,get_IYl(cpu));else cpu_add(cpu,cpu->reg_L); if(prefix)t_states+=4; break;
        case 0x86: { if(prefix){int8_t d=(int8_t)readByte(cpu->reg_PC++);cpu_add(cpu,readByte((prefix==1?cpu->reg_IX:cpu->reg_IY)+d));t_states+=15;} else{cpu_add(cpu,readByte(get_HL(cpu)));t_states+=3;} break; }
        case 0x80: cpu_add(cpu,cpu->reg_B);break; case 0x81: cpu_add(cpu,cpu->reg_C);break; case 0x82: cpu_add(cpu,cpu->reg_D);break; case 0x83: cpu_add(cpu,cpu->reg_E);break; case 0x87: cpu_add(cpu,cpu->reg_A);break;
        case 0x8C: if(prefix==1)cpu_adc(cpu,get_IXh(cpu));else if(prefix==2)cpu_adc(cpu,get_IYh(cpu));else cpu_adc(cpu,cpu->reg_H); if(prefix)t_states+=4; break;
        case 0x8D: if(prefix==1)cpu_adc(cpu,get_IXl(cpu));else if(prefix==2)cpu_adc(cpu,get_IYl(cpu));else cpu_adc(cpu,cpu->reg_L); if(prefix)t_states+=4; break;
        case 0x8E: { if(prefix){int8_t d=(int8_t)readByte(cpu->reg_PC++);cpu_adc(cpu,readByte((prefix==1?cpu->reg_IX:cpu->reg_IY)+d));t_states+=15;} else{cpu_adc(cpu,readByte(get_HL(cpu)));t_states+=3;} break; }
        case 0x88: cpu_adc(cpu,cpu->reg_B);break; case 0x89: cpu_adc(cpu,cpu->reg_C);break; case 0x8A: cpu_adc(cpu,cpu->reg_D);break; case 0x8B: cpu_adc(cpu,cpu->reg_E);break; case 0x8F: cpu_adc(cpu,cpu->reg_A);break;
        case 0x94: if(prefix==1)cpu_sub(cpu,get_IXh(cpu),1);else if(prefix==2)cpu_sub(cpu,get_IYh(cpu),1);else cpu_sub(cpu,cpu->reg_H,1); if(prefix)t_states+=4; break;
        case 0x95: if(prefix==1)cpu_sub(cpu,get_IXl(cpu),1);else if(prefix==2)cpu_sub(cpu,get_IYl(cpu),1);else cpu_sub(cpu,cpu->reg_L,1); if(prefix)t_states+=4; break;
        case 0x96: { if(prefix){int8_t d=(int8_t)readByte(cpu->reg_PC++);cpu_sub(cpu,readByte((prefix==1?cpu->reg_IX:cpu->reg_IY)+d),1);t_states+=15;} else{cpu_sub(cpu,readByte(get_HL(cpu)),1);t_states+=3;} break; }
        case 0x90: cpu_sub(cpu,cpu->reg_B,1);break; case 0x91: cpu_sub(cpu,cpu->reg_C,1);break; case 0x92: cpu_sub(cpu,cpu->reg_D,1);break; case 0x93: cpu_sub(cpu,cpu->reg_E,1);break; case 0x97: cpu_sub(cpu,cpu->reg_A,1);break;
        case 0x9C: if(prefix==1)cpu_sbc(cpu,get_IXh(cpu));else if(prefix==2)cpu_sbc(cpu,get_IYh(cpu));else cpu_sbc(cpu,cpu->reg_H); if(prefix)t_states+=4; break;
        case 0x9D: if(prefix==1)cpu_sbc(cpu,get_IXl(cpu));else if(prefix==2)cpu_sbc(cpu,get_IYl(cpu));else cpu_sbc(cpu,cpu->reg_L); if(prefix)t_states+=4; break;
        case 0x9E: { if(prefix){int8_t d=(int8_t)readByte(cpu->reg_PC++);cpu_sbc(cpu,readByte((prefix==1?cpu->reg_IX:cpu->reg_IY)+d));t_states+=15;} else{cpu_sbc(cpu,readByte(get_HL(cpu)));t_states+=3;} break; }
        case 0x98: cpu_sbc(cpu,cpu->reg_B);break; case 0x99: cpu_sbc(cpu,cpu->reg_C);break; case 0x9A: cpu_sbc(cpu,cpu->reg_D);break; case 0x9B: cpu_sbc(cpu,cpu->reg_E);break; case 0x9F: cpu_sbc(cpu,cpu->reg_A);break;
        case 0xA4: if(prefix==1)cpu_and(cpu,get_IXh(cpu));else if(prefix==2)cpu_and(cpu,get_IYh(cpu));else cpu_and(cpu,cpu->reg_H); if(prefix)t_states+=4; break;
        case 0xA5: if(prefix==1)cpu_and(cpu,get_IXl(cpu));else if(prefix==2)cpu_and(cpu,get_IYl(cpu));else cpu_and(cpu,cpu->reg_L); if(prefix)t_states+=4; break;
        case 0xA6: { if(prefix){int8_t d=(int8_t)readByte(cpu->reg_PC++);cpu_and(cpu,readByte((prefix==1?cpu->reg_IX:cpu->reg_IY)+d));t_states+=15;} else{cpu_and(cpu,readByte(get_HL(cpu)));t_states+=3;} break; }
        case 0xA0: cpu_and(cpu,cpu->reg_B);break; case 0xA1: cpu_and(cpu,cpu->reg_C);break; case 0xA2: cpu_and(cpu,cpu->reg_D);break; case 0xA3: cpu_and(cpu,cpu->reg_E);break; case 0xA7: cpu_and(cpu,cpu->reg_A);break;
        case 0xAC: if(prefix==1)cpu_xor(cpu,get_IXh(cpu));else if(prefix==2)cpu_xor(cpu,get_IYh(cpu));else cpu_xor(cpu,cpu->reg_H); if(prefix)t_states+=4; break;
        case 0xAD: if(prefix==1)cpu_xor(cpu,get_IXl(cpu));else if(prefix==2)cpu_xor(cpu,get_IYl(cpu));else cpu_xor(cpu,cpu->reg_L); if(prefix)t_states+=4; break;
        case 0xAE: { if(prefix){int8_t d=(int8_t)readByte(cpu->reg_PC++);cpu_xor(cpu,readByte((prefix==1?cpu->reg_IX:cpu->reg_IY)+d));t_states+=15;} else{cpu_xor(cpu,readByte(get_HL(cpu)));t_states+=3;} break; }
        case 0xA8: cpu_xor(cpu,cpu->reg_B);break; case 0xA9: cpu_xor(cpu,cpu->reg_C);break; case 0xAA: cpu_xor(cpu,cpu->reg_D);break; case 0xAB: cpu_xor(cpu,cpu->reg_E);break; case 0xAF: cpu_xor(cpu,cpu->reg_A);break;
        case 0xB4: if(prefix==1)cpu_or(cpu,get_IXh(cpu));else if(prefix==2)cpu_or(cpu,get_IYh(cpu));else cpu_or(cpu,cpu->reg_H); if(prefix)t_states+=4; break;
        case 0xB5: if(prefix==1)cpu_or(cpu,get_IXl(cpu));else if(prefix==2)cpu_or(cpu,get_IYl(cpu));else cpu_or(cpu,cpu->reg_L); if(prefix)t_states+=4; break;
        case 0xB6: { if(prefix){int8_t d=(int8_t)readByte(cpu->reg_PC++);cpu_or(cpu,readByte((prefix==1?cpu->reg_IX:cpu->reg_IY)+d));t_states+=15;} else{cpu_or(cpu,readByte(get_HL(cpu)));t_states+=3;} break; }
        case 0xB0: cpu_or(cpu,cpu->reg_B);break; case 0xB1: cpu_or(cpu,cpu->reg_C);break; case 0xB2: cpu_or(cpu,cpu->reg_D);break; case 0xB3: cpu_or(cpu,cpu->reg_E);break; case 0xB7: cpu_or(cpu,cpu->reg_A);break;
        case 0xBC: if(prefix==1)cpu_sub(cpu,get_IXh(cpu),0);else if(prefix==2)cpu_sub(cpu,get_IYh(cpu),0);else cpu_sub(cpu,cpu->reg_H,0); if(prefix)t_states+=4; break;
        case 0xBD: if(prefix==1)cpu_sub(cpu,get_IXl(cpu),0);else if(prefix==2)cpu_sub(cpu,get_IYl(cpu),0);else cpu_sub(cpu,cpu->reg_L,0); if(prefix)t_states+=4; break;
        case 0xBE: { if(prefix){int8_t d=(int8_t)readByte(cpu->reg_PC++);cpu_sub(cpu,readByte((prefix==1?cpu->reg_IX:cpu->reg_IY)+d),0);t_states+=15;} else{cpu_sub(cpu,readByte(get_HL(cpu)),0);t_states+=3;} break; }
        case 0xB8: cpu_sub(cpu,cpu->reg_B,0);break; case 0xB9: cpu_sub(cpu,cpu->reg_C,0);break; case 0xBA: cpu_sub(cpu,cpu->reg_D,0);break; case 0xBB: cpu_sub(cpu,cpu->reg_E,0);break; case 0xBF: cpu_sub(cpu,cpu->reg_A,0);break;
        case 0xC6: cpu_add(cpu,readByte(cpu->reg_PC++)); t_states+=3; break; case 0xCE: cpu_adc(cpu,readByte(cpu->reg_PC++)); t_states+=3; break;
        case 0xD6: cpu_sub(cpu,readByte(cpu->reg_PC++),1); t_states+=3; break; case 0xDE: cpu_sbc(cpu,readByte(cpu->reg_PC++)); t_states+=3; break;
        case 0xE6: cpu_and(cpu,readByte(cpu->reg_PC++)); t_states+=3; break; case 0xF6: cpu_or(cpu,readByte(cpu->reg_PC++)); t_states+=3; break;
        case 0xEE: cpu_xor(cpu,readByte(cpu->reg_PC++)); t_states+=3; break; case 0xFE: cpu_sub(cpu,readByte(cpu->reg_PC++),0); t_states+=3; break;
        case 0x01: set_BC(cpu,readWord(cpu->reg_PC));cpu->reg_PC+=2; t_states+=6; break; case 0x11: set_DE(cpu,readWord(cpu->reg_PC));cpu->reg_PC+=2; t_states+=6; break;
        case 0x21: if(prefix==1){cpu->reg_IX=readWord(cpu->reg_PC);cpu->reg_PC+=2;t_states+=10;}else if(prefix==2){cpu->reg_IY=readWord(cpu->reg_PC);cpu->reg_PC+=2;t_states+=10;}else{set_HL(cpu,readWord(cpu->reg_PC));cpu->reg_PC+=2;t_states+=6;} break;
        case 0x31: cpu->reg_SP=readWord(cpu->reg_PC);cpu->reg_PC+=2; t_states+=6; break;
        case 0x09: { if(prefix==1)cpu_add_ixiy(cpu,&cpu->reg_IX,get_BC(cpu));else if(prefix==2)cpu_add_ixiy(cpu,&cpu->reg_IY,get_BC(cpu));else cpu_add_hl(cpu,get_BC(cpu)); t_states+=(prefix?11:7); break; }
        case 0x19: { if(prefix==1)cpu_add_ixiy(cpu,&cpu->reg_IX,get_DE(cpu));else if(prefix==2)cpu_add_ixiy(cpu,&cpu->reg_IY,get_DE(cpu));else cpu_add_hl(cpu,get_DE(cpu)); t_states+=(prefix?11:7); break; }
        case 0x29: { if(prefix==1)cpu_add_ixiy(cpu,&cpu->reg_IX,cpu->reg_IX);else if(prefix==2)cpu_add_ixiy(cpu,&cpu->reg_IY,cpu->reg_IY);else cpu_add_hl(cpu,get_HL(cpu)); t_states+=(prefix?11:7); break; }
        case 0x39: { if(prefix==1)cpu_add_ixiy(cpu,&cpu->reg_IX,cpu->reg_SP);else if(prefix==2)cpu_add_ixiy(cpu,&cpu->reg_IY,cpu->reg_SP);else cpu_add_hl(cpu,cpu->reg_SP); t_states+=(prefix?11:7); break; }
        case 0x03: set_BC(cpu,get_BC(cpu)+1); t_states+=2; break; case 0x13: set_DE(cpu,get_DE(cpu)+1); t_states+=2; break;
        case 0x23: if(prefix==1)cpu->reg_IX++;else if(prefix==2)cpu->reg_IY++;else set_HL(cpu,get_HL(cpu)+1); t_states+=(prefix?6:2); break;
        case 0x33: cpu->reg_SP++; t_states+=2; break;
        case 0x0B: set_BC(cpu,get_BC(cpu)-1); t_states+=2; break; case 0x1B: set_DE(cpu,get_DE(cpu)-1); t_states+=2; break;
        case 0x2B: if(prefix==1)cpu->reg_IX--;else if(prefix==2)cpu->reg_IY--;else set_HL(cpu,get_HL(cpu)-1); t_states+=(prefix?6:2); break;
        case 0x3B: cpu->reg_SP--; t_states+=2; break;
        case 0x22: { uint16_t a=readWord(cpu->reg_PC);cpu->reg_PC+=2;if(prefix==1)writeWord(a,cpu->reg_IX);else if(prefix==2)writeWord(a,cpu->reg_IY);else writeWord(a,get_HL(cpu)); t_states+=(prefix?16:12); break; }
        case 0x2A: { uint16_t a=readWord(cpu->reg_PC);cpu->reg_PC+=2;if(prefix==1)cpu->reg_IX=readWord(a);else if(prefix==2)cpu->reg_IY=readWord(a);else set_HL(cpu,readWord(a)); t_states+=(prefix?16:12); break; }
        case 0xC5: cpu_push(cpu,get_BC(cpu)); t_states+=7; break; case 0xD5: cpu_push(cpu,get_DE(cpu)); t_states+=7; break;
        case 0xE5: if(prefix==1)cpu_push(cpu,cpu->reg_IX);else if(prefix==2)cpu_push(cpu,cpu->reg_IY);else cpu_push(cpu,get_HL(cpu)); t_states+=(prefix?11:7); break;
        case 0xF5: cpu_push(cpu,get_AF(cpu)); t_states+=7; break;
        case 0xC1: set_BC(cpu,cpu_pop(cpu)); t_states+=6; break; case 0xD1: set_DE(cpu,cpu_pop(cpu)); t_states+=6; break;
        case 0xE1: if(prefix==1)cpu->reg_IX=cpu_pop(cpu);else if(prefix==2)cpu->reg_IY=cpu_pop(cpu);else set_HL(cpu,cpu_pop(cpu)); t_states+=(prefix?10:6); break;
        case 0xF1: set_AF(cpu,cpu_pop(cpu)); t_states+=6; break;
        case 0x08: { uint8_t tA=cpu->reg_A;uint8_t tF=cpu->reg_F;cpu->reg_A=cpu->alt_reg_A;cpu->reg_F=cpu->alt_reg_F;cpu->alt_reg_A=tA;cpu->alt_reg_F=tF; break; }
        case 0xD9: { uint8_t tB=cpu->reg_B;uint8_t tC=cpu->reg_C;cpu->reg_B=cpu->alt_reg_B;cpu->reg_C=cpu->alt_reg_C;cpu->alt_reg_B=tB;cpu->alt_reg_C=tC;uint8_t tD=cpu->reg_D;uint8_t tE=cpu->reg_E;cpu->reg_D=cpu->alt_reg_D;cpu->reg_E=cpu->alt_reg_E;cpu->alt_reg_D=tD;cpu->alt_reg_E=tE;uint8_t tH=cpu->reg_H;uint8_t tL=cpu->reg_L;cpu->reg_H=cpu->alt_reg_H;cpu->reg_L=cpu->alt_reg_L;cpu->alt_reg_H=tH;cpu->alt_reg_L=tL; break; }
        case 0xEB: { uint8_t tD=cpu->reg_D;uint8_t tE=cpu->reg_E;cpu->reg_D=cpu->reg_H;cpu->reg_E=cpu->reg_L;cpu->reg_H=tD;cpu->reg_L=tE; break; }
        case 0xC3: cpu->reg_PC=readWord(cpu->reg_PC); t_states+=6; break;
        case 0xE9: if(prefix==1)cpu->reg_PC=cpu->reg_IX;else if(prefix==2)cpu->reg_PC=cpu->reg_IY;else cpu->reg_PC=get_HL(cpu); if(prefix)t_states+=4; break;
        case 0xC2: if(!get_flag(cpu,FLAG_Z)){cpu->reg_PC=readWord(cpu->reg_PC);t_states+=6;} else {cpu->reg_PC+=2;t_states+=6;} break;
        case 0xCA: if( get_flag(cpu,FLAG_Z)){cpu->reg_PC=readWord(cpu->reg_PC);t_states+=6;} else {cpu->reg_PC+=2;t_states+=6;} break;
        case 0xD2: if(!get_flag(cpu,FLAG_C)){cpu->reg_PC=readWord(cpu->reg_PC);t_states+=6;} else {cpu->reg_PC+=2;t_states+=6;} break;
        case 0xDA: if( get_flag(cpu,FLAG_C)){cpu->reg_PC=readWord(cpu->reg_PC);t_states+=6;} else {cpu->reg_PC+=2;t_states+=6;} break;
        case 0xE2: if(!get_flag(cpu,FLAG_PV)){cpu->reg_PC=readWord(cpu->reg_PC);t_states+=6;} else {cpu->reg_PC+=2;t_states+=6;} break;
        case 0xEA: if( get_flag(cpu,FLAG_PV)){cpu->reg_PC=readWord(cpu->reg_PC);t_states+=6;} else {cpu->reg_PC+=2;t_states+=6;} break;
        case 0xF2: if(!get_flag(cpu,FLAG_S)){cpu->reg_PC=readWord(cpu->reg_PC);t_states+=6;} else {cpu->reg_PC+=2;t_states+=6;} break;
        case 0xFA: if( get_flag(cpu,FLAG_S)){cpu->reg_PC=readWord(cpu->reg_PC);t_states+=6;} else {cpu->reg_PC+=2;t_states+=6;} break;
        case 0x18: { int8_t o=(int8_t)readByte(cpu->reg_PC++);cpu->reg_PC+=o; t_states+=8; break; }
        case 0x10: { int8_t o=(int8_t)readByte(cpu->reg_PC++);cpu->reg_B--;if(cpu->reg_B!=0){cpu->reg_PC+=o;t_states+=9;}else{t_states+=4;} break; } // DJNZ
        case 0x20: { int8_t o=(int8_t)readByte(cpu->reg_PC++);if(!get_flag(cpu,FLAG_Z)){cpu->reg_PC+=o;t_states+=8;}else{t_states+=3;} break; }
        case 0x28: { int8_t o=(int8_t)readByte(cpu->reg_PC++);if(get_flag(cpu,FLAG_Z)){cpu->reg_PC+=o;t_states+=8;}else{t_states+=3;} break; }
        case 0x30: { int8_t o=(int8_t)readByte(cpu->reg_PC++);if(!get_flag(cpu,FLAG_C)){cpu->reg_PC+=o;t_states+=8;}else{t_states+=3;} break; }
        case 0x38: { int8_t o=(int8_t)readByte(cpu->reg_PC++);if(get_flag(cpu,FLAG_C)){cpu->reg_PC+=o;t_states+=8;}else{t_states+=3;} break; }
        case 0xCD: { uint16_t a=readWord(cpu->reg_PC);cpu_push(cpu,cpu->reg_PC+2);cpu->reg_PC=a; t_states+=13; break; }
        case 0xC9: cpu->reg_PC=cpu_pop(cpu); t_states+=6; break;
        case 0xC4: if(!get_flag(cpu,FLAG_Z)){uint16_t a=readWord(cpu->reg_PC);cpu_push(cpu,cpu->reg_PC+2);cpu->reg_PC=a;t_states+=13;}else{cpu->reg_PC+=2;t_states+=7;} break;
        case 0xCC: if(get_flag(cpu,FLAG_Z)){uint16_t a=readWord(cpu->reg_PC);cpu_push(cpu,cpu->reg_PC+2);cpu->reg_PC=a;t_states+=13;}else{cpu->reg_PC+=2;t_states+=7;} break;
        case 0xD4: if(!get_flag(cpu,FLAG_C)){uint16_t a=readWord(cpu->reg_PC);cpu_push(cpu,cpu->reg_PC+2);cpu->reg_PC=a;t_states+=13;}else{cpu->reg_PC+=2;t_states+=7;} break;
        case 0xDC: if(get_flag(cpu,FLAG_C)){uint16_t a=readWord(cpu->reg_PC);cpu_push(cpu,cpu->reg_PC+2);cpu->reg_PC=a;t_states+=13;}else{cpu->reg_PC+=2;t_states+=7;} break;
        case 0xE4: if(!get_flag(cpu,FLAG_PV)){uint16_t a=readWord(cpu->reg_PC);cpu_push(cpu,cpu->reg_PC+2);cpu->reg_PC=a;t_states+=13;}else{cpu->reg_PC+=2;t_states+=7;} break;
        case 0xEC: if(get_flag(cpu,FLAG_PV)){uint16_t a=readWord(cpu->reg_PC);cpu_push(cpu,cpu->reg_PC+2);cpu->reg_PC=a;t_states+=13;}else{cpu->reg_PC+=2;t_states+=7;} break;
        case 0xF4: if(!get_flag(cpu,FLAG_S)){uint16_t a=readWord(cpu->reg_PC);cpu_push(cpu,cpu->reg_PC+2);cpu->reg_PC=a;t_states+=13;}else{cpu->reg_PC+=2;t_states+=7;} break;
        case 0xFC: if(get_flag(cpu,FLAG_S)){uint16_t a=readWord(cpu->reg_PC);cpu_push(cpu,cpu->reg_PC+2);cpu->reg_PC=a;t_states+=13;}else{cpu->reg_PC+=2;t_states+=7;} break;
        case 0xC0: if(!get_flag(cpu,FLAG_Z)){cpu->reg_PC=cpu_pop(cpu);t_states+=7;}else{t_states+=1;} break;
        case 0xC8: if(get_flag(cpu,FLAG_Z)){cpu->reg_PC=cpu_pop(cpu);t_states+=7;}else{t_states+=1;} break;
        case 0xD0: if(!get_flag(cpu,FLAG_C)){cpu->reg_PC=cpu_pop(cpu);t_states+=7;}else{t_states+=1;} break;
        case 0xD8: if(get_flag(cpu,FLAG_C)){cpu->reg_PC=cpu_pop(cpu);t_states+=7;}else{t_states+=1;} break;
        case 0xE0: if(!get_flag(cpu,FLAG_PV)){cpu->reg_PC=cpu_pop(cpu);t_states+=7;}else{t_states+=1;} break;
        case 0xE8: if(get_flag(cpu,FLAG_PV)){cpu->reg_PC=cpu_pop(cpu);t_states+=7;}else{t_states+=1;} break;
        case 0xF0: if(!get_flag(cpu,FLAG_S)){cpu->reg_PC=cpu_pop(cpu);t_states+=7;}else{t_states+=1;} break;
        case 0xF8: if(get_flag(cpu,FLAG_S)){cpu->reg_PC=cpu_pop(cpu);t_states+=7;}else{t_states+=1;} break;
        case 0xC7: cpu_push(cpu,cpu->reg_PC);cpu->reg_PC=0x00; t_states+=7; break; case 0xCF: cpu_push(cpu,cpu->reg_PC);cpu->reg_PC=0x08; t_states+=7; break;
        case 0xD7: cpu_push(cpu,cpu->reg_PC);cpu->reg_PC=0x10; t_states+=7; break; case 0xDF: cpu_push(cpu,cpu->reg_PC);cpu->reg_PC=0x18; t_states+=7; break;
        case 0xE7: cpu_push(cpu,cpu->reg_PC);cpu->reg_PC=0x20; t_states+=7; break; case 0xEF: cpu_push(cpu,cpu->reg_PC);cpu->reg_PC=0x28; t_states+=7; break;
        case 0xF7: cpu_push(cpu,cpu->reg_PC);cpu->reg_PC=0x30; t_states+=7; break; case 0xFF: cpu_push(cpu,cpu->reg_PC);cpu->reg_PC=0x38; t_states+=7; break;
        case 0x04: cpu->reg_B=cpu_inc(cpu,cpu->reg_B);break; case 0x0C: cpu->reg_C=cpu_inc(cpu,cpu->reg_C);break; case 0x14: cpu->reg_D=cpu_inc(cpu,cpu->reg_D);break; case 0x1C: cpu->reg_E=cpu_inc(cpu,cpu->reg_E);break;
        case 0x24: if(prefix==1){set_IXh(cpu,cpu_inc(cpu,get_IXh(cpu)));t_states+=4;}else if(prefix==2){set_IYh(cpu,cpu_inc(cpu,get_IYh(cpu)));t_states+=4;}else{cpu->reg_H=cpu_inc(cpu,cpu->reg_H);}break;
        case 0x2C: if(prefix==1){set_IXl(cpu,cpu_inc(cpu,get_IXl(cpu)));t_states+=4;}else if(prefix==2){set_IYl(cpu,cpu_inc(cpu,get_IYl(cpu)));t_states+=4;}else{cpu->reg_L=cpu_inc(cpu,cpu->reg_L);}break;
        case 0x3C: cpu->reg_A=cpu_inc(cpu,cpu->reg_A);break; case 0x34: { if(prefix){int8_t d=(int8_t)readByte(cpu->reg_PC++);uint16_t a=(prefix==1?cpu->reg_IX:cpu->reg_IY)+d;writeByte(a,cpu_inc(cpu,readByte(a)));t_states+=19;}else{writeByte(get_HL(cpu),cpu_inc(cpu,readByte(get_HL(cpu))));t_states+=7;}break; }
        case 0x05: cpu->reg_B=cpu_dec(cpu,cpu->reg_B);break; case 0x0D: cpu->reg_C=cpu_dec(cpu,cpu->reg_C);break; case 0x15: cpu->reg_D=cpu_dec(cpu,cpu->reg_D);break; case 0x1D: cpu->reg_E=cpu_dec(cpu,cpu->reg_E);break;
        case 0x25: if(prefix==1){set_IXh(cpu,cpu_dec(cpu,get_IXh(cpu)));t_states+=4;}else if(prefix==2){set_IYh(cpu,cpu_dec(cpu,get_IYh(cpu)));t_states+=4;}else{cpu->reg_H=cpu_dec(cpu,cpu->reg_H);}break;
        case 0x2D: if(prefix==1){set_IXl(cpu,cpu_dec(cpu,get_IXl(cpu)));t_states+=4;}else if(prefix==2){set_IYl(cpu,cpu_dec(cpu,get_IYl(cpu)));t_states+=4;}else{cpu->reg_L=cpu_dec(cpu,cpu->reg_L);}break;
        case 0x3D: cpu->reg_A=cpu_dec(cpu,cpu->reg_A);break; case 0x35: { if(prefix){int8_t d=(int8_t)readByte(cpu->reg_PC++);uint16_t a=(prefix==1?cpu->reg_IX:cpu->reg_IY)+d;writeByte(a,cpu_dec(cpu,readByte(a)));t_states+=19;}else{writeByte(get_HL(cpu),cpu_dec(cpu,readByte(get_HL(cpu))));t_states+=7;}break; }
        case 0x07: { uint8_t c=(cpu->reg_A&0x80)?1:0;cpu->reg_A=(cpu->reg_A<<1)|c;set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,c);break; }
        case 0x0F: { uint8_t c=(cpu->reg_A&0x01);cpu->reg_A=(cpu->reg_A>>1)|(c<<7);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,c);break; }
        case 0x17: { uint8_t oc=get_flag(cpu,FLAG_C);uint8_t nc=(cpu->reg_A&0x80)?1:0;cpu->reg_A=(cpu->reg_A<<1)|oc;set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,nc);break; }
        case 0x1F: { uint8_t oc=get_flag(cpu,FLAG_C);uint8_t nc=(cpu->reg_A&0x01);cpu->reg_A=(cpu->reg_A>>1)|(oc<<7);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,nc);break; }
        case 0x27: { uint8_t a=cpu->reg_A;uint8_t corr=0;if(get_flag(cpu,FLAG_H)||((a&0x0F)>9)){corr|=0x06;}if(get_flag(cpu,FLAG_C)||(a>0x99)){corr|=0x60;set_flag(cpu,FLAG_C,1);}if(get_flag(cpu,FLAG_N)){cpu->reg_A-=corr;}else{cpu->reg_A+=corr;}set_flags_szp(cpu,cpu->reg_A);break; }
        case 0x2F: cpu->reg_A=~cpu->reg_A;set_flag(cpu,FLAG_H,1);set_flag(cpu,FLAG_N,1);break;
        case 0x37: set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_C,1);break;
        case 0x3F: set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_H,get_flag(cpu,FLAG_C));set_flag(cpu,FLAG_C,!get_flag(cpu,FLAG_C));break;
        case 0xCB: t_states += (prefix==1) ? cpu_ddfd_cb_step(cpu,&cpu->reg_IX,1) : (prefix==2 ? cpu_ddfd_cb_step(cpu,&cpu->reg_IY,0) : cpu_cb_step(cpu)); break;
        case 0xED: t_states += cpu_ed_step(cpu); break;
        case 0xE3: { uint16_t t;uint16_t spv=readWord(cpu->reg_SP);if(prefix==1){t=cpu->reg_IX;cpu->reg_IX=spv;t_states+=19;}else if(prefix==2){t=cpu->reg_IY;cpu->reg_IY=spv;t_states+=19;}else{t=get_HL(cpu);set_HL(cpu,spv);t_states+=15;}writeWord(cpu->reg_SP,t);break; }
        case 0xF9: if(prefix==1)cpu->reg_SP=cpu->reg_IX;else if(prefix==2)cpu->reg_SP=cpu->reg_IY;else cpu->reg_SP=get_HL(cpu); t_states+=(prefix?6:2); break;
        case 0xD3: { uint8_t p=readByte(cpu->reg_PC++);uint16_t port=(cpu->reg_A<<8)|p;io_write(port,cpu->reg_A); t_states+=7; break; }
        case 0xDB: { uint8_t p=readByte(cpu->reg_PC++);uint16_t port=(cpu->reg_A<<8)|p;cpu->reg_A=io_read(port); t_states+=7; break; }
        case 0xF3: cpu->iff1=0;cpu->iff2=0;cpu->ei_delay=0; break;
        case 0xFB: cpu->ei_delay=1; break;
        case 0x76: cpu->halted=1; break;
        default: if(prefix)cpu->reg_PC--; printf("Error: Unknown opcode: 0x%s%02X at address 0x%04X\n",(prefix==1?"DD":(prefix==2?"FD":"")),opcode,cpu->reg_PC-1); exit(1);
    }
    ula_instruction_progress_ptr = NULL;
    return t_states;
}

// --- Test Harness Utilities ---
static void cpu_reset_state(Z80* cpu) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->interruptMode = 1;
    cpu->reg_SP = 0xFFFF;
}

static void memory_clear(void) {
    memset(memory, 0, sizeof(memory));
}

static bool append_output_char(char* output, size_t* length, size_t capacity, char ch) {
    if (*length + 1 >= capacity) {
        return false;
    }
    output[*length] = ch;
    (*length)++;
    output[*length] = '\0';
    return true;
}

static bool test_cb_sll_register(void) {
    Z80 cpu;
    cpu_reset_state(&cpu);
    memory_clear();
    cpu.reg_PC = 0x0000;
    cpu.reg_B = 0x80;
    memory[0x0000] = 0xCB;
    memory[0x0001] = 0x30; // SLL B
    total_t_states = 0;
    int t_states = cpu_step(&cpu);
    return cpu.reg_B == 0x01 && get_flag(&cpu, FLAG_C) && !get_flag(&cpu, FLAG_Z) && t_states == 8 && cpu.reg_PC == 0x0002;
}

static bool test_cb_sll_memory(void) {
    Z80 cpu;
    cpu_reset_state(&cpu);
    memory_clear();
    cpu.reg_PC = 0x0000;
    cpu.reg_H = 0x80;
    cpu.reg_L = 0x00;
    memory[0x8000] = 0x02;
    memory[0x0000] = 0xCB;
    memory[0x0001] = 0x36; // SLL (HL)
    total_t_states = 0;
    int t_states = cpu_step(&cpu);
    bool ok = memory[0x8000] == 0x05 && !get_flag(&cpu, FLAG_C) && t_states == 15 && cpu.reg_PC == 0x0002;
    if (!ok) {
        printf("    (HL) result=0x%02X, C=%d, t=%d, PC=0x%04X\n",
               memory[0x8000], get_flag(&cpu, FLAG_C), t_states, cpu.reg_PC);
    }
    return ok;
}

static bool test_ddcb_register_result(void) {
    Z80 cpu;
    cpu_reset_state(&cpu);
    memory_clear();
    cpu.reg_PC = 0x0000;
    cpu.reg_IX = 0x8000;
    cpu.reg_B = 0x00;
    memory[0x8000] = 0x80;
    memory[0x0000] = 0xDD;
    memory[0x0001] = 0xCB;
    memory[0x0002] = 0x00;
    memory[0x0003] = 0x30; // SLL (IX+0),B
    total_t_states = 0;
    int t_states = cpu_step(&cpu);
    bool ok = cpu.reg_B == 0x01 && memory[0x8000] == 0x01 && get_flag(&cpu, FLAG_C) && t_states == 20;
    if (!ok) {
        printf("    (IX+d) result=0x%02X, C=%d, t=%d\n",
               memory[0x8000], get_flag(&cpu, FLAG_C), t_states);
    }
    return ok;
}

static bool test_ddcb_memory_result(void) {
    Z80 cpu;
    cpu_reset_state(&cpu);
    memory_clear();
    cpu.reg_PC = 0x0000;
    cpu.reg_IY = 0x8100;
    memory[0x8100] = 0x02;
    memory[0x0000] = 0xFD;
    memory[0x0001] = 0xCB;
    memory[0x0002] = 0x00;
    memory[0x0003] = 0x36; // SLL (IY+0)
    total_t_states = 0;
    int t_states = cpu_step(&cpu);
    bool ok = memory[0x8100] == 0x05 && !get_flag(&cpu, FLAG_C) && t_states == 23;
    if (!ok) {
        printf("    (IY+d) result=0x%02X, C=%d, t=%d\n",
               memory[0x8100], get_flag(&cpu, FLAG_C), t_states);
    }
    return ok;
}

static bool test_neg_duplicates(void) {
    Z80 cpu;
    cpu_reset_state(&cpu);
    memory_clear();
    cpu.reg_PC = 0x0000;
    cpu.reg_A = 0x01;
    memory[0x0000] = 0xED;
    memory[0x0001] = 0x4C; // NEG duplicate
    total_t_states = 0;
    int t_states = cpu_step(&cpu);
    return cpu.reg_A == 0xFF && get_flag(&cpu, FLAG_C) && get_flag(&cpu, FLAG_N) && t_states == 8;
}

static bool test_im_modes(void) {
    Z80 cpu;
    cpu_reset_state(&cpu);
    memory_clear();
    cpu.reg_PC = 0x0000;
    memory[0x0000] = 0xED; memory[0x0001] = 0x46; // IM 0
    memory[0x0002] = 0xED; memory[0x0003] = 0x56; // IM 1
    memory[0x0004] = 0xED; memory[0x0005] = 0x5E; // IM 2
    total_t_states = 0;
    (void)cpu_step(&cpu);
    (void)cpu_step(&cpu);
    (void)cpu_step(&cpu);
    return cpu.interruptMode == 2;
}

static bool test_in_flags(void) {
    Z80 cpu;
    cpu_reset_state(&cpu);
    memory_clear();
    cpu.reg_PC = 0x0000;
    cpu.reg_B = 0x00;
    cpu.reg_C = 0x01; // Non-ULA port
    memory[0x0000] = 0xED;
    memory[0x0001] = 0x40; // IN B,(C)
    total_t_states = 0;
    (void)cpu_step(&cpu);
    return cpu.reg_B == 0xFF && get_flag(&cpu, FLAG_H) && get_flag(&cpu, FLAG_N);
}

static bool test_interrupt_im2(void) {
    Z80 cpu;
    cpu_reset_state(&cpu);
    memory_clear();
    cpu.interruptMode = 2;
    cpu.reg_I = 0x80;
    cpu.reg_SP = 0xFFFE;
    cpu.reg_PC = 0x1234;
    memory[0x80FF] = 0x78;
    memory[0x8100] = 0x56;
    int t_states = cpu_interrupt(&cpu, 0xFF);
    bool ok = cpu.reg_PC == 0x5678 && cpu.reg_SP == 0xFFFC && memory[0xFFFC] == 0x34 &&
              memory[0xFFFD] == 0x12 && t_states == 19;
    if (!ok) {
        printf("    IM2 PC=%04X SP=%04X stack=%02X%02X t=%d\n",
               cpu.reg_PC, cpu.reg_SP, memory[0xFFFD], memory[0xFFFC], t_states);
    }
    return ok;
}

static bool test_interrupt_im1(void) {
    Z80 cpu;
    cpu_reset_state(&cpu);
    memory_clear();
    cpu.interruptMode = 1;
    cpu.reg_SP = 0xFFFE;
    cpu.reg_PC = 0x2222;
    int t_states = cpu_interrupt(&cpu, 0xFF);
    return cpu.reg_PC == 0x0038 && cpu.reg_SP == 0xFFFC && memory[0xFFFC] == 0x22 && memory[0xFFFD] == 0x22 && t_states == 13;
}

static bool run_unit_tests(void) {
    struct {
        const char* name;
        bool (*fn)(void);
    } tests[] = {
        {"CB SLL register", test_cb_sll_register},
        {"CB SLL (HL)", test_cb_sll_memory},
        {"DDCB SLL register", test_ddcb_register_result},
        {"DDCB SLL memory", test_ddcb_memory_result},
        {"NEG duplicates", test_neg_duplicates},
        {"IM mode transitions", test_im_modes},
        {"IN flag behaviour", test_in_flags},
        {"IM 2 interrupt vector", test_interrupt_im2},
        {"IM 1 interrupt vector", test_interrupt_im1},
    };

    bool all_passed = true;
    printf("Running CPU unit tests...\n");
    for (size_t i = 0; i < sizeof(tests)/sizeof(tests[0]); ++i) {
        bool ok = tests[i].fn();
        printf("  %-28s %s\n", tests[i].name, ok ? "PASS" : "FAIL");
        if (!ok) {
            all_passed = false;
        }
    }
    return all_passed;
}

static bool handle_cpm_bdos(Z80* cpu, char* output, size_t* out_len, size_t out_cap, int* terminated) {
    uint8_t func = cpu->reg_C;
    uint16_t ret = cpu_pop(cpu);
    switch (func) {
        case 0x00:
            *terminated = 1;
            cpu->reg_PC = ret;
            return true;
        case 0x02:
            if (!append_output_char(output, out_len, out_cap, (char)cpu->reg_E)) {
                return false;
            }
            cpu->reg_PC = ret;
            return true;
        case 0x09: {
            uint16_t addr = get_DE(cpu);
            while (1) {
                char ch = (char)memory[addr++];
                if (ch == '$') {
                    break;
                }
                if (!append_output_char(output, out_len, out_cap, ch)) {
                    return false;
                }
            }
            cpu->reg_PC = ret;
            return true;
        }
        default:
            cpu->reg_PC = ret;
            return true;
    }
}

static int run_z80_com_test(const char* path, const char* success_marker, char* output, size_t output_cap) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        return -1;
    }

    Z80 cpu;
    cpu_reset_state(&cpu);
    memory_clear();

    size_t loaded = fread(memory + 0x0100, 1, sizeof(memory) - 0x0100, f);
    fclose(f);
    if (loaded == 0) {
        return 0;
    }

    memory[0x0000] = 0xC3; // JP 0x0100
    memory[0x0001] = 0x00;
    memory[0x0002] = 0x01;
    memory[0x0005] = 0xC9; // RET

    cpu.reg_PC = 0x0100;
    cpu.reg_SP = 0xFFFF;
    cpu.interruptMode = 1;
    cpu.iff1 = cpu.iff2 = 0;

    size_t out_len = 0;
    if (output_cap > 0) {
        output[0] = '\0';
    }

    const uint64_t max_cycles = 400000000ULL;
    uint64_t cycles = 0;
    int terminated = 0;

    while (!terminated && cycles < max_cycles) {
        if (cpu.reg_PC == 0x0005) {
            if (!handle_cpm_bdos(&cpu, output, &out_len, output_cap, &terminated)) {
                return 0;
            }
            continue;
        }

        int t_states = cpu_step(&cpu);
        if (t_states <= 0) {
            return 0;
        }
        cycles += (uint64_t)t_states;
    }

    if (!terminated) {
        return 0;
    }

    if (success_marker && strstr(output, success_marker) == NULL) {
        return 0;
    }

    return 1;
}

static int run_cpu_tests(const char* rom_dir) {
    bool unit_pass = run_unit_tests();
    bool overall = unit_pass;

    const struct {
        const char* filename;
        const char* marker;
        const char* label;
    } optional_tests[] = {
        {"zexdoc.com", "ZEXDOC", "ZEXDOC"},
        {"zexall.com", "ZEXALL", "ZEXALL"},
    };

    char full_path[512];
    char output_log[32768];

    for (size_t i = 0; i < sizeof(optional_tests)/sizeof(optional_tests[0]); ++i) {
        int required = 0;
        if (rom_dir && rom_dir[0] != '\0') {
            required = snprintf(full_path, sizeof(full_path), "%s/%s", rom_dir, optional_tests[i].filename);
        } else {
            required = snprintf(full_path, sizeof(full_path), "%s", optional_tests[i].filename);
        }
        if (required < 0 || (size_t)required >= sizeof(full_path)) {
            fprintf(stderr, "Skipping %s (path too long)\n", optional_tests[i].label);
            continue;
        }
        int result = run_z80_com_test(full_path, optional_tests[i].marker, output_log, sizeof(output_log));
        if (result == -1) {
            printf("Skipping %s (missing %s)\n", optional_tests[i].label, full_path);
            continue;
        }
        if (result == 1) {
            printf("%s test PASS\n", optional_tests[i].label);
        } else {
            printf("%s test FAIL\n", optional_tests[i].label);
            printf("Output:\n%s\n", output_log);
            overall = false;
        }
    }

    return overall ? 0 : 1;
}

static void print_usage(const char* prog) {
    fprintf(stderr,
            "Usage: %s [--audio-dump <wav_file>] [--beeper-log] [--tape-debug] "
            "[--tap <tap_file> | --tzx <tzx_file> | --wav <wav_file>] "
            "[--save-tap <tap_file> | --save-wav <wav_file>] "
            "[--test-rom-dir <dir>] [--run-tests] [rom_file]\n",
            prog);
}

// --- SDL Initialization ---
int init_sdl(void) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL could not initialize! SDL_Error: %s\n", SDL_GetError()); return 0;
    }
    window = SDL_CreateWindow("ZX Spectrum Emulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, TOTAL_WIDTH * DISPLAY_SCALE, TOTAL_HEIGHT * DISPLAY_SCALE, SDL_WINDOW_SHOWN);
    if (!window) { fprintf(stderr, "Window Error: %s\n", SDL_GetError()); return 0; }
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) { fprintf(stderr, "Renderer Error: %s\n", SDL_GetError()); return 0; }
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING, TOTAL_WIDTH, TOTAL_HEIGHT);
    if (!texture) { fprintf(stderr, "Texture Error: %s\n", SDL_GetError()); return 0; }
    SDL_RenderSetLogicalSize(renderer, TOTAL_WIDTH, TOTAL_HEIGHT);

    SDL_AudioSpec wanted_spec, have_spec;
    SDL_zero(wanted_spec);
    wanted_spec.freq = 44100;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = 1;
    wanted_spec.samples = 512;
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = NULL;

    if (SDL_OpenAudio(&wanted_spec, &have_spec) < 0) {
        fprintf(stderr, "Failed to open audio: %s\n", SDL_GetError());
        beeper_set_latency_limit(256.0);
    } else {
        if (have_spec.format != AUDIO_S16SYS || have_spec.channels != 1) {
            fprintf(stderr, "Unexpected audio format (format=%d, channels=%d). Audio disabled.\n", have_spec.format, have_spec.channels);
            SDL_CloseAudio();
            beeper_set_latency_limit(256.0);
        } else {
            audio_sample_rate = have_spec.freq > 0 ? have_spec.freq : wanted_spec.freq;
            audio_available = 1;
            beeper_cycles_per_sample = CPU_CLOCK_HZ / (double)audio_sample_rate;
            double latency_limit = have_spec.samples > 0 ? (double)have_spec.samples : (double)wanted_spec.samples;
            if (latency_limit < 256.0) {
                latency_limit = 256.0;
            }
            beeper_set_latency_limit(latency_limit);
            BEEPER_LOG(
                "[BEEPER] latency clamp set to %.0f samples (audio buffer %u, throttle %.0f, trim %.0f)\n",
                beeper_max_latency_samples,
                have_spec.samples > 0 ? have_spec.samples : wanted_spec.samples,
                beeper_latency_threshold(),
                beeper_latency_trim_samples);
            beeper_reset_audio_state(total_t_states, beeper_state);
            if (audio_dump_path && !audio_dump_file) {
                if (!audio_dump_start(audio_dump_path, (uint32_t)audio_sample_rate)) {
                    audio_dump_path = NULL;
                }
            }
            SDL_PauseAudio(0); // Start playing sound
        }
    }
    return 1;
}

// --- SDL Cleanup ---
void cleanup_sdl(void) {
    if (audio_available) {
        SDL_CloseAudio();
        audio_available = 0;
        beeper_set_latency_limit(256.0);
    }
    audio_dump_finish();
    if (texture) {
        SDL_DestroyTexture(texture);
    }
    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    if (window) {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
}

// --- Render ZX Spectrum Screen ---
void render_screen(void) {
    uint32_t border_rgba = spectrum_colors[border_color_idx & 7];
    uint64_t frame_count = total_t_states / T_STATES_PER_FRAME;
    int flash_phase = (int)((frame_count >> 5) & 1ULL);
    for(int y=0; y<TOTAL_HEIGHT; ++y) { for(int x=0; x<TOTAL_WIDTH; ++x) { if(x<BORDER_SIZE || x>=BORDER_SIZE+SCREEN_WIDTH || y<BORDER_SIZE || y>=BORDER_SIZE+SCREEN_HEIGHT) pixels[y * TOTAL_WIDTH + x] = border_rgba; } }
    for (int y = 0; y < SCREEN_HEIGHT; ++y) {
        for (int x_char = 0; x_char < SCREEN_WIDTH / 8; ++x_char) {
            uint16_t pix_addr = VRAM_START + ((y & 0xC0) << 5) + ((y & 7) << 8) + ((y & 0x38) << 2) + x_char;
            uint16_t attr_addr = ATTR_START + (y / 8 * 32) + x_char;
            uint8_t pix_byte = memory[pix_addr]; uint8_t attr_byte = memory[attr_addr];
            int ink_idx=attr_byte&7; int pap_idx=(attr_byte>>3)&7; int bright=(attr_byte>>6)&1; int flash=(attr_byte>>7)&1;
            const uint32_t* cmap=bright?spectrum_bright_colors:spectrum_colors; uint32_t ink=cmap[ink_idx]; uint32_t pap=cmap[pap_idx];
            if (flash && flash_phase) {
                uint32_t tmp = ink;
                ink = pap;
                pap = tmp;
            }
            for (int bit = 0; bit < 8; ++bit) { int sx=BORDER_SIZE+x_char*8+(7-bit); int sy=BORDER_SIZE+y; pixels[sy*TOTAL_WIDTH+sx]=((pix_byte>>bit)&1)?ink:pap; }
        }
    }
    tape_render_overlay();
    SDL_UpdateTexture(texture, NULL, pixels, TOTAL_WIDTH * sizeof(uint32_t));
    SDL_RenderClear(renderer); SDL_RenderCopy(renderer, texture, NULL, NULL); SDL_RenderPresent(renderer);
}

// --- SDL Keycode to Spectrum Matrix Mapping ---
int map_sdl_key_to_spectrum(SDL_Keycode k, int* r, uint8_t* m) {
    if(k==SDLK_LSHIFT||k==SDLK_RSHIFT){*r=0;*m=0x01;return 1;} if(k==SDLK_z){*r=0;*m=0x02;return 1;} if(k==SDLK_x){*r=0;*m=0x04;return 1;} if(k==SDLK_c){*r=0;*m=0x08;return 1;} if(k==SDLK_v){*r=0;*m=0x10;return 1;}
    if(k==SDLK_a){*r=1;*m=0x01;return 1;} if(k==SDLK_s){*r=1;*m=0x02;return 1;} if(k==SDLK_d){*r=1;*m=0x04;return 1;} if(k==SDLK_f){*r=1;*m=0x08;return 1;} if(k==SDLK_g){*r=1;*m=0x10;return 1;}
    if(k==SDLK_q){*r=2;*m=0x01;return 1;} if(k==SDLK_w){*r=2;*m=0x02;return 1;} if(k==SDLK_e){*r=2;*m=0x04;return 1;} if(k==SDLK_r){*r=2;*m=0x08;return 1;} if(k==SDLK_t){*r=2;*m=0x10;return 1;}
    if(k==SDLK_1){*r=3;*m=0x01;return 1;} if(k==SDLK_2){*r=3;*m=0x02;return 1;} if(k==SDLK_3){*r=3;*m=0x04;return 1;} if(k==SDLK_4){*r=3;*m=0x08;return 1;} if(k==SDLK_5){*r=3;*m=0x10;return 1;}
    if(k==SDLK_0){*r=4;*m=0x01;return 1;} if(k==SDLK_9){*r=4;*m=0x02;return 1;} if(k==SDLK_8){*r=4;*m=0x04;return 1;} if(k==SDLK_7){*r=4;*m=0x08;return 1;} if(k==SDLK_6){*r=4;*m=0x10;return 1;}
    if(k==SDLK_p){*r=5;*m=0x01;return 1;} if(k==SDLK_o){*r=5;*m=0x02;return 1;} if(k==SDLK_i){*r=5;*m=0x04;return 1;} if(k==SDLK_u){*r=5;*m=0x08;return 1;} if(k==SDLK_y){*r=5;*m=0x10;return 1;}
    if(k==SDLK_RETURN){*r=6;*m=0x01;return 1;} if(k==SDLK_l){*r=6;*m=0x02;return 1;} if(k==SDLK_k){*r=6;*m=0x04;return 1;} if(k==SDLK_j){*r=6;*m=0x08;return 1;} if(k==SDLK_h){*r=6;*m=0x10;return 1;}
    if(k==SDLK_SPACE){*r=7;*m=0x01;return 1;} if(k==SDLK_LCTRL||k==SDLK_RCTRL){*r=7;*m=0x02;return 1;} if(k==SDLK_m){*r=7;*m=0x04;return 1;} if(k==SDLK_n){*r=7;*m=0x08;return 1;} if(k==SDLK_b){*r=7;*m=0x10;return 1;}
    if(k==SDLK_BACKSPACE){*r=4;*m=0x01;return 1;} // Partial map for Backspace (Shift+0)
    return 0;
}

// --- Audio Callback ---
void audio_callback(void* userdata, Uint8* stream, int len) {
    Sint16* buffer = (Sint16*)stream;
    int num_samples = len / (int)sizeof(Sint16);
    double cycles_per_sample = beeper_cycles_per_sample;
    double playback_position = beeper_playback_position;
    double last_input = beeper_hp_last_input;
    double last_output = beeper_hp_last_output;
    int level = beeper_playback_level;

    if (cycles_per_sample <= 0.0) {
        memset(buffer, 0, (size_t)len);
        return;
    }

    if (beeper_event_head == beeper_event_tail && cycles_per_sample > 0.0) {
        double idle_cycles = playback_position - (double)beeper_last_event_t_state;
        if (idle_cycles > 0.0) {
            double idle_samples = idle_cycles / cycles_per_sample;
            if (idle_samples >= BEEPER_IDLE_RESET_SAMPLES) {
                memset(buffer, 0, (size_t)len);

                double new_position = playback_position + cycles_per_sample * (double)num_samples;
                double writer_cursor = beeper_writer_cursor;
                double writer_lag_samples = 0.0;
                if (cycles_per_sample > 0.0) {
                    writer_lag_samples = (new_position - writer_cursor) / cycles_per_sample;
                }

                if (!beeper_idle_log_active) {
                    double idle_ms = (idle_cycles / CPU_CLOCK_HZ) * 1000.0;
                    BEEPER_LOG(
                        "[BEEPER] idle reset #%llu after %.0f samples (idle %.2f ms, playback %.0f -> %.0f cycles, writer %llu, cursor %.0f, lag %.2f samples)\n",
                        (unsigned long long)(beeper_idle_reset_count + 1u),
                        idle_samples,
                        idle_ms,
                        playback_position,
                        new_position,
                        (unsigned long long)beeper_last_event_t_state,
                        writer_cursor,
                        writer_lag_samples);
                    if (beeper_logging_enabled) {
                        beeper_idle_log_active = 1;
                        ++beeper_idle_reset_count;
                    }
                }

                double baseline = (level ? 1.0 : -1.0) * (double)AUDIO_AMPLITUDE;
                last_input = baseline;
                last_output = 0.0;

                audio_dump_write_samples(buffer, (size_t)num_samples);

                beeper_playback_level = level;
                beeper_playback_position = new_position;
                if (beeper_writer_cursor < new_position) {
                    beeper_writer_cursor = new_position;
                }
                if (new_position > (double)beeper_last_event_t_state) {
                    uint64_t idle_sync_state = (uint64_t)llround(new_position);
                    if (idle_sync_state < beeper_last_event_t_state) {
                        idle_sync_state = beeper_last_event_t_state;
                    }
                    beeper_last_event_t_state = idle_sync_state;
                }
                beeper_hp_last_input = last_input;
                beeper_hp_last_output = last_output;
                (void)userdata;
                return;
            }
        }
    }

    for (int i = 0; i < num_samples; ++i) {
        double target_position = playback_position + cycles_per_sample;

        while (beeper_event_head != beeper_event_tail &&
               (double)beeper_events[beeper_event_head].t_state <= target_position) {
            level = beeper_events[beeper_event_head].level ? 1 : 0;
            playback_position = (double)beeper_events[beeper_event_head].t_state;
            beeper_event_head = (beeper_event_head + 1) % BEEPER_EVENT_CAPACITY;
        }

        double raw_sample = (level ? 1.0 : -1.0) * (double)AUDIO_AMPLITUDE;
        double filtered_sample = raw_sample - last_input + BEEPER_HP_ALPHA * last_output;
        last_input = raw_sample;
        last_output = filtered_sample;

        if (filtered_sample > 32767.0) {
            filtered_sample = 32767.0;
        } else if (filtered_sample < -32768.0) {
            filtered_sample = -32768.0;
        }
        buffer[i] = (Sint16)lrint(filtered_sample);

        playback_position = target_position;
    }

    audio_dump_write_samples(buffer, (size_t)num_samples);

    beeper_playback_level = level;
    beeper_playback_position = playback_position;
    if (beeper_writer_cursor < playback_position) {
        beeper_writer_cursor = playback_position;
    }
    beeper_hp_last_input = last_input;
    beeper_hp_last_output = last_output;

    (void)userdata;
}

// --- Main Program ---
int main(int argc, char *argv[]) {
    const char* rom_filename = NULL;
    int rom_provided = 0;
    int run_tests = 0;
    const char* test_rom_dir = "tests/roms";

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--audio-dump") == 0) {
            if (i + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            audio_dump_path = argv[++i];
        } else if (strcmp(argv[i], "--beeper-log") == 0) {
            beeper_logging_enabled = 1;
        } else if (strcmp(argv[i], "--tape-debug") == 0) {
            tape_debug_logging = 1;
            tape_log("Tape debug logging enabled\n");
        } else if (strcmp(argv[i], "--tap") == 0) {
            if (i + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            if (tape_input_format != TAPE_FORMAT_NONE) {
                fprintf(stderr, "Only one tape image may be specified\n");
                return 1;
            }
            tape_input_format = TAPE_FORMAT_TAP;
            tape_input_path = argv[++i];
        } else if (strcmp(argv[i], "--tzx") == 0) {
            if (i + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            if (tape_input_format != TAPE_FORMAT_NONE) {
                fprintf(stderr, "Only one tape image may be specified\n");
                return 1;
            }
            tape_input_format = TAPE_FORMAT_TZX;
            tape_input_path = argv[++i];
        } else if (strcmp(argv[i], "--wav") == 0) {
            if (i + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            if (tape_input_format != TAPE_FORMAT_NONE) {
                fprintf(stderr, "Only one tape image may be specified\n");
                return 1;
            }
            tape_input_format = TAPE_FORMAT_WAV;
            tape_input_path = argv[++i];
        } else if (strcmp(argv[i], "--save-tap") == 0) {
            if (i + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            if (tape_recorder.enabled) {
                fprintf(stderr, "Only one tape recording output may be specified\n");
                return 1;
            }
            tape_recorder_enable(argv[++i], TAPE_OUTPUT_TAP);
        } else if (strcmp(argv[i], "--save-wav") == 0) {
            if (i + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            if (tape_recorder.enabled) {
                fprintf(stderr, "Only one tape recording output may be specified\n");
                return 1;
            }
            tape_recorder_enable(argv[++i], TAPE_OUTPUT_WAV);
        } else if (strcmp(argv[i], "--test-rom-dir") == 0) {
            if (i + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            test_rom_dir = argv[++i];
        } else if (strcmp(argv[i], "--run-tests") == 0) {
            run_tests = 1;
        } else {
            TapeFormat inferred_format = tape_format_from_extension(argv[i]);
            if (inferred_format != TAPE_FORMAT_NONE && tape_input_format == TAPE_FORMAT_NONE) {
                tape_input_format = inferred_format;
                tape_input_path = argv[i];
            } else if (!rom_filename) {
                rom_filename = argv[i];
                rom_provided = 1;
            } else {
                fprintf(stderr, "Unknown argument: %s\n", argv[i]);
                print_usage(argv[0]);
                return 1;
            }
        }
    }

    if (run_tests) {
        return run_cpu_tests(test_rom_dir);
    }

    if (!rom_filename) {
        rom_filename = default_rom_filename;
    }

    FILE* rf = fopen(rom_filename, "rb");
    char *rom_fallback_path = NULL;
    const char *rom_log_path = rom_filename;
    if (!rf && !rom_provided) {
        rom_fallback_path = build_executable_relative_path(argv[0], rom_filename);
        if (rom_fallback_path) {
            rf = fopen(rom_fallback_path, "rb");
            if (rf) {
                rom_log_path = rom_fallback_path;
            }
        }
    }
    if (!rf) {
        perror("ROM open error");
        fprintf(stderr, "File: %s\n", rom_log_path);
        free(rom_fallback_path);
        return 1;
    }
    size_t br = fread(memory, 1, 0x4000, rf);
    if (br != 0x4000) {
        fprintf(stderr, "ROM read error(%zu)\n", br);
        fclose(rf);
        free(rom_fallback_path);
        return 1;
    }
    fclose(rf);
    printf("Loaded %zu bytes from %s\n", br, rom_log_path);
    free(rom_fallback_path);

    tape_playback.use_waveform_playback = 0;
    if (tape_input_format != TAPE_FORMAT_NONE && tape_input_path) {
        if (tape_input_format == TAPE_FORMAT_WAV) {
            if (!tape_load_wav(tape_input_path, &tape_playback)) {
                tape_waveform_reset(&tape_playback.waveform);
                return 1;
            }
            printf("Loaded WAV tape %s (%zu transitions @ %u Hz)\n",
                   tape_input_path,
                   tape_playback.waveform.count,
                   (unsigned)tape_playback.waveform.sample_rate);
            if (tape_playback.waveform.count == 0) {
                fprintf(stderr, "Warning: WAV tape '%s' contains no transitions\n", tape_input_path);
            }
            tape_input_enabled = 1;
        } else {
            tape_playback.format = tape_input_format;
            tape_waveform_reset(&tape_playback.waveform);
            if (!tape_load_image(tape_input_path, tape_input_format, &tape_playback.image)) {
                tape_free_image(&tape_playback.image);
                return 1;
            }
            if (!tape_generate_waveform_from_image(&tape_playback.image, &tape_playback.waveform)) {
                fprintf(stderr, "Failed to synthesise tape playback waveform for '%s'\n", tape_input_path);
                tape_free_image(&tape_playback.image);
                tape_waveform_reset(&tape_playback.waveform);
                return 1;
            }
            tape_playback.use_waveform_playback = 1;
            printf("Loaded tape image %s (%zu blocks)\n", tape_input_path, tape_playback.image.count);
            if (tape_playback.image.count == 0) {
                fprintf(stderr, "Warning: tape image '%s' is empty\n", tape_input_path);
                tape_input_enabled = 0;
            } else {
                tape_input_enabled = 1;
            }
        }
    } else {
        tape_input_enabled = 0;
    }

    if(!init_sdl()){tape_shutdown();cleanup_sdl();return 1;}

    Z80 cpu={0}; cpu.reg_PC=0x0000; cpu.reg_SP=0xFFFF; cpu.iff1=0; cpu.iff2=0; cpu.interruptMode=1;
    cpu.halted = 0; cpu.ei_delay = 0;
    total_t_states = 0;

    if (tape_input_enabled) {
        tape_reset_playback(&tape_playback);
        tape_deck_status = TAPE_DECK_STATUS_STOP;
    } else if (tape_recorder.enabled) {
        tape_deck_status = TAPE_DECK_STATUS_STOP;
    } else {
        tape_deck_status = TAPE_DECK_STATUS_IDLE;
    }

    if (tape_input_enabled || tape_recorder.enabled) {
        printf("Tape controls: F5 Play, F6 Stop, F7 Rewind");
        if (tape_recorder.enabled) {
            printf(", F8 Record");
        }
        printf("\n");
    }

    if (audio_available) {
        SDL_LockAudio();
        beeper_reset_audio_state(total_t_states, beeper_state);
        SDL_UnlockAudio();
    } else {
        beeper_reset_audio_state(total_t_states, beeper_state);
    }

    printf("Starting Z80 emulation...\n");

    int quit = 0;
    SDL_Event e;
    uint64_t performance_frequency = SDL_GetPerformanceFrequency();
    uint64_t previous_counter = SDL_GetPerformanceCounter();
    double cycle_accumulator = 0.0;
    int frame_t_state_accumulator = 0;

    while (!quit) {
        while (SDL_PollEvent(&e) != 0) {
            if (e.type == SDL_QUIT) {
                quit = 1;
            } else if (e.type == SDL_MOUSEBUTTONDOWN) {
                if (tape_handle_mouse_button(&e)) {
                    continue;
                }
            } else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                if (tape_handle_control_key(&e)) {
                    continue;
                }
                int row = -1;
                uint8_t mask = 0;
                int mapped = map_sdl_key_to_spectrum(e.key.keysym.sym, &row, &mask);
                if (mapped) {
                    if (e.type == SDL_KEYDOWN) {
                        keyboard_matrix[row] &= ~mask;
                        if (e.key.keysym.sym == SDLK_BACKSPACE) {
                            keyboard_matrix[0] &= ~0x01; // Press Shift
                        }
                    } else {
                        keyboard_matrix[row] |= mask;
                        if (e.key.keysym.sym == SDLK_BACKSPACE) {
                            keyboard_matrix[0] |= 0x01; // Release Shift
                        }
                    }
                }
            }
        }

        if (quit) {
            break;
        }

        uint64_t current_counter = SDL_GetPerformanceCounter();
        double elapsed_seconds = (double)(current_counter - previous_counter) / (double)performance_frequency;
        previous_counter = current_counter;

        if (elapsed_seconds < 0.0) {
            elapsed_seconds = 0.0;
        }

        cycle_accumulator += elapsed_seconds * CPU_CLOCK_HZ;
        if (cycle_accumulator > CPU_CLOCK_HZ * 0.25) {
            cycle_accumulator = CPU_CLOCK_HZ * 0.25;
        }

        if (audio_available && beeper_cycles_per_sample > 0.0) {
            double latency_samples = beeper_current_latency_samples();
            double threshold = beeper_latency_threshold();
            if (latency_samples >= threshold) {
                Uint32 delay_ms = beeper_recommended_throttle_delay(latency_samples);
                SDL_Delay(delay_ms);
                continue;
            }
        }

        if (cycle_accumulator < 1.0) {
            SDL_Delay(0);
            continue;
        }

        int cycles_to_execute = (int)cycle_accumulator;
        int latency_poll_cycles = 0;
        int latency_poll_threshold = 0;
        int throttled_audio = 0;
        double throttled_latency_samples = 0.0;

        if (audio_available && beeper_cycles_per_sample > 0.0) {
            latency_poll_threshold = (int)(beeper_cycles_per_sample * 32.0);
            if (latency_poll_threshold < 128) {
                latency_poll_threshold = 128;
            }
        }

        while (cycles_to_execute > 0) {
            if (cpu.ei_delay) {
                cpu.iff1 = cpu.iff2 = 1;
                cpu.ei_delay = 0;
            }

            int t_states = cpu.halted ? 4 : cpu_step(&cpu);
            if (t_states <= 0) {
                t_states = 4;
            }

            cycles_to_execute -= t_states;
            cycle_accumulator -= t_states;
            if (cycle_accumulator < 0.0) {
                cycle_accumulator = 0.0;
            }

            frame_t_state_accumulator += t_states;
            total_t_states += t_states;

            ula_process_port_events(total_t_states);
            tape_update(total_t_states);
            tape_recorder_update(total_t_states, 0);

            if (audio_available && latency_poll_threshold > 0) {
                latency_poll_cycles += t_states;
                if (latency_poll_cycles >= latency_poll_threshold) {
                    latency_poll_cycles = 0;
                    double latency_samples = beeper_current_latency_samples();
                    if (latency_samples >= beeper_latency_threshold()) {
                        throttled_audio = 1;
                        throttled_latency_samples = latency_samples;
                        break;
                    }
                }
            }

            while (frame_t_state_accumulator >= T_STATES_PER_FRAME) {
                if (cpu.iff1) {
                    int interrupt_t_states = cpu_interrupt(&cpu, 0xFF);
                    total_t_states += interrupt_t_states;
                    frame_t_state_accumulator += interrupt_t_states;
                }
                render_screen();
                frame_t_state_accumulator -= T_STATES_PER_FRAME;
            }
        }

        if (throttled_audio) {
            Uint32 delay_ms = beeper_recommended_throttle_delay(throttled_latency_samples);
            SDL_Delay(delay_ms);
            continue;
        }
    }

    printf("Emulation finished.\nFinal State:\nPC:%04X SP:%04X AF:%04X BC:%04X DE:%04X HL:%04X IX:%04X IY:%04X\n",cpu.reg_PC,cpu.reg_SP,get_AF(&cpu),get_BC(&cpu),get_DE(&cpu),get_HL(&cpu),cpu.reg_IX,cpu.reg_IY);
    tape_shutdown();
    cleanup_sdl(); return 0;
}
static void ula_queue_port_value(uint8_t value) {
    uint64_t event_t_state = total_t_states;
    if (ula_instruction_progress_ptr) {
        event_t_state = ula_instruction_base_tstate + (uint64_t)(*ula_instruction_progress_ptr);
    }

    if (ula_write_count > 0) {
        uint64_t previous_t_state = ula_write_queue[ula_write_count - 1].t_state;
        if (event_t_state < previous_t_state) {
            event_t_state = previous_t_state;
        }
    }

    if (ula_write_count < (sizeof(ula_write_queue) / sizeof(ula_write_queue[0]))) {
        ula_write_queue[ula_write_count].value = value;
        ula_write_queue[ula_write_count].t_state = event_t_state;
        ula_write_count++;
    } else {
        memmove(&ula_write_queue[0], &ula_write_queue[1], (ula_write_count - 1) * sizeof(UlaWriteEvent));
        ula_write_queue[ula_write_count - 1].value = value;
        ula_write_queue[ula_write_count - 1].t_state = event_t_state;
    }
}

static void ula_process_port_events(uint64_t current_t_state) {
    if (ula_write_count == 0) {
        return;
    }

    (void)current_t_state;

    for (size_t i = 0; i < ula_write_count; ++i) {
        uint8_t value = ula_write_queue[i].value;
        uint64_t event_t_state = ula_write_queue[i].t_state;
        border_color_idx = value & 0x07;

        int new_beeper_state = (value >> 4) & 0x01;
        if (new_beeper_state != beeper_state) {
            beeper_state = new_beeper_state;
            beeper_push_event(event_t_state, beeper_state);
        }

        int mic_level = (value >> 3) & 0x01;
        tape_recorder_handle_mic(event_t_state, mic_level);
    }

    ula_write_count = 0;
}

