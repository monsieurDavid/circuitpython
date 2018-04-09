/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2017 Scott Shawcroft for Adafruit Industries
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "shared_dma.h"

#include <string.h>

#include "py/gc.h"
#include "py/mpstate.h"

#include "hal/utils/include/utils.h"

#include "shared-bindings/microcontroller/__init__.h"

// We allocate three DMA resources for the entire lifecycle of the board (not the
// vm) because the general_dma resource will be shared between the REPL and SPI
// flash. Both uses must block each other in order to prevent conflict.
COMPILER_ALIGNED(16) static DmacDescriptor dma_descriptors[3];

// Don't use these directly. They are used by the DMA engine itself.
COMPILER_ALIGNED(16) static DmacDescriptor write_back_descriptors[3];

#define AUDIO_DMA_CHANNEL 0
#define SHARED_TX_CHANNEL 1
#define SHARED_RX_CHANNEL 2


#ifdef SAMD21
#define FIRST_SERCOM_RX_TRIGSRC 0x01
#define FIRST_SERCOM_TX_TRIGSRC 0x02
#endif
#ifdef SAMD51
#define FIRST_SERCOM_RX_TRIGSRC 0x04
#define FIRST_SERCOM_TX_TRIGSRC 0x05
#endif

// static void dma_configure_audio(uint8_t channel) {
//     system_interrupt_enter_critical_section();
//     /** Select the DMA channel and clear software trigger */
//     DMAC->CHID.reg = DMAC_CHID_ID(channel);
//     DMAC->CHCTRLA.reg &= ~DMAC_CHCTRLA_ENABLE;
//     DMAC->CHCTRLA.reg = DMAC_CHCTRLA_SWRST;
//     DMAC->SWTRIGCTRL.reg &= (uint32_t)(~(1 << channel));
//     uint32_t event_output_enable = 0;
//     if (output_event) {
//         event_output_enable = DMAC_CHCTRLB_EVOE;
//     }
//     DMAC->CHCTRLB.reg = DMAC_CHCTRLB_LVL(DMA_PRIORITY_LEVEL_0) |
//             DMAC_CHCTRLB_TRIGSRC(trigsrc) |
//             DMAC_CHCTRLB_TRIGACT(DMA_TRIGGER_ACTION_BEAT) |
//             event_output_enable;
//     // config.peripheral_trigger = DAC_DMAC_ID_EMPTY;
//     // config.trigger_action = DMA_TRIGGER_ACTION_BEAT;
//     // config.event_config.input_action = DMA_EVENT_INPUT_TRIG;
//     // config.event_config.event_output_enable = true;
//     system_interrupt_leave_critical_section();
// }

void init_shared_dma(void) {
    // Turn on the clocks
    #ifdef SAMD51
    MCLK->AHBMASK.reg |= MCLK_AHBMASK_DMAC;
    #endif

    #ifdef SAMD21
    PM->AHBMASK.reg |= PM_AHBMASK_DMAC;
    PM->APBBMASK.reg |= PM_APBBMASK_DMAC;
    #endif

    DMAC->CTRL.reg = DMAC_CTRL_SWRST;

    DMAC->BASEADDR.reg = (uint32_t) dma_descriptors;
    DMAC->WRBADDR.reg = (uint32_t) write_back_descriptors;

    DMAC->CTRL.reg = DMAC_CTRL_DMAENABLE | DMAC_CTRL_LVLEN0;

    // This allocates the lowest channel first so make sure the audio is first
    // so it gets the highest priority.
    // dma_configure_audio(0);
}

static uint8_t sercom_index(Sercom* sercom) {
    #ifdef SAMD21
    return ((uint32_t) sercom - (uint32_t) SERCOM0) / 0x400;
    #else
    const Sercom* sercoms[SERCOM_INST_NUM] = SERCOM_INSTS;
    for (uint8_t i = 0; i < SERCOM_INST_NUM; i++) {
        if (sercoms[i] == sercom) {
            return i;
        }
    }
    return 0;
    #endif
}

