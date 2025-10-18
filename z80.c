#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <SDL.h>
#include <math.h> // For fmod in audio callback

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
#define VRAM_START 0x4000
#define ATTR_START 0x5800
#define T_STATES_PER_FRAME 69888 // 3.5MHz / 50Hz (Spectrum CPU speed)

// --- SDL Globals ---
SDL_Window* window = NULL;
SDL_Renderer* renderer = NULL;
SDL_Texture* texture = NULL;
uint32_t pixels[ TOTAL_WIDTH * TOTAL_HEIGHT ];

// --- Audio Globals ---
volatile int beeper_state = 0; // 0 = off, 1 = on
const int AUDIO_AMPLITUDE = 2000;
volatile double beeper_frequency = 440.0; // The frequency the Z80 is *trying* to play
volatile double audio_sample_index = 0.0; // Tracks our position in the generated wave

// --- Timing Globals ---
uint64_t total_t_states = 0; // A global clock for the entire CPU
uint32_t last_beeper_toggle_t_states = 0; // T-state time of the last beeper toggle

// --- ZX Spectrum Colours ---
const uint32_t spectrum_colors[8] = {0x000000FF,0x0000CDFF,0xCD0000FF,0xCD00CDFF,0x00CD00FF,0x00CDCDFF,0xCDCD00FF,0xCFCFCFFF};
const uint32_t spectrum_bright_colors[8] = {0x000000FF,0x0000FFFF,0xFF0000FF,0xFF00FFFF,0x00FF00FF,0x00FFFFFF,0xFFFF00FF,0xFFFFFFFF};

