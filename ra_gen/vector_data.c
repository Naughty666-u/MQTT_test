/* generated vector source file - do not edit */
        #include "bsp_api.h"
        /* Do not build these data structures if no interrupts are currently allocated because IAR will have build errors. */
        #if VECTOR_DATA_IRQ_COUNT > 0
        BSP_DONT_REMOVE const fsp_vector_t g_vector_table[BSP_ICU_VECTOR_NUM_ENTRIES] BSP_PLACE_IN_SECTION(BSP_SECTION_APPLICATION_VECTORS) =
        {
                        [0] = sci_uart_rxi_isr, /* SCI0 RXI (Receive data full) */
            [1] = sci_uart_txi_isr, /* SCI0 TXI (Transmit data empty) */
            [2] = sci_uart_tei_isr, /* SCI0 TEI (Transmit end) */
            [3] = sci_uart_eri_isr, /* SCI0 ERI (Receive error) */
            [4] = sci_uart_rxi_isr, /* SCI2 RXI (Receive data full) */
            [5] = sci_uart_txi_isr, /* SCI2 TXI (Transmit data empty) */
            [6] = sci_uart_tei_isr, /* SCI2 TEI (Transmit end) */
            [7] = sci_uart_eri_isr, /* SCI2 ERI (Receive error) */
            [8] = gpt_counter_overflow_isr, /* GPT0 COUNTER OVERFLOW (Overflow) */
            [9] = sci_uart_rxi_isr, /* SCI3 RXI (Receive data full) */
            [10] = sci_uart_txi_isr, /* SCI3 TXI (Transmit data empty) */
            [11] = sci_uart_tei_isr, /* SCI3 TEI (Transmit end) */
            [12] = sci_uart_eri_isr, /* SCI3 ERI (Receive error) */
            [13] = r_icu_isr, /* ICU IRQ6 (External pin interrupt 6) */
            [14] = r_icu_isr, /* ICU IRQ7 (External pin interrupt 7) */
        };
        #if BSP_FEATURE_ICU_HAS_IELSR
        const bsp_interrupt_event_t g_interrupt_event_link_select[BSP_ICU_VECTOR_NUM_ENTRIES] =
        {
            [0] = BSP_PRV_VECT_ENUM(EVENT_SCI0_RXI,GROUP0), /* SCI0 RXI (Receive data full) */
            [1] = BSP_PRV_VECT_ENUM(EVENT_SCI0_TXI,GROUP1), /* SCI0 TXI (Transmit data empty) */
            [2] = BSP_PRV_VECT_ENUM(EVENT_SCI0_TEI,GROUP2), /* SCI0 TEI (Transmit end) */
            [3] = BSP_PRV_VECT_ENUM(EVENT_SCI0_ERI,GROUP3), /* SCI0 ERI (Receive error) */
            [4] = BSP_PRV_VECT_ENUM(EVENT_SCI2_RXI,GROUP4), /* SCI2 RXI (Receive data full) */
            [5] = BSP_PRV_VECT_ENUM(EVENT_SCI2_TXI,GROUP5), /* SCI2 TXI (Transmit data empty) */
            [6] = BSP_PRV_VECT_ENUM(EVENT_SCI2_TEI,GROUP6), /* SCI2 TEI (Transmit end) */
            [7] = BSP_PRV_VECT_ENUM(EVENT_SCI2_ERI,GROUP7), /* SCI2 ERI (Receive error) */
            [8] = BSP_PRV_VECT_ENUM(EVENT_GPT0_COUNTER_OVERFLOW,GROUP0), /* GPT0 COUNTER OVERFLOW (Overflow) */
            [9] = BSP_PRV_VECT_ENUM(EVENT_SCI3_RXI,GROUP1), /* SCI3 RXI (Receive data full) */
            [10] = BSP_PRV_VECT_ENUM(EVENT_SCI3_TXI,GROUP2), /* SCI3 TXI (Transmit data empty) */
            [11] = BSP_PRV_VECT_ENUM(EVENT_SCI3_TEI,GROUP3), /* SCI3 TEI (Transmit end) */
            [12] = BSP_PRV_VECT_ENUM(EVENT_SCI3_ERI,GROUP4), /* SCI3 ERI (Receive error) */
            [13] = BSP_PRV_VECT_ENUM(EVENT_ICU_IRQ6,GROUP5), /* ICU IRQ6 (External pin interrupt 6) */
            [14] = BSP_PRV_VECT_ENUM(EVENT_ICU_IRQ7,GROUP6), /* ICU IRQ7 (External pin interrupt 7) */
        };
        #endif
        #endif