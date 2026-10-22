/* Pokewalker infrared: IrDA SIR, 115200 baud 8N1, bytes XORed with 0xAA.
 *
 * The Stick S3 has no IrDA hardware, so stickwalker samples a photodiode on a
 * dedicated core. The Flipper's STM32WB55 does: USART1 has an IrDA SIR
 * encoder/decoder, which pairs with an IrDA transceiver such as the Vishay
 * TFBS4711 or TFDU4101:
 *   pin 13 (USART1 TX, PB6) -> transceiver TXD (active-high 3/16-bit pulses)
 *   pin 14 (USART1 RX, PB7) <- transceiver RXD (active-low pulses)
 * Transceivers echo their own transmission on RXD, so reception is ignored
 * while sending and briefly afterwards. TXD drives the LED while high, so
 * pin 13 is held low whenever the USART is not using it.
 *
 * Bytes are timestamped in the receive interrupt. As on the Stick, a quiet
 * gap ends a burst, and src/support/ir.c receives one burst per packet. */
#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_resources.h>
#include <expansion/expansion.h>
#include <stm32wbxx_ll_usart.h>

#include "flip_port.h"

#include "pw_begin.h"
#include "ir_transport.h"
#include "sound_bridge.h"

#define IR_BAUD        115200u
#define IR_XOR         0xaau
#define RING_SIZE      512u
#define BURST_GAP_US   350u
#define MAX_FRAME      136u
#define TICK_HZ        32768u
/* The transceiver's echo of our last byte can finish just after the USART
 * reports transmission complete; a reply from the DS takes far longer. */
#define ECHO_GUARD_US  100u
#define TAG "flip_ir"

static FuriHalSerialHandle* serial;
static Expansion* expansion;
static bool rx_running;
static volatile bool failed;
static volatile bool overflow;
static volatile bool transmitting;
static volatile uint32_t echo_guard_end;

static uint8_t ring_bytes[RING_SIZE];
static uint32_t ring_cycles[RING_SIZE];
static volatile uint32_t ring_head;
static uint32_t ring_tail;

static const char* status_text = "idle";

/* Link health for the status LED: the last clean burst and the last sign of
 * trouble, as cycle counts. There is no signal strength on an IrDA link, so
 * "poor" means the bytes are not coming through cleanly. */
static volatile bool session;
static volatile uint32_t last_good_cycles;
static volatile uint32_t last_bad_cycles;
static volatile bool trouble_seen;

/* Transceiver probe: while probing, our own echo is counted instead of
 * being thrown away. */
static volatile bool probing;
static volatile unsigned probe_bytes;
static const char* name_text = "not probed";

static void rx_callback(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* context) {
    UNUSED(context);
    const bool echo = transmitting || (int32_t)(DWT->CYCCNT - echo_guard_end) < 0;
    /* A pulse train that does not decode as a byte: misalignment, distance
     * or sunlight. Our own echo is not counted. */
    if(!echo && (event & (FuriHalSerialRxEventFrameError | FuriHalSerialRxEventNoiseError |
                          FuriHalSerialRxEventOverrunError))) {
        last_bad_cycles = DWT->CYCCNT;
        trouble_seen = true;
    }
    if(!(event & FuriHalSerialRxEventData)) return;
    while(furi_hal_serial_async_rx_available(handle)) {
        const uint8_t value = furi_hal_serial_async_rx(handle);
        if(probing) {
            ++probe_bytes;
            continue;
        }
        /* IrDA is half duplex; ignore our own echo. */
        if(echo) continue;
        const uint32_t head = ring_head;
        if(head - ring_tail >= RING_SIZE) {
            overflow = true;
            continue;
        }
        ring_bytes[head % RING_SIZE] = value;
        ring_cycles[head % RING_SIZE] = DWT->CYCCNT;
        ring_head = head + 1;
    }
}

static uint32_t tx_psc = 1;

/* IrDA must be reconfigured with the USART disabled. Normal mode emits
 * 3/16-bit pulses, 1.63 us at 115200 baud: the bottom of the TFBS4711's
 * 1.6 us input range, so the optical pulses come out weak. Low-power mode
 * emits 3 periods of a prescaled clock instead, so a prescaler for about
 * 1.5 MHz gives 2 us pulses with margin inside IrDA's 1.41-2.23 us window.
 * Low-power mode decodes incoming pulses differently, though, and the DS's
 * replies come out garbled in it, so it is only switched on to transmit. */
static void set_irda_mode(bool low_power) {
    LL_USART_Disable(USART1);
    LL_USART_SetIrdaPrescaler(USART1, low_power ? tx_psc : 1);
    LL_USART_SetIrdaPowerMode(USART1, low_power ? LL_USART_IRDA_POWER_LOW : LL_USART_IRDA_POWER_NORMAL);
    LL_USART_EnableIrda(USART1);
    /* The 8-byte transmit FIFO keeps the frame flowing if the thread is
     * briefly preempted; the walker's protocol needs each packet sent as
     * one unbroken burst. */
    LL_USART_EnableFIFO(USART1);
    LL_USART_Enable(USART1);
    while(!LL_USART_IsActiveFlag_TEACK(USART1) || !LL_USART_IsActiveFlag_REACK(USART1)) {
    }
}