// --- Keyboard State ---
uint8_t keyboard_matrix[8] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

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
int cpu_interrupt(Z80* cpu, uint16_t vector_addr);
void audio_callback(void* userdata, Uint8* stream, int len);


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
void io_write(uint16_t port, uint8_t value) {
    if ((port & 1) == 0) { // ULA Port FE
        border_color_idx = value & 0x07;
        
        int new_beeper_state = (value >> 4) & 0x01;
        
        // Check if the beeper bit has *toggled*
        if (new_beeper_state != beeper_state) {
            uint32_t half_period_t_states = total_t_states - last_beeper_toggle_t_states;
            
            if (half_period_t_states > 20) { // Avoid division by zero
                uint32_t full_period_t_states = half_period_t_states * 2;
                beeper_frequency = 3500000.0 / full_period_t_states;
            }
            last_beeper_toggle_t_states = total_t_states;
            beeper_state = new_beeper_state;
        }
    }
    (void)port; (void)value;
}
uint8_t io_read(uint16_t port) {
    if ((port & 1) == 0) {
        uint8_t result = 0xFF;
        uint8_t high_byte = (port >> 8) & 0xFF;
        for (int row = 0; row < 8; ++row) { if (! (high_byte & (1 << row)) ) { result &= keyboard_matrix[row]; } }
        result |= 0xA0; // Set tape/unused bits high
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
static inline void set_flags_szp(Z80* cpu,uint8_t r){set_flag(cpu,FLAG_S,r&0x80);set_flag(cpu,FLAG_Z,r==0);uint8_t p=0;uint8_t t=r;for(int i=0;i<8;i++){if(t&1)p=!p;t>>=1;}set_flag(cpu,FLAG_PV,!p);}

// --- 8-Bit Arithmetic/Logic Helper Functions ---
void cpu_add(Z80* cpu,uint8_t v){uint16_t r=cpu->reg_A+v;uint8_t hc=((cpu->reg_A&0x0F)+(v&0x0F))>0x0F;set_flag(cpu,FLAG_S,r&0x80);set_flag(cpu,FLAG_Z,(r&0xFF)==0);set_flag(cpu,FLAG_H,hc);set_flag(cpu,FLAG_PV,((cpu->reg_A^v^0x80)&(r^v)&0x80)!=0);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,r>0xFF);cpu->reg_A=r&0xFF;}
void cpu_adc(Z80* cpu,uint8_t v){uint8_t c=get_flag(cpu,FLAG_C);uint16_t r=cpu->reg_A+v+c;uint8_t hc=((cpu->reg_A&0x0F)+(v&0x0F)+c)>0x0F;set_flag(cpu,FLAG_S,r&0x80);set_flag(cpu,FLAG_Z,(r&0xFF)==0);set_flag(cpu,FLAG_H,hc);set_flag(cpu,FLAG_PV,((cpu->reg_A^v^0x80)&(r^v)&0x80)!=0);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,r>0xFF);cpu->reg_A=r&0xFF;}
void cpu_sub(Z80* cpu,uint8_t v,int s){uint16_t r=cpu->reg_A-v;uint8_t hb=((cpu->reg_A&0x0F)<(v&0x0F));set_flag(cpu,FLAG_S,r&0x80);set_flag(cpu,FLAG_Z,(r&0xFF)==0);set_flag(cpu,FLAG_H,hb);set_flag(cpu,FLAG_PV,((cpu->reg_A^v)&(cpu->reg_A^r)&0x80)!=0);set_flag(cpu,FLAG_N,1);set_flag(cpu,FLAG_C,r>0xFF);if(s)cpu->reg_A=r&0xFF;}
void cpu_sbc(Z80* cpu,uint8_t v){uint8_t c=get_flag(cpu,FLAG_C);uint16_t r=cpu->reg_A-v-c;uint8_t hb=((cpu->reg_A&0x0F)<((v&0x0F)+c));set_flag(cpu,FLAG_S,r&0x80);set_flag(cpu,FLAG_Z,(r&0xFF)==0);set_flag(cpu,FLAG_H,hb);set_flag(cpu,FLAG_PV,((cpu->reg_A^v)&(cpu->reg_A^r)&0x80)!=0);set_flag(cpu,FLAG_N,1);set_flag(cpu,FLAG_C,r>0xFF);cpu->reg_A=r&0xFF;}
void cpu_and(Z80* cpu,uint8_t v){cpu->reg_A&=v;set_flags_szp(cpu,cpu->reg_A);set_flag(cpu,FLAG_H,1);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,0);}
void cpu_or(Z80* cpu,uint8_t v){cpu->reg_A|=v;set_flags_szp(cpu,cpu->reg_A);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,0);}
void cpu_xor(Z80* cpu,uint8_t v){cpu->reg_A^=v;set_flags_szp(cpu,cpu->reg_A);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,0);}
uint8_t cpu_inc(Z80* cpu,uint8_t v){uint8_t r=v+1;set_flag(cpu,FLAG_S,r&0x80);set_flag(cpu,FLAG_Z,r==0);set_flag(cpu,FLAG_H,(v&0x0F)==0x0F);set_flag(cpu,FLAG_PV,v==0x7F);set_flag(cpu,FLAG_N,0);return r;}
uint8_t cpu_dec(Z80* cpu,uint8_t v){uint8_t r=v-1;set_flag(cpu,FLAG_S,r&0x80);set_flag(cpu,FLAG_Z,r==0);set_flag(cpu,FLAG_H,(v&0x0F)==0x00);set_flag(cpu,FLAG_PV,v==0x80);set_flag(cpu,FLAG_N,1);return r;}
void cpu_add_hl(Z80* cpu,uint16_t v){uint16_t hl=get_HL(cpu);uint32_t r=hl+v;set_flag(cpu,FLAG_H,((hl&0x0FFF)+(v&0x0FFF))>0x0FFF);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,r>0xFFFF);set_HL(cpu,r&0xFFFF);}
void cpu_add_ixiy(Z80* cpu,uint16_t* rr,uint16_t v){uint16_t ixy=*rr;uint32_t r=ixy+v;set_flag(cpu,FLAG_H,((ixy&0x0FFF)+(v&0x0FFF))>0x0FFF);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,r>0xFFFF);*rr=r&0xFFFF;}
void cpu_adc_hl(Z80* cpu,uint16_t v){uint16_t hl=get_HL(cpu);uint8_t c=get_flag(cpu,FLAG_C);uint32_t r=hl+v+c;set_flag(cpu,FLAG_S,(r&0x8000)!=0);set_flag(cpu,FLAG_Z,(r&0xFFFF)==0);set_flag(cpu,FLAG_H,((hl&0x0FFF)+(v&0x0FFF)+c)>0x0FFF);set_flag(cpu,FLAG_PV,(((hl^v^0x8000)&(r^v)&0x8000))!=0);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,r>0xFFFF);set_HL(cpu,r&0xFFFF);}
void cpu_sbc_hl(Z80* cpu,uint16_t v){uint16_t hl=get_HL(cpu);uint8_t c=get_flag(cpu,FLAG_C);uint32_t r=hl-v-c;set_flag(cpu,FLAG_S,(r&0x8000)!=0);set_flag(cpu,FLAG_Z,(r&0xFFFF)==0);set_flag(cpu,FLAG_H,((hl&0x0FFF)<((v&0x0FFF)+c)));set_flag(cpu,FLAG_PV,((hl^v)&(hl^(uint16_t)r)&0x8000)!=0);set_flag(cpu,FLAG_N,1);set_flag(cpu,FLAG_C,r>0xFFFF);set_HL(cpu,r&0xFFFF);}
void cpu_push(Z80* cpu,uint16_t v){cpu->reg_SP--;writeByte(cpu->reg_SP,(v>>8)&0xFF);cpu->reg_SP--;writeByte(cpu->reg_SP,v&0xFF);}
uint16_t cpu_pop(Z80* cpu){uint8_t lo=readByte(cpu->reg_SP);cpu->reg_SP++;uint8_t hi=readByte(cpu->reg_SP);cpu->reg_SP++;return(hi<<8)|lo;}
uint8_t cpu_rlc(Z80* cpu,uint8_t v){uint8_t c=(v&0x80)?1:0;uint8_t r=(v<<1)|c;set_flags_szp(cpu,r);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,c);return r;}
uint8_t cpu_rrc(Z80* cpu,uint8_t v){uint8_t c=(v&0x01);uint8_t r=(v>>1)|(c<<7);set_flags_szp(cpu,r);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,c);return r;}
uint8_t cpu_rl(Z80* cpu,uint8_t v){uint8_t oc=get_flag(cpu,FLAG_C);uint8_t nc=(v&0x80)?1:0;uint8_t r=(v<<1)|oc;set_flags_szp(cpu,r);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,nc);return r;}
uint8_t cpu_rr(Z80* cpu,uint8_t v){uint8_t oc=get_flag(cpu,FLAG_C);uint8_t nc=(v&0x01);uint8_t r=(v>>1)|(oc<<7);set_flags_szp(cpu,r);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,nc);return r;}
uint8_t cpu_sla(Z80* cpu,uint8_t v){uint8_t c=(v&0x80)?1:0;uint8_t r=(v<<1);set_flags_szp(cpu,r);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,c);return r;}
uint8_t cpu_sra(Z80* cpu,uint8_t v){uint8_t c=(v&0x01);uint8_t r=(v>>1)|(v&0x80);set_flags_szp(cpu,r);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,c);return r;}
uint8_t cpu_srl(Z80* cpu,uint8_t v){uint8_t c=(v&0x01);uint8_t r=(v>>1);set_flags_szp(cpu,r);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_C,c);return r;}
void cpu_bit(Z80* cpu,uint8_t v,int b){uint8_t m=(1<<b);set_flag(cpu,FLAG_Z,(v&m)==0);set_flag(cpu,FLAG_PV,(v&m)==0);set_flag(cpu,FLAG_H,1);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_S,(b==7)&&(v&0x80));}

