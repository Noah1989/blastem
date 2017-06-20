#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "romdb.h"
#include "util.h"
#include "hash.h"
#include "genesis.h"
#include "menu.h"
#include "xband.h"
#include "realtec.h"

#define DOM_TITLE_START 0x120
#define DOM_TITLE_END 0x150
#define TITLE_START DOM_TITLE_END
#define TITLE_END (TITLE_START+48)
#define ROM_END   0x1A4
#define RAM_ID    0x1B0
#define RAM_FLAGS 0x1B2
#define RAM_START 0x1B4
#define RAM_END   0x1B8
#define REGION_START 0x1F0

char const *save_type_name(uint8_t save_type)
{
	if (save_type == SAVE_I2C) {
		return "EEPROM";
	} else if(save_type == SAVE_NOR) {
		return "NOR Flash";
	}
	return "SRAM";
}

enum {
	I2C_IDLE,
	I2C_START,
	I2C_DEVICE_ACK,
	I2C_ADDRESS_HI,
	I2C_ADDRESS_HI_ACK,
	I2C_ADDRESS,
	I2C_ADDRESS_ACK,
	I2C_READ,
	I2C_READ_ACK,
	I2C_WRITE,
	I2C_WRITE_ACK
};

char * i2c_states[] = {
	"idle",
	"start",
	"device ack",
	"address hi",
	"address hi ack",
	"address",
	"address ack",
	"read",
	"read_ack",
	"write",
	"write_ack"
};

void eeprom_init(eeprom_state *state, uint8_t *buffer, uint32_t size)
{
	state->slave_sda = 1;
	state->host_sda = state->scl = 0;
	state->buffer = buffer;
	state->size = size;
	state->state = I2C_IDLE;
}

void set_host_sda(eeprom_state *state, uint8_t val)
{
	if (state->scl) {
		if (val & ~state->host_sda) {
			//low to high, stop condition
			state->state = I2C_IDLE;
			state->slave_sda = 1;
		} else if (~val & state->host_sda) {
			//high to low, start condition
			state->state = I2C_START;
			state->slave_sda = 1;
			state->counter = 8;
		}
	}
	state->host_sda = val;
}

void set_scl(eeprom_state *state, uint8_t val)
{
	if (val & ~state->scl) {
		//low to high transition
		switch (state->state)
		{
		case I2C_START:
		case I2C_ADDRESS_HI:
		case I2C_ADDRESS:
		case I2C_WRITE:
			state->latch = state->host_sda | state->latch << 1;
			state->counter--;
			if (!state->counter) {
				switch (state->state & 0x7F)
				{
				case I2C_START:
					state->state = I2C_DEVICE_ACK;
					break;
				case I2C_ADDRESS_HI:
					state->address = state->latch << 8;
					state->state = I2C_ADDRESS_HI_ACK;
					break;
				case I2C_ADDRESS:
					state->address |= state->latch;
					state->state = I2C_ADDRESS_ACK;
					break;
				case I2C_WRITE:
					state->buffer[state->address] = state->latch;
					state->state = I2C_WRITE_ACK;
					break;
				}
			}
			break;
		case I2C_DEVICE_ACK:
			if (state->latch & 1) {
				state->state = I2C_READ;
				state->counter = 8;
				if (state->size < 256) {
					state->address = state->latch >> 1;
				}
				state->latch = state->buffer[state->address];
			} else {
				if (state->size < 256) {
					state->address = state->latch >> 1;
					state->state = I2C_WRITE;
				} else if (state->size < 4096) {
					state->address = (state->latch & 0xE) << 7;
					state->state = I2C_ADDRESS;
				} else {
					state->state = I2C_ADDRESS_HI;
				}
				state->counter = 8;
			}
			break;
		case I2C_ADDRESS_HI_ACK:
			state->state = I2C_ADDRESS;
			state->counter = 8;
			break;
		case I2C_ADDRESS_ACK:
			state->state = I2C_WRITE;
			state->address &= state->size-1;
			state->counter = 8;
			break;
		case I2C_READ:
			state->counter--;
			if (!state->counter) {
				state->state = I2C_READ_ACK;
			}
			break;
		case I2C_READ_ACK:
			state->state = I2C_READ;
			state->counter = 8;
			state->address++;
			//TODO: page mask
			state->address &= state->size-1;
			state->latch = state->buffer[state->address];
			break;
		case I2C_WRITE_ACK:
			state->state = I2C_WRITE;
			state->counter = 8;
			state->address++;
			//TODO: page mask
			state->address &= state->size-1;
			break;
		}
	} else if (~val & state->scl) {
		//high to low transition
		switch (state->state & 0x7F)
		{
		case I2C_DEVICE_ACK:
		case I2C_ADDRESS_HI_ACK:
		case I2C_ADDRESS_ACK:
		case I2C_READ_ACK:
		case I2C_WRITE_ACK:
			state->slave_sda = 0;
			break;
		case I2C_READ:
			state->slave_sda = state->latch >> 7;
			state->latch = state->latch << 1;
			break;
		default:
			state->slave_sda = 1;
			break;
		}
	}
	state->scl = val;
}

uint8_t get_sda(eeprom_state *state)
{
	return state->host_sda & state->slave_sda;
}

enum {
	NOR_NORMAL,
	NOR_PRODUCTID,
	NOR_BOOTBLOCK
};

enum {
	NOR_CMD_IDLE,
	NOR_CMD_AA,
	NOR_CMD_55
};

//Technically this value shoudl be slightly different between NTSC and PAL
//as it's defined as 200 micro-seconds, not in clock cycles
#define NOR_WRITE_PAUSE 10690 

