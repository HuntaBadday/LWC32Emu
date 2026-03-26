#include <random>
#include <cstdint>

// Constants for each opcode
#define BRK 0
#define JMP 1
#define MOV 2
#define LOD 3
#define STO 4
#define RLOD 5
#define RSTO 6
#define ALU 7
#define CMP 8
#define PUSH 9
#define POP 10
#define JSR 11
#define RTS 12
#define INT 13
#define MBIT 14
#define NOP 15

// Constants for each ALU operation
#define ADD 0
#define ADC 1
#define SUB 2
#define SBC 3
#define AND 4
#define OR 5
#define XOR 6
#define RAND 7
#define SHL 8
#define ROL 9
#define SHR 10
#define ROR 11
#define MUL 12
#define SMUL 13
#define DIV 14
#define SDIV 15

// Register ids
#define RR 13
#define SP 14
#define ST 15

// Flag bits
#define F_C 0x0001
#define F_Z 0x0002
#define F_V 0x0004
#define F_N 0x0008

#define F_XA 0x0010
#define F_XB 0x0020

#define F_I0 0x0100
#define F_I1 0x0200
#define F_I2 0x0400
#define F_I3 0x0800

#define F_P 0x4000
#define F_B 0x8000

std::random_device LWC32RandomDevice;
std::mt19937 LWC32RandomNumberGenerator(LWC32RandomDevice());

class LWC32 {
    // All public! This is so extracting all state information is possible.
    // Modifying internal states voids all warranty!
    public:
    
    // Instruction operand lengths
    const int opNums[16] = {
        0, 1, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 0, 1, 2, 0
    };
    
    // Register files
    unsigned short registers[16];
    
    unsigned short intRegisters[4];
    
    // Program counter
    unsigned short pc;
    
    // Instruction register
    unsigned short ir;
    
    // CPU's state
    // 0 - Initialize
    // 1 - Fetch
    // 2 - Execute1
    // 3 - Execute2
    int cpuState;
    
    // IR Gets split into these registers
    int inst;
    int op1;
    int op2;
    int op3;
    
    // Flag for if the cpu is executing inside an interrupt
    bool insideInt;
    
    // Set for if the CPU should go to the fetch phase right away
    bool goToFetch;
    
    // CPU IO
    unsigned short dataBusInput; // Input to the CPU's data bus
    unsigned short dataBusOutput; // Output from the CPU's data bus
    unsigned short addressOutput; // Address output from the CPU
    
    bool readState; // CPU's memory bus read pin state
    bool writeState; // CPU's memory bus write pin state
    
    bool setCarryState; // CPU's set carry pin input
    bool setOverflowState; // CPU's set carry pin input
    unsigned char auxPinStates; // CPU's aux pins input; Aux pins 0 - 1 are bits 0 - 1
    unsigned char interruptPinStates; // CPU's interrupt pins input; Interrupt pins 0 - 3 are bits 0 - 3
    
    bool clockState; // CPU clock input
    bool syncState; // CPU sync output
    bool resetState; // CPU reset input
    
    int registerToLoad; // If not zero, will load a register on the next clock off
    
    // Pin management stuff
    bool lastClockState;
    bool lastSetCarryState;
    bool lastSetOverflowState;
    unsigned char lastAuxState;
    
    // Constructor
    LWC32() {
        goToFetch = false;
        insideInt = false;
        
        cpuState = -1;
        
        lastClockState = false;
        lastSetCarryState = false;
        lastSetOverflowState = false;
        lastAuxState = 0;
        
        registerToLoad = 0;
    }
    