// --- 0xCB Prefix CPU Step Function ---
int cpu_cb_step(Z80* cpu) {
    uint8_t op=readByte(cpu->reg_PC++);uint8_t x=(op>>6)&3;uint8_t y=(op>>3)&7;uint8_t z=op&7;
    uint8_t operand;uint16_t hl_addr=0;int is_hl=(z==6);
    if(is_hl)hl_addr=get_HL(cpu);
    switch(z){case 0:operand=cpu->reg_B;break;case 1:operand=cpu->reg_C;break;case 2:operand=cpu->reg_D;break;case 3:operand=cpu->reg_E;break;case 4:operand=cpu->reg_H;break;case 5:operand=cpu->reg_L;break;case 6:operand=readByte(hl_addr);break;case 7:operand=cpu->reg_A;break;default:operand=0;}
    uint8_t result=0;
    switch(x){case 0:switch(y){case 0:result=cpu_rlc(cpu,operand);break;case 1:result=cpu_rrc(cpu,operand);break;case 2:result=cpu_rl(cpu,operand);break;case 3:result=cpu_rr(cpu,operand);break;case 4:result=cpu_sla(cpu,operand);break;case 5:result=cpu_sra(cpu,operand);break;case 6:result=0;/*SLL*/break;case 7:result=cpu_srl(cpu,operand);break;}break;
               case 1:cpu_bit(cpu,operand,y);return is_hl ? 4 : 0; // 12T/8T total (base 8)
               case 2:result=operand&~(1<<y);break;case 3:result=operand|(1<<y);break;}
    switch(z){case 0:cpu->reg_B=result;break;case 1:cpu->reg_C=result;break;case 2:cpu->reg_D=result;break;case 3:cpu->reg_E=result;break;case 4:cpu->reg_H=result;break;case 5:cpu->reg_L=result;break;case 6:writeByte(hl_addr,result);break;case 7:cpu->reg_A=result;break;}
    return is_hl ? 7 : 0; // 15T/8T total
}

