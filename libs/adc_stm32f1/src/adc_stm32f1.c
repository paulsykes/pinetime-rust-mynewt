/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
//  Based on adc_stm32f4. We currently support blocking reads, not interrupts (marked by #ifdef TODO).
//  Tested OK with internal temparature sensor.
//  ExternalTrigConv must be set to ADC_SOFTWARE_START for STM32F1.
//  HAL should be called in this sequence:
//    __HAL_RCC_ADC1_CLK_ENABLE();
//    HAL_ADC_Init(hadc1);
//    HAL_ADC_ConfigChannel(hadc1, &temp_config);
//    HAL_ADC_Start(hadc1);
//    HAL_ADC_PollForConversion(hadc1, 10 * 1000 /* HAL_MAX_DELAY */);
//    HAL_ADC_Stop(hadc1);
//  See https://github.com/cnoviello/mastering-stm32/blob/master/nucleo-f446RE/src/ch12/main-ex1.c
//  and https://os.mbed.com/users/hudakz/code/Internal_Temperature_F103RB/file/f5c604b5eceb/main.cpp/

#include <hal/hal_bsp.h>
#include <assert.h>
#include <os/mynewt.h>
#include <mcu/cmsis_nvic.h>
#include <console/console.h>
#include "stm32f1xx_hal_dma.h"
#include "stm32f1xx_hal_adc.h"
#include "stm32f1xx_hal_rcc.h"
#include "stm32f1xx_hal_cortex.h"
#include "stm32f1xx_hal.h"
#include "adc_stm32f1/adc_stm32f1.h"
#include "stm32f1xx_hal_dma.h"
#include "mcu/stm32f1xx_mynewt_hal.h"

#if MYNEWT_VAL(ADC_1)||MYNEWT_VAL(ADC_2)||MYNEWT_VAL(ADC_3)
#include <adc/adc.h>
#endif

#ifdef TODO
#define STM32F1_IS_DMA_ADC_CHANNEL(CHANNEL) ((CHANNEL) <= DMA_CHANNEL_2)

static DMA_HandleTypeDef *dma_handle[5];
static struct adc_dev *adc_dma[5];
#endif  //  TODO

struct stm32f1_adc_stats {
    uint16_t adc_events;
    uint16_t adc_error;
    uint16_t adc_dma_xfer_failed;
    uint16_t adc_dma_xfer_aborted;
    uint16_t adc_dma_xfer_complete;
    uint16_t adc_dma_start_error;
    uint16_t adc_dma_overrun;
    uint16_t adc_internal_error;
};

static struct stm32f1_adc_stats stm32f1_adc_stats;

/// Set to 1 if RTC has been configured. Defined in apps/my_sensor_app/src/rtc.c
extern int rtc_configured;

static void
config_clk(void)
{
    //  Don't configure the clocks if RTC has already configured them.
    if (rtc_configured) { return; }

    //  Added to configure the ADC clock.  According to Blue Pill HAL: repos/apache-mynewt-core/hw/bsp/bluepill/src/hal_bsp.c
    //    HSI Clock = 8 MHz
    //    PLL Clock = (HSI / 2) * 16 = 64 MHz
    //    ADC / APB2 / PCLK2 Clock = PLL / 4 = 16 MHz
    //  which is too high - ADC clock must not exceed 14 MHz
    //  So we slow down the clock by changing the divider from 4 to 8:
    //    ADC / APB2 / PCLK2 Clock = PLL / 8 = 8 MHz
    RCC_ClkInitTypeDef clkinitstruct = { 0 };
    //  console_printf("config adc clock\n");  ////

    //  Set ADC / APB2 / PCLK2 Clock = PLL / 8 = 8 MHz
    clkinitstruct.ClockType = (RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK2);
    clkinitstruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;  //  Value 2
    //  Previously: clkinitstruct.APB2CLKDivider = RCC_HCLK_DIV4;  //  Value 1280
    clkinitstruct.APB2CLKDivider = RCC_HCLK_DIV8;  //  Value 1536
    if (HAL_RCC_ClockConfig(&clkinitstruct, FLASH_LATENCY_2) != HAL_OK) { assert(0); }  //  Latency=2
}