void nor_flash_init(nor_state *state, uint8_t *buffer, uint32_t size, uint32_t page_size, uint16_t product_id, uint8_t bus_flags)
{
	state->buffer = buffer;
	state->page_buffer = malloc(page_size);
	memset(state->page_buffer, 0xFF, page_size);
	state->size = size;
	state->page_size = page_size;
	state->product_id = product_id;
	state->last_write_cycle = 0xFFFFFFFF;
	state->mode = NOR_NORMAL;
	state->cmd_state = NOR_CMD_IDLE;
	state->alt_cmd = 0;
	state->bus_flags = bus_flags;
}

void nor_run(nor_state *state, uint32_t cycle)
{
	if (state->last_write_cycle == 0xFFFFFFFF) {
		return;
	}
	if (cycle - state->last_write_cycle >= NOR_WRITE_PAUSE) {
		state->last_write_cycle = 0xFFFFFFFF;
		for (uint32_t i = 0; i < state->page_size; i++) {
			state->buffer[state->current_page + i] = state->page_buffer[i];
		}
		memset(state->page_buffer, 0xFF, state->page_size);
	}
}

uint8_t nor_flash_read_b(uint32_t address, void *vcontext)
{
	m68k_context *m68k = vcontext;
	genesis_context *gen = m68k->system;
	nor_state *state = &gen->nor;
	if (
		((address & 1) && state->bus_flags == RAM_FLAG_EVEN) ||
		(!(address & 1) && state->bus_flags == RAM_FLAG_ODD)
	) {
		return 0xFF;
	}
	if (state->bus_flags != RAM_FLAG_BOTH) {
		address = address >> 1;
	}
	
	nor_run(state, m68k->current_cycle);
	switch (state->mode)
	{
	case NOR_NORMAL:
		return state->buffer[address & (state->size-1)];
		break;
	case NOR_PRODUCTID:
		switch (address & (state->size - 1))
		{
		case 0:
			return state->product_id >> 8;
		case 1:
			return state->product_id;
		case 2:
			//TODO: Implement boot block protection
			return 0xFE;
		default:
			return 0xFE;
		}
		break;
	case NOR_BOOTBLOCK:
		break;
	}
	return 0xFF;
}

uint16_t nor_flash_read_w(uint32_t address, void *context)
{
	uint16_t value = nor_flash_read_b(address, context) << 8;
	value |= nor_flash_read_b(address+1, context);
	return value;
}

void nor_write_byte(nor_state *state, uint32_t address, uint8_t value, uint32_t cycle)
{
	switch(state->mode)
	{
	case NOR_NORMAL:
		if (state->last_write_cycle != 0xFFFFFFFF) {
			state->current_page = address & (state->size - 1) & ~(state->page_size - 1);
		}
		state->page_buffer[address & (state->page_size - 1)] = value;
		break;
	case NOR_PRODUCTID:
		break;
	case NOR_BOOTBLOCK:
		//TODO: Implement boot block protection
		state->mode = NOR_NORMAL;
		break;
	}
}

void *nor_flash_write_b(uint32_t address, void *vcontext, uint8_t value)
{
	m68k_context *m68k = vcontext;
	genesis_context *gen = m68k->system;
	nor_state *state = &gen->nor;
	if (
		((address & 1) && state->bus_flags == RAM_FLAG_EVEN) ||
		(!(address & 1) && state->bus_flags == RAM_FLAG_ODD)
	) {
		return vcontext;
	}
	if (state->bus_flags != RAM_FLAG_BOTH) {
		address = address >> 1;
	}
	
	nor_run(state, m68k->current_cycle);
	switch (state->cmd_state)
	{
	case NOR_CMD_IDLE:
		if (value == 0xAA && (address & (state->size - 1)) == 0x5555) {
			state->cmd_state = NOR_CMD_AA;
		} else {
			nor_write_byte(state, address, value, m68k->current_cycle);
			state->cmd_state = NOR_CMD_IDLE;
		}
		break;
	case NOR_CMD_AA:
		if (value == 0x55 && (address & (state->size - 1)) == 0x2AAA) {
			state->cmd_state = NOR_CMD_55;
		} else {
			nor_write_byte(state, 0x5555, 0xAA, m68k->current_cycle);
			nor_write_byte(state, address, value, m68k->current_cycle);
			state->cmd_state = NOR_CMD_IDLE;
		}
		break;
	case NOR_CMD_55:
		if ((address & (state->size - 1)) == 0x5555) {
			if (state->alt_cmd) {
				switch(value)
				{
				case 0x10:
					puts("UNIMPLEMENTED: NOR flash erase");
					break;
				case 0x20:
					puts("UNIMPLEMENTED: NOR flash disable protection");
					break;
				case 0x40:
					state->mode = NOR_BOOTBLOCK;
					break;
				case 0x60:
					state->mode = NOR_PRODUCTID;
					break;
				}
			} else {
				switch(value)
				{
				case 0x80:
					state->alt_cmd = 1;
					break;
				case 0x90:
					state->mode = NOR_PRODUCTID;
					break;
				case 0xA0:
					puts("UNIMPLEMENTED: NOR flash enable protection");
					break;
				case 0xF0:
					state->mode = NOR_NORMAL;
					break;
				default:
					printf("Unrecognized unshifted NOR flash command %X\n", value);
				}
			}
		} else {
			nor_write_byte(state, 0x5555, 0xAA, m68k->current_cycle);
			nor_write_byte(state, 0x2AAA, 0x55, m68k->current_cycle);
			nor_write_byte(state, address, value, m68k->current_cycle);
		}
		state->cmd_state = NOR_CMD_IDLE;
		break;
	}
	return vcontext;
}

