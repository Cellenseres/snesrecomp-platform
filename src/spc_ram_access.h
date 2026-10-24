#ifndef SNESRECOMP_SPC_RAM_ACCESS_H
#define SNESRECOMP_SPC_RAM_ACCESS_H

/* Keep MMIO, IPL overlay and diagnostic writes on the authoritative bus. */
static inline uint8_t snesrecomp_spc_read_ram(Apu *apu, uint16_t address) {
    if ((address & 0xfff0u) != 0x00f0u &&
        (!apu->romReadable || address < 0xffc0u)) return apu->ram[address];
    return apu_cpuRead(apu, address);
}

static inline void snesrecomp_spc_write_ram(Apu *apu, uint16_t address, uint8_t value) {
#if !SNESRECOMP_TRACE && !SNESRECOMP_SPC_DIAGNOSTICS
    if ((address & 0xfff0u) != 0x00f0u) {
        apu->ram[address] = value;
        return;
    }
#endif
    apu_cpuWrite(apu, address, value);
}
#endif