static void dma_configure(uint8_t channel_number, uint8_t trigsrc, bool output_event) {
    #ifdef SAMD21
    common_hal_mcu_disable_interrupts();
    /** Select the DMA channel and clear software trigger */
    DMAC->CHID.reg = DMAC_CHID_ID(channel_number);
    DMAC->CHCTRLA.reg &= ~DMAC_CHCTRLA_ENABLE;
    DMAC->CHCTRLA.reg = DMAC_CHCTRLA_SWRST;
    DMAC->SWTRIGCTRL.reg &= (uint32_t)(~(1 << channel_number));
    uint32_t event_output_enable = 0;
    if (output_event) {
        event_output_enable = DMAC_CHCTRLB_EVOE;
    }
    DMAC->CHCTRLB.reg = DMAC_CHCTRLB_LVL_LVL0 |
            DMAC_CHCTRLB_TRIGSRC(trigsrc) |
            DMAC_CHCTRLB_TRIGACT_BEAT |
            event_output_enable;
    common_hal_mcu_enable_interrupts();
    #endif

    #ifdef SAMD51
    DmacChannel* channel = &DMAC->Channel[channel_number];
    channel->CHCTRLA.reg &= ~DMAC_CHCTRLA_ENABLE;
    channel->CHCTRLA.reg = DMAC_CHCTRLA_SWRST;
    if (output_event) {
        channel->CHEVCTRL.reg = DMAC_CHEVCTRL_EVOE;
    }
    channel->CHCTRLA.reg = DMAC_CHCTRLA_TRIGSRC(trigsrc) |
                           DMAC_CHCTRLA_TRIGACT_BURST |
                           DMAC_CHCTRLA_BURSTLEN_SINGLE;
    #endif
}

static void enable_channel(uint8_t channel_number) {
    #ifdef SAMD21
    common_hal_mcu_disable_interrupts();
    /** Select the DMA channel and clear software trigger */
    DMAC->CHID.reg = DMAC_CHID_ID(channel_number);
    DMAC->CHCTRLA.bit.ENABLE = true;
    common_hal_mcu_enable_interrupts();
    #endif

    #ifdef SAMD51
    DmacChannel* channel = &DMAC->Channel[channel_number];
    channel->CHCTRLA.bit.ENABLE = true;
    #endif
}

static uint8_t transfer_status(uint8_t channel_number) {
    #ifdef SAMD21
    common_hal_mcu_disable_interrupts();
    /** Select the DMA channel and clear software trigger */
    DMAC->CHID.reg = DMAC_CHID_ID(channel_number);
    uint8_t status = DMAC->CHINTFLAG.reg;
    common_hal_mcu_enable_interrupts();
    return status;
    #endif

    #ifdef SAMD51
    DmacChannel* channel = &DMAC->Channel[channel_number];
    return channel->CHINTFLAG.reg;
    #endif
}

static bool channel_free(uint8_t channel_number) {
    #ifdef SAMD21
    common_hal_mcu_disable_interrupts();
    /** Select the DMA channel and clear software trigger */
    DMAC->CHID.reg = DMAC_CHID_ID(channel_number);
    bool channel_free = DMAC->CHSTATUS.reg == 0;
    common_hal_mcu_enable_interrupts();
    return channel_free;
    #endif

    #ifdef SAMD51
    DmacChannel* channel = &DMAC->Channel[channel_number];
    return channel->CHSTATUS.reg == 0;
    #endif
}

