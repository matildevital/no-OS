/***************************************************************************//**
 *   @file   ad9172/src/parameters.h
 *   @brief  Parameters Definitions.
 *   @author Cristian Pop (cristian.pop@analog.com)
********************************************************************************
 * Copyright 2019(c) Analog Devices, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
*******************************************************************************/
#ifndef __PARAMETERS_H__
#define __PARAMETERS_H__

#ifndef PLATFORM_ZYNQMP
#define PLATFORM_ZYNQMP
#endif

#include <xparameters.h>

/* SPI Instance selection matching xparameters.h (SPI0 at 0xff040000) */
/* SPI Instance selection matching xparameters.h (SPI0 at 0xff040000) */
#ifdef PLATFORM_ZYNQMP
#ifdef XPAR_XSPIPS_0_DEVICE_ID
#define SPI_DEVICE_ID               XPAR_XSPIPS_0_DEVICE_ID
#else
#define SPI_DEVICE_ID               0
#endif
 
#ifdef XPAR_XGPIOPS_0_DEVICE_ID
#define GPIO_DEVICE_ID              XPAR_XGPIOPS_0_DEVICE_ID
#else
#define GPIO_DEVICE_ID              0
#endif
#else
#define SPI_DEVICE_ID               XPAR_PS7_SPI_0_DEVICE_ID
#define GPIO_DEVICE_ID              XPAR_PS7_GPIO_0_DEVICE_ID
#endif

/* EMIO GPIO numbering.
 * ZynqMP: MIO = 0..77, so EMIO starts at 78.  (Zynq-7000: EMIO starts at 54.)
 * DAC_CTRL_EMIO_OFFSET must match the gpio_x[21+:4] slice used for dac_ctrl
 * in system_top.v.
 *   dac_ctrl[0] = FMC_PE_CTRL (used as AD9172 reset)
 *   dac_ctrl[1] = FMC_TXEN_0
 *   dac_ctrl[2] = FMC_TXEN_1
 */
#ifdef PLATFORM_ZYNQMP
#define GPIO_EMIO_BASE              78
#else
#define GPIO_EMIO_BASE              54
#endif
#define DAC_CTRL_EMIO_OFFSET        21
#define GPIO_DAC_CTRL(n)            (GPIO_EMIO_BASE + DAC_CTRL_EMIO_OFFSET + (n))
 
#define SPI_HMC7044_CS              0x00
#define SPI_AD9172_CS               0x01
 
#define UART_DEVICE_ID              0
#define UART_IRQ_ID                 XPAR_XUARTPS_0_INTERRUPTS
#define UART_BAUDRATE              115200


/* AXI IP Core Base Addresses from xparameters.h */
#define TX_JESD_BASEADDR            XPAR_DAC_JESD204_LINK_TX_AXI_BASEADDR             /* 0x84a90000 */
#define TX_XCVR_BASEADDR            XPAR_DAC_JESD204_XCVR_BASEADDR                    /* 0x84a60000 */
#define TX_CORE_BASEADDR            XPAR_DAC_JESD204_TRANSPORT_DAC_TPL_CORE_BASEADDR /* 0x84a04000 */
#define TX_DMA_BASEADDR             XPAR_DAC_DMA_BASEADDR                             /* 0x9c420000 */

/* DDR Base Address from xparameters.h */
#ifdef PLATFORM_ZYNQMP
#define DDR_MEM_BASEADDR            (XPAR_PSU_DDR_0_BASEADDRESS + 0xA000000)
#else
#define DDR_MEM_BASEADDR            (XPAR_PS7_DDR_0_S_AXI_BASEADDR + 0xA000000)
#endif

#endif /* __PARAMETERS_H__ */