void *nor_flash_write_w(uint32_t address, void *vcontext, uint16_t value)
{
	nor_flash_write_b(address, vcontext, value >> 8);
	return nor_flash_write_b(address + 1, vcontext, value);
}

uint16_t read_sram_w(uint32_t address, m68k_context * context)
{
	genesis_context * gen = context->system;
	address &= gen->save_ram_mask;
	switch(gen->save_type)
	{
	case RAM_FLAG_BOTH:
		return gen->save_storage[address] << 8 | gen->save_storage[address+1];
	case RAM_FLAG_EVEN:
		return gen->save_storage[address >> 1] << 8 | 0xFF;
	case RAM_FLAG_ODD:
		return gen->save_storage[address >> 1] | 0xFF00;
	}
	return 0xFFFF;//We should never get here
}

uint8_t read_sram_b(uint32_t address, m68k_context * context)
{
	genesis_context * gen = context->system;
	address &= gen->save_ram_mask;
	switch(gen->save_type)
	{
	case RAM_FLAG_BOTH:
		return gen->save_storage[address];
	case RAM_FLAG_EVEN:
		if (address & 1) {
			return 0xFF;
		} else {
			return gen->save_storage[address >> 1];
		}
	case RAM_FLAG_ODD:
		if (address & 1) {
			return gen->save_storage[address >> 1];
		} else {
			return 0xFF;
		}
	}
	return 0xFF;//We should never get here
}

m68k_context * write_sram_area_w(uint32_t address, m68k_context * context, uint16_t value)
{
	genesis_context * gen = context->system;
	if ((gen->bank_regs[0] & 0x3) == 1) {
		address &= gen->save_ram_mask;
		switch(gen->save_type)
		{
		case RAM_FLAG_BOTH:
			gen->save_storage[address] = value >> 8;
			gen->save_storage[address+1] = value;
			break;
		case RAM_FLAG_EVEN:
			gen->save_storage[address >> 1] = value >> 8;
			break;
		case RAM_FLAG_ODD:
			gen->save_storage[address >> 1] = value;
			break;
		}
	}
	return context;
}

m68k_context * write_sram_area_b(uint32_t address, m68k_context * context, uint8_t value)
{
	genesis_context * gen = context->system;
	if ((gen->bank_regs[0] & 0x3) == 1) {
		address &= gen->save_ram_mask;
		switch(gen->save_type)
		{
		case RAM_FLAG_BOTH:
			gen->save_storage[address] = value;
			break;
		case RAM_FLAG_EVEN:
			if (!(address & 1)) {
				gen->save_storage[address >> 1] = value;
			}
			break;
		case RAM_FLAG_ODD:
			if (address & 1) {
				gen->save_storage[address >> 1] = value;
			}
			break;
		}
	}
	return context;
}

m68k_context * write_bank_reg_w(uint32_t address, m68k_context * context, uint16_t value)
{
	genesis_context * gen = context->system;
	address &= 0xE;
	address >>= 1;
	gen->bank_regs[address] = value;
	if (!address) {
		if (value & 1) {
			for (int i = 0; i < 8; i++)
			{
				context->mem_pointers[gen->mapper_start_index + i] = NULL;
			}
		} else {
			//Used for games that only use the mapper for SRAM
			context->mem_pointers[gen->mapper_start_index] = gen->cart + 0x200000/2;
			//For games that need more than 4MB
			for (int i = 1; i < 8; i++)
			{
				context->mem_pointers[gen->mapper_start_index + i] = gen->cart + 0x40000*gen->bank_regs[i];
			}
		}
	} else {
		void *new_ptr = gen->cart + 0x40000*value;
		if (context->mem_pointers[gen->mapper_start_index + address] != new_ptr) {
			m68k_invalidate_code_range(gen->m68k, address * 0x80000, (address + 1) * 0x80000);
			context->mem_pointers[gen->mapper_start_index + address] = new_ptr;
		}
	}
	return context;
}

m68k_context * write_bank_reg_b(uint32_t address, m68k_context * context, uint8_t value)
{
	if (address & 1) {
		write_bank_reg_w(address, context, value);
	}
	return context;
}
eeprom_map *find_eeprom_map(uint32_t address, genesis_context *gen)
{
	for (int i = 0; i < gen->num_eeprom; i++)
	{
		if (address >= gen->eeprom_map[i].start && address <= gen->eeprom_map[i].end) {
			return  gen->eeprom_map + i;
		}
	}
	return NULL;
}

void * write_eeprom_i2c_w(uint32_t address, void * context, uint16_t value)
{
	genesis_context *gen = ((m68k_context *)context)->system;
	eeprom_map *map = find_eeprom_map(address, gen);
	if (!map) {
		fatal_error("Could not find EEPROM map for address %X\n", address);
	}
	if (map->scl_mask) {
		set_scl(&gen->eeprom, (value & map->scl_mask) != 0);
	}
	if (map->sda_write_mask) {
		set_host_sda(&gen->eeprom, (value & map->sda_write_mask) != 0);
	}
	return context;
}

