/***************************************************************************//**
 *   @file   ad9172/src/main.c
 *   @brief  Implementation of Main Function.
 *   @author Cristian Pop (cristianpop@analog.com)
********************************************************************************
 * Copyright 2019(c) Analog Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
*******************************************************************************/

#include <stdio.h>
#include "hmc7044.h"
#include "ad9172.h"
#include "axi_adxcvr.h"
#include "axi_jesd204_tx.h"
#include "axi_dac_core.h"
#include "parameters.h"
#include "inttypes.h"
#include "no_os_error.h"
#include <xparameters.h>
#include <xil_cache.h>
#include "app_config.h"
#include "no_os_spi.h"
#include "xilinx_spi.h"
#include "no_os_gpio.h"
#include "xilinx_gpio.h"
#include "xil_io.h"

#ifdef DMA_EXAMPLE
#include "axi_dmac.h"
#endif /* DMA_EXAMPLE */

#ifdef IIO_SUPPORT
#include "iio_app.h"
#include "iio_axi_dac.h"
#include "xilinx_uart.h"
#endif

/*
 * ADDED (diagnostic only): tests the SPI path to the HMC7044 using only ADI's
 * normal no-OS SPI functions. The HMC7044 answers SPI as soon as it is powered,
 * it does not need hmc7044_init() first.
 * Frame format (3 bytes): [read/write bit + address high] [address low] [data]
 * Register 0x08 is the HMC7044 "scratchpad": whatever you write, you read back.
 * Registers 0x78..0x7A are the read-only Product ID: datasheet default 0x01, 0x52, 0x04.
 */
static int hmc7044_spi_test(struct no_os_spi_init_param *spi_param)
{
	struct no_os_spi_desc *spi;
	uint8_t buf[3];
	int ret;
	int good = 1;
 
	/* Test 1: open the SPI port */
	ret = no_os_spi_init(&spi, spi_param);
	if (ret != 0) {
		printf("[FAIL] SPI test 1/5: no_os_spi_init() returned %d (PS SPI could not be opened)\n", ret);
		return -1;
	}
	printf("[ OK ] SPI test 1/5: SPI port opened\n");
 
	/* DIAGNOSTIC (read only): SPI0 registers. After the fix the driver's own
	 * configuration should be visible here during transfers. */
	printf("[INFO] SPI0 registers: CR=0x%08X ISR=0x%08X\n",
	       (unsigned int)*(volatile uint32_t *)0xFF040000,
	       (unsigned int)*(volatile uint32_t *)0xFF040004);
 
	/* Test 2: write 0xAD into register 0x08 */
	buf[0] = 0x00; buf[1] = 0x08; buf[2] = 0xAD;
	ret = no_os_spi_write_and_read(spi, buf, 3);
	if (ret != 0) {
		printf("[FAIL] SPI test 2/5: write returned %d -> the SPI driver itself refused the transfer\n"
		       "       (software/PS-SPI problem, NOT the chip or the wiring)\n", ret);
		good = 0;
	} else {
		printf("[ OK ] SPI test 2/5: write transfer accepted\n");
	}
 
	/* Test 3: read register 0x08 back, expect 0xAD */
	buf[0] = 0x80; buf[1] = 0x08; buf[2] = 0x00;
	ret = no_os_spi_write_and_read(spi, buf, 3);
	if (ret != 0) {
		printf("[FAIL] SPI test 3/5: read returned %d -> the SPI driver itself refused the transfer\n", ret);
		good = 0;
	} else if (buf[2] == 0xAD) {
		printf("[ OK ] SPI test 3/5: read back 0xAD -> the HMC7044 answers on SPI!\n");
	} else {
		printf("[FAIL] SPI test 3/5: read back 0x%02X instead of 0xAD -> transfers work but the chip\n"
		       "       does not answer (check wiring/pins, VADJ, board power)\n", (unsigned int)buf[2]);
		good = 0;
	}
 
	/* Test 4: read the Product ID registers 0x78, 0x79, 0x7A (datasheet: 0x01, 0x52, 0x04) */
	{
		static const uint8_t id_expected[3] = {0x01, 0x52, 0x04};
		uint8_t id[3] = {0, 0, 0};
		int id_ok = 1;
		int i;
 
		for (i = 0; i < 3; i++) {
			buf[0] = 0x80; buf[1] = 0x78 + i; buf[2] = 0x00;
			ret = no_os_spi_write_and_read(spi, buf, 3);
			if (ret != 0)
				id_ok = 0;
			id[i] = buf[2];
			if (id[i] != id_expected[i])
				id_ok = 0;
		}
		if (id_ok) {
			printf("[ OK ] SPI test 4/5: Product ID = 0x%02X 0x%02X 0x%02X (matches HMC7044 datasheet)\n",
			       id[0], id[1], id[2]);
		} else {
			printf("[FAIL] SPI test 4/5: Product ID = 0x%02X 0x%02X 0x%02X, expected 0x01 0x52 0x04\n",
			       id[0], id[1], id[2]);
			good = 0;
		}
	}
 
	/* Test 5: show the alarm register 0x7D (information only).
	 * Datasheet: bit 3 = 1 means PLL1 and PLL2 are both locked. */
	buf[0] = 0x80; buf[1] = 0x7D; buf[2] = 0x00;
	ret = no_os_spi_write_and_read(spi, buf, 3);
	printf("[INFO] SPI test 5/5: alarm register 0x7D = 0x%02X (return %d); bit 3 set = PLLs locked\n",
	       (unsigned int)buf[2], ret);
 
	no_os_spi_remove(spi);
	return good ? 0 : -1;
}