// Do write and read simultaneously. If buffer_out is NULL, write the tx byte over and over.
// If buffer_out is a real buffer, ignore tx.
// DMAs buffer_out -> dest
// DMAs src -> buffer_in
static int32_t shared_dma_transfer(void* peripheral,
                                   const uint8_t* buffer_out, volatile uint32_t* dest,
                                   volatile uint32_t* src, uint8_t* buffer_in,
                                   uint32_t length, uint8_t tx) {
    if (!channel_free(SHARED_TX_CHANNEL) ||
        (buffer_in != NULL && !channel_free(SHARED_RX_CHANNEL))) {
        return -1;
    }

    uint32_t beat_size = DMAC_BTCTRL_BEATSIZE_BYTE;
    bool sercom = true;
    bool tx_active = false;
    bool rx_active = false;
    uint16_t beat_length = length;
    #ifdef SAMD51
    if (peripheral == QSPI) {
        // Check input alignment on word boundaries.
        if ((((uint32_t) buffer_in) & 0x3) != 0 ||
            (((uint32_t) buffer_out) & 0x3) != 0) {
            return -3;
        }
        beat_size = DMAC_BTCTRL_BEATSIZE_WORD | DMAC_BTCTRL_SRCINC | DMAC_BTCTRL_DSTINC;
        beat_length /= 4;
        sercom = false;
        if (buffer_out != NULL) {
            dma_configure(SHARED_TX_CHANNEL, QSPI_DMAC_ID_TX, false);
            tx_active = true;
        } else {
            dma_configure(SHARED_RX_CHANNEL, QSPI_DMAC_ID_RX, false);
            rx_active = true;
        }

    } else {
    #endif

        // sercom index is incorrect for SAMD51
        dma_configure(SHARED_TX_CHANNEL, sercom_index(peripheral) * 2 + FIRST_SERCOM_TX_TRIGSRC, false);
        tx_active = true;
        if (buffer_in != NULL) {
            dma_configure(SHARED_RX_CHANNEL, sercom_index(peripheral) * 2 + FIRST_SERCOM_RX_TRIGSRC, false);
            rx_active = true;
        }

    #ifdef SAMD51
    }
    #endif

    // Set up RX first.
    if (rx_active) {
        DmacDescriptor* rx_descriptor = &dma_descriptors[SHARED_RX_CHANNEL];
        rx_descriptor->BTCTRL.reg = beat_size | DMAC_BTCTRL_DSTINC;
        rx_descriptor->BTCNT.reg = beat_length;
        rx_descriptor->SRCADDR.reg = ((uint32_t) src);
        #ifdef SAMD51
        if (peripheral == QSPI) {
            rx_descriptor->SRCADDR.reg = ((uint32_t) src + length);
        }
        #endif
        rx_descriptor->DSTADDR.reg = ((uint32_t)buffer_in + length);
        rx_descriptor->BTCTRL.bit.VALID = true;
    }

    // Set up TX second.
    if (tx_active) {
        DmacDescriptor* tx_descriptor = &dma_descriptors[SHARED_TX_CHANNEL];
        tx_descriptor->BTCTRL.reg = beat_size;
        tx_descriptor->BTCNT.reg = beat_length;

        if (buffer_out != NULL) {
            tx_descriptor->SRCADDR.reg = ((uint32_t)buffer_out + length);
            tx_descriptor->BTCTRL.reg |= DMAC_BTCTRL_SRCINC;
        } else {
            tx_descriptor->SRCADDR.reg = ((uint32_t) &tx);
        }
        tx_descriptor->DSTADDR.reg = ((uint32_t) dest);
        tx_descriptor->BTCTRL.bit.VALID = true;
    }
    if (sercom) {
        SercomSpi *s = &((Sercom*) peripheral)->SPI;
        s->INTFLAG.reg = SERCOM_SPI_INTFLAG_RXC | SERCOM_SPI_INTFLAG_DRE;
    } else {
        //QSPI->INTFLAG.reg = QSPI_INTFLAG_RXC | QSPI_INTFLAG_DRE;
    }
    // Start the RX job first so we don't miss the first byte. The TX job clocks
    // the output.
    if (rx_active) {
        enable_channel(SHARED_RX_CHANNEL);
    }
    if (tx_active) {
        enable_channel(SHARED_TX_CHANNEL);
    }


    if (sercom) {
        //DMAC->SWTRIGCTRL.reg |= (1 << SHARED_TX_CHANNEL);
    } else {
        // Do a manual copy to trigger then DMA. We do 32-bit accesses to match the DMA.
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wcast-align"
        if (rx_active) {
            //buffer_in[0] = *src;
            DMAC->SWTRIGCTRL.reg |= (1 << SHARED_RX_CHANNEL);
        } else {
            //*(uint32_t*)dest = ((uint32_t*) buffer_out)[0];
        }
        #pragma GCC diagnostic pop
    }

    // Channels cycle between Suspend -> Pending -> Busy and back while transfering. So, we check
    // the channels transfer status for an error or completion.
    if (rx_active) {
        while ((transfer_status(SHARED_RX_CHANNEL) & 0x3) == 0) {}
    }
    if (tx_active) {
        while ((transfer_status(SHARED_TX_CHANNEL) & 0x3) == 0) {}
    }

    if (sercom) {
        Sercom* s = (Sercom*) peripheral;
        // Wait for the SPI transfer to complete.
        while (s->SPI.INTFLAG.bit.TXC == 0) {}

        // This transmit will cause the RX buffer overflow but we're OK with that.
        // So, read the garbage and clear the overflow flag.
        if (!rx_active) {
            while (s->SPI.INTFLAG.bit.RXC == 1) {
                s->SPI.DATA.reg;
            }
            s->SPI.STATUS.bit.BUFOVF = 1;
            s->SPI.INTFLAG.reg = SERCOM_SPI_INTFLAG_ERROR;
        }
    }

    if ((!rx_active || transfer_status(SHARED_RX_CHANNEL) == DMAC_CHINTFLAG_TCMPL) &&
        (!tx_active || transfer_status(SHARED_TX_CHANNEL) == DMAC_CHINTFLAG_TCMPL)) {
        return length;
    }
    return -2;
}