static void
stm32f1_adc_clk_enable(ADC_HandleTypeDef *hadc)
{
    config_clk();  ////  Added to configure the ADC clock.

    uintptr_t adc_addr = (uintptr_t)hadc->Instance;

    switch (adc_addr) {
#if defined(ADC1)
        case (uintptr_t)ADC1:
            __HAL_RCC_ADC1_CLK_ENABLE();
            break;
#endif
#if defined(ADC2)
        case (uintptr_t)ADC2:
            __HAL_RCC_ADC2_CLK_ENABLE();
            break;
#endif
#if defined(ADC3)
        case (uintptr_t)ADC3:
            __HAL_RCC_ADC3_CLK_ENABLE();
            break;
#endif
        default:
            assert(0);
    }
}

static void
stm32f1_adc_clk_disable(ADC_HandleTypeDef *hadc)
{
    uintptr_t adc_addr = (uintptr_t)hadc->Instance;

    switch (adc_addr) {
#if defined(ADC1)
        case (uintptr_t)ADC1:
            __HAL_RCC_ADC1_CLK_DISABLE();
            break;
#endif
#if defined(ADC2)
        case (uintptr_t)ADC2:
            __HAL_RCC_ADC2_CLK_DISABLE();
            break;
#endif
#if defined(ADC3)
        case (uintptr_t)ADC3:
            __HAL_RCC_ADC3_CLK_DISABLE();
            break;
#endif
        default:
            assert(0);
    }
}

static int
stm32f1_resolve_adc_gpio(ADC_HandleTypeDef *adc, uint8_t cnum,
        GPIO_InitTypeDef *gpio)
{
    uintptr_t adc_addr = (uintptr_t)adc->Instance;
    uint32_t pin;
    int rc;

    rc = OS_OK;
    switch (adc_addr) {
#if defined(ADC1) || defined(ADC2)
#if defined(ADC1)
        case (uintptr_t)ADC1:
#endif
#if defined(ADC2)
        case (uintptr_t)ADC2:
#endif
            switch(cnum) {
                case ADC_CHANNEL_4:
                    pin = ADC12_CH4_PIN;
                    goto done;
                case ADC_CHANNEL_5:
                    pin = ADC12_CH5_PIN;
                    goto done;
                case ADC_CHANNEL_6:
                    pin = ADC12_CH6_PIN;
                    goto done;
                case ADC_CHANNEL_7:
                    pin = ADC12_CH7_PIN;
                    goto done;
                case ADC_CHANNEL_8:
                    pin = ADC12_CH8_PIN;
                    goto done;
                case ADC_CHANNEL_9:
                    pin = ADC12_CH9_PIN;
                    goto done;
                case ADC_CHANNEL_14:
                    pin = ADC12_CH14_PIN;
                    goto done;
                case ADC_CHANNEL_15:
                    pin = ADC12_CH15_PIN;
                    goto done;
            }
#endif
        /*
         * Falling through intentionally as ADC_3 contains seperate pins for
         * Channels that ADC_1 and ADC_2 contain as well.
         */
#if defined(ADC3)
        case (uintptr_t)ADC3:
            switch(cnum) {
                case ADC_CHANNEL_0:
                    pin = ADC123_CH0_PIN;
                    goto done;
                case ADC_CHANNEL_1:
                    pin = ADC123_CH1_PIN;
                    goto done;
                case ADC_CHANNEL_2:
                    pin = ADC123_CH2_PIN;
                    goto done;
                case ADC_CHANNEL_3:
                    pin = ADC123_CH3_PIN;
                    goto done;
                case ADC_CHANNEL_4:
                    pin = ADC3_CH4_PIN;
                    goto done;
                case ADC_CHANNEL_5:
                    pin = ADC3_CH5_PIN;
                    goto done;
                case ADC_CHANNEL_6:
                    pin = ADC3_CH6_PIN;
                    goto done;
                case ADC_CHANNEL_7:
                    pin = ADC3_CH7_PIN;
                    goto done;
                case ADC_CHANNEL_8:
                    pin = ADC3_CH8_PIN;
                    goto done;
                case ADC_CHANNEL_9:
                    pin = ADC3_CH9_PIN;
                    goto done;
                case ADC_CHANNEL_10:
                    pin = ADC123_CH10_PIN;
                    goto done;
                case ADC_CHANNEL_11:
                    pin = ADC123_CH11_PIN;
                    goto done;
                case ADC_CHANNEL_12:
                    pin = ADC123_CH12_PIN;
                    goto done;
                case ADC_CHANNEL_13:
                    pin = ADC123_CH13_PIN;
                    goto done;
                case ADC_CHANNEL_14:
                    pin = ADC3_CH14_PIN;
                    goto done;
                case ADC_CHANNEL_15:
                    pin = ADC3_CH15_PIN;
                    goto done;
            }
#endif
        default:
            rc = OS_EINVAL;
            return rc;
    }
done:
    *gpio = (GPIO_InitTypeDef) {
        .Pin = pin,
        .Mode = GPIO_MODE_ANALOG,
        .Pull = GPIO_NOPULL,
        //// .Alternate = pin
    };
    return rc;
}

