#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdarg.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define TAPE_GETCWD _getcwd
#define STAT_STRUCT struct _stat
#define STAT_FUNC _stat
#define STAT_ISDIR(mode) (((mode) & _S_IFDIR) != 0)
#else
#include <unistd.h>
#define TAPE_GETCWD getcwd
#define STAT_STRUCT struct stat
#define STAT_FUNC stat
#define STAT_ISDIR(mode) S_ISDIR(mode)
#endif
#include <SDL.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// --- Z80 Flag Register Bits ---
#define FLAG_C  (1 << 0) // Carry Flag
#define FLAG_N  (1 << 1) // Add/Subtract Flag
#define FLAG_PV (1 << 2) // Parity/Overflow Flag
#define FLAG_H  (1 << 4) // Half Carry Flag
#define FLAG_Z  (1 << 6) // Zero Flag
#define FLAG_S  (1 << 7) // Sign Flag

// --- Global Memory ---
uint8_t memory[0x10000]; // 65536 bytes

typedef enum {
    SPECTRUM_MODEL_48K,
    SPECTRUM_MODEL_128K,
    SPECTRUM_MODEL_PLUS2A,
    SPECTRUM_MODEL_PLUS3
} SpectrumModel;

static const char* spectrum_model_to_string(SpectrumModel model) {
    switch (model) {
        case SPECTRUM_MODEL_48K:
            return "48K";
        case SPECTRUM_MODEL_128K:
            return "128K";
        case SPECTRUM_MODEL_PLUS2A:
            return "+2A";
        case SPECTRUM_MODEL_PLUS3:
            return "+3";
        default:
            break;
    }
    return "Unknown";
}

typedef enum {
    CONTENTION_PROFILE_48K,
    CONTENTION_PROFILE_128K,
    CONTENTION_PROFILE_128K_PLUS2A,
    CONTENTION_PROFILE_128K_PLUS3
} SpectrumContentionProfile;

typedef enum {
    PERIPHERAL_CONTENTION_NONE,
    PERIPHERAL_CONTENTION_IF1,
    PERIPHERAL_CONTENTION_PLUS3
} PeripheralContentionProfile;

static SpectrumModel spectrum_model = SPECTRUM_MODEL_48K;
static SpectrumContentionProfile spectrum_contention_profile = CONTENTION_PROFILE_48K;
static PeripheralContentionProfile peripheral_contention_profile = PERIPHERAL_CONTENTION_NONE;
static uint8_t rom_pages[4][0x4000];
static uint8_t ram_pages[8][0x4000];
static uint8_t current_rom_page = 0;
static uint8_t current_screen_bank = 5;
static uint8_t current_paged_bank = 0;
static int paging_disabled = 0;
static uint8_t rom_page_count = 1;
static uint8_t page_contended[4] = {0u, 1u, 0u, 0u};
static uint8_t floating_bus_last_value = 0xFFu;
static uint8_t gate_array_7ffd_state = 0u;
static uint8_t gate_array_1ffd_state = 0u;
static uint8_t ay_registers[16];
static uint8_t ay_selected_register = 0u;
static int ay_register_latched = 0;

typedef enum {
    MEMORY_PAGE_NONE,
    MEMORY_PAGE_ROM,
    MEMORY_PAGE_RAM
} SpectrumMemoryPageType;

typedef struct {
    SpectrumMemoryPageType type;
    uint8_t index;
} SpectrumMemoryPage;

static SpectrumMemoryPage spectrum_pages[4] = {
    {MEMORY_PAGE_ROM, 0u},
    {MEMORY_PAGE_RAM, 5u},
    {MEMORY_PAGE_RAM, 2u},
    {MEMORY_PAGE_RAM, 0u}
};

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
#define BORDER_EVENT_CAPACITY 65536
#define ULA_LINES_PER_FRAME 312
#define ULA_T_STATES_PER_LINE 224
#define ULA_VISIBLE_TOP_LINES 12
#define ULA_VISIBLE_BOTTOM_LINES 12
#define ULA_LEFT_BORDER_TSTATES 24
#define ULA_DISPLAY_TSTATES 128
#define ULA_RIGHT_BORDER_TSTATES 24
#define ULA_LINE_VISIBLE_TSTATES (ULA_LEFT_BORDER_TSTATES + ULA_DISPLAY_TSTATES + ULA_RIGHT_BORDER_TSTATES)

const double CPU_CLOCK_HZ = 3500000.0;

// --- SDL Globals ---
SDL_Window* window = NULL;
SDL_Renderer* renderer = NULL;
SDL_Texture* texture = NULL;
uint32_t pixels[ TOTAL_WIDTH * TOTAL_HEIGHT ];
static int window_fullscreen = 0;

// --- Audio Globals ---
volatile int beeper_state = 0; // 0 = low, 1 = high
const int AUDIO_AMPLITUDE = 2000;
static const double BEEPER_IDLE_RESET_SAMPLES = 512.0;
static const double BEEPER_REWIND_TOLERANCE_SAMPLES = 8.0;
int audio_sample_rate = 44100;
int audio_available = 0;
static int audio_channel_count = 1;

static double beeper_max_latency_samples = 256.0;
static double beeper_latency_throttle_samples = 320.0;
static double beeper_latency_release_samples = 256.0;
static double beeper_latency_trim_samples = 512.0;
static const double BEEPER_HP_ALPHA = 0.995;

static const char* audio_dump_path = NULL;

static const int16_t TAPE_WAV_AMPLITUDE = 20000;

static int speaker_tape_playback_level = 1;
static int speaker_tape_record_level = 1;
static int speaker_output_level = 1;

static const double AY_CLOCK_HZ = 1750000.0;
static double ay_cycles_per_sample = 0.0;
static double ay_global_gain = 6000.0;
static double ay_channel_pan[3] = {-0.75, 0.0, 0.75};
static const double ay_volume_table[16] = {
    0.0000, 0.0049, 0.0098, 0.0205,
    0.0390, 0.0600, 0.0901, 0.1353,
    0.1900, 0.2700, 0.3900, 0.5600,
    0.8000, 1.1500, 1.6500, 2.3000
};

typedef struct {
    double tone_period[3];
    double tone_counter[3];
    int tone_output[3];
    double noise_period;
    double noise_counter;
    uint32_t noise_lfsr;
    int noise_output;
    double envelope_period;
    double envelope_counter;
    uint8_t envelope_volume;
    int envelope_direction;
    int envelope_hold;
    int envelope_alternate;
    int envelope_continue;
    int envelope_active;
} AyState;

static AyState ay_state;
static double ay_hp_last_input_left = 0.0;
static double ay_hp_last_output_left = 0.0;
static double ay_hp_last_input_right = 0.0;
static double ay_hp_last_output_right = 0.0;

// --- Tape Constants ---
static const int TAPE_PILOT_PULSE_TSTATES = 2168;
static const int TAPE_SYNC_FIRST_PULSE_TSTATES = 667;
static const int TAPE_SYNC_SECOND_PULSE_TSTATES = 735;
static const int TAPE_BIT0_PULSE_TSTATES = 855;
static const int TAPE_BIT1_PULSE_TSTATES = 1710;
static const int TAPE_HEADER_PILOT_COUNT = 8063;
static const int TAPE_DATA_PILOT_COUNT = 3223;
static const uint64_t TAPE_SILENCE_THRESHOLD_TSTATES = 350000ULL; // 0.1 second
static const uint64_t TAPE_RECORDER_AUTOSTOP_TSTATES = 7000000ULL; // ~2 seconds

typedef enum {
    TAPE_BLOCK_TYPE_STANDARD,
    TAPE_BLOCK_TYPE_TURBO,
    TAPE_BLOCK_TYPE_PURE_DATA,
    TAPE_BLOCK_TYPE_PURE_TONE,
    TAPE_BLOCK_TYPE_PULSE_SEQUENCE,
    TAPE_BLOCK_TYPE_DIRECT_RECORDING
} TapeBlockType;

typedef struct {
    TapeBlockType type;
    uint8_t* data;
    uint32_t length;
    uint32_t pause_ms;
    uint8_t used_bits_in_last_byte;
    uint16_t pilot_pulse_tstates;
    uint32_t pilot_pulse_count;
    uint16_t sync_first_pulse_tstates;
    uint16_t sync_second_pulse_tstates;
    uint16_t bit0_first_pulse_tstates;
    uint16_t bit0_second_pulse_tstates;
    uint16_t bit1_first_pulse_tstates;
    uint16_t bit1_second_pulse_tstates;
    uint16_t tone_pulse_tstates;
    uint32_t tone_pulse_count;
    uint16_t* pulse_sequence_durations;
    size_t pulse_sequence_count;
    uint32_t direct_tstates_per_sample;
    uint8_t* direct_samples;
    uint32_t direct_sample_count;
    int direct_initial_level;
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
    SNAPSHOT_FORMAT_NONE,
    SNAPSHOT_FORMAT_SNA,
    SNAPSHOT_FORMAT_Z80
} SnapshotFormat;

typedef struct Z80 Z80;

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
    uint64_t idle_start_tstate;
} TapeRecorder;

static TapePlaybackState tape_playback = {0};
static TapeRecorder tape_recorder = {0};
static int tape_ear_state = 1;
static int tape_input_enabled = 0;

static FILE* spectrum_log_file = NULL;

typedef enum {
    TAPE_DECK_STATUS_IDLE,
    TAPE_DECK_STATUS_PLAY,
    TAPE_DECK_STATUS_STOP,
    TAPE_DECK_STATUS_REWIND,
    TAPE_DECK_STATUS_RECORD
} TapeDeckStatus;

static TapeDeckStatus tape_deck_status = TAPE_DECK_STATUS_IDLE;
static int tape_debug_logging = 0;
static int paging_debug_logging = 0;
static int paging_log_registers = 0;
static int ram_hash_logging = 0;
static Z80* paging_cpu_state = NULL;

static void spectrum_init_log_output(void) {
    if (spectrum_log_file) {
        return;
    }

    FILE* stdout_file = freopen("z80.log", "w", stdout);
    if (stdout_file) {
        setvbuf(stdout_file, NULL, _IOLBF, 0);
    }

    const char* stderr_mode = stdout_file ? "a" : "w";
    FILE* stderr_file = freopen("z80.log", stderr_mode, stderr);
    if (stderr_file) {
        setvbuf(stderr_file, NULL, _IOLBF, 0);
        spectrum_log_file = stderr_file;
        return;
    }

    spectrum_log_file = stdout_file ? stdout_file : stderr;
}

static void spectrum_log_cpu_state(uint64_t tstate);
static void spectrum_log_ram_hashes(const char* reason);
static uint32_t spectrum_hash_buffer(const uint8_t* data, size_t length);

typedef enum {
    TAPE_MANAGER_MODE_HIDDEN,
    TAPE_MANAGER_MODE_MENU,
    TAPE_MANAGER_MODE_FILE_INPUT,
    TAPE_MANAGER_MODE_FILE_BROWSER
} TapeManagerMode;

static TapeManagerMode tape_manager_mode = TAPE_MANAGER_MODE_HIDDEN;
static char tape_manager_status[128] = "PRESS TAB TO OPEN TAPE MANAGER";
static char tape_manager_input_buffer[PATH_MAX];
static size_t tape_manager_input_length = 0u;

#define TAPE_MANAGER_BROWSER_MAX_ENTRIES 256
#define TAPE_MANAGER_BROWSER_VISIBLE_LINES 10

typedef struct {
    char name[PATH_MAX];
    int is_dir;
    int is_up;
} TapeBrowserEntry;

static char tape_manager_browser_path[PATH_MAX];
static TapeBrowserEntry tape_manager_browser_entries[TAPE_MANAGER_BROWSER_MAX_ENTRIES];
static int tape_manager_browser_entry_count = 0;
static int tape_manager_browser_selection = 0;
static int tape_manager_browser_scroll = 0;

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

static void paging_log(const char* fmt, ...) {
    if (!paging_debug_logging || !fmt) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    fputs("[PAGE] ", stderr);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

static void spectrum_format_page_label(const SpectrumMemoryPage* page, char* out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }

    if (!page) {
        snprintf(out, out_size, "<null>");
        return;
    }

    switch (page->type) {
        case MEMORY_PAGE_ROM:
            snprintf(out, out_size, "ROM%u", (unsigned)page->index);
            break;
        case MEMORY_PAGE_RAM:
            snprintf(out, out_size, "RAM%u", (unsigned)page->index);
            break;
        default:
            snprintf(out, out_size, "None");
            break;
    }
}

static void spectrum_log_paging_state(const char* reason,
                                      uint16_t port,
                                      uint8_t value,
                                      uint64_t tstate) {
    if (!paging_debug_logging) {
        return;
    }

    char labels[4][8];
    for (int i = 0; i < 4; ++i) {
        spectrum_format_page_label(&spectrum_pages[i], labels[i], sizeof(labels[i]));
    }

    paging_log("%s t=%" PRIu64 " model=%s port=0x%04X value=0x%02X 7FFD=0x%02X 1FFD=0x%02X disabled=%d\n",
               reason ? reason : "paging",
               (uint64_t)tstate,
               spectrum_model_to_string(spectrum_model),
               (unsigned)port,
               (unsigned)value,
               (unsigned)gate_array_7ffd_state,
               (unsigned)gate_array_1ffd_state,
               paging_disabled);
    paging_log("    map: 0=%s 1=%s 2=%s 3=%s screen=%u\n",
               labels[0],
               labels[1],
               labels[2],
               labels[3],
               (unsigned)current_screen_bank);
    spectrum_log_cpu_state(tstate);
}

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
    {'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}},
    {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x04, 0x04, 0x00}},
    {'/', {0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00}},
    {'?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}},
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
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'J', {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x11, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'_', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}},
    {':', {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00}}
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
static char tape_input_path_storage[PATH_MAX];
static SnapshotFormat snapshot_input_format = SNAPSHOT_FORMAT_NONE;
static const char* snapshot_input_path = NULL;

static int string_ends_with_case_insensitive(const char* str, const char* suffix);
static TapeFormat tape_format_from_extension(const char* path);
static SnapshotFormat snapshot_format_from_extension(const char* path);
static int snapshot_load_sna(const char* path, Z80* cpu);
static int snapshot_load_z80(const char* path, Z80* cpu);
static int snapshot_load(const char* path, SnapshotFormat format, Z80* cpu);
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
static int tape_waveform_add_with_pending(TapeWaveform* waveform, uint64_t duration, uint64_t* pending_silence);
static int tape_generate_waveform_from_image(const TapeImage* image, TapeWaveform* waveform);
static int tape_load_wav(const char* path, TapePlaybackState* state);
static int tape_create_blank_wav(const char* path, uint32_t sample_rate);
static void tape_manager_browser_normalize_separators(char* path);
static void tape_manager_browser_strip_trailing_separator(char* path);
static int tape_manager_browser_parent_path(const char* path, char* out, size_t out_size);
static int tape_manager_browser_can_go_up(const char* path);
static int tape_browser_entry_compare(const void* a, const void* b);
static int tape_manager_browser_join_path(const char* base, const char* name, char* out, size_t out_size);
static void tape_manager_browser_clamp_selection(void);
static void tape_manager_browser_move_selection(int delta);
static void tape_manager_browser_page_selection(int delta);
static int tape_manager_refresh_browser(const char* directory);
static void tape_manager_begin_browser(void);
static void tape_manager_browser_activate_selection(void);
static void tape_manager_browser_go_parent(void);
static void tape_manager_browser_extract_directory(const char* path, char* directory, size_t directory_size);
static int tape_manager_create_blank_tape(const char* path, TapeFormat format);
static int tape_manager_ensure_path_available(const char* path, TapeFormat format);
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
static int tape_manager_handle_event(const SDL_Event* event);
static int tape_manager_handle_text_input(const SDL_Event* event);
static void tape_manager_toggle(void);
static void tape_manager_hide(void);
static void tape_manager_show_menu(void);
static void tape_manager_begin_path_input(void);
static void tape_manager_cancel_path_input(void);
static void tape_manager_set_status(const char* fmt, ...);
static int tape_manager_load_path(const char* path);
static void tape_manager_eject_tape(void);
static void tape_render_manager(void);
static void tape_set_input_path(const char* path);
static void tape_playback_accumulate_elapsed(TapePlaybackState* state, uint64_t stop_t_state);
static uint64_t tape_playback_elapsed_tstates(const TapePlaybackState* state, uint64_t current_t_state);
static uint64_t tape_recorder_elapsed_tstates(uint64_t current_t_state);
static void tape_render_overlay(void);
static int speaker_calculate_output_level(void);
static void speaker_update_output(uint64_t t_state, int emit_event);
static void ay_reset_state(void);
static void ay_set_sample_rate(int sample_rate);
static void ay_mix_sample(double elapsed_cycles, double* left_out, double* right_out);
static void ay_write_register(uint8_t reg, uint8_t value);
static int ay_parse_pan_spec(const char* spec);
static void toggle_fullscreen(void);
static void ula_queue_port_value(uint8_t value);
static void ula_process_port_events(uint64_t current_t_state);

static FILE* audio_dump_file = NULL;
static uint32_t audio_dump_data_bytes = 0;
static uint16_t audio_dump_channels = 1;

#define BEEPER_EVENT_CAPACITY 8192