int32_t sercom_dma_transfer(Sercom* sercom, const uint8_t* buffer_out, uint8_t* buffer_in,
                            uint32_t length) {
    return shared_dma_transfer(sercom, buffer_out, &sercom->SPI.DATA.reg, &sercom->SPI.DATA.reg, buffer_in, length, 0);
}

int32_t sercom_dma_write(Sercom* sercom, const uint8_t* buffer, uint32_t length) {
    return shared_dma_transfer(sercom, buffer, &sercom->SPI.DATA.reg, NULL, NULL, length, 0);
}

int32_t sercom_dma_read(Sercom* sercom, uint8_t* buffer, uint32_t length, uint8_t tx) {
    return shared_dma_transfer(sercom, NULL, &sercom->SPI.DATA.reg, &sercom->SPI.DATA.reg, buffer, length, tx);
}

#ifdef SAMD51
int32_t qspi_dma_write(uint32_t address, const uint8_t* buffer, uint32_t length) {
    return shared_dma_transfer(QSPI, buffer, (uint32_t*) (QSPI_AHB + address), NULL, NULL, length, 0);
}

int32_t qspi_dma_read(uint32_t address, uint8_t* buffer, uint32_t length) {
    return shared_dma_transfer(QSPI, NULL, NULL, (uint32_t*) (QSPI_AHB + address), buffer, length, 0);
}
#endif

bool allocate_block_counter() {
//     // Find a timer to count DMA block completions.
//     Tc *t = NULL;
//     Tc *tcs[TC_INST_NUM] = TC_INSTS;
//     for (uint8_t i = TC_INST_NUM; i > 0; i--) {
//         if (tcs[i - 1]->COUNT16.CTRLA.bit.ENABLE == 0) {
//             t = tcs[i - 1];
//             break;
//         }
//     }
//     if (t == NULL) {
//         return false;
//     }
//     MP_STATE_VM(audiodma_block_counter) = gc_alloc(sizeof(struct tc_module), false);
//     if (MP_STATE_VM(audiodma_block_counter) == NULL) {
//         return false;
//     }
//
//     // Don't bother setting the period. We set it before you playback anything.
//     struct tc_config config_tc;
//     tc_get_config_defaults(&config_tc);
//     config_tc.counter_size    = TC_COUNTER_SIZE_16BIT;
//     config_tc.clock_prescaler = TC_CLOCK_PRESCALER_DIV1;
//     if (tc_init(MP_STATE_VM(audiodma_block_counter), t, &config_tc) != STATUS_OK) {
//         return false;
//     };
//
//     struct tc_events events_tc;
//     events_tc.generate_event_on_overflow = false;
//     events_tc.on_event_perform_action = true;
//     events_tc.event_action = TC_EVENT_ACTION_INCREMENT_COUNTER;
//     tc_enable_events(MP_STATE_VM(audiodma_block_counter), &events_tc);
//
//     // Connect the timer overflow event, which happens at the target frequency,
//     // to the DAC conversion trigger.
//     MP_STATE_VM(audiodma_block_event) = gc_alloc(sizeof(struct events_resource), false);
//     if (MP_STATE_VM(audiodma_block_event) == NULL) {
//         return false;
//     }
//     struct events_config config;
//     events_get_config_defaults(&config);
//
//     uint8_t user = EVSYS_ID_USER_TC3_EVU;
//     if (t == TC4) {
//         user = EVSYS_ID_USER_TC4_EVU;
//     } else if (t == TC5) {
//         user = EVSYS_ID_USER_TC5_EVU;
// #ifdef TC6
//     } else if (t == TC6) {
//         user = EVSYS_ID_USER_TC6_EVU;
// #endif
// #ifdef TC7
//     } else if (t == TC7) {
//         user = EVSYS_ID_USER_TC7_EVU;
// #endif
//     }
//
//     config.generator    = EVSYS_ID_GEN_DMAC_CH_0;
//     config.path         = EVENTS_PATH_ASYNCHRONOUS;
//     if (events_allocate(MP_STATE_VM(audiodma_block_event), &config) != STATUS_OK ||
//         events_attach_user(MP_STATE_VM(audiodma_block_event), user) != STATUS_OK) {
//         return false;
//     }
//
//     tc_enable(MP_STATE_VM(audiodma_block_counter));
//     tc_stop_counter(MP_STATE_VM(audiodma_block_counter));
    return true;
}

void switch_audiodma_trigger(uint8_t trigger_dmac_id) {
    //dma_configure(audio_dma.channel_id, trigger_dmac_id, true);
}