static void enable_irda(void) {
    /* The USART clock is recovered from the baud register. */
    const uint32_t usart_clock = (USART1->BRR & 0xFFFFu) * IR_BAUD;
    tx_psc = (usart_clock + 750000u) / 1500000u;
    if(tx_psc < 1) tx_psc = 1;
    if(tx_psc > 255) tx_psc = 255;
    FURI_LOG_I(TAG, "tx pulse %lu ns (psc %lu, usart clock %lu Hz)",
               (unsigned long)(3000000000ull * tx_psc / usart_clock), (unsigned long)tx_psc,
               (unsigned long)usart_clock);
    set_irda_mode(false);
    /* The transceiver drives RXD; the pull-up only keeps an unconnected pin
     * from floating. */
    furi_hal_gpio_init_ex(
        &gpio_usart_rx, GpioModeAltFunctionPushPull, GpioPullUp, GpioSpeedVeryHigh,
        GpioAltFn7USART1);
}

static void hold_tx_low(void) {
    /* Keep a transceiver's LED off while the USART does not own pin 13. */
    furi_hal_gpio_write(&gpio_usart_tx, false);
    furi_hal_gpio_init(&gpio_usart_tx, GpioModeOutputPushPull, GpioPullNo, GpioSpeedLow);
}

void StickIrInitPins(void) {
    hold_tx_low();
}

void StickIrConfigure(void) {
    failed = false;
    if(serial) return;
    /* An expansion module would otherwise own the USART on pins 13/14. */
    expansion = furi_record_open(RECORD_EXPANSION);
    expansion_disable(expansion);
    serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    if(!serial) {
        expansion_enable(expansion);
        furi_record_close(RECORD_EXPANSION);
        expansion = NULL;
        failed = true;
        status_text = "USART busy";
        return;
    }
    furi_hal_serial_init(serial, IR_BAUD);
    enable_irda();
    status_text = "ready";
}

void StickIrStart(void) {
    StickSoundQuiesceForIr();
    if(!serial) {
        failed = true;
        return;
    }
    ring_tail = ring_head;
    overflow = false;
    transmitting = false;
    echo_guard_end = DWT->CYCCNT;
    if(!rx_running) {
        furi_hal_serial_async_rx_start(serial, rx_callback, NULL, true);
        rx_running = true;
    }
    status_text = "connecting";
    last_good_cycles = last_bad_cycles = DWT->CYCCNT;
    trouble_seen = false;
    session = true;
}

void StickIrStop(void) {
    session = false;
    if(serial) {
        if(rx_running) furi_hal_serial_async_rx_stop(serial);
        rx_running = false;
        furi_hal_serial_deinit(serial);
        furi_hal_serial_control_release(serial);
        serial = NULL;
        hold_tx_low();
    }
    if(expansion) {
        expansion_enable(expansion);
        furi_record_close(RECORD_EXPANSION);
        expansion = NULL;
    }
    status_text = failed ? "failed" : "idle";
}