// --- 0xED Prefix CPU Step Function ---
int cpu_ed_step(Z80* cpu) {
    uint8_t op=readByte(cpu->reg_PC++);
    switch(op){
        case 0x4A:cpu_adc_hl(cpu,get_BC(cpu));return 7; case 0x5A:cpu_adc_hl(cpu,get_DE(cpu));return 7; case 0x6A:cpu_adc_hl(cpu,get_HL(cpu));return 7; case 0x7A:cpu_adc_hl(cpu,cpu->reg_SP);return 7;
        case 0x42:cpu_sbc_hl(cpu,get_BC(cpu));return 7; case 0x52:cpu_sbc_hl(cpu,get_DE(cpu));return 7; case 0x62:cpu_sbc_hl(cpu,get_HL(cpu));return 7; case 0x72:cpu_sbc_hl(cpu,cpu->reg_SP);return 7;
        case 0x43:writeWord(readWord(cpu->reg_PC),get_BC(cpu));cpu->reg_PC+=2;return 12; case 0x53:writeWord(readWord(cpu->reg_PC),get_DE(cpu));cpu->reg_PC+=2;return 12;
        case 0x63:writeWord(readWord(cpu->reg_PC),get_HL(cpu));cpu->reg_PC+=2;return 12; case 0x73:writeWord(readWord(cpu->reg_PC),cpu->reg_SP);cpu->reg_PC+=2;return 12;
        case 0x4B:set_BC(cpu,readWord(readWord(cpu->reg_PC)));cpu->reg_PC+=2;return 12; case 0x5B:set_DE(cpu,readWord(readWord(cpu->reg_PC)));cpu->reg_PC+=2;return 12;
        case 0x6B:set_HL(cpu,readWord(readWord(cpu->reg_PC)));cpu->reg_PC+=2;return 12; case 0x7B:cpu->reg_SP=readWord(readWord(cpu->reg_PC));cpu->reg_PC+=2;return 12;
        case 0xA0:{uint8_t v=readByte(get_HL(cpu));writeByte(get_DE(cpu),v);set_DE(cpu,get_DE(cpu)+1);set_HL(cpu,get_HL(cpu)+1);set_BC(cpu,get_BC(cpu)-1);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_PV,get_BC(cpu)!=0);return 8;}
        case 0xB0:{uint8_t v=readByte(get_HL(cpu));writeByte(get_DE(cpu),v);set_DE(cpu,get_DE(cpu)+1);set_HL(cpu,get_HL(cpu)+1);set_BC(cpu,get_BC(cpu)-1);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);if(get_BC(cpu)!=0){cpu->reg_PC-=2;set_flag(cpu,FLAG_PV,1);return 13;}else{set_flag(cpu,FLAG_PV,0);return 8;}}
        case 0xA8:{uint8_t v=readByte(get_HL(cpu));writeByte(get_DE(cpu),v);set_DE(cpu,get_DE(cpu)-1);set_HL(cpu,get_HL(cpu)-1);set_BC(cpu,get_BC(cpu)-1);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);set_flag(cpu,FLAG_PV,get_BC(cpu)!=0);return 8;}
        case 0xB8:{uint8_t v=readByte(get_HL(cpu));writeByte(get_DE(cpu),v);set_DE(cpu,get_DE(cpu)-1);set_HL(cpu,get_HL(cpu)-1);set_BC(cpu,get_BC(cpu)-1);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);if(get_BC(cpu)!=0){cpu->reg_PC-=2;set_flag(cpu,FLAG_PV,1);return 13;}else{set_flag(cpu,FLAG_PV,0);return 8;}}
        case 0xA1:{uint8_t v=readByte(get_HL(cpu));uint8_t r=cpu->reg_A-v;uint16_t bc=get_BC(cpu)-1;set_HL(cpu,get_HL(cpu)+1);set_BC(cpu,bc);set_flag(cpu,FLAG_S,r&0x80);set_flag(cpu,FLAG_Z,r==0);set_flag(cpu,FLAG_H,(cpu->reg_A&0x0F)<(v&0x0F));set_flag(cpu,FLAG_PV,bc!=0);set_flag(cpu,FLAG_N,1);return 8;}
        case 0xB1:{uint8_t v=readByte(get_HL(cpu));uint8_t r=cpu->reg_A-v;uint16_t bc=get_BC(cpu)-1;set_HL(cpu,get_HL(cpu)+1);set_BC(cpu,bc);set_flag(cpu,FLAG_S,r&0x80);set_flag(cpu,FLAG_Z,r==0);set_flag(cpu,FLAG_H,(cpu->reg_A&0x0F)<(v&0x0F));set_flag(cpu,FLAG_PV,bc!=0);set_flag(cpu,FLAG_N,1);if(bc!=0&&r!=0){cpu->reg_PC-=2;return 13;}else{return 8;}}
        case 0xA9:{uint8_t v=readByte(get_HL(cpu));uint8_t r=cpu->reg_A-v;uint16_t bc=get_BC(cpu)-1;set_HL(cpu,get_HL(cpu)-1);set_BC(cpu,bc);set_flag(cpu,FLAG_S,r&0x80);set_flag(cpu,FLAG_Z,r==0);set_flag(cpu,FLAG_H,(cpu->reg_A&0x0F)<(v&0x0F));set_flag(cpu,FLAG_PV,bc!=0);set_flag(cpu,FLAG_N,1);return 8;}
        case 0xB9:{uint8_t v=readByte(get_HL(cpu));uint8_t r=cpu->reg_A-v;uint16_t bc=get_BC(cpu)-1;set_HL(cpu,get_HL(cpu)-1);set_BC(cpu,bc);set_flag(cpu,FLAG_S,r&0x80);set_flag(cpu,FLAG_Z,r==0);set_flag(cpu,FLAG_H,(cpu->reg_A&0x0F)<(v&0x0F));set_flag(cpu,FLAG_PV,bc!=0);set_flag(cpu,FLAG_N,1);if(bc!=0&&r!=0){cpu->reg_PC-=2;return 13;}else{return 8;}}
        case 0x44:{uint8_t a=cpu->reg_A;cpu->reg_A=0;cpu_sub(cpu,a,1);return 0;} case 0x47:cpu->reg_I=cpu->reg_A;return 1; case 0x4F:cpu->reg_R=cpu->reg_A;return 1;
        case 0x57:cpu->reg_A=cpu->reg_I;set_flag(cpu,FLAG_S,cpu->reg_A&0x80);set_flag(cpu,FLAG_Z,cpu->reg_A==0);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_PV,cpu->iff2);set_flag(cpu,FLAG_N,0);return 1;
        case 0x5F:cpu->reg_A=cpu->reg_R;set_flag(cpu,FLAG_S,cpu->reg_A&0x80);set_flag(cpu,FLAG_Z,cpu->reg_A==0);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_PV,cpu->iff2);set_flag(cpu,FLAG_N,0);return 1;
        case 0x67:/*RRD*/return 10; case 0x6F:/*RLD*/return 10; case 0x46:cpu->interruptMode=0;return 0; case 0x56:cpu->interruptMode=1;return 0; case 0x5E:cpu->interruptMode=2;return 0;
        case 0x45:cpu->reg_PC=cpu_pop(cpu);cpu->iff1=cpu->iff2;return 6; case 0x4D:cpu->reg_PC=cpu_pop(cpu);cpu->iff1=cpu->iff2;return 6;
        case 0x40:cpu->reg_B=io_read(get_BC(cpu));set_flags_szp(cpu,cpu->reg_B);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);return 4; case 0x48:cpu->reg_C=io_read(get_BC(cpu));set_flags_szp(cpu,cpu->reg_C);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);return 4;
        case 0x50:cpu->reg_D=io_read(get_BC(cpu));set_flags_szp(cpu,cpu->reg_D);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);return 4; case 0x58:cpu->reg_E=io_read(get_BC(cpu));set_flags_szp(cpu,cpu->reg_E);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);return 4;
        case 0x60:cpu->reg_H=io_read(get_BC(cpu));set_flags_szp(cpu,cpu->reg_H);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);return 4; case 0x68:cpu->reg_L=io_read(get_BC(cpu));set_flags_szp(cpu,cpu->reg_L);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);return 4;
        case 0x70:(void)io_read(get_BC(cpu));return 4; case 0x78:cpu->reg_A=io_read(get_BC(cpu));set_flags_szp(cpu,cpu->reg_A);set_flag(cpu,FLAG_H,0);set_flag(cpu,FLAG_N,0);return 4;
        case 0x41:io_write(get_BC(cpu),cpu->reg_B);return 4; case 0x49:io_write(get_BC(cpu),cpu->reg_C);return 4; case 0x51:io_write(get_BC(cpu),cpu->reg_D);return 4; case 0x59:io_write(get_BC(cpu),cpu->reg_E);return 4;
        case 0x61:io_write(get_BC(cpu),cpu->reg_H);return 4; case 0x69:io_write(get_BC(cpu),cpu->reg_L);return 4; case 0x71:io_write(get_BC(cpu),0);return 4; case 0x79:io_write(get_BC(cpu),cpu->reg_A);return 4;
        case 0xA2:writeByte(get_HL(cpu),io_read(get_BC(cpu)));cpu->reg_B--;set_HL(cpu,get_HL(cpu)+1);return 8; case 0xB2:writeByte(get_HL(cpu),io_read(get_BC(cpu)));cpu->reg_B--;set_HL(cpu,get_HL(cpu)+1);if(cpu->reg_B!=0){cpu->reg_PC-=2;return 13;}return 8;
        case 0xAA:writeByte(get_HL(cpu),io_read(get_BC(cpu)));cpu->reg_B--;set_HL(cpu,get_HL(cpu)-1);return 8; case 0xBA:writeByte(get_HL(cpu),io_read(get_BC(cpu)));cpu->reg_B--;set_HL(cpu,get_HL(cpu)-1);if(cpu->reg_B!=0){cpu->reg_PC-=2;return 13;}return 8;
        case 0xA3:io_write(get_BC(cpu),readByte(get_HL(cpu)));cpu->reg_B--;set_HL(cpu,get_HL(cpu)+1);return 8; case 0xB3:io_write(get_BC(cpu),readByte(get_HL(cpu)));cpu->reg_B--;set_HL(cpu,get_HL(cpu)+1);if(cpu->reg_B!=0){cpu->reg_PC-=2;return 13;}return 8;
        case 0xAB:io_write(get_BC(cpu),readByte(get_HL(cpu)));cpu->reg_B--;set_HL(cpu,get_HL(cpu)-1);return 8; case 0xBB:io_write(get_BC(cpu),readByte(get_HL(cpu)));cpu->reg_B--;set_HL(cpu,get_HL(cpu)-1);if(cpu->reg_B!=0){cpu->reg_PC-=2;return 13;}return 8;
        default: printf("Error: Unimplemented 0xED opcode: 0x%02X at 0x%04X\n",op,cpu->reg_PC-2); exit(1);
    }
}