void * write_eeprom_i2c_b(uint32_t address, void * context, uint8_t value)
{
	genesis_context *gen = ((m68k_context *)context)->system;
	eeprom_map *map = find_eeprom_map(address, gen);
	if (!map) {
		fatal_error("Could not find EEPROM map for address %X\n", address);
	}

	uint16_t expanded, mask;
	if (address & 1) {
		expanded = value;
		mask = 0xFF;
	} else {
		expanded = value << 8;
		mask = 0xFF00;
	}
	if (map->scl_mask & mask) {
		set_scl(&gen->eeprom, (expanded & map->scl_mask) != 0);
	}
	if (map->sda_write_mask & mask) {
		set_host_sda(&gen->eeprom, (expanded & map->sda_write_mask) != 0);
	}
	return context;
}

uint16_t read_eeprom_i2c_w(uint32_t address, void * context)
{
	genesis_context *gen = ((m68k_context *)context)->system;
	eeprom_map *map = find_eeprom_map(address, gen);
	if (!map) {
		fatal_error("Could not find EEPROM map for address %X\n", address);
	}
	uint16_t ret = 0;
	if (map->sda_read_bit < 16) {
		ret = get_sda(&gen->eeprom) << map->sda_read_bit;
	}
	return ret;	
}

uint8_t read_eeprom_i2c_b(uint32_t address, void * context)
{
	genesis_context *gen = ((m68k_context *)context)->system;
	eeprom_map *map = find_eeprom_map(address, gen);
	if (!map) {
		fatal_error("Could not find EEPROM map for address %X\n", address);
	}
	uint8_t bit = address & 1 ? map->sda_read_bit : map->sda_read_bit - 8;
	uint8_t ret = 0;
	if (bit < 8) {
		ret = get_sda(&gen->eeprom) << bit;
	}
	return ret;
}

tern_node *load_rom_db()
{
	tern_node *db = parse_bundled_config("rom.db");
	if (!db) {
		fatal_error("Failed to load ROM DB\n");
	}
	return db;
}

char *get_header_name(uint8_t *rom)
{
	//TODO: Should probably prefer the title field that corresponds to the user's region preference
	uint8_t *last = rom + TITLE_END - 1;
	uint8_t *src = rom + TITLE_START;
	
	for (;;)
	{
		while (last > src && (*last <=  0x20 || *last >= 0x80))
		{
			last--;
		}
		if (last == src) {
			if (src == rom + TITLE_START) {
				src = rom + DOM_TITLE_START;
				last = rom + DOM_TITLE_END - 1;
			} else {
				return strdup("UNKNOWN");
			}
		} else {
			last++;
			char *ret = malloc(last - src + 1);
			uint8_t *dst;
			uint8_t last_was_space = 1;
			for (dst = ret; src < last; src++)
			{
				if (*src >= 0x20 && *src < 0x80) {
					if (*src == ' ') {
						if (last_was_space) {
							continue;
						}
						last_was_space = 1;
					} else {
						last_was_space = 0;
					}
					*(dst++) = *src;
				}
			}
			*dst = 0;
			return ret;
		}
	}
}

char *region_chars = "JUEW";
uint8_t region_bits[] = {REGION_J, REGION_U, REGION_E, REGION_J|REGION_U|REGION_E};

uint8_t translate_region_char(uint8_t c)
{	
	for (int i = 0; i < sizeof(region_bits); i++)
	{
		if (c == region_chars[i]) {
			return region_bits[i];
		}
	}
	uint8_t bin_region = 0;
	if (c >= '0' && c <= '9') {
		bin_region = c - '0';
	} else if (c >= 'A' && c <= 'F') {
		bin_region = c - 'A' + 0xA;
	} else if (c >= 'a' && c <= 'f') {
		bin_region = c - 'a' + 0xA;
	}
	uint8_t ret = 0;
	if (bin_region & 8) {
		ret |= REGION_E;
	}
	if (bin_region & 4) {
		ret |= REGION_U;
	}
	if (bin_region & 1) {
		ret |= REGION_J;
	}
	return ret;
}

uint8_t get_header_regions(uint8_t *rom)
{
	uint8_t regions = 0;
	for (int i = 0; i < 3; i++)
	{
		regions |= translate_region_char(rom[REGION_START + i]);
	}
	return regions;
}

uint32_t get_u32be(uint8_t *data)
{
	return *data << 24 | data[1] << 16 | data[2] << 8 | data[3];
}

uint32_t calc_mask(uint32_t src_size, uint32_t start, uint32_t end)
{
	uint32_t map_size = end-start+1;
	if (src_size < map_size) {
		return nearest_pow2(src_size)-1;
	} else if (!start) {
		return 0xFFFFFF;
	} else {
		return nearest_pow2(map_size)-1;
	}
}

