#include "lwc32.cpp"
#include <cstdlib>
#include <cstdio>
#include <curses.h>
#include <cstring>
#include <ctime>
#include <chrono>
#include <thread>
#include "memoryMap.hpp"
using namespace std;

// Macro for control character
#define ctrl(x) ((x) & 0x1f)

// Minimum size the terminal is allowed to be
#define MINWIDTH 106
#define MINHEIGHT 34

// Terminal size
#define TERM_WIDTH 64
#define TERM_HEIGHT 32

// How many miliseconds until the terminal is updated
#define REFRESH_RATE 20

int cycleTime;  // Time in ns per half cycle
int clockSpeed = 20000;   // Clock speed in hertz
int newClockSpeed;
bool pauseClock = false;    // Whether the clock is paused or not

bool modeSetClock = false;
bool clockSleepMode = false;    // Whether clock uses a wait loop or thread sleep, default thread sleep

uint64_t startTime;
uint64_t timeSincelastClock;
uint64_t lastRealUpdate;

// Initial value to force the code to run to setup windows for the real terminal size
int lastCols = -1;
int lastLines = -1;

// CPU object
LWC32 cpu;

// Pointer to the machine's memory space
unsigned short* mainMemory;

// Pointer to the MPU memory
unsigned char* mpuMemory;

// The set width of the diagnostic window
#define DIAG_WIN_WIDTH 40

// Buffer to keep the keyboard's input
char keyboardBuffer[256];
// Read pointer
unsigned char kbR = 0;
// Write pointer
unsigned char kbW = 0;
// Boolean for if the buffer is full
bool kbF = false;

// Whether to enable the cursor on the text display
bool enableCursor = false;

// All used windows
WINDOW *textWindow;
WINDOW *diagWindow;
WINDOW *boxWindow;

// Definition for printing the cpu state on the diag window
void printLWC32Diag(LWC32 cpu);

// Loads a file into memory, first two bytes of the file is the load position
void loadFile(char* filename, unsigned short *memory, int memSize) {
    FILE* file = fopen(filename, "rb");
    if (file == NULL) {
        printf("Error opening file: %s\n", filename);
        exit(-1);
    }
    
    int a1 = fgetc(file);
    int a2 = fgetc(file);
    
    if (a2 == EOF) {
        printf("Unexpected end of file: %s\n", filename);
        exit(-1);
    }
    
    int loadIndex = a2&0xff | (a1&0xff)<<8;
    if (loadIndex >= memSize) {
        printf("File \"%s\" loads out of bounds.\n", filename);
        exit(-1);
    }
    
    printf("Loading file \"%s\" at address %04X\n", filename, loadIndex);
    
    for (int i = 0 ;; i++) {
        int readValue1 = fgetc(file);
        int readValue2 = fgetc(file);
        if (readValue1 == EOF || readValue2 == EOF)
            break;
        
        if (loadIndex+i >= memSize) {
            printf("File \"%s\" loaded out of bounds. %04X\n", filename, loadIndex+i);
            exit(-1);
        }
        
        unsigned short v = readValue2&0xff | (readValue1&0xff)<<8;
        memory[loadIndex+i] = v;
    }
    fclose(file);
}

// Write to the keyboard buffer
void writeKB(char c) {
    if (!kbF) {
        keyboardBuffer[kbW++] = c;
    }
    if (kbW == kbR) {
        kbF = true;
    }
}

// Read from the keyboard buffer
int readKB() {
    if (kbR != kbW || kbF) {
        return keyboardBuffer[kbR++];
        kbF = false;
    } else {
        return -1;
    }
}

// Get time per half cycle
int toNsPerHalfCycle(int hertz) {
    return 1000000000/hertz/2;
}