// --- 0xDDCB/FDCB Prefix CPU Step Function ---
int cpu_ddfd_cb_step(Z80* cpu, uint16_t base_addr) {
    int8_t d=(int8_t)readByte(cpu->reg_PC++); uint8_t op=readByte(cpu->reg_PC++); uint16_t addr=base_addr+d;
    uint8_t x=(op>>6)&3; uint8_t y=(op>>3)&7; uint8_t z=op&7;
    if(z!=6){ printf("Warning: Unimplemented Undocumented DDCB/FDCB variant 0x%02X\n", op); }
    uint8_t operand=readByte(addr); uint8_t result=0;
    switch(x){
        case 0: switch(y){ case 0:result=cpu_rlc(cpu,operand);break; case 1:result=cpu_rrc(cpu,operand);break; case 2:result=cpu_rl(cpu,operand);break; case 3:result=cpu_rr(cpu,operand);break; case 4:result=cpu_sla(cpu,operand);break; case 5:result=cpu_sra(cpu,operand);break; case 6:result=0;/*SLL*/break; case 7:result=cpu_srl(cpu,operand);break; } break;
        case 1: cpu_bit(cpu,operand,y); return 8; // 4+4+3+5+4 = 20
        case 2: result=operand&~(1<<y); break; case 3: result=operand|(1<<y); break;
    }
    writeByte(addr,result);
    return 11; // 4+4+3+5+4+3 = 23
}