#ifdef TODO
static IRQn_Type
stm32f1_resolve_adc_dma_irq(DMA_HandleTypeDef *hdma)
{
    uintptr_t stream_addr = (uintptr_t)hdma->Instance;

    assert(STM32F1_IS_DMA_ADC_CHANNEL(hdma->Init.Channel));

    switch(stream_addr) {
        /* DMA2 */
        case (uintptr_t)DMA2_Stream0:
            return DMA2_Stream0_IRQn;
        case (uintptr_t)DMA2_Stream1:
            return DMA2_Stream1_IRQn;
        case (uintptr_t)DMA2_Stream2:
            return DMA2_Stream2_IRQn;
        case (uintptr_t)DMA2_Stream3:
            return DMA2_Stream3_IRQn;
        case (uintptr_t)DMA2_Stream4:
            return DMA2_Stream4_IRQn;
        default:
            assert(0);
    }
}

static void
dma2_stream0_irq_handler(void)
{
    HAL_DMA_IRQHandler(dma_handle[0]);
}

static void
dma2_stream1_irq_handler(void)
{
    HAL_DMA_IRQHandler(dma_handle[1]);
}

static void
dma2_stream2_irq_handler(void)
{
    HAL_DMA_IRQHandler(dma_handle[2]);
}

static void
dma2_stream3_irq_handler(void)
{
    HAL_DMA_IRQHandler(dma_handle[3]);
}

static void
dma2_stream4_irq_handler(void)
{
    HAL_DMA_IRQHandler(dma_handle[4]);
}

uint32_t
stm32f1_resolve_adc_dma_irq_handler(DMA_HandleTypeDef *hdma)
{
    switch((uintptr_t)hdma->Instance) {
        /* DMA2 */
        case (uintptr_t)DMA2_Stream0:
            return (uint32_t)&dma2_stream0_irq_handler;
        case (uintptr_t)DMA2_Stream1:
            return (uint32_t)&dma2_stream1_irq_handler;
        case (uintptr_t)DMA2_Stream2:
            return (uint32_t)&dma2_stream2_irq_handler;
        case (uintptr_t)DMA2_Stream3:
            return (uint32_t)&dma2_stream3_irq_handler;
        case (uintptr_t)DMA2_Stream4:
            return (uint32_t)&dma2_stream4_irq_handler;
        default:
            assert(0);
    }
}

static int
stm32f1_resolve_dma_handle_idx(DMA_HandleTypeDef *hdma)
{
    uintptr_t stream_addr = (uintptr_t)hdma->Instance;
    return ((stream_addr & 0xFF) - ((uintptr_t)DMA2_Stream0_BASE & 0xFF))/0x18;
}
#endif  //  TODO

void
HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc)
{
    ++stm32f1_adc_stats.adc_error;

    if (hadc->ErrorCode & HAL_ADC_ERROR_DMA) {
        /* DMA transfer error */
        ++stm32f1_adc_stats.adc_dma_xfer_failed;
    } else if (hadc->ErrorCode & HAL_ADC_ERROR_OVR) {
        /* DMA transfer overrun */
        ++stm32f1_adc_stats.adc_dma_overrun;
    } else if (hadc->ErrorCode & HAL_ADC_ERROR_INTERNAL) {
       /* ADC IP Internal Error */
        ++stm32f1_adc_stats.adc_internal_error;
    }
}