void flip_ir_shutdown(void) {
    StickIrStop();
    /* Return the pin to its default state for other apps. */
    furi_hal_gpio_init(&gpio_usart_tx, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
}

const char* flip_ir_status(void) {
    return status_text;
}

void flip_ir_probe(void) {
    if(session) return;
    StickIrConfigure();
    if(!serial) {
        name_text = "USART busy";
        return;
    }
    /* A transceiver drives RXD high all the time it is idle (the TFBS4711
     * sources 200 uA there); an empty header does not drive it at all. So
     * lean on the pin with the weak internal pull-down and see who wins.
     * No infrared is emitted. */
    furi_hal_gpio_init(&gpio_usart_rx, GpioModeInput, GpioPullDown, GpioSpeedLow);
    furi_delay_ms(2);
    unsigned high = 0;
    for(unsigned i = 0; i < 8; ++i) {
        if(furi_hal_gpio_read(&gpio_usart_rx)) ++high;
        furi_delay_us(200);
    }
    const bool present = high == 8;
    furi_hal_gpio_init_ex(
        &gpio_usart_rx, GpioModeAltFunctionPushPull, GpioPullUp, GpioSpeedVeryHigh,
        GpioAltFn7USART1);
    name_text = present ? "transceiver found" : "no transceiver";
    FURI_LOG_I(TAG, "probe: rx high %u/8, %s", high, name_text);
    StickIrStop();
}

const char* flip_ir_name(void) {
    return name_text;
}

FlipIrLink flip_ir_link(void) {
    if(!session) return FlipIrLinkNone;
    const uint32_t now = DWT->CYCCNT;
    const uint32_t per_ms = furi_hal_cortex_instructions_per_microsecond() * 1000u;
    if(trouble_seen && now - last_bad_cycles < 700u * per_ms) return FlipIrLinkPoor;
    /* Packets flow every few milliseconds in a healthy session; a second of
     * silence means the DS is out of range or not pointed at us. */
    if(now - last_good_cycles < 1000u * per_ms) return FlipIrLinkGood;
    return FlipIrLinkPoor;
}

static u16 ticks_from_us(uint64_t us) {
    return (u16)((us * TICK_HZ) / 1000000u);
}

u16 StickIrTicks(void) {
    return ticks_from_us(flip_now_us());
}

void StickIrDelayTicks(u16 ticks) {
    const u16 start = StickIrTicks();
    while((u16)(StickIrTicks() - start) < ticks) {
    }
}

void StickIrSendFrame(const u8* logical, u8 length) {
    uint8_t wire[MAX_FRAME];
    if(!serial || !logical || !length || length > MAX_FRAME) {
        failed = true;
        return;
    }
    for(unsigned i = 0; i < length; ++i)
        wire[i] = logical[i] ^ IR_XOR;
    transmitting = true;
    set_irda_mode(true);
    /* Feed the FIFO ourselves at raised priority so no other thread can
     * open a gap inside the frame; a gap makes the DS drop the packet. */
    const FuriThreadPriority prio = furi_thread_get_current_priority();
    furi_thread_set_current_priority(FuriThreadPriorityHighest);
    for(unsigned i = 0; i < length; ++i) {
        while(!LL_USART_IsActiveFlag_TXE_TXFNF(USART1)) {
        }
        LL_USART_TransmitData8(USART1, wire[i]);
    }
    while(!LL_USART_IsActiveFlag_TC(USART1)) {
    }
    furi_thread_set_current_priority(prio);
    /* Our own echo may have overrun the receiver meanwhile; clear it so
     * reception resumes cleanly. */
    while(LL_USART_IsActiveFlag_RXNE_RXFNE(USART1)) {
        (void)LL_USART_ReceiveData8(USART1);
        if(probing) ++probe_bytes;
    }
    LL_USART_ClearFlag_ORE(USART1);
    set_irda_mode(false);
    echo_guard_end = DWT->CYCCNT + ECHO_GUARD_US * furi_hal_cortex_instructions_per_microsecond();
    transmitting = false;
    FURI_LOG_D(TAG, "tx %u: %02x %02x %02x %02x", length, logical[0],
               length > 1 ? logical[1] : 0, length > 2 ? logical[2] : 0, length > 3 ? logical[3] : 0);
}

void StickIrSendByte(u8 logical) {
    StickIrSendFrame(&logical, 1);
}

int StickIrTakeBurst(u8* wire, u8 capacity, u8* length, u16* lastObservationTick) {
    const uint32_t head = ring_head;
    if(head == ring_tail) return 0;
    const uint32_t gap_cycles = BURST_GAP_US * furi_hal_cortex_instructions_per_microsecond();

    /* Find the end of the oldest burst. */
    uint32_t end = ring_tail + 1;
    while(end != head && (uint32_t)(ring_cycles[end % RING_SIZE] -
                                    ring_cycles[(end - 1) % RING_SIZE]) < gap_cycles)
        ++end;
    const uint32_t last_cycles = ring_cycles[(end - 1) % RING_SIZE];
    /* Still arriving: wait for the line to go quiet. */
    if(end == head && (uint32_t)(DWT->CYCCNT - last_cycles) < gap_cycles) return 0;

    const uint32_t count = end - ring_tail;
    if(count > capacity) {
        FURI_LOG_E(TAG, "rx burst %lu > %u", (unsigned long)count, capacity);
        ring_tail = end;
        failed = true;
        status_text = "frame too long";
        last_bad_cycles = DWT->CYCCNT;
        trouble_seen = true;
        return 0;
    }
    /* Every real packet carries an 8-byte header; anything shorter is noise
     * that happened to decode. */
    if(count >= 8 && !overflow) last_good_cycles = last_cycles;
    else {
        last_bad_cycles = DWT->CYCCNT;
        trouble_seen = true;
    }
    for(uint32_t i = 0; i < count; ++i)
        wire[i] = ring_bytes[(ring_tail + i) % RING_SIZE];
    ring_tail = end;
    FURI_LOG_D(TAG, "rx %lu: %02x %02x %02x %02x%s", (unsigned long)count, wire[0] ^ IR_XOR,
               count > 1 ? wire[1] ^ IR_XOR : 0, count > 2 ? wire[2] ^ IR_XOR : 0,
               count > 3 ? wire[3] ^ IR_XOR : 0, overflow ? " OVERFLOW" : "");
    *length = (u8)count;
    *lastObservationTick = ticks_from_us(flip_cycles_to_us(last_cycles));
    status_text = "receiving";
    return 1;
}

int StickIrFailed(void) {
    if(overflow) {
        failed = true;
        status_text = "overflow";
    }
    return failed;
}