// Refresh the whole terminal screen
void screenRefresh() {
    int kbInput = wgetch(textWindow);
    
    // Handle's keyboard input
    if (kbInput == KEY_F(2)) {
        cpu.interruptPinStates |= 0b0001;
    } else if(kbInput == KEY_F(3)) {
        cpu.interruptPinStates |= 0b0010;
    } else if(kbInput == KEY_F(4)) {
        cpu.interruptPinStates |= 0b0100;
    } else if(kbInput == KEY_F(5)) {
        cpu.interruptPinStates |= 0b1000;
    } else if (kbInput == KEY_F(6)) {
        cpu.setCarryState = true;
    } else if (kbInput == KEY_F(7)) {
        cpu.resetState = true;
    } else if (kbInput == KEY_F(8)) {
        pauseClock ^= true;
    } else if (kbInput == KEY_F(9)) {
        cpu.clockState ^= true;
    } else if (kbInput == KEY_F(10)) {
        if (!modeSetClock) {
            modeSetClock = true;
            newClockSpeed = 0;
        } else {
            modeSetClock = false;
            clockSpeed = newClockSpeed;
        }
    } else if (kbInput == KEY_F(11)) {
        clockSleepMode ^= true;
    } else if (kbInput == KEY_BACKSPACE || kbInput == KEY_DC || kbInput == 127) {
        if (modeSetClock) {
            newClockSpeed /= 10;
        } else {
            writeKB(0x08);
        }
    } else if (kbInput >= 0 && kbInput <= 0xff) {
        if (modeSetClock) {
            if ((char)kbInput >= 0x30 && (char)kbInput <= 0x39) {
                newClockSpeed *= 10;
                newClockSpeed += (char)kbInput-0x30;
            } else if ((char)kbInput == 0xa) {
                modeSetClock = false;
                clockSpeed = newClockSpeed;
            }
        } else {
            writeKB((char)kbInput);
        }
    }
    
    if (clockSpeed <= 0) {
        clockSpeed = 1;
    }
    cycleTime = toNsPerHalfCycle(clockSpeed);
    
    // Runs if the terminal is too small
    if (LINES < MINHEIGHT || COLS < MINWIDTH) {
        wclear(stdscr);
        wmove(stdscr, LINES/2, COLS/2-9);
        wprintw(stdscr, "Minimum size: %dx%d\n", MINWIDTH, MINHEIGHT);
        wrefresh(stdscr);
        return;
    }
    
    // Resizes the windows if the terminal size has changed
    if (COLS != lastCols || LINES != lastLines) {
        lastCols = COLS;
        lastLines = LINES;
        
        wclear(stdscr);
        wrefresh(stdscr);
        
        wresize(diagWindow, LINES-1, DIAG_WIN_WIDTH-1);
        wresize(textWindow, TERM_HEIGHT, TERM_WIDTH);
        wresize(boxWindow, TERM_HEIGHT+2, TERM_WIDTH+2);
        
        wclear(boxWindow);
        box(boxWindow, 0, 0);
        wrefresh(boxWindow);
        
        return;
    }
    
    // Update the text output with the frame buffer
    for (int y = 0; y < TERM_HEIGHT; y++) {
        for (int x = 0; x < TERM_WIDTH; x++) {
            int i = x+y*TERM_WIDTH;
            char c;
            if (i == mainMemory[CURSOR] && enableCursor) {
                c = '_';
            } else {
                c = mainMemory[SCREEN_MEMORY+i];
            }
            c &= 0x7f;
            if (c < 0x20 || c == 0x7f) {
                c = 0x20;
            }
            mvwaddch(textWindow, y, x, c);
        }
    }
    
    // Print the diagnostics
    wmove(diagWindow, 0, 0);
    printLWC32Diag(cpu);
    
    wprintw(diagWindow, "F2-F5: Trigger interrupt 1-4\n");
    wprintw(diagWindow, "F6: Set carry - F7: Reset   \n");
    wprintw(diagWindow, "F8: Pause/Unpause - F9: Toggle Clock\n");
    wprintw(diagWindow, "F10: Set Clockspeed                 \n");
    wprintw(diagWindow, "F11: Toggle Clock Wait Mode\n");
    wprintw(diagWindow, "                           ");
    
    // Refresh the screen
    wrefresh(textWindow);
    wrefresh(diagWindow);
    
    lastCols = COLS;
    lastLines = LINES;
}

// Scrolls the data in the frame buffer up one line
void scrollTerm() {
    for (int y = 1; y < TERM_HEIGHT; y++) {
        for (int x = 0; x < TERM_WIDTH; x++) {
            int i = x+y*TERM_WIDTH;
            char c = mainMemory[SCREEN_MEMORY+i];
            mainMemory[SCREEN_MEMORY+i-TERM_WIDTH] = c;
        }
    }
    for (int x = 0; x < TERM_WIDTH; x++) {
        mainMemory[SCREEN_MEMORY+TERM_WIDTH*(TERM_HEIGHT-1)+x] = 0x20;
    }
}

// Return the cursor back to the start of the line
void cr() {
    int y = mainMemory[CURSOR] / TERM_WIDTH;
    mainMemory[CURSOR] = y*TERM_WIDTH;
}

// Move the cursor down one line, scrolling if needed
void lf() {
    if (mainMemory[CURSOR]+TERM_WIDTH >= SCREEN_MEMORY_SIZE) {
        scrollTerm();
    } else {
        mainMemory[CURSOR] += TERM_WIDTH;
    }
}