#ifdef TODO
/**
 * Callback that gets called by the HAL when ADC conversion is complete and
 * the DMA buffer is full. If a secondary buffer exists it will the buffers.
 *
 * @param ADC Handle
 */
void
HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    int rc;
    struct adc_dev *adc;
    DMA_HandleTypeDef *hdma;
    struct stm32f1_adc_dev_cfg *cfg;
    void *buf;

    assert(hadc);
    hdma = hadc->DMA_Handle;

    ++stm32f1_adc_stats.adc_dma_xfer_complete;

    adc = adc_dma[stm32f1_resolve_dma_handle_idx(hdma)];
    cfg  = (struct stm32f1_adc_dev_cfg *)adc->ad_dev.od_init_arg;

    buf = cfg->primarybuf;
    /**
     * If primary buffer gets full and secondary buffer exists, swap the
     * buffers and start ADC conversion with DMA with the now primary
     * buffer(former secondary buffer)
     * If the secondary buffer(former primary buffer) doesn't get processed
     * by the application in sampling period required for the primary/secondary buffer
     * i,e; (sample itvl * ADC_NUMBER_SAMPLES), the buffers would get swapped resulting
     * in new sample data.
     */
    if (cfg->secondarybuf) {
        cfg->primarybuf = cfg->secondarybuf;
        cfg->secondarybuf = buf;

        if (HAL_ADC_Start_DMA(hadc, cfg->primarybuf, cfg->buflen) != HAL_OK) {
            ++stm32f1_adc_stats.adc_dma_start_error;
        }
    }

    rc = adc->ad_event_handler_func(adc, NULL, ADC_EVENT_RESULT, buf,
                                    cfg->buflen);

    if (rc) {
        ++stm32f1_adc_stats.adc_error;
    }
}
#endif  //  TODO

static void
stm32f1_adc_dma_init(ADC_HandleTypeDef* hadc)
{
#ifdef TODO
    DMA_HandleTypeDef *hdma;
#endif  //  TODO
    assert(hadc);
#ifdef TODO
    hdma = hadc->DMA_Handle;
#endif  //  TODO
    stm32f1_adc_clk_enable(hadc);
#ifdef TODO
    __HAL_RCC_DMA2_CLK_ENABLE();

    HAL_DMA_Init(hdma);
    dma_handle[stm32f1_resolve_dma_handle_idx(hdma)] = hdma;

    NVIC_SetPriority(stm32f1_resolve_adc_dma_irq(hdma),
                     NVIC_EncodePriority(NVIC_GetPriorityGrouping(), 0, 0));
    NVIC_SetVector(stm32f1_resolve_adc_dma_irq(hdma),
                   stm32f1_resolve_adc_dma_irq_handler(hdma));
    NVIC_EnableIRQ(stm32f1_resolve_adc_dma_irq(hdma));
#endif  //  TODO
}

static void
stm32f1_adc_init(struct adc_dev *dev)
{
    struct stm32f1_adc_dev_cfg *adc_config;
    ADC_HandleTypeDef *hadc;

    assert(dev);

    adc_config = (struct stm32f1_adc_dev_cfg *)dev->ad_dev.od_init_arg;
    hadc = adc_config->sac_adc_handle;

    stm32f1_adc_dma_init(hadc);

    if (HAL_ADC_Init(hadc) != HAL_OK) {
        assert(0);
    }
}

static void
stm32f1_adc_uninit(struct adc_dev *dev)
{
    GPIO_InitTypeDef gpio_td;
    ////  DMA_HandleTypeDef *hdma;
    ADC_HandleTypeDef *hadc;
    struct stm32f1_adc_dev_cfg *cfg;
    uint8_t cnum;

    assert(dev);
    cfg  = (struct stm32f1_adc_dev_cfg *)dev->ad_dev.od_init_arg;
    hadc = cfg->sac_adc_handle;
    ////  hdma = hadc->DMA_Handle;
    cnum = dev->ad_chans->c_cnum;

#ifdef TODO
    __HAL_RCC_DMA2_CLK_DISABLE();
    if (HAL_DMA_DeInit(hdma) != HAL_OK) {
        assert(0);
    }
#endif  //  TODO
    stm32f1_adc_clk_disable(hadc);

    ////  NVIC_DisableIRQ(stm32f1_resolve_adc_dma_irq(hdma));

    //  Temperature and VREF channels don't use GPIO.  No need to deinit GPIO.
    if (cnum != ADC_CHANNEL_TEMPSENSOR && cnum != ADC_CHANNEL_VREFINT) {
        //  Deinit the GPIO.
        if (stm32f1_resolve_adc_gpio(hadc, cnum, &gpio_td)) {
            goto err;
        }
        if (hal_gpio_deinit_stm(gpio_td.Pin, &gpio_td)) {
            goto err;
        }
    }
    //  TODO: Call HAL_ADC_DeInit
err:
    return;
}