typedef struct {
    uint64_t t_state;
    int8_t level;
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

static int spectrum_rom_contains_ascii(const uint8_t *rom, size_t size, const char *needle) {
    if (!rom || !needle) {
        return 0;
    }

    size_t needle_len = strlen(needle);
    if (needle_len == 0u || needle_len > size) {
        return 0;
    }

    for (size_t i = 0; i + needle_len <= size; ++i) {
        size_t j = 0u;
        while (j < needle_len && rom[i + j] == (uint8_t)needle[j]) {
            ++j;
        }
        if (j == needle_len) {
            return 1;
        }
    }

    return 0;
}

static int spectrum_rom_contains_ascii_case_insensitive(const uint8_t *rom,
                                                        size_t size,
                                                        const char *needle) {
    if (!rom || !needle) {
        return 0;
    }

    size_t needle_len = strlen(needle);
    if (needle_len == 0u || needle_len > size) {
        return 0;
    }

    for (size_t i = 0; i + needle_len <= size; ++i) {
        size_t j = 0u;
        while (j < needle_len) {
            unsigned char rom_byte = rom[i + j];
            unsigned char needle_byte = (unsigned char)needle[j];
            if (tolower(rom_byte) != tolower(needle_byte)) {
                break;
            }
            ++j;
        }
        if (j == needle_len) {
            return 1;
        }
    }

    return 0;
}

static int spectrum_rom_bank_seems_48k_basic(const uint8_t *rom) {
    if (!rom) {
        return 0;
    }

    int has_1982 = spectrum_rom_contains_ascii(rom, 0x4000u, "1982");
    int has_sinclair_research = spectrum_rom_contains_ascii_case_insensitive(rom, 0x4000u, "Sinclair Research");
    return has_1982 && has_sinclair_research;
}

static int spectrum_rom_bank_seems_128k_menu(const uint8_t *rom) {
    if (!rom) {
        return 0;
    }

    static const char *menu_markers[] = {"128K", "1986", "1985", "AMSTRAD", "MENU"};
    size_t marker_count = sizeof(menu_markers) / sizeof(menu_markers[0]);
    int marker_hit = 0;
    for (size_t i = 0; i < marker_count; ++i) {
        if (spectrum_rom_contains_ascii_case_insensitive(rom, 0x4000u, menu_markers[i])) {
            marker_hit = 1;
            break;
        }
    }

    if (!marker_hit) {
        return 0;
    }

    int has_128_text = spectrum_rom_contains_ascii(rom, 0x4000u, "128");
    return has_128_text;
}

static void spectrum_swap_rom_banks(size_t a,
                                    size_t b,
                                    uint8_t bank_loaded[4],
                                    uint8_t bank_loaded_from_primary[4],
                                    uint8_t bank_loaded_from_companion[4],
                                    uint8_t bank_loaded_from_hint[4],
                                    uint8_t bank_mirrored_from[4],
                                    uint8_t bank_matches_basic[4],
                                    uint8_t bank_matches_menu[4]) {
    if (a >= 4u || b >= 4u || a == b) {
        return;
    }

    uint8_t temp_bank[0x4000u];
    memcpy(temp_bank, rom_pages[a], sizeof(temp_bank));
    memcpy(rom_pages[a], rom_pages[b], sizeof(temp_bank));
    memcpy(rom_pages[b], temp_bank, sizeof(temp_bank));

    if (bank_loaded) {
        uint8_t tmp_flag = bank_loaded[a];
        bank_loaded[a] = bank_loaded[b];
        bank_loaded[b] = tmp_flag;
    }
    if (bank_loaded_from_primary) {
        uint8_t tmp_flag = bank_loaded_from_primary[a];
        bank_loaded_from_primary[a] = bank_loaded_from_primary[b];
        bank_loaded_from_primary[b] = tmp_flag;
    }
    if (bank_loaded_from_companion) {
        uint8_t tmp_flag = bank_loaded_from_companion[a];
        bank_loaded_from_companion[a] = bank_loaded_from_companion[b];
        bank_loaded_from_companion[b] = tmp_flag;
    }
    if (bank_loaded_from_hint) {
        uint8_t tmp_flag = bank_loaded_from_hint[a];
        bank_loaded_from_hint[a] = bank_loaded_from_hint[b];
        bank_loaded_from_hint[b] = tmp_flag;
    }
    if (bank_mirrored_from) {
        uint8_t tmp_origin = bank_mirrored_from[a];
        bank_mirrored_from[a] = bank_mirrored_from[b];
        bank_mirrored_from[b] = tmp_origin;
        for (size_t i = 0; i < 4u; ++i) {
            if (i == a || i == b) {
                continue;
            }
            if (bank_mirrored_from[i] == (uint8_t)a) {
                bank_mirrored_from[i] = (uint8_t)b;
            } else if (bank_mirrored_from[i] == (uint8_t)b) {
                bank_mirrored_from[i] = (uint8_t)a;
            }
        }
    }
    if (bank_matches_basic) {
        uint8_t tmp_flag = bank_matches_basic[a];
        bank_matches_basic[a] = bank_matches_basic[b];
        bank_matches_basic[b] = tmp_flag;
    }
    if (bank_matches_menu) {
        uint8_t tmp_flag = bank_matches_menu[a];
        bank_matches_menu[a] = bank_matches_menu[b];
        bank_matches_menu[b] = tmp_flag;
    }
}

static size_t spectrum_expected_rom_banks(SpectrumModel model) {
    switch (model) {
        case SPECTRUM_MODEL_128K:
            return 2u;
        case SPECTRUM_MODEL_PLUS2A:
        case SPECTRUM_MODEL_PLUS3:
            return 4u;
        case SPECTRUM_MODEL_48K:
        default:
            return 1u;
    }
}

static int spectrum_load_rom_bank_file(const char *path, uint8_t *dest) {
    if (!path || !dest) {
        return 0;
    }

    FILE *bank_file = fopen(path, "rb");
    if (!bank_file) {
        return 0;
    }

    memset(dest, 0, 0x4000);
    size_t read = fread(dest, 1, 0x4000, bank_file);
    fclose(bank_file);

    if (read < 0x4000) {
        fprintf(stderr,
                "ROM bank '%s' is %zu bytes; expected at least 16384 bytes\n",
                path,
                read);
        if (read == 0) {
            return 0;
        }
    }

    if (read > 0x4000) {
        fprintf(stderr,
                "ROM bank '%s' is larger than 16384 bytes; extra data will be ignored\n",
                path);
    }

    return 1;
}

static int spectrum_populate_rom_pages(const char *rom_primary_path,
                                       const uint8_t *rom_buffer,
                                       size_t rom_size) {
    if (!rom_buffer || rom_size < 0x4000) {
        return 0;
    }

    size_t expected_banks = spectrum_expected_rom_banks(spectrum_model);
    if (expected_banks == 0u) {
        expected_banks = 1u;
    }
    if (expected_banks > 4u) {
        expected_banks = 4u;
    }

    const char *rom_label = rom_primary_path ? rom_primary_path : "<buffer>";
    printf("Preparing ROM layout for model %d using '%s' (%zu expected bank%s)\n",
           (int)spectrum_model,
           rom_label,
           expected_banks,
           expected_banks == 1u ? "" : "s");

    char rom_stem[PATH_MAX];
    char rom_extension[PATH_MAX];
    rom_stem[0] = '\0';
    rom_extension[0] = '\0';

    if (rom_primary_path) {
        size_t path_len = strlen(rom_primary_path);
        if (path_len >= PATH_MAX) {
            path_len = PATH_MAX - 1u;
        }

        const char *last_slash = strrchr(rom_primary_path, '/');
        const char *last_backslash = strrchr(rom_primary_path, '\\');
        if (!last_slash || (last_backslash && last_backslash > last_slash)) {
            last_slash = last_backslash;
        }

        const char *extension = strrchr(rom_primary_path, '.');
        if (extension && last_slash && extension < last_slash) {
            extension = NULL;
        }

        size_t stem_len = extension ? (size_t)(extension - rom_primary_path) : path_len;
        if (stem_len >= PATH_MAX) {
            stem_len = PATH_MAX - 1u;
        }
        memcpy(rom_stem, rom_primary_path, stem_len);
        rom_stem[stem_len] = '\0';

        if (extension) {
            size_t ext_len = strlen(extension);
            if (ext_len >= PATH_MAX) {
                ext_len = PATH_MAX - 1u;
            }
            memcpy(rom_extension, extension, ext_len);
            rom_extension[ext_len] = '\0';
        }
    }

    size_t stem_length = strlen(rom_stem);
    int bank_hint = -1;
    char separator_hint = '-';
    if (stem_length >= 2u) {
        char sep_candidate = rom_stem[stem_length - 2u];
        char digit_candidate = rom_stem[stem_length - 1u];
        if ((sep_candidate == '-' || sep_candidate == '_') && isdigit((unsigned char)digit_candidate)) {
            int digit_value = digit_candidate - '0';
            if (digit_value >= 0 && digit_value <= 9) {
                bank_hint = digit_value;
                separator_hint = sep_candidate;
                stem_length -= 2u;
                rom_stem[stem_length] = '\0';
            }
        }
    }

    if (bank_hint < 0 || bank_hint > 3) {
        bank_hint = -1;
    }

    if (bank_hint >= 0) {
        printf("Primary ROM filename supplies numeric bank hint %d (separator '%c')\n",
               bank_hint,
               separator_hint);
    } else {
        printf("Primary ROM filename did not contain a numeric bank hint\n");
    }

    memset(rom_pages, 0, sizeof(rom_pages));
    uint8_t bank_loaded[4] = {0, 0, 0, 0};
    uint8_t bank_loaded_from_hint[4] = {0, 0, 0, 0};
    uint8_t bank_loaded_from_primary[4] = {0, 0, 0, 0};
    uint8_t bank_loaded_from_companion[4] = {0, 0, 0, 0};
    uint8_t bank_mirrored_from[4];
    uint8_t bank_matches_basic[4] = {0, 0, 0, 0};
    uint8_t bank_matches_menu[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < 4u; ++i) {
        bank_mirrored_from[i] = 0xFFu;
    }

    size_t dest_order[4];
    size_t dest_count = 0u;
    if (bank_hint >= 0) {
        dest_order[dest_count++] = (size_t)bank_hint;
        for (size_t bank = 0; bank < 4u; ++bank) {
            if ((int)bank == bank_hint) {
                continue;
            }
            dest_order[dest_count++] = bank;
        }
    } else {
        for (size_t bank = 0; bank < 4u; ++bank) {
            dest_order[dest_count++] = bank;
        }
    }

    size_t buffer_offset = 0u;
    size_t chunk_index = 0u;
    while (buffer_offset + 0x4000u <= rom_size && chunk_index < dest_count) {
        size_t dest_index = dest_order[chunk_index];
        memcpy(rom_pages[dest_index], rom_buffer + buffer_offset, 0x4000u);
        bank_loaded[dest_index] = 1u;
        bank_loaded_from_primary[dest_index] = 1u;
        bank_mirrored_from[dest_index] = (uint8_t)dest_index;
        if (chunk_index == 0u && bank_hint >= 0 && dest_index == (size_t)bank_hint) {
            bank_loaded_from_hint[dest_index] = 1u;
        }
        printf("Primary ROM chunk %zu copied into bank %zu (0x%04zX bytes)%s\n",
               chunk_index,
               dest_index,
               (size_t)0x4000u,
               (bank_hint >= 0 && dest_index == (size_t)bank_hint) ? " [hinted]" : "");
        buffer_offset += 0x4000u;
        ++chunk_index;
    }

    if (buffer_offset < rom_size && chunk_index < dest_count) {
        size_t remaining = rom_size - buffer_offset;
        if (remaining > 0x4000u) {
            remaining = 0x4000u;
        }
        size_t dest_index = dest_order[chunk_index];
        memcpy(rom_pages[dest_index], rom_buffer + buffer_offset, remaining);
        if (remaining < 0x4000u) {
            memset(rom_pages[dest_index] + remaining, 0, 0x4000u - remaining);
        }
        bank_loaded[dest_index] = 1u;
        bank_loaded_from_primary[dest_index] = 1u;
        bank_mirrored_from[dest_index] = (uint8_t)dest_index;
        if (chunk_index == 0u && bank_hint >= 0 && dest_index == (size_t)bank_hint) {
            bank_loaded_from_hint[dest_index] = 1u;
        }
        printf("Primary ROM tail chunk copied into bank %zu (%zu bytes)%s\n",
               dest_index,
               remaining,
               (bank_hint >= 0 && dest_index == (size_t)bank_hint) ? " [hinted]" : "");
    }

    size_t current_loaded = 0u;
    for (size_t i = 0; i < 4u; ++i) {
        if (bank_loaded[i]) {
            ++current_loaded;
        }
    }

    char separator_candidates[3];
    size_t separator_count = 0u;
    separator_candidates[separator_count++] = separator_hint;
    if (separator_hint != '-') {
        separator_candidates[separator_count++] = '-';
    }
    if (separator_hint != '_' && separator_count < 3u) {
        separator_candidates[separator_count++] = '_';
    }

    if (rom_stem[0] == '\0' && rom_primary_path) {
        size_t path_len = strlen(rom_primary_path);
        if (path_len >= PATH_MAX) {
            path_len = PATH_MAX - 1u;
        }
        memcpy(rom_stem, rom_primary_path, path_len);
        rom_stem[path_len] = '\0';
        stem_length = strlen(rom_stem);
    } else {
        stem_length = strlen(rom_stem);
    }

    size_t banks_needed = expected_banks;
    if (banks_needed == 0u) {
        banks_needed = 1u;
    }
    if (banks_needed > 4u) {
        banks_needed = 4u;
    }

    char companion_path[PATH_MAX];
    if (current_loaded < banks_needed && rom_stem[0] != '\0') {
        for (size_t sep_index = 0; sep_index < separator_count && current_loaded < banks_needed; ++sep_index) {
            char separator = separator_candidates[sep_index];
            if (separator == '\0') {
                continue;
            }
            for (size_t bank = 0; bank < banks_needed && current_loaded < banks_needed; ++bank) {
                if (bank_loaded[bank]) {
                    continue;
                }
                int written = snprintf(companion_path,
                                       sizeof(companion_path),
                                       "%s%c%zu%s",
                                       rom_stem,
                                       separator,
                                       bank,
                                       rom_extension);
                if (written <= 0 || (size_t)written >= sizeof(companion_path)) {
                    continue;
                }
                if (spectrum_load_rom_bank_file(companion_path, rom_pages[bank])) {
                    bank_loaded[bank] = 1u;
                    bank_loaded_from_hint[bank] = 1u;
                    bank_loaded_from_companion[bank] = 1u;
                    bank_mirrored_from[bank] = (uint8_t)bank;
                    ++current_loaded;
                    printf("Loaded ROM bank %zu from %s\n", bank, companion_path);
                }
            }
        }
    }

    size_t inspect_limit = banks_needed;
    if (inspect_limit > 4u) {
        inspect_limit = 4u;
    }

    int hints_cover_all = 1;
    for (size_t bank = 0; bank < inspect_limit; ++bank) {
        if (!bank_loaded[bank] || !bank_loaded_from_hint[bank]) {
            hints_cover_all = 0;
            break;
        }
    }

    int enforce_pair_order = (expected_banks <= 2u);
    if (hints_cover_all) {
        if (enforce_pair_order && inspect_limit >= 2u) {
            printf("All required ROM banks arrived with numeric hints; verifying canonical order\n");
        } else {
            printf("All required ROM banks arrived with numeric hints; skipping heuristic reordering\n");
        }
    }

    if (inspect_limit >= 2u && enforce_pair_order) {
        if (!hints_cover_all) {
            printf("ROM bank hints incomplete; analysing signatures to find menu and BASIC banks\n");
        }
        int menu_candidate = -1;
        int basic_candidate = -1;
        size_t non_basic_candidates[4];
        size_t non_basic_count = 0u;
        for (size_t bank = 0; bank < inspect_limit; ++bank) {
            if (!bank_loaded[bank]) {
                continue;
            }

            if (spectrum_rom_bank_seems_48k_basic(rom_pages[bank])) {
                if (!bank_matches_basic[bank]) {
                    printf("ROM bank %zu matches 48K BASIC signature\n", bank);
                    bank_matches_basic[bank] = 1u;
                }
                if (basic_candidate < 0) {
                    basic_candidate = (int)bank;
                }
                continue;
            }

            if (menu_candidate < 0 && spectrum_rom_bank_seems_128k_menu(rom_pages[bank])) {
                if (!bank_matches_menu[bank]) {
                    printf("ROM bank %zu matches 128K menu signature\n", bank);
                    bank_matches_menu[bank] = 1u;
                }
                menu_candidate = (int)bank;
            }

            if (non_basic_count < sizeof(non_basic_candidates) / sizeof(non_basic_candidates[0])) {
                non_basic_candidates[non_basic_count++] = bank;
            }
        }

        if (!hints_cover_all && menu_candidate < 0 && basic_candidate >= 0 && non_basic_count > 0u) {
            menu_candidate = (int)non_basic_candidates[0];
            printf("Assuming ROM bank %d is the 128K menu (non-48K companion)\n", menu_candidate);
        }

        if (menu_candidate >= 0 && menu_candidate != 0) {
            int original_menu = menu_candidate;
            spectrum_swap_rom_banks(0u,
                                     (size_t)menu_candidate,
                                     bank_loaded,
                                     bank_loaded_from_primary,
                                     bank_loaded_from_companion,
                                     bank_loaded_from_hint,
                                     bank_mirrored_from,
                                     bank_matches_basic,
                                     bank_matches_menu);
            const char *note = hints_cover_all ? " (overrode numeric hints)" : "";
            printf("Reordered ROM bank %d into slot 0 for 128K menu%s\n", original_menu, note);
            if (basic_candidate == 0) {
                basic_candidate = original_menu;
            } else if (basic_candidate == original_menu) {
                basic_candidate = 0;
            }
            menu_candidate = 0;
        }

        size_t desired_basic_slot = 1u;
        if (basic_candidate >= 0 && inspect_limit > desired_basic_slot && (size_t)basic_candidate != desired_basic_slot) {
            int original_basic = basic_candidate;
            spectrum_swap_rom_banks(desired_basic_slot,
                                     (size_t)basic_candidate,
                                     bank_loaded,
                                     bank_loaded_from_primary,
                                     bank_loaded_from_companion,
                                     bank_loaded_from_hint,
                                     bank_mirrored_from,
                                     bank_matches_basic,
                                     bank_matches_menu);
            const char *note = hints_cover_all ? " (overrode numeric hints)" : "";
            printf("Reordered ROM bank %d into slot %zu for 48K BASIC%s\n",
                   original_basic,
                   desired_basic_slot,
                   note);
            if (menu_candidate == (int)desired_basic_slot) {
                menu_candidate = original_basic;
            } else if (menu_candidate == original_basic) {
                menu_candidate = (int)desired_basic_slot;
            }
            basic_candidate = (int)desired_basic_slot;
        }
    }

    int fallback_bank = -1;
    for (size_t i = 0; i < 4u; ++i) {
        if (bank_loaded[i]) {
            fallback_bank = (int)i;
            break;
        }
    }

    if (fallback_bank < 0) {
        fprintf(stderr, "Failed to populate any ROM bank from '%s'\n", rom_primary_path ? rom_primary_path : "<buffer>");
        return 0;
    }

    if (current_loaded < banks_needed) {
        fprintf(stderr,
                "Warning: only %zu of %zu ROM banks were found for '%s'; mirroring bank %d\n",
                current_loaded,
                banks_needed,
                rom_label,
                fallback_bank);
        for (size_t bank = 0; bank < banks_needed; ++bank) {
            if (!bank_loaded[bank]) {
                memcpy(rom_pages[bank], rom_pages[fallback_bank], 0x4000);
                bank_loaded[bank] = 1u;
                bank_mirrored_from[bank] = (uint8_t)fallback_bank;
            }
        }
        current_loaded = banks_needed;
    }

    size_t final_bank_count = banks_needed;
    if (current_loaded > final_bank_count) {
        final_bank_count = current_loaded;
        if (final_bank_count > 4u) {
            final_bank_count = 4u;
        }
    }

    if (final_bank_count == 0u) {
        final_bank_count = 1u;
    }

    for (size_t bank = final_bank_count; bank < 4u; ++bank) {
        size_t mirror = bank % final_bank_count;
        memcpy(rom_pages[bank], rom_pages[mirror], 0x4000);
        bank_mirrored_from[bank] = (uint8_t)mirror;
    }

    rom_page_count = (uint8_t)final_bank_count;

    for (size_t bank = 0; bank < final_bank_count; ++bank) {
        if (!bank_loaded[bank]) {
            bank_matches_basic[bank] = 0u;
            bank_matches_menu[bank] = 0u;
            continue;
        }
        bank_matches_basic[bank] = spectrum_rom_bank_seems_48k_basic(rom_pages[bank]) ? 1u : 0u;
        bank_matches_menu[bank] = spectrum_rom_bank_seems_128k_menu(rom_pages[bank]) ? 1u : 0u;
    }

    printf("Final ROM bank layout (%zu active bank%s, ROM page count %u):\n",
           final_bank_count,
           final_bank_count == 1u ? "" : "s",
           (unsigned int)rom_page_count);
    for (size_t bank = 0; bank < final_bank_count; ++bank) {
        const char *source;
        char source_detail[64];
        if (bank_loaded_from_primary[bank]) {
            source = "primary image";
            source_detail[0] = '\0';
        } else if (bank_loaded_from_companion[bank]) {
            source = "companion image";
            source_detail[0] = '\0';
        } else if (bank_mirrored_from[bank] != 0xFFu && bank_mirrored_from[bank] != bank) {
            source = "mirrored";
            snprintf(source_detail,
                     sizeof(source_detail),
                     " (from bank %u)",
                     (unsigned int)bank_mirrored_from[bank]);
        } else if (bank_mirrored_from[bank] == bank) {
            source = "initialised";
            source_detail[0] = '\0';
        } else {
            source = "uninitialised";
            source_detail[0] = '\0';
        }

        const char *classification = "no signature";
        if (bank_matches_menu[bank] || spectrum_rom_bank_seems_128k_menu(rom_pages[bank])) {
            classification = "128K menu signature";
        } else if (bank_matches_basic[bank] || spectrum_rom_bank_seems_48k_basic(rom_pages[bank])) {
            classification = "48K BASIC signature";
        }

        printf("  bank %zu: %s%s%s\n",
               bank,
               source,
               source_detail,
               bank_loaded_from_hint[bank] ? " [hinted]" : "");
        printf("    signature: %s\n", classification);
    }

    return 1;
}

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
int cpu_nmi(Z80* cpu);
int cpu_ddfd_cb_step(Z80* cpu, uint16_t* index_reg, int is_ix);
void audio_callback(void* userdata, Uint8* stream, int len);
static void border_record_event(uint64_t event_t_state, uint8_t color_idx);
static void border_draw_span(uint64_t span_start, uint64_t span_end, uint8_t color_idx);
static void spectrum_map_page(int segment, SpectrumMemoryPageType type, uint8_t index);
static void spectrum_refresh_visible_ram(void);
static void spectrum_apply_memory_configuration(void);
static void spectrum_update_contention_flags(void);
static inline int ula_contention_penalty(uint64_t t_state);
static void beeper_reset_audio_state(uint64_t current_t_state, int current_level);
static void beeper_set_latency_limit(double sample_limit);
static void beeper_push_event(uint64_t t_state, int level);
static size_t beeper_catch_up_to(double catch_up_position, double playback_position_snapshot);
static double beeper_current_latency_samples(void);
static double beeper_latency_threshold(void);
static Uint32 beeper_recommended_throttle_delay(double latency_samples);
static int audio_dump_start(const char* path, uint32_t sample_rate, uint16_t channels);
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
static const uint8_t spectrum_contended_bank_masks[] = {
    [CONTENTION_PROFILE_48K] = (uint8_t)(1u << 5),
    [CONTENTION_PROFILE_128K] = (uint8_t)((1u << 1) | (1u << 3) | (1u << 5) | (1u << 7)),
    [CONTENTION_PROFILE_128K_PLUS2A] = (uint8_t)((1u << 4) | (1u << 5) | (1u << 6) | (1u << 7)),
    [CONTENTION_PROFILE_128K_PLUS3] = (uint8_t)((1u << 4) | (1u << 5) | (1u << 6) | (1u << 7))
};

static const int spectrum_contention_penalties[][8] = {
    [CONTENTION_PROFILE_48K]         = {6, 5, 4, 3, 2, 1, 0, 0},
    [CONTENTION_PROFILE_128K]        = {6, 5, 4, 3, 2, 1, 0, 0},
    [CONTENTION_PROFILE_128K_PLUS2A] = {0, 6, 5, 4, 3, 2, 1, 0},
    [CONTENTION_PROFILE_128K_PLUS3]  = {0, 6, 5, 4, 3, 2, 1, 0}
};

static inline void spectrum_reset_floating_bus(void) {
    floating_bus_last_value = 0xFFu;
}

static inline uint64_t spectrum_current_access_tstate(void) {
    if (ula_instruction_progress_ptr) {
        return ula_instruction_base_tstate + (uint64_t)(*ula_instruction_progress_ptr);
    }
    return total_t_states;
}

static inline int spectrum_is_ram_bank_contended(uint8_t bank) {
    if (bank >= 8u) {
        return 0;
    }
    uint8_t mask = spectrum_contended_bank_masks[spectrum_contention_profile];
    return (mask & (uint8_t)(1u << bank)) != 0u;
}

static void spectrum_map_page(int segment, SpectrumMemoryPageType type, uint8_t index) {
    if (segment < 0 || segment >= 4) {
        return;
    }

    SpectrumMemoryPage *slot = &spectrum_pages[segment];
    uint16_t base = (uint16_t)(segment * 0x4000u);
    if (slot->type == type && slot->index == index && slot->type != MEMORY_PAGE_NONE) {
        return;
    }

    if (slot->type == MEMORY_PAGE_RAM && slot->index < 8u) {
        memcpy(ram_pages[slot->index], memory + base, 0x4000);
    }

    if (type == MEMORY_PAGE_ROM) {
        uint8_t rom_limit = rom_page_count > 0u ? rom_page_count : 1u;
        uint8_t rom_index = (uint8_t)(index % rom_limit);
        memcpy(memory + base, rom_pages[rom_index], 0x4000);
        slot->type = MEMORY_PAGE_ROM;
        slot->index = rom_index;
    } else if (type == MEMORY_PAGE_RAM) {
        uint8_t ram_index = (uint8_t)(index % 8u);
        memcpy(memory + base, ram_pages[ram_index], 0x4000);
        slot->type = MEMORY_PAGE_RAM;
        slot->index = ram_index;
    } else {
        memset(memory + base, 0, 0x4000);
        slot->type = MEMORY_PAGE_NONE;
        slot->index = 0u;
    }
}

static void spectrum_refresh_visible_ram(void) {
    for (int segment = 0; segment < 4; ++segment) {
        SpectrumMemoryPage* slot = &spectrum_pages[segment];
        if (slot->type == MEMORY_PAGE_RAM && slot->index < 8u) {
            uint16_t base = (uint16_t)(segment * 0x4000u);
            memcpy(memory + base, ram_pages[slot->index], 0x4000u);
        }
    }
}

static void spectrum_update_contention_flags(void) {
    for (int segment = 0; segment < 4; ++segment) {
        uint8_t contended = 0u;
        if (spectrum_pages[segment].type == MEMORY_PAGE_RAM) {
            contended = (uint8_t)spectrum_is_ram_bank_contended(spectrum_pages[segment].index);
        }
        page_contended[segment] = contended;
    }
}

static void spectrum_set_contention_profile(SpectrumContentionProfile profile) {
    spectrum_contention_profile = profile;
    spectrum_update_contention_flags();
}

static void spectrum_set_peripheral_contention_profile(PeripheralContentionProfile profile) {
    peripheral_contention_profile = profile;
}

static inline void apply_port_contention(uint64_t access_t_state) {
    if (!ula_instruction_progress_ptr) {
        return;
    }

    int penalty = 0;
    switch (peripheral_contention_profile) {
        case PERIPHERAL_CONTENTION_NONE:
            return;
        case PERIPHERAL_CONTENTION_IF1:
            penalty = ula_contention_penalty(access_t_state);
            break;
        case PERIPHERAL_CONTENTION_PLUS3:
            penalty = ula_contention_penalty(access_t_state) + 3;
            break;
    }

    if (penalty > 0) {
        *ula_instruction_progress_ptr += penalty;
    }
}

static inline uint16_t spectrum_screen_pixel_offset(uint32_t y, uint32_t x_char) {
    return (uint16_t)(((y & 0xC0u) << 5) | ((y & 0x07u) << 8) | ((y & 0x38u) << 2) | x_char);
}

static inline uint16_t spectrum_screen_attr_offset(uint32_t y, uint32_t x_char) {
    return (uint16_t)(((y >> 3) * 32u) + x_char);
}

static uint8_t spectrum_sample_floating_bus(uint64_t access_t_state) {
    uint32_t phase = (uint32_t)(access_t_state % T_STATES_PER_FRAME);
    if (phase < 14336u || phase >= 57344u) {
        return floating_bus_last_value;
    }

    uint32_t display_phase = phase - 14336u;
    uint32_t line = display_phase / 224u;
    uint32_t line_phase = display_phase % 224u;

    if (line_phase < 48u || line_phase >= 176u) {
        return floating_bus_last_value;
    }

    uint32_t column_phase = line_phase - 48u;
    uint32_t sub_phase = column_phase & 3u;
    uint32_t x_char = column_phase >> 2;
    if (x_char >= 32u) {
        return floating_bus_last_value;
    }

    if (current_screen_bank >= 8u) {
        return floating_bus_last_value;
    }

    const uint8_t* vram_bank;
    const uint8_t* attr_bank;
    if (spectrum_pages[1].type == MEMORY_PAGE_RAM && spectrum_pages[1].index == current_screen_bank) {
        vram_bank = memory + VRAM_START;
        attr_bank = memory + ATTR_START;
    } else {
        const uint8_t* bank = ram_pages[current_screen_bank];
        vram_bank = bank;
        attr_bank = bank + (ATTR_START - VRAM_START);
    }

    uint8_t value;
    if ((sub_phase & 2u) == 0u) {
        uint16_t offset = spectrum_screen_pixel_offset(line, x_char);
        value = vram_bank[offset];
    } else {
        uint16_t offset = spectrum_screen_attr_offset(line, x_char);
        value = attr_bank[offset];
    }

    floating_bus_last_value = value;
    return value;
}

static inline int ula_contention_penalty(uint64_t t_state) {
    uint32_t phase = (uint32_t)(t_state % T_STATES_PER_FRAME);
    if (phase < 14336u || phase >= 57344u) {
        return 0;
    }
    const int* penalties = spectrum_contention_penalties[spectrum_contention_profile];
    return penalties[phase & 7u];
}

static void spectrum_apply_memory_configuration(void) {
    if (spectrum_model == SPECTRUM_MODEL_48K) {
        current_rom_page = 0u;
        current_screen_bank = 5u;
        current_paged_bank = 7u;
        spectrum_map_page(0, MEMORY_PAGE_ROM, 0u);
        spectrum_map_page(1, MEMORY_PAGE_RAM, 5u);
        spectrum_map_page(2, MEMORY_PAGE_RAM, 2u);
        spectrum_map_page(3, MEMORY_PAGE_RAM, 7u);
        spectrum_update_contention_flags();
        return;
    }

    int plus_model = (spectrum_model == SPECTRUM_MODEL_PLUS2A) || (spectrum_model == SPECTRUM_MODEL_PLUS3);
    if (!plus_model) {
        uint8_t desired_rom = (uint8_t)((gate_array_7ffd_state >> 4) & 0x01u);
        uint8_t rom_limit = rom_page_count > 0u ? rom_page_count : 1u;
        current_rom_page = (uint8_t)(desired_rom % rom_limit);
        current_screen_bank = (gate_array_7ffd_state & 0x08u) ? 7u : 5u;
        spectrum_map_page(0, MEMORY_PAGE_ROM, current_rom_page);
        spectrum_map_page(1, MEMORY_PAGE_RAM, 5u);
        spectrum_map_page(2, MEMORY_PAGE_RAM, 2u);
        spectrum_map_page(3, MEMORY_PAGE_RAM, current_paged_bank);
        spectrum_update_contention_flags();
        return;
    }

    uint8_t rom_low = (uint8_t)((gate_array_7ffd_state >> 4) & 0x01u);
    uint8_t rom_high = (uint8_t)(gate_array_1ffd_state & 0x01u);
    uint8_t desired_rom = (uint8_t)(((rom_high & 0x01u) << 1) | rom_low);
    uint8_t rom_limit = rom_page_count > 0u ? rom_page_count : 1u;
    current_rom_page = (uint8_t)(desired_rom % rom_limit);

    int special_paging = (gate_array_1ffd_state & 0x04u) != 0;
    int all_ram = !special_paging && ((gate_array_1ffd_state & 0x02u) != 0);

    if (special_paging) {
        static const uint8_t special_maps[4][4] = {
            {0u, 1u, 2u, 3u},
            {4u, 5u, 6u, 7u},
            {4u, 5u, 6u, 3u},
            {4u, 7u, 6u, 3u}
        };
        uint8_t config = (uint8_t)(gate_array_1ffd_state & 0x03u);
        const uint8_t* map = special_maps[config];
        spectrum_map_page(0, MEMORY_PAGE_RAM, map[0]);
        spectrum_map_page(1, MEMORY_PAGE_RAM, map[1]);
        spectrum_map_page(2, MEMORY_PAGE_RAM, map[2]);
        spectrum_map_page(3, MEMORY_PAGE_RAM, map[3]);
        if (spectrum_pages[1].type == MEMORY_PAGE_RAM) {
            current_screen_bank = spectrum_pages[1].index;
        } else {
            current_screen_bank = 5u;
        }
    } else {
        if (all_ram) {
            spectrum_map_page(0, MEMORY_PAGE_RAM, 0u);
        } else {
            spectrum_map_page(0, MEMORY_PAGE_ROM, current_rom_page);
        }
        spectrum_map_page(1, MEMORY_PAGE_RAM, 5u);
        spectrum_map_page(2, MEMORY_PAGE_RAM, 2u);
        spectrum_map_page(3, MEMORY_PAGE_RAM, current_paged_bank);
        current_screen_bank = (gate_array_7ffd_state & 0x08u) ? 7u : 5u;
    }

    spectrum_update_contention_flags();
}

static uint32_t spectrum_hash_buffer(const uint8_t* data, size_t length) {
    const uint32_t fnv_offset = 2166136261u;
    const uint32_t fnv_prime = 16777619u;
    uint32_t hash = fnv_offset;

    if (!data) {
        return hash;
    }

    for (size_t i = 0; i < length; ++i) {
        hash ^= data[i];
        hash *= fnv_prime;
    }

    return hash;
}

static void spectrum_log_ram_hashes(const char* reason) {
    if (!ram_hash_logging) {
        return;
    }

    const char* tag = reason ? reason : "ram";
    fprintf(stderr,
            "[RAM ] %s hashes 7FFD=%02X 1FFD=%02X disabled=%d screen=%u",
            tag,
            (unsigned)gate_array_7ffd_state,
            (unsigned)gate_array_1ffd_state,
            paging_disabled,
            (unsigned)current_screen_bank);

    for (int bank = 0; bank < 8; ++bank) {
        uint32_t hash = spectrum_hash_buffer(ram_pages[bank], 0x4000u);
        fprintf(stderr, " bank%u=%08X", bank, hash);
    }

    fputc('\n', stderr);
}

static void spectrum_configure_model(SpectrumModel model) {
    spectrum_model = model;
    paging_disabled = 0;
    gate_array_7ffd_state = 0u;
    gate_array_1ffd_state = 0u;
    ay_selected_register = 0u;
    ay_register_latched = 0;
    memset(ay_registers, 0, sizeof(ay_registers));
    ay_reset_state();
    current_screen_bank = 5u;
    current_rom_page = 0u;
    current_paged_bank = (model == SPECTRUM_MODEL_48K) ? 7u : 0u;
    for (int i = 0; i < 4; ++i) {
        spectrum_pages[i].type = MEMORY_PAGE_NONE;
        spectrum_pages[i].index = 0u;
    }
    if (model == SPECTRUM_MODEL_48K) {
        spectrum_contention_profile = CONTENTION_PROFILE_48K;
    } else if (model == SPECTRUM_MODEL_128K) {
        spectrum_contention_profile = CONTENTION_PROFILE_128K;
    } else if (model == SPECTRUM_MODEL_PLUS2A) {
        spectrum_contention_profile = CONTENTION_PROFILE_128K_PLUS2A;
    } else {
        spectrum_contention_profile = CONTENTION_PROFILE_128K_PLUS3;
    }

    if (model == SPECTRUM_MODEL_PLUS3) {
        peripheral_contention_profile = PERIPHERAL_CONTENTION_PLUS3;
    } else {
        peripheral_contention_profile = PERIPHERAL_CONTENTION_NONE;
    }
    spectrum_reset_floating_bus();
    spectrum_apply_memory_configuration();
    spectrum_log_paging_state("model configure", 0u, 0u, total_t_states);
}

static inline void apply_memory_contention(uint16_t addr) {
    if (!ula_instruction_progress_ptr) {
        return;
    }
    uint16_t page = (uint16_t)(addr >> 14);
    if (page > 3u) {
        return;
    }
    if (!page_contended[page]) {
        return;
    }
    uint64_t access_t_state = spectrum_current_access_tstate();
    int penalty = ula_contention_penalty(access_t_state);
    if (penalty > 0) {
        *ula_instruction_progress_ptr += penalty;
    }
}

static inline void spectrum_write_ram_shadow(uint16_t addr, uint8_t val) {
    uint16_t page = (uint16_t)(addr >> 14);
    if (page >= 4u) {
        return;
    }
    SpectrumMemoryPage *slot = &spectrum_pages[page];
    if (slot->type != MEMORY_PAGE_RAM || slot->index >= 8u) {
        return;
    }
    uint16_t base = (uint16_t)(page * 0x4000u);
    ram_pages[slot->index][addr - base] = val;
}

static void spectrum_map_rom_page(uint8_t page) {
    uint8_t rom_limit = rom_page_count > 0u ? rom_page_count : 1u;
    uint8_t desired = (uint8_t)(page % rom_limit);
    if (spectrum_model == SPECTRUM_MODEL_PLUS2A || spectrum_model == SPECTRUM_MODEL_PLUS3) {
        gate_array_7ffd_state = (uint8_t)((gate_array_7ffd_state & (uint8_t)~0x10u) | ((desired & 0x01u) << 4));
        gate_array_1ffd_state = (uint8_t)((gate_array_1ffd_state & (uint8_t)~0x01u) | ((desired >> 1) & 0x01u));
    } else {
        gate_array_7ffd_state = (uint8_t)((gate_array_7ffd_state & (uint8_t)~0x10u) | ((desired & 0x01u) << 4));
    }
    current_rom_page = desired;
    spectrum_pages[0].type = MEMORY_PAGE_NONE;
    spectrum_pages[0].index = 0u;
    spectrum_apply_memory_configuration();
}

static void spectrum_map_upper_bank(uint8_t new_bank) {
    if (spectrum_model == SPECTRUM_MODEL_48K) {
        return;
    }
    new_bank &= 0x07u;
    if (new_bank == current_paged_bank) {
        return;
    }
    current_paged_bank = new_bank;
    gate_array_7ffd_state = (uint8_t)((gate_array_7ffd_state & (uint8_t)~0x07u) | current_paged_bank);
    spectrum_apply_memory_configuration();
}

uint8_t readByte(uint16_t addr) {
    apply_memory_contention(addr);
    return memory[addr];
}

void writeByte(uint16_t addr, uint8_t val) {
    apply_memory_contention(addr);
    if (addr < 0x4000u) {
        return;
    }
    memory[addr] = val;
    spectrum_write_ram_shadow(addr, val);
}

uint16_t readWord(uint16_t addr) {
    uint8_t lo = readByte(addr);
    uint8_t hi = readByte((uint16_t)(addr + 1u));
    return (uint16_t)((hi << 8) | lo);
}

void writeWord(uint16_t addr, uint16_t val) {
    uint8_t lo = (uint8_t)(val & 0xFFu);
    uint8_t hi = (uint8_t)((val >> 8) & 0xFFu);
    writeByte(addr, lo);
    writeByte((uint16_t)(addr + 1u), hi);
}

// --- I/O Port Access Helpers ---
uint8_t border_color_idx = 0;

typedef struct {
    uint64_t t_state;
    uint8_t color_idx;
} BorderColorEvent;

static BorderColorEvent border_color_events[BORDER_EVENT_CAPACITY];
static size_t border_color_event_count = 0;
static uint64_t border_frame_start_tstate = 0;
static uint8_t border_frame_color = 0;

static void beeper_reset_audio_state(uint64_t current_t_state, int current_level) {
    beeper_event_head = 0;
    beeper_event_tail = 0;
    beeper_last_event_t_state = current_t_state;
    beeper_playback_position = (double)current_t_state;
    beeper_writer_cursor = (double)current_t_state;
    beeper_playback_level = current_level;
    double baseline = (double)current_level * (double)AUDIO_AMPLITUDE;
    beeper_hp_last_input = baseline;
    beeper_hp_last_output = 0.0;
    beeper_idle_log_active = 0;
}

static void beeper_force_resync(uint64_t sync_t_state) {
    double baseline = (double)beeper_playback_level * (double)AUDIO_AMPLITUDE;
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

static int audio_dump_start(const char* path, uint32_t sample_rate, uint16_t channels) {
    if (!path || channels == 0u) {
        return 0;
    }

    audio_dump_file = fopen(path, "wb");
    if (!audio_dump_file) {
        fprintf(stderr, "[BEEPER] failed to open audio dump '%s': %s\n", path, strerror(errno));
        return 0;
    }

    audio_dump_data_bytes = 0;
    audio_dump_channels = channels;

    uint8_t header[44];
    memset(header, 0, sizeof(header));

    memcpy(header, "RIFF", 4);
    audio_dump_write_uint32(header + 4, 36u);
    memcpy(header + 8, "WAVE", 4);
    memcpy(header + 12, "fmt ", 4);
    audio_dump_write_uint32(header + 16, 16u); // PCM chunk size
    audio_dump_write_uint16(header + 20, 1u);  // PCM format
    audio_dump_write_uint16(header + 22, channels);
    audio_dump_write_uint32(header + 24, sample_rate);
    uint32_t byte_rate = sample_rate * channels * (uint32_t)sizeof(Sint16);
    audio_dump_write_uint32(header + 28, byte_rate);
    uint16_t block_align = (uint16_t)(channels * (uint16_t)sizeof(Sint16));
    audio_dump_write_uint16(header + 32, block_align);
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

    tape_log("Block %zu: type=%d length=%u pause=%u", index, (int)block->type, block->length, block->pause_ms);

    switch (block->type) {
        case TAPE_BLOCK_TYPE_STANDARD:
        case TAPE_BLOCK_TYPE_TURBO:
        case TAPE_BLOCK_TYPE_PURE_DATA:
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
            return;
        case TAPE_BLOCK_TYPE_PURE_TONE:
            tape_log(" tone pulses=%u length=%u\n",
                     (unsigned)block->tone_pulse_count,
                     (unsigned)block->tone_pulse_tstates);
            return;
        case TAPE_BLOCK_TYPE_PULSE_SEQUENCE:
            tape_log(" pulse-seq count=%zu\n", block->pulse_sequence_count);
            return;
        case TAPE_BLOCK_TYPE_DIRECT_RECORDING:
            tape_log(" direct samples=%u tstates/sample=%u\n",
                     (unsigned)block->direct_sample_count,
                     (unsigned)block->direct_tstates_per_sample);
            return;
    }
}

static void tape_free_image(TapeImage* image) {
    if (!image) {
        return;
    }
    if (image->blocks) {
        for (size_t i = 0; i < image->count; ++i) {
            free(image->blocks[i].data);
            image->blocks[i].data = NULL;
            free(image->blocks[i].pulse_sequence_durations);
            image->blocks[i].pulse_sequence_durations = NULL;
            free(image->blocks[i].direct_samples);
            image->blocks[i].direct_samples = NULL;
        }
        free(image->blocks);
    }
    image->blocks = NULL;
    image->count = 0;
    image->capacity = 0;
}

static TapeBlock* tape_image_new_block(TapeImage* image) {
    if (!image) {
        return NULL;
    }

    if (image->count == image->capacity) {
        size_t new_capacity = image->capacity ? image->capacity * 2 : 8;
        TapeBlock* new_blocks = (TapeBlock*)realloc(image->blocks, new_capacity * sizeof(TapeBlock));
        if (!new_blocks) {
            return NULL;
        }
        image->blocks = new_blocks;
        image->capacity = new_capacity;
    }

    TapeBlock* block = &image->blocks[image->count];
    block->type = TAPE_BLOCK_TYPE_STANDARD;
    block->length = 0u;
    block->pause_ms = 0u;
    block->data = NULL;
    block->used_bits_in_last_byte = 8u;
    block->pilot_pulse_tstates = (uint16_t)TAPE_PILOT_PULSE_TSTATES;
    block->pilot_pulse_count = 0u;
    block->sync_first_pulse_tstates = (uint16_t)TAPE_SYNC_FIRST_PULSE_TSTATES;
    block->sync_second_pulse_tstates = (uint16_t)TAPE_SYNC_SECOND_PULSE_TSTATES;
    block->bit0_first_pulse_tstates = (uint16_t)TAPE_BIT0_PULSE_TSTATES;
    block->bit0_second_pulse_tstates = (uint16_t)TAPE_BIT0_PULSE_TSTATES;
    block->bit1_first_pulse_tstates = (uint16_t)TAPE_BIT1_PULSE_TSTATES;
    block->bit1_second_pulse_tstates = (uint16_t)TAPE_BIT1_PULSE_TSTATES;
    block->tone_pulse_tstates = 0u;
    block->tone_pulse_count = 0u;
    block->pulse_sequence_durations = NULL;
    block->pulse_sequence_count = 0u;
    block->direct_tstates_per_sample = 0u;
    block->direct_samples = NULL;
    block->direct_sample_count = 0u;
    block->direct_initial_level = 1;
    return block;
}

static int tape_image_add_block(TapeImage* image, const uint8_t* data, uint32_t length, uint32_t pause_ms) {
    TapeBlock* block = tape_image_new_block(image);
    if (!block) {
        return 0;
    }

    block->type = TAPE_BLOCK_TYPE_STANDARD;
    block->length = length;
    block->pause_ms = pause_ms;

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

static int tape_image_add_turbo_block(TapeImage* image,
                                      const uint8_t* data,
                                      uint32_t length,
                                      uint32_t pause_ms,
                                      uint16_t pilot_pulse_tstates,
                                      uint32_t pilot_pulses,
                                      uint16_t sync1_tstates,
                                      uint16_t sync2_tstates,
                                      uint16_t bit0_first,
                                      uint16_t bit0_second,
                                      uint16_t bit1_first,
                                      uint16_t bit1_second,
                                      uint8_t used_bits) {
    TapeBlock* block = tape_image_new_block(image);
    if (!block) {
        return 0;
    }

    block->type = TAPE_BLOCK_TYPE_TURBO;
    block->length = length;
    block->pause_ms = pause_ms;
    block->pilot_pulse_tstates = pilot_pulse_tstates;
    block->pilot_pulse_count = pilot_pulses;
    block->sync_first_pulse_tstates = sync1_tstates;
    block->sync_second_pulse_tstates = sync2_tstates;
    block->bit0_first_pulse_tstates = bit0_first;
    block->bit0_second_pulse_tstates = bit0_second;
    block->bit1_first_pulse_tstates = bit1_first;
    block->bit1_second_pulse_tstates = bit1_second;
    block->used_bits_in_last_byte = (used_bits == 0u) ? 8u : used_bits;

    if (length > 0u) {
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

static int tape_image_add_pure_data_block(TapeImage* image,
                                          const uint8_t* data,
                                          uint32_t length,
                                          uint32_t pause_ms,
                                          uint16_t bit0_first,
                                          uint16_t bit0_second,
                                          uint16_t bit1_first,
                                          uint16_t bit1_second,
                                          uint8_t used_bits) {
    return tape_image_add_turbo_block(image,
                                      data,
                                      length,
                                      pause_ms,
                                      0u,
                                      0u,
                                      0u,
                                      0u,
                                      bit0_first,
                                      bit0_second,
                                      bit1_first,
                                      bit1_second,
                                      used_bits);
}

static int tape_image_add_pure_tone_block(TapeImage* image,
                                          uint16_t pulse_tstates,
                                          uint32_t pulse_count,
                                          uint32_t pause_ms) {
    TapeBlock* block = tape_image_new_block(image);
    if (!block) {
        return 0;
    }

    block->type = TAPE_BLOCK_TYPE_PURE_TONE;
    block->tone_pulse_tstates = pulse_tstates;
    block->tone_pulse_count = pulse_count;
    block->pause_ms = pause_ms;

    tape_log_block_summary(block, image->count);
    image->count++;
    return 1;
}

static int tape_image_add_pulse_sequence_block(TapeImage* image,
                                               const uint16_t* durations,
                                               size_t count,
                                               uint32_t pause_ms) {
    TapeBlock* block = tape_image_new_block(image);
    if (!block) {
        return 0;
    }

    block->type = TAPE_BLOCK_TYPE_PULSE_SEQUENCE;
    block->pause_ms = pause_ms;

    if (count > 0u) {
        size_t bytes = count * sizeof(uint16_t);
        block->pulse_sequence_durations = (uint16_t*)malloc(bytes);
        if (!block->pulse_sequence_durations) {
            return 0;
        }
        memcpy(block->pulse_sequence_durations, durations, bytes);
        block->pulse_sequence_count = count;
    }

    tape_log_block_summary(block, image->count);
    image->count++;
    return 1;
}

static int tape_image_add_direct_recording_block(TapeImage* image,
                                                 uint32_t tstates_per_sample,
                                                 const uint8_t* samples,
                                                 uint32_t sample_bytes,
                                                 uint8_t used_bits,
                                                 uint32_t pause_ms) {
    TapeBlock* block = tape_image_new_block(image);
    if (!block) {
        return 0;
    }

    block->type = TAPE_BLOCK_TYPE_DIRECT_RECORDING;
    block->pause_ms = pause_ms;
    block->direct_tstates_per_sample = tstates_per_sample;
    block->used_bits_in_last_byte = (used_bits == 0u) ? 8u : used_bits;

    if (sample_bytes > 0u) {
        block->direct_samples = (uint8_t*)malloc(sample_bytes);
        if (!block->direct_samples) {
            return 0;
        }
        memcpy(block->direct_samples, samples, sample_bytes);
    }

    uint32_t total_bits = sample_bytes * 8u;
    if (sample_bytes > 0u && block->used_bits_in_last_byte < 8u) {
        total_bits -= (uint32_t)(8u - block->used_bits_in_last_byte);
    }
    block->direct_sample_count = total_bits;
    if (block->direct_sample_count > 0u && block->direct_samples) {
        block->direct_initial_level = (block->direct_samples[0] & 0x80u) ? 1 : 0;
    } else {
        block->direct_initial_level = 0;
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

static int tape_waveform_add_with_pending(TapeWaveform* waveform,
                                          uint64_t duration,
                                          uint64_t* pending_silence) {
    if (pending_silence && *pending_silence) {
        duration += *pending_silence;
        *pending_silence = 0;
    }
    return tape_waveform_add_pulse(waveform, duration);
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
    if (waveform->count == 0) {
        waveform->initial_level = 1;
    }
    for (size_t block_index = 0; block_index < image->count; ++block_index) {
        const TapeBlock* block = &image->blocks[block_index];
        switch (block->type) {
            case TAPE_BLOCK_TYPE_STANDARD:
            case TAPE_BLOCK_TYPE_TURBO:
            case TAPE_BLOCK_TYPE_PURE_DATA: {
                uint32_t pilot_count = 0u;
                uint16_t pilot_pulse = block->pilot_pulse_tstates;
                uint16_t sync1 = block->sync_first_pulse_tstates;
                uint16_t sync2 = block->sync_second_pulse_tstates;
                uint16_t bit0_first = block->bit0_first_pulse_tstates;
                uint16_t bit0_second = block->bit0_second_pulse_tstates;
                uint16_t bit1_first = block->bit1_first_pulse_tstates;
                uint16_t bit1_second = block->bit1_second_pulse_tstates;

                if (block->type == TAPE_BLOCK_TYPE_STANDARD) {
                    if (block->length > 0 && block->data && block->data[0] == 0x00) {
                        pilot_count = (uint32_t)TAPE_HEADER_PILOT_COUNT;
                    } else {
                        pilot_count = (uint32_t)TAPE_DATA_PILOT_COUNT;
                    }
                    pilot_pulse = (uint16_t)TAPE_PILOT_PULSE_TSTATES;
                    sync1 = (uint16_t)TAPE_SYNC_FIRST_PULSE_TSTATES;
                    sync2 = (uint16_t)TAPE_SYNC_SECOND_PULSE_TSTATES;
                    bit0_first = (uint16_t)TAPE_BIT0_PULSE_TSTATES;
                    bit0_second = (uint16_t)TAPE_BIT0_PULSE_TSTATES;
                    bit1_first = (uint16_t)TAPE_BIT1_PULSE_TSTATES;
                    bit1_second = (uint16_t)TAPE_BIT1_PULSE_TSTATES;
                } else {
                    pilot_count = block->pilot_pulse_count;
                }

                for (uint32_t pulse = 0; pulse < pilot_count; ++pulse) {
                    uint64_t duration = (uint64_t)pilot_pulse;
                    if (!tape_waveform_add_with_pending(waveform, duration, &pending_silence)) {
                        return 0;
                    }
                }

                if (sync1 > 0u) {
                    if (!tape_waveform_add_with_pending(waveform, (uint64_t)sync1, &pending_silence)) {
                        return 0;
                    }
                }

                if (sync2 > 0u) {
                    if (!tape_waveform_add_pulse(waveform, (uint64_t)sync2)) {
                        return 0;
                    }
                }

                if (block->length > 0 && block->data) {
                    for (uint32_t byte_index = 0; byte_index < block->length; ++byte_index) {
                        uint8_t value = block->data[byte_index];
                        uint8_t mask = 0x80u;
                        uint8_t bits_to_emit = 8u;
                        if (byte_index == block->length - 1u) {
                            uint8_t used_bits = block->used_bits_in_last_byte;
                            if (used_bits > 0u && used_bits < 8u) {
                                bits_to_emit = used_bits;
                            }
                        }
                        for (uint8_t bit = 0u; bit < bits_to_emit; ++bit) {
                            int is_one = (value & mask) ? 1 : 0;
                            uint16_t first = is_one ? bit1_first : bit0_first;
                            uint16_t second = is_one ? bit1_second : bit0_second;
                            if (!tape_waveform_add_with_pending(waveform, (uint64_t)first, &pending_silence)) {
                                return 0;
                            }
                            if (!tape_waveform_add_pulse(waveform, (uint64_t)second)) {
                                return 0;
                            }
                            mask >>= 1;
                        }
                    }
                }
                break;
            }
            case TAPE_BLOCK_TYPE_PURE_TONE: {
                uint32_t pulse_count = block->tone_pulse_count;
                for (uint32_t i = 0; i < pulse_count; ++i) {
                    if (!tape_waveform_add_with_pending(waveform, (uint64_t)block->tone_pulse_tstates, &pending_silence)) {
                        return 0;
                    }
                }
                break;
            }
            case TAPE_BLOCK_TYPE_PULSE_SEQUENCE: {
                for (size_t i = 0; i < block->pulse_sequence_count; ++i) {
                    uint64_t duration = (uint64_t)block->pulse_sequence_durations[i];
                    if (!tape_waveform_add_with_pending(waveform, duration, &pending_silence)) {
                        return 0;
                    }
                }
                break;
            }
            case TAPE_BLOCK_TYPE_DIRECT_RECORDING: {
                if (block->direct_sample_count > 0u && block->direct_samples) {
                    uint32_t samples = block->direct_sample_count;
                    uint32_t tstates_per_sample = block->direct_tstates_per_sample ? block->direct_tstates_per_sample : 1u;
                    int current_level = block->direct_initial_level ? 1 : 0;
                    if (waveform->count == 0) {
                        waveform->initial_level = current_level;
                    }
                    uint64_t run_length = 0;
                    for (uint32_t sample_index = 0; sample_index < samples; ++sample_index) {
                        uint32_t byte_index = sample_index / 8u;
                        uint32_t bit_index = 7u - (sample_index % 8u);
                        int level = (block->direct_samples[byte_index] >> bit_index) & 0x01;
                        if (sample_index == 0) {
                            current_level = level;
                            run_length = (uint64_t)tstates_per_sample;
                            continue;
                        }
                        if (level == current_level) {
                            run_length += (uint64_t)tstates_per_sample;
                        } else {
                            if (!tape_waveform_add_with_pending(waveform, run_length, &pending_silence)) {
                                return 0;
                            }
                            current_level = level;
                            run_length = (uint64_t)tstates_per_sample;
                        }
                    }
                    pending_silence += run_length;
                }
                break;
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
            if (block_length > 0u) {
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
                if (!tape_image_add_block(image, buffer, block_length, pause_ms == 0xFFFFu ? 0u : pause_ms)) {
                    fprintf(stderr, "Failed to store TZX block\n");
                    free(buffer);
                    fclose(tf);
                    return 0;
                }
                free(buffer);
            } else {
                uint8_t empty = 0;
                if (!tape_image_add_block(image, &empty, 0u, pause_ms == 0xFFFFu ? 0u : pause_ms)) {
                    fprintf(stderr, "Failed to store empty TZX block\n");
                    fclose(tf);
                    return 0;
                }
            }
        } else if (block_id == 0x11) {
            uint8_t header_bytes[16];
            if (fread(header_bytes, sizeof(header_bytes), 1, tf) != 1) {
                fprintf(stderr, "Truncated TZX turbo data block\n");
                fclose(tf);
                return 0;
            }
            uint16_t pilot_pulse = (uint16_t)header_bytes[0] | ((uint16_t)header_bytes[1] << 8);
            uint16_t sync1 = (uint16_t)header_bytes[2] | ((uint16_t)header_bytes[3] << 8);
            uint16_t sync2 = (uint16_t)header_bytes[4] | ((uint16_t)header_bytes[5] << 8);
            uint16_t bit0_first = (uint16_t)header_bytes[6] | ((uint16_t)header_bytes[7] << 8);
            uint16_t bit0_second = (uint16_t)header_bytes[8] | ((uint16_t)header_bytes[9] << 8);
            uint16_t bit1_first = (uint16_t)header_bytes[10] | ((uint16_t)header_bytes[11] << 8);
            uint16_t bit1_second = (uint16_t)header_bytes[12] | ((uint16_t)header_bytes[13] << 8);
            uint16_t pilot_count = (uint16_t)header_bytes[14] | ((uint16_t)header_bytes[15] << 8);
            int used_bits = fgetc(tf);
            if (used_bits == EOF) {
                fprintf(stderr, "Truncated TZX turbo data block\n");
                fclose(tf);
                return 0;
            }
            uint8_t pause_bytes[2];
            if (fread(pause_bytes, sizeof(pause_bytes), 1, tf) != 1) {
                fprintf(stderr, "Truncated TZX turbo data block\n");
                fclose(tf);
                return 0;
            }
            uint8_t length_bytes[3];
            if (fread(length_bytes, sizeof(length_bytes), 1, tf) != 1) {
                fprintf(stderr, "Truncated TZX turbo data block\n");
                fclose(tf);
                return 0;
            }
            uint32_t pause_ms = (uint32_t)pause_bytes[0] | ((uint32_t)pause_bytes[1] << 8);
            uint32_t block_length = (uint32_t)length_bytes[0] |
                                    ((uint32_t)length_bytes[1] << 8) |
                                    ((uint32_t)length_bytes[2] << 16);
            uint8_t* buffer = NULL;
            if (block_length > 0u) {
                buffer = (uint8_t*)malloc(block_length);
                if (!buffer) {
                    fprintf(stderr, "Out of memory while reading TZX turbo block\n");
                    fclose(tf);
                    return 0;
                }
                if (fread(buffer, block_length, 1, tf) != 1) {
                    fprintf(stderr, "Failed to read TZX turbo block payload\n");
                    free(buffer);
                    fclose(tf);
                    return 0;
                }
            }
            if (!tape_image_add_turbo_block(image,
                                            buffer,
                                            block_length,
                                            pause_ms == 0xFFFFu ? 0u : pause_ms,
                                            pilot_pulse,
                                            pilot_count,
                                            sync1,
                                            sync2,
                                            bit0_first,
                                            bit0_second,
                                            bit1_first,
                                            bit1_second,
                                            (uint8_t)(used_bits & 0xFF))) {
                fprintf(stderr, "Failed to store TZX turbo block\n");
                free(buffer);
                fclose(tf);
                return 0;
            }
            free(buffer);
        } else if (block_id == 0x12) {
            uint8_t tone_bytes[4];
            if (fread(tone_bytes, sizeof(tone_bytes), 1, tf) != 1) {
                fprintf(stderr, "Truncated TZX pure tone block\n");
                fclose(tf);
                return 0;
            }
            uint16_t pulse_length = (uint16_t)tone_bytes[0] | ((uint16_t)tone_bytes[1] << 8);
            uint16_t pulse_count = (uint16_t)tone_bytes[2] | ((uint16_t)tone_bytes[3] << 8);
            if (!tape_image_add_pure_tone_block(image, pulse_length, pulse_count, 0u)) {
                fprintf(stderr, "Failed to store TZX pure tone block\n");
                fclose(tf);
                return 0;
            }
        } else if (block_id == 0x13) {
            int pulse_count = fgetc(tf);
            if (pulse_count == EOF) {
                fprintf(stderr, "Truncated TZX pulse sequence block\n");
                fclose(tf);
                return 0;
            }
            if (pulse_count == 0) {
                continue;
            }
            size_t count = (size_t)(pulse_count & 0xFF);
            uint16_t* durations = (uint16_t*)malloc(count * sizeof(uint16_t));
            if (!durations) {
                fprintf(stderr, "Out of memory while reading pulse sequence\n");
                fclose(tf);
                return 0;
            }
            for (size_t i = 0; i < count; ++i) {
                uint8_t duration_bytes[2];
                if (fread(duration_bytes, sizeof(duration_bytes), 1, tf) != 1) {
                    fprintf(stderr, "Truncated TZX pulse sequence block\n");
                    free(durations);
                    fclose(tf);
                    return 0;
                }
                durations[i] = (uint16_t)duration_bytes[0] | ((uint16_t)duration_bytes[1] << 8);
            }
            if (!tape_image_add_pulse_sequence_block(image, durations, count, 0u)) {
                fprintf(stderr, "Failed to store TZX pulse sequence block\n");
                free(durations);
                fclose(tf);
                return 0;
            }
            free(durations);
        } else if (block_id == 0x14) {
            uint8_t header_bytes[8];
            if (fread(header_bytes, sizeof(header_bytes), 1, tf) != 1) {
                fprintf(stderr, "Truncated TZX pure data block\n");
                fclose(tf);
                return 0;
            }
            uint16_t bit0_first = (uint16_t)header_bytes[0] | ((uint16_t)header_bytes[1] << 8);
            uint16_t bit0_second = (uint16_t)header_bytes[2] | ((uint16_t)header_bytes[3] << 8);
            uint16_t bit1_first = (uint16_t)header_bytes[4] | ((uint16_t)header_bytes[5] << 8);
            uint16_t bit1_second = (uint16_t)header_bytes[6] | ((uint16_t)header_bytes[7] << 8);
            int used_bits = fgetc(tf);
            if (used_bits == EOF) {
                fprintf(stderr, "Truncated TZX pure data block\n");
                fclose(tf);
                return 0;
            }
            uint8_t pause_bytes[2];
            if (fread(pause_bytes, sizeof(pause_bytes), 1, tf) != 1) {
                fprintf(stderr, "Truncated TZX pure data block\n");
                fclose(tf);
                return 0;
            }
            uint8_t length_bytes[3];
            if (fread(length_bytes, sizeof(length_bytes), 1, tf) != 1) {
                fprintf(stderr, "Truncated TZX pure data block\n");
                fclose(tf);
                return 0;
            }
            uint32_t pause_ms = (uint32_t)pause_bytes[0] | ((uint32_t)pause_bytes[1] << 8);
            uint32_t block_length = (uint32_t)length_bytes[0] |
                                    ((uint32_t)length_bytes[1] << 8) |
                                    ((uint32_t)length_bytes[2] << 16);
            uint8_t* buffer = NULL;
            if (block_length > 0u) {
                buffer = (uint8_t*)malloc(block_length);
                if (!buffer) {
                    fprintf(stderr, "Out of memory while reading TZX pure data block\n");
                    fclose(tf);
                    return 0;
                }
                if (fread(buffer, block_length, 1, tf) != 1) {
                    fprintf(stderr, "Failed to read TZX pure data block payload\n");
                    free(buffer);
                    fclose(tf);
                    return 0;
                }
            }
            if (!tape_image_add_pure_data_block(image,
                                                buffer,
                                                block_length,
                                                pause_ms == 0xFFFFu ? 0u : pause_ms,
                                                bit0_first,
                                                bit0_second,
                                                bit1_first,
                                                bit1_second,
                                                (uint8_t)(used_bits & 0xFF))) {
                fprintf(stderr, "Failed to store TZX pure data block\n");
                free(buffer);
                fclose(tf);
                return 0;
            }
            free(buffer);
        } else if (block_id == 0x15) {
            uint8_t header_bytes[5];
            if (fread(header_bytes, sizeof(header_bytes), 1, tf) != 1) {
                fprintf(stderr, "Truncated TZX direct recording block\n");
                fclose(tf);
                return 0;
            }
            uint16_t sample_tstates = (uint16_t)header_bytes[0] | ((uint16_t)header_bytes[1] << 8);
            uint16_t pause_ms = (uint16_t)header_bytes[2] | ((uint16_t)header_bytes[3] << 8);
            uint8_t used_bits = header_bytes[4];
            uint8_t length_bytes[3];
            if (fread(length_bytes, sizeof(length_bytes), 1, tf) != 1) {
                fprintf(stderr, "Truncated TZX direct recording block\n");
                fclose(tf);
                return 0;
            }
            uint32_t sample_bytes = (uint32_t)length_bytes[0] |
                                     ((uint32_t)length_bytes[1] << 8) |
                                     ((uint32_t)length_bytes[2] << 16);
            uint8_t* buffer = NULL;
            if (sample_bytes > 0u) {
                buffer = (uint8_t*)malloc(sample_bytes);
                if (!buffer) {
                    fprintf(stderr, "Out of memory while reading TZX direct recording block\n");
                    fclose(tf);
                    return 0;
                }
                if (fread(buffer, sample_bytes, 1, tf) != 1) {
                    fprintf(stderr, "Failed to read TZX direct recording block payload\n");
                    free(buffer);
                    fclose(tf);
                    return 0;
                }
            }
            if (!tape_image_add_direct_recording_block(image,
                                                       sample_tstates,
                                                       buffer,
                                                       sample_bytes,
                                                       used_bits,
                                                       pause_ms == 0xFFFFu ? 0u : (uint32_t)pause_ms)) {
                fprintf(stderr, "Failed to store TZX direct recording block\n");
                free(buffer);
                fclose(tf);
                return 0;
            }
            free(buffer);
        } else {
            fprintf(stderr, "Unsupported TZX block type 0x%02X in '%s'\n", block_id, path);
            fclose(tf);
            return 0;
        }
    }

    fclose(tf);
    return 1;
}

static int z80_decompress_stream(FILE* sf, uint8_t* dest, size_t dest_size) {
    if (!sf || !dest) {
        return 0;
    }

    size_t dest_index = 0;
    while (dest_index < dest_size) {
        int byte_value = fgetc(sf);
        if (byte_value == EOF) {
            return 0;
        }
        if (byte_value != 0xED) {
            dest[dest_index++] = (uint8_t)byte_value;
            continue;
        }

        int next_value = fgetc(sf);
        if (next_value == EOF) {
            return 0;
        }
        if (next_value != 0xED) {
            dest[dest_index++] = 0xED;
            if (ungetc(next_value, sf) == EOF) {
                return 0;
            }
            continue;
        }

        int count_value = fgetc(sf);
        int data_value = fgetc(sf);
        if (count_value == EOF || data_value == EOF) {
            return 0;
        }
        size_t repeat = (count_value == 0) ? 256u : (size_t)count_value;
        for (size_t i = 0; i < repeat; ++i) {
            if (dest_index >= dest_size) {
                return 0;
            }
            dest[dest_index++] = (uint8_t)data_value;
        }
    }

    return 1;
}

static int z80_decompress_block_buffer(const uint8_t* src,
                                       size_t src_len,
                                       uint8_t* dest,
                                       size_t dest_len) {
    if (!src || !dest) {
        return 0;
    }

    size_t src_index = 0;
    size_t dest_index = 0;
    while (dest_index < dest_len && src_index < src_len) {
        uint8_t value = src[src_index++];
        if (value != 0xED || src_index >= src_len) {
            dest[dest_index++] = value;
            continue;
        }

        uint8_t next = src[src_index];
        if (next != 0xED) {
            dest[dest_index++] = value;
            continue;
        }

        src_index++;
        if (src_index + 2 > src_len) {
            return 0;
        }
        uint8_t count_byte = src[src_index++];
        uint8_t data_byte = src[src_index++];
        size_t repeat = (count_byte == 0) ? 256u : (size_t)count_byte;
        for (size_t i = 0; i < repeat; ++i) {
            if (dest_index >= dest_len) {
                return 0;
            }
            dest[dest_index++] = data_byte;
        }
    }

    return dest_index == dest_len;
}

static SnapshotFormat snapshot_format_from_extension(const char* path) {
    if (!path) {
        return SNAPSHOT_FORMAT_NONE;
    }

    if (string_ends_with_case_insensitive(path, ".sna")) {
        return SNAPSHOT_FORMAT_SNA;
    }
    if (string_ends_with_case_insensitive(path, ".z80")) {
        return SNAPSHOT_FORMAT_Z80;
    }

    return SNAPSHOT_FORMAT_NONE;
}

static int snapshot_load_sna(const char* path, Z80* cpu) {
    if (!path || !cpu) {
        return 0;
    }

    FILE* sf = fopen(path, "rb");
    if (!sf) {
        fprintf(stderr, "Failed to open SNA snapshot '%s': %s\n", path, strerror(errno));
        return 0;
    }

    uint8_t header[27];
    if (fread(header, sizeof(header), 1, sf) != 1) {
        fprintf(stderr, "Failed to read SNA header from '%s'\n", path);
        fclose(sf);
        return 0;
    }

    cpu->reg_I = header[0];
    uint16_t hl_alt = (uint16_t)header[1] | ((uint16_t)header[2] << 8);
    uint16_t de_alt = (uint16_t)header[3] | ((uint16_t)header[4] << 8);
    uint16_t bc_alt = (uint16_t)header[5] | ((uint16_t)header[6] << 8);
    uint16_t af_alt = (uint16_t)header[7] | ((uint16_t)header[8] << 8);
    uint16_t hl = (uint16_t)header[9] | ((uint16_t)header[10] << 8);
    uint16_t de = (uint16_t)header[11] | ((uint16_t)header[12] << 8);
    uint16_t bc = (uint16_t)header[13] | ((uint16_t)header[14] << 8);
    uint16_t iy = (uint16_t)header[15] | ((uint16_t)header[16] << 8);
    uint16_t ix = (uint16_t)header[17] | ((uint16_t)header[18] << 8);
    uint8_t iff2 = header[19];
    uint8_t r = header[20];
    uint16_t af = (uint16_t)header[21] | ((uint16_t)header[22] << 8);
    uint16_t sp = (uint16_t)header[23] | ((uint16_t)header[24] << 8);
    uint8_t interrupt_mode = header[25];
    uint8_t border = header[26] & 0x07u;

    cpu->alt_reg_L = (uint8_t)(hl_alt & 0xFFu);
    cpu->alt_reg_H = (uint8_t)(hl_alt >> 8);
    cpu->alt_reg_E = (uint8_t)(de_alt & 0xFFu);
    cpu->alt_reg_D = (uint8_t)(de_alt >> 8);
    cpu->alt_reg_C = (uint8_t)(bc_alt & 0xFFu);
    cpu->alt_reg_B = (uint8_t)(bc_alt >> 8);
    cpu->alt_reg_F = (uint8_t)(af_alt & 0xFFu);
    cpu->alt_reg_A = (uint8_t)(af_alt >> 8);

    cpu->reg_L = (uint8_t)(hl & 0xFFu);
    cpu->reg_H = (uint8_t)(hl >> 8);
    cpu->reg_E = (uint8_t)(de & 0xFFu);
    cpu->reg_D = (uint8_t)(de >> 8);
    cpu->reg_C = (uint8_t)(bc & 0xFFu);
    cpu->reg_B = (uint8_t)(bc >> 8);
    cpu->reg_IY = iy;
    cpu->reg_IX = ix;
    cpu->iff2 = (iff2 & 0x01u) ? 1 : 0;
    cpu->iff1 = cpu->iff2;
    cpu->reg_R = r;
    cpu->reg_F = (uint8_t)(af & 0xFFu);
    cpu->reg_A = (uint8_t)(af >> 8);
    cpu->reg_SP = sp;
    cpu->interruptMode = (int)(interrupt_mode & 0x03u);
    cpu->ei_delay = 0;
    cpu->halted = 0;

    border_color_idx = border;

    uint8_t* ram_buffer = (uint8_t*)malloc(0xC000u);
    if (!ram_buffer) {
        fprintf(stderr, "Out of memory loading SNA snapshot\n");
        fclose(sf);
        return 0;
    }

    if (fread(ram_buffer, 0xC000u, 1, sf) != 1) {
        fprintf(stderr, "Failed to read SNA RAM image from '%s'\n", path);
        free(ram_buffer);
        fclose(sf);
        return 0;
    }

    uint8_t pc_bytes[2];
    if (fread(pc_bytes, sizeof(pc_bytes), 1, sf) != 1) {
        fprintf(stderr, "Failed to read SNA program counter from '%s'\n", path);
        free(ram_buffer);
        fclose(sf);
        return 0;
    }
    uint16_t pc = (uint16_t)pc_bytes[0] | ((uint16_t)pc_bytes[1] << 8);
    cpu->reg_PC = pc;

    long extension_pos = ftell(sf);
    size_t extension_bytes = 0u;
    if (extension_pos >= 0 && fseek(sf, 0, SEEK_END) == 0) {
        long end_pos = ftell(sf);
        if (end_pos >= extension_pos) {
            extension_bytes = (size_t)(end_pos - extension_pos);
        }
        if (fseek(sf, extension_pos, SEEK_SET) != 0) {
            fprintf(stderr, "Failed to seek within SNA snapshot '%s'\n", path);
            free(ram_buffer);
            fclose(sf);
            return 0;
        }
    }

    int loaded_ok = 0;
    if (extension_bytes == 0u) {
        spectrum_configure_model(SPECTRUM_MODEL_48K);
        memcpy(ram_pages[5], ram_buffer + 0x0000u, 0x4000u);
        memcpy(ram_pages[2], ram_buffer + 0x4000u, 0x4000u);
        memcpy(ram_pages[7], ram_buffer + 0x8000u, 0x4000u);
        spectrum_apply_memory_configuration();
        spectrum_refresh_visible_ram();
        loaded_ok = 1;
    } else {
        size_t required_bytes = 2u + (size_t)8u * 0x4000u;
        if (extension_bytes < required_bytes) {
            fprintf(stderr, "SNA snapshot '%s' truncated 128K payload (%zu bytes)\n",
                    path,
                    extension_bytes);
        } else {
            uint8_t port_7ffd = 0u;
            uint8_t trdos_flag = 0u;
            if (fread(&port_7ffd, 1, 1, sf) != 1 || fread(&trdos_flag, 1, 1, sf) != 1) {
                fprintf(stderr, "Failed to read SNA 128K gate-array state from '%s'\n", path);
            } else {
                (void)trdos_flag;
                loaded_ok = 1;
                for (int bank = 0; bank < 8; ++bank) {
                    if (fread(ram_pages[bank], 0x4000u, 1, sf) != 1) {
                        fprintf(stderr, "Failed to read SNA 128K RAM bank %d from '%s'\n",
                                bank,
                                path);
                        loaded_ok = 0;
                        break;
                    }
                }
            }

            if (loaded_ok) {
                size_t consumed = required_bytes;
                uint8_t port_1ffd = 0u;
                int have_1ffd = 0;
                if (extension_bytes > consumed) {
                    size_t remaining = extension_bytes - consumed;
                    if (remaining >= 1u) {
                        if (fread(&port_1ffd, 1, 1, sf) != 1) {
                            fprintf(stderr, "Failed to read SNA 128K 1FFD state from '%s'\n", path);
                            loaded_ok = 0;
                        } else {
                            have_1ffd = 1;
                            consumed += 1u;
                        }
                    }
                    if (loaded_ok && extension_bytes > consumed) {
                        fprintf(stderr,
                                "SNA snapshot '%s' has %zu unexpected padding bytes\n",
                                path,
                                extension_bytes - consumed);
                        loaded_ok = 0;
                    }
                }

                if (loaded_ok) {
                    SpectrumModel preferred = spectrum_model;
                    SpectrumModel target_model;
                    if (have_1ffd) {
                        if (preferred == SPECTRUM_MODEL_PLUS2A || preferred == SPECTRUM_MODEL_PLUS3) {
                            target_model = preferred;
                        } else {
                            target_model = SPECTRUM_MODEL_PLUS2A;
                        }
                    } else {
                        if (preferred == SPECTRUM_MODEL_128K ||
                            preferred == SPECTRUM_MODEL_PLUS2A ||
                            preferred == SPECTRUM_MODEL_PLUS3) {
                            target_model = preferred;
                        } else {
                            target_model = SPECTRUM_MODEL_128K;
                        }
                    }

                    spectrum_configure_model(target_model);

                    gate_array_7ffd_state = (uint8_t)(port_7ffd & 0x3Fu);
                    paging_disabled = (gate_array_7ffd_state & 0x20u) ? 1 : 0;
                    current_paged_bank = (uint8_t)(gate_array_7ffd_state & 0x07u);
                    if (target_model == SPECTRUM_MODEL_PLUS2A || target_model == SPECTRUM_MODEL_PLUS3) {
                        gate_array_1ffd_state = have_1ffd ? (uint8_t)(port_1ffd & 0x07u) : 0u;
                    } else {
                        gate_array_1ffd_state = 0u;
                    }

                    spectrum_apply_memory_configuration();
                    spectrum_refresh_visible_ram();
                }
            }
        }
    }

    free(ram_buffer);

    if (!loaded_ok) {
        fclose(sf);
        return 0;
    }

    fclose(sf);

    fprintf(stderr,
            "SNA snapshot '%s': model=%s PC=%04X SP=%04X IM=%d border=%u bank=%u rom=%u %s\n",
            path,
            spectrum_model_to_string(spectrum_model),
            (unsigned)cpu->reg_PC,
            (unsigned)cpu->reg_SP,
            cpu->interruptMode,
            (unsigned)border,
            (unsigned)current_paged_bank,
            (unsigned)current_rom_page,
            paging_disabled ? "paging=locked" : "paging=unlocked");
    return 1;
}

static SpectrumModel z80_hardware_mode_to_model(uint8_t hardware_mode) {
    switch (hardware_mode) {
        case 3:
        case 4:
        case 5:
            return SPECTRUM_MODEL_128K;
        case 6:
            return SPECTRUM_MODEL_PLUS2A;
        case 7:
            return SPECTRUM_MODEL_PLUS3;
        case 0:
        case 1:
        case 2:
        default:
            return SPECTRUM_MODEL_48K;
    }
}

static int z80_map_page_id_to_offset(int page_id, size_t* offset, uint8_t* mask_bit) {
    if (!offset || !mask_bit) {
        return 0;
    }

    switch (page_id) {
        case 8:
            *offset = 0x0000u;
            *mask_bit = 0x01u;
            return 1;
        case 4:
            *offset = 0x4000u;
            *mask_bit = 0x02u;
            return 1;
        case 5:
            *offset = 0x8000u;
            *mask_bit = 0x04u;
            return 1;
        default:
            break;
    }

    return 0;
}

static int z80_map_page_id_to_bank(int page_id, uint8_t* bank, uint16_t* mask_bit) {
    if (!bank || !mask_bit) {
        return 0;
    }

    if (page_id >= 0 && page_id <= 7) {
        *bank = (uint8_t)page_id;
        *mask_bit = (uint16_t)(1u << page_id);
        return 1;
    }

    switch (page_id) {
        case 8:
            *bank = 0u;
            *mask_bit = 1u;
            return 1;
        case 9:
            *bank = 1u;
            *mask_bit = 2u;
            return 1;
        case 10:
            *bank = 2u;
            *mask_bit = 4u;
            return 1;
        default:
            break;
    }

    return 0;
}

static int snapshot_load_z80(const char* path, Z80* cpu) {
    if (!path || !cpu) {
        return 0;
    }

    FILE* sf = fopen(path, "rb");
    if (!sf) {
        fprintf(stderr, "Failed to open Z80 snapshot '%s': %s\n", path, strerror(errno));
        return 0;
    }

    uint8_t header[30];
    if (fread(header, sizeof(header), 1, sf) != 1) {
        fprintf(stderr, "Failed to read Z80 header from '%s'\n", path);
        fclose(sf);
        return 0;
    }

    uint16_t pc = (uint16_t)header[6] | ((uint16_t)header[7] << 8);
    int header_pc_zero = (pc == 0u);
    uint16_t sp = (uint16_t)header[8] | ((uint16_t)header[9] << 8);
    uint8_t flags = header[12];
    uint8_t compressed_flag = (flags & 0x20u) ? 1u : 0u;
    uint8_t r_bit_7 = (flags & 0x01u) ? 0x80u : 0u;
    border_color_idx = (flags >> 1) & 0x07u;

    uint8_t hardware_mode = 0u;

    spectrum_configure_model(SPECTRUM_MODEL_48K);

    cpu->reg_A = header[0];
    cpu->reg_F = header[1];
    cpu->reg_C = header[2];
    cpu->reg_B = header[3];
    cpu->reg_L = header[4];
    cpu->reg_H = header[5];
    cpu->reg_SP = sp;
    cpu->reg_I = header[10];
    cpu->reg_R = (uint8_t)((header[11] & 0x7Fu) | r_bit_7);

    cpu->reg_E = header[13];
    cpu->reg_D = header[14];
    cpu->alt_reg_C = header[15];
    cpu->alt_reg_B = header[16];
    cpu->alt_reg_E = header[17];
    cpu->alt_reg_D = header[18];
    cpu->alt_reg_L = header[19];
    cpu->alt_reg_H = header[20];
    cpu->alt_reg_A = header[21];
    cpu->alt_reg_F = header[22];
    cpu->reg_IY = (uint16_t)header[23] | ((uint16_t)header[24] << 8);
    cpu->reg_IX = (uint16_t)header[25] | ((uint16_t)header[26] << 8);
    cpu->iff1 = (header[27] & 0x01u) ? 1 : 0;
    cpu->iff2 = (header[28] & 0x01u) ? 1 : 0;

    uint8_t interrupt_mode = header[29];
    cpu->interruptMode = (int)(interrupt_mode & 0x03u);

    cpu->ei_delay = 0;
    cpu->halted = 0;

    uint8_t port_7ffd = 0u;
    uint8_t port_1ffd = 0u;
    uint8_t* ram_buffer = NULL;

    if (pc != 0) {
        ram_buffer = (uint8_t*)malloc(0xC000u);
        if (!ram_buffer) {
            fprintf(stderr, "Out of memory loading Z80 snapshot\n");
            fclose(sf);
            return 0;
        }

        if (compressed_flag) {
            if (!z80_decompress_stream(sf, ram_buffer, 0xC000u)) {
                fprintf(stderr, "Failed to decompress V1 Z80 snapshot '%s'\n", path);
                free(ram_buffer);
                fclose(sf);
                return 0;
            }
        } else {
            if (fread(ram_buffer, 0xC000u, 1, sf) != 1) {
                fprintf(stderr, "Failed to read V1 Z80 RAM image from '%s'\n", path);
                free(ram_buffer);
                fclose(sf);
                return 0;
            }
        }

        memcpy(ram_pages[5], ram_buffer + 0x0000u, 0x4000u);
        memcpy(ram_pages[2], ram_buffer + 0x4000u, 0x4000u);
        memcpy(ram_pages[7], ram_buffer + 0x8000u, 0x4000u);
        spectrum_refresh_visible_ram();
        free(ram_buffer);
        spectrum_apply_memory_configuration();
    } else {
        uint8_t ext_len_bytes[2];
        if (fread(ext_len_bytes, sizeof(ext_len_bytes), 1, sf) != 1) {
            fprintf(stderr, "Failed to read Z80 extended header length from '%s'\n", path);
            fclose(sf);
            return 0;
        }
        uint16_t ext_len = (uint16_t)ext_len_bytes[0] | ((uint16_t)ext_len_bytes[1] << 8);
        uint8_t* ext_header = NULL;
        if (ext_len > 0u) {
            ext_header = (uint8_t*)malloc(ext_len);
            if (!ext_header) {
                fprintf(stderr, "Out of memory reading Z80 extended header\n");
                fclose(sf);
                return 0;
            }
            if (fread(ext_header, ext_len, 1, sf) != 1) {
                fprintf(stderr, "Failed to read Z80 extended header from '%s'\n", path);
                free(ext_header);
                fclose(sf);
                return 0;
            }
        }

        if (ext_header && ext_len >= 2u) {
            pc = (uint16_t)ext_header[0] | ((uint16_t)ext_header[1] << 8);
        }
        hardware_mode = (ext_header && ext_len >= 3u) ? ext_header[2] : 0u;
        port_7ffd = (ext_header && ext_len >= 4u) ? ext_header[3] : 0u;
        port_1ffd = (ext_header && ext_len >= 5u) ? ext_header[4] : 0u;

        SpectrumModel target_model = z80_hardware_mode_to_model(hardware_mode);
        spectrum_configure_model(target_model);

        int is_128k_family = (target_model != SPECTRUM_MODEL_48K);
        uint16_t loaded_mask = 0u;
        uint16_t expected_mask = is_128k_family ? 0xFFu : 0x07u;

        if (!is_128k_family) {
            ram_buffer = (uint8_t*)malloc(0xC000u);
            if (!ram_buffer) {
                fprintf(stderr, "Out of memory loading Z80 snapshot\n");
                free(ext_header);
                fclose(sf);
                return 0;
            }
        }

        while (loaded_mask != expected_mask) {
            uint8_t len_bytes[2];
            if (fread(len_bytes, sizeof(len_bytes), 1, sf) != 1) {
                if (feof(sf) && loaded_mask != 0u) {
                    break;
                }
                fprintf(stderr, "Truncated Z80 memory block in '%s'\n", path);
                free(ram_buffer);
                free(ext_header);
                fclose(sf);
                return 0;
            }
            uint16_t block_len = (uint16_t)len_bytes[0] | ((uint16_t)len_bytes[1] << 8);
            int page_id = fgetc(sf);
            if (page_id == EOF) {
                fprintf(stderr, "Truncated Z80 memory block header in '%s'\n", path);
                free(ram_buffer);
                free(ext_header);
                fclose(sf);
                return 0;
            }

            uint8_t* segment = NULL;
            uint16_t mask_bit = 0u;
            if (is_128k_family) {
                uint8_t bank = 0u;
                if (!z80_map_page_id_to_bank(page_id, &bank, &mask_bit)) {
                    fprintf(stderr, "Z80 snapshot '%s' contains unsupported memory page id %d\n", path, page_id);
                    if (fseek(sf, block_len, SEEK_CUR) != 0) {
                        fprintf(stderr, "Failed to seek past unsupported block in '%s'\n", path);
                        free(ram_buffer);
                        free(ext_header);
                        fclose(sf);
                        return 0;
                    }
                    continue;
                }
                if ((loaded_mask & mask_bit) != 0u) {
                    fprintf(stderr, "Z80 snapshot '%s' repeats memory page id %d\n", path, page_id);
                    if (fseek(sf, block_len, SEEK_CUR) != 0) {
                        fprintf(stderr, "Failed to seek past repeated block in '%s'\n", path);
                        free(ram_buffer);
                        free(ext_header);
                        fclose(sf);
                        return 0;
                    }
                    continue;
                }
                segment = ram_pages[bank];
            } else {
                size_t segment_offset = 0u;
                uint8_t offset_mask = 0u;
                if (!z80_map_page_id_to_offset(page_id, &segment_offset, &offset_mask)) {
                    fprintf(stderr, "Z80 snapshot '%s' contains unsupported memory page id %d\n", path, page_id);
                    if (fseek(sf, block_len, SEEK_CUR) != 0) {
                        fprintf(stderr, "Failed to seek past unsupported block in '%s'\n", path);
                        free(ram_buffer);
                        free(ext_header);
                        fclose(sf);
                        return 0;
                    }
                    continue;
                }
                mask_bit = offset_mask;
                if ((loaded_mask & mask_bit) != 0u) {
                    fprintf(stderr, "Z80 snapshot '%s' repeats memory page id %d\n", path, page_id);
                    if (fseek(sf, block_len, SEEK_CUR) != 0) {
                        fprintf(stderr, "Failed to seek past repeated block in '%s'\n", path);
                        free(ram_buffer);
                        free(ext_header);
                        fclose(sf);
                        return 0;
                    }
                    continue;
                }
                segment = ram_buffer + segment_offset;
            }

            if (block_len == 0xFFFFu) {
                if (fread(segment, 0x4000u, 1, sf) != 1) {
                    fprintf(stderr, "Failed to read uncompressed block from '%s'\n", path);
                    free(ram_buffer);
                    free(ext_header);
                    fclose(sf);
                    return 0;
                }
            } else {
                uint8_t* block_data = (uint8_t*)malloc(block_len);
                if (!block_data) {
                    fprintf(stderr, "Out of memory reading Z80 block\n");
                    free(ram_buffer);
                    free(ext_header);
                    fclose(sf);
                    return 0;
                }
                if (fread(block_data, block_len, 1, sf) != 1) {
                    fprintf(stderr, "Failed to read Z80 block payload from '%s'\n", path);
                    free(block_data);
                    free(ram_buffer);
                    free(ext_header);
                    fclose(sf);
                    return 0;
                }
                if (!z80_decompress_block_buffer(block_data, block_len, segment, 0x4000u)) {
                    fprintf(stderr, "Failed to decompress Z80 block from '%s'\n", path);
                    free(block_data);
                    free(ram_buffer);
                    free(ext_header);
                    fclose(sf);
                    return 0;
                }
                free(block_data);
            }

            loaded_mask |= mask_bit;
        }

        if (!is_128k_family) {
            memcpy(ram_pages[5], ram_buffer + 0x0000u, 0x4000u);
            memcpy(ram_pages[2], ram_buffer + 0x4000u, 0x4000u);
            memcpy(ram_pages[7], ram_buffer + 0x8000u, 0x4000u);
            spectrum_refresh_visible_ram();
            free(ram_buffer);
        }

        free(ext_header);

        gate_array_7ffd_state = (uint8_t)(port_7ffd & 0x3Fu);
        paging_disabled = (gate_array_7ffd_state & 0x20u) ? 1 : 0;
        current_paged_bank = (uint8_t)(gate_array_7ffd_state & 0x07u);
        if (target_model == SPECTRUM_MODEL_PLUS2A || target_model == SPECTRUM_MODEL_PLUS3) {
            gate_array_1ffd_state = (uint8_t)(port_1ffd & 0x07u);
        } else {
            gate_array_1ffd_state = 0u;
        }

        spectrum_apply_memory_configuration();
        spectrum_refresh_visible_ram();
    }

    cpu->reg_PC = pc;

    fclose(sf);

    const char* version_label = header_pc_zero ? "V2/3" : "V1";
    fprintf(stderr,
            "Z80 snapshot '%s': version %s PC=%04X SP=%04X IM=%d border=%u\n",
            path,
            version_label,
            (unsigned)cpu->reg_PC,
            (unsigned)cpu->reg_SP,
            cpu->interruptMode,
            (unsigned)border_color_idx);
    return 1;
}

static int snapshot_load(const char* path, SnapshotFormat format, Z80* cpu) {
    switch (format) {
        case SNAPSHOT_FORMAT_SNA:
            return snapshot_load_sna(path, cpu);
        case SNAPSHOT_FORMAT_Z80:
            return snapshot_load_z80(path, cpu);
        default:
            break;
    }
    return 0;
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

static void tape_set_input_path(const char* path) {
    if (path && *path) {
        size_t length = strlen(path);
        if (length >= sizeof(tape_input_path_storage)) {
            length = sizeof(tape_input_path_storage) - 1u;
        }
        memcpy(tape_input_path_storage, path, length);
        tape_input_path_storage[length] = '\0';
        tape_input_path = tape_input_path_storage;
    } else {
        tape_input_path_storage[0] = '\0';
        tape_input_path = NULL;
    }
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

static int speaker_component_amplitude(int level) {
    return level ? 1 : -1;
}

static int speaker_calculate_output_level(void) {
    int combined = speaker_component_amplitude(beeper_state);
    combined += speaker_component_amplitude(speaker_tape_playback_level);
    combined += speaker_component_amplitude(speaker_tape_record_level);
    return combined;
}

static void speaker_update_output(uint64_t t_state, int emit_event) {
    int new_level = speaker_calculate_output_level();
    if (new_level != speaker_output_level) {
        speaker_output_level = new_level;
        if (emit_event) {
            beeper_push_event(t_state, new_level);
        }
    }
}

static double ay_clamp_pan(double value) {
    if (value < -1.0) {
        return -1.0;
    }
    if (value > 1.0) {
        return 1.0;
    }
    return value;
}

static void ay_compute_pan_gains(double pan, double* left_gain, double* right_gain) {
    if (!left_gain || !right_gain) {
        return;
    }
    double clamped = ay_clamp_pan(pan);
    double angle = (clamped + 1.0) * (M_PI * 0.25);
    *left_gain = cos(angle);
    *right_gain = sin(angle);
}

static double ay_highpass(double input, double* last_input, double* last_output) {
    if (!last_input || !last_output) {
        return 0.0;
    }
    double filtered = input - *last_input + BEEPER_HP_ALPHA * (*last_output);
    *last_input = input;
    *last_output = filtered;
    return filtered;
}

static void ay_refresh_tone_period(int channel) {
    if (channel < 0 || channel > 2) {
        return;
    }
    uint8_t low = ay_registers[channel * 2];
    uint8_t high = (uint8_t)(ay_registers[channel * 2 + 1] & 0x0Fu);
    uint16_t period = (uint16_t)low | ((uint16_t)high << 8);
    if (period == 0u) {
        period = 1u;
    }
    double cycles = (double)period * 16.0;
    ay_state.tone_period[channel] = cycles;
    if (ay_state.tone_counter[channel] <= 0.0 || ay_state.tone_counter[channel] > cycles) {
        ay_state.tone_counter[channel] = cycles;
    }
}

static void ay_refresh_noise_period(void) {
    uint8_t value = (uint8_t)(ay_registers[6] & 0x1Fu);
    if (value == 0u) {
        value = 1u;
    }
    double cycles = (double)value * 16.0;
    ay_state.noise_period = cycles;
    if (ay_state.noise_counter <= 0.0 || ay_state.noise_counter > cycles) {
        ay_state.noise_counter = cycles;
    }
}

static void ay_refresh_envelope_period(void) {
    uint16_t period = (uint16_t)ay_registers[11] | ((uint16_t)ay_registers[12] << 8);
    if (period == 0u) {
        period = 1u;
    }
    double cycles = (double)period * 256.0;
    ay_state.envelope_period = cycles;
    if (ay_state.envelope_counter <= 0.0 || ay_state.envelope_counter > cycles) {
        ay_state.envelope_counter = cycles;
    }
}

static void ay_restart_envelope(void) {
    uint8_t shape = (uint8_t)(ay_registers[13] & 0x0Fu);
    ay_state.envelope_continue = (shape >> 3) & 0x01;
    ay_state.envelope_direction = ((shape >> 2) & 0x01) ? 1 : -1;
    ay_state.envelope_alternate = (shape >> 1) & 0x01;
    ay_state.envelope_hold = shape & 0x01;
    if (!ay_state.envelope_continue) {
        ay_state.envelope_hold = 1;
        ay_state.envelope_alternate = 0;
    }
    ay_state.envelope_volume = (uint8_t)((ay_state.envelope_direction > 0) ? 0u : 15u);
    ay_state.envelope_active = 1;
    if (ay_state.envelope_counter <= 0.0) {
        ay_state.envelope_counter = ay_state.envelope_period > 0.0 ? ay_state.envelope_period : 1.0;
    }
}

static void ay_handle_envelope_limit(void) {
    if (!ay_state.envelope_continue) {
        ay_state.envelope_active = 0;
        ay_state.envelope_volume = (uint8_t)((ay_state.envelope_direction > 0) ? 15u : 0u);
        return;
    }

    if (ay_state.envelope_hold) {
        ay_state.envelope_active = 0;
        if (ay_state.envelope_alternate) {
            ay_state.envelope_direction = -ay_state.envelope_direction;
        }
        ay_state.envelope_volume = (uint8_t)((ay_state.envelope_direction > 0) ? 15u : 0u);
        return;
    }

    if (ay_state.envelope_alternate) {
        ay_state.envelope_direction = -ay_state.envelope_direction;
    }
    ay_state.envelope_volume = (uint8_t)((ay_state.envelope_direction > 0) ? 0u : 15u);
}

static void ay_step_envelope(void) {
    if (!ay_state.envelope_active) {
        return;
    }
    if (ay_state.envelope_direction > 0) {
        if (ay_state.envelope_volume < 15u) {
            ++ay_state.envelope_volume;
            return;
        }
    } else {
        if (ay_state.envelope_volume > 0u) {
            --ay_state.envelope_volume;
            return;
        }
    }
    ay_handle_envelope_limit();
}

static void ay_step_generators(double elapsed_cycles) {
    for (int ch = 0; ch < 3; ++ch) {
        double period = ay_state.tone_period[ch];
        if (period <= 0.0) {
            ay_state.tone_output[ch] = 1;
            ay_state.tone_counter[ch] = 1.0;
            continue;
        }
        double counter = ay_state.tone_counter[ch] - elapsed_cycles;
        while (counter <= 0.0) {
            counter += period;
            ay_state.tone_output[ch] ^= 1;
        }
        ay_state.tone_counter[ch] = counter;
    }

    double noise_period = ay_state.noise_period > 0.0 ? ay_state.noise_period : 1.0;
    double noise_counter = ay_state.noise_counter - elapsed_cycles;
    while (noise_counter <= 0.0) {
        noise_counter += noise_period;
        uint32_t lfsr = ay_state.noise_lfsr & 0x1FFFFu;
        uint32_t feedback = (uint32_t)(((lfsr ^ (lfsr >> 3)) & 0x01u));
        lfsr = (lfsr >> 1) | (feedback << 16);
        if (lfsr == 0u) {
            lfsr = 0x1FFFFu;
        }
        ay_state.noise_lfsr = lfsr & 0x1FFFFu;
        ay_state.noise_output = (int)(lfsr & 0x01u);
    }
    ay_state.noise_counter = noise_counter;

    double env_period = ay_state.envelope_period > 0.0 ? ay_state.envelope_period : 1.0;
    double env_counter = ay_state.envelope_counter - elapsed_cycles;
    while (env_counter <= 0.0) {
        env_counter += env_period;
        ay_step_envelope();
    }
    ay_state.envelope_counter = env_counter;
}

static double ay_channel_volume_value(int channel) {
    if (channel < 0 || channel > 2) {
        return 0.0;
    }
    uint8_t reg = ay_registers[8 + channel];
    if (reg & 0x10u) {
        return ay_volume_table[ay_state.envelope_volume & 0x0Fu];
    }
    return ay_volume_table[reg & 0x0Fu];
}

static double ay_channel_level(int channel) {
    uint8_t mixer = ay_registers[7];
    int tone_disabled = (mixer >> channel) & 0x01;
    int noise_disabled = (mixer >> (channel + 3)) & 0x01;
    int tone = tone_disabled ? 1 : ay_state.tone_output[channel];
    int noise = noise_disabled ? 1 : ay_state.noise_output;
    int active = tone & noise;
    if (!active) {
        return 0.0;
    }
    return ay_channel_volume_value(channel);
}

static void ay_reset_state_internal(void) {
    memset(&ay_state, 0, sizeof(ay_state));
    ay_state.noise_lfsr = 0x1FFFFu;
    ay_state.noise_output = 1;
    for (int ch = 0; ch < 3; ++ch) {
        ay_refresh_tone_period(ch);
        ay_state.tone_output[ch] = 0;
    }
    ay_refresh_noise_period();
    ay_refresh_envelope_period();
    ay_restart_envelope();
    ay_hp_last_input_left = 0.0;
    ay_hp_last_output_left = 0.0;
    ay_hp_last_input_right = 0.0;
    ay_hp_last_output_right = 0.0;
}

static void ay_reset_state(void) {
    int locked = 0;
    if (audio_available) {
        SDL_LockAudio();
        locked = 1;
    }
    ay_reset_state_internal();
    if (locked) {
        SDL_UnlockAudio();
    }
}

static void ay_set_sample_rate(int sample_rate) {
    if (sample_rate <= 0) {
        ay_cycles_per_sample = 0.0;
        return;
    }
    ay_cycles_per_sample = AY_CLOCK_HZ / (double)sample_rate;
}

static void ay_mix_sample(double elapsed_cycles, double* left_out, double* right_out) {
    if (!left_out || !right_out) {
        return;
    }
    if (ay_cycles_per_sample <= 0.0) {
        *left_out = 0.0;
        *right_out = 0.0;
        return;
    }

    double step_cycles = (elapsed_cycles > 0.0) ? elapsed_cycles : ay_cycles_per_sample;
    ay_step_generators(step_cycles);

    double left = 0.0;
    double right = 0.0;
    for (int ch = 0; ch < 3; ++ch) {
        double level = ay_channel_level(ch);
        if (level <= 0.0) {
            continue;
        }
        double pan_left = 0.0;
        double pan_right = 0.0;
        ay_compute_pan_gains(ay_channel_pan[ch], &pan_left, &pan_right);
        left += level * pan_left;
        right += level * pan_right;
    }

    double filtered_left = ay_highpass(left, &ay_hp_last_input_left, &ay_hp_last_output_left);
    double filtered_right = ay_highpass(right, &ay_hp_last_input_right, &ay_hp_last_output_right);

    *left_out = filtered_left * ay_global_gain;
    *right_out = filtered_right * ay_global_gain;
}

static void ay_write_register(uint8_t reg, uint8_t value) {
    uint8_t index = (uint8_t)(reg & 0x0Fu);
    ay_registers[index] = value;
    int locked = 0;
    if (audio_available) {
        SDL_LockAudio();
        locked = 1;
    }
    switch (index) {
        case 0:
        case 1:
            ay_refresh_tone_period(0);
            break;
        case 2:
        case 3:
            ay_refresh_tone_period(1);
            break;
        case 4:
        case 5:
            ay_refresh_tone_period(2);
            break;
        case 6:
            ay_refresh_noise_period();
            break;
        case 11:
        case 12:
            ay_refresh_envelope_period();
            break;
        case 13:
            ay_restart_envelope();
            break;
        default:
            break;
    }
    if (locked) {
        SDL_UnlockAudio();
    }
}

static int ay_parse_pan_spec(const char* spec) {
    if (!spec) {
        return 0;
    }
    double parsed[3];
    size_t index = 0;
    const char* cursor = spec;
    while (index < 3) {
        while (*cursor && isspace((unsigned char)*cursor)) {
            ++cursor;
        }
        if (*cursor == '\0') {
            return 0;
        }
        char* endptr = NULL;
        double value = strtod(cursor, &endptr);
        if (endptr == cursor) {
            return 0;
        }
        parsed[index++] = ay_clamp_pan(value);
        cursor = endptr;
        while (*cursor && isspace((unsigned char)*cursor)) {
            ++cursor;
        }
        if (index < 3) {
            if (*cursor != ',' && *cursor != ';' && *cursor != '/') {
                return 0;
            }
            ++cursor;
        } else if (*cursor != '\0') {
            return 0;
        }
    }
    for (size_t i = 0; i < 3; ++i) {
        ay_channel_pan[i] = parsed[i];
    }
    return 1;
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
    if (state == &tape_playback &&
        (state->format == TAPE_FORMAT_WAV || tape_recorder.output_format == TAPE_OUTPUT_WAV)) {
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
    speaker_tape_playback_level = tape_ear_state;
    speaker_update_output(total_t_states, 0);
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
    speaker_tape_playback_level = tape_ear_state;
    speaker_update_output(start_time, 1);
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
        speaker_tape_playback_level = tape_ear_state;
        speaker_update_output(start_time, 1);
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
    speaker_tape_playback_level = tape_ear_state;
    speaker_update_output(current_t_state, 1);
    return 1;
}

static void tape_rewind_playback(TapePlaybackState* state) {
    if (!state) {
        return;
    }
    tape_reset_playback(state);
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

static void tape_overlay_draw_rect(int x, int y, int width, int height, uint32_t fill_color, uint32_t border_color) {
    if (width <= 0 || height <= 0) {
        return;
    }

    for (int yy = 0; yy < height; ++yy) {
        int py = y + yy;
        if (py < 0 || py >= TOTAL_HEIGHT) {
            continue;
        }
        for (int xx = 0; xx < width; ++xx) {
            int px = x + xx;
            if (px < 0 || px >= TOTAL_WIDTH) {
                continue;
            }
            int is_border = (yy == 0 || yy == height - 1 || xx == 0 || xx == width - 1);
            pixels[py * TOTAL_WIDTH + px] = is_border ? border_color : fill_color;
        }
    }
}

static int tape_use_recorder_time(void) {
    int use_recorder_time = 0;

    if (tape_recorder.recording) {
        use_recorder_time = 1;
    } else if (!tape_playback.playing) {
        switch (tape_deck_status) {
            case TAPE_DECK_STATUS_RECORD:
                use_recorder_time = 1;
                break;
            default:
                break;
        }
    }

    if (!use_recorder_time && !tape_input_enabled && tape_recorder.enabled) {
        use_recorder_time = 1;
    }

    return use_recorder_time;
}

static uint64_t tape_current_elapsed_tstates(void) {
    int use_recorder_time = tape_use_recorder_time();
    int shared_wav = (tape_input_format == TAPE_FORMAT_WAV) ||
                     (tape_recorder.output_format == TAPE_OUTPUT_WAV);

    uint64_t elapsed = 0ull;

    if (shared_wav) {
        if (tape_recorder.recording) {
            elapsed = tape_recorder_elapsed_tstates(total_t_states);
        } else if (tape_playback.playing) {
            elapsed = tape_playback_elapsed_tstates(&tape_playback, total_t_states);
        } else if (tape_recorder.enabled && tape_recorder.output_format == TAPE_OUTPUT_WAV) {
            elapsed = tape_recorder.position_tstates;
        } else {
            elapsed = tape_wav_shared_position_tstates;
        }
    } else if (use_recorder_time) {
        elapsed = tape_recorder_elapsed_tstates(total_t_states);
    } else if (tape_input_enabled) {
        elapsed = tape_playback_elapsed_tstates(&tape_playback, total_t_states);
    }

    if (elapsed == 0ull) {
        if (tape_playback.position_tstates > 0ull) {
            elapsed = tape_playback.position_tstates;
        } else if (tape_recorder.position_tstates > 0ull) {
            elapsed = tape_recorder.position_tstates;
        } else if ((tape_input_format == TAPE_FORMAT_WAV ||
                    tape_recorder.output_format == TAPE_OUTPUT_WAV) &&
                   tape_wav_shared_position_tstates > 0ull) {
            elapsed = tape_wav_shared_position_tstates;
        }
    }

    return elapsed;
}

static void tape_format_counter_text(char* buffer, size_t buffer_size) {
    if (!buffer || buffer_size == 0u) {
        return;
    }

    uint64_t elapsed_tstates = tape_current_elapsed_tstates();
    uint64_t clock_hz = (uint64_t)(CPU_CLOCK_HZ + 0.5);
    if (clock_hz == 0ull) {
        clock_hz = 1ull;
    }

    uint64_t total_tenths = (elapsed_tstates * 10ull + clock_hz / 2ull) / clock_hz;
    uint64_t minutes = total_tenths / 600ull;
    uint64_t seconds = (total_tenths / 10ull) % 60ull;
    uint64_t tenths = total_tenths % 10ull;

    if (minutes > 99ull) {
        minutes = 99ull;
    }

    (void)snprintf(buffer,
                   buffer_size,
                   "%02llu:%02llu.%1llu",
                   (unsigned long long)minutes,
                   (unsigned long long)seconds,
                   (unsigned long long)tenths);
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
    return;
}

static void tape_manager_clear_input(void) {
    tape_manager_input_length = 0u;
    if (sizeof(tape_manager_input_buffer) > 0u) {
        tape_manager_input_buffer[0] = '\0';
    }
}

static void tape_manager_browser_normalize_separators(char* path) {
    if (!path) {
        return;
    }
    for (char* cursor = path; *cursor; ++cursor) {
        if (*cursor == '\\') {
            *cursor = '/';
        }
    }
}

static void tape_manager_browser_strip_trailing_separator(char* path) {
    if (!path) {
        return;
    }
    size_t length = strlen(path);
    while (length > 0u) {
        if (path[length - 1u] != '/') {
            break;
        }
        if (length == 1u) {
            break;
        }
#ifdef _WIN32
        if (length == 3u && path[1] == ':' && path[2] == '/') {
            break;
        }
#endif
        path[length - 1u] = '\0';
        --length;
    }
}

static int tape_manager_browser_parent_path(const char* path, char* out, size_t out_size) {
    if (!path || !out || out_size == 0u) {
        return 0;
    }

    size_t length = strlen(path);
    if (length >= out_size) {
        length = out_size - 1u;
    }
    if (length >= out_size) {
        length = out_size - 1u;
    }
    memcpy(out, path, length);
    out[length] = '\0';
    tape_manager_browser_normalize_separators(out);
    tape_manager_browser_strip_trailing_separator(out);

    size_t current_length = strlen(out);
    if (current_length == 0u) {
        (void)snprintf(out, out_size, ".");
        return 1;
    }
    if (strcmp(out, "/") == 0) {
        return 1;
    }
#ifdef _WIN32
    if (current_length == 2u && out[1] == ':') {
        out[2] = '/';
        out[3] = '\0';
        return 1;
    }
    if (current_length == 3u && out[1] == ':' && out[2] == '/') {
        return 1;
    }
#endif

    while (current_length > 0u && out[current_length - 1u] != '/') {
        --current_length;
    }

    if (current_length == 0u) {
        out[0] = '/';
        out[1] = '\0';
    } else if (current_length == 1u) {
        out[1] = '\0';
    } else {
        out[current_length - 1u] = '\0';
        tape_manager_browser_strip_trailing_separator(out);
        if (out[0] == '\0') {
            out[0] = '/';
            out[1] = '\0';
        }
#ifdef _WIN32
        if (strlen(out) == 2u && out[1] == ':') {
            out[2] = '/';
            out[3] = '\0';
        }
#endif
    }

    return 1;
}

static int tape_manager_browser_can_go_up(const char* path) {
    if (!path || !*path) {
        return 0;
    }
    char parent[PATH_MAX];
    if (!tape_manager_browser_parent_path(path, parent, sizeof(parent))) {
        return 0;
    }
    return strcmp(parent, path) != 0;
}

static int tape_browser_entry_compare(const void* a, const void* b) {
    const TapeBrowserEntry* lhs = (const TapeBrowserEntry*)a;
    const TapeBrowserEntry* rhs = (const TapeBrowserEntry*)b;
    if (lhs->is_dir && !rhs->is_dir) {
        return -1;
    }
    if (!lhs->is_dir && rhs->is_dir) {
        return 1;
    }
    return strcmp(lhs->name, rhs->name);
}

static int tape_manager_browser_join_path(const char* base, const char* name, char* out, size_t out_size) {
    if (!name || !out || out_size == 0u) {
        return 0;
    }

    if (!base || !*base) {
        base = ".";
    }

    size_t base_length = strlen(base);
    int need_separator = 0;
    if (base_length > 0u && base[base_length - 1u] != '/') {
        need_separator = 1;
    }

    int written = snprintf(out, out_size, "%s%s%s", base, need_separator ? "/" : "", name);
    if (written < 0) {
        return 0;
    }
    if ((size_t)written >= out_size) {
        return 0;
    }

    tape_manager_browser_normalize_separators(out);
    return 1;
}

static void tape_manager_browser_clamp_selection(void) {
    if (tape_manager_browser_entry_count <= 0) {
        tape_manager_browser_selection = 0;
        tape_manager_browser_scroll = 0;
        return;
    }

    if (tape_manager_browser_selection < 0) {
        tape_manager_browser_selection = 0;
    }
    if (tape_manager_browser_selection >= tape_manager_browser_entry_count) {
        tape_manager_browser_selection = tape_manager_browser_entry_count - 1;
    }

    if (tape_manager_browser_scroll < 0) {
        tape_manager_browser_scroll = 0;
    }

    int max_scroll = tape_manager_browser_entry_count - TAPE_MANAGER_BROWSER_VISIBLE_LINES;
    if (max_scroll < 0) {
        max_scroll = 0;
    }
    if (tape_manager_browser_scroll > max_scroll) {
        tape_manager_browser_scroll = max_scroll;
    }

    if (tape_manager_browser_selection < tape_manager_browser_scroll) {
        tape_manager_browser_scroll = tape_manager_browser_selection;
    }
    if (tape_manager_browser_selection >= tape_manager_browser_scroll + TAPE_MANAGER_BROWSER_VISIBLE_LINES) {
        tape_manager_browser_scroll = tape_manager_browser_selection - TAPE_MANAGER_BROWSER_VISIBLE_LINES + 1;
        if (tape_manager_browser_scroll < 0) {
            tape_manager_browser_scroll = 0;
        }
    }
}

static void tape_manager_browser_move_selection(int delta) {
    if (tape_manager_browser_entry_count <= 0) {
        return;
    }

    int new_selection = tape_manager_browser_selection + delta;
    if (new_selection < 0) {
        new_selection = 0;
    }
    if (new_selection >= tape_manager_browser_entry_count) {
        new_selection = tape_manager_browser_entry_count - 1;
    }

    tape_manager_browser_selection = new_selection;
    tape_manager_browser_clamp_selection();
}

static void tape_manager_browser_page_selection(int delta) {
    if (tape_manager_browser_entry_count <= 0) {
        return;
    }

    int amount = delta * TAPE_MANAGER_BROWSER_VISIBLE_LINES;
    int new_selection = tape_manager_browser_selection + amount;
    if (new_selection < 0) {
        new_selection = 0;
    }
    if (new_selection >= tape_manager_browser_entry_count) {
        new_selection = tape_manager_browser_entry_count - 1;
    }

    tape_manager_browser_selection = new_selection;
    tape_manager_browser_clamp_selection();
}

static void tape_manager_browser_extract_directory(const char* path, char* directory, size_t directory_size) {
    if (!directory || directory_size == 0u) {
        return;
    }

    directory[0] = '\0';
    if (!path || !*path) {
        return;
    }

    size_t length = strlen(path);
    if (length >= directory_size) {
        length = directory_size - 1u;
    }
    if (length >= directory_size) {
        length = directory_size - 1u;
    }

    memcpy(directory, path, length);
    directory[length] = '\0';
    tape_manager_browser_normalize_separators(directory);

    char* last_separator = strrchr(directory, '/');
    if (!last_separator) {
        directory[0] = '.';
        directory[1] = '\0';
        return;
    }

    if (last_separator == directory) {
        directory[1] = '\0';
    } else {
        *last_separator = '\0';
        tape_manager_browser_strip_trailing_separator(directory);
        if (directory[0] == '\0') {
            directory[0] = '.';
            directory[1] = '\0';
        }
    }

#ifdef _WIN32
    if (strlen(directory) == 2u && directory[1] == ':') {
        directory[2] = '/';
        directory[3] = '\0';
    }
#endif
}

static const char* tape_manager_filename(const char* path) {
    if (!path || !*path) {
        return NULL;
    }

    const char* base = path;
    const char* slash = strrchr(path, '/');
    if (slash && slash[1] != '\0') {
        base = slash + 1;
    }
#ifdef _WIN32
    const char* backslash = strrchr(path, '\\');
    if (backslash && backslash[1] != '\0' && backslash + 1 > base) {
        base = backslash + 1;
    }
#else
    const char* backslash = strrchr(path, '\\');
    if (backslash && backslash[1] != '\0' && backslash + 1 > base) {
        base = backslash + 1;
    }
#endif
    if (*base == '\0') {
        return path;
    }
    return base;
}

static void tape_manager_set_status(const char* fmt, ...) {
    if (!fmt) {
        tape_manager_status[0] = '\0';
        return;
    }

    va_list args;
    va_start(args, fmt);
    vsnprintf(tape_manager_status, sizeof(tape_manager_status), fmt, args);
    va_end(args);
}

static void tape_manager_hide(void) {
    if (tape_manager_mode == TAPE_MANAGER_MODE_FILE_INPUT) {
        SDL_StopTextInput();
    }
    tape_manager_mode = TAPE_MANAGER_MODE_HIDDEN;
    tape_manager_clear_input();
}

static void tape_manager_show_menu(void) {
    if (tape_manager_mode == TAPE_MANAGER_MODE_FILE_INPUT) {
        SDL_StopTextInput();
    }
    tape_manager_mode = TAPE_MANAGER_MODE_MENU;
}

static void tape_manager_toggle(void) {
    if (tape_manager_mode == TAPE_MANAGER_MODE_HIDDEN) {
        tape_manager_show_menu();
    } else {
        tape_manager_hide();
    }
}

static int tape_manager_refresh_browser(const char* directory) {
    char target[PATH_MAX];
    if (directory && *directory) {
        size_t length = strlen(directory);
        if (length >= sizeof(target)) {
            length = sizeof(target) - 1u;
        }
        if (length >= sizeof(target)) {
            length = sizeof(target) - 1u;
        }
        memcpy(target, directory, length);
        target[length] = '\0';
    } else {
        if (!TAPE_GETCWD(target, sizeof(target))) {
            target[0] = '.';
            target[1] = '\0';
        }
    }

    tape_manager_browser_normalize_separators(target);
    if (target[0] == '\0') {
        target[0] = '.';
        target[1] = '\0';
    }

    DIR* dir = opendir(target);
    if (!dir) {
        fprintf(stderr, "Failed to open directory '%s': %s\n", target, strerror(errno));
        return 0;
    }

    TapeBrowserEntry entries[TAPE_MANAGER_BROWSER_MAX_ENTRIES];
    int entry_count = 0;

    struct dirent* dent = NULL;
    while ((dent = readdir(dir)) != NULL) {
        if (strcmp(dent->d_name, ".") == 0) {
            continue;
        }
        if (strcmp(dent->d_name, "..") == 0 && !tape_manager_browser_can_go_up(target)) {
            continue;
        }
        if (entry_count >= TAPE_MANAGER_BROWSER_MAX_ENTRIES) {
            break;
        }

        char full_path[PATH_MAX];
        if (!tape_manager_browser_join_path(target, dent->d_name, full_path, sizeof(full_path))) {
            continue;
        }

        int is_dir = 0;
#if defined(DT_DIR)
        if (dent->d_type == DT_DIR) {
            is_dir = 1;
        } else {
#endif
            STAT_STRUCT st;
            if (STAT_FUNC(full_path, &st) == 0 && STAT_ISDIR(st.st_mode)) {
                is_dir = 1;
            }
#if defined(DT_DIR)
        }
#endif

        if (!is_dir) {
            TapeFormat entry_format = tape_format_from_extension(dent->d_name);
            if (entry_format == TAPE_FORMAT_NONE) {
                continue;
            }
        }

        TapeBrowserEntry* slot = &entries[entry_count++];
        memset(slot, 0, sizeof(*slot));
        size_t name_length = strlen(dent->d_name);
        if (name_length >= sizeof(slot->name)) {
            name_length = sizeof(slot->name) - 1u;
        }
        memcpy(slot->name, dent->d_name, name_length);
        slot->name[name_length] = '\0';
        slot->is_dir = is_dir ? 1 : 0;
        slot->is_up = 0;
    }

    closedir(dir);

    qsort(entries, entry_count, sizeof(TapeBrowserEntry), tape_browser_entry_compare);

    strncpy(tape_manager_browser_path, target, sizeof(tape_manager_browser_path) - 1u);
    tape_manager_browser_path[sizeof(tape_manager_browser_path) - 1u] = '\0';

    tape_manager_browser_entry_count = 0;

    if (tape_manager_browser_can_go_up(tape_manager_browser_path)) {
        TapeBrowserEntry* up = &tape_manager_browser_entries[tape_manager_browser_entry_count++];
        memset(up, 0, sizeof(*up));
        up->is_dir = 1;
        up->is_up = 1;
        (void)snprintf(up->name, sizeof(up->name), "..");
    }

    for (int i = 0; i < entry_count && tape_manager_browser_entry_count < TAPE_MANAGER_BROWSER_MAX_ENTRIES; ++i) {
        tape_manager_browser_entries[tape_manager_browser_entry_count] = entries[i];
        tape_manager_browser_entry_count++;
    }

    tape_manager_browser_selection = 0;
    tape_manager_browser_scroll = 0;
    tape_manager_browser_clamp_selection();

    return 1;
}

static void tape_manager_begin_browser(void) {
    if (tape_manager_mode == TAPE_MANAGER_MODE_FILE_INPUT) {
        SDL_StopTextInput();
    }

    char initial_directory[PATH_MAX];
    initial_directory[0] = '\0';

    if (tape_manager_browser_path[0] != '\0') {
        strncpy(initial_directory, tape_manager_browser_path, sizeof(initial_directory) - 1u);
        initial_directory[sizeof(initial_directory) - 1u] = '\0';
    } else if (tape_input_path && *tape_input_path) {
        tape_manager_browser_extract_directory(tape_input_path, initial_directory, sizeof(initial_directory));
    }

    if (!tape_manager_refresh_browser(initial_directory)) {
        if (!tape_manager_refresh_browser(NULL)) {
            tape_manager_set_status("FAILED TO OPEN FILE BROWSER");
            tape_manager_show_menu();
            return;
        }
    }

    tape_manager_mode = TAPE_MANAGER_MODE_FILE_BROWSER;
    if (tape_manager_browser_entry_count > 0) {
        tape_manager_set_status("SELECT FILE AND PRESS RETURN");
    } else {
        tape_manager_set_status("NO TAPES FOUND IN DIRECTORY");
    }
}

static void tape_manager_browser_go_parent(void) {
    if (tape_manager_browser_path[0] == '\0') {
        return;
    }
    char parent[PATH_MAX];
    if (!tape_manager_browser_parent_path(tape_manager_browser_path, parent, sizeof(parent))) {
        return;
    }
    if (strcmp(parent, tape_manager_browser_path) == 0) {
        tape_manager_set_status("ALREADY AT ROOT");
        return;
    }
    if (!tape_manager_refresh_browser(parent)) {
        tape_manager_set_status("FAILED TO OPEN PARENT");
        return;
    }
    if (tape_manager_browser_entry_count > 0) {
        tape_manager_set_status("SELECT FILE AND PRESS RETURN");
    } else {
        tape_manager_set_status("NO TAPES FOUND IN DIRECTORY");
    }
}

static void tape_manager_browser_activate_selection(void) {
    if (tape_manager_browser_entry_count <= 0) {
        return;
    }
    if (tape_manager_browser_selection < 0 || tape_manager_browser_selection >= tape_manager_browser_entry_count) {
        return;
    }

    const TapeBrowserEntry* entry = &tape_manager_browser_entries[tape_manager_browser_selection];
    if (entry->is_up) {
        tape_manager_browser_go_parent();
        return;
    }

    char full_path[PATH_MAX];
    if (!tape_manager_browser_join_path(tape_manager_browser_path, entry->name, full_path, sizeof(full_path))) {
        tape_manager_set_status("PATH TOO LONG");
        return;
    }

    if (entry->is_dir) {
        if (!tape_manager_refresh_browser(full_path)) {
            tape_manager_set_status("FAILED TO OPEN DIRECTORY");
            return;
        }
        if (tape_manager_browser_entry_count > 0) {
            tape_manager_set_status("SELECT FILE AND PRESS RETURN");
        } else {
            tape_manager_set_status("NO TAPES FOUND IN DIRECTORY");
        }
        return;
    }

    (void)tape_manager_load_path(full_path);
}

static void tape_manager_begin_path_input(void) {
    size_t length = 0u;
    if (tape_input_path && *tape_input_path) {
        length = strlen(tape_input_path);
        if (length >= sizeof(tape_manager_input_buffer)) {
            length = sizeof(tape_manager_input_buffer) - 1u;
        }
        memcpy(tape_manager_input_buffer, tape_input_path, length);
    }
    tape_manager_input_buffer[length] = '\0';
    tape_manager_input_length = length;
    tape_manager_mode = TAPE_MANAGER_MODE_FILE_INPUT;
    SDL_StartTextInput();
    tape_manager_set_status("ENTER TAPE PATH AND PRESS RETURN");
}

static void tape_manager_cancel_path_input(void) {
    tape_manager_show_menu();
    tape_manager_clear_input();
}

static void tape_manager_eject_tape(void) {
    if (!tape_input_enabled && tape_input_format == TAPE_FORMAT_NONE) {
        tape_manager_set_status("NO TAPE TO EJECT");
        return;
    }

    tape_pause_playback(&tape_playback, total_t_states);
    tape_recorder_stop_session(total_t_states, 1);
    tape_free_image(&tape_playback.image);
    tape_waveform_reset(&tape_playback.waveform);
    tape_playback.format = TAPE_FORMAT_NONE;
    tape_playback.use_waveform_playback = 0;
    tape_playback.playing = 0;
    tape_input_enabled = 0;
    tape_input_format = TAPE_FORMAT_NONE;
    tape_set_input_path(NULL);
    tape_reset_playback(&tape_playback);
    tape_wav_shared_position_tstates = 0;
    if (tape_recorder.enabled) {
        tape_deck_status = TAPE_DECK_STATUS_STOP;
    } else {
        tape_deck_status = TAPE_DECK_STATUS_IDLE;
    }
    tape_manager_set_status("EJECTED TAPE");
    printf("Tape ejected\n");
}

static int tape_manager_create_blank_tape(const char* path, TapeFormat format) {
    if (!path || format == TAPE_FORMAT_NONE) {
        return 0;
    }

    switch (format) {
        case TAPE_FORMAT_TAP:
            {
                FILE* tf = fopen(path, "wb");
                if (!tf) {
                    fprintf(stderr, "Failed to create TAP file '%s': %s\n", path, strerror(errno));
                    return 0;
                }
                if (fclose(tf) != 0) {
                    fprintf(stderr, "Failed to finalize TAP file '%s': %s\n", path, strerror(errno));
                    return 0;
                }
                printf("Created empty TAP tape %s\n", path);
                return 1;
            }
        case TAPE_FORMAT_TZX:
            {
                FILE* tf = fopen(path, "wb");
                if (!tf) {
                    fprintf(stderr, "Failed to create TZX file '%s': %s\n", path, strerror(errno));
                    return 0;
                }
                uint8_t header[10] = {'Z','X','T','a','p','e','!',0x1A,0x01,0x14};
                if (fwrite(header, sizeof(header), 1, tf) != 1) {
                    fprintf(stderr, "Failed to write TZX header to '%s'\n", path);
                    fclose(tf);
                    return 0;
                }
                if (fclose(tf) != 0) {
                    fprintf(stderr, "Failed to finalize TZX file '%s': %s\n", path, strerror(errno));
                    return 0;
                }
                printf("Created empty TZX tape %s\n", path);
                return 1;
            }
        case TAPE_FORMAT_WAV:
            {
                uint32_t sample_rate = (audio_sample_rate > 0) ? (uint32_t)audio_sample_rate : 44100u;
                if (!tape_create_blank_wav(path, sample_rate)) {
                    return 0;
                }
                return 1;
            }
        default:
            break;
    }

    return 0;
}

static int tape_manager_ensure_path_available(const char* path, TapeFormat format) {
    if (!path || format == TAPE_FORMAT_NONE) {
        return 0;
    }

    const char* base_name = tape_manager_filename(path);
    if (!base_name) {
        base_name = path;
    }

    STAT_STRUCT st;
    if (STAT_FUNC(path, &st) == 0) {
        if (STAT_ISDIR(st.st_mode)) {
            tape_manager_set_status("PATH IS A DIRECTORY");
            return 0;
        }
        return 1;
    }

    if (errno != ENOENT) {
        tape_manager_set_status("FAILED TO ACCESS %s (%s)", base_name, strerror(errno));
        return 0;
    }

    if (!tape_manager_create_blank_tape(path, format)) {
        tape_manager_set_status("FAILED TO CREATE %s", base_name);
        return 0;
    }

    return 1;
}

static int tape_manager_load_path(const char* path) {
    if (!path || !*path) {
        tape_manager_set_status("NO FILE SPECIFIED");
        return 0;
    }

    TapeFormat format = tape_format_from_extension(path);
    if (format == TAPE_FORMAT_NONE) {
        tape_manager_set_status("UNSUPPORTED TAPE FORMAT");
        return 0;
    }

    if (!tape_manager_ensure_path_available(path, format)) {
        return 0;
    }

    TapePlaybackState new_state;
    memset(&new_state, 0, sizeof(new_state));

    if (format == TAPE_FORMAT_WAV) {
        if (!tape_load_wav(path, &new_state)) {
            tape_free_image(&new_state.image);
            tape_waveform_reset(&new_state.waveform);
            tape_manager_set_status("FAILED TO LOAD WAV TAPE");
            return 0;
        }
    } else {
        new_state.format = format;
        if (!tape_load_image(path, format, &new_state.image)) {
            tape_free_image(&new_state.image);
            tape_manager_set_status("FAILED TO LOAD TAPE IMAGE");
            return 0;
        }
        if (!tape_generate_waveform_from_image(&new_state.image, &new_state.waveform)) {
            tape_free_image(&new_state.image);
            tape_waveform_reset(&new_state.waveform);
            tape_manager_set_status("FAILED TO PREPARE TAPE AUDIO");
            return 0;
        }
        new_state.use_waveform_playback = 1;
    }

    tape_reset_playback(&new_state);

    tape_pause_playback(&tape_playback, total_t_states);
    tape_recorder_stop_session(total_t_states, 1);
    tape_free_image(&tape_playback.image);
    tape_waveform_reset(&tape_playback.waveform);

    tape_playback = new_state;
    tape_input_format = format;
    tape_set_input_path(path);

    if (format == TAPE_FORMAT_WAV) {
        tape_playback.use_waveform_playback = 0;
    }

    tape_reset_playback(&tape_playback);

    if (format == TAPE_FORMAT_WAV) {
        tape_input_enabled = 1;
        tape_wav_shared_position_tstates = 0;
        printf("Loaded WAV tape %s (%zu transitions @ %u Hz)\n",
               tape_input_path,
               tape_playback.waveform.count,
               (unsigned)tape_playback.waveform.sample_rate);
        if (tape_playback.waveform.count == 0) {
            fprintf(stderr, "Warning: WAV tape '%s' contains no transitions\n", tape_input_path);
        }
    } else {
        printf("Loaded tape image %s (%zu blocks)\n", tape_input_path, tape_playback.image.count);
        if (tape_playback.image.count == 0) {
            fprintf(stderr, "Warning: tape image '%s' is empty\n", tape_input_path);
            tape_input_enabled = 0;
        } else {
            tape_input_enabled = 1;
        }
    }

    if (tape_input_enabled || tape_recorder.enabled) {
        tape_deck_status = TAPE_DECK_STATUS_STOP;
    } else {
        tape_deck_status = TAPE_DECK_STATUS_IDLE;
    }

    tape_manager_clear_input();
    tape_manager_show_menu();

    const char* base_name = tape_manager_filename(tape_input_path);
    if (!base_name) {
        base_name = tape_input_path ? tape_input_path : "(NONE)";
    }
    tape_manager_set_status("LOADED %s", base_name);
    return 1;
}

static int tape_manager_handle_event(const SDL_Event* event) {
    if (!event) {
        return 0;
    }

    if (tape_manager_mode == TAPE_MANAGER_MODE_HIDDEN) {
        return 0;
    }

    if (event->type == SDL_KEYUP) {
        return 1;
    }

    if (event->type != SDL_KEYDOWN) {
        return 0;
    }

    if (event->key.repeat) {
        return 1;
    }

    SDL_Keycode key = event->key.keysym.sym;
    SDL_Keymod mods = event->key.keysym.mod;

    if (tape_manager_mode == TAPE_MANAGER_MODE_FILE_INPUT) {
        switch (key) {
            case SDLK_ESCAPE:
                tape_manager_cancel_path_input();
                tape_manager_set_status("LOAD CANCELLED");
                return 1;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                tape_manager_input_buffer[tape_manager_input_length] = '\0';
                (void)tape_manager_load_path(tape_manager_input_buffer);
                return 1;
            case SDLK_BACKSPACE:
            case SDLK_DELETE:
                if (tape_manager_input_length > 0u) {
                    tape_manager_input_length--;
                    tape_manager_input_buffer[tape_manager_input_length] = '\0';
                }
                return 1;
            default:
                return 1;
        }
    }

    if (tape_manager_mode == TAPE_MANAGER_MODE_FILE_BROWSER) {
        switch (key) {
            case SDLK_ESCAPE:
                tape_manager_show_menu();
                tape_manager_set_status("BROWSE CANCELLED");
                return 1;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                tape_manager_browser_activate_selection();
                return 1;
            case SDLK_UP:
                tape_manager_browser_move_selection(-1);
                return 1;
            case SDLK_DOWN:
                tape_manager_browser_move_selection(1);
                return 1;
            case SDLK_PAGEUP:
                tape_manager_browser_page_selection(-1);
                return 1;
            case SDLK_PAGEDOWN:
                tape_manager_browser_page_selection(1);
                return 1;
            case SDLK_HOME:
                if (tape_manager_browser_entry_count > 0) {
                    tape_manager_browser_selection = 0;
                    tape_manager_browser_clamp_selection();
                }
                return 1;
            case SDLK_END:
                if (tape_manager_browser_entry_count > 0) {
                    tape_manager_browser_selection = tape_manager_browser_entry_count - 1;
                    tape_manager_browser_clamp_selection();
                }
                return 1;
            case SDLK_BACKSPACE:
                tape_manager_browser_go_parent();
                return 1;
            default:
                return 1;
        }
    }

    switch (key) {
        case SDLK_ESCAPE:
            tape_manager_hide();
            return 1;
        case SDLK_l:
            tape_manager_begin_path_input();
            return 1;
        case SDLK_b:
            tape_manager_begin_browser();
            return 1;
        case SDLK_e:
            tape_manager_eject_tape();
            return 1;
        case SDLK_p:
            tape_deck_play(total_t_states);
            tape_manager_set_status("PLAY COMMAND SENT");
            return 1;
        case SDLK_s:
            tape_deck_stop(total_t_states);
            tape_manager_set_status("STOP COMMAND SENT");
            return 1;
        case SDLK_w:
            tape_deck_rewind(total_t_states);
            tape_manager_set_status("REWIND COMMAND SENT");
            return 1;
        case SDLK_r:
            tape_deck_record(total_t_states, (mods & KMOD_SHIFT) ? 1 : 0);
            if (tape_recorder.recording) {
                if (mods & KMOD_SHIFT) {
                    tape_manager_set_status("APPEND RECORDING ACTIVE");
                } else {
                    tape_manager_set_status("RECORDING ACTIVE");
                }
            } else if (tape_recorder.enabled) {
                tape_manager_set_status("RECORD COMMAND SENT");
            } else {
                tape_manager_set_status("NO RECORDER OUTPUT CONFIGURED");
            }
            return 1;
        default:
            return 1;
    }
}

static int tape_manager_handle_text_input(const SDL_Event* event) {
    if (!event || event->type != SDL_TEXTINPUT) {
        return 0;
    }

    if (tape_manager_mode != TAPE_MANAGER_MODE_FILE_INPUT) {
        return 0;
    }

    const char* text = event->text.text;
    while (text && *text) {
        if (tape_manager_input_length + 1u < sizeof(tape_manager_input_buffer)) {
            tape_manager_input_buffer[tape_manager_input_length++] = *text;
            tape_manager_input_buffer[tape_manager_input_length] = '\0';
        }
        ++text;
    }
    return 1;
}

static void tape_render_manager(void) {
    if (tape_manager_mode == TAPE_MANAGER_MODE_HIDDEN) {
        return;
    }

    int scale = 2;

layout_retry:
    const int spacing = scale;
    const int padding = 12;
    const int line_height = TAPE_OVERLAY_FONT_HEIGHT * scale;
    const int line_gap = scale;
    const int section_gap = line_height;
    const int counter_scale = (scale > 1) ? 3 : 2;
    const int counter_padding = scale * 4;
    const int control_button_size = line_height * 2;
    const int control_button_gap = scale * 4;
    const int text_button_padding = scale * 4;
    const int text_button_height = line_height + scale * 6;
    const int text_button_gap = scale * 4;
    const int input_box_padding = scale * 4;
    const int input_box_height = line_height + scale * 4;

    const uint32_t panel_background = 0x1C1C1CF0u;
    const uint32_t panel_border = 0xFFFFFFFFu;
    const uint32_t title_color = 0xFFFFFFFFu;
    const uint32_t text_color = 0xDDDDDDFFu;
    const uint32_t dim_text_color = 0xB0B0B0FFu;
    const uint32_t status_color = 0x9FD36CFFu;
    const uint32_t counter_background = 0x101820FFu;
    const uint32_t counter_border = 0x4F81EFFFu;
    const uint32_t counter_text_color = 0xE0F2FFFFu;
    const uint32_t text_button_fill = 0x2C2C2CFFu;
    const uint32_t text_button_disabled_fill = 0x242424FFu;
    const uint32_t text_button_border = 0xFFFFFFFFu;
    const uint32_t input_box_border = 0x6FA7FFFFu;

    const char* deck_state_text = "IDLE";
    if (tape_recorder.recording) {
        deck_state_text = "RECORDING";
    } else if (tape_playback.playing) {
        deck_state_text = "PLAYING";
    } else {
        switch (tape_deck_status) {
            case TAPE_DECK_STATUS_PLAY:
                deck_state_text = "PLAY";
                break;
            case TAPE_DECK_STATUS_STOP:
                deck_state_text = "STOP";
                break;
            case TAPE_DECK_STATUS_REWIND:
                deck_state_text = "REWIND";
                break;
            case TAPE_DECK_STATUS_RECORD:
                deck_state_text = "RECORD";
                break;
            case TAPE_DECK_STATUS_IDLE:
            default:
                break;
        }
    }

    const char* tape_name = tape_manager_filename(tape_input_path);
    if (!tape_name) {
        tape_name = (tape_input_path && *tape_input_path) ? tape_input_path : "(NONE)";
    }

    const char* recorder_name = NULL;
    if (tape_recorder.enabled && tape_recorder.output_path && *tape_recorder.output_path) {
        recorder_name = tape_manager_filename(tape_recorder.output_path);
        if (!recorder_name) {
            recorder_name = tape_recorder.output_path;
        }
    }

    char deck_line[64];
    (void)snprintf(deck_line, sizeof(deck_line), "DECK: %s", deck_state_text);
    char tape_line[PATH_MAX + 16];
    (void)snprintf(tape_line, sizeof(tape_line), "TAPE: %s", tape_name);
    char recorder_line[PATH_MAX + 16];
    if (recorder_name) {
        (void)snprintf(recorder_line, sizeof(recorder_line), "RECORDER: %s", recorder_name);
    } else {
        (void)snprintf(recorder_line, sizeof(recorder_line), "RECORDER: (NONE)");
    }

    char counter_text[16];
    tape_format_counter_text(counter_text, sizeof(counter_text));
    int counter_text_width = tape_overlay_text_width(counter_text, counter_scale, spacing);
    int counter_text_height = TAPE_OVERLAY_FONT_HEIGHT * counter_scale;
    int counter_box_width = counter_text_width + counter_padding * 2;
    int counter_box_height = counter_text_height + counter_padding * 2;

    char status_line[sizeof(tape_manager_status) + 16];
    if (tape_manager_status[0] != '\0') {
        (void)snprintf(status_line, sizeof(status_line), "STATUS: %s", tape_manager_status);
    } else {
        (void)snprintf(status_line, sizeof(status_line), "STATUS: READY");
    }

    int record_available = (tape_recorder.enabled ||
                            (tape_input_format == TAPE_FORMAT_WAV && tape_input_path)) ? 1 : 0;
    int show_play = tape_input_enabled ? 1 : 0;
    int show_stop = (tape_input_enabled || tape_recorder.enabled) ? 1 : 0;
    int show_rewind = tape_input_enabled ? 1 : 0;
    int show_record = record_available ? 1 : 0;

    int control_count = 0;
    int control_row_width = 0;
    if (show_play) {
        control_row_width += control_button_size;
        ++control_count;
    }
    if (show_stop) {
        if (control_count > 0) {
            control_row_width += control_button_gap;
        }
        control_row_width += control_button_size;
        ++control_count;
    }
    if (show_rewind) {
        if (control_count > 0) {
            control_row_width += control_button_gap;
        }
        control_row_width += control_button_size;
        ++control_count;
    }
    if (show_record) {
        if (control_count > 0) {
            control_row_width += control_button_gap;
        }
        control_row_width += control_button_size;
        ++control_count;
    }

    struct TapeManagerActionButton {
        const char* label;
        int enabled;
    };

    struct TapeManagerActionButton action_buttons[] = {
        {"LOAD", 1},
        {"BROWSE", 1},
        {"EJECT", (tape_input_enabled || tape_input_format != TAPE_FORMAT_NONE) ? 1 : 0},
        {"CLOSE", 1}
    };
    const int action_button_count = (int)(sizeof(action_buttons) / sizeof(action_buttons[0]));

    int action_button_widths[action_button_count];
    int action_row_width = 0;
    for (int i = 0; i < action_button_count; ++i) {
        int label_width = tape_overlay_text_width(action_buttons[i].label, scale, spacing);
        int button_width = label_width + text_button_padding * 2;
        action_button_widths[i] = button_width;
        if (i > 0) {
            action_row_width += text_button_gap;
        }
        action_row_width += button_width;
    }

    const char* shortcuts_lines[4] = {0};
    int shortcuts_line_widths[4] = {0};
    int shortcuts_line_count = 0;
    int shortcuts_width = 0;
    if (tape_manager_mode == TAPE_MANAGER_MODE_MENU) {
        shortcuts_lines[shortcuts_line_count] =
            "SHORTCUTS: P PLAY  S STOP  W REWIND  R RECORD";
        shortcuts_line_widths[shortcuts_line_count] =
            tape_overlay_text_width(shortcuts_lines[shortcuts_line_count], scale, spacing);
        if (shortcuts_line_widths[shortcuts_line_count] > shortcuts_width) {
            shortcuts_width = shortcuts_line_widths[shortcuts_line_count];
        }
        shortcuts_line_count++;

        shortcuts_lines[shortcuts_line_count] =
            "SHIFT+R APPEND  L LOAD  B BROWSE  E EJECT  TAB/ESC CLOSE";
        shortcuts_line_widths[shortcuts_line_count] =
            tape_overlay_text_width(shortcuts_lines[shortcuts_line_count], scale, spacing);
        if (shortcuts_line_widths[shortcuts_line_count] > shortcuts_width) {
            shortcuts_width = shortcuts_line_widths[shortcuts_line_count];
        }
        shortcuts_line_count++;
    } else if (tape_manager_mode == TAPE_MANAGER_MODE_FILE_BROWSER) {
        shortcuts_lines[shortcuts_line_count] =
            "ARROWS MOVE  RETURN OPEN/LOAD  BACKSPACE UP";
        shortcuts_line_widths[shortcuts_line_count] =
            tape_overlay_text_width(shortcuts_lines[shortcuts_line_count], scale, spacing);
        if (shortcuts_line_widths[shortcuts_line_count] > shortcuts_width) {
            shortcuts_width = shortcuts_line_widths[shortcuts_line_count];
        }
        shortcuts_line_count++;

        shortcuts_lines[shortcuts_line_count] =
            "ESC CANCEL  TAB CLOSE";
        shortcuts_line_widths[shortcuts_line_count] =
            tape_overlay_text_width(shortcuts_lines[shortcuts_line_count], scale, spacing);
        if (shortcuts_line_widths[shortcuts_line_count] > shortcuts_width) {
            shortcuts_width = shortcuts_line_widths[shortcuts_line_count];
        }
        shortcuts_line_count++;
    }

    char input_prompt[48];
    char input_line[PATH_MAX + 4];
    char input_help[64];
    int input_prompt_width = 0;
    int input_line_width = 0;
    int input_box_width = 0;
    int input_help_width = 0;

    char browser_path_line[PATH_MAX + 32];
    int browser_path_width = 0;
    int browser_list_width = 0;
    int browser_visible_lines = 0;
    const char* browser_empty_line = "NO TAPES FOUND";

    if (tape_manager_mode == TAPE_MANAGER_MODE_FILE_INPUT) {
        (void)snprintf(input_prompt, sizeof(input_prompt), "LOAD TAPE PATH:");
        (void)snprintf(input_line, sizeof(input_line), "> %s", tape_manager_input_buffer);
        (void)snprintf(input_help, sizeof(input_help), "RETURN TO LOAD, ESC TO CANCEL, TAB CLOSES");
        input_prompt_width = tape_overlay_text_width(input_prompt, scale, spacing);
        input_line_width = tape_overlay_text_width(input_line, scale, spacing);
        input_box_width = input_line_width + input_box_padding * 2;
        input_help_width = tape_overlay_text_width(input_help, scale, spacing);
    }

    if (tape_manager_mode == TAPE_MANAGER_MODE_FILE_BROWSER) {
        const char* directory_display = (tape_manager_browser_path[0] != '\0') ?
                                        tape_manager_browser_path : ".";
        (void)snprintf(browser_path_line, sizeof(browser_path_line), "BROWSE: %s", directory_display);
        browser_path_width = tape_overlay_text_width(browser_path_line, scale, spacing);

        int start = tape_manager_browser_scroll;
        int end = start + TAPE_MANAGER_BROWSER_VISIBLE_LINES;
        if (end > tape_manager_browser_entry_count) {
            end = tape_manager_browser_entry_count;
        }
        browser_visible_lines = end - start;
        if (browser_visible_lines > 0) {
            for (int i = start; i < end; ++i) {
                const TapeBrowserEntry* entry = &tape_manager_browser_entries[i];
                const char* name = entry->is_up ? ".." : entry->name;
                const char* suffix = entry->is_dir ? "/" : "";
                char entry_line[PATH_MAX + 16];
                char marker = (i == tape_manager_browser_selection) ? '>' : ' ';
                (void)snprintf(entry_line, sizeof(entry_line), "%c %s%s", marker, name, suffix);
                int width = tape_overlay_text_width(entry_line, scale, spacing);
                if (width > browser_list_width) {
                    browser_list_width = width;
                }
            }
        } else {
            int width = tape_overlay_text_width(browser_empty_line, scale, spacing);
            if (width > browser_list_width) {
                browser_list_width = width;
            }
        }
    }

    const int info_line_count = 3;
    int info_width = tape_overlay_text_width(deck_line, scale, spacing);
    int tape_line_width = tape_overlay_text_width(tape_line, scale, spacing);
    int recorder_line_width = tape_overlay_text_width(recorder_line, scale, spacing);
    if (tape_line_width > info_width) {
        info_width = tape_line_width;
    }
    if (recorder_line_width > info_width) {
        info_width = recorder_line_width;
    }

    int panel_width = tape_overlay_text_width("TAPE MANAGER", scale, spacing);
    if (info_width > panel_width) {
        panel_width = info_width;
    }
    if (counter_box_width > panel_width) {
        panel_width = counter_box_width;
    }
    int status_width = tape_overlay_text_width(status_line, scale, spacing);
    if (status_width > panel_width) {
        panel_width = status_width;
    }
    if (control_row_width > panel_width) {
        panel_width = control_row_width;
    }
    if (tape_manager_mode == TAPE_MANAGER_MODE_MENU && action_row_width > panel_width) {
        panel_width = action_row_width;
    }
    if (tape_manager_mode == TAPE_MANAGER_MODE_FILE_BROWSER) {
        if (browser_path_width > panel_width) {
            panel_width = browser_path_width;
        }
        if (browser_list_width > panel_width) {
            panel_width = browser_list_width;
        }
    }
    if ((tape_manager_mode == TAPE_MANAGER_MODE_MENU ||
         tape_manager_mode == TAPE_MANAGER_MODE_FILE_BROWSER) &&
        shortcuts_width > panel_width) {
        panel_width = shortcuts_width;
    }
    if (tape_manager_mode == TAPE_MANAGER_MODE_FILE_INPUT) {
        if (input_prompt_width > panel_width) {
            panel_width = input_prompt_width;
        }
        if (input_box_width > panel_width) {
            panel_width = input_box_width;
        }
        if (input_help_width > panel_width) {
            panel_width = input_help_width;
        }
    }

    panel_width += padding * 2;

    if (panel_width > TOTAL_WIDTH && scale > 1) {
        --scale;
        goto layout_retry;
    }

    int info_height = info_line_count * line_height + (info_line_count - 1) * line_gap;

    int panel_height = padding * 2;
    panel_height += line_height; // title
    panel_height += line_gap;
    panel_height += info_height;
    panel_height += section_gap;
    panel_height += line_height; // counter label
    panel_height += line_gap;
    panel_height += counter_box_height;

    if (tape_manager_mode == TAPE_MANAGER_MODE_MENU) {
        if (control_count > 0) {
            panel_height += section_gap;
            panel_height += control_button_size;
        }

        panel_height += section_gap;
        panel_height += text_button_height;
    } else if (tape_manager_mode == TAPE_MANAGER_MODE_FILE_INPUT) {
        panel_height += section_gap;
        panel_height += line_height; // input prompt
        panel_height += line_gap;
        panel_height += input_box_height;
        panel_height += line_gap;
        panel_height += line_height; // input help
    } else {
        panel_height += section_gap;
        panel_height += line_height; // browser path
        panel_height += line_gap;
        int browser_lines = (browser_visible_lines > 0) ? browser_visible_lines : 1;
        panel_height += browser_lines * line_height;
    }

    panel_height += section_gap;
    panel_height += line_height; // status line

    if ((tape_manager_mode == TAPE_MANAGER_MODE_MENU ||
         tape_manager_mode == TAPE_MANAGER_MODE_FILE_BROWSER) &&
        shortcuts_line_count > 0) {
        panel_height += line_gap;
        panel_height += shortcuts_line_count * line_height;
        if (shortcuts_line_count > 1) {
            panel_height += (shortcuts_line_count - 1) * line_gap;
        }
    }

    int origin_x = (TOTAL_WIDTH - panel_width) / 2;
    int origin_y = (TOTAL_HEIGHT - panel_height) / 2;
    if (origin_x < 0) {
        origin_x = 0;
    }
    if (origin_y < 0) {
        origin_y = 0;
    }

    tape_overlay_draw_rect(origin_x, origin_y, panel_width, panel_height, panel_background, panel_border);

    int cursor_x = origin_x + padding;
    int cursor_y = origin_y + padding;

    tape_overlay_draw_text(cursor_x, cursor_y, "TAPE MANAGER", scale, spacing, title_color);
    cursor_y += line_height + line_gap;

    tape_overlay_draw_text(cursor_x, cursor_y, deck_line, scale, spacing, text_color);
    cursor_y += line_height + line_gap;
    tape_overlay_draw_text(cursor_x, cursor_y, tape_line, scale, spacing, text_color);
    cursor_y += line_height + line_gap;
    tape_overlay_draw_text(cursor_x, cursor_y, recorder_line, scale, spacing, text_color);
    cursor_y += line_height;

    cursor_y += section_gap;

    tape_overlay_draw_text(cursor_x, cursor_y, "COUNTER", scale, spacing, dim_text_color);
    cursor_y += line_height + line_gap;

    int counter_box_x = origin_x + (panel_width - counter_box_width) / 2;
    tape_overlay_draw_rect(counter_box_x, cursor_y, counter_box_width, counter_box_height, counter_background, counter_border);
    int counter_text_x = counter_box_x + (counter_box_width - counter_text_width) / 2;
    int counter_text_y = cursor_y + (counter_box_height - counter_text_height) / 2;
    tape_overlay_draw_text(counter_text_x, counter_text_y, counter_text, counter_scale, spacing, counter_text_color);
    cursor_y += counter_box_height;

    if (tape_manager_mode == TAPE_MANAGER_MODE_MENU) {
        if (control_count > 0) {
            cursor_y += section_gap;
            int controls_x = origin_x + (panel_width - control_row_width) / 2;
            int button_x = controls_x;
            int button_y = cursor_y;
            TapeControlAction highlight_action = tape_control_action_from_status(tape_deck_status);

            if (show_play) {
                tape_overlay_draw_control_button(button_x,
                                                 button_y,
                                                 control_button_size,
                                                 scale,
                                                 TAPE_CONTROL_ACTION_PLAY,
                                                 tape_input_enabled,
                                                 highlight_action == TAPE_CONTROL_ACTION_PLAY);
                button_x += control_button_size + control_button_gap;
            }
            if (show_stop) {
                tape_overlay_draw_control_button(button_x,
                                                 button_y,
                                                 control_button_size,
                                                 scale,
                                                 TAPE_CONTROL_ACTION_STOP,
                                                 show_stop,
                                                 highlight_action == TAPE_CONTROL_ACTION_STOP);
                button_x += control_button_size + control_button_gap;
            }
            if (show_rewind) {
                tape_overlay_draw_control_button(button_x,
                                                 button_y,
                                                 control_button_size,
                                                 scale,
                                                 TAPE_CONTROL_ACTION_REWIND,
                                                 tape_input_enabled,
                                                 highlight_action == TAPE_CONTROL_ACTION_REWIND);
                button_x += control_button_size + control_button_gap;
            }
            if (show_record) {
                tape_overlay_draw_control_button(button_x,
                                                 button_y,
                                                 control_button_size,
                                                 scale,
                                                 TAPE_CONTROL_ACTION_RECORD,
                                                 record_available,
                                                 highlight_action == TAPE_CONTROL_ACTION_RECORD);
            }

            cursor_y += control_button_size;

        }

        cursor_y += section_gap;

        int actions_x = origin_x + (panel_width - action_row_width) / 2;
        int action_y = cursor_y;
        for (int i = 0; i < action_button_count; ++i) {
            uint32_t fill_color = action_buttons[i].enabled ? text_button_fill : text_button_disabled_fill;
            uint32_t label_color = action_buttons[i].enabled ? text_color : dim_text_color;
            int button_width = action_button_widths[i];
            tape_overlay_draw_rect(actions_x, action_y, button_width, text_button_height, fill_color, text_button_border);
            int label_width = tape_overlay_text_width(action_buttons[i].label, scale, spacing);
            int label_x = actions_x + (button_width - label_width) / 2;
            int label_y = action_y + (text_button_height - line_height) / 2;
            tape_overlay_draw_text(label_x, label_y, action_buttons[i].label, scale, spacing, label_color);
            actions_x += button_width + text_button_gap;
        }

        cursor_y += text_button_height;
    } else if (tape_manager_mode == TAPE_MANAGER_MODE_FILE_INPUT) {
        cursor_y += section_gap;

        tape_overlay_draw_text(cursor_x, cursor_y, input_prompt, scale, spacing, dim_text_color);
        cursor_y += line_height + line_gap;

        int input_box_x = origin_x + (panel_width - input_box_width) / 2;
        tape_overlay_draw_rect(input_box_x, cursor_y, input_box_width, input_box_height, text_button_fill, input_box_border);
        int input_text_x = input_box_x + (input_box_width - input_line_width) / 2;
        int input_text_y = cursor_y + (input_box_height - line_height) / 2;
        tape_overlay_draw_text(input_text_x, input_text_y, input_line, scale, spacing, text_color);
        cursor_y += input_box_height + line_gap;

        int help_x = origin_x + (panel_width - input_help_width) / 2;
        tape_overlay_draw_text(help_x, cursor_y, input_help, scale, spacing, dim_text_color);
        cursor_y += line_height;
    } else {
        cursor_y += section_gap;

        tape_overlay_draw_text(cursor_x, cursor_y, browser_path_line, scale, spacing, dim_text_color);
        cursor_y += line_height + line_gap;

        int start = tape_manager_browser_scroll;
        int end = start + TAPE_MANAGER_BROWSER_VISIBLE_LINES;
        if (end > tape_manager_browser_entry_count) {
            end = tape_manager_browser_entry_count;
        }

        if (end > start) {
            for (int i = start; i < end; ++i) {
                const TapeBrowserEntry* entry = &tape_manager_browser_entries[i];
                const char* name = entry->is_up ? ".." : entry->name;
                const char* suffix = entry->is_dir ? "/" : "";
                char entry_line[PATH_MAX + 16];
                char marker = (i == tape_manager_browser_selection) ? '>' : ' ';
                (void)snprintf(entry_line, sizeof(entry_line), "%c %s%s", marker, name, suffix);
                uint32_t entry_color = (i == tape_manager_browser_selection) ? title_color : text_color;
                tape_overlay_draw_text(cursor_x, cursor_y, entry_line, scale, spacing, entry_color);
                cursor_y += line_height;
            }
        } else {
            tape_overlay_draw_text(cursor_x, cursor_y, browser_empty_line, scale, spacing, dim_text_color);
            cursor_y += line_height;
        }
    }

    cursor_y += section_gap;

    int status_x = origin_x + (panel_width - status_width) / 2;
    tape_overlay_draw_text(status_x, cursor_y, status_line, scale, spacing, status_color);
    cursor_y += line_height;

    if ((tape_manager_mode == TAPE_MANAGER_MODE_MENU ||
         tape_manager_mode == TAPE_MANAGER_MODE_FILE_BROWSER) &&
        shortcuts_line_count > 0) {
        cursor_y += line_gap;
        for (int i = 0; i < shortcuts_line_count; ++i) {
            int shortcuts_x = origin_x + (panel_width - shortcuts_line_widths[i]) / 2;
            tape_overlay_draw_text(shortcuts_x,
                                   cursor_y,
                                   shortcuts_lines[i],
                                   scale,
                                   spacing,
                                   dim_text_color);
            cursor_y += line_height;
            if (i + 1 < shortcuts_line_count) {
                cursor_y += line_gap;
            }
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
                    speaker_tape_playback_level = tape_ear_state;
                    speaker_update_output(start_time, 1);
                }
            } else {
                state->phase = TAPE_PHASE_DONE;
                state->playing = 0;
                tape_ear_state = 1;
                speaker_tape_playback_level = tape_ear_state;
                speaker_update_output(start_time, 1);
            }
        }
    } else {
        if (tape_debug_logging) {
            tape_log("Playback complete after block %zu\n", state->current_block);
        }
        state->phase = TAPE_PHASE_DONE;
        state->playing = 0;
        tape_ear_state = 1;
        speaker_tape_playback_level = tape_ear_state;
        speaker_update_output(state->next_transition_tstate, 1);
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
            speaker_tape_playback_level = tape_ear_state;
            speaker_update_output(transition_time, 1);
            state->waveform_index++;
            state->last_transition_tstate = transition_time;
            if (state->waveform_index < state->waveform.count) {
                uint64_t duration = (uint64_t)state->waveform.pulses[state->waveform_index].duration;
                state->next_transition_tstate = transition_time + duration;
            } else {
                state->playing = 0;
                tape_playback_accumulate_elapsed(state, transition_time);
                if (state == &tape_playback &&
                    (state->format == TAPE_FORMAT_WAV ||
                     tape_recorder.output_format == TAPE_OUTPUT_WAV)) {
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
                    speaker_tape_playback_level = tape_ear_state;
                    speaker_update_output(state->pause_end_tstate, 1);
                    tape_playback_accumulate_elapsed(state, state->pause_end_tstate);
                    state->last_transition_tstate = state->pause_end_tstate;
                    if (state == &tape_playback &&
                        (state->format == TAPE_FORMAT_WAV ||
                         tape_recorder.output_format == TAPE_OUTPUT_WAV)) {
                        tape_wav_shared_position_tstates = state->position_tstates;
                    }
                    tape_deck_status = TAPE_DECK_STATUS_STOP;
                    break;
                }
                if (!tape_begin_block(state, state->current_block, state->pause_end_tstate)) {
                    state->phase = TAPE_PHASE_DONE;
                    state->playing = 0;
                    tape_ear_state = 1;
                    speaker_tape_playback_level = tape_ear_state;
                    speaker_update_output(state->pause_end_tstate, 1);
                    tape_playback_accumulate_elapsed(state, state->pause_end_tstate);
                    state->last_transition_tstate = state->pause_end_tstate;
                    if (state == &tape_playback &&
                        (state->format == TAPE_FORMAT_WAV ||
                         tape_recorder.output_format == TAPE_OUTPUT_WAV)) {
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
        speaker_tape_playback_level = tape_ear_state;
        speaker_update_output(transition_time, 1);
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
            if (state == &tape_playback &&
                (state->format == TAPE_FORMAT_WAV ||
                 tape_recorder.output_format == TAPE_OUTPUT_WAV)) {
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
    tape_recorder.idle_start_tstate = 0;
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
            if (head_tstates == 0 && tape_recorder.position_tstates > 0) {
                head_tstates = tape_recorder.position_tstates;
                tape_wav_shared_position_tstates = head_tstates;
            }
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
    tape_recorder.idle_start_tstate = 0;
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
        tape_recorder.idle_start_tstate = 0;
    }

    speaker_tape_record_level = 1;
    speaker_update_output(current_t_state, 1);

    if (finalize_output && tape_recorder.session_dirty) {
        if (!tape_recorder_write_output()) {
            fprintf(stderr, "Failed to save tape recording\n");
        }
    }

    if (tape_recorder.output_format == TAPE_OUTPUT_WAV) {
        tape_wav_shared_position_tstates = 0;
        if (tape_input_format == TAPE_FORMAT_WAV &&
            tape_input_path &&
            tape_recorder.output_path &&
            strcmp(tape_input_path, tape_recorder.output_path) == 0) {
            if (!tape_load_wav(tape_input_path, &tape_playback)) {
                tape_waveform_reset(&tape_playback.waveform);
                tape_input_enabled = 0;
            } else {
                tape_reset_playback(&tape_playback);
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

static int tape_recorder_finalize_block(uint64_t current_t_state, int force_flush) {
    if (!tape_recorder.block_active || tape_recorder.pulse_count == 0) {
        if (force_flush) {
            tape_recorder.block_active = 0;
            tape_recorder.idle_start_tstate = 0;
        }
        return 0;
    }

    uint64_t idle_cycles = 0;
    if (current_t_state > tape_recorder.last_transition_tstate) {
        idle_cycles = current_t_state - tape_recorder.last_transition_tstate;
    }

    if (!force_flush && idle_cycles < TAPE_SILENCE_THRESHOLD_TSTATES) {
        return 0;
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
    if (force_flush) {
        tape_recorder.idle_start_tstate = 0;
    } else {
        tape_recorder.idle_start_tstate = current_t_state;
    }

    return 1;
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
        speaker_tape_record_level = level ? 1 : 0;
        speaker_update_output(t_state, 1);
        tape_recorder.idle_start_tstate = 0;
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
    speaker_tape_record_level = level ? 1 : 0;
    speaker_update_output(t_state, 1);
}

static void tape_recorder_update(uint64_t current_t_state, int force_flush) {
    if (!tape_recorder.enabled) {
        return;
    }
    if (!tape_recorder.recording && !force_flush) {
        return;
    }
    (void)tape_recorder_finalize_block(current_t_state, force_flush);

    if (!force_flush && tape_recorder.recording) {
        uint64_t idle_start = tape_recorder.idle_start_tstate;
        if (idle_start > 0 && current_t_state > idle_start && tape_recorder.session_dirty) {
            uint64_t idle_duration = current_t_state - idle_start;
            if (idle_duration >= TAPE_RECORDER_AUTOSTOP_TSTATES) {
                tape_recorder_stop_session(current_t_state, 1);
            }
        }
    }
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
    tape_manager_hide();
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
    if (key == SDLK_TAB) {
        if (event->type == SDL_KEYDOWN && !event->key.repeat) {
            tape_manager_toggle();
        }
        return 1;
    }

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
                if (tape_manager_mode != TAPE_MANAGER_MODE_HIDDEN) {
                    tape_manager_set_status("PLAY COMMAND SENT");
                }
                break;
            case TAPE_CONTROL_ACTION_STOP:
                tape_deck_stop(total_t_states);
                if (tape_manager_mode != TAPE_MANAGER_MODE_HIDDEN) {
                    tape_manager_set_status("STOP COMMAND SENT");
                }
                break;
            case TAPE_CONTROL_ACTION_REWIND:
                tape_deck_rewind(total_t_states);
                if (tape_manager_mode != TAPE_MANAGER_MODE_HIDDEN) {
                    tape_manager_set_status("REWIND COMMAND SENT");
                }
                break;
            case TAPE_CONTROL_ACTION_RECORD:
                {
                    SDL_Keymod mods = SDL_GetModState();
                    int append_mode = (mods & KMOD_SHIFT) ? 1 : 0;
                    tape_deck_record(total_t_states, append_mode);
                    if (tape_manager_mode != TAPE_MANAGER_MODE_HIDDEN) {
                        if (tape_recorder.recording) {
                            if (append_mode) {
                                tape_manager_set_status("APPEND RECORDING ACTIVE");
                            } else {
                                tape_manager_set_status("RECORDING ACTIVE");
                            }
                        } else if (tape_recorder.enabled) {
                            tape_manager_set_status("RECORD COMMAND SENT");
                        } else {
                            tape_manager_set_status("NO RECORDER OUTPUT CONFIGURED");
                        }
                    }
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
            level = beeper_events[head].level;
            head = (head + 1) % BEEPER_EVENT_CAPACITY;
            ++consumed;
        }

        double raw_sample = (double)level * (double)AUDIO_AMPLITUDE;
        double filtered_sample = raw_sample - last_input + BEEPER_HP_ALPHA * last_output;
        last_input = raw_sample;
        last_output = filtered_sample;

        playback_position = target_position;
    }

    if (playback_position < catch_up_position) {
        double target_position = playback_position + cycles_per_sample;

        while (head != beeper_event_tail &&
               (double)beeper_events[head].t_state <= target_position) {
            level = beeper_events[head].level;
            head = (head + 1) % BEEPER_EVENT_CAPACITY;
            ++consumed;
        }

        double raw_sample = (double)level * (double)AUDIO_AMPLITUDE;
        double filtered_sample = raw_sample - last_input + BEEPER_HP_ALPHA * last_output;
        last_input = raw_sample;
        last_output = filtered_sample;

        playback_position = target_position;
    }

    while (head != beeper_event_tail &&
           (double)beeper_events[head].t_state <= catch_up_position) {
        level = beeper_events[head].level;
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
    beeper_events[beeper_event_tail].level = (int8_t)level;
    beeper_event_tail = next_tail;

    if (locked_audio) {
        SDL_UnlockAudio();
    }
}

void io_write(uint16_t port, uint8_t value) {
    uint64_t access_t_state = total_t_states;
    if ((port & 1) == 0) { // ULA Port FE
        ula_queue_port_value(value);
    }

    if ((port & 1) != 0) {
        access_t_state = spectrum_current_access_tstate();
        apply_port_contention(access_t_state);
    }

    int is_128k_family = (spectrum_model != SPECTRUM_MODEL_48K);
    if (is_128k_family) {
        uint16_t ay_port = (uint16_t)(port & 0xC002u);
        if (ay_port == 0xC000u) {
            ay_selected_register = (uint8_t)(value & 0x0Fu);
            ay_register_latched = 1;
            return;
        }
        if (ay_port == 0x8000u) {
            uint8_t reg = (uint8_t)(ay_selected_register & 0x0Fu);
            ay_write_register(reg, value);
            ay_register_latched = 1;
            return;
        }
    }

    if (is_128k_family && (port & 0x7FFD) == 0x7FFD) {
        if (!paging_disabled) {
            uint8_t masked = (uint8_t)(value & 0x3Fu);
            gate_array_7ffd_state = (uint8_t)((gate_array_7ffd_state & (uint8_t)~0x3Fu) | masked);
            uint8_t new_bank = (uint8_t)(masked & 0x07u);
            if (new_bank != current_paged_bank) {
                current_paged_bank = new_bank;
            }
            if (value & 0x20u) {
                paging_disabled = 1;
            }
            spectrum_apply_memory_configuration();
            spectrum_log_paging_state("write 0x7FFD", port, value, access_t_state);
        } else {
            spectrum_log_paging_state("ignored 0x7FFD", port, value, access_t_state);
        }
        return;
    }

    if ((spectrum_model == SPECTRUM_MODEL_PLUS2A || spectrum_model == SPECTRUM_MODEL_PLUS3) &&
        (port & 0x1FFD) == 0x1FFD) {
        if (!paging_disabled) {
            gate_array_1ffd_state = (uint8_t)(value & 0x07u);
            spectrum_apply_memory_configuration();
            spectrum_log_paging_state("write 0x1FFD", port, value, access_t_state);
        } else {
            spectrum_log_paging_state("ignored 0x1FFD", port, value, access_t_state);
        }
        return;
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

    if (spectrum_model != SPECTRUM_MODEL_48K) {
        uint16_t ay_port = (uint16_t)(port & 0xC002u);
        if (ay_port == 0xC000u || ay_port == 0x8000u) {
            if (!ay_register_latched) {
                return 0xFFu;
            }
            uint8_t reg = (uint8_t)(ay_selected_register & 0x0Fu);
            return ay_registers[reg];
        }
    }

    uint64_t access_t_state = spectrum_current_access_tstate();
    apply_port_contention(access_t_state);
    return spectrum_sample_floating_bus(access_t_state);
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

static inline int parity_even(uint8_t value) {
    value ^= (uint8_t)(value >> 4);
    value ^= (uint8_t)(value >> 2);
    value ^= (uint8_t)(value >> 1);
    return (value & 1u) == 0u;
}

static void spectrum_log_cpu_state(uint64_t tstate) {
    if (!paging_debug_logging || !paging_log_registers || !paging_cpu_state) {
        return;
    }

    const Z80* cpu = paging_cpu_state;
    uint16_t af = (uint16_t)((cpu->reg_A << 8) | cpu->reg_F);
    uint16_t bc = (uint16_t)((cpu->reg_B << 8) | cpu->reg_C);
    uint16_t de = (uint16_t)((cpu->reg_D << 8) | cpu->reg_E);
    uint16_t hl = (uint16_t)((cpu->reg_H << 8) | cpu->reg_L);
    paging_log(
        "    cpu: t=%" PRIu64 " PC=%04X SP=%04X AF=%04X BC=%04X DE=%04X HL=%04X IX=%04X IY=%04X IFF1=%d IM=%d HALT=%d\n",
        tstate,
        (unsigned)cpu->reg_PC,
        (unsigned)cpu->reg_SP,
        (unsigned)af,
        (unsigned)bc,
        (unsigned)de,
        (unsigned)hl,
        (unsigned)cpu->reg_IX,
        (unsigned)cpu->reg_IY,
        cpu->iff1,
        cpu->interruptMode,
        cpu->halted);
}

static inline void set_block_io_flags(Z80* cpu, uint8_t value, uint8_t offset, uint8_t new_b) {
    uint8_t sum8 = (uint8_t)(value + offset);
    uint16_t half = (uint16_t)(value & 0x0Fu) + (uint16_t)(offset & 0x0Fu);
    set_flag(cpu, FLAG_S, new_b & 0x80u);
    set_flag(cpu, FLAG_Z, new_b == 0u);
    set_flag(cpu, FLAG_H, half > 0x0Fu);
    set_flag(cpu, FLAG_N, value & 0x80u);
    set_flag(cpu, FLAG_PV, parity_even((uint8_t)(((sum8 & 0x07u) ^ new_b))));
    set_xy_flags(cpu, sum8);
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
            set_flag(cpu, FLAG_N, 0);
            return 8;
        }
        case 0x48: {
            uint8_t value = io_read(get_BC(cpu));
            cpu->reg_C = value;
            set_flags_szp(cpu, value);
            set_flag(cpu, FLAG_H, 1);
            set_flag(cpu, FLAG_N, 0);
            return 8;
        }
        case 0x50: {
            uint8_t value = io_read(get_BC(cpu));
            cpu->reg_D = value;
            set_flags_szp(cpu, value);
            set_flag(cpu, FLAG_H, 1);
            set_flag(cpu, FLAG_N, 0);
            return 8;
        }
        case 0x58: {
            uint8_t value = io_read(get_BC(cpu));
            cpu->reg_E = value;
            set_flags_szp(cpu, value);
            set_flag(cpu, FLAG_H, 1);
            set_flag(cpu, FLAG_N, 0);
            return 8;
        }
        case 0x60: {
            uint8_t value = io_read(get_BC(cpu));
            cpu->reg_H = value;
            set_flags_szp(cpu, value);
            set_flag(cpu, FLAG_H, 1);
            set_flag(cpu, FLAG_N, 0);
            return 8;
        }
        case 0x68: {
            uint8_t value = io_read(get_BC(cpu));
            cpu->reg_L = value;
            set_flags_szp(cpu, value);
            set_flag(cpu, FLAG_H, 1);
            set_flag(cpu, FLAG_N, 0);
            return 8;
        }
        case 0x70: {
            uint8_t value = io_read(get_BC(cpu));
            set_flags_szp(cpu, value);
            set_flag(cpu, FLAG_H, 1);
            set_flag(cpu, FLAG_N, 0);
            return 8;
        }
        case 0x78: {
            uint8_t value = io_read(get_BC(cpu));
            cpu->reg_A = value;
            set_flags_szp(cpu, value);
            set_flag(cpu, FLAG_H, 1);
            set_flag(cpu, FLAG_N, 0);
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
        case 0xA2: {
            uint16_t bc = get_BC(cpu);
            uint8_t value = io_read(bc);
            uint16_t hl = get_HL(cpu);
            writeByte(hl, value);
            uint16_t new_hl = (uint16_t)(hl + 1u);
            set_HL(cpu, new_hl);
            uint8_t new_b = (uint8_t)(cpu->reg_B - 1u);
            cpu->reg_B = new_b;
            uint8_t offset = (uint8_t)(cpu->reg_C + 1u);
            set_block_io_flags(cpu, value, offset, new_b);
            return 8;
        }
        case 0xB2: {
            uint16_t bc = get_BC(cpu);
            uint8_t value = io_read(bc);
            uint16_t hl = get_HL(cpu);
            writeByte(hl, value);
            uint16_t new_hl = (uint16_t)(hl + 1u);
            set_HL(cpu, new_hl);
            uint8_t new_b = (uint8_t)(cpu->reg_B - 1u);
            cpu->reg_B = new_b;
            uint8_t offset = (uint8_t)(cpu->reg_C + 1u);
            set_block_io_flags(cpu, value, offset, new_b);
            if (new_b != 0u) {
                cpu->reg_PC -= 2;
                return 17;
            }
            return 12;
        }
        case 0xAA: {
            uint16_t bc = get_BC(cpu);
            uint8_t value = io_read(bc);
            uint16_t hl = get_HL(cpu);
            writeByte(hl, value);
            uint16_t new_hl = (uint16_t)(hl - 1u);
            set_HL(cpu, new_hl);
            uint8_t new_b = (uint8_t)(cpu->reg_B - 1u);
            cpu->reg_B = new_b;
            uint8_t offset = (uint8_t)(cpu->reg_C - 1u);
            set_block_io_flags(cpu, value, offset, new_b);
            return 8;
        }
        case 0xBA: {
            uint16_t bc = get_BC(cpu);
            uint8_t value = io_read(bc);
            uint16_t hl = get_HL(cpu);
            writeByte(hl, value);
            uint16_t new_hl = (uint16_t)(hl - 1u);
            set_HL(cpu, new_hl);
            uint8_t new_b = (uint8_t)(cpu->reg_B - 1u);
            cpu->reg_B = new_b;
            uint8_t offset = (uint8_t)(cpu->reg_C - 1u);
            set_block_io_flags(cpu, value, offset, new_b);
            if (new_b != 0u) {
                cpu->reg_PC -= 2;
                return 17;
            }
            return 12;
        }
        case 0xA3: {
            uint16_t hl = get_HL(cpu);
            uint8_t value = readByte(hl);
            io_write(get_BC(cpu), value);
            uint16_t new_hl = (uint16_t)(hl + 1u);
            set_HL(cpu, new_hl);
            uint8_t new_b = (uint8_t)(cpu->reg_B - 1u);
            cpu->reg_B = new_b;
            uint8_t offset = cpu->reg_L;
            set_block_io_flags(cpu, value, offset, new_b);
            return 8;
        }
        case 0xB3: {
            uint16_t hl = get_HL(cpu);
            uint8_t value = readByte(hl);
            io_write(get_BC(cpu), value);
            uint16_t new_hl = (uint16_t)(hl + 1u);
            set_HL(cpu, new_hl);
            uint8_t new_b = (uint8_t)(cpu->reg_B - 1u);
            cpu->reg_B = new_b;
            uint8_t offset = cpu->reg_L;
            set_block_io_flags(cpu, value, offset, new_b);
            if (new_b != 0u) {
                cpu->reg_PC -= 2;
                return 17;
            }
            return 12;
        }
        case 0xAB: {
            uint16_t hl = get_HL(cpu);
            uint8_t value = readByte(hl);
            io_write(get_BC(cpu), value);
            uint16_t new_hl = (uint16_t)(hl - 1u);
            set_HL(cpu, new_hl);
            uint8_t new_b = (uint8_t)(cpu->reg_B - 1u);
            cpu->reg_B = new_b;
            uint8_t offset = cpu->reg_L;
            set_block_io_flags(cpu, value, offset, new_b);
            return 8;
        }
        case 0xBB: {
            uint16_t hl = get_HL(cpu);
            uint8_t value = readByte(hl);
            io_write(get_BC(cpu), value);
            uint16_t new_hl = (uint16_t)(hl - 1u);
            set_HL(cpu, new_hl);
            uint8_t new_b = (uint8_t)(cpu->reg_B - 1u);
            cpu->reg_B = new_b;
            uint8_t offset = cpu->reg_L;
            set_block_io_flags(cpu, value, offset, new_b);
            if (new_b != 0u) {
                cpu->reg_PC -= 2;
                return 17;
            }
            return 12;
        }
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
int cpu_nmi(Z80* cpu) {
    int* previous_progress_ptr = ula_instruction_progress_ptr;
    uint64_t previous_base_tstate = ula_instruction_base_tstate;
    int t_states = 0;

    ula_instruction_base_tstate = total_t_states;
    ula_instruction_progress_ptr = &t_states;

    if (cpu->halted) {
        cpu->reg_PC++;
        cpu->halted = 0;
        t_states += 4;
    }

    cpu->iff2 = cpu->iff1;
    cpu->iff1 = 0;

    cpu->reg_R = (cpu->reg_R + 1) | (cpu->reg_R & 0x80);
    t_states += 5;

    uint8_t pc_high = (uint8_t)(cpu->reg_PC >> 8);
    uint8_t pc_low = (uint8_t)(cpu->reg_PC & 0xFF);

    cpu->reg_SP = (uint16_t)(cpu->reg_SP - 1);
    writeByte(cpu->reg_SP, pc_high);
    t_states += 3;
    cpu->reg_SP = (uint16_t)(cpu->reg_SP - 1);
    writeByte(cpu->reg_SP, pc_low);
    t_states += 3;

    cpu->reg_PC = 0x0066;

    ula_instruction_progress_ptr = previous_progress_ptr;
    ula_instruction_base_tstate = previous_base_tstate;

    return t_states;
}

int cpu_interrupt(Z80* cpu, uint8_t data_bus) {
    int* previous_progress_ptr = ula_instruction_progress_ptr;
    uint64_t previous_base_tstate = ula_instruction_base_tstate;
    int t_states = 0;

    ula_instruction_base_tstate = total_t_states;
    ula_instruction_progress_ptr = &t_states;

    if (cpu->halted) {
        cpu->reg_PC++;
        cpu->halted = 0;
        t_states += 4;
    }

    cpu->iff1 = cpu->iff2 = 0;
    cpu->reg_R = (cpu->reg_R + 1) | (cpu->reg_R & 0x80);

    t_states += 7;

    uint16_t vector = 0x0038;
    switch (cpu->interruptMode) {
        case 0:
        case 1:
            break;
        case 2: {
            uint16_t table_addr = (uint16_t)(((uint16_t)cpu->reg_I << 8) | data_bus);
            uint8_t low = readByte(table_addr);
            t_states += 3;
            uint8_t high = readByte((uint16_t)(table_addr + 1));
            t_states += 3;
            vector = (uint16_t)(((uint16_t)high << 8) | low);
            break;
        }
        default:
            break;
    }

    uint8_t pc_high = (uint8_t)(cpu->reg_PC >> 8);
    uint8_t pc_low = (uint8_t)(cpu->reg_PC & 0xFF);

    cpu->reg_SP = (uint16_t)(cpu->reg_SP - 1);
    writeByte(cpu->reg_SP, pc_high);
    t_states += 3;
    cpu->reg_SP = (uint16_t)(cpu->reg_SP - 1);
    writeByte(cpu->reg_SP, pc_low);
    t_states += 3;

    cpu->reg_PC = vector;

    ula_instruction_progress_ptr = previous_progress_ptr;
    ula_instruction_base_tstate = previous_base_tstate;

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
    memset(ram_pages, 0, sizeof(ram_pages));
    if (rom_page_count == 0u) {
        rom_page_count = 1u;
    }
    spectrum_configure_model(spectrum_model);
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

static int contains_case_insensitive(const char* haystack, const char* needle) {
    if (!haystack || !needle || needle[0] == '\0') {
        return 0;
    }
    size_t needle_len = strlen(needle);
    for (const char* h = haystack; *h; ++h) {
        size_t matched = 0;
        while (matched < needle_len && h[matched] &&
               tolower((unsigned char)h[matched]) == tolower((unsigned char)needle[matched])) {
            matched++;
        }
        if (matched == needle_len) {
            return 1;
        }
    }
    return 0;
}

static int string_equals_case_insensitive(const char* a, const char* b) {
    if (!a || !b) {
        return 0;
    }
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return 0;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
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
    int t_states = cpu_step(&cpu);
    total_t_states += t_states;
    return cpu.reg_B == 0xFF && get_flag(&cpu, FLAG_H) && !get_flag(&cpu, FLAG_N);
}

static bool test_ini_flags(void) {
    Z80 cpu;
    cpu_reset_state(&cpu);
    memory_clear();
    for (int i = 0; i < 8; ++i) {
        keyboard_matrix[i] = 0xFF;
    }
    keyboard_matrix[0] = 0x12;

    cpu.reg_PC = 0x0000;
    cpu.reg_B = 0x02;
    cpu.reg_C = 0x34;
    cpu.reg_H = 0x40;
    cpu.reg_L = 0x00;
    cpu.reg_F = FLAG_C;
    memory[0x0000] = 0xED;
    memory[0x0001] = 0xA2; // INI

    total_t_states = 0;
    int t_states = cpu_step(&cpu);

    uint8_t stored = memory[0x4000];
    bool ok = (t_states == 12) &&
              (cpu.reg_B == 0x01) &&
              (get_HL(&cpu) == 0x4001) &&
              (stored == 0xF2) &&
              !get_flag(&cpu, FLAG_S) &&
              !get_flag(&cpu, FLAG_Z) &&
              !get_flag(&cpu, FLAG_H) &&
              get_flag(&cpu, FLAG_N) &&
              get_flag(&cpu, FLAG_PV) &&
              (cpu.reg_F & FLAG_C) &&
              ((cpu.reg_F & 0x28u) == 0x20u);
    keyboard_matrix[0] = 0xFF;
    if (!ok) {
        printf("    INI t=%d B=%02X HL=%04X stored=%02X F=%02X\n",
               t_states, cpu.reg_B, get_HL(&cpu), stored, cpu.reg_F);
    }
    return ok;
}

static bool test_outd_flags(void) {
    Z80 cpu;
    cpu_reset_state(&cpu);
    memory_clear();
    for (int i = 0; i < 8; ++i) {
        keyboard_matrix[i] = 0xFF;
    }

    cpu.reg_PC = 0x0000;
    cpu.reg_B = 0x02;
    cpu.reg_C = 0x01;
    cpu.reg_H = 0x20;
    cpu.reg_L = 0x01;
    cpu.reg_F = FLAG_C;
    memory[0x0000] = 0xED;
    memory[0x0001] = 0xAB; // OUTD
    memory[0x2001] = 0x40;

    total_t_states = 0;
    int t_states = cpu_step(&cpu);

    bool ok = (t_states == 12) &&
              (cpu.reg_B == 0x01) &&
              (get_HL(&cpu) == 0x2000) &&
              !get_flag(&cpu, FLAG_S) &&
              !get_flag(&cpu, FLAG_Z) &&
              !get_flag(&cpu, FLAG_H) &&
              !get_flag(&cpu, FLAG_N) &&
              !get_flag(&cpu, FLAG_PV) &&
              (cpu.reg_F & FLAG_C) &&
              ((cpu.reg_F & 0x28u) == 0x00u);
    if (!ok) {
        printf("    OUTD t=%d B=%02X HL=%04X F=%02X\n",
               t_states, cpu.reg_B, get_HL(&cpu), cpu.reg_F);
    }
    return ok;
}

static bool test_inir_repeat(void) {
    Z80 cpu;
    cpu_reset_state(&cpu);
    memory_clear();
    for (int i = 0; i < 8; ++i) {
        keyboard_matrix[i] = 0xFF;
    }

    cpu.reg_PC = 0x0000;
    cpu.reg_B = 0x02;
    cpu.reg_C = 0x00;
    cpu.reg_H = 0x40;
    cpu.reg_L = 0x00;
    memory[0x0000] = 0xED;
    memory[0x0001] = 0xB2; // INIR

    total_t_states = 0;
    int t_states = cpu_step(&cpu);

    bool ok = (t_states == 21) &&
              (cpu.reg_B == 0x01) &&
              (get_HL(&cpu) == 0x4001) &&
              (cpu.reg_PC == 0x0000);
    if (!ok) {
        printf("    INIR t=%d B=%02X HL=%04X PC=%04X\n",
               t_states, cpu.reg_B, get_HL(&cpu), cpu.reg_PC);
    }
    return ok;
}

static bool test_otdr_repeat(void) {
    Z80 cpu;
    cpu_reset_state(&cpu);
    memory_clear();

    cpu.reg_PC = 0x0000;
    cpu.reg_B = 0x02;
    cpu.reg_C = 0x01;
    cpu.reg_H = 0x20;
    cpu.reg_L = 0x01;
    memory[0x0000] = 0xED;
    memory[0x0001] = 0xBB; // OTDR
    memory[0x2001] = 0x7F;

    total_t_states = 0;
    int t_states = cpu_step(&cpu);

    bool ok = (t_states == 21) &&
              (cpu.reg_B == 0x01) &&
              (get_HL(&cpu) == 0x2000) &&
              (cpu.reg_PC == 0x0000);
    if (!ok) {
        printf("    OTDR t=%d B=%02X HL=%04X PC=%04X\n",
               t_states, cpu.reg_B, get_HL(&cpu), cpu.reg_PC);
    }
    return ok;
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

static bool test_nmi_stack_behaviour(void) {
    SpectrumModel previous_model = spectrum_model;
    spectrum_configure_model(SPECTRUM_MODEL_48K);
    memory_clear();

    Z80 cpu;
    cpu_reset_state(&cpu);
    cpu.reg_PC = 0x1234;
    cpu.reg_SP = 0xC100;
    cpu.iff1 = 1;
    cpu.iff2 = 0;

    memory[0x0066] = 0xED;
    memory[0x0067] = 0x45; // RETN

    total_t_states = 0;
    int nmi_t = cpu_nmi(&cpu);
    bool ok = (nmi_t == 11) &&
              (cpu.reg_PC == 0x0066) &&
              (cpu.reg_SP == 0xC0FE) &&
              (cpu.iff1 == 0) &&
              (cpu.iff2 == 1) &&
              (memory[0xC0FF] == 0x12) &&
              (memory[0xC0FE] == 0x34);

    int retn_t = 0;
    if (ok) {
        retn_t = cpu_step(&cpu);
        ok = (retn_t == 14) &&
             (cpu.reg_PC == 0x1234) &&
             (cpu.reg_SP == 0xC100) &&
             (cpu.iff1 == 1) &&
             (cpu.iff2 == 1);
    }

    if (!ok) {
        printf("    NMI state PC=%04X SP=%04X IFF1=%d IFF2=%d stack=%02X%02X nmi_t=%d retn_t=%d\n",
               cpu.reg_PC,
               cpu.reg_SP,
               cpu.iff1,
               cpu.iff2,
               memory[0xC0FF],
               memory[0xC0FE],
               nmi_t,
               retn_t);
    }

    spectrum_configure_model(previous_model);
    memory_clear();
    return ok;
}

static bool test_floating_bus_samples_screen_memory(void) {
    SpectrumModel previous_model = spectrum_model;
    SpectrumContentionProfile previous_profile = spectrum_contention_profile;
    PeripheralContentionProfile previous_peripheral = peripheral_contention_profile;

    spectrum_configure_model(SPECTRUM_MODEL_48K);
    memory_clear();
    spectrum_set_contention_profile(CONTENTION_PROFILE_48K);
    spectrum_set_peripheral_contention_profile(PERIPHERAL_CONTENTION_NONE);

    memory[VRAM_START] = 0x3Cu;
    memory[ATTR_START] = 0x5Au;
    spectrum_reset_floating_bus();

    Z80 cpu;
    cpu_reset_state(&cpu);
    memory[0x0000] = 0xDB; // IN A,(n)
    memory[0x0001] = 0xFF; // Port 0xFF (floating bus)

    const uint64_t pixel_target = 14336u + 48u;
    total_t_states = pixel_target - 4u;
    cpu.reg_PC = 0x0000;
    int pixel_t = cpu_step(&cpu);
    uint8_t pixel_sample = cpu.reg_A;

    cpu_reset_state(&cpu);
    spectrum_reset_floating_bus();
    memory[0x0000] = 0xDB;
    memory[0x0001] = 0xFF;
    const uint64_t attr_target = 14336u + 50u;
    total_t_states = attr_target - 4u;
    cpu.reg_PC = 0x0000;
    int attr_t = cpu_step(&cpu);
    uint8_t attr_sample = cpu.reg_A;

    bool timings_ok = (pixel_t >= 11) && (attr_t >= 11);
    bool samples_ok = (pixel_sample == 0x3Cu) && (attr_sample == 0x5Au);

    if (!(timings_ok && samples_ok)) {
        printf("    floating bus debug pixel=%02X attr=%02X t_pix=%d t_attr=%d\n",
               pixel_sample,
               attr_sample,
               pixel_t,
               attr_t);
    }

    spectrum_configure_model(previous_model);
    memory_clear();
    spectrum_set_contention_profile(previous_profile);
    spectrum_set_peripheral_contention_profile(previous_peripheral);

    return timings_ok && samples_ok;
}

static bool test_plus2a_contention_profile(void) {
    SpectrumModel previous_model = spectrum_model;
    SpectrumContentionProfile previous_profile = spectrum_contention_profile;
    PeripheralContentionProfile previous_peripheral = peripheral_contention_profile;

    spectrum_configure_model(SPECTRUM_MODEL_128K);
    memory_clear();
    spectrum_set_contention_profile(CONTENTION_PROFILE_128K_PLUS2A);
    spectrum_set_peripheral_contention_profile(PERIPHERAL_CONTENTION_NONE);

    spectrum_map_upper_bank(4u);
    spectrum_update_contention_flags();
    uint8_t contended_bank4 = page_contended[3];

    spectrum_map_upper_bank(2u);
    spectrum_update_contention_flags();
    uint8_t contended_bank2 = page_contended[3];

    uint8_t screen_flag = page_contended[1];

    spectrum_configure_model(previous_model);
    memory_clear();
    spectrum_set_contention_profile(previous_profile);
    spectrum_set_peripheral_contention_profile(previous_peripheral);

    return (contended_bank4 == 1u) && (contended_bank2 == 0u) && (screen_flag == 1u);
}

static bool test_plus3_rom_and_all_ram_paging(void) {
    SpectrumModel previous_model = spectrum_model;
    SpectrumContentionProfile previous_profile = spectrum_contention_profile;
    PeripheralContentionProfile previous_peripheral = peripheral_contention_profile;

    uint8_t previous_rom_count = rom_page_count;

    spectrum_configure_model(SPECTRUM_MODEL_PLUS3);
    memory_clear();
    spectrum_set_contention_profile(CONTENTION_PROFILE_128K_PLUS3);
    spectrum_set_peripheral_contention_profile(PERIPHERAL_CONTENTION_NONE);

    rom_page_count = 4u;
    for (uint8_t i = 0; i < 4u; ++i) {
        memset(rom_pages[i], (int)(0x30u + i), 0x4000);
    }
    spectrum_map_rom_page(0u);

    uint8_t rom0_val = readByte(0x0000);
    bool rom0_ok = (rom0_val == 0x30u);

    io_write(0x7FFD, 0x10u);
    io_write(0x1FFD, 0x01u);
    uint8_t rom3_val = readByte(0x0000);
    bool rom3_ok = (rom3_val == 0x33u);

    ram_pages[0][0] = 0xAAu;
    io_write(0x1FFD, 0x02u);
    uint8_t all_ram_val = readByte(0x0000);
    bool all_ram_ok = (all_ram_val == 0xAAu);

    io_write(0x1FFD, 0x01u);
    uint8_t rom_restore_val = readByte(0x0000);
    bool rom_restore_ok = (rom_restore_val == 0x33u);

    if (!(rom0_ok && rom3_ok && all_ram_ok && rom_restore_ok)) {
        printf("    plus3 rom debug: rom0=%02X rom3=%02X all_ram=%02X restore=%02X\n",
               rom0_val,
               rom3_val,
               all_ram_val,
               rom_restore_val);
    }

    spectrum_configure_model(previous_model);
    memory_clear();
    spectrum_set_contention_profile(previous_profile);
    spectrum_set_peripheral_contention_profile(previous_peripheral);
    rom_page_count = previous_rom_count;
    spectrum_apply_memory_configuration();

    return rom0_ok && rom3_ok && all_ram_ok && rom_restore_ok;
}

static bool test_plus3_special_paging_modes(void) {
    SpectrumModel previous_model = spectrum_model;
    SpectrumContentionProfile previous_profile = spectrum_contention_profile;
    PeripheralContentionProfile previous_peripheral = peripheral_contention_profile;
    uint8_t previous_rom_count = rom_page_count;

    spectrum_configure_model(SPECTRUM_MODEL_PLUS3);
    memory_clear();
    spectrum_set_contention_profile(CONTENTION_PROFILE_128K_PLUS3);
    spectrum_set_peripheral_contention_profile(PERIPHERAL_CONTENTION_NONE);

    uint8_t markers[8];
    for (uint8_t bank = 0; bank < 8u; ++bank) {
        markers[bank] = (uint8_t)(0x40u + bank);
        memset(ram_pages[bank], 0, 0x4000);
    }

    writeByte(0x4000, markers[5]);
    writeByte(0x8000, markers[2]);
    writeByte(0xC000, markers[0]);
    for (uint8_t bank = 1; bank < 8u; ++bank) {
        spectrum_map_upper_bank(bank);
        writeByte(0xC000, markers[bank]);
    }
    spectrum_map_upper_bank(0u);

    static const uint8_t maps[4][4] = {
        {0u, 1u, 2u, 3u},
        {4u, 5u, 6u, 7u},
        {4u, 5u, 6u, 3u},
        {4u, 7u, 6u, 3u}
    };

    bool mapping_ok = true;
    for (uint8_t config = 0; config < 4u; ++config) {
        io_write(0x1FFD, (uint8_t)(0x04u | config));
        for (uint8_t page = 0; page < 4u; ++page) {
            uint16_t addr = (uint16_t)(page * 0x4000u);
            uint8_t expected = markers[maps[config][page]];
            uint8_t observed = readByte(addr);
            if (observed != expected) {
                mapping_ok = false;
                printf("    plus3 special debug config=%u page=%u expect=%02X got=%02X\n",
                       (unsigned)config,
                       (unsigned)page,
                       expected,
                       observed);
            }
        }
        if (current_screen_bank != maps[config][1]) {
            mapping_ok = false;
            printf("    plus3 special screen config=%u expect bank %u got %u\n",
                   (unsigned)config,
                   (unsigned)maps[config][1],
                   (unsigned)current_screen_bank);
        }
    }

    io_write(0x1FFD, 0x04u);
    io_write(0x7FFD, 0x20u);
    io_write(0x1FFD, 0x05u);
    uint8_t disable_low = readByte(0x0000);
    uint8_t disable_high = readByte(0x4000);
    bool disable_ok = (disable_low == markers[maps[0][0]]) &&
                      (disable_high == markers[maps[0][1]]);
    if (!disable_ok) {
        printf("    plus3 special disable expect %02X/%02X got %02X/%02X\n",
               markers[maps[0][0]],
               markers[maps[0][1]],
               disable_low,
               disable_high);
    }

    spectrum_configure_model(previous_model);
    memory_clear();
    spectrum_set_contention_profile(previous_profile);
    spectrum_set_peripheral_contention_profile(previous_peripheral);
    rom_page_count = previous_rom_count;
    spectrum_apply_memory_configuration();

    return mapping_ok && disable_ok;
}

static bool test_peripheral_port_contention(void) {
    SpectrumModel previous_model = spectrum_model;
    SpectrumContentionProfile previous_profile = spectrum_contention_profile;
    PeripheralContentionProfile previous_peripheral = peripheral_contention_profile;

    spectrum_configure_model(SPECTRUM_MODEL_48K);
    memory_clear();
    spectrum_set_contention_profile(CONTENTION_PROFILE_48K);
    spectrum_set_peripheral_contention_profile(PERIPHERAL_CONTENTION_NONE);

    memory[VRAM_START] = 0x66u;
    memory[0x0000] = 0xDB;
    memory[0x0001] = 0xFF;

    Z80 cpu;
    cpu_reset_state(&cpu);
    total_t_states = (14336u + 48u) - 4u;
    cpu.reg_PC = 0x0000;
    int base_t = cpu_step(&cpu);

    cpu_reset_state(&cpu);
    memory[0x0000] = 0xDB;
    memory[0x0001] = 0xFF;
    spectrum_reset_floating_bus();
    spectrum_set_peripheral_contention_profile(PERIPHERAL_CONTENTION_IF1);
    total_t_states = (14336u + 48u) - 4u;
    cpu.reg_PC = 0x0000;
    int contended_t = cpu_step(&cpu);

    bool contention_effective = contended_t > base_t;

    spectrum_configure_model(previous_model);
    memory_clear();
    spectrum_set_contention_profile(previous_profile);
    spectrum_set_peripheral_contention_profile(previous_peripheral);

    return contention_effective;
}

static bool test_plus3_contention_penalty_shift(void) {
    SpectrumModel previous_model = spectrum_model;
    SpectrumContentionProfile previous_profile = spectrum_contention_profile;
    PeripheralContentionProfile previous_peripheral = peripheral_contention_profile;

    spectrum_configure_model(SPECTRUM_MODEL_48K);
    memory_clear();
    spectrum_set_contention_profile(CONTENTION_PROFILE_48K);
    spectrum_set_peripheral_contention_profile(PERIPHERAL_CONTENTION_NONE);

    Z80 cpu;
    cpu_reset_state(&cpu);
    memory[0x0000] = 0x7E; // LD A,(HL)
    memory[0x4000] = 0x11;

    cpu.reg_PC = 0x0000;
    cpu.reg_H = 0x40;
    cpu.reg_L = 0x00;
    total_t_states = 0;
    int base_48k = cpu_step(&cpu);

    cpu_reset_state(&cpu);
    memory[0x0000] = 0x7E;
    cpu.reg_PC = 0x0000;
    cpu.reg_H = 0x40;
    cpu.reg_L = 0x00;
    total_t_states = 14336ULL;
    int contended_48k = cpu_step(&cpu);

    spectrum_configure_model(SPECTRUM_MODEL_PLUS3);
    memory_clear();
    spectrum_set_contention_profile(CONTENTION_PROFILE_128K_PLUS3);
    spectrum_set_peripheral_contention_profile(PERIPHERAL_CONTENTION_NONE);

    cpu_reset_state(&cpu);
    memory[0x0000] = 0x7E;
    memory[0x4000] = 0x22;
    cpu.reg_PC = 0x0000;
    cpu.reg_H = 0x40;
    cpu.reg_L = 0x00;
    total_t_states = 0;
    int base_plus3 = cpu_step(&cpu);

    cpu_reset_state(&cpu);
    memory[0x0000] = 0x7E;
    memory[0x4000] = 0x33;
    cpu.reg_PC = 0x0000;
    cpu.reg_H = 0x40;
    cpu.reg_L = 0x00;
    total_t_states = 14336ULL;
    int plus3_phase0 = cpu_step(&cpu);

    cpu_reset_state(&cpu);
    memory[0x0000] = 0x7E;
    memory[0x4000] = 0x44;
    cpu.reg_PC = 0x0000;
    cpu.reg_H = 0x40;
    cpu.reg_L = 0x00;
    total_t_states = 14337ULL;
    int plus3_phase1 = cpu_step(&cpu);

    int early_penalty = contended_48k - base_48k;
    int plus3_penalty0 = plus3_phase0 - base_plus3;
    int plus3_penalty1 = plus3_phase1 - base_plus3;

    bool base_matches = (base_plus3 == base_48k);
    bool early_profile_penalises = (early_penalty > 0);
    bool late_profile_offsets = base_matches &&
                                (plus3_penalty0 == early_penalty + 1) &&
                                (plus3_penalty1 == early_penalty) &&
                                (plus3_phase0 > plus3_phase1);

    if (!early_profile_penalises || !late_profile_offsets) {
        printf("    late gate-array debug 48k=%d/%d(+%d) plus3=%d/%d/%d(+%d/+%d) base_match=%d\n",
               contended_48k,
               base_48k,
               early_penalty,
               base_plus3,
               plus3_phase0,
               plus3_phase1,
               plus3_penalty0,
               plus3_penalty1,
               base_matches);
    }

    spectrum_configure_model(previous_model);
    memory_clear();
    spectrum_set_contention_profile(previous_profile);
    spectrum_set_peripheral_contention_profile(previous_peripheral);

    return early_profile_penalises && late_profile_offsets;
}

static bool test_plus3_peripheral_wait_states(void) {
    SpectrumModel previous_model = spectrum_model;
    SpectrumContentionProfile previous_profile = spectrum_contention_profile;
    PeripheralContentionProfile previous_peripheral = peripheral_contention_profile;

    spectrum_configure_model(SPECTRUM_MODEL_PLUS3);
    memory_clear();
    spectrum_set_contention_profile(CONTENTION_PROFILE_128K_PLUS3);
    spectrum_set_peripheral_contention_profile(PERIPHERAL_CONTENTION_NONE);

    Z80 cpu;
    cpu_reset_state(&cpu);
    memory[0x0000] = 0xDB; // IN A,(n)
    memory[0x0001] = 0xFF;
    cpu.reg_A = 0x00;
    cpu.reg_PC = 0x0000;
    total_t_states = 0;
    int base_t = cpu_step(&cpu);

    cpu_reset_state(&cpu);
    memory[0x0000] = 0xDB;
    memory[0x0001] = 0xFF;
    cpu.reg_A = 0x00;
    cpu.reg_PC = 0x0000;
    spectrum_set_peripheral_contention_profile(PERIPHERAL_CONTENTION_PLUS3);
    total_t_states = 0;
    int plus3_t = cpu_step(&cpu);

    bool wait_states_ok = (plus3_t == base_t + 3);
    if (!wait_states_ok) {
        printf("    plus3 peripheral wait debug base=%d wait=%d\n", base_t, plus3_t);
    }

    spectrum_configure_model(previous_model);
    memory_clear();
    spectrum_set_contention_profile(previous_profile);
    spectrum_set_peripheral_contention_profile(previous_peripheral);

    return wait_states_ok;
}

static bool test_128k_bank_switching(void) {
    SpectrumModel previous_model = spectrum_model;
    spectrum_configure_model(SPECTRUM_MODEL_128K);
    memory_clear();

    writeByte(0xC000, 0x12);
    uint8_t before_flag = page_contended[3];

    io_write(0x7FFD, 0x01); // Page bank 1 into 0xC000
    uint8_t during_flag = page_contended[3];
    bool saved_bank0 = (ram_pages[0][0] == 0x12) && (memory[0xC000] == 0x00);

    writeByte(0xC000, 0x77);
    bool bank1_written = (ram_pages[1][0] == 0x77);

    io_write(0x7FFD, 0x00); // Restore bank 0
    uint8_t after_flag = page_contended[3];
    bool restored = (memory[0xC000] == 0x12);

    spectrum_configure_model(previous_model);
    memory_clear();

    return (before_flag == 0u) && (during_flag == 1u) && (after_flag == 0u) && saved_bank0 && bank1_written && restored;
}

static bool test_128k_contention_penalty(void) {
    SpectrumModel previous_model = spectrum_model;
    spectrum_configure_model(SPECTRUM_MODEL_128K);
    memory_clear();

    Z80 cpu;
    cpu_reset_state(&cpu);
    memory[0x0000] = 0x7E; // LD A,(HL)
    cpu.reg_PC = 0x0000;
    cpu.reg_H = 0xC0;
    cpu.reg_L = 0x00;
    writeByte(0xC000, 0x42); // Bank 0 (uncontended)
    total_t_states = 14336ULL;
    int uncontended_t = cpu_step(&cpu);
    bool base_ok = (cpu.reg_A == 0x42);

    cpu_reset_state(&cpu);
    cpu.reg_PC = 0x0000;
    cpu.reg_H = 0xC0;
    cpu.reg_L = 0x00;
    memory[0x0000] = 0x7E;
    io_write(0x7FFD, 0x01); // Switch to contended bank 1
    writeByte(0xC000, 0x24);
    total_t_states = 14336ULL;
    int contended_t = cpu_step(&cpu);
    bool cont_ok = (cpu.reg_A == 0x24);
    uint8_t observed_mem = memory[0xC000];
    uint8_t observed_a = cpu.reg_A;
    io_write(0x7FFD, 0x00);

    spectrum_configure_model(previous_model);
    memory_clear();

    bool ok = base_ok && cont_ok && (contended_t > uncontended_t);
    if (!ok) {
        printf("    contention debug base=%d cont=%d base_ok=%d cont_ok=%d mem=%02X a=%02X\n",
               uncontended_t,
               contended_t,
               base_ok,
               cont_ok,
               observed_mem,
               observed_a);
    }
    return ok;
}

static const char snapshot_fixture_default_dir[] = "tests/snapshots";

static int snapshot_fixture_path(char* buffer,
                                 size_t capacity,
                                 const char* override_dir,
                                 const char* filename) {
    if (!buffer || capacity == 0u || !filename) {
        return 0;
    }

    const char* base = snapshot_fixture_default_dir;
    if (override_dir && override_dir[0] != '\0') {
        base = override_dir;
    }

    int required = snprintf(buffer, capacity, "%s/%s", base, filename);
    if (required < 0 || (size_t)required >= capacity) {
        return 0;
    }
    return 1;
}

static int snapshot_fixture_file_exists(const char* path) {
    if (!path) {
        return 0;
    }
    FILE* file = fopen(path, "rb");
    if (!file) {
        return 0;
    }
    fclose(file);
    return 1;
}

static int snapshot_fixture_ensure_directory(const char* path) {
    if (!path || path[0] == '\0') {
        return 0;
    }
#ifdef _WIN32
    if (_mkdir(path) == 0 || errno == EEXIST) {
        return 1;
    }
#else
    if (mkdir(path, 0777) == 0 || errno == EEXIST) {
        return 1;
    }
#endif
    fprintf(stderr, "Failed to create directory '%s': %s\n", path, strerror(errno));
    return 0;
}

static int snapshot_fixture_uses_default_dir(const char* override_dir) {
    if (!override_dir || override_dir[0] == '\0') {
        return 1;
    }
    return strcmp(override_dir, snapshot_fixture_default_dir) == 0;
}

static int snapshot_fixture_decode_base64(const char* source_path, const char* output_path) {
    FILE* input = fopen(source_path, "rb");
    if (!input) {
        fprintf(stderr, "Failed to open Base64 snapshot '%s': %s\n", source_path, strerror(errno));
        return 0;
    }

    if (fseek(input, 0, SEEK_END) != 0) {
        fprintf(stderr, "Failed to seek within Base64 snapshot '%s'\n", source_path);
        fclose(input);
        return 0;
    }

    long raw_size = ftell(input);
    if (raw_size < 0) {
        fprintf(stderr, "Failed to determine size of Base64 snapshot '%s'\n", source_path);
        fclose(input);
        return 0;
    }
    if (fseek(input, 0, SEEK_SET) != 0) {
        fprintf(stderr, "Failed to rewind Base64 snapshot '%s'\n", source_path);
        fclose(input);
        return 0;
    }

    char* buffer = (char*)malloc((size_t)raw_size);
    if (!buffer) {
        fprintf(stderr, "Out of memory decoding Base64 snapshot '%s'\n", source_path);
        fclose(input);
        return 0;
    }

    size_t read = fread(buffer, 1u, (size_t)raw_size, input);
    fclose(input);
    if (read != (size_t)raw_size) {
        fprintf(stderr, "Failed to read Base64 snapshot '%s'\n", source_path);
        free(buffer);
        return 0;
    }

    size_t cleaned_length = 0u;
    for (size_t i = 0; i < read; ++i) {
        unsigned char ch = (unsigned char)buffer[i];
        if (ch == '\r' || ch == '\n' || ch == '\t' || ch == ' ') {
            continue;
        }
        buffer[cleaned_length++] = (char)ch;
    }

    if ((cleaned_length % 4u) != 0u) {
        fprintf(stderr, "Base64 snapshot '%s' has invalid length\n", source_path);
        free(buffer);
        return 0;
    }

    FILE* output = fopen(output_path, "wb");
    if (!output) {
        fprintf(stderr, "Failed to create snapshot fixture '%s': %s\n", output_path, strerror(errno));
        free(buffer);
        return 0;
    }

    int success = 1;
    for (size_t i = 0; i < cleaned_length; i += 4u) {
        unsigned char c0 = (unsigned char)buffer[i + 0u];
        unsigned char c1 = (unsigned char)buffer[i + 1u];
        unsigned char c2 = (unsigned char)buffer[i + 2u];
        unsigned char c3 = (unsigned char)buffer[i + 3u];

        if (c0 == '=' || c1 == '=') {
            success = 0;
            break;
        }

        int v0;
        int v1;
        int v2 = -1;
        int v3 = -1;

        if (c0 >= 'A' && c0 <= 'Z') {
            v0 = (int)(c0 - 'A');
        } else if (c0 >= 'a' && c0 <= 'z') {
            v0 = (int)(c0 - 'a' + 26);
        } else if (c0 >= '0' && c0 <= '9') {
            v0 = (int)(c0 - '0' + 52);
        } else if (c0 == '+') {
            v0 = 62;
        } else if (c0 == '/') {
            v0 = 63;
        } else {
            success = 0;
            break;
        }

        if (c1 >= 'A' && c1 <= 'Z') {
            v1 = (int)(c1 - 'A');
        } else if (c1 >= 'a' && c1 <= 'z') {
            v1 = (int)(c1 - 'a' + 26);
        } else if (c1 >= '0' && c1 <= '9') {
            v1 = (int)(c1 - '0' + 52);
        } else if (c1 == '+') {
            v1 = 62;
        } else if (c1 == '/') {
            v1 = 63;
        } else {
            success = 0;
            break;
        }

        if (c2 != '=') {
            if (c2 >= 'A' && c2 <= 'Z') {
                v2 = (int)(c2 - 'A');
            } else if (c2 >= 'a' && c2 <= 'z') {
                v2 = (int)(c2 - 'a' + 26);
            } else if (c2 >= '0' && c2 <= '9') {
                v2 = (int)(c2 - '0' + 52);
            } else if (c2 == '+') {
                v2 = 62;
            } else if (c2 == '/') {
                v2 = 63;
            } else {
                success = 0;
                break;
            }
        }

        if (c3 != '=') {
            if (c3 >= 'A' && c3 <= 'Z') {
                v3 = (int)(c3 - 'A');
            } else if (c3 >= 'a' && c3 <= 'z') {
                v3 = (int)(c3 - 'a' + 26);
            } else if (c3 >= '0' && c3 <= '9') {
                v3 = (int)(c3 - '0' + 52);
            } else if (c3 == '+') {
                v3 = 62;
            } else if (c3 == '/') {
                v3 = 63;
            } else {
                success = 0;
                break;
            }
        }

        uint32_t triple = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12);
        if (c2 != '=') {
            triple |= ((uint32_t)v2 << 6);
        }
        if (c3 != '=') {
            triple |= (uint32_t)v3;
        }

        fputc((int)((triple >> 16) & 0xFFu), output);
        if (c2 != '=') {
            fputc((int)((triple >> 8) & 0xFFu), output);
            if (c3 != '=') {
                fputc((int)(triple & 0xFFu), output);
            }
        } else if (c3 != '=') {
            success = 0;
            break;
        }

        if ((c2 == '=' || c3 == '=') && (i + 4u) != cleaned_length) {
            success = 0;
            break;
        }
    }

    free(buffer);

    if (fclose(output) != 0) {
        fprintf(stderr, "Failed to finalize snapshot fixture '%s': %s\n", output_path, strerror(errno));
        success = 0;
    }

    if (!success) {
        fprintf(stderr, "Failed to decode Base64 snapshot '%s'\n", source_path);
        remove(output_path);
    }

    return success;
}

static int snapshot_fixture_generate_base64(const char* filename, const char* output_path) {
    if (!filename || !output_path) {
        return 0;
    }

    char encoded_name[256];
    int encoded_len = snprintf(encoded_name, sizeof(encoded_name), "%s.b64", filename);
    if (encoded_len < 0 || (size_t)encoded_len >= sizeof(encoded_name)) {
        return 0;
    }

    char encoded_path[PATH_MAX];
    if (!snapshot_fixture_path(encoded_path, sizeof(encoded_path), NULL, encoded_name)) {
        return 0;
    }

    if (!snapshot_fixture_file_exists(encoded_path)) {
        return 0;
    }

    return snapshot_fixture_decode_base64(encoded_path, output_path);
}

static void snapshot_fixture_fill_standard_header(uint8_t header[27]) {
    static const uint8_t template[27] = {
        0x3F,       // I
        0x22, 0x11, // HL'
        0x44, 0x33, // DE'
        0x66, 0x55, // BC'
        0x88, 0x77, // AF'
        0xAA, 0x99, // HL
        0xCC, 0xBB, // DE
        0xEE, 0xDD, // BC
        0x34, 0x12, // IY
        0x78, 0x56, // IX
        0x01,       // IFF2
        0xA5,       // R
        0xE1, 0xF0, // AF
        0xE0, 0xFF, // SP
        0x02,       // IM
        0x04        // Border colour
    };
    memcpy(header, template, sizeof(template));
}

static int snapshot_fixture_write_ram_block(FILE* out, uint8_t value, size_t size) {
    if (!out || size == 0u) {
        return 0;
    }

    uint8_t block[0x4000];
    size_t chunk = sizeof(block);
    while (size > 0u) {
        size_t to_write = (size < chunk) ? size : chunk;
        memset(block, value, to_write);
        if (fwrite(block, 1u, to_write, out) != to_write) {
            return 0;
        }
        size -= to_write;
    }
    return 1;
}

static int snapshot_fixture_generate_sna_48k(const char* output_path) {
    FILE* out = fopen(output_path, "wb");
    if (!out) {
        fprintf(stderr, "Failed to create 48K SNA fixture '%s': %s\n", output_path, strerror(errno));
        return 0;
    }

    uint8_t header[27];
    snapshot_fixture_fill_standard_header(header);
    int ok = (fwrite(header, 1u, sizeof(header), out) == sizeof(header));
    ok = ok && snapshot_fixture_write_ram_block(out, 0x10u, 0x4000u);
    ok = ok && snapshot_fixture_write_ram_block(out, 0x20u, 0x4000u);
    ok = ok && snapshot_fixture_write_ram_block(out, 0x30u, 0x4000u);
    uint8_t pc_bytes[2] = {0x00u, 0x40u};
    if (ok) {
        ok = (fwrite(pc_bytes, 1u, sizeof(pc_bytes), out) == sizeof(pc_bytes));
    }

    if (fclose(out) != 0) {
        ok = 0;
    }

    if (!ok) {
        fprintf(stderr, "Failed to synthesise 48K SNA fixture '%s'\n", output_path);
        remove(output_path);
    }

    return ok;
}

static int snapshot_fixture_generate_sna_128k_locked(const char* output_path) {
    FILE* out = fopen(output_path, "wb");
    if (!out) {
        fprintf(stderr, "Failed to create 128K SNA fixture '%s': %s\n", output_path, strerror(errno));
        return 0;
    }

    static const uint8_t bank_patterns[8] = {0x00u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u, 0x77u};

    uint8_t header[27];
    snapshot_fixture_fill_standard_header(header);
    int ok = (fwrite(header, 1u, sizeof(header), out) == sizeof(header));
    ok = ok && snapshot_fixture_write_ram_block(out, 0x55u, 0x4000u);
    ok = ok && snapshot_fixture_write_ram_block(out, 0x22u, 0x4000u);
    ok = ok && snapshot_fixture_write_ram_block(out, 0x55u, 0x4000u);
    uint8_t pc_bytes[2] = {0x00u, 0x40u};
    if (ok) {
        ok = (fwrite(pc_bytes, 1u, sizeof(pc_bytes), out) == sizeof(pc_bytes));
    }
    uint8_t ports[2] = {0x25u, 0x00u};
    if (ok) {
        ok = (fwrite(ports, 1u, sizeof(ports), out) == sizeof(ports));
    }
    for (int bank = 0; ok && bank < 8; ++bank) {
        ok = snapshot_fixture_write_ram_block(out, bank_patterns[bank], 0x4000u);
    }

    if (fclose(out) != 0) {
        ok = 0;
    }

    if (!ok) {
        fprintf(stderr, "Failed to synthesise 128K SNA fixture '%s'\n", output_path);
        remove(output_path);
    }

    return ok;
}

static int snapshot_fixture_generate_sna_plus2a_special(const char* output_path) {
    FILE* out = fopen(output_path, "wb");
    if (!out) {
        fprintf(stderr, "Failed to create +2A SNA fixture '%s': %s\n", output_path, strerror(errno));
        return 0;
    }

    static const uint8_t bank_patterns[8] = {0x00u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u, 0x77u};

    uint8_t header[27];
    snapshot_fixture_fill_standard_header(header);
    int ok = (fwrite(header, 1u, sizeof(header), out) == sizeof(header));
    ok = ok && snapshot_fixture_write_ram_block(out, 0xAAu, 0x4000u);
    ok = ok && snapshot_fixture_write_ram_block(out, 0xBBu, 0x4000u);
    ok = ok && snapshot_fixture_write_ram_block(out, 0xCCu, 0x4000u);
    uint8_t pc_bytes[2] = {0x00u, 0x40u};
    if (ok) {
        ok = (fwrite(pc_bytes, 1u, sizeof(pc_bytes), out) == sizeof(pc_bytes));
    }
    uint8_t ports[2] = {0x02u, 0x00u};
    if (ok) {
        ok = (fwrite(ports, 1u, sizeof(ports), out) == sizeof(ports));
    }
    for (int bank = 0; ok && bank < 8; ++bank) {
        ok = snapshot_fixture_write_ram_block(out, bank_patterns[bank], 0x4000u);
    }
    uint8_t port_1ffd = 0x05u;
    if (ok) {
        ok = (fwrite(&port_1ffd, 1u, 1u, out) == 1u);
    }

    if (fclose(out) != 0) {
        ok = 0;
    }

    if (!ok) {
        fprintf(stderr, "Failed to synthesise +2A SNA fixture '%s'\n", output_path);
        remove(output_path);
    }

    return ok;
}

static int snapshot_fixture_generate_sna_plus3_rompaging(const char* output_path) {
    FILE* out = fopen(output_path, "wb");
    if (!out) {
        fprintf(stderr, "Failed to create +3 SNA fixture '%s': %s\n", output_path, strerror(errno));
        return 0;
    }

    static const uint8_t bank_patterns[8] = {0x00u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u, 0x77u};

    uint8_t header[27];
    snapshot_fixture_fill_standard_header(header);
    int ok = (fwrite(header, 1u, sizeof(header), out) == sizeof(header));
    ok = ok && snapshot_fixture_write_ram_block(out, 0x90u, 0x4000u);
    ok = ok && snapshot_fixture_write_ram_block(out, 0xA0u, 0x4000u);
    ok = ok && snapshot_fixture_write_ram_block(out, 0xB0u, 0x4000u);
    uint8_t pc_bytes[2] = {0x00u, 0x40u};
    if (ok) {
        ok = (fwrite(pc_bytes, 1u, sizeof(pc_bytes), out) == sizeof(pc_bytes));
    }
    uint8_t ports[2] = {0x14u, 0x00u};
    if (ok) {
        ok = (fwrite(ports, 1u, sizeof(ports), out) == sizeof(ports));
    }
    for (int bank = 0; ok && bank < 8; ++bank) {
        ok = snapshot_fixture_write_ram_block(out, bank_patterns[bank], 0x4000u);
    }
    uint8_t port_1ffd = 0x01u;
    if (ok) {
        ok = (fwrite(&port_1ffd, 1u, 1u, out) == 1u);
    }

    if (fclose(out) != 0) {
        ok = 0;
    }

    if (!ok) {
        fprintf(stderr, "Failed to synthesise +3 SNA fixture '%s'\n", output_path);
        remove(output_path);
    }

    return ok;
}

static int snapshot_fixture_generate_sna(const char* filename, const char* output_path) {
    if (!filename || !output_path) {
        return 0;
    }

    if (strcmp(filename, "48k-basic.sna") == 0) {
        return snapshot_fixture_generate_sna_48k(output_path);
    }
    if (strcmp(filename, "128k-locked-bank5.sna") == 0) {
        return snapshot_fixture_generate_sna_128k_locked(output_path);
    }
    if (strcmp(filename, "plus2a-special.sna") == 0) {
        return snapshot_fixture_generate_sna_plus2a_special(output_path);
    }
    if (strcmp(filename, "plus3-rompaging.sna") == 0) {
        return snapshot_fixture_generate_sna_plus3_rompaging(output_path);
    }
    return 0;
}

static int snapshot_fixture_generate_default(char* buffer,
                                             size_t capacity,
                                             const char* filename) {
    if (!buffer || capacity == 0u || !filename) {
        return 0;
    }

    char generated_dir[PATH_MAX];
    if (!snapshot_fixture_path(generated_dir, sizeof(generated_dir), snapshot_fixture_default_dir, "generated")) {
        return 0;
    }

    if (!snapshot_fixture_ensure_directory(generated_dir)) {
        return 0;
    }

    if (!snapshot_fixture_path(buffer, capacity, generated_dir, filename)) {
        return 0;
    }

    if (snapshot_fixture_generate_sna(filename, buffer)) {
        return 1;
    }

    if (snapshot_fixture_generate_base64(filename, buffer)) {
        return 1;
    }

    fprintf(stderr, "Snapshot fixture '%s' is missing (no generator or Base64 source)\n", filename);
    remove(buffer);
    return 0;
}

static int snapshot_fixture_resolve(char* buffer,
                                    size_t capacity,
                                    const char* override_dir,
                                    const char* filename) {
    if (!snapshot_fixture_path(buffer, capacity, override_dir, filename)) {
        return 0;
    }

    if (snapshot_fixture_file_exists(buffer)) {
        return 1;
    }

    if (!snapshot_fixture_uses_default_dir(override_dir)) {
        fprintf(stderr, "Snapshot fixture '%s' not found in '%s'\n", filename, override_dir);
        return 0;
    }

    return snapshot_fixture_generate_default(buffer, capacity, filename);
}

static bool memory_block_matches(uint16_t base, uint8_t value) {
    static const uint16_t offsets[] = {0x0000u, 0x1FFFu, 0x3FFFu};
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        uint16_t addr = (uint16_t)(base + offsets[i]);
        if (memory[addr] != value) {
            return false;
        }
    }
    return true;
}

static bool test_snapshot_sna_48k(const char* override_dir) {
    char path[512];
    if (!snapshot_fixture_resolve(path, sizeof(path), override_dir, "48k-basic.sna")) {
        printf("    failed to prepare fixture 48k-basic.sna\n");
        return false;
    }

    Z80 cpu;
    cpu_reset_state(&cpu);
    spectrum_configure_model(SPECTRUM_MODEL_48K);
    memory_clear();
    border_color_idx = 0u;

    if (!snapshot_load(path, SNAPSHOT_FORMAT_SNA, &cpu)) {
        return false;
    }

    bool main_regs_ok = (cpu.reg_A == 0xF0u) && (cpu.reg_F == 0xE1u) &&
                        (cpu.reg_B == 0xDDu) && (cpu.reg_C == 0xEEu) &&
                        (cpu.reg_D == 0xBBu) && (cpu.reg_E == 0xCCu) &&
                        (cpu.reg_H == 0x99u) && (cpu.reg_L == 0xAAu);
    bool alt_regs_ok = (cpu.alt_reg_A == 0x77u) && (cpu.alt_reg_F == 0x88u) &&
                       (cpu.alt_reg_B == 0x55u) && (cpu.alt_reg_C == 0x66u) &&
                       (cpu.alt_reg_D == 0x33u) && (cpu.alt_reg_E == 0x44u) &&
                       (cpu.alt_reg_H == 0x11u) && (cpu.alt_reg_L == 0x22u);
    bool special_regs_ok = (cpu.reg_I == 0x3Fu) && (cpu.reg_R == 0xA5u) &&
                           (cpu.reg_IX == 0x5678u) && (cpu.reg_IY == 0x1234u) &&
                           (cpu.reg_SP == 0xFFE0u) && (cpu.reg_PC == 0x4000u);
    bool interrupt_state_ok = (cpu.iff1 == 1) && (cpu.iff2 == 1) && (cpu.interruptMode == 2);
    bool model_ok = (spectrum_model == SPECTRUM_MODEL_48K) &&
                    (current_paged_bank == 7u) &&
                    (current_rom_page == 0u) &&
                    (paging_disabled == 0);
    bool border_ok = (border_color_idx == 0x04u);
    bool memory_ok = memory_block_matches(0x4000u, 0x10u) &&
                     memory_block_matches(0x8000u, 0x20u) &&
                     memory_block_matches(0xC000u, 0x30u);

    if (!memory_ok) {
        printf("    48K SNA memory mismatch: %02X %02X %02X\n",
               memory[0x4000], memory[0x8000], memory[0xC000]);
    }
    if (!main_regs_ok || !alt_regs_ok || !special_regs_ok || !interrupt_state_ok || !model_ok || !border_ok) {
        printf("    48K SNA state debug: A=%02X F=%02X PC=%04X SP=%04X IFF1=%d IFF2=%d model=%s border=%u\n",
               cpu.reg_A,
               cpu.reg_F,
               cpu.reg_PC,
               cpu.reg_SP,
               cpu.iff1,
               cpu.iff2,
               spectrum_model_to_string(spectrum_model),
               (unsigned)border_color_idx);
    }

    return main_regs_ok && alt_regs_ok && special_regs_ok && interrupt_state_ok &&
           model_ok && border_ok && memory_ok;
}

static bool test_snapshot_sna_128k_locked(const char* override_dir) {
    char path[512];
    if (!snapshot_fixture_resolve(path, sizeof(path), override_dir, "128k-locked-bank5.sna")) {
        printf("    failed to prepare fixture 128k-locked-bank5.sna\n");
        return false;
    }

    Z80 cpu;
    cpu_reset_state(&cpu);
    spectrum_configure_model(SPECTRUM_MODEL_48K);
    memory_clear();
    border_color_idx = 0u;

    if (!snapshot_load(path, SNAPSHOT_FORMAT_SNA, &cpu)) {
        return false;
    }

    bool model_ok = (spectrum_model == SPECTRUM_MODEL_128K);
    bool paging_ok = (paging_disabled == 1) &&
                     (gate_array_7ffd_state == 0x25u) &&
                     (current_paged_bank == 5u) &&
                     (current_screen_bank == 5u) &&
                     (gate_array_1ffd_state == 0u);
    bool memory_ok = memory_block_matches(0x4000u, 0x55u) &&
                     memory_block_matches(0x8000u, 0x22u) &&
                     memory_block_matches(0xC000u, 0x55u);

    if (!memory_ok || !paging_ok || !model_ok) {
        printf("    128K SNA debug: model=%s paging=%d bank=%u screen=%u rom=%u 0x4000=%02X 0x8000=%02X 0xC000=%02X\n",
               spectrum_model_to_string(spectrum_model),
               paging_disabled,
               (unsigned)current_paged_bank,
               (unsigned)current_screen_bank,
               (unsigned)current_rom_page,
               memory[0x4000],
               memory[0x8000],
               memory[0xC000]);
    }

    return model_ok && paging_ok && memory_ok;
}

static bool test_snapshot_sna_plus2a_special(const char* override_dir) {
    char path[512];
    if (!snapshot_fixture_resolve(path, sizeof(path), override_dir, "plus2a-special.sna")) {
        printf("    failed to prepare fixture plus2a-special.sna\n");
        return false;
    }

    Z80 cpu;
    cpu_reset_state(&cpu);
    spectrum_configure_model(SPECTRUM_MODEL_48K);
    memory_clear();
    border_color_idx = 0u;

    if (!snapshot_load(path, SNAPSHOT_FORMAT_SNA, &cpu)) {
        return false;
    }

    bool model_ok = (spectrum_model == SPECTRUM_MODEL_PLUS2A);
    bool paging_ok = (gate_array_7ffd_state == 0x02u) &&
                     (gate_array_1ffd_state == 0x05u) &&
                     (paging_disabled == 0) &&
                     (current_paged_bank == 2u);
    bool mapping_ok = (spectrum_pages[0].type == MEMORY_PAGE_RAM && spectrum_pages[0].index == 4u) &&
                      (spectrum_pages[1].type == MEMORY_PAGE_RAM && spectrum_pages[1].index == 5u) &&
                      (spectrum_pages[2].type == MEMORY_PAGE_RAM && spectrum_pages[2].index == 6u) &&
                      (spectrum_pages[3].type == MEMORY_PAGE_RAM && spectrum_pages[3].index == 7u);
    bool memory_ok = memory_block_matches(0x0000u, 0x44u) &&
                     memory_block_matches(0x4000u, 0x55u) &&
                     memory_block_matches(0x8000u, 0x66u) &&
                     memory_block_matches(0xC000u, 0x77u);

    if (!memory_ok || !mapping_ok || !paging_ok || !model_ok) {
        printf("    +2A SNA debug: model=%s 7FFD=%02X 1FFD=%02X pages=%u/%u/%u/%u mem=%02X/%02X/%02X/%02X\n",
               spectrum_model_to_string(spectrum_model),
               (unsigned)gate_array_7ffd_state,
               (unsigned)gate_array_1ffd_state,
               spectrum_pages[0].index,
               spectrum_pages[1].index,
               spectrum_pages[2].index,
               spectrum_pages[3].index,
               memory[0x0000],
               memory[0x4000],
               memory[0x8000],
               memory[0xC000]);
    }

    return model_ok && paging_ok && mapping_ok && memory_ok;
}

static bool test_snapshot_sna_plus3_rom(const char* override_dir) {
    char path[512];
    if (!snapshot_fixture_resolve(path, sizeof(path), override_dir, "plus3-rompaging.sna")) {
        printf("    failed to prepare fixture plus3-rompaging.sna\n");
        return false;
    }

    Z80 cpu;
    cpu_reset_state(&cpu);
    spectrum_configure_model(SPECTRUM_MODEL_PLUS3);
    memory_clear();
    spectrum_configure_model(SPECTRUM_MODEL_PLUS3);
    border_color_idx = 0u;

    uint8_t previous_rom_count = rom_page_count;
    rom_page_count = 4u;

    spectrum_model = SPECTRUM_MODEL_PLUS3;
    bool loaded = snapshot_load(path, SNAPSHOT_FORMAT_SNA, &cpu);

    rom_page_count = previous_rom_count;

    if (!loaded) {
        return false;
    }

    bool model_ok = (spectrum_model == SPECTRUM_MODEL_PLUS3);
    bool rom_ok = (current_rom_page == 3u) && (spectrum_pages[0].type == MEMORY_PAGE_ROM);
    bool paging_ok = (paging_disabled == 0) &&
                     (current_paged_bank == 4u) &&
                     (gate_array_7ffd_state == 0x14u) &&
                     (gate_array_1ffd_state == 0x01u);
    bool screen_ok = (current_screen_bank == 5u);
    bool memory_ok = memory_block_matches(0x4000u, 0x55u) &&
                     memory_block_matches(0x8000u, 0x22u) &&
                     memory_block_matches(0xC000u, 0x44u);

    if (!memory_ok || !model_ok || !rom_ok || !paging_ok || !screen_ok) {
        printf("    +3 SNA debug: model=%s rom=%u bank=%u screen=%u 7FFD=%02X 1FFD=%02X mem=%02X/%02X/%02X\n",
               spectrum_model_to_string(spectrum_model),
               (unsigned)current_rom_page,
               (unsigned)current_paged_bank,
               (unsigned)current_screen_bank,
               (unsigned)gate_array_7ffd_state,
               (unsigned)gate_array_1ffd_state,
               memory[0x4000],
               memory[0x8000],
               memory[0xC000]);
    }

    return model_ok && rom_ok && paging_ok && screen_ok && memory_ok;
}

static bool test_snapshot_z80_v1_compressed(const char* override_dir) {
    char path[512];
    if (!snapshot_fixture_resolve(path, sizeof(path), override_dir, "v1-compressed.z80")) {
        printf("    failed to prepare fixture v1-compressed.z80\n");
        return false;
    }

    Z80 cpu;
    cpu_reset_state(&cpu);
    spectrum_configure_model(SPECTRUM_MODEL_48K);
    memory_clear();
    border_color_idx = 0u;

    if (!snapshot_load(path, SNAPSHOT_FORMAT_Z80, &cpu)) {
        return false;
    }

    bool registers_ok = (cpu.reg_A == 0x11u) && (cpu.reg_F == 0x22u) &&
                        (cpu.reg_B == 0x44u) && (cpu.reg_C == 0x33u) &&
                        (cpu.reg_D == 0x88u) && (cpu.reg_E == 0x77u) &&
                        (cpu.reg_H == 0x66u) && (cpu.reg_L == 0x55u);
    bool alt_ok = (cpu.alt_reg_A == 0xFFu) && (cpu.alt_reg_F == 0x00u) &&
                  (cpu.alt_reg_B == 0xAAu) && (cpu.alt_reg_C == 0x99u) &&
                  (cpu.alt_reg_D == 0xCCu) && (cpu.alt_reg_E == 0xBBu) &&
                  (cpu.alt_reg_H == 0xEEu) && (cpu.alt_reg_L == 0xDDu);
    bool special_ok = (cpu.reg_I == 0x99u) && (cpu.reg_R == 0xFFu) &&
                      (cpu.reg_IX == 0x2468u) && (cpu.reg_IY == 0x1357u) &&
                      (cpu.reg_SP == 0xD432u) && (cpu.reg_PC == 0x9ABCu);
    bool interrupt_ok = (cpu.iff1 == 1) && (cpu.iff2 == 0) && (cpu.interruptMode == 1);
    bool border_ok = (border_color_idx == 5u);
    bool memory_ok = memory_block_matches(0x4000u, 0x41u) &&
                     memory_block_matches(0x8000u, 0x52u) &&
                     memory_block_matches(0xC000u, 0x63u);

    if (!memory_ok || !registers_ok || !alt_ok || !special_ok || !interrupt_ok || !border_ok) {
        printf("    Z80 V1 debug: A=%02X F=%02X PC=%04X R=%02X border=%u mem=%02X/%02X/%02X\n",
               cpu.reg_A,
               cpu.reg_F,
               cpu.reg_PC,
               cpu.reg_R,
               (unsigned)border_color_idx,
               memory[0x4000],
               memory[0x8000],
               memory[0xC000]);
    }

    return registers_ok && alt_ok && special_ok && interrupt_ok && border_ok && memory_ok;
}

static bool test_snapshot_z80_v3_extended(const char* override_dir) {
    char path[512];
    if (!snapshot_fixture_resolve(path, sizeof(path), override_dir, "v3-128k.z80")) {
        printf("    failed to prepare fixture v3-128k.z80\n");
        return false;
    }

    Z80 cpu;
    cpu_reset_state(&cpu);
    spectrum_configure_model(SPECTRUM_MODEL_48K);
    memory_clear();
    border_color_idx = 0u;

    if (!snapshot_load(path, SNAPSHOT_FORMAT_Z80, &cpu)) {
        return false;
    }

    bool model_ok = (spectrum_model == SPECTRUM_MODEL_128K);
    bool paging_ok = (gate_array_7ffd_state == 0x13u) &&
                     (paging_disabled == 0) &&
                     (current_paged_bank == 3u) &&
                     (spectrum_pages[3].type == MEMORY_PAGE_RAM) &&
                     (spectrum_pages[3].index == 3u);
    bool registers_ok = (cpu.reg_A == 0xAAu) && (cpu.reg_F == 0x11u) &&
                        (cpu.reg_PC == 0x1234u) && (cpu.reg_SP == 0xBEEFu);
    bool refresh_ok = (cpu.reg_R == 0x83u) && (cpu.reg_I == 0xEDu);
    bool border_ok = (border_color_idx == 6u);
    bool interrupt_ok = (cpu.iff1 == 1) && (cpu.iff2 == 1) && (cpu.interruptMode == 2);
    bool memory_ok = memory_block_matches(0x4000u, 0x45u) &&
                     memory_block_matches(0x8000u, 0x42u) &&
                     memory_block_matches(0xC000u, 0x43u);

    if (!memory_ok || !registers_ok || !refresh_ok || !border_ok || !interrupt_ok || !paging_ok || !model_ok) {
        printf("    Z80 V3 debug: model=%s PC=%04X SP=%04X R=%02X border=%u mem=%02X/%02X/%02X bank=%u\n",
               spectrum_model_to_string(spectrum_model),
               cpu.reg_PC,
               cpu.reg_SP,
               cpu.reg_R,
               (unsigned)border_color_idx,
               memory[0x4000],
               memory[0x8000],
               memory[0xC000],
               (unsigned)current_paged_bank);
    }

    return model_ok && paging_ok && registers_ok && refresh_ok && border_ok && interrupt_ok && memory_ok;
}

static bool snapshot_probe_load_file(const char* path, SnapshotFormat format) {
    if (!path || format == SNAPSHOT_FORMAT_NONE) {
        return false;
    }

    Z80 cpu;
    cpu_reset_state(&cpu);
    spectrum_configure_model(SPECTRUM_MODEL_48K);
    memory_clear();
    border_color_idx = 0u;

    return snapshot_load(path, format, &cpu) ? true : false;
}

static bool run_snapshot_probes(const char* override_dir) {
    char probe_dir[PATH_MAX];
    if (!snapshot_fixture_path(probe_dir, sizeof(probe_dir), override_dir, "probes")) {
        return true;
    }

    DIR* dir = opendir(probe_dir);
    if (!dir) {
        printf("Snapshot compatibility probes directory %s not found (skipping)\n", probe_dir);
        return true;
    }

    printf("Running snapshot compatibility probes in %s...\n", probe_dir);

    bool any_files = false;
    bool all_passed = true;
    struct dirent* entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        SnapshotFormat format = snapshot_format_from_extension(entry->d_name);
        if (format == SNAPSHOT_FORMAT_NONE) {
            continue;
        }

        char full_path[PATH_MAX];
        int required = snprintf(full_path, sizeof(full_path), "%s/%s", probe_dir, entry->d_name);
        if (required < 0 || (size_t)required >= sizeof(full_path)) {
            printf("  %-28s SKIP (path too long)\n", entry->d_name);
            continue;
        }

        STAT_STRUCT info;
        if (STAT_FUNC(full_path, &info) != 0 || STAT_ISDIR(info.st_mode)) {
            continue;
        }

        any_files = true;
        bool ok = snapshot_probe_load_file(full_path, format);
        printf("  %-28s %s\n", entry->d_name, ok ? "PASS" : "FAIL");
        if (!ok) {
            all_passed = false;
        }
    }

    closedir(dir);

    if (!any_files) {
        printf("  (no .sna or .z80 files found in %s)\n", probe_dir);
    }

    return all_passed;
}

static bool run_snapshot_tests(const char* override_dir) {
    struct {
        const char* name;
        bool (*fn)(const char* dir);
    } tests[] = {
        {"SNA 48K register restore", test_snapshot_sna_48k},
        {"SNA 128K locked paging", test_snapshot_sna_128k_locked},
        {"SNA +2A special map", test_snapshot_sna_plus2a_special},
        {"SNA +3 ROM paging", test_snapshot_sna_plus3_rom},
        {"Z80 V1 compressed RAM", test_snapshot_z80_v1_compressed},
        {"Z80 V3 extended header", test_snapshot_z80_v3_extended},
    };

    printf("Running snapshot loader tests...\n");
    bool all_passed = true;
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        bool ok = tests[i].fn(override_dir);
        printf("  %-28s %s\n", tests[i].name, ok ? "PASS" : "FAIL");
        if (!ok) {
            all_passed = false;
        }
    }

    bool probes_ok = run_snapshot_probes(override_dir);
    if (!probes_ok) {
        all_passed = false;
    }

    return all_passed;
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
        {"INI flag behaviour", test_ini_flags},
        {"OUTD flag behaviour", test_outd_flags},
        {"INIR repeat timing", test_inir_repeat},
        {"OTDR repeat timing", test_otdr_repeat},
        {"IM 2 interrupt vector", test_interrupt_im2},
        {"IM 1 interrupt vector", test_interrupt_im1},
        {"NMI stack handling", test_nmi_stack_behaviour},
        {"Floating bus samples", test_floating_bus_samples_screen_memory},
        {"+2A contention profile", test_plus2a_contention_profile},
        {"+3 ROM/all-RAM paging", test_plus3_rom_and_all_ram_paging},
        {"+3 special paging", test_plus3_special_paging_modes},
        {"Peripheral contention", test_peripheral_port_contention},
        {"Late GA contention timing", test_plus3_contention_penalty_shift},
        {"+3 peripheral wait-states", test_plus3_peripheral_wait_states},
        {"128K bank paging", test_128k_bank_switching},
        {"128K contention penalties", test_128k_contention_penalty},
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

    total_t_states = 0;

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
        total_t_states += (uint64_t)t_states;
    }

    if (!terminated) {
        return 0;
    }

    if (success_marker) {
        if (!output || strstr(output, success_marker) == NULL) {
            return 0;
        }
    } else {
        if (output && (contains_case_insensitive(output, "fail") ||
                       contains_case_insensitive(output, "error"))) {
            return 0;
        }
    }

    return 1;
}

static void toggle_fullscreen(void) {
    if (!window) {
        return;
    }

    Uint32 flags = window_fullscreen ? 0u : SDL_WINDOW_FULLSCREEN_DESKTOP;
    if (SDL_SetWindowFullscreen(window, flags) != 0) {
        fprintf(stderr, "Failed to toggle fullscreen: %s\n", SDL_GetError());
        return;
    }

    window_fullscreen = window_fullscreen ? 0 : 1;
    SDL_ShowCursor(window_fullscreen ? SDL_DISABLE : SDL_ENABLE);
}

static int run_cpu_tests(const char* rom_dir, const char* snapshot_dir) {
    bool unit_pass = run_unit_tests();
    bool snapshot_pass = run_snapshot_tests(snapshot_dir);
    bool overall = unit_pass && snapshot_pass;

    const struct {
        const char* filename;
        const char* marker;
        const char* label;
    } optional_tests[] = {
        {"zexdoc.com", "ZEXDOC", "ZEXDOC"},
        {"zexall.com", "ZEXALL", "ZEXALL"},
        {"z80full.com", NULL, "Z80FULL"},
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
            "Usage: %s [--audio-dump <wav_file>] [--beeper-log] [--tape-debug] [--paging-log] [--paging-log-regs] [--ram-hash-log] "
            "[--model <48k|128k|plus2a|plus3> | --48k | --128k | --plus2a | --plus3] "
            "[--contention <48k|128k|plus2a|plus3>] "
            "[--peripheral <none|if1|plus3>] "
            "[--ay-gain <multiplier>] [--ay-pan <left,center,right>] "
            "[--tap <tap_file> | --tzx <tzx_file> | --wav <wav_file>] "
            "[--snapshot <sna_or_z80>] "
            "[--save-tap <tap_file> | --save-wav <wav_file>] "
            "[--test-rom-dir <dir>] [--snapshot-test-dir <dir>] "
            "[--run-tests] [--fullscreen] [rom_file]\n",
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
    wanted_spec.channels = 2;
    wanted_spec.samples = 512;
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = NULL;

    if (SDL_OpenAudio(&wanted_spec, &have_spec) < 0) {
        fprintf(stderr, "Failed to open audio: %s\n", SDL_GetError());
        ay_set_sample_rate(0);
        beeper_set_latency_limit(256.0);
    } else {
        if (have_spec.format != AUDIO_S16SYS || have_spec.channels < 1 || have_spec.channels > 2) {
            fprintf(stderr, "Unexpected audio format (format=%d, channels=%d). Audio disabled.\n", have_spec.format, have_spec.channels);
            SDL_CloseAudio();
            ay_set_sample_rate(0);
            beeper_set_latency_limit(256.0);
        } else {
            audio_sample_rate = have_spec.freq > 0 ? have_spec.freq : wanted_spec.freq;
            ay_set_sample_rate(audio_sample_rate);
            audio_channel_count = (have_spec.channels > 0) ? have_spec.channels : wanted_spec.channels;
            if (audio_channel_count <= 0) {
                audio_channel_count = 1;
            }
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
            beeper_reset_audio_state(total_t_states, speaker_output_level);
            if (audio_dump_path && !audio_dump_file) {
                if (!audio_dump_start(audio_dump_path,
                                      (uint32_t)audio_sample_rate,
                                      (uint16_t)audio_channel_count)) {
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
    ay_set_sample_rate(0);
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
    uint64_t frame_start = border_frame_start_tstate;
    uint64_t frame_end = frame_start + T_STATES_PER_FRAME;

    uint8_t start_color = border_frame_color & 0x07u;
    size_t drop_count = 0;
    while (drop_count < border_color_event_count && border_color_events[drop_count].t_state <= frame_start) {
        start_color = border_color_events[drop_count].color_idx & 0x07u;
        ++drop_count;
    }
    if (drop_count > 0) {
        size_t remaining = border_color_event_count - drop_count;
        if (remaining > 0) {
            memmove(&border_color_events[0], &border_color_events[drop_count], remaining * sizeof(BorderColorEvent));
        }
        border_color_event_count -= drop_count;
    }
    border_frame_color = start_color;

    uint32_t base_rgba = spectrum_colors[start_color];
    size_t total_pixels = (size_t)TOTAL_WIDTH * (size_t)TOTAL_HEIGHT;
    for (size_t i = 0; i < total_pixels; ++i) {
        pixels[i] = base_rgba;
    }

    uint64_t segment_start = frame_start;
    uint8_t current_color = start_color;
    size_t event_index = 0;
    while (event_index < border_color_event_count && border_color_events[event_index].t_state < frame_end) {
        uint64_t event_time = border_color_events[event_index].t_state;
        if (event_time > segment_start) {
            border_draw_span(segment_start, event_time, current_color);
        }
        current_color = border_color_events[event_index].color_idx & 0x07u;
        segment_start = event_time;
        ++event_index;
    }
    if (frame_end > segment_start) {
        border_draw_span(segment_start, frame_end, current_color);
    }

    if (event_index > 0) {
        size_t remaining = border_color_event_count - event_index;
        if (remaining > 0) {
            memmove(&border_color_events[0], &border_color_events[event_index], remaining * sizeof(BorderColorEvent));
        }
        border_color_event_count = remaining;
    }

    border_frame_start_tstate = frame_end;
    border_frame_color = current_color & 0x07u;

    uint64_t frame_count = total_t_states / T_STATES_PER_FRAME;
    int flash_phase = (int)((frame_count >> 5) & 1ULL);
    const uint8_t* vram_bank = memory + VRAM_START;
    const uint8_t* attr_bank = memory + ATTR_START;
    if (current_screen_bank < 8u) {
        if (spectrum_pages[1].type == MEMORY_PAGE_RAM && spectrum_pages[1].index == current_screen_bank) {
            vram_bank = memory + VRAM_START;
            attr_bank = memory + ATTR_START;
        } else {
            const uint8_t* bank = ram_pages[current_screen_bank];
            vram_bank = bank;
            attr_bank = bank + (ATTR_START - VRAM_START);
        }
    }
    for (int y = 0; y < SCREEN_HEIGHT; ++y) {
        for (int x_char = 0; x_char < SCREEN_WIDTH / 8; ++x_char) {
            uint16_t pix_addr = VRAM_START + ((y & 0xC0) << 5) + ((y & 7) << 8) + ((y & 0x38) << 2) + x_char;
            uint16_t attr_addr = ATTR_START + (y / 8 * 32) + x_char;
            uint16_t pix_offset = (uint16_t)(pix_addr - VRAM_START);
            uint16_t attr_offset = (uint16_t)(attr_addr - ATTR_START);
            uint8_t pix_byte = vram_bank[pix_offset];
            uint8_t attr_byte = attr_bank[attr_offset];
            int ink_idx = attr_byte & 7;
            int pap_idx = (attr_byte >> 3) & 7;
            int bright = (attr_byte >> 6) & 1;
            int flash = (attr_byte >> 7) & 1;
            const uint32_t* cmap = bright ? spectrum_bright_colors : spectrum_colors;
            uint32_t ink = cmap[ink_idx];
            uint32_t pap = cmap[pap_idx];
            if (flash && flash_phase) {
                uint32_t tmp = ink;
                ink = pap;
                pap = tmp;
            }
            for (int bit = 0; bit < 8; ++bit) {
                int sx = BORDER_SIZE + x_char * 8 + (7 - bit);
                int sy = BORDER_SIZE + y;
                pixels[sy * TOTAL_WIDTH + sx] = ((pix_byte >> bit) & 1) ? ink : pap;
            }
        }
    }
    tape_render_overlay();
    tape_render_manager();
    SDL_UpdateTexture(texture, NULL, pixels, TOTAL_WIDTH * sizeof(uint32_t));
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
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
    int channels = audio_channel_count > 0 ? audio_channel_count : 1;
    if (channels <= 0) {
        channels = 1;
    }
    int num_frames = (channels > 0) ? (num_samples / channels) : 0;
    double cycles_per_sample = beeper_cycles_per_sample;
    double playback_position = beeper_playback_position;
    double last_input = beeper_hp_last_input;
    double last_output = beeper_hp_last_output;
    int level = beeper_playback_level;

    if (cycles_per_sample <= 0.0 || num_frames <= 0) {
        memset(buffer, 0, (size_t)len);
        return;
    }

    if (beeper_event_head == beeper_event_tail && cycles_per_sample > 0.0) {
        double idle_cycles = playback_position - (double)beeper_last_event_t_state;
        if (idle_cycles > 0.0) {
            double idle_samples = idle_cycles / cycles_per_sample;
            if (idle_samples >= BEEPER_IDLE_RESET_SAMPLES) {
                memset(buffer, 0, (size_t)len);

                double new_position = playback_position + cycles_per_sample * (double)num_frames;
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

                double baseline = (double)level * (double)AUDIO_AMPLITUDE;
                last_input = baseline;
                last_output = 0.0;

                audio_dump_write_samples(buffer, (size_t)(num_frames * channels));

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

    for (int frame = 0; frame < num_frames; ++frame) {
        double target_position = playback_position + cycles_per_sample;

        while (beeper_event_head != beeper_event_tail &&
               (double)beeper_events[beeper_event_head].t_state <= target_position) {
            level = beeper_events[beeper_event_head].level;
            playback_position = (double)beeper_events[beeper_event_head].t_state;
            beeper_event_head = (beeper_event_head + 1) % BEEPER_EVENT_CAPACITY;
        }

        double raw_sample = (double)level * (double)AUDIO_AMPLITUDE;
        double filtered_sample = raw_sample - last_input + BEEPER_HP_ALPHA * last_output;
        last_input = raw_sample;
        last_output = filtered_sample;
        double ay_left = 0.0;
        double ay_right = 0.0;
        ay_mix_sample(ay_cycles_per_sample, &ay_left, &ay_right);
        double left_value = filtered_sample + ay_left;
        double right_value = filtered_sample + ay_right;
        double mono_value = filtered_sample + 0.5 * (ay_left + ay_right);

        double output_left = (channels > 1) ? left_value : mono_value;
        double output_right = (channels > 1) ? right_value : mono_value;

        if (output_left > 32767.0) {
            output_left = 32767.0;
        } else if (output_left < -32768.0) {
            output_left = -32768.0;
        }

        if (output_right > 32767.0) {
            output_right = 32767.0;
        } else if (output_right < -32768.0) {
            output_right = -32768.0;
        }

        Sint16* frame_ptr = &buffer[frame * channels];
        frame_ptr[0] = (Sint16)lrint(output_left);
        if (channels > 1) {
            frame_ptr[1] = (Sint16)lrint(output_right);
        }
        playback_position = target_position;
    }

    audio_dump_write_samples(buffer, (size_t)(num_frames * channels));

    beeper_playback_level = level;
    beeper_playback_position = playback_position;
    if (beeper_writer_cursor < playback_position) {
        beeper_writer_cursor = playback_position;
    }
    beeper_hp_last_input = last_input;
    beeper_hp_last_output = last_output;

    (void)userdata;
}

static void border_record_event(uint64_t event_t_state, uint8_t color_idx) {
    color_idx &= 0x07u;

    if (event_t_state < border_frame_start_tstate) {
        border_frame_color = color_idx;
        return;
    }

    if (border_color_event_count > 0) {
        BorderColorEvent* last = &border_color_events[border_color_event_count - 1];
        if (event_t_state < last->t_state) {
            event_t_state = last->t_state;
        }
        if (last->color_idx == color_idx) {
            return;
        }
    } else if (color_idx == border_frame_color) {
        return;
    }

    size_t capacity = BORDER_EVENT_CAPACITY;
    if (border_color_event_count >= capacity) {
        size_t drop = border_color_event_count - capacity + 1u;
        if (drop > border_color_event_count) {
            drop = border_color_event_count;
        }
        if (drop > 0) {
            size_t remaining = border_color_event_count - drop;
            if (remaining > 0) {
                memmove(&border_color_events[0], &border_color_events[drop], remaining * sizeof(BorderColorEvent));
            }
            border_color_event_count = remaining;
        }
    }

    border_color_events[border_color_event_count].t_state = event_t_state;
    border_color_events[border_color_event_count].color_idx = color_idx;
    ++border_color_event_count;
}

static void border_draw_span(uint64_t span_start, uint64_t span_end, uint8_t color_idx) {
    if (span_end <= span_start) {
        return;
    }

    uint64_t frame_start = border_frame_start_tstate;
    if (span_end <= frame_start) {
        return;
    }

    uint64_t frame_end = frame_start + T_STATES_PER_FRAME;
    if (span_start < frame_start) {
        span_start = frame_start;
    }
    if (span_end > frame_end) {
        span_end = frame_end;
    }
    if (span_end <= span_start) {
        return;
    }

    uint32_t rgba = spectrum_colors[color_idx & 0x07u];

    uint64_t offset_start = span_start - frame_start;
    uint64_t offset_end = span_end - frame_start;

    uint64_t line_start = offset_start / ULA_T_STATES_PER_LINE;
    uint64_t line_end = (offset_end + (ULA_T_STATES_PER_LINE - 1u)) / ULA_T_STATES_PER_LINE;
    if (line_end > ULA_LINES_PER_FRAME) {
        line_end = ULA_LINES_PER_FRAME;
    }

    for (uint64_t line = line_start; line < line_end; ++line) {
        if (line < ULA_VISIBLE_TOP_LINES) {
            continue;
        }
        if (line >= ULA_LINES_PER_FRAME - ULA_VISIBLE_BOTTOM_LINES) {
            continue;
        }

        int y = (int)(line - ULA_VISIBLE_TOP_LINES);
        if (y < 0 || y >= TOTAL_HEIGHT) {
            continue;
        }

        uint64_t line_tstate_start = line * ULA_T_STATES_PER_LINE;
        uint64_t line_tstate_end = line_tstate_start + ULA_T_STATES_PER_LINE;

        uint64_t segment_start = offset_start > line_tstate_start ? offset_start : line_tstate_start;
        uint64_t segment_end = offset_end < line_tstate_end ? offset_end : line_tstate_end;
        if (segment_end <= segment_start) {
            continue;
        }

        uint64_t rel_start = segment_start - line_tstate_start;
        uint64_t rel_end = segment_end - line_tstate_start;

        if (rel_start >= ULA_LINE_VISIBLE_TSTATES) {
            continue;
        }
        if (rel_end > ULA_LINE_VISIBLE_TSTATES) {
            rel_end = ULA_LINE_VISIBLE_TSTATES;
        }

        int col_start = (int)(rel_start * 2u);
        int col_end = (int)(rel_end * 2u);

        if (col_start < 0) {
            col_start = 0;
        }
        if (col_end > TOTAL_WIDTH) {
            col_end = TOTAL_WIDTH;
        }
        if (col_start >= col_end) {
            continue;
        }

        uint32_t* row = &pixels[y * TOTAL_WIDTH];
        if (y < BORDER_SIZE || y >= BORDER_SIZE + SCREEN_HEIGHT) {
            for (int x = col_start; x < col_end; ++x) {
                row[x] = rgba;
            }
        } else {
            int left_limit = BORDER_SIZE;
            if (col_start < left_limit) {
                int left_start = col_start;
                int left_end = col_end < left_limit ? col_end : left_limit;
                for (int x = left_start; x < left_end; ++x) {
                    row[x] = rgba;
                }
            }

            int right_limit = TOTAL_WIDTH - BORDER_SIZE;
            if (col_end > right_limit) {
                int right_start = col_start > right_limit ? col_start : right_limit;
                if (right_start < col_end) {
                    for (int x = right_start; x < col_end; ++x) {
                        row[x] = rgba;
                    }
                }
            }
        }
    }
}

// --- Main Program ---
int main(int argc, char *argv[]) {
    const char* rom_filename = NULL;
    int rom_provided = 0;
    int run_tests = 0;
    const char* test_rom_dir = "tests/roms";
    const char* snapshot_test_dir = "tests/snapshots";
    int launch_fullscreen = 0;
    Z80 cpu;
    memset(&cpu, 0, sizeof(cpu));
    paging_cpu_state = &cpu;

    spectrum_init_log_output();

    tape_set_input_path(NULL);

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--audio-dump") == 0) {
            if (i + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            audio_dump_path = argv[++i];
        } else if (strcmp(argv[i], "--model") == 0) {
            if (i + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            const char* model_value = argv[++i];
            if (string_equals_case_insensitive(model_value, "48k")) {
                spectrum_configure_model(SPECTRUM_MODEL_48K);
            } else if (string_equals_case_insensitive(model_value, "128k")) {
                spectrum_configure_model(SPECTRUM_MODEL_128K);
            } else if (string_equals_case_insensitive(model_value, "plus2a") ||
                       string_equals_case_insensitive(model_value, "+2a")) {
                spectrum_configure_model(SPECTRUM_MODEL_PLUS2A);
            } else if (string_equals_case_insensitive(model_value, "plus3") ||
                       string_equals_case_insensitive(model_value, "+3")) {
                spectrum_configure_model(SPECTRUM_MODEL_PLUS3);
            } else {
                fprintf(stderr, "Unknown model: %s\n", model_value);
                return 1;
            }
        } else if (strcmp(argv[i], "--48k") == 0) {
            spectrum_configure_model(SPECTRUM_MODEL_48K);
        } else if (strcmp(argv[i], "--128k") == 0) {
            spectrum_configure_model(SPECTRUM_MODEL_128K);
        } else if (strcmp(argv[i], "--plus2a") == 0) {
            spectrum_configure_model(SPECTRUM_MODEL_PLUS2A);
        } else if (strcmp(argv[i], "--plus3") == 0) {
            spectrum_configure_model(SPECTRUM_MODEL_PLUS3);
        } else if (strcmp(argv[i], "--contention") == 0) {
            if (i + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            const char* profile_value = argv[++i];
            if (string_equals_case_insensitive(profile_value, "48k")) {
                spectrum_set_contention_profile(CONTENTION_PROFILE_48K);
            } else if (string_equals_case_insensitive(profile_value, "128k")) {
                spectrum_set_contention_profile(CONTENTION_PROFILE_128K);
            } else if (string_equals_case_insensitive(profile_value, "plus2a") ||
                       string_equals_case_insensitive(profile_value, "+2a")) {
                spectrum_set_contention_profile(CONTENTION_PROFILE_128K_PLUS2A);
            } else if (string_equals_case_insensitive(profile_value, "plus3") ||
                       string_equals_case_insensitive(profile_value, "+3")) {
                spectrum_set_contention_profile(CONTENTION_PROFILE_128K_PLUS3);
            } else {
                fprintf(stderr, "Unknown contention profile: %s\n", profile_value);
                return 1;
            }
        } else if (strcmp(argv[i], "--peripheral") == 0) {
            if (i + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            const char* peripheral_value = argv[++i];
            if (string_equals_case_insensitive(peripheral_value, "none")) {
                spectrum_set_peripheral_contention_profile(PERIPHERAL_CONTENTION_NONE);
            } else if (string_equals_case_insensitive(peripheral_value, "if1") ||
                       string_equals_case_insensitive(peripheral_value, "interface1")) {
                spectrum_set_peripheral_contention_profile(PERIPHERAL_CONTENTION_IF1);
            } else if (string_equals_case_insensitive(peripheral_value, "plus3") ||
                       string_equals_case_insensitive(peripheral_value, "+3")) {
                spectrum_set_peripheral_contention_profile(PERIPHERAL_CONTENTION_PLUS3);
            } else {
                fprintf(stderr, "Unknown peripheral contention profile: %s\n", peripheral_value);
                return 1;
            }
        } else if (strcmp(argv[i], "--ay-gain") == 0) {
            if (i + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            ay_global_gain = strtod(argv[++i], NULL);
            if (ay_global_gain < 0.0) {
                ay_global_gain = 0.0;
            }
        } else if (strcmp(argv[i], "--ay-pan") == 0) {
            if (i + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            if (!ay_parse_pan_spec(argv[++i])) {
                fprintf(stderr, "Invalid AY pan specification. Use three comma-separated values within [-1,1].\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--beeper-log") == 0) {
            beeper_logging_enabled = 1;
        } else if (strcmp(argv[i], "--tape-debug") == 0) {
            tape_debug_logging = 1;
            tape_log("Tape debug logging enabled\n");
        } else if (strcmp(argv[i], "--paging-log") == 0) {
            paging_debug_logging = 1;
            spectrum_log_paging_state("paging debug enabled", 0u, 0u, total_t_states);
        } else if (strcmp(argv[i], "--paging-log-regs") == 0) {
            paging_debug_logging = 1;
            paging_log_registers = 1;
            spectrum_log_paging_state("paging register logging enabled", 0u, 0u, total_t_states);
        } else if (strcmp(argv[i], "--ram-hash-log") == 0) {
            ram_hash_logging = 1;
            spectrum_log_ram_hashes("startup");
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
            tape_set_input_path(argv[++i]);
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
            tape_set_input_path(argv[++i]);
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
            tape_set_input_path(argv[++i]);
        } else if (strcmp(argv[i], "--snapshot") == 0) {
            if (i + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            if (snapshot_input_format != SNAPSHOT_FORMAT_NONE) {
                fprintf(stderr, "Only one snapshot image may be specified\n");
                return 1;
            }
            snapshot_input_path = argv[++i];
            snapshot_input_format = snapshot_format_from_extension(snapshot_input_path);
            if (snapshot_input_format == SNAPSHOT_FORMAT_NONE) {
                fprintf(stderr, "Unsupported snapshot type for '%s'\n", snapshot_input_path);
                return 1;
            }
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
        } else if (strcmp(argv[i], "--snapshot-test-dir") == 0) {
            if (i + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            snapshot_test_dir = argv[++i];
        } else if (strcmp(argv[i], "--run-tests") == 0) {
            run_tests = 1;
        } else if (strcmp(argv[i], "--fullscreen") == 0) {
            launch_fullscreen = 1;
        } else {
            SnapshotFormat inferred_snapshot = snapshot_format_from_extension(argv[i]);
            TapeFormat inferred_format = tape_format_from_extension(argv[i]);
            if (inferred_snapshot != SNAPSHOT_FORMAT_NONE && snapshot_input_format == SNAPSHOT_FORMAT_NONE) {
                snapshot_input_format = inferred_snapshot;
                snapshot_input_path = argv[i];
            } else if (inferred_format != TAPE_FORMAT_NONE && tape_input_format == TAPE_FORMAT_NONE) {
                tape_input_format = inferred_format;
                tape_set_input_path(argv[i]);
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
        return run_cpu_tests(test_rom_dir, snapshot_test_dir);
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
    uint8_t rom_buffer[0x10000];
    size_t rom_read = fread(rom_buffer, 1, sizeof(rom_buffer), rf);
    fclose(rf);
    if (rom_read < 0x4000) {
        fprintf(stderr, "ROM read error (%zu bytes)\n", rom_read);
        free(rom_fallback_path);
        return 1;
    }

    memset(memory, 0, sizeof(memory));
    memset(ram_pages, 0, sizeof(ram_pages));

    if (!spectrum_populate_rom_pages(rom_log_path, rom_buffer, rom_read)) {
        free(rom_fallback_path);
        return 1;
    }

    spectrum_configure_model(spectrum_model);
    spectrum_map_rom_page(0u);

    if (snapshot_input_format != SNAPSHOT_FORMAT_NONE && snapshot_input_path) {
        if (!snapshot_load(snapshot_input_path, snapshot_input_format, &cpu)) {
            free(rom_fallback_path);
            return 1;
        }
        printf("Loaded snapshot %s\n", snapshot_input_path);
    }

    printf("Loaded %zu bytes from %s (%u ROM banks)\n",
           rom_read,
           rom_log_path,
           (unsigned int)rom_page_count);
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

    speaker_output_level = speaker_calculate_output_level();

    if(!init_sdl()){tape_shutdown();cleanup_sdl();return 1;}

    if (launch_fullscreen && !window_fullscreen) {
        toggle_fullscreen();
    }

    if (snapshot_input_format == SNAPSHOT_FORMAT_NONE) {
        cpu.reg_PC = 0x0000;
        cpu.reg_SP = 0xFFFF;
        cpu.iff1 = 0;
        cpu.iff2 = 0;
        cpu.interruptMode = 1;
    }
    cpu.halted = 0;
    cpu.ei_delay = 0;
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

    printf("Tape manager: Press Tab to open popup controls\n");

    if (audio_available) {
        SDL_LockAudio();
        beeper_reset_audio_state(total_t_states, speaker_output_level);
        SDL_UnlockAudio();
    } else {
        beeper_reset_audio_state(total_t_states, speaker_output_level);
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
            } else if (e.type == SDL_TEXTINPUT) {
                if (tape_manager_handle_text_input(&e)) {
                    continue;
                }
            } else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                    SDL_Keycode sym = e.key.keysym.sym;
                    SDL_Keymod mods = e.key.keysym.mod;
                    if (sym == SDLK_ESCAPE) {
                        quit = 1;
                        continue;
                    }
                    if (sym == SDLK_F11 || (sym == SDLK_RETURN && (mods & KMOD_ALT))) {
                        toggle_fullscreen();
                        continue;
                    }
                }
                if (tape_handle_control_key(&e)) {
                    continue;
                }
                if (tape_manager_handle_event(&e)) {
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

    spectrum_log_ram_hashes("exit");
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
        border_record_event(event_t_state, (uint8_t)(border_color_idx & 0x07));

        int new_beeper_state = (value >> 4) & 0x01;
        if (new_beeper_state != beeper_state) {
            beeper_state = new_beeper_state;
            speaker_update_output(event_t_state, 1);
        }

        int mic_level = (value >> 3) & 0x01;
        tape_recorder_handle_mic(event_t_state, mic_level);
    }

    ula_write_count = 0;
}