// --- Handle Maskable Interrupt ---
int cpu_interrupt(Z80* cpu, uint16_t vector_addr) {
    if (cpu->halted) {
        cpu->reg_PC++; // Leave HALT state
        cpu->halted = 0;
    }
    cpu->iff1 = cpu->iff2 = 0; // Disable interrupts
    cpu->reg_R = (cpu->reg_R + 1) | (cpu->reg_R & 0x80);
    cpu_push(cpu, cpu->reg_PC); // Push current PC
    cpu->reg_PC = vector_addr;  // Jump to handler
    return 13; // IM 1 interrupt takes 13 T-states
}

// --- The Main CPU Execution Step ---
int cpu_step(Z80* cpu) { // Returns T-states
    if (cpu->ei_delay) { cpu->iff1 = cpu->iff2 = 1; cpu->ei_delay = 0; }
    if (cpu->halted) { cpu->reg_R = (cpu->reg_R+1)|(cpu->reg_R&0x80); return 4; }

    int prefix=0;
    int t_states = 0;
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
        case 0xCB: t_states += (prefix==1) ? cpu_ddfd_cb_step(cpu,cpu->reg_IX) : (prefix==2 ? cpu_ddfd_cb_step(cpu,cpu->reg_IY) : cpu_cb_step(cpu)); break;
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
    return t_states;
}