void add_memmap_header(rom_info *info, uint8_t *rom, uint32_t size, memmap_chunk const *base_map, int base_chunks)
{
	uint32_t rom_end = get_u32be(rom + ROM_END) + 1;
	if (size > rom_end) {
		rom_end = size;
	} else if (rom_end > nearest_pow2(size)) {
		rom_end = nearest_pow2(size);
	}
	if (size >= 0x80000 && !memcmp("SEGA SSF", rom + 0x100, 8)) {
		info->mapper_start_index = 0;
		info->map_chunks = base_chunks + 9;
		info->map = malloc(sizeof(memmap_chunk) * info->map_chunks);
		memset(info->map, 0, sizeof(memmap_chunk)*9);
		memcpy(info->map+9, base_map, sizeof(memmap_chunk) * base_chunks);
		
		info->map[0].start = 0;
		info->map[0].end = 0x80000;
		info->map[0].mask = 0xFFFFFF;
		info->map[0].flags = MMAP_READ;
		info->map[0].buffer = rom;
		
		for (int i = 1; i < 8; i++)
		{
			info->map[i].start = i * 0x80000;
			info->map[i].end = (i + 1) * 0x80000;
			info->map[i].mask = 0x7FFFF;
			info->map[i].buffer = (i + 1) * 0x80000 <= size ? rom + i * 0x80000 : rom;
			info->map[i].ptr_index = i;
			info->map[i].flags = MMAP_READ | MMAP_PTR_IDX | MMAP_CODE;
			
		}
		info->map[8].start = 0xA13000;
		info->map[8].end = 0xA13100;
		info->map[8].mask = 0xFF;
		info->map[8].write_16 = (write_16_fun)write_bank_reg_w;
		info->map[8].write_8 = (write_8_fun)write_bank_reg_b;
	} else if (rom[RAM_ID] == 'R' && rom[RAM_ID+1] == 'A') {
		uint32_t ram_start = get_u32be(rom + RAM_START);
		uint32_t ram_end = get_u32be(rom + RAM_END);
		uint32_t ram_flags = info->save_type = rom[RAM_FLAGS] & RAM_FLAG_MASK;
		ram_start &= 0xFFFFFE;
		ram_end |= 1;
		info->save_mask = ram_end - ram_start;
		uint32_t save_size = info->save_mask + 1;
		if (ram_flags != RAM_FLAG_BOTH) {
			save_size /= 2;
		}
		info->save_size = save_size;
		info->save_buffer = malloc(save_size);

		info->map_chunks = base_chunks + (ram_start >= rom_end ? 2 : 3);
		info->map = malloc(sizeof(memmap_chunk) * info->map_chunks);
		memset(info->map, 0, sizeof(memmap_chunk)*2);
		memcpy(info->map+2, base_map, sizeof(memmap_chunk) * base_chunks);

		if (ram_start >= rom_end) {
			info->map[0].end = rom_end < 0x400000 ? nearest_pow2(rom_end) - 1 : 0xFFFFFF;
			if (info->map[0].end > ram_start) {
				info->map[0].end = ram_start;
			}
			//TODO: ROM mirroring
			info->map[0].mask = 0xFFFFFF;
			info->map[0].flags = MMAP_READ;
			info->map[0].buffer = rom;

			info->map[1].start = ram_start;
			info->map[1].mask = info->save_mask;
			info->map[1].end = ram_end + 1;
			info->map[1].flags = MMAP_READ | MMAP_WRITE;

			if (ram_flags == RAM_FLAG_ODD) {
				info->map[1].flags |= MMAP_ONLY_ODD;
			} else if (ram_flags == RAM_FLAG_EVEN) {
				info->map[1].flags |= MMAP_ONLY_EVEN;
			}
			info->map[1].buffer = info->save_buffer;
		} else {
			//Assume the standard Sega mapper
			info->map[0].end = 0x200000;
			info->map[0].mask = 0xFFFFFF;
			info->map[0].flags = MMAP_READ;
			info->map[0].buffer = rom;

			info->map[1].start = 0x200000;
			info->map[1].end = 0x400000;
			info->map[1].mask = 0x1FFFFF;
			info->map[1].flags = MMAP_READ | MMAP_PTR_IDX | MMAP_FUNC_NULL;
			info->map[1].ptr_index = info->mapper_start_index = 0;
			info->map[1].read_16 = (read_16_fun)read_sram_w;//these will only be called when mem_pointers[2] == NULL
			info->map[1].read_8 = (read_8_fun)read_sram_b;
			info->map[1].write_16 = (write_16_fun)write_sram_area_w;//these will be called all writes to the area
			info->map[1].write_8 = (write_8_fun)write_sram_area_b;
			info->map[1].buffer = rom + 0x200000;

			memmap_chunk *last = info->map + info->map_chunks - 1;
			memset(last, 0, sizeof(memmap_chunk));
			last->start = 0xA13000;
			last->end = 0xA13100;
			last->mask = 0xFF;
			last->write_16 = (write_16_fun)write_bank_reg_w;
			last->write_8 = (write_8_fun)write_bank_reg_b;
		}
	} else {
		info->map_chunks = base_chunks + 1;
		info->map = malloc(sizeof(memmap_chunk) * info->map_chunks);
		memset(info->map, 0, sizeof(memmap_chunk));
		memcpy(info->map+1, base_map, sizeof(memmap_chunk) * base_chunks);

		info->map[0].end = rom_end > 0x400000 ? rom_end : 0x400000;
		info->map[0].mask = rom_end < 0x400000 ? nearest_pow2(rom_end) - 1 : 0xFFFFFF;
		info->map[0].flags = MMAP_READ;
		info->map[0].buffer = rom;
		info->save_type = SAVE_NONE;
	}
}

rom_info configure_rom_heuristics(uint8_t *rom, uint32_t rom_size, memmap_chunk const *base_map, uint32_t base_chunks)
{
	rom_info info;
	info.name = get_header_name(rom);
	info.regions = get_header_regions(rom);
	add_memmap_header(&info, rom, rom_size, base_map, base_chunks);
	info.port1_override = info.port2_override = info.ext_override = info.mouse_mode = NULL;
	return info;
}

typedef struct {
	rom_info     *info;
	uint8_t      *rom;
	uint8_t      *lock_on;
	tern_node    *root;
	uint32_t     rom_size;
	uint32_t     lock_on_size;
	int          index;
	int          num_els;
	uint16_t     ptr_index;
} map_iter_state;