/**
 * Open the STM32F1 ADC device
 *
 * This function locks the device for access from other tasks.
 *
 * @param odev The OS device to open
 * @param wait The time in MS to wait.  If 0 specified, returns immediately
 *             if resource unavailable.  If OS_WAIT_FOREVER specified, blocks
 *             until resource is available.
 * @param arg  Argument provided by higher layer to open.
 *
 * @return 0 on success, non-zero on failure.
 */
static int
stm32f1_adc_open(struct os_dev *odev, uint32_t wait, void *arg)
{
#ifdef TODO
    DMA_HandleTypeDef *hdma;
    ADC_HandleTypeDef *hadc;
    struct stm32f1_adc_dev_cfg *cfg;
#endif  //  TODO
    //  console_printf("open adc1\n");  ////
    struct adc_dev *dev;
    int rc;

    assert(odev);
    rc = OS_OK;
    dev = (struct adc_dev *) odev;

    if (os_started()) {
        rc = os_mutex_pend(&dev->ad_lock, wait);
        if (rc != OS_OK) {
            goto err;
        }
    }

    if (odev->od_flags & OS_DEV_F_STATUS_OPEN) {
        os_mutex_release(&dev->ad_lock);
        rc = OS_EBUSY;
        goto err;
    }

    stm32f1_adc_init(dev);

#ifdef TODO    
    cfg  = (struct stm32f1_adc_dev_cfg *)dev->ad_dev.od_init_arg;
    hadc = cfg->sac_adc_handle;
    hdma = hadc->DMA_Handle;

    adc_dma[stm32f1_resolve_dma_handle_idx(hdma)] = dev;
#endif  //  TODO    

    return (OS_OK);
err:
    return (rc);
}


/**
 * Close the STM32F1 ADC device.
 *
 * This function unlocks the device.
 *
 * @param odev The device to close.
 */
static int
stm32f1_adc_close(struct os_dev *odev)
{
    //  console_printf("close adc1\n");  ////
    struct adc_dev *dev;

    dev = (struct adc_dev *) odev;

    stm32f1_adc_uninit(dev);

    if (os_started()) {
        os_mutex_release(&dev->ad_lock);
    }

    return (OS_OK);
}

/**
 * Configure an ADC channel on the STM32F1 ADC.
 *
 * @param dev The ADC device to configure
 * @param cnum The channel on the ADC device to configure
 * @param cfgdata An opaque pointer to channel config, expected to be
 *                a ADC_ChannelConfTypeDef
 *
 * @return 0 on success, non-zero on failure.
 */
static int
stm32f1_adc_configure_channel(struct adc_dev *dev, uint8_t cnum,
        void *cfgdata)
{
    int rc;
    ADC_HandleTypeDef *hadc;
    struct stm32f1_adc_dev_cfg *cfg;
    struct adc_chan_config *chan_cfg;
    GPIO_InitTypeDef gpio_td;

    rc = OS_EINVAL;

    if (dev == NULL && !IS_ADC_CHANNEL(cnum)) {
        goto err;
    }

    cfg  = (struct stm32f1_adc_dev_cfg *)dev->ad_dev.od_init_arg;
    hadc = cfg->sac_adc_handle;
    chan_cfg = &((struct adc_chan_config *)cfg->sac_chans)[cnum];

    cfgdata = (ADC_ChannelConfTypeDef *)cfgdata;

    if ((HAL_ADC_ConfigChannel(hadc, cfgdata)) != HAL_OK) {
        goto err;
    }

    dev->ad_chans[cnum].c_res = chan_cfg->c_res;
    dev->ad_chans[cnum].c_refmv = chan_cfg->c_refmv;
    dev->ad_chans[cnum].c_configured = 1;
    dev->ad_chans[cnum].c_cnum = cnum;

    //  Temperature and VREF channels don't use GPIO.  No need to init GPIO.
    if (cnum == ADC_CHANNEL_TEMPSENSOR || cnum == ADC_CHANNEL_VREFINT) {
        return OS_OK;
    }

    if (stm32f1_resolve_adc_gpio(hadc, cnum, &gpio_td)) {
        goto err;
    }

    hal_gpio_init_stm(gpio_td.Pin, &gpio_td);

    return (OS_OK);
err:
    return (rc);
}