    // Main logic update
    void UpdateLogic() {
        // Gets the clock phase
        bool clockHigh = !lastClockState && clockState;
        bool clockLow = lastClockState && !clockState;
        lastClockState = clockState;
        
        // When reset pin is pulled high
        if (resetState) {
            // 0xffff is the boot vector address
            addressOutput = 0xffff;
            dataBusOutput = 0;
            
            // Initial pin states to read boot vector
            syncState = false;
            readState = true;
            writeState = false;
            
            // Reset initial CPU state
            cpuState = 0;
            insideInt = false;
            
            // Initialize the status register
            registers[ST] = 0;
            return;
        }
        
        // If the set carry pin goes high
        if (setCarryState && !lastSetCarryState)
            registers[ST] |= F_C;
        // If the set overflow pin goes high
        if (setOverflowState && !lastSetOverflowState)
            registers[ST] |= F_V;
        // Bit logic to detect rising edge of aux pins to set the aux flags
        int auxChange = ~lastAuxState & auxPinStates & 0x3;
        registers[ST] |= (unsigned short)(auxChange << 4);
        
        lastSetCarryState = setCarryState;
        lastSetOverflowState = setOverflowState;
        lastAuxState = auxPinStates;
        
        if (clockHigh) {
            switch (cpuState) {
                case 1: // Fetch phase 1
                    // Fetch instruction
                    // If interrupt, replace IR to BRK with OP3 = INT#
                    if ((interruptPinStates & registers[ST]>>8 & 0xf) != 0 && !insideInt) {
                        ir = interruptPinStates & 0xf;
                        registers[ST] &= F_B^0xffff;
                    } else {
                        ir = dataBusInput;
                        if ((ir&0xf000) == 0) {
                            // If the interrupt was forced, set the flag
                            registers[ST] |= F_B;
                        }
                        addressOutput = ++pc;
                    }
                    syncState = false;
                    break;
                case 2: // Execute phase 2
                    execStage2();
                    cpuState = 3; // Do this to make sure that state 2 doesn't run on the next clock low, forcing the code to go back to fetch on the next clock low.
                    goToFetch = true;
            }
        } else if (clockLow) {
            switch (cpuState) {
                case 0: // Reset
                    // Read the boot vector and set the pc to it
                    pc = dataBusInput;
                    addressOutput = pc;
                    cpuState = 1;
                    syncState = true;
                    break;
                case 1: // Fetch phase 2
                    // Read constant value on second phase of fetch
                    registers[0] = dataBusInput;
                    cpuState = 2;
                    
                    // Extract instruction and operand
                    inst = ir>>12&0xf;
                    op1 = ir>>8&0xf;
                    op2 = ir>>4&0xf;
                    op3 = ir&0xf;
                    
                    // Check if a constant value is used
                    if (opNums[inst] >= 1) {
                        if (op1 == 0) pc++;
                    }
                    if (opNums[inst] == 2) {
                        if (op2 == 0) pc++;
                    }
                case 2: // Execute phase 1
                    // Default skip second instruction phase
                    goToFetch = true;
                    
                    // Do the first phase
                    execStage1();
            }
        }
        
        // Clean up end of instruction
        if (goToFetch && clockLow) {
            // Do a load to a register is needed
            if (registerToLoad == -1) {
                pc = dataBusInput;
                registerToLoad = 0;
            } else if (registerToLoad != 0) {
                writeRegister(registerToLoad, dataBusInput);
                if (registerToLoad != ST)
                    genZN(dataBusInput);
                registerToLoad = 0;
            }
            
            // Setup next fetch
            cpuState = 1;
            goToFetch = false;
            addressOutput = pc;
            readState = true;
            writeState = false;
            dataBusOutput = 0;
            syncState = true;
        }
    }
    