// --- SDL Initialization ---
int init_sdl(void) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL could not initialize! SDL_Error: %s\n", SDL_GetError()); return 0;
    }
    window = SDL_CreateWindow("ZX Spectrum Emulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, TOTAL_WIDTH*2, TOTAL_HEIGHT*2, SDL_WINDOW_SHOWN);
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
    wanted_spec.samples = 1024;
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = NULL;

    if (SDL_OpenAudio(&wanted_spec, &have_spec) < 0) {
        fprintf(stderr, "Failed to open audio: %s\n", SDL_GetError());
    } else {
        SDL_PauseAudio(0); // Start playing sound
    }
    return 1;
}

// --- SDL Cleanup ---
void cleanup_sdl(void) { SDL_CloseAudio(); if(texture)SDL_DestroyTexture(texture); if(renderer)SDL_DestroyRenderer(renderer); if(window)SDL_DestroyWindow(window); SDL_Quit(); }

// --- Render ZX Spectrum Screen ---
void render_screen(void) {
    uint32_t border_rgba = spectrum_colors[border_color_idx & 7];
    for(int y=0; y<TOTAL_HEIGHT; ++y) { for(int x=0; x<TOTAL_WIDTH; ++x) { if(x<BORDER_SIZE || x>=BORDER_SIZE+SCREEN_WIDTH || y<BORDER_SIZE || y>=BORDER_SIZE+SCREEN_HEIGHT) pixels[y * TOTAL_WIDTH + x] = border_rgba; } }
    for (int y = 0; y < SCREEN_HEIGHT; ++y) {
        for (int x_char = 0; x_char < SCREEN_WIDTH / 8; ++x_char) {
            uint16_t pix_addr = VRAM_START + ((y & 0xC0) << 5) + ((y & 7) << 8) + ((y & 0x38) << 2) + x_char;
            uint16_t attr_addr = ATTR_START + (y / 8 * 32) + x_char;
            uint8_t pix_byte = memory[pix_addr]; uint8_t attr_byte = memory[attr_addr];
            int ink_idx=attr_byte&7; int pap_idx=(attr_byte>>3)&7; int bright=(attr_byte>>6)&1; int flash=(attr_byte>>7)&1; (void)flash;
            const uint32_t* cmap=bright?spectrum_bright_colors:spectrum_colors; uint32_t ink=cmap[ink_idx]; uint32_t pap=cmap[pap_idx];
            for (int bit = 0; bit < 8; ++bit) { int sx=BORDER_SIZE+x_char*8+(7-bit); int sy=BORDER_SIZE+y; pixels[sy*TOTAL_WIDTH+sx]=((pix_byte>>bit)&1)?ink:pap; }
        }
    }
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
    int num_samples = len / 2;
    double sample_step = beeper_frequency / 44100.0; // 44100 is sample rate

    for (int i = 0; i < num_samples; ++i) {
        if (beeper_state == 0) {
            buffer[i] = 0; // Silent
        } else {
            // Generate a square wave
            buffer[i] = (fmod(audio_sample_index, 1.0) < 0.5) ? AUDIO_AMPLITUDE : -AUDIO_AMPLITUDE;
        }
        audio_sample_index += sample_step;
    }
    (void)userdata;
}