#ifdef TODO
/**
 * Set buffer to read data into.  Implementation of setbuffer handler.
 * Sets both the primary and secondary buffers for DMA.
 *
 * For our current implementation we are using DMA in circular mode
 *
 */
static int
stm32f1_adc_set_buffer(struct adc_dev *dev, void *buf1, void *buf2,
        int buflen)
{
    struct stm32f1_adc_dev_cfg *cfg;
    int rc;


    assert(dev != NULL && buf1 != NULL);
    rc = OS_OK;
    buflen /= sizeof(uint32_t);

    cfg  = (struct stm32f1_adc_dev_cfg *)dev->ad_dev.od_init_arg;

    cfg->primarybuf = buf1;
    cfg->secondarybuf = buf2;
    cfg->buflen = buflen;

    return rc;
}

static int
stm32f1_adc_release_buffer(struct adc_dev *dev, void *buf, int buf_len)
{
    ADC_HandleTypeDef *hadc;
    struct stm32f1_adc_dev_cfg *cfg;

    assert(dev);
    cfg  = (struct stm32f1_adc_dev_cfg *)dev->ad_dev.od_init_arg;
    hadc = cfg->sac_adc_handle;

    HAL_ADC_Stop_DMA(hadc);

    return (0);
}
#else
//  Catch calls to these functions
static int
stm32f1_adc_set_buffer(struct adc_dev *dev, void *buf1, void *buf2,
        int buflen)
{
    assert(0);  //  Not implemented.
    return (0);
}

static int
stm32f1_adc_release_buffer(struct adc_dev *dev, void *buf, int buf_len)
{
    assert(0);  //  Not implemented.
    return (0);
}
#endif  //  TODO

/**
 * Trigger an ADC sample.
 *
 * @param ADC device structure
 * @return OS_OK on success, non OS_OK on failure
 */
static int
stm32f1_adc_sample(struct adc_dev *dev)
{
    int rc;
    ADC_HandleTypeDef *hadc;
    struct stm32f1_adc_dev_cfg *cfg;

    assert(dev);
    cfg  = (struct stm32f1_adc_dev_cfg *)dev->ad_dev.od_init_arg;
    hadc = cfg->sac_adc_handle;

    rc = OS_EINVAL;

    if (HAL_ADC_Start_DMA(hadc, cfg->primarybuf, cfg->buflen) != HAL_OK) {
        ++stm32f1_adc_stats.adc_dma_start_error;
        goto err;
    }

    rc = OS_OK;

err:
    return rc;
}

/**
 * Blocking read of an ADC channel, returns result as an integer.
 *
 * @param1 ADC device structure
 * @param2 channel number
 * @param3 ADC result ptr
 */
static int
stm32f1_adc_read_channel(struct adc_dev *dev, uint8_t cnum, int *result)
{
    //  New implementation that actually blocks when reading a channel.
    ADC_HandleTypeDef *hadc;
    struct stm32f1_adc_dev_cfg *cfg;
    int val = -1;

    assert(dev != NULL && result != NULL);
    cfg  = (struct stm32f1_adc_dev_cfg *)dev->ad_dev.od_init_arg;
    hadc = cfg->sac_adc_handle;

    while (HAL_ADCEx_Calibration_Start(hadc) != HAL_OK);  // Calibrate AD converter.

    //  Start reading ADC values and convert them by rank.
    HAL_ADC_Start(hadc);

    //  Wait for ADC conversion to be completed.
    HAL_StatusTypeDef rc = HAL_ADC_PollForConversion(hadc, 10 * 1000);  //  Wait up to 10 seconds.  TODO: Yield to task scheduler while waiting.
    assert(rc == HAL_OK);
    if (rc != HAL_OK) { HAL_ADC_Stop(hadc); return rc; }  //  Exit in case of error.

    //  Fetch the converted ADC value.
    val = HAL_ADC_GetValue(hadc);
    *result = val;

    //  Stop reading ADC values.
    HAL_ADC_Stop(hadc);
    return (OS_OK);
}

