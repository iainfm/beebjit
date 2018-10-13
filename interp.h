#ifndef BEEBJIT_INTERP_H
#define BEEBJIT_INTERP_H

struct bbc_options;
struct interp_struct;
struct state_6502;

struct interp_struct* interp_create(struct state_6502* p_state_6502,
                                    unsigned char* p_mem,
                                    struct bbc_options* p_options);
void interp_destroy(struct interp_struct* p_interp);

void interp_enter(struct interp_struct* p_interp);

#endif /* BEEBJIT_INTERP_H */