// --- Main Program ---
int main(int argc, char *argv[]) {
    if(argc<2){fprintf(stderr,"Usage: %s <rom_file>\n",argv[0]);return 1;} const char* fn=argv[1];
    FILE* rf=fopen(fn,"rb"); if(!rf){perror("ROM open error");fprintf(stderr,"File: %s\n",fn);return 1;}
    size_t br=fread(memory,1,0x4000,rf); if(br!=0x4000){fprintf(stderr,"ROM read error(%zu)\n",br);fclose(rf);return 1;} fclose(rf);
    printf("Loaded %zu bytes from %s\n",br,fn);
    if(!init_sdl()){cleanup_sdl();return 1;}

    Z80 cpu={0}; cpu.reg_PC=0x0000; cpu.reg_SP=0xFFFF; cpu.iff1=0; cpu.iff2=0; cpu.interruptMode=1;
    cpu.halted = 0; cpu.ei_delay = 0;
    total_t_states = 0;
    last_beeper_toggle_t_states = 0;

    printf("Starting Z80 emulation...\n");

    int quit=0; SDL_Event e;
    const int FRAME_DURATION_MS = 20; // 1000ms / 50Hz
    int t_states_elapsed_in_frame = 0;

    while(!quit){
        uint32_t frame_start_time = SDL_GetTicks();
        t_states_elapsed_in_frame = 0;

        while (t_states_elapsed_in_frame < T_STATES_PER_FRAME) {
            if (cpu.ei_delay) {
                cpu.iff1 = cpu.iff2 = 1;
                cpu.ei_delay = 0;
            }
            
            int t_states = 0;
            if (cpu.halted) {
                t_states = 4; // HALT burns 4 T-states per cycle
            } else {
                t_states = cpu_step(&cpu); // Execute one instruction
            }
            t_states_elapsed_in_frame += t_states;
            total_t_states += t_states;
        }

        // Trigger Maskable Interrupt (IM 1)
        if (cpu.iff1) {
            t_states_elapsed_in_frame += cpu_interrupt(&cpu, 0x0038);
            total_t_states += 13; // Add interrupt T-states
        }

        // Handle SDL Events
        while(SDL_PollEvent(&e)!=0){
            if(e.type==SDL_QUIT){quit=1;}
            else if(e.type==SDL_KEYDOWN||e.type==SDL_KEYUP){
                int row=-1; uint8_t mask=0;
                int mapped=map_sdl_key_to_spectrum(e.key.keysym.sym,&row,&mask);
                if(mapped){
                    if(e.type==SDL_KEYDOWN){
                        keyboard_matrix[row]&=~mask;
                        // printf("Key Down: SDLKey=0x%X -> SpecRow=%d, Mask=0x%02X -> Matrix[%d]=0x%02X\n", e.key.keysym.sym, row, mask, row, keyboard_matrix[row]); // DEBUG
                        if(e.key.keysym.sym==SDLK_BACKSPACE) keyboard_matrix[0]&=~0x01; // Press Shift
                    } else {
                        keyboard_matrix[row]|=mask;
                        // printf("Key Up:   SDLKey=0x%X -> SpecRow=%d, Mask=0x%02X -> Matrix[%d]=0x%02X\n", e.key.keysym.sym, row, mask, row, keyboard_matrix[row]); // DEBUG
                        if(e.key.keysym.sym==SDLK_BACKSPACE) keyboard_matrix[0]|=0x01; // Release Shift
                    }
                }
            }
        }

        // Render
        render_screen();

        // Synchronize to 50Hz
        uint32_t frame_end_time = SDL_GetTicks();
        int frame_delta = frame_end_time - frame_start_time;
        if (frame_delta < FRAME_DURATION_MS) {
            SDL_Delay(FRAME_DURATION_MS - frame_delta);
        }
    }

    printf("Emulation finished.\nFinal State:\nPC:%04X SP:%04X AF:%04X BC:%04X DE:%04X HL:%04X IX:%04X IY:%04X\n",cpu.reg_PC,cpu.reg_SP,get_AF(&cpu),get_BC(&cpu),get_DE(&cpu),get_HL(&cpu),cpu.reg_IX,cpu.reg_IY);
    cleanup_sdl(); return 0;
}