    // First execution phase
    void execStage1() {
        switch (inst) {
            case BRK:
                insideInt = true;
                addressOutput = --registers[SP];
                dataBusOutput = pc;
                pc = intRegisters[pinsToNum(op3)];
                readState = false;
                goToFetch = false;
                break;
            case JSR:
                addressOutput = --registers[SP];
                dataBusOutput = pc;
            case JMP:
                if (((registers[ST] & (op3|op2<<4) & 0x3f) != 0) != ((op2 & 0x4) != 0)) {
                    if (op2 & 0x8) {
                        pc += registers[op1];
                    } else {
                        pc = registers[op1];
                    }
                }
                readState = false;
                goToFetch = false;
                break;
            case MOV:
                writeRegister(op1, registers[op2]);
                if (op1 != ST)
                    genZN(registers[op2]);
                break;
            case LOD:
                if (op3 != 0) {
                    addressOutput = registers[op2]+registers[op3];
                } else {
                    addressOutput = registers[op2];
                }
                goToFetch = false;
                break;
            case STO:
                if (op3 != 0) {
                    addressOutput = registers[op2]+registers[op3];
                } else {
                    addressOutput = registers[op2];
                }
                dataBusOutput = registers[op1];
                readState = false;
                goToFetch = false;
                break;
            case RLOD:
                if (op3 != 0 && op3 != ST) {
                    addressOutput = pc + registers[op2]+registers[op3];
                } else {
                    addressOutput = pc + registers[op2];
                }
                if (op3 == ST) {
                    readState = false;
                }
                goToFetch = false;
                break;
            case RSTO:
                if (op3 != 0) {
                    addressOutput = pc + registers[op2]+registers[op3];
                } else {
                    addressOutput = pc + registers[op2];
                }
                dataBusOutput = registers[op1];
                readState = false;
                goToFetch = false;
                break;
            case ALU:
                doALU(true);
                break;
            case CMP:
                doALU(false);
                break;
            case PUSH:
                addressOutput = --registers[SP];
                dataBusOutput = registers[op1];
                readState = false;
                goToFetch = false;
                break;
            case POP:
                addressOutput = registers[SP]++;
                goToFetch = false;
                break;
            case RTS:
                addressOutput = registers[SP]++;
                goToFetch = false;
                if ((op3 & 0x1) != 0) {
                    insideInt = false;
                }
                break;
            case INT:
                if (op2 == 0) {
                    if (!isSafeMode()) {
                        intRegisters[pinsToNum(op3)] = registers[op1];
                    }
                } else {
                    writeRegister(op1, intRegisters[pinsToNum(op3)]);
                }
                break;
            case NOP:
                if ((op3 & 0x1) != 0) {
                    cpuState = -1; // Set to an undefined state for to halt
                    readState = false;
                    goToFetch = false;
                }
                break;
        }
    }
    
    void execStage2() {
        switch (inst) {
            case BRK:
                writeState = true;
                break;
            case LOD:
                registerToLoad = op1;
                break;
            case STO:
                writeState = true;
                break;
            case RLOD:
                if (op3 == ST) {
                    registers[op1] = addressOutput;
                    writeRegister(op1, addressOutput);
                    if (op1 != ST)
                        genZN(addressOutput);
                } else {
                    registerToLoad = op1;
                }
                break;
            case RSTO:
                writeState = true;
                break;
            case PUSH:
                writeState = true;
                break;
            case POP:
                registerToLoad = op1;
                break;
            case JSR:
                writeState = true;
                break;
            case RTS:
                registerToLoad = -1;
                break;
        }
    }
    
