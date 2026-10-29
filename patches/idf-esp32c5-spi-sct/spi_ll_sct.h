/*------------------------------------------------------------------------------
 * Segmented-Configure-Transfer
 *----------------------------------------------------------------------------*/
#define SPI_LL_CONF_BUF_SET_BIT(_w, _m)  ({                                                                                        \
            (_w) |= (_m);                                                                       \
        })
#define SPI_LL_CONF_BUF_CLR_BIT(_w, _m)  ({                                                                                        \
            (_w) &= ~(_m);                                                                      \
        })

#define SPI_LL_CONF_BUF_SET_FIELD(_w, _f, val) ({                                                                                   \
            ((_w) = (((_w) & ~((_f##_V) << (_f##_S))) | (((val) & (_f##_V))<<(_f##_S))));                \
        })

#define SPI_LL_CONF_BUF_GET_FIELD(_w, _f) ({                                                                                       \
            (((_w) >> (_f##_S)) & (_f##_V));                                                                   \
        })

//This offset is 1, for bitmap
#define SPI_LL_CONF_BUFFER_OFFSET               (1)
//bitmap must be the first
#define SPI_LL_CONF_BITMAP_POS                  (0)

#define SPI_LL_ADDR_REG_POS                     (0)
#define SPI_LL_CTRL_REG_POS                     (1)
#define SPI_LL_CLOCK_REG_POS                    (2)
#define SPI_LL_USER_REG_POS                     (3)
#define SPI_LL_USER1_REG_POS                    (4)
#define SPI_LL_USER2_REG_POS                    (5)
#define SPI_LL_MS_DLEN_REG_POS                  (6)
#define SPI_LL_MISC_REG_POS                     (7)
#define SPI_LL_DIN_MODE_REG_POS                 (8)
#define SPI_LL_DIN_NUM_REG_POS                  (9)
#define SPI_LL_DOUT_MODE_REG_POS                (10)
#define SPI_LL_DMA_CONF_REG_POS                 (11)
#define SPI_LL_DMA_INT_ENA_REG_POS              (12)
#define SPI_LL_DMA_INT_CLR_REG_POS              (13)

#define SPI_LL_SCT_MAGIC_NUMBER                 (0x2)

/**
 * Set conf phase bits len to HW for segment config trans mode.
 *
 * @param hw Beginning address of the peripheral registers.
 * @param conf_bitlen Value of field conf_bitslen in cmd reg.
 */
static inline void spi_ll_set_conf_phase_bits_len(spi_dev_t *hw, uint32_t conf_bitlen)
{
    if (conf_bitlen <= SOC_SPI_SCT_CONF_BITLEN_MAX) {
        hw->cmd.conf_bitlen = conf_bitlen;
    }
}

/**
 * Update the conf buffer for conf phase
 *
 * @param hw Beginning address of the peripheral registers.
 * @param is_end Is this transaction the end of this segment.
 * @param conf_buffer Conf buffer to be updated.
 */
static inline void spi_ll_format_conf_phase_conf_buffer(spi_dev_t *hw, bool is_end, uint32_t conf_buffer[SOC_SPI_SCT_BUFFER_NUM_MAX])
{
    //user reg: usr_conf_nxt
    if (is_end) {
        SPI_LL_CONF_BUF_CLR_BIT(conf_buffer[SPI_LL_USER_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_USR_CONF_NXT_M);
    } else {
        SPI_LL_CONF_BUF_SET_BIT(conf_buffer[SPI_LL_USER_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_USR_CONF_NXT_M);
    }
}

/**
 * Update the line mode of conf buffer for conf phase
 *
 * @param hw Beginning address of the peripheral registers.
 * @param line_mode line mode struct of each phase.
 * @param conf_buffer Conf buffer to be updated.
 */
static inline void spi_ll_format_line_mode_conf_buff(spi_dev_t *hw, spi_line_mode_t line_mode, uint32_t conf_buffer[SOC_SPI_SCT_BUFFER_NUM_MAX])
{
    conf_buffer[SPI_LL_CTRL_REG_POS + SPI_LL_CONF_BUFFER_OFFSET] &= ~SPI_LL_ONE_LINE_CTRL_MASK;
    conf_buffer[SPI_LL_USER_REG_POS + SPI_LL_CONF_BUFFER_OFFSET] &= ~SPI_LL_ONE_LINE_USER_MASK;

    switch (line_mode.cmd_lines) {
    case 2: SPI_LL_CONF_BUF_SET_BIT(conf_buffer[SPI_LL_CTRL_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_FCMD_DUAL_M); break;
    case 4: SPI_LL_CONF_BUF_SET_BIT(conf_buffer[SPI_LL_CTRL_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_FCMD_QUAD_M); break;
    default: break;
    }

    switch (line_mode.addr_lines) {
    case 2: SPI_LL_CONF_BUF_SET_BIT(conf_buffer[SPI_LL_CTRL_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_FADDR_DUAL_M); break;
    case 4: SPI_LL_CONF_BUF_SET_BIT(conf_buffer[SPI_LL_CTRL_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_FADDR_QUAD_M); break;
    default: break;
    }

    switch (line_mode.data_lines) {
    case 2: SPI_LL_CONF_BUF_SET_BIT(conf_buffer[SPI_LL_CTRL_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_FREAD_DUAL_M);
        SPI_LL_CONF_BUF_SET_BIT(conf_buffer[SPI_LL_USER_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_FWRITE_DUAL_M);
        break;
    case 4: SPI_LL_CONF_BUF_SET_BIT(conf_buffer[SPI_LL_CTRL_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_FREAD_QUAD_M);
        SPI_LL_CONF_BUF_SET_BIT(conf_buffer[SPI_LL_USER_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_FWRITE_QUAD_M);
        break;
    default: break;
    }
}

/**
 * Update the conf buffer for prep phase
 *
 * @param hw Beginning address of the peripheral registers.
 * @param setup CS setup time
 * @param conf_buffer Conf buffer to be updated.
 */
static inline void spi_ll_format_prep_phase_conf_buffer(spi_dev_t *hw, uint8_t setup, uint32_t conf_buffer[SOC_SPI_SCT_BUFFER_NUM_MAX])
{
    //user reg: cs_setup
    if (setup) {
        SPI_LL_CONF_BUF_SET_BIT(conf_buffer[SPI_LL_USER_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_CS_SETUP_M);
    } else {
        SPI_LL_CONF_BUF_CLR_BIT(conf_buffer[SPI_LL_USER_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_CS_SETUP_M);
    }

    //user1 reg: cs_setup_time
    SPI_LL_CONF_BUF_SET_FIELD(conf_buffer[SPI_LL_USER1_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_CS_SETUP_TIME, setup - 1);
}

/**
 * Update the conf buffer for cmd phase
 *
 * @param hw Beginning address of the peripheral registers.
 * @param cmd Command value
 * @param cmdlen Length of the cmd phase
 * @param lsbfirst Whether LSB first
 * @param conf_buffer Conf buffer to be updated.
 */
static inline void spi_ll_format_cmd_phase_conf_buffer(spi_dev_t *hw, uint16_t cmd, int cmdlen, bool lsbfirst, uint32_t conf_buffer[SOC_SPI_SCT_BUFFER_NUM_MAX])
{
    //user reg: usr_command
    if (cmdlen) {
        SPI_LL_CONF_BUF_SET_BIT(conf_buffer[SPI_LL_USER_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_USR_COMMAND_M);
    } else {
        SPI_LL_CONF_BUF_CLR_BIT(conf_buffer[SPI_LL_USER_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_USR_COMMAND_M);
    }

    //user2 reg: usr_command_bitlen
    SPI_LL_CONF_BUF_SET_FIELD(conf_buffer[SPI_LL_USER2_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_USR_COMMAND_BITLEN, cmdlen - 1);

    //user2 reg: usr_command_value
    if (lsbfirst) {
        SPI_LL_CONF_BUF_SET_FIELD(conf_buffer[SPI_LL_USER2_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_USR_COMMAND_VALUE, cmd);
    } else {
        SPI_LL_CONF_BUF_SET_FIELD(conf_buffer[SPI_LL_USER2_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_USR_COMMAND_VALUE, HAL_SPI_SWAP_DATA_TX(cmd, cmdlen));
    }
}

/**
 * Update the conf buffer for addr phase
 *
 * @param hw Beginning address of the peripheral registers.
 * @param addr Address to set
 * @param addrlen Length of the address phase
 * @param lsbfirst whether the LSB first feature is enabled.
 * @param conf_buffer Conf buffer to be updated.
 */
static inline void spi_ll_format_addr_phase_conf_buffer(spi_dev_t *hw, uint64_t addr, int addrlen, bool lsbfirst, uint32_t conf_buffer[SOC_SPI_SCT_BUFFER_NUM_MAX])
{
    //user reg: usr_addr
    if (addrlen) {
        SPI_LL_CONF_BUF_SET_BIT(conf_buffer[SPI_LL_USER_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_USR_ADDR_M);
    } else {
        SPI_LL_CONF_BUF_CLR_BIT(conf_buffer[SPI_LL_USER_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_USR_ADDR_M);
    }

    //user1 reg: usr_addr_bitlen
    SPI_LL_CONF_BUF_SET_FIELD(conf_buffer[SPI_LL_USER1_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_USR_ADDR_BITLEN, addrlen - 1);

    //addr reg: addr
    if (lsbfirst) {
        SPI_LL_CONF_BUF_SET_FIELD(conf_buffer[SPI_LL_ADDR_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_USR_ADDR_VALUE, HAL_SWAP32(addr));
    } else {
        SPI_LL_CONF_BUF_SET_FIELD(conf_buffer[SPI_LL_ADDR_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_USR_ADDR_VALUE, (addr << (32 - addrlen)));
    }
}

/**
 * Update the conf buffer for dummy phase
 *
 * @param hw Beginning address of the peripheral registers.
 * @param dummy_n Dummy cycles used. 0 to disable the dummy phase.
 * @param conf_buffer Conf buffer to be updated.
 */
static inline void spi_ll_format_dummy_phase_conf_buffer(spi_dev_t *hw, int dummy_n, uint32_t conf_buffer[SOC_SPI_SCT_BUFFER_NUM_MAX])
{
    //user reg: usr_dummy
    if (dummy_n) {
        SPI_LL_CONF_BUF_SET_BIT(conf_buffer[SPI_LL_USER_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_USR_DUMMY_M);
    } else {
        SPI_LL_CONF_BUF_CLR_BIT(conf_buffer[SPI_LL_USER_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_USR_DUMMY_M);
    }

    //user1 reg: usr_dummy_cyclelen
    SPI_LL_CONF_BUF_SET_FIELD(conf_buffer[SPI_LL_USER1_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_USR_DUMMY_CYCLELEN, dummy_n - 1);
}

/**
 * Update the conf buffer for dout phase
 *
 * @param hw Beginning address of the peripheral registers.
 * @param bitlen output length, in bits.
 * @param conf_buffer Conf buffer to be updated.
 */
static inline void spi_ll_format_dout_phase_conf_buffer(spi_dev_t *hw, int bitlen, uint32_t conf_buffer[SOC_SPI_SCT_BUFFER_NUM_MAX])
{
    if (bitlen) {
        //user reg: usr_mosi
        SPI_LL_CONF_BUF_SET_BIT(conf_buffer[SPI_LL_USER_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_USR_MOSI_M);
        //dma_conf reg: dma_tx_ena
        SPI_LL_CONF_BUF_SET_BIT(conf_buffer[SPI_LL_DMA_CONF_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_DMA_TX_ENA_M);
        //ms_dlen reg: ms_data_bitlen
        SPI_LL_CONF_BUF_SET_FIELD(conf_buffer[SPI_LL_MS_DLEN_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_MS_DATA_BITLEN, bitlen - 1);
    } else {
        //user reg: usr_mosi
        SPI_LL_CONF_BUF_CLR_BIT(conf_buffer[SPI_LL_USER_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_USR_MOSI_M);
        //dma_conf reg: dma_tx_ena
        SPI_LL_CONF_BUF_CLR_BIT(conf_buffer[SPI_LL_DMA_CONF_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_DMA_TX_ENA_M);
    }
}

/**
 * Update the conf buffer for din phase
 *
 * @param hw Beginning address of the peripheral registers.
 * @param bitlen input length, in bits.
 * @param conf_buffer Conf buffer to be updated.
 */
static inline void spi_ll_format_din_phase_conf_buffer(spi_dev_t *hw, int bitlen, uint32_t conf_buffer[SOC_SPI_SCT_BUFFER_NUM_MAX])
{
    if (bitlen) {
        //user reg: usr_miso
        SPI_LL_CONF_BUF_SET_BIT(conf_buffer[SPI_LL_USER_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_USR_MISO_M);
        //dma_conf reg: dma_rx_ena
        SPI_LL_CONF_BUF_SET_BIT(conf_buffer[SPI_LL_DMA_CONF_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_DMA_RX_ENA_M);
        //ms_dlen reg: ms_data_bitlen
        SPI_LL_CONF_BUF_SET_FIELD(conf_buffer[SPI_LL_MS_DLEN_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_MS_DATA_BITLEN, bitlen - 1);
    } else {
        //user reg: usr_miso
        SPI_LL_CONF_BUF_CLR_BIT(conf_buffer[SPI_LL_USER_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_USR_MISO_M);
        //dma_conf reg: dma_rx_ena
        SPI_LL_CONF_BUF_CLR_BIT(conf_buffer[SPI_LL_DMA_CONF_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_DMA_RX_ENA_M);
    }
}

/**
 * Update the conf buffer for done phase
 *
 * @param hw Beginning address of the peripheral registers.
 * @param setup CS hold time
 * @param conf_buffer Conf buffer to be updated.
 */
static inline void spi_ll_format_done_phase_conf_buffer(spi_dev_t *hw, int hold, uint32_t conf_buffer[SOC_SPI_SCT_BUFFER_NUM_MAX])
{
    //user reg: cs_hold
    if (hold) {
        SPI_LL_CONF_BUF_SET_BIT(conf_buffer[SPI_LL_USER_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_CS_HOLD_M);
    } else {
        SPI_LL_CONF_BUF_CLR_BIT(conf_buffer[SPI_LL_USER_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_CS_HOLD_M);
    }

    //user1 reg: cs_hold_time
    SPI_LL_CONF_BUF_SET_FIELD(conf_buffer[SPI_LL_USER1_REG_POS + SPI_LL_CONF_BUFFER_OFFSET], SPI_CS_HOLD_TIME, hold);
}

/**
 * Initialize the conf buffer:
 *
 * - init bitmap
 * - save all register values into the rest of the conf buffer words
 *
 * @param hw Beginning address of the peripheral registers.
 * @param conf_buffer Conf buffer to be updated.
 */
__attribute__((always_inline))
static inline void spi_ll_init_conf_buffer(spi_dev_t *hw, uint32_t conf_buffer[SOC_SPI_SCT_BUFFER_NUM_MAX])
{
    conf_buffer[SPI_LL_CONF_BITMAP_POS] = 0x7FFF | (SPI_LL_SCT_MAGIC_NUMBER << 28);
    conf_buffer[SPI_LL_ADDR_REG_POS + SPI_LL_CONF_BUFFER_OFFSET] = hw->addr.usr_addr_value;
    conf_buffer[SPI_LL_CTRL_REG_POS + SPI_LL_CONF_BUFFER_OFFSET] = hw->ctrl.val;
    conf_buffer[SPI_LL_CLOCK_REG_POS + SPI_LL_CONF_BUFFER_OFFSET] = hw->clock.val;
    conf_buffer[SPI_LL_USER_REG_POS + SPI_LL_CONF_BUFFER_OFFSET] = hw->user.val;
    conf_buffer[SPI_LL_USER1_REG_POS + SPI_LL_CONF_BUFFER_OFFSET] = hw->user1.val;
    conf_buffer[SPI_LL_USER2_REG_POS + SPI_LL_CONF_BUFFER_OFFSET] = hw->user2.val;
    conf_buffer[SPI_LL_MS_DLEN_REG_POS + SPI_LL_CONF_BUFFER_OFFSET] = hw->ms_dlen.val;
    conf_buffer[SPI_LL_MISC_REG_POS + SPI_LL_CONF_BUFFER_OFFSET] = hw->misc.val;
    conf_buffer[SPI_LL_DIN_MODE_REG_POS + SPI_LL_CONF_BUFFER_OFFSET] = hw->din_mode.val;
    conf_buffer[SPI_LL_DIN_NUM_REG_POS + SPI_LL_CONF_BUFFER_OFFSET] = hw->din_num.val;
    conf_buffer[SPI_LL_DOUT_MODE_REG_POS + SPI_LL_CONF_BUFFER_OFFSET] = hw->dout_mode.val;
    conf_buffer[SPI_LL_DMA_CONF_REG_POS + SPI_LL_CONF_BUFFER_OFFSET] = hw->dma_conf.val;
    conf_buffer[SPI_LL_DMA_INT_ENA_REG_POS + SPI_LL_CONF_BUFFER_OFFSET] = hw->dma_int_ena.val;
    conf_buffer[SPI_LL_DMA_INT_CLR_REG_POS + SPI_LL_CONF_BUFFER_OFFSET] = hw->dma_int_clr.val;
}

/**
 * Enable/Disable the conf phase
 *
 * @param hw Beginning address of the peripheral registers.
 * @param enable True: enable; False: disable
 */
static inline void spi_ll_conf_state_enable(spi_dev_t *hw, bool enable)
{
    hw->slave.usr_conf = enable;
}

/**
 * Set Segmented-Configure-Transfer required magic value
 *
 * @param hw Beginning address of the peripheral registers.
 * @param magic_value magic value
 */
static inline void spi_ll_set_magic_number(spi_dev_t *hw, uint8_t magic_value)
{
    hw->slave.dma_seg_magic_value = magic_value;
}