void eeprom_read_fun(char *key, tern_val val, uint8_t valtype, void *data)
{
	int bit = atoi(key);
	if (bit < 0 || bit > 15) {
		fprintf(stderr, "bit %s is out of range", key);
		return;
	}
	if (valtype != TVAL_PTR) {
		fprintf(stderr, "bit %s has a non-scalar value", key);
		return;
	}
	char *pin = val.ptrval;
	if (strcmp(pin, "sda")) {
		fprintf(stderr, "bit %s is connected to unrecognized read pin %s", key, pin);
		return;
	}
	eeprom_map *map = data;
	map->sda_read_bit = bit;
}

void eeprom_write_fun(char *key, tern_val val, uint8_t valtype, void *data)
{
	int bit = atoi(key);
	if (bit < 0 || bit > 15) {
		fprintf(stderr, "bit %s is out of range", key);
		return;
	}
	if (valtype != TVAL_PTR) {
		fprintf(stderr, "bit %s has a non-scalar value", key);
		return;
	}
	char *pin = val.ptrval;
	eeprom_map *map = data;
	if (!strcmp(pin, "sda")) {
		map->sda_write_mask = 1 << bit;
		return;
	}
	if (!strcmp(pin, "scl")) {
		map->scl_mask = 1 << bit;
		return;
	}
	fprintf(stderr, "bit %s is connected to unrecognized write pin %s", key, pin);
}

void process_sram_def(char *key, map_iter_state *state)
{
	if (!state->info->save_size) {
		char * size = tern_find_path(state->root, "SRAM\0size\0", TVAL_PTR).ptrval;
		if (!size) {
			fatal_error("ROM DB map entry %d with address %s has device type SRAM, but the SRAM size is not defined\n", state->index, key);
		}
		state->info->save_size = atoi(size);
		if (!state->info->save_size) {
			fatal_error("SRAM size %s is invalid\n", size);
		}
		state->info->save_mask = nearest_pow2(state->info->save_size)-1;
		state->info->save_buffer = malloc(state->info->save_size);
		memset(state->info->save_buffer, 0, state->info->save_size);
		char *bus = tern_find_path(state->root, "SRAM\0bus\0", TVAL_PTR).ptrval;
		if (!strcmp(bus, "odd")) {
			state->info->save_type = RAM_FLAG_ODD;
		} else if(!strcmp(bus, "even")) {
			state->info->save_type = RAM_FLAG_EVEN;
		} else {
			state->info->save_type = RAM_FLAG_BOTH;
		}
	}
}

void process_eeprom_def(char * key, map_iter_state *state)
{
	if (!state->info->save_size) {
		char * size = tern_find_path(state->root, "EEPROM\0size\0", TVAL_PTR).ptrval;
		if (!size) {
			fatal_error("ROM DB map entry %d with address %s has device type EEPROM, but the EEPROM size is not defined\n", state->index, key);
		}
		state->info->save_size = atoi(size);
		if (!state->info->save_size) {
			fatal_error("EEPROM size %s is invalid\n", size);
		}
		char *etype = tern_find_path(state->root, "EEPROM\0type\0", TVAL_PTR).ptrval;
		if (!etype) {
			etype = "i2c";
		}
		if (!strcmp(etype, "i2c")) {
			state->info->save_type = SAVE_I2C;
		} else {
			fatal_error("EEPROM type %s is invalid\n", etype);
		}
		state->info->save_buffer = malloc(state->info->save_size);
		memset(state->info->save_buffer, 0xFF, state->info->save_size);
		state->info->eeprom_map = malloc(sizeof(eeprom_map) * state->num_els);
		memset(state->info->eeprom_map, 0, sizeof(eeprom_map) * state->num_els);
	}
}

void process_nor_def(char *key, map_iter_state *state)
{
	if (!state->info->save_size) {
		char *size = tern_find_path(state->root, "NOR\0size\0", TVAL_PTR).ptrval;
		if (!size) {
			fatal_error("ROM DB map entry %d with address %s has device type NOR, but the NOR size is not defined\n", state->index, key);
		}
		state->info->save_size = atoi(size);
		if (!state->info->save_size) {
			fatal_error("NOR size %s is invalid\n", size);
		}
		char *page_size = tern_find_path(state->root, "NOR\0page_size\0", TVAL_PTR).ptrval;
		if (!page_size) {
			fatal_error("ROM DB map entry %d with address %s has device type NOR, but the NOR page size is not defined\n", state->index, key);
		}
		state->info->save_page_size = atoi(size);
		if (!state->info->save_page_size) {
			fatal_error("NOR page size %s is invalid\n", size);
		}
		char *product_id = tern_find_path(state->root, "NOR\0product_id\0", TVAL_PTR).ptrval;
		if (!product_id) {
			fatal_error("ROM DB map entry %d with address %s has device type NOR, but the NOR product ID is not defined\n", state->index, key);
		}
		state->info->save_product_id = strtol(product_id, NULL, 16);
		char *bus = tern_find_path(state->root, "NOR\0bus\0", TVAL_PTR).ptrval;
		if (!strcmp(bus, "odd")) {
			state->info->save_bus = RAM_FLAG_ODD;
		} else if(!strcmp(bus, "even")) {
			state->info->save_bus = RAM_FLAG_EVEN;
		} else {
			state->info->save_bus = RAM_FLAG_BOTH;
		}
		state->info->save_type = SAVE_NOR;
		state->info->save_buffer = malloc(state->info->save_size);
		memset(state->info->save_buffer, 0xFF, state->info->save_size);
	}
}

