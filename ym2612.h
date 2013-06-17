#ifndef YM2612_H_
#define YM2612_H_

#include <stdint.h>
#include <stdio.h>

#define NUM_PART_REGS (0xB7-0x30)
#define NUM_CHANNELS 6
#define NUM_OPERATORS (4*NUM_CHANNELS)

#define YM_OPT_WAVE_LOG 1

typedef struct {
	uint32_t phase_inc;
	uint32_t phase_counter;
	uint16_t envelope;
	int16_t  output;
	uint16_t total_level;
	uint16_t sustain_level;
	uint8_t  rates[4];
	uint8_t  key_scaling;
	uint8_t  multiple;
	uint8_t  detune;
	uint8_t  env_phase;
} ym_operator;

typedef struct {
	FILE *   logfile;
	uint16_t fnum;
	int16_t  output;
	uint8_t  block_fnum_latch;
	uint8_t  block;
	uint8_t  keycode;
	uint8_t  algorithm;
	uint8_t  feedback;
	uint8_t  ams;
	uint8_t  pms;
	uint8_t  lr;
} ym_channel;

typedef struct {
	uint16_t fnum;
	uint8_t  block;
	uint8_t  block_fnum_latch;
	uint8_t  keycode;
} ym_supp;

typedef struct {
    int16_t     *audio_buffer;
    int16_t     *back_buffer;
    double      buffer_fraction;
    double      buffer_inc;
    uint32_t    clock_inc;
    uint32_t    buffer_pos;
    uint32_t    sample_limit;
	uint32_t    current_cycle;
	uint32_t    write_cycle;
	ym_operator operators[NUM_OPERATORS];
	ym_channel  channels[NUM_CHANNELS];
	uint16_t    timer_a;
	uint16_t    timer_a_load;
	uint16_t    timer_b;
	uint16_t    timer_b_load;
	uint16_t    env_counter;
	ym_supp     ch3_supp[3];
	uint8_t     ch3_mode;
	uint8_t     current_op;
	uint8_t     current_env_op;
	
	uint8_t     timer_control;
	uint8_t     dac_enable;
	uint8_t     status;
	uint8_t     selected_reg;
	uint8_t     selected_part;
} ym2612_context;

void ym_init(ym2612_context * context, uint32_t sample_rate, uint32_t master_clock, uint32_t clock_div, uint32_t sample_limit, uint32_t options);
void ym_run(ym2612_context * context, uint32_t to_cycle);
void ym_address_write_part1(ym2612_context * context, uint8_t address);
void ym_address_write_part2(ym2612_context * context, uint8_t address);
void ym_data_write(ym2612_context * context, uint8_t value);
uint8_t ym_read_status(ym2612_context * context);

#endif //YM2612_H_