void UpdateScreenThread() {
    while (1) {
        this_thread::sleep_for(chrono::milliseconds(REFRESH_RATE));
        screenRefresh();
    }
}

// Get time stamp in miliseconds.
uint64_t milis() {
    uint64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch())
        .count();
    return ms; 
}

// Get time stamp in microseconds.
uint64_t micros() {
    uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch())
        .count();
    return us; 
}

// Get time stamp in nanoseconds.
uint64_t nanos() {
    uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch())
        .count();
    return ns; 
}

bool canRead() {
    return (mpuMemory[mainMemory[0]<<8&0xff00 | cpu.addressOutput>>8] & 0b1) != 0 || !cpu.isSafeMode();
}

bool canWrite() {
    return (mpuMemory[mainMemory[0]<<8&0xff00 | cpu.addressOutput>>8] & 0b10) != 0 || !cpu.isSafeMode();
}

int main(int argc, char *argv[]) {
    
    // Setup memory for the machine
    mainMemory = (unsigned short*)malloc(0x10000*sizeof(unsigned short));
    
    // Setup mpu memory for the mpu
    mpuMemory = (unsigned char*)malloc(0x10000*sizeof(unsigned char));
    
    // Clear mpu memory
    memset(mpuMemory, 0, 0x10000);
    
    // Make sure the mpu bank selector is block 0
    mainMemory[0] = 0;
    
    // Load all files in the arguments
    if (argc == 1) {
        printf("Usage:\nlwc32emu ROM ...");
        exit(-1);
    }
    for (int i = 1; i < argc; i++) {
        loadFile(argv[i], mainMemory, 0x10000);
    }
    
    // Init curses
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    
    textWindow = newwin(MINHEIGHT-2, 1, 1, 41);
    diagWindow = newwin(MINHEIGHT, 39, 1, 1);
    boxWindow = newwin(MINHEIGHT, 3, 0, 40);
    
    // Make sure the keyboard input is non-blocking, and we capture F keys too
    keypad(textWindow, TRUE);
    nodelay(textWindow, TRUE);
    
    wmove(textWindow, 0, 0);
    
    // Start the thread for refreshing the screen
    thread t_refresh(UpdateScreenThread);
    
    // Set the cpu initial pin state
    cpu.resetState = true;
    
    cycleTime = toNsPerHalfCycle(clockSpeed);
    
    for (;;) {
        uint64_t st = nanos();
        
        if (!clockSleepMode) {
            this_thread::sleep_for(chrono::nanoseconds(cycleTime));
        } else {
            while (nanos() < st+cycleTime) {
                this_thread::yield();
            }
        }
        
        uint64_t last = startTime;
        startTime = st;
        
        bool realUpdate = false;
        if (milis() > lastRealUpdate+1000) {
            realUpdate = true;
            lastRealUpdate = milis();
            timeSincelastClock = startTime-last;
        }
        
        if (!pauseClock) {
            cpu.clockState ^= true; // Toggle cpu clock
        }
       
        cpu.UpdateLogic(); // Update cpu state
        
        if (cpu.resetState) {
            memset(mpuMemory, 0, 0x10000);
            mainMemory[0] = 0;
        }
        
        cpu.resetState = false; // Clear reset
        cpu.setCarryState = false; // Clear set carry
        cpu.auxPinStates = 0;
        
        // Read memory
        if (cpu.readState && canRead()) {
            cpu.dataBusInput = mainMemory[cpu.addressOutput];
        } else {
            cpu.dataBusInput = 0;
        }
        
        // Perform a read from device if requested
        if (cpu.readState && canRead()) {
            if (cpu.addressOutput >= 0x0001 && cpu.addressOutput <= 0x00ff) {
                cpu.dataBusInput = mpuMemory[mainMemory[0]<<8&0xff00 | cpu.addressOutput];
            } else if (cpu.addressOutput == 0x100 && cpu.clockState) {
                // Address 0x0100 is the terminal I/O
                int c = readKB();
                if (c == -1) {
                    cpu.dataBusInput = 0;
                    cpu.auxPinStates |= 0b01;
                } else {
                    cpu.dataBusInput = (char)c;
                    cpu.auxPinStates |= 0b10;
                }
            } else if (cpu.addressOutput == 0x0101 && cpu.clockState) {
                cpu.dataBusInput = kbR != kbW || kbF ? 1 : 0;
            }
        }
        
        // Write memory
        if (cpu.writeState && canWrite()) {
            mainMemory[cpu.addressOutput] = cpu.dataBusOutput;
        }
        
        // Perform a write to device if requested
        if (cpu.writeState && canWrite()) {
            if (cpu.addressOutput >= 0x0001 && cpu.addressOutput <= 0x00ff) {
                mpuMemory[mainMemory[0]<<8&0xff00 | cpu.addressOutput] = cpu.dataBusOutput;
            } else if (cpu.addressOutput == 0x100) {
                // Address 0x0100 is terminal I/O
                char c = cpu.dataBusOutput;
                if (c == 0x8 && mainMemory[CURSOR] > 0) {
                    mainMemory[SCREEN_MEMORY+--mainMemory[CURSOR]] = 0x20;
                } else if (c == 0xa) {
                    lf();
                    cr();
                } else if (c == 0xc) {
                    for (int i = 0; i < SCREEN_MEMORY_SIZE; i++) {
                        mainMemory[SCREEN_MEMORY+i] = 0x20;
                    }
                    mainMemory[CURSOR] = 0;
                } else if (c >= 0x20 && c <= 0x7e) {
                    mainMemory[SCREEN_MEMORY+mainMemory[CURSOR]++] = c;
                    if (mainMemory[CURSOR] >= SCREEN_MEMORY_SIZE) {
                        mainMemory[CURSOR] = SCREEN_MEMORY_SIZE-TERM_WIDTH;
                        lf();
                    }
                }
            } else if (cpu.addressOutput == 0x0101) {
                if ((cpu.dataBusOutput & 0b1) != 0) {
                    kbR = kbW;
                    kbF = false;
                }
                if ((cpu.dataBusOutput & 0b10) != 0) {
                    enableCursor = true;
                }
                if ((cpu.dataBusOutput & 0b100) != 0) {
                    enableCursor = false;
                }
            } else if (cpu.addressOutput == 0x01ff) {
                if (cpu.dataBusOutput == 0) break;
                cpu.interruptPinStates &= cpu.dataBusOutput^0b1111;
            }
        }
    }
    
    // Close curses
    endwin();
    
    return 0;
}