void add_eeprom_map(tern_node *node, uint32_t start, uint32_t end, map_iter_state *state)
{
	eeprom_map *eep_map = state->info->eeprom_map + state->info->num_eeprom;
	eep_map->start = start;
	eep_map->end = end;
	eep_map->sda_read_bit = 0xFF;
	tern_node * bits_read = tern_find_node(node, "bits_read");
	if (bits_read) {
		tern_foreach(bits_read, eeprom_read_fun, eep_map);
	}
	tern_node * bits_write = tern_find_node(node, "bits_write");
	if (bits_write) {
		tern_foreach(bits_write, eeprom_write_fun, eep_map);
	}
	printf("EEPROM address %X: sda read: %X, sda write: %X, scl: %X\n", start, eep_map->sda_read_bit, eep_map->sda_write_mask, eep_map->scl_mask);
	state->info->num_eeprom++;
}

void map_iter_fun(char *key, tern_val val, uint8_t valtype, void *data)
{
	map_iter_state *state = data;
	if (valtype != TVAL_NODE) {
		fatal_error("ROM DB map entry %d with address %s is not a node\n", state->index, key);
	}
	tern_node *node = val.ptrval;
	uint32_t start = strtol(key, NULL, 16);
	uint32_t end = strtol(tern_find_ptr_default(node, "last", "0"), NULL, 16);
	if (!end || end < start) {
		fatal_error("'last' value is missing or invalid for ROM DB map entry %d with address %s\n", state->index, key);
	}
	char * dtype = tern_find_ptr_default(node, "device", "ROM");
	uint32_t offset = strtol(tern_find_ptr_default(node, "offset", "0"), NULL, 16);
	memmap_chunk *map = state->info->map + state->index;
	map->start = start;
	map->end = end + 1;
	if (!strcmp(dtype, "ROM")) {
		map->buffer = state->rom + offset;
		map->flags = MMAP_READ;
		map->mask = calc_mask(state->rom_size - offset, start, end);
	} else if (!strcmp(dtype, "LOCK-ON")) {
		if (state->lock_on) {
			if (state->lock_on_size > offset) {
				map->mask = calc_mask(state->lock_on_size - offset, start, end);
			} else {
				map->mask = calc_mask(state->lock_on_size, start, end);
			}
			map->buffer = state->lock_on + (offset & (nearest_pow2(state->lock_on_size) - 1));
		} else if (state->rom_size > start) {
			//This is a bit of a hack to deal with pre-combined S3&K/S2&K ROMs and S&K ROM hacks
			map->buffer = state->rom + start;
			map->mask = calc_mask(state->rom_size - start, start, end);
		} else {
			//skip this entry if there is no lock on cartridge attached
			return;
		}
		map->flags = MMAP_READ;
	} else if (!strcmp(dtype, "EEPROM")) {
		process_eeprom_def(key, state);
		add_eeprom_map(node, start, end, state);

		map->write_16 = write_eeprom_i2c_w;
		map->write_8 = write_eeprom_i2c_b;
		map->read_16 = read_eeprom_i2c_w;
		map->read_8 = read_eeprom_i2c_b;
		map->mask = 0xFFFFFF;
	} else if (!strcmp(dtype, "SRAM")) {
		process_sram_def(key, state);
		map->buffer = state->info->save_buffer + offset;
		map->flags = MMAP_READ | MMAP_WRITE;
		if (state->info->save_type == RAM_FLAG_ODD) {
			map->flags |= MMAP_ONLY_ODD;
		} else if(state->info->save_type == RAM_FLAG_EVEN) {
			map->flags |= MMAP_ONLY_EVEN;
		}
		map->mask = calc_mask(state->info->save_size, start, end);
	} else if (!strcmp(dtype, "RAM")) {
		uint32_t size = strtol(tern_find_ptr_default(node, "size", "0"), NULL, 16);
		if (!size || size > map->end - map->start) {
			size = map->end - map->start;
		}
		map->buffer = malloc(size);
		map->mask = calc_mask(size, start, end);
		map->flags = MMAP_READ | MMAP_WRITE;
		char *bus = tern_find_ptr_default(node, "bus", "both");
		if (!strcmp(bus, "odd")) {
			map->flags |= MMAP_ONLY_ODD;
		} else if (!strcmp(bus, "even")) {
			map->flags |= MMAP_ONLY_EVEN;
		} else {
			map->flags |= MMAP_CODE;
		}
	} else if (!strcmp(dtype, "NOR")) {
		process_nor_def(key, state);
		
		map->write_16 = nor_flash_write_w;
		map->write_8 = nor_flash_write_b;
		map->read_16 = nor_flash_read_w;
		map->read_8 = nor_flash_read_b;
		map->mask = 0xFFFFFF;
	} else if (!strcmp(dtype, "Sega mapper")) {
		state->info->mapper_start_index = state->ptr_index++;
		state->info->map_chunks+=7;
		state->info->map = realloc(state->info->map, sizeof(memmap_chunk) * state->info->map_chunks);
		memset(state->info->map + state->info->map_chunks - 7, 0, sizeof(memmap_chunk) * 7);
		map = state->info->map + state->index;
		char *save_device = tern_find_path(node, "save\0device\0", TVAL_PTR).ptrval;
		if (save_device && !strcmp(save_device, "EEPROM")) {
			process_eeprom_def(key, state);
			add_eeprom_map(node, start & map->mask, end & map->mask, state);
		}
		for (int i = 0; i < 7; i++, state->index++, map++)
		{
			map->start = start + i * 0x80000;
			map->end = start + (i + 1) * 0x80000;
			map->mask = 0x7FFFF;
			map->buffer = state->rom + offset + i * 0x80000;
			map->ptr_index = state->ptr_index++;
			if (i < 3 || !save_device) {
				map->flags = MMAP_READ | MMAP_PTR_IDX | MMAP_CODE;
			} else {
				map->flags = MMAP_READ | MMAP_PTR_IDX | MMAP_CODE | MMAP_FUNC_NULL;
				if (!strcmp(save_device, "SRAM")) {
					process_sram_def(key, state);
					map->read_16 = (read_16_fun)read_sram_w;//these will only be called when mem_pointers[2] == NULL
					map->read_8 = (read_8_fun)read_sram_b;
					map->write_16 = (write_16_fun)write_sram_area_w;//these will be called all writes to the area
					map->write_8 = (write_8_fun)write_sram_area_b;
				} else if (!strcmp(save_device, "EEPROM")) {
					map->write_16 = write_eeprom_i2c_w;
					map->write_8 = write_eeprom_i2c_b;
					map->read_16 = read_eeprom_i2c_w;
					map->read_8 = read_eeprom_i2c_b;
				}
			}
		}
		map->start = 0xA13000;
		map->end = 0xA13100;
		map->mask = 0xFF;
		map->write_16 = (write_16_fun)write_bank_reg_w;
		map->write_8 = (write_8_fun)write_bank_reg_b;
	} else if (!strcmp(dtype, "MENU")) {
		//fake hardware for supporting menu
		map->buffer = NULL;
		map->mask = 0xFF;
		map->write_16 = menu_write_w;
		map->read_16 = menu_read_w;
	} else if (!strcmp(dtype, "fixed")) {
		uint16_t *value =  malloc(2);
		map->buffer = value;
		map->mask = 0;
		map->flags = MMAP_READ;
		*value = strtol(tern_find_ptr_default(node, "value", "0"), NULL, 16);
	} else {
		fatal_error("Invalid device type %s for ROM DB map entry %d with address %s\n", dtype, state->index, key);
	}
	state->index++;
}

