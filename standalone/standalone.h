#ifndef SWR_STANDALONE_H
#define SWR_STANDALONE_H

#include <stdint.h>
#include <stdio.h>

extern FILE* standalone_log;

void standalone_write_jmp(uint32_t retail_addr, uint32_t target);
void standalone_install_bodies(void);
void standalone_install_compat(void);
int standalone_retail_text_loaded(void);
uint32_t standalone_retail_of(uint32_t body);
uint32_t standalone_body_of(uint32_t retail);
// Whether 0x401000-0x4ac000 holds jumps to our bodies. The plain standalone needs no retail code
// address; the feature build (its deltas call game functions through *_ADDR) and retail bisecting
// (SWR_STANDALONE_RETAIL_TEXT) still do.
int standalone_code_range_mapped(void);

#endif