// Print the cpu state to the diag window
void printLWC32Diag(LWC32 cpu) {
    if (modeSetClock) {
        wprintw(diagWindow, "Cycle Time: ");
        wattron(diagWindow, A_REVERSE);
        wprintw(diagWindow, "%dHz\n", newClockSpeed);
        wattroff(diagWindow, A_REVERSE);
    } else {
        wprintw(diagWindow, "Cycle Time: %dns (%dHz)\n", cycleTime*2, clockSpeed);
    }
    wprintw(diagWindow, "Real  Time: %lldns (%dHz)\n\n", timeSincelastClock*2, 1000000000/timeSincelastClock/2);
    
    wprintw(diagWindow, "R0: %04X  R1: %04X  R2: %04X  R3: %04X\n", cpu.registers[1], cpu.registers[2], cpu.registers[3], cpu.registers[4]);
    wprintw(diagWindow, "R4: %04X  R5: %04X  R6: %04X  R7: %04X\n", cpu.registers[5], cpu.registers[6], cpu.registers[7], cpu.registers[8]);
    wprintw(diagWindow, "R8: %04X  R9: %04X  RA: %04X  RB: %04X\n", cpu.registers[9], cpu.registers[10], cpu.registers[11], cpu.registers[12]);
    wprintw(diagWindow, "RR: %04X  DI: %04X  DO: %04X  AD: %04X\n\n", cpu.registers[13], cpu.dataBusInput, cpu.dataBusOutput, cpu.addressOutput);
    
    wprintw(diagWindow, "ST: ");
    
    for (int i = 15; i >= 0; i--) {
        wprintw(diagWindow, "%d", cpu.registers[ST]>>i&1);
    }
    
    wprintw(diagWindow, "\n\nCK: %01X  CS: %01X\n", cpu.clockState, cpu.cpuState);
    wprintw(diagWindow, "RD: %01X  WR: %01X\n", cpu.readState, cpu.writeState);
    wprintw(diagWindow, "CR: %01X  CW: %01X\n", canRead(), canWrite());
    wprintw(diagWindow, "MPUA: %04X\n\n", cpu.addressOutput>>8 | mainMemory[0]<<8&0xff00);
    
    wprintw(diagWindow, "PC: %04X  SP: %04X\n", cpu.pc, cpu.registers[14]);
    wprintw(diagWindow, "IR: %04X  IL: %04X\n\n", cpu.ir, cpu.registers[0]);
    wprintw(diagWindow, "INT: %d\n\n", cpu.insideInt);
    wprintw(diagWindow, "IRQ: ");
    
    for (int i = 3; i >= 0; i--) {
        wprintw(diagWindow, "%d", cpu.interruptPinStates>>i&1);
    }
    wprintw(diagWindow, "\n\n");
}