rom_info configure_rom(tern_node *rom_db, void *vrom, uint32_t rom_size, void *lock_on, uint32_t lock_on_size, memmap_chunk const *base_map, uint32_t base_chunks)
{
	uint8_t product_id[GAME_ID_LEN+1];
	uint8_t *rom = vrom;
	product_id[GAME_ID_LEN] = 0;
	for (int i = 0; i < GAME_ID_LEN; i++)
	{
		if (rom[GAME_ID_OFF + i] <= ' ') {
			product_id[i] = 0;
			break;
		}
		product_id[i] = rom[GAME_ID_OFF + i];

	}
	printf("Product ID: %s\n", product_id);
	uint8_t raw_hash[20];
	sha1(vrom, rom_size, raw_hash);
	uint8_t hex_hash[41];
	bin_to_hex(hex_hash, raw_hash, 20);
	printf("SHA1: %s\n", hex_hash);
	tern_node * entry = tern_find_node(rom_db, hex_hash);
	if (!entry) {
		entry = tern_find_node(rom_db, product_id);
	}
	if (!entry) {
		puts("Not found in ROM DB, examining header\n");
		if (xband_detect(rom, rom_size)) {
			return xband_configure_rom(rom_db, rom, rom_size, lock_on, lock_on_size, base_map, base_chunks);
		}
		if (realtec_detect(rom, rom_size)) {
			return realtec_configure_rom(rom, rom_size, base_map, base_chunks);
		}
		return configure_rom_heuristics(rom, rom_size, base_map, base_chunks);
	}
	rom_info info;
	info.name = tern_find_ptr(entry, "name");
	if (info.name) {
		printf("Found name: %s\n", info.name);
		info.name = strdup(info.name);
	} else {
		info.name = get_header_name(rom);
	}

	char *dbreg = tern_find_ptr(entry, "regions");
	info.regions = 0;
	if (dbreg) {
		while (*dbreg != 0)
		{
			info.regions |= translate_region_char(*(dbreg++));
		}
	}
	if (!info.regions) {
		info.regions = get_header_regions(rom);
	}

	tern_node *map = tern_find_node(entry, "map");
	if (map) {
		info.save_type = SAVE_NONE;
		info.map_chunks = tern_count(map);
		if (info.map_chunks) {
			info.map_chunks += base_chunks;
			info.save_buffer = NULL;
			info.save_size = 0;
			info.map = malloc(sizeof(memmap_chunk) * info.map_chunks);
			info.eeprom_map = NULL;
			info.num_eeprom = 0;
			memset(info.map, 0, sizeof(memmap_chunk) * info.map_chunks);
			map_iter_state state = {
				.info = &info, 
				.rom = rom, 
				.lock_on = lock_on,
				.root = entry, 
				.rom_size = rom_size, 
				.lock_on_size = lock_on_size,
				.index = 0, 
				.num_els = info.map_chunks - base_chunks,
				.ptr_index = 0
			};
			tern_foreach(map, map_iter_fun, &state);
			memcpy(info.map + state.index, base_map, sizeof(memmap_chunk) * base_chunks);
		} else {
			add_memmap_header(&info, rom, rom_size, base_map, base_chunks);
		}
	} else {
		add_memmap_header(&info, rom, rom_size, base_map, base_chunks);
	}

	tern_node *device_overrides = tern_find_node(entry, "device_overrides");
	if (device_overrides) {
		info.port1_override = tern_find_ptr(device_overrides, "1");
		info.port2_override = tern_find_ptr(device_overrides, "2");
		info.ext_override = tern_find_ptr(device_overrides, "ext");
	} else {
		info.port1_override = info.port2_override = info.ext_override = NULL;
	}
	info.mouse_mode = tern_find_ptr(entry, "mouse_mode");

	return info;
}