#ifdef NOTUSED  //  Previous implementation of stm32f1_adc_read_channel(), which is not a blocking read.
    /**
     * Blocking read of an ADC channel, returns result as an integer.
     *
     * @param1 ADC device structure
     * @param2 channel number
     * @param3 ADC result ptr
     */
    static int
    stm32f1_adc_read_channel(struct adc_dev *dev, uint8_t cnum, int *result)
    {
        ADC_HandleTypeDef *hadc;
        struct stm32f1_adc_dev_cfg *cfg;

        assert(dev != NULL && result != NULL);
        cfg  = (struct stm32f1_adc_dev_cfg *)dev->ad_dev.od_init_arg;
        hadc = cfg->sac_adc_handle;

        *result = HAL_ADC_GetValue(hadc);

        return (OS_OK);
    }
#endif //  NOTUSED

static int
stm32f1_adc_read_buffer(struct adc_dev *dev, void *buf, int buf_len, int off,
                        int *result)
{

    assert(off < buf_len);

    /*
     * If secondary buffer exists the primary buf is going to be cached
     * in the secondary buffer if the primary buffer is full and we
     * would be reading that instead since the buffer is specified by
     * the application
     */
    *result = *((uint32_t *)buf + off);

    return (OS_OK);
}

/**
 * Callback to return size of buffer
 *
 * @param1 ADC device ptr
 * @param2 Total number of channels
 * @param3 Total number of samples
 * @return Length of buffer in bytes
 */
static int
stm32f1_adc_size_buffer(struct adc_dev *dev, int chans, int samples)
{
    return (sizeof(uint32_t) * chans * samples);
}

/**
 * ADC device driver functions
 */
static const struct adc_driver_funcs stm32f1_adc_funcs = {
        .af_configure_channel = stm32f1_adc_configure_channel,
        .af_sample = stm32f1_adc_sample,
        .af_read_channel = stm32f1_adc_read_channel,
        .af_set_buffer = stm32f1_adc_set_buffer,
        .af_release_buffer = stm32f1_adc_release_buffer,
        .af_read_buffer = stm32f1_adc_read_buffer,
        .af_size_buffer = stm32f1_adc_size_buffer,
};

/**
 * Callback to initialize an adc_dev structure from the os device
 * initialization callback.  This sets up a stm32f1_adc_device(), so
 * that subsequent lookups to this device allow us to manipulate it.
 *
 * @param1 os device ptr
 * @param2 stm32f1 ADC device cfg ptr
 * @return OS_OK on success
 */
int
stm32f1_adc_dev_init(struct os_dev *odev, void *arg)
{
    struct stm32f1_adc_dev_cfg *sac;
    struct adc_dev *dev;

    sac = (struct stm32f1_adc_dev_cfg *) arg;

    assert(sac != NULL);

    dev = (struct adc_dev *)odev;

    os_mutex_init(&dev->ad_lock);

    dev->ad_chans = (void *) sac->sac_chans;
    dev->ad_chan_count = sac->sac_chan_count;

    OS_DEV_SETHANDLERS(odev, stm32f1_adc_open, stm32f1_adc_close);

    dev->ad_funcs = &stm32f1_adc_funcs;

#ifdef NOTUSED
    //  TODO: Move to stm32f1_adc_open
    __HAL_RCC_ADC1_CLK_ENABLE();  ////  TODO: Added enable ADC1

    struct stm32f1_adc_dev_cfg *cfg  = (struct stm32f1_adc_dev_cfg *)dev->ad_dev.od_init_arg;
    ADC_HandleTypeDef *hadc = cfg->sac_adc_handle;
    HAL_StatusTypeDef rc = HAL_ADC_Init(hadc);  ////  Added HAL initalisation, which was missing from the STM32F4 code.
    if (rc != HAL_OK) { return rc; }
#endif  //  NOTUSED
    return (OS_OK);
}