int main(void)
{
	struct xil_spi_init_param xil_spi_param = {
		.type = SPI_PS,
#ifdef SDT
		.base_addr = XPAR_XSPIPS_0_BASEADDR,
#endif
		.flags = 0
	};

	struct no_os_spi_init_param hmc7044_spi_param = {
		.device_id = SPI_DEVICE_ID,
		.max_speed_hz = 10000000,
		.mode = NO_OS_SPI_MODE_0,
		.chip_select = SPI_HMC7044_CS,
		.platform_ops = &xil_spi_ops,
		.extra = &xil_spi_param
	};

	struct hmc7044_chan_spec chan_spec[4] = {
		{.disable = 0, .num = 2, .divider = 16, .driver_mode = 1},		                    /* DAC_CLK (200 MHz) */
		{.disable = 0, .num = 3, .divider = 512, .driver_mode = 1, .is_sysref = true},		/* DAC_SYSREF (6.25 MHz) */
		{.disable = 0, .num = 12, .divider = 16, .driver_mode = 2},		                    /* FPGA_CLK (200 MHz) */
		{.disable = 0, .num = 13, .divider = 512, .driver_mode = 2, .is_sysref = true},	    /* FPGA_SYSREF (6.25 MHz) */
	};

	struct hmc7044_init_param hmc7044_param = {
		.spi_init = &hmc7044_spi_param,
		.clkin_freq = {122880000, 0, 0, 0},
		.vcxo_freq = 122880000,                         /* Internal 122.88 MHz VCXO */
		.pll2_freq = 3200000000ULL,
		.pll1_loop_bw = 200,
		.sysref_timer_div = 1024,
		.jesd204_max_sysref_frequency_hz = 6250000,     /* 6.25 MHz target */
		.pulse_gen_mode = 0,
		.in_buf_mode = {0x15, 0, 0, 0, 0x15},
		.gpi_ctrl = {0x00, 0x00, 0x00, 0x00},
		.gpo_ctrl = {0x1f, 0x2b, 0x00, 0x00},
		.num_channels = 4,
		.channels = chan_spec,
		.export_no_os_clk = true
	};

	/* JESD204 Link Config: Mode 0 (M=2, L=1, F=4, S=1, HD=1, N=16, NP=16) */
	struct jesd204_tx_init tx_jesd_init = {
		.name = "tx_jesd",
		.base = TX_JESD_BASEADDR,
		.octets_per_frame = 4,       /* F = 4 */
		.frames_per_multiframe = 32, /* K = 32 */
		.converters_per_device = 2,  /* M = 2 */
		.converter_resolution = 16,  /* N = 16 */
		.bits_per_sample = 16,       /* NP = 16 */
		.high_density = true,        /* HD = 1 */
		.control_bits_per_sample = 0,
		.subclass = 1,
		.device_clk_khz = 200000,	/* (lane_clk_khz / 40) */ /* tx_out_clk = 200 MHz */
		.lane_clk_khz = 8000000,	/* LaneRate = ( M/L)*NP*(10/8)*DataRate */ /* Lane Rate = 8 Gbps */
	};

	struct adxcvr_init tx_adxcvr_init = {
		.name = "tx_adxcvr",
		.base = TX_XCVR_BASEADDR,
		.sys_clk_sel = ADXCVR_SYS_CLK_QPLL0,
		.out_clk_sel = ADXCVR_REFCLK,   /* CHANGED: tx_out_clk = 200 MHz (DIV2 gave 100 MHz) */
		.lpm_enable = 1,
		.lane_rate_khz = 8000000,	/* LaneRate = ( M/L)*NP*(10/8)*DataRate */
		.ref_rate_khz = 200000,		/* FPGA_CLK, output 12 of HMC 7044 */
		.export_no_os_clk = true
	};

	struct no_os_spi_init_param ad9172_spi_param = {
		.max_speed_hz = 1000000,
		.mode = NO_OS_SPI_MODE_0,
		.chip_select = SPI_AD9172_CS,
		.platform_ops = &xil_spi_ops,
		.extra = &xil_spi_param
	};

	struct xil_gpio_init_param xilinx_gpio_init_param = {
		.type = GPIO_PS,
#ifdef SDT
		.base_addr = XPAR_XGPIOPS_0_BASEADDR,
#endif
		.device_id = GPIO_DEVICE_ID
	};
	struct ad9172_init_param ad9172_param = {
		.spi_init = &ad9172_spi_param,	/* no_os_spi_init_param */
		.gpio_reset = {
			.number = 78 + 21 + 0,  /* CHANGED: ZynqMP EMIO starts at 78, dac_ctrl is EMIO 21..24 */
			.platform_ops = &xil_gpio_ops,
			.extra = &xilinx_gpio_init_param
		},
		.gpio_txen0 = {
			.number = 78 + 21 + 1,  /* dac_ctrl[1] */
			.platform_ops = &xil_gpio_ops,
			.extra = &xilinx_gpio_init_param
		},
		.gpio_txen1 = {
			.number = 78 + 21 + 2, /* dac_ctrl[2] */
			.platform_ops = &xil_gpio_ops,
			.extra = &xilinx_gpio_init_param
		},
		.dac_rate_khz = 3200000,		/* CHANGED: 200 MSPS x 8 x 2 = 3.2 GSPS */
		.dac_clkin_Hz = 200000000,		/* DAC_CLK, output 2 of HMC 7044 */
		.jesd_link_mode = 0,            /* ADI DAC Mode 0 */
		.jesd_subclass = 1,
		.dac_interpolation = 8,
		.channel_interpolation = 2,	/* CHANGED (was 4) */
		.clock_output_config = 0,
		.scrambling = 1,
		.sysref_mode = 2, /* SYSREF_CONTINUOUS */
		.pll_bypass = false,
		.syncoutb_type = SIGNAL_LVDS,
		.sysref_coupling = COUPLING_AC,
		.logic_lanes_mapping = 1,
		.logic_lanes = {7, 1, 2, 3, 4, 5, 6, 0},
		.use_jesd_fsm = true
	};

	struct axi_dac_channel chan_spec_dac[2] = {
		{.dds_frequency_0 = 40000000, .dds_phase_0 = 0, .dds_scale_0 = 500 * 1000},
		{.dds_frequency_0 = 40000000, .dds_phase_0 = 90000, .dds_scale_0 = 500 * 1000},
	};

	struct axi_dac_init tx_dac_init = {
		.name = "tx_dac",
		.base = TX_CORE_BASEADDR,
		.num_channels = 2,           /* M = 2 converters */
		.channels = chan_spec_dac,
		.rate = 1
	};

#ifdef DMA_EXAMPLE
	struct axi_dmac_init tx_dmac_init = {
		"tx_dmac",
		TX_DMA_BASEADDR,
		IRQ_DISABLED
	};
	struct axi_dmac *tx_dmac;
#endif /* DMA_EXAMPLE */

	struct hmc7044_dev *hmc7044_device;
	struct ad9172_dev *ad9172_device;
	struct axi_jesd204_tx *tx_jesd;
	struct adxcvr *tx_adxcvr;
	struct axi_dac *tx_dac;
	int32_t status;

#ifdef XILINX_PLATFORM
	/* Enable the instruction cache. */
	Xil_ICacheEnable();
	/* Enable the data cache. */
	Xil_DCacheEnable();
#endif /* XILINX_PLATFORM */

	printf("\n===== ZCU104 + AD9174 JESD204B bring-up =====\n");

	printf("PL access test: reading JESD TX AXI register 0x0 ... ");
	printf("0x%08X (PL reachable)\n", (unsigned int)*(volatile uint32_t *)TX_JESD_BASEADDR);

	/* ADDED: SPI test before touching the HMC7044 driver */
	/*if (hmc7044_spi_test(&hmc7044_spi_param) != 0) {
		printf("[FAIL] stopping: SPI to the HMC7044 does not work, see lines above\n");
		goto error_0;
	}*/

	status = hmc7044_init(&hmc7044_device, &hmc7044_param);
	if (status != 0) {
		printf("[FAIL] hmc7044_init() error: %"PRIi32"\n", status);
		goto error_0;	/* CHANGED from error_1: nothing valid to remove yet */
	}
	printf("[ OK ] hmc7044_init: clock chip configured\n");

#ifdef USE_JESD_FSM
	ad9172_param.use_jesd_fsm = true;
	printf("Using JESD FSM\n");

	status = adxcvr_init(&tx_adxcvr, &tx_adxcvr_init);
	if (status != 0) {
		printf("error: %s: adxcvr_init() failed\n", tx_adxcvr_init.name);
		goto error_3;
	}
	printf("[ OK ] adxcvr_init: transceiver configured\n");

	tx_jesd_init.lane_clk = tx_adxcvr->clk_out;

	status = axi_jesd204_tx_init(&tx_jesd, &tx_jesd_init);
	if (status != 0) {
		printf("error: %s: axi_jesd204_tx_init_legacy() failed\n", tx_jesd_init.name);
		goto error_2;
	}
	printf("[ OK ] axi_jesd204_tx_init: JESD link layer initialised\n");

	ad9172_param.dac_clk = hmc7044_device->clk_desc[2];
	ad9172_param.clk_data = tx_adxcvr->clk_out;

	status = ad9172_init(&ad9172_device, &ad9172_param);
	if (status != 0) {
		printf("ad9172_init() error: %"PRIi32"\n", status);
		goto error_4;
	}
	printf("[ OK ] ad9172_init: DAC initialised\n");

	struct jesd204_topology *topology;
	struct jesd204_topology_dev devs[] = {
		{
			.jdev = hmc7044_device->jdev,
			.link_ids = {0},
			.links_number = 1,
			.is_sysref_provider = true,
		},
		{
			.jdev = tx_jesd->jdev,
			.link_ids = {0},
			.links_number = 1,
		},
		{
			.jdev = ad9172_device->jdev,
			.link_ids = {0},
			.links_number = 1,
			.is_top_device = true,
		},
	};

	jesd204_topology_init(&topology, devs,
			      sizeof(devs) / sizeof(*devs));

	/* The lane arrives with P/N swapped: invert logical lane 0 in the DAC */
	ad917x_jesd_invert_lane(&ad9172_device->st->dac_h, 0, 1);

	jesd204_fsm_start(topology, JESD204_LINKS_ALL);
		{
		ad917x_handle_t *h = &ad9172_device->st->dac_h;
		uint8_t v;
		uint16_t r;

		printf("ILAS received 0x400-0x40E:");
		for (r = 0x400; r <= 0x40E; r++) {
			ad917x_register_read(h, r, &v);
			printf(" %02X", v);
		}
		printf("\nILAS expected 0x450-0x45D:");
		for (r = 0x450; r <= 0x45D; r++) {
			ad917x_register_read(h, r, &v);
			printf(" %02X", v);
		}
		printf("\n");
	}
	printf("[ OK ] JESD204 state machine finished (the link status printed below tells if it worked)\n");
#else
	status = axi_jesd204_tx_init_legacy(&tx_jesd, &tx_jesd_init);
	if (status != 0) {
		printf("error: %s: axi_jesd204_tx_init_legacy() failed\n", tx_jesd_init.name);
		goto error_2;
	}

	status = adxcvr_init(&tx_adxcvr, &tx_adxcvr_init);
	if (status != 0) {
		printf("error: %s: adxcvr_init() failed\n", tx_adxcvr_init.name);
		goto error_3;
	}

	status = adxcvr_clk_enable(tx_adxcvr);
	if (status != 0) {
		printf("error: %s: adxcvr_clk_enable() failed\n", tx_adxcvr->name);
		goto error_3;
	}

	status = axi_jesd204_tx_lane_clk_enable(tx_jesd);
	if (status != 0) {
		printf("error: %s: axi_jesd204_tx_lane_clk_enable() failed\n", tx_jesd->name);
		goto error_3;
	}

	ad9172_param.dac_clk = hmc7044_device->clk_desc[2];
	ad9172_param.clk_data = tx_adxcvr->clk_out;

	status = ad9172_init(&ad9172_device, &ad9172_param);
	if (status != 0) {
		printf("ad9172_init() error: %"PRIi32"\n", status);
		goto error_4;
	}

#endif

	status = axi_dac_init(&tx_dac, &tx_dac_init);
	if (status != 0) {
		printf("axi_dac_init() error: %"PRIi32"\n", status);
		goto error_5;
	}
	printf("[ OK ] axi_dac_init: DAC data core initialised\n");

	printf("---- JESD204 TX link status (you want to see: Link status: DATA) ----\n");
	axi_jesd204_tx_status_read(tx_jesd);
	printf("----------------------------------------------------------------------\n");

#ifdef DMA_EXAMPLE
	extern const uint32_t sine_lut_iq[1024];
	axi_dac_load_custom_data(tx_dac, sine_lut_iq,
				 NO_OS_ARRAY_SIZE(sine_lut_iq),
				 DDR_MEM_BASEADDR);
#ifndef ALTERA_PLATFORM
	Xil_DCacheFlush();
#endif
	axi_dmac_init(&tx_dmac, &tx_dmac_init);
	struct axi_dma_transfer transfer = {
		// Number of bytes to write/read
		.size = sizeof(sine_lut_iq) * (tx_dac->num_channels / 2),
		// Transfer done flag
		.transfer_done = 0,
		// Signal transfer mode
		.cyclic = NO,
		// Address of data source
		.src_addr = (uintptr_t)DDR_MEM_BASEADDR,
		// Address of data destination
		.dest_addr = 0
	};
	axi_dmac_transfer_start(tx_dmac, &transfer);
	/* Wait until transfer finishes */
	status = axi_dmac_transfer_wait_completion(tx_dmac, 500);
	if (status)
		return status;
	printf("[ OK ] DMA transfer complete\n");
	/* Flush cache data. */
	Xil_DCacheInvalidateRange((uintptr_t)DDR_MEM_BASEADDR,
				  sizeof(sine_lut_iq) * (tx_dac->num_channels / 2));
	printf("Tx DMA example: address=%#x samples=%lu channels=%u bits=%u\n",
	       (uintptr_t)DDR_MEM_BASEADDR,
	       transfer.size / (tx_jesd_init.converter_resolution / 8),
	       tx_dac_init.num_channels, tx_jesd_init.converter_resolution);
#else /* DMA_EXAMPLE */
	printf("Set dds frequency at 40 MHz\n");

	axi_dac_set_datasel(tx_dac, -1, AXI_DAC_DATA_SEL_DDS);
	axi_dac_data_setup(tx_dac);
#endif /* DMA_EXAMPLE */

#ifdef IIO_SUPPORT
	printf("The board accepts libiio clients connections through the serial backend.\n");

	struct xil_uart_init_param platform_uart_init_par = {
#ifdef XPAR_XUARTLITE_NUM_INSTANCES
		.type = UART_PL,
#ifdef SDT
		.base_addr = XPAR_XUARTLITE_0_BASEADDR,
#endif
#else
		.type = UART_PS,
#ifdef SDT
		.base_addr = XPAR_XUARTPS_0_BASEADDR,
#endif
		.irq_id = UART_IRQ_ID
#endif
	};

	struct no_os_uart_init_param iio_uart_ip = {
		.device_id = UART_DEVICE_ID,
		.irq_id = UART_IRQ_ID,
		.baud_rate = UART_BAUDRATE,
		.size = NO_OS_UART_CS_8,
		.parity = NO_OS_UART_PAR_NO,
		.stop = NO_OS_UART_STOP_1_BIT,
		.extra = &platform_uart_init_par,
		.platform_ops = &xil_uart_ops
	};

	struct iio_app_desc *app;
	struct iio_app_init_param app_init_param = { 0 };
	struct iio_axi_dac_desc *iio_axi_dac_desc;
	struct iio_device *dac_dev_desc;
	struct axi_dmac_init tx_dmac_init = {
		"tx_dmac",
		TX_DMA_BASEADDR,
		IRQ_DISABLED
	};
	struct axi_dmac *tx_dmac;

	axi_dmac_init(&tx_dmac, &tx_dmac_init);

	struct iio_axi_dac_init_param iio_axi_dac_init_par;
	iio_axi_dac_init_par = (struct iio_axi_dac_init_param) {
		.tx_dac = tx_dac,
		.tx_dmac = tx_dmac,
		.dcache_flush_range = (void (*)(uint32_t, uint32_t))Xil_DCacheFlushRange
	};

	status = iio_axi_dac_init(&iio_axi_dac_desc, &iio_axi_dac_init_par);
	if (NO_OS_IS_ERR_VALUE(status))
		return -1;

	iio_axi_dac_get_dev_descriptor(iio_axi_dac_desc, &dac_dev_desc);
	static struct iio_data_buffer write_buff = {
		.buff = (void *)DDR_MEM_BASEADDR,
		.size = 0xFFFFFFFF,
	};
	struct iio_app_device devices[] = {
		IIO_APP_DEVICE("axi_dac", iio_axi_dac_desc, dac_dev_desc,
			       &write_buff, NULL, NULL),
	};

	app_init_param.devices = devices;
	app_init_param.nb_devices = NO_OS_ARRAY_SIZE(devices);
	app_init_param.uart_init_params = iio_uart_ip;

	status = iio_app_init(&app, app_init_param);
	if (status)
		return status;

	return iio_app_run(app);

#endif

	printf("===== DONE. If 'Link status: DATA' is shown above, the JESD204 interface is up =====\n");
	printf("Bye\n");

error_5:
	axi_dac_remove(tx_dac);
error_4:
	ad9172_remove(ad9172_device);
error_3:
	adxcvr_remove(tx_adxcvr);
error_2:
	axi_jesd204_tx_remove(tx_jesd);
error_1:
	hmc7044_remove(hmc7044_device);
error_0:	/* ADDED: nothing to remove */

#ifdef XILINX_PLATFORM
	/* Disable the instruction cache. */
	Xil_ICacheDisable();
	/* Disable the data cache. */
	Xil_DCacheDisable();
#endif /* XILINX_PLATFORM */

	return 0;
}