    void doALU(bool writeBack) {
        int tmp;
        int tmp2;
        unsigned short q;
        unsigned short r;
        unsigned short output = 0;
        switch (op3) {
            case ADD:
                tmp = registers[op1] + registers[op2];
                output = (unsigned short)tmp;
                genCarry(tmp);
                genOverflow(registers[op1], registers[op2]);
                break;
            case ADC:
                tmp = registers[op1] + registers[op2] + (registers[ST]&F_C);
                output = (unsigned short)tmp;
                genCarry(tmp);
                genOverflow(registers[op1], registers[op2] + (registers[ST]&F_C));
                break;
            case SUB:
                tmp = registers[op1] + (registers[op2]^0xffff) + 1;
                output = (unsigned short)tmp;
                genCarry(tmp);
                genOverflow(registers[op1], (registers[op2]^0xffff) + 1);
                break;
            case SBC:
                tmp = registers[op1] + (registers[op2]^0xffff) + (registers[ST]&F_C);
                output = (unsigned short)tmp;
                genCarry(tmp);
                genOverflow(registers[op1], (registers[op2]^0xffff) + (registers[ST]&F_C));
                break;
            case AND:
                output = (unsigned short)(registers[op1]&registers[op2]);
                break;
            case OR:
                output = (unsigned short)(registers[op1]|registers[op2]);
                break;
            case XOR:
                output = (unsigned short)(registers[op1]^registers[op2]);
                break;
            case RAND:
                output = get16BitRandom()&registers[op2];
                break;
            case SHL:
                output = registers[op1] << 1;
                registers[ST] &= ~(F_C);
                registers[ST] |= (registers[op1] & 0x8000) >> 15;
                break;
            case ROL:
                output = registers[op1] << 1;
                output |= registers[ST]&F_C;
                registers[ST] &= ~(F_C);
                registers[ST] |= (registers[op1] & 0x8000) >> 15;
                break;
            case SHR:
                output = registers[op1] >> 1;
                registers[ST] &= ~(F_C);
                registers[ST] |= registers[op1] & 0x0001;

                if (op2 == 0xf) {
                    output = output&0x7fff | registers[op1]&0x8000;
                }
                break;
            case ROR:
                output = registers[op1] >> 1;
                output |= (registers[ST]&F_C)<<15;
                registers[ST] &= ~(F_C);
                registers[ST] |= registers[op1] & 0x0001;
                break;
            case MUL:
                tmp = registers[op1]*registers[op2];
                if (writeBack) {
                    output = (unsigned short)tmp;
                    registers[RR] = (unsigned short)(tmp>>16);
                }
                genCarry(tmp);
                break;
            case SMUL:
                tmp = (short)registers[op1] * (short)registers[op2];
                if (writeBack) {
                    output = (unsigned short)tmp;
                    registers[RR] = (unsigned short)(tmp>>16);
                }
                genCarry(tmp);
                break;
            case DIV:
                if(registers[op2] == 0){
                    q = 0;
                    r = 0;
                } else {
                    q = (unsigned short)(registers[op1] / registers[op2]);
                    r = (unsigned short)(registers[op1] % registers[op2]);
                }
                if (writeBack) {
                    output = q;
                }
                registers[RR] = r;
                break;
            case SDIV:
                if(registers[op2] == 0){
                    q = 0;
                    r = 0;
                } else {
                    q = (unsigned short)((short)registers[op1] / (short)registers[op2]);
                    r = (unsigned short)((short)registers[op1] % (short)registers[op2]);
                }
                if (writeBack) {
                    output = q;
                }
                registers[RR] = r;
                break;
        }
        if (writeBack) {
            writeRegister(op1, output);
        }
        if (op1 != ST) {
            genZN(output);
        }
    }
    
    bool isSafeMode() {
        return registers[ST]&F_P && !insideInt;
    }
    
    void writeRegister(int reg, ushort value) {
        if (reg == ST && isSafeMode()) {
            registers[ST] = registers[ST]&0xff00 | value&0x00ff;
        } else {
            registers[reg] = value;
        }
    }
    
    // Generate carry flag depending on ALU result
    void genCarry(int data) {
        registers[ST] &= ~F_C;
        // Check if there was an overflow from an addition
        if((unsigned int)data >= 0x10000){
            registers[ST] |= F_C;
        }
    }
    
    // Generate overflow flag depending on the result of an addition
    void genOverflow(short x, short y) {
        registers[ST] &= ~F_V;
        short s = x+y;
        if (x > 0 && y > 0 && s < 0 || x < 0 && y < 0 && s > 0) {
            registers[ST] |= F_V;
        }
    }
    
    // Generate zero and negative flag based on data
    void genZN(unsigned short data) {
        registers[ST] &= ~(F_Z|F_N);
        registers[ST] |= (unsigned short)(data == 0 ? F_Z : 0);
        registers[ST] |= (unsigned short)((data & 0x8000) != 0 ? F_N : 0);
    }
    
    // Convert a set of pin booleans to a number that can be used for the interrupt number
    int pinsToNum(unsigned char pins) {
        if ((pins & 0b1) != 0) {
            return 0;
        } else if ((pins & 0b10) != 0) {
            return 1;
        } else if ((pins & 0b100) != 0) {
            return 2;
        } else if ((pins & 0b1000) != 0) {
            return 3;
        }
        return 0;
    }
    
    // Get Random Number
    uint16_t get16BitRandom() {
        std::uniform_int_distribution<uint16_t> dist(0x0000, 0xffff);
        return dist(LWC32RandomNumberGenerator);
    